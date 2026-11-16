#!/usr/bin/env python3
"""Task 19 benchmark: how long the Universal Remote libraries take to scan.

The device menu "Universal Remote > AC" sat on a blank screen for ~3.5 s because
`irUniversalButtons()` walks the whole .ir file with `readStringUntil('\\n')`
before the first row can be drawn. This script runs the same function over the
console (`ir uni buttons <category>`) and times it from the command byte to the
"<cat>: N buttons" summary line, so the number is the library scan alone (boot,
menu drawing and key polling are not in it).

    python3 FREEBUFF/tests/t19_ir_universal_bench.py [port] [category...]

Prints one line per category:  <category>  <buttons>  <scan ms>  (best of 3)
Exit status is 0 only if every category answered with its button list.
"""
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
CATS = sys.argv[2:] or ["TV", "AC", "Fans", "Audio", "Projectors", "LEDs"]
BAUD = 115200
BOOT_WAIT = 9.0  # the board reboots when the port is opened
REPEATS = 3

SUMMARY = re.compile(r"^\w+: (\d+) buttons", re.M)


def open_port():
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    s.dtr = True  # the firmware's USB CDC only transmits with DTR asserted
    s.rts = False
    time.sleep(BOOT_WAIT)
    s.reset_input_buffer()
    drain(s, 1.2)
    return s


def drain(s, idle=1.2, limit=30.0):
    """Read until the line has been quiet for `idle` seconds."""
    out = bytearray()
    start = time.time()
    last = time.time()
    while time.time() - start < limit:
        chunk = s.read(4096)
        if chunk:
            out.extend(chunk)
            last = time.time()
        elif time.time() - last > idle:
            break
    return out.decode("utf-8", "replace")


def one_run(s, cat):
    """Send the command, return (elapsed_ms, button_count or None, text)."""
    s.reset_input_buffer()
    t0 = time.time()
    s.write(b"ir uni buttons " + cat.encode() + b"\n")
    s.flush()

    text = bytearray()
    deadline = t0 + 30.0
    elapsed = None
    while time.time() < deadline:
        chunk = s.read(4096)
        if not chunk:
            continue
        text.extend(chunk)
        m = SUMMARY.search(text.decode("utf-8", "replace"))
        if m:
            elapsed = (time.time() - t0) * 1000.0
            count = int(m.group(1))
            break
    drain(s, 0.25)
    if elapsed is None:
        return None, None, text.decode("utf-8", "replace")
    return elapsed, count, "" 


def main():
    s = open_port()
    failures = 0

    for cat in CATS:
        best = None
        count = None
        err = ""
        for _ in range(REPEATS):
            ms, n, err = one_run(s, cat)
            if ms is None:
                break
            count = n
            best = ms if best is None else min(best, ms)
        if best is None or not count:
            failures += 1
            print("%-11s FAIL  %s" % (cat, " | ".join(err.strip().splitlines()[-3:])))
        else:
            print("%-11s ok  %3d buttons  %6.0f ms" % (cat, count, best))

    s.close()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
