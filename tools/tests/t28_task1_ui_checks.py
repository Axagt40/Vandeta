#!/usr/bin/env python3
"""Task 1 (2026-10-22) regression pass, driven over USB only.

Covers the parts of the task list that can be checked without a keypad or a
second radio:

  * the MITM Attack page has exactly the clean row set (no Station / Last /
    Status / Create example / Pick payload),
  * the menu keeps its cursor on the row that was used (no "restart"),
  * HID Settings has no Payload Manager and a toggle stays on its row,
  * NRF24 has the RGB Remote page and it is the small one,
  * a BLE feature first no longer makes the MITM network scan refuse for RAM,
  * RFID > Field detector renders its screen and exits on BACK.

The board is hard-reset first, every menu is entered with `option <n>` (which
records the label it selected), and the screen is read back through the
firmware's own text stream (`display start`). Stop the screen mirror first - it
holds /dev/ttyACM0.

    python3 tools/tests/t28_task1_ui_checks.py [port]
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

MAIN_WIFI, MAIN_BLE, MAIN_NRF, MAIN_RFID = 0, 1, 3, 9
WIFI_ATKS = 2
MITM_ROW = 8
BLE_DETECTOR = 2

passes = 0
fails = 0


def check(ok, what, detail=""):
    global passes, fails
    if ok:
        passes += 1
        print("  PASS  " + what)
    else:
        fails += 1
        print("  FAIL  " + what + (("  [" + detail + "]") if detail else ""))


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
        t = m.decode(errors="replace")
        if t not in seen:
            seen.add(t)
            out.append(t)
    return out


def select(s, row, settle=1.8):
    """Press one row of the menu that is on screen; returns the labels."""
    send(s, "ui select %d" % row, settle)
    return strings(grab(s, 0.8))


def ui_state(s):
    send(s, "ui", 1.0)
    for line in strings(grab(s, 1.2)):
        if line.startswith("UISTATE "):
            return line
    return ""


def menu_rows(s):
    """The option labels of the current menu, read back from UISTATE."""
    line = ui_state(s)
    return re.findall(r'"label":"([^"]*)"', line) if line else []


def back_to_main(s, tries=6):
    """Escape until the main menu is really on screen.

    A fixed number of `nav esc` presses is not reliable: a page that rescans
    when it is left (the WiFi network list does) eats an escape while it
    redraws, so the next section started one level too deep. Asking the
    firmware where it is beats counting presses.
    """
    for _ in range(tries):
        if '"menu_title":"main_menu"' in ui_state(s):
            return True
        send(s, "nav esc", 1.0)
    return '"menu_title":"main_menu"' in ui_state(s)


def selected_label(text):
    for t in text:
        m = re.search(r"Forcely Selected: (.+)$", t)
        if m:
            return m.group(1).strip()
        m = re.search(r"Selected option \d+: (.+)$", t)
        if m:
            return m.group(1).strip()
    return ""


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
        print("  internal free at the menu:", mem_internal(s), "B")
        send(s, "display stop")
        send(s, "display start")

        print("\n===== MITM Attack page =====")
        select(s, MAIN_WIFI)
        select(s, WIFI_ATKS)
        select(s, MITM_ROW)
        rows = menu_rows(s)
        print("  rows:", rows)
        want = [
            "Network:",
            "Target:",
            "Payload:",
            "Preview payload",
            "Start attack",
            "Captures",
            "Main Menu",
        ]
        check(len(rows) == len(want), "MITM page has %d rows" % len(want), str(len(rows)))
        for i, prefix in enumerate(want):
            check(i < len(rows) and rows[i].startswith(prefix), "row %d is %r" % (i, prefix), rows[i] if i < len(rows) else "-")
        joined = " | ".join(rows)
        for gone in ("Station:", "Last:", "Status", "Create example", "Pick payload"):
            check(gone not in joined, "no %r row" % gone)

        print("\n===== cursor stays on the row used =====")
        # Row 3 opens the payload viewer; BACK returns to the page and must land
        # on row 3, not row 0 ("the menu restarted").
        send(s, "ui select 3", 1.5)
        grab(s, 0.8)
        send(s, "nav esc", 1.2)
        send(s, "ui", 1.0)
        state = [t for t in strings(grab(s, 1.0)) if t.startswith("[UI]")]
        check(bool(state) and "active=3" in state[-1], "back from the viewer keeps row 3", state[-1] if state else "-")

        back_to_main(s)

        print("\n===== HID Settings =====")
        select(s, 13)  # Others
        select(s, 3)  # BadUSB & HID
        select(s, 0)  # HID Settings
        rows = menu_rows(s)
        print("  rows:", rows)
        check(not any("Payload Manager" in r for r in rows), "no Payload Manager row")
        idx = next((i for i, r in enumerate(rows) if r.startswith("Show Output")), -1)
        check(idx >= 0, "Show Output row present")
        if idx >= 0:
            before = rows[idx]
            select(s, idx)
            rows2 = menu_rows(s)
            send(s, "ui", 1.0)
            state = [t for t in strings(grab(s, 1.0)) if t.startswith("[UI]")]
            check(rows2[idx] != before, "the toggle label changed", rows2[idx] if idx < len(rows2) else "-")
            check(bool(state) and ("active=%d" % idx) in state[-1], "toggle keeps row %d" % idx, state[-1] if state else "-")
            select(s, idx)  # put the setting back the way it was
        back_to_main(s)

        print("\n===== NRF24 RGB Remote =====")
        select(s, MAIN_NRF)
        rows = menu_rows(s)
        check(any(r == "RGB Remote" for r in rows), "NRF24 lists RGB Remote", str(rows))
        idx = rows.index("RGB Remote") if "RGB Remote" in rows else -1
        if idx >= 0:
            select(s, idx)
            rows = menu_rows(s)
            print("  rows:", rows)
            check(len(rows) == 6, "RGB Remote page has 5 rows + Main Menu", str(len(rows)))
            for prefix in ("Capture", "Hunt unknown remote", "Preset:", "Replay capture", "Save capture"):
                check(any(r.startswith(prefix) for r in rows), "row %r" % prefix)
            send(s, "nav esc", 1.0)
        send(s, "nav esc", 0.9)

        print("\n===== RAM: a BLE feature first, then MITM network =====")
        select(s, MAIN_BLE)
        select(s, BLE_DETECTOR, settle=4.0)
        grab(s, 2.0)
        send(s, "nav esc", 1.2)
        send(s, "nav esc", 1.0)
        print("  internal free after BLE Detector:", mem_internal(s), "B")
        select(s, MAIN_WIFI)
        select(s, WIFI_ATKS)
        select(s, MITM_ROW)
        send(s, "ui select 0", 9.0)  # Network -> scan (a real scan takes ~3 s)
        text = strings(grab(s, 4.0))
        bad = [t for t in text if "Low RAM" in t or "radio memory" in t or "free BLE" in t]
        check(not bad, "no RAM refusal on the network scan", "; ".join(bad[:2]))
        check(
            any(("Scanning" in t) or ("dBm" in t) or ("MITM NETWORK" in t) for t in text),
            "the scan actually ran",
            "; ".join(text[-3:]),
        )
        send(s, "nav esc", 1.2)
        for _ in range(2):
            send(s, "nav esc", 0.9)

        print("\n===== RFID Field detector =====")
        select(s, MAIN_RFID)
        rows = menu_rows(s)
        check(any(r == "Field detector" for r in rows), "RFID lists Field detector", str(rows))
        idx = rows.index("Field detector") if "Field detector" in rows else -1
        if idx >= 0:
            # Re-arm the stream first: the firmware only sends draws that are new
            # since the ring was cleared, so a page entered while the stream is
            # already running can arrive with its static lines deduplicated.
            send(s, "display stop")
            send(s, "display start")
            send(s, "ui select %d" % idx, 1.5)
            text = strings(grab(s, 4.0))
            for want in ("NFC FIELD", "quiet", "BACK exit"):
                check(any(want in t for t in text), "screen shows %r" % want, "; ".join(text[-3:]))
            check(any("tag" in t for t in text), "screen shows the tag line")
            send(s, "nav esc", 1.2)
            check(bool([t for t in strings(grab(s, 1.0)) if "RFID" in t]), "BACK returns to the RFID menu")
    finally:
        s.close()

    print("\n===== SUMMARY =====")
    print("  %d PASS, %d FAIL" % (passes, fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
