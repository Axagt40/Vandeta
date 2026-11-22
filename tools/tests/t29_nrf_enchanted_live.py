#!/usr/bin/env python3
"""nRF24 "Enchanted" jammer, exercised live against a real 2.4 GHz source.

The 2026-10-22 pass verified the Enchanted engine on the board but had no
transmitter it controlled, so the target list was whatever the room already
radiated (DISCOVERIES: "the data rate decides whether anything is captured").
This run points it at a BLE speaker that is known to be playing, which is a
continuous 2.4 GHz source on the BLE channels, and checks three things:

  1. the page comes up in the documented state (Test / Sequential, TGT sweep),
  2. `Sel` cycles Sequential -> FHSS -> Enchanted and the Enchanted scan reports
     targets (the engine logs `[nrfjam] ench: N targets, top CH X (score S)`
     whenever the count or the top channel changes),
  3. leaving the page hands the radio back cleanly (`nrfcap info` chip=ok) and
     the internal heap is where it started.

Every check reads the firmware's own text, so nothing here needs a camera.

    python3 tools/tests/t29_nrf_enchanted_live.py [port]
"""
import os
import re
import subprocess
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200
ESPTOOL = os.path.expanduser("~/.platformio/penv/bin/esptool")

MAIN_NRF = 3

passes = 0
fails = 0
notes = []


def check(ok, what, detail=""):
    global passes, fails
    if ok:
        passes += 1
        print("  PASS  " + what)
    else:
        fails += 1
        print("  FAIL  " + what + (("  [" + detail + "]") if detail else ""))


def note(text):
    notes.append(text)
    print("  NOTE  " + text)


def hard_reset():
    if not os.path.exists(ESPTOOL):
        return
    subprocess.run(
        [ESPTOOL, "--chip", "esp32s3", "--port", PORT, "--after", "hard_reset", "chip_id"],
        capture_output=True,
        timeout=60,
    )


def open_port():
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    s.dtr = True  # the firmware's USB CDC only transmits with DTR asserted
    s.rts = False
    time.sleep(9)  # 7 s splash + margin
    s.reset_input_buffer()
    return s


def send(s, cmd, wait=0.8):
    s.write((cmd + "\r\n").encode())
    s.flush()
    time.sleep(wait)


def grab(s, seconds):
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        c = s.read(8192)
        if c:
            buf.extend(c)
    return bytes(buf)


def strings(data):
    out, seen = [], set()
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        t = m.decode(errors="replace").strip()
        if t and t not in seen:
            seen.add(t)
            out.append(t)
    return out


def rows(s):
    send(s, "ui", 1.0)
    for line in strings(grab(s, 1.2)):
        if line.startswith("UISTATE "):
            return re.findall(r'"label":"([^"]*)"', line)
    return []


def select(s, row, settle=1.8):
    send(s, "ui select %d" % row, settle)
    return strings(grab(s, 0.7))


def line_with(text, prefix):
    for t in text:
        if prefix in t:
            return t
    return ""


def ench_logs(text):
    hits = []
    for t in text:
        m = re.search(r"\[nrfjam\] ench: (\d+) targets, top CH (\d+) \(score (\d+)\)", t)
        if m:
            hits.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    return hits


def mem_internal(s):
    send(s, "mem", 1.2)
    for t in strings(grab(s, 1.5)):
        m = re.search(r"internal free\s*:\s*(\d+)", t)
        if m:
            return int(m.group(1))
    return -1


