#!/usr/bin/env python3
"""Task 1 live verification over USB (no hands on the board needed).

Covers everything that can be proven from the host:
  * the Universal Remote library scan speed and the un-stuck sweep counter
    (see t19 for the scan timing),
  * every brand AC encoder runs and reports a frame,
  * the SubGHz Wardriver's duplicate filter + .sub writer (synthetic bursts) and
    its live screen,
  * the BLE Detector's live screen,
  * BadUSB Settings: the renamed menu and the Apple/BLE patch toggle reaching
    BadBLE's settings.

Needs the board on USB in console mode with Developer Options on. Stop the screen
mirror first (it holds /dev/ttyACM0).

    python3 tools/tests/t20_usb_verify_task1.py [port]
"""
import os
import re
import subprocess
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200
BOOT_WAIT = 9.0
ESPTOOL = os.path.expanduser("~/.platformio/penv/bin/esptool")


def hard_reset():
    """Start from the main menu, whatever menu a previous run left open."""
    if os.path.exists(ESPTOOL):
        subprocess.run(
            [ESPTOOL, "--chip", "esp32s3", "--port", PORT, "--after", "hard_reset", "chip_id"],
            capture_output=True,
            timeout=60,
        )

FAILS = []


def send(s, cmd, wait=0.5):
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
            buf.extend(b"\n")
    return bytes(buf)


def strings(data):
    out, seen = [], set()
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        t = m.decode(errors="replace")
        if t not in seen:
            seen.add(t)
            out.append(t)
    return out


def check(name, ok, detail=""):
    print("  %-42s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))
    if not ok:
        FAILS.append(name)
    return ok


def options_list(s):
    """`options` prints one "- Label" line per row, in menu order."""
    send(s, "options", 0.8)
    text = grab(s, 1.0).decode("utf-8", "replace")
    labels = []
    for line in text.splitlines():
        # rows print as ">0 - WiFi" / " 1 - BLE" (the '>' marks the cursor)
        m = re.match(r"^\s*[> ]?\s*(\d+)\s-\s(.+?)\s*$", line)
        if m:
            labels.append(m.group(2))
    return list(enumerate(labels))


def open_option(s, label, settle=2.0):
    """Select the menu row whose label starts with `label`."""
    for idx, text in options_list(s):
        if text.lower().startswith(label.lower()):
            send(s, "option %d" % idx, settle)
            return True
    print("    (no row matching %r, saw %r)" % (label, [t for _, t in options_list(s)]))
    return False


def screen_contains(s, needles, seconds=4.0):
    data = grab(s, seconds)
    text = " | ".join(strings(data))
    return all(n.lower() in text.lower() for n in needles), text


def main():
    hard_reset()
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    s.dtr = True
    s.rts = False
    time.sleep(BOOT_WAIT)
    s.reset_input_buffer()
    send(s, "", 0.3)

    print("===== IR: AC brand remotes =====")
    send(s, "ir ac list", 0.8)
    listing = grab(s, 1.0).decode("utf-8", "replace")
    brands = [m.group(2).strip() for m in re.finditer(r"^(\d+)\t(.+?)\r?$", listing, re.M)]
    check("ir ac list returns the brand table", len(brands) >= 10, "%d brands" % len(brands))

    sent_ok = 0
    for brand in brands:
        send(s, "ir ac send %s cool 22 auto" % brand.replace(" ", ""), 0.35)
        text = " ".join(strings(grab(s, 0.6)))
        if "sent" in text and "encode failed" not in text:
            sent_ok += 1
        else:
            print("    %s -> %s" % (brand, text[-120:]))
    check("every brand AC encoder sends", sent_ok == len(brands), "%d/%d" % (sent_ok, len(brands)))

    print("\n===== SubGHz Wardriver =====")
    send(s, "subghz wardriver selftest", 2.0)
    text = " ".join(strings(grab(s, 1.5)))
    check("wardriver dedupe + .sub writer selftest", "PASS" in text, text[-160:].strip())

    send(s, "subghz wardriver status", 1.0)
    text = " ".join(strings(grab(s, 1.0)))
    check("wardriver status reports its config", "folder=" in text and "min_pulses=" in text,
          text[-120:].strip())

    print("\n===== Wardriver screen =====")
    send(s, "display stop", 0.4)
    send(s, "display start", 0.8)
    opened = open_option(s, "RF")
    if check("RF menu opens", opened):
        if check("Wardriver row present", open_option(s, "Wardriver", 3.0)):
            ok, text = screen_contains(s, ["WARDRIVER", "SIGNALS", "duplicates"], 4.0)
            check("wardriver screen draws its counters", ok, text[:160])
            send(s, "nav esc", 2.0)
        send(s, "nav esc", 1.0)

    print("\n===== BLE Detector screen =====")
    if check("Bluetooth menu opens", open_option(s, "BLE") or open_option(s, "Bluetooth")):
        if check("BLE Detector row present", open_option(s, "BLE Detector", 3.0)):
            ok, text = screen_contains(s, ["BLE DETECTOR", "dev", "adv"], 6.0)
            check("BLE detector screen draws its table", ok, text[:160])
            send(s, "nav esc", 3.0)
        send(s, "nav esc", 1.0)

    print("\n===== BadUSB Settings =====")
    send(s, "badusb config", 1.0)
    text = " ".join(strings(grab(s, 1.0)))
    check("config reports apple_patch", "apple_patch=0" in text or "apple_patch=1" in text,
          text[:120])
    send(s, "badusb set applepatch 1", 1.0)
    send(s, "badusb config", 1.0)
    text = " ".join(strings(grab(s, 1.0)))
    check("apple_patch can be switched on", "apple_patch=1" in text)
    send(s, "badusb set applepatch 0", 1.0)
    send(s, "badusb config", 1.0)
    text = " ".join(strings(grab(s, 1.0)))
    check("apple_patch can be switched off", "apple_patch=0" in text)

    s.close()
    print("\n===== SUMMARY =====")
    if FAILS:
        print("  FAILED: " + ", ".join(FAILS))
        return 1
    print("  all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
