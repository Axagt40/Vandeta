#!/usr/bin/env python3
"""Task 17 phase 2 verification (WiFi source).

Two hard-won rules from live testing:
  * any non-`nav` serial command calls backToMenu() (src/core/serialcmds.cpp),
    so `display ...` must be sent ONCE (to arm the stream), never mid-navigation;
  * key presses over USB drop, and submenus do not wrap, so the highlight is
    verified from the printed options dump after every press instead of counted.

Flow: fresh boot -> RF menu -> Packet Monitor (Sub-GHz) -> its menu ->
"Change source" -> WiFi. Then sample the live table.

    python3 tools/tests/t17_wifi_source.py [port]
"""
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200


def open_port():
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    time.sleep(8)  # opening the port resets the board
    s.reset_input_buffer()
    return s


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
    return bytes(buf)


def strings(data):
    out, seen = [], set()
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        t = m.decode(errors="replace")
        if t not in seen:
            seen.add(t)
            out.append(t)
    return [t for t in out if not t.startswith(("COMMAND", "# "))]


def highlighted(data):
    rows = []
    for line in data.decode(errors="replace").splitlines():
        line = line.rstrip()
        if "Options available" in line:
            rows = []
            continue
        m = re.match(r"\s*(>)?\s*(\d+)\s+-\s+(.*)$", line)
        if m:
            rows.append((m.group(3).strip(), bool(m.group(1))))
    return next((l for l, h in rows if h), None)


def press_until(s, label, tries=16, verbose=False):
    seen = []
    for _ in range(tries):
        send(s, "nav next", 0.45)
        cur = highlighted(grab(s, 1.4))
        if verbose:
            seen.append(cur)
        if cur == label:
            return True
    if verbose:
        print(f"  [press_until {label!r}] saw {seen}")
    return False


def main():
    s = open_port()
    try:
        # Arm the stream once. No display commands after this (they backToMenu).
        send(s, "display stop")
        send(s, "display start")

        # main -> RF
        send(s, "nav next", 0.5)
        send(s, "nav next", 0.5)
        send(s, "nav sel", 1.5)

        if not press_until(s, "Packet Monitor", verbose=True):
            print("!! could not highlight Packet Monitor in RF menu")
            return
        send(s, "nav sel", 2.0)

        txt = strings(grab(s, 3.0))
        if not any("PACKET MONITOR" in t for t in txt):
            print("!! dashboard did not open; saw:", txt[:16])
            return
        print("dashboard open (Sub-GHz source):")
        for t in txt[:10]:
            print("  " + t)

        # dashboard menu -> Change source (Sub-GHz -> WiFi)
        send(s, "nav sel", 1.2)
        if not press_until(s, "Change source"):
            print("!! could not highlight Change source")
            return
        send(s, "nav sel", 2.5)
        print("\nselected Change source (Sub-GHz -> WiFi)")

        for i in range(3):
            txt = strings(grab(s, 3.5))
            print(f"\n-- sample {i} --")
            for t in txt[:26]:
                print("  " + t)
    finally:
        s.close()


if __name__ == "__main__":
    main()