def main():
    hard_reset()
    s = open_port()
    try:
        print("\n===== baseline =====")
        boot_mem = mem_internal(s)
        print("  internal free at the menu:", boot_mem, "B")
        send(s, "nrfcap info", 2.5)
        info = strings(grab(s, 1.5))
        chip = line_with(info, "chip=")
        print(" ", chip or "(no nrfcap info)")
        check("chip=ok" in chip, "the nRF24 answered before the test", chip)

        # The screen stream itself costs ~13 KB of internal DRAM, so the heap
        # baseline for the jammer run has to be taken AFTER it is running -
        # otherwise "the jammer leaked 13 KB" is just the display stream.
        send(s, "display stop")
        send(s, "display start")
        base_mem = mem_internal(s)
        print("  internal free with the screen stream on:", base_mem, "B")

        print("\n===== NRF24 > NRF Jammer =====")
        select(s, MAIN_NRF)
        page = rows(s)
        print("  NRF24 rows:", page)
        idx = next((i for i, r in enumerate(page) if "Jammer" in r), -1)
        check(idx >= 0, "the NRF24 menu lists NRF Jammer", str(page))
        if idx < 0:
            return 1
        sel_text = select(s, idx, 2.0)
        text = strings(grab(s, 2.0))
        # The page's first paint can be deduplicated away by the screen ring
        # (DISCOVERIES 42), so the header is asserted on the redraw that the
        # first `Sel` forces - that paint is complete and always streamed.
        label = line_with(sel_text, "Forcely Selected:")
        check("NRF Jammer" in label, "the jammer row was the one selected", label or "-")

        print("\n===== Sel cycles the type =====")
        send(s, "press ok", 1.2)
        text = strings(grab(s, 1.5))
        for want in ("NRF JAMMER", "STATUS : 1 ACTIVE", "MODE : Test", "TYPE : FHSS", "TGT  :"):
            check(any(want in t for t in text), "page shows %r" % want, "; ".join(text[-4:]))
        send(s, "press ok", 1.2)
        text = strings(grab(s, 1.5))
        check(any("TYPE : Enchanted" in t for t in text), "Sel 2 -> Enchanted", "; ".join(text[-3:]))

        print("\n===== Enchanted on the Test channel list =====")
        seen = ench_logs(strings(grab(s, 9.0)))
        print("  scan log:", seen)
        check(len(seen) > 0, "the Enchanted scan logged a target line")

        print("\n===== Enchanted on BLEch (the speaker) =====")
        send(s, "press right", 1.2)  # Test -> WiFi
        send(s, "press right", 1.5)  # WiFi -> BLEch
        text = strings(grab(s, 2.0))
        check(any("MODE : BLEch" in t for t in text), "MODE switched to BLEch", "; ".join(text[-4:]))

        text = strings(grab(s, 20.0))
        seen = ench_logs(text)
        print("  scan log:", seen)
        ble_found = [x for x in seen if x[0] > 0]
        check(len(ble_found) > 0, "BLE channels carry traffic while the speaker plays", str(seen))
        if ble_found:
            top = max(ble_found, key=lambda x: x[2])
            note("strongest target: %d channels, top CH %d (score %d)" % top)
            # nRF channel n is 2400+n MHz. BLE advertising sits on 2402/2426/2480
            # (nRF 2/26/80) and the 37 data channels on 2404-2478 (nRF 4-78), so
            # every target of this list should land inside the BLE band.
            in_ble = [x for x in ble_found if 2 <= x[1] <= 80]
            check(len(in_ble) == len(ble_found), "every target is inside the BLE band", str(ble_found))
            adv = [x for x in ble_found if x[1] in (2, 26, 80)]
            if adv:
                note("an advertising channel is among the targets: %s" % str(adv[0]))

        print("\n===== Enchanted on BLE Adv Pri =====")
        send(s, "press right", 1.5)  # BLEch -> BLE Adv Pri
        text = strings(grab(s, 2.0))
        check(any("MODE : BLE Adv Pri" in t for t in text), "MODE switched to BLE Adv Pri", "; ".join(text[-4:]))
        seen = ench_logs(strings(grab(s, 12.0)))
        print("  scan log:", seen)
        check(len(seen) > 0, "the advertising list also produced target lines", str(seen))

        print("\n===== a second mode change re-targets (no stale list) =====")
        send(s, "press left", 1.5)  # back to BLEch
        text = strings(grab(s, 3.0))
        check(any("MODE : BLEch" in t for t in text), "MODE went back to BLEch", "; ".join(text[-4:]))

        print("\n===== leaving the page =====")
        send(s, "nav esc", 2.0)
        text = strings(grab(s, 1.5))
        check(any("NRF24" in t or "Information" in t for t in text), "BACK returns to the NRF24 menu", "; ".join(text[-3:]))

        send(s, "nrfcap info", 2.5)
        info = strings(grab(s, 1.5))
        chip = line_with(info, "chip=")
        print(" ", chip or "(no nrfcap info)")
        check("chip=ok" in chip, "the nRF24 is healthy after the jammer", chip)
        check("packets=0" in chip or "profile=-1" in chip, "no capture profile left armed", chip)

        mem = mem_internal(s)
        print("  internal free after the test:", mem, "B (baseline %d)" % base_mem)
        if base_mem > 0 and mem > 0:
            check(mem >= base_mem - 4096, "heap is back to the baseline", "%d vs %d" % (mem, base_mem))
    finally:
        s.close()

    print("\n===== SUMMARY =====")
    print("  %d PASS, %d FAIL" % (passes, fails))
    for n in notes:
        print("  NOTE  " + n)
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
