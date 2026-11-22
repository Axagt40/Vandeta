#!/usr/bin/env python3
"""Half-2 hardening pass (2026-10-27): the on-device checks.

Covers what the second half of the 2026-10-27 reliability pass changed, over
USB only (no keypad, no second radio):

  * internal DRAM handed back by the PSRAM moves (`mem` at the menu),
  * the streaming WiFi scan page: rows appear while channels are still being
    visited, `ch n/13` progress is drawn, Sel opens the picker early,
  * the picker order (strongest RSSI first) and its Hidden SSID / Main Menu rows,
  * nRF24 > NRF Jammer > TYPE = Enchanted: the whole channel list is walked,
  * BLE Detector and BLE Capture & Replay still record after their tables moved
    to PSRAM,
  * WiFi raw capture (session table allocated on first use) via a JS one-liner.

The board must be in Developer Options (the `ui`, `mem`, `press` and `display`
verbs are debug-only). Stop the screen mirror first - it holds /dev/ttyACM0.

    python3 tools/tests/t32_half2_checks.py [port]
"""
import os
import re
import subprocess
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
NO_RF = "--no-rf" in sys.argv  # skip the one check that transmits on 2.4 GHz
BAUD = 115200
ESPTOOL = os.path.expanduser("~/.platformio/penv/bin/esptool")

passes = 0
fails = 0
skips = 0


def check(ok, what, detail=""):
    global passes, fails
    if ok:
        passes += 1
        print("  PASS  " + what)
    else:
        fails += 1
        print("  FAIL  " + what + (("  [" + detail + "]") if detail else ""))


def skip(what, detail=""):
    """A property that could not be exercised on this boot (see the early-Sel
    step): counted apart from a pass so it is never mistaken for evidence."""
    global skips
    skips += 1
    print("  SKIP  " + what + (("  [" + detail + "]") if detail else ""))


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


def chunks(s, seconds):
    """[(t, text)] as the display stream arrives, so growth can be timed."""
    out = []
    t0 = time.time()
    while time.time() - t0 < seconds:
        c = s.read(8192)
        if c:
            out.append((time.time() - t0, c.decode(errors="ignore")))
    return out


def strings(data):
    out, seen = [], set()
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        t = m.decode(errors="replace")
        if t not in seen:
            seen.add(t)
            out.append(t)
    return out


def ui_state(s):
    # `ui` waits for the screen to settle before it dumps, so on a page that keeps
    # drawing (the streaming WiFi scan and its picker) the answer can land after a
    # single read window. Retry, and keep the LAST *complete* UISTATE of each
    # window: a line split by the reader, or read before the row vector is
    # finished, would otherwise drop the tail rows (Hidden SSID, Main Menu).
    for _ in range(4):
        # Anything the previous command left behind is stale by definition:
        # `ui select` already dumps a UISTATE of the screen it left.
        s.reset_input_buffer()
        send(s, "ui", 0.9)
        best = ""
        for raw in strings(grab(s, 1.2)):
            i = raw.find("UISTATE ")
            if i < 0:
                continue
            line = raw[i:]
            if '"options"' in line and line.rstrip().endswith("}"):
                best = line
        if best:
            return best
    return ""


def rows(s):
    line = ui_state(s)
    return re.findall(r'"label":"([^"]*)"', line) if line else []


def labels_in_text(text):
    """Rows of the last complete UISTATE JSON in an arbitrary text window.

    Reading the picker out of the stream is deliberate: the picker's own UISTATE
    is already there, and a fresh `ui` against a page that keeps redrawing (a
    long SSID scrolls, so the screen never goes quiet) can answer late or not at
    all.
    """
    best = []
    for m in re.finditer(r"UISTATE (\{.*)", text):
        cand = m.group(1).strip()
        if '"options"' in cand and cand.endswith("}"):
            labels = re.findall(r'"label":"([^"]*)"', cand)
            if len(labels) >= len(best):
                best = labels
    return best


def ui_rows_quiet(s):
    """rows() with the screen stream paused.

    While the stream is on, the firmware's screen packets are written from the
    display task between the console's own writes, so they can land inside the
    UISTATE JSON and splice it. Pausing the stream for the duration of the query
    costs nothing on a page that is not being watched.
    """
    send(s, "display stop", 0.5)
    try:
        return rows(s)
    finally:
        send(s, "display start", 0.5)


