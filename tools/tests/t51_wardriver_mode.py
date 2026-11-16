#!/usr/bin/env python3
"""Task 7: the wardriver's three-state Mode switch, over USB.

Checks, in the order the task lists them:

  1. the console verb: bare form reports, `hop|fasthop|wide` selects, anything
     else is refused with usage and leaves the mode alone;
  2. one source of truth: the console mode and `tune get ward.mode` agree;
  3. the mode survives a reboot (the config key is written and read back);
  4. `subghz wardriver selftest` passes in all three modes;
  5. a capture runs and stops cleanly in each mode, and the stop line carries
     the mode and the new `overruns` counter;
  6. the CC1101 is still usable afterwards - the wide filter is restored and a
     normal receive still returns.

What this cannot check on a bench with no remote/keyfob: a real held press in
fast hop, a 3 m remote captured in wideband, and a 433.60 MHz carrier. Those
are stated as limits in the change log, not claimed here.

    python3 FREEBUFF/tests/t51_wardriver_mode.py [port]
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

RESULTS = []
MODES = ["hop", "fasthop", "wide"]


def record(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("  %-54s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))


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
    time.sleep(9)
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
    out = []
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        t = m.decode(errors="replace").rstrip()
        if t and t not in out:
            out.append(t)
    return out


def console(s, cmd, wait=1.0):
    send(s, cmd, wait)
    return strings(grab(s, 0.7))


def find_line(lines, prefix):
    for t in reversed(lines):
        if t.startswith(prefix):
            return t
    return ""


def mode_of(lines):
    line = find_line(lines, "mode=")
    return line.split("=", 1)[1].strip() if line else None


def tune_mode(s):
    out = console(s, "tune get ward.mode", 1.2)
    for t in out:
        m = re.match(r"ward\.mode = (\d+)", t)
        if m:
            return int(m.group(1))
    return None


def main():
    hard_reset()
    s = open_port()
    try:
        print("\n=== 0. Developer Mode ON, start from Hop ===")
        console(s, "usbdebug on", 2.5)
        out = console(s, "subghz wardriver mode hop", 1.2)
        record("mode hop accepted", mode_of(out) == "hop", find_line(out, "mode="))
        out = console(s, "subghz wardriver mode", 1.0)
        record("bare mode reports the current value", mode_of(out) == "hop", find_line(out, "mode="))

        print("\n=== 1. the verb: select, read back, refuse nonsense ===")
        for i, m in enumerate(MODES):
            out = console(s, "subghz wardriver mode " + m, 1.2)
            record("mode %s accepted" % m, mode_of(out) == m, find_line(out, "mode="))
            out = console(s, "subghz wardriver status", 1.3)
            record("status reports %s" % m, mode_of(out) == m, find_line(out, "mode="))
            record("tune ward.mode is %d in %s" % (i, m), tune_mode(s) == i)

        out = console(s, "subghz wardriver mode nope", 1.2)
        joined = "\n".join(out)
        record("unknown mode refused with usage", "usage: subghz wardriver mode" in joined, find_line(out, "usage:"))
        out = console(s, "subghz wardriver mode", 1.0)
        record("a refusal leaves the mode alone", mode_of(out) == "wide", find_line(out, "mode="))

        print("\n=== 2. selftest passes in all three modes ===")
        for m in MODES:
            console(s, "subghz wardriver mode " + m, 1.0)
            out = console(s, "subghz wardriver selftest", 3.0)
            line = find_line(out, "[wardriver selftest]")
            record("selftest PASS in %s" % m, line.endswith("PASS"), line[-70:])

        print("\n=== 3. a capture runs and stops in each mode ===")
        for m in MODES:
            console(s, "subghz wardriver mode " + m, 1.0)
            send(s, "subghz wardriver capture 3", 0.5)
            out = strings(grab(s, 9.0))
            line = find_line(out, "[wardriver]")
            record(
                "capture completes in %s with the new counters" % m,
                bool(line) and "overruns" in line and m.upper() in line,
                line[-100:],
            )

        print("\n=== 4. the mode survives a reboot ===")
        console(s, "subghz wardriver mode fasthop", 1.2)
        s.write(b"reboot\r\n")
        s.flush()
        time.sleep(1.0)
        s.close()
        time.sleep(4)
        s = open_port()
        out = console(s, "subghz wardriver status", 1.5)
        record("after reboot the mode is fasthop", mode_of(out) == "fasthop", find_line(out, "mode="))

        print("\n=== 5. CC1101 still healthy after the wide filter ===")
        console(s, "subghz wardriver mode wide", 1.0)
        send(s, "subghz rx 2", 0.5)
        rx = strings(grab(s, 6.0))
        record("a normal receive still runs after wideband", bool(rx), " | ".join(rx[-3:]) if rx else "")
        out = console(s, "subghz wardriver mode hop", 1.2)
        record("mode restored to hop", mode_of(out) == "hop", find_line(out, "mode="))
    finally:
        s.close()

    ok = sum(1 for _, o, _ in RESULTS if o)
    print("\n===== SUMMARY =====")
    print("  %d passed, %d failed" % (ok, len(RESULTS) - ok))
    for n, o, _ in RESULTS:
        if not o:
            print("    FAILED: " + n)
    return 0 if ok == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