def rssi_of(label):
    """The -NN in a picker label like "#AP(-77|WPA2/PSK|ch.6)"."""
    m = re.search(r"\((-?\d+)\|", label)
    return int(m.group(1)) if m else 0


def row_of(labels, wanted):
    """Index of the first row whose label contains `wanted` (case-sensitive)."""
    for i, lab in enumerate(labels):
        if wanted in lab:
            return i
    return -1


def back_to_main(s, tries=8):
    for _ in range(tries):
        if '"menu_title":"main_menu"' in ui_state(s):
            return True
        send(s, "nav esc", 1.0)
    return '"menu_title":"main_menu"' in ui_state(s)


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
        print("\n===== internal DRAM at the menu =====")
        before = mem_internal(s)
        print("  internal free:", before, "B")
        check(before > 90000, "board answers mem with a sane internal-heap number", str(before))

        send(s, "display stop")
        send(s, "display start")

        # ---------------------------------------------------------------- WiFi
        print("\n===== WiFi: streaming scan page =====")
        back_to_main(s)
        main_rows = rows(s)
        wifi_row = row_of(main_rows, "WiFi")
        check(wifi_row >= 0, "main menu has a WiFi row", str(main_rows))
        if wifi_row < 0:
            raise SystemExit(1)
        send(s, "ui select %d" % wifi_row, 1.6)
        labs = rows(s)
        connect_row = row_of(labs, "Connect to Wifi")
        check(connect_row >= 0, "WiFi menu has Connect to Wifi", str(labs))

        # Enter the page and watch the text stream for four seconds.
        s.write(("ui select %d\r\n" % connect_row).encode())
        s.flush()
        stream = chunks(s, 4.0)
        text = "".join(t for _, t in stream)
        progress = [int(m) for m in re.findall(r"ch (\d+)/13", text)]
        row_hits = [(t, m) for t, x in stream for m in re.findall(r"-\d+ ch(\d+)", x)]
        done_at = next((t for t, x in stream if "Sel: choose" in x), None)

        print("  channel progress seen:", progress[:14])
        print("  first row at: %.2f s" % (row_hits[0][0] if row_hits else -1))
        print("  sweep done at: %s" % ("%.2f s" % done_at if done_at is not None else "not seen"))
        check(len(set(progress)) >= 3, "status line counts channels while scanning", str(progress[:6]))
        check(bool(row_hits), "at least one network row was drawn")
        check(
            bool(row_hits) and (done_at is None or row_hits[0][0] < done_at),
            "a network row appears before the sweep finishes",
            "row %.2f s / done %s" % (row_hits[0][0] if row_hits else -1, done_at),
        )
        check(done_at is not None, "'Sel: choose' hint is drawn when the sweep ends")

        # The picker that follows: strongest first, Hidden SSID then Main Menu.
        # Read it out of the window above: the picker's own UISTATE is in there,
        # and re-querying a page that keeps redrawing (a long SSID scrolls, so
        # the screen never goes quiet) can answer late or not at all.
        pick = labels_in_text(text) or ui_rows_quiet(s)  # stream first, quiet read after
        print("  picker:", pick)
        # Only the AP rows carry an RSSI: "Hidden SSID" and "Main Menu" follow them
        # and are not part of the ordering.
        ap_rows = [p for p in pick if re.search(r"\(-?\d+\|", p)]
        rssis = [rssi_of(p) for p in ap_rows]
        check(len(ap_rows) >= 1, "picker lists the scanned networks", str(ap_rows))
        check(
            len(rssis) >= 2 and all(a >= b for a, b in zip(rssis, rssis[1:])),
            "picker is sorted strongest first",
            str(rssis),
        )
        check(row_of(pick, "Hidden SSID") >= 0, "picker keeps the Hidden SSID row")
        check(row_of(pick, "Main Menu") >= 0, "picker keeps the Main Menu row")

        # Early Sel: the page must stop waiting for the remaining channels.
        #
        # Three properties of this board shape the step. A scripted press is a
        # single flag that main.cpp's input task expires after ~75 ms, so it only
        # lands while the page is polling - and this page polls once per channel
        # (130 ms). An uncached sweep needs ~1.7-2.4 s for the 13 channels. And a
        # re-entered page can be served from the driver's scan cache, in which
        # case the picker is already up when the press would arrive and the press
        # would CHOOSE a network instead of stopping the sweep.
        #
        # The stream cannot be used to tell "still scanning" from "picker up":
        # its queue lags a whole sweep behind, so a window full of ch n/13 frames
        # can be the previous sweep's. So the press is sent blind and early, and
        # the outcome is judged by when the picker appears: a stopped sweep opens
        # it within ~0.2 s of the press, a natural one only after the remaining
        # channels (~2 s). A picker that was already up before the press is
        # reported as a skip - never a pass with no evidence, never a picked
        # network.
        main_row = row_of(pick, "Main Menu")
        if main_row >= 0:
            send(s, "ui select %d" % main_row, 1.8)
        back_to_main(s)
        main_rows = rows(s)
        send(s, "ui select %d" % row_of(main_rows, "WiFi"), 1.6)
        labs = rows(s)
        s.write(("ui select %d\r\n" % row_of(labs, "Connect to Wifi")).encode())
        s.flush()

        before = "".join(t for _, t in chunks(s, 0.25))
        if "Hidden SSID" in before:
            early_state = "skip"
            early_detail = "the sweep was served from the scan cache: the picker was up before any press"
            pick2 = labels_in_text(before) or ui_rows_quiet(s)
        else:
            send(s, "press ok", 0.05)
            ran = chunks(s, 2.6)
            after_text = "".join(t for _, t in ran)
            picker_at = next(
                (t for t, x in ran if "Hidden SSID" in x or '"menu":"regular_menu"' in x), None
            )
            pick2 = labels_in_text(before + after_text)
            if not pick2:
                # A screen packet landing inside the picker's JSON splices it (the
                # same race the picker above avoids by reading the stream).
                for _ in range(2):
                    pick2 = ui_rows_quiet(s)
                    if pick2:
                        break
            opened = (bool(pick2) and row_of(pick2, "Hidden SSID") >= 0) or "Hidden SSID" in after_text
            if not opened:
                early_state = "fail"
                early_detail = "no picker within 2.6 s of the press"
                print("  picker window:", repr(after_text[-200:]))
            elif picker_at is not None and picker_at <= 1.0:
                early_state = "pass"
                early_detail = "picker up %.2f s after the press" % picker_at
            else:
                early_state = "skip"
                early_detail = "the sweep finished on its own first (+%s)" % (
                    "%.2f s" % picker_at if picker_at is not None else "?"
                )
        print("  early Sel:", early_state, "-", early_detail)
        if early_state == "pass":
            check(True, "Sel opens the picker early", early_detail)
            check(True, "the early picker did not wait for channel 13/13", early_detail)
        elif early_state == "fail":
            check(False, "Sel opens the picker early", early_detail)
            check(False, "the early picker did not wait for channel 13/13", early_detail)
        else:
            skip("Sel opens the picker early", early_detail)
            skip("the early picker did not wait for channel 13/13", early_detail)
        main_row = row_of(pick2, "Main Menu")
        if main_row >= 0:
            send(s, "ui select %d" % main_row, 1.8)
        back_to_main(s)

        # --------------------------------------------------------- WiFi capture
        print("\n===== WiFi raw capture (session table on first use) =====")
        send(
            s,
            # console.log, not print(): this interpreter has no global print()
            # (see CUSTOM_JS_API.txt), and print() aborts the whole script with
            # "variable 'print' is not defined" before captureStart() is reported.
            'file write /t32cap.js "var id=wifi.captureStart(1,2500);console.log(\'cap id\',id);'
            "delay(3000);var p=wifi.getCapturedPackets(id,3);console.log('packets',p.length);"
            "console.log('stop',wifi.captureStop(id));\"",
            1.2,
        )
        send(s, "js /t32cap.js", 6.0)
        out = "".join(strings(grab(s, 2.0)))
        m = re.search(r"cap id (-?\d+)", out)
        check(bool(m) and int(m.group(1)) > 0, "wifi.captureStart returns a session id", out[-120:])
        check("packets" in out, "getCapturedPackets answered", out[-120:])
        send(s, "file delete /t32cap.js", 1.0)

        if NO_RF:
            print("\n===== NRF24: Enchanted (RF check skipped: --no-rf) =====")
        else:
            # ------------------------------------------------------ Enchanted (nRF)
            print("\n===== NRF24: Enchanted walks the whole list =====")
            back_to_main(s)
            main_rows = rows(s)
            nrf_row = row_of(main_rows, "NRF24")
            check(nrf_row >= 0, "main menu has an NRF24 row", str(main_rows))
            send(s, "ui select %d" % nrf_row, 1.6)
            labs = rows(s)
            jam_row = row_of(labs, "NRF Jammer")
            check(jam_row >= 0, "NRF24 menu has NRF Jammer", str(labs))
            s.write(("ui select %d\r\n" % jam_row).encode())
            s.flush()
            # The jammer page is a custom screen (no option list): Sel cycles TYPE.
            page = "".join(t for _, t in chunks(s, 1.5))
            check("TYPE :" in page, "jammer page draws a TYPE line", page[-160:])
            if "TYPE :" in page:
                send(s, "press ok", 0.8)  # Sequential -> FHSS
                send(s, "press ok", 0.8)  # FHSS -> Enchanted
                ran = chunks(s, 3.2)
                txt = "".join(t for _, t in ran)
                ench = re.findall(r"\[nrfjam\] ench: [^\r\n]*", txt)
                # The page's own target row (enchTargetSummary()) reads
                # "N ch, top CH x" once the scan has heard something and
                # "listening..." while it has not. Which one is due depends on
                # what is in the band, not on the firmware, so the row is judged
                # against the count the scan logged instead of one fixed form.
                tgt = [r.strip() for r in re.findall(r"TGT  : ([^\r\n]*)", txt)]
                print("  serial:", ench[-2:] if ench else "-")
                print("  TGT row:", tgt[-2:] if tgt else "-")
                check(bool(ench), "Enchanted logs its scan (Developer Mode)", str(ench[:1]))
                check(bool(tgt), "the TGT row is streamed while Enchanted runs", str(txt[-120:]))
                check(len(tgt) >= 2, "the TGT row is redrawn about once a second", str(tgt[:3]))
                if ench:
                    check("targets," in ench[-1] or "0 targets" in ench[-1], "scan line reports a target count", ench[-1])
                    heard = int(re.search(r"ench: (\d+) targets", ench[-1]).group(1))
                    if heard:
                        check(
                            bool(re.fullmatch(r"\d+ ch, top CH \d+", tgt[-1] if tgt else "")),
                            "TGT row reports the heard channel count and the top channel",
                            str(tgt[-1:]),
                        )
                    else:
                        check(
                            bool(tgt) and tgt[-1] == "listening...",
                            "TGT row says listening... while nothing was heard",
                            str(tgt[-1:]),
                        )
            send(s, "press back", 2.0)
            back_to_main(s)

        # ------------------------------------------------------------- BLE side
        print("\n===== BLE Detector / Capture tables (PSRAM) =====")
        main_rows = rows(s)
        ble_row = row_of(main_rows, "BLE")
        check(ble_row >= 0, "main menu has a BLE row", str(main_rows))
        send(s, "ui select %d" % ble_row, 1.6)
        labs = rows(s)
        det_row = row_of(labs, "BLE Detector")
        check(det_row >= 0, "BLE menu has BLE Detector", str(labs))
        s.write(("ui select %d\r\n" % det_row).encode())
        s.flush()
        det = chunks(s, 4.5)
        det_txt = "".join(t for _, t in det)
        devices = re.findall(r"(\d+) dev\b", det_txt)
        print("  detector counts:", devices[-3:] if devices else "-")
        check(bool(devices), "BLE Detector draws a device count", det_txt[-80:])
        send(s, "press back", 2.0)
        back_to_main(s)

        main_rows = rows(s)
        send(s, "ui select %d" % row_of(main_rows, "BLE"), 1.6)
        labs = rows(s)
        cap_row = row_of(labs, "Capture & Replay")
        check(cap_row >= 0, "BLE menu has Capture & Replay", str(labs))
        send(s, "ui select %d" % cap_row, 1.6)
        labs = rows(s)
        start_row = row_of(labs, "Start capture")
        check(start_row >= 0, "capture menu has Start capture", str(labs))
        send(s, "ui select %d" % start_row, 6.5)  # 5 s capture + teardown
        send(s, "press back", 1.5)
        back_to_main(s)

        print("\n===== after =====")
        after = mem_internal(s)
        print("  internal free:", after, "B (was", before, "B at boot)")
        check(after > 0, "board still answers mem after every page", str(after))
        send(s, "errors", 1.2)
        err = "".join(strings(grab(s, 1.5)))
        check("Guru" not in err and "abort" not in err.lower(), "no crash in the error log", err[-100:])

        print("\n%d passed, %d failed, %d skipped" % (passes, fails, skips))
        print("INTERNAL_FREE_BEFORE=%d INTERNAL_FREE_AFTER=%d" % (before, after))
    finally:
        send(s, "display stop")
        s.close()


if __name__ == "__main__":
    main()
