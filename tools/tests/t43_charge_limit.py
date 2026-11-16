#!/usr/bin/env python3
"""Charge limit holds on the board (device, USB only).

The BQ25896 sends its registers back to defaults when its I2C watchdog expires,
so a limit could read back correctly and still be gone a minute later - that is
how "the charging limiter does not work" looked. This test drives the real
`battery` console verb:

  1. read the charge target at boot,
  2. set the limit to 80% and read it back (must be 4032 mV),
  3. set it to 70% and read it back (must be 3936 mV),
  4. wait 75 s - past the watchdog's 40 s window - and read it again (must still
     be 3936 mV, not the 4208 mV default),
  5. restore the limit the board had at the start.

It needs no key press and no charger: the target voltage is a register value,
not a battery state.

    python3 FREEBUFF/tests/t43_charge_limit.py [port]
"""
import os
import re
import subprocess
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
ESPTOOL = os.path.join(ROOT, ".pio", "packages", "tool-esptoolpy", "esptool.py")
WATCHDOG_WAIT = 75

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


def text(data):
    return b"\n".join(re.findall(rb"[\x20-\x7e]{3,}", data)).decode("ascii", "replace")


def field(body, name):
    m = re.search(name + r"\s*:\s*(.+)", body)
    return m.group(1).strip() if m else ""


def main():
    if os.path.exists(ESPTOOL):
        subprocess.run(
            [sys.executable, ESPTOOL, "--chip", "esp32s3", "--port", PORT, "--after", "hard_reset", "chip_id"],
            capture_output=True,
            timeout=60,
        )
    s = serial.Serial(PORT, 115200, timeout=0.2)
    s.dtr = True
    time.sleep(9)
    s.reset_input_buffer()

    def cmd(c, wait=1.0):
        s.write((c + "\r\n").encode())
        s.flush()
        time.sleep(wait)
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < 0.6:
            b = s.read(8192)
            if b:
                buf.extend(b)
        return text(bytes(buf))

    try:
        boot = cmd("battery", 1.2)
        start_body = cmd("battery", 1.0)
        start_pct = field(start_body, "Charge limit")
        check(start_pct.endswith("%"), "the battery page answers with a charge limit (" + start_pct + ")",
              start_body[-300:])
        check("Charge target :" in start_body, "the charger's target voltage is readable", start_body[-300:])

        cmd("settings -setting_name chargeLimit -setting_value 2", 1.2)  # 80%
        body = cmd("battery", 1.0)
        check(field(body, "Charge limit") == "80%", "the limit is set to 80%", field(body, "Charge limit"))
        check(field(body, "Charge target").startswith("4032"), "80% writes 4032 mV",
              field(body, "Charge target"))

        cmd("settings -setting_name chargeLimit -setting_value 3", 1.2)  # 70%
        body = cmd("battery", 1.0)
        check(field(body, "Charge limit") == "70%", "the limit is set to 70%", field(body, "Charge limit"))
        check(field(body, "Charge target").startswith("3936"), "70% writes 3936 mV",
              field(body, "Charge target"))

        print("  ... waiting %d s for the BQ25896 watchdog window" % WATCHDOG_WAIT)
        time.sleep(WATCHDOG_WAIT)
        body = cmd("battery", 1.2)
        target = field(body, "Charge target")
        check(target.startswith("3936"), "the limit survives the watchdog window (" + target + ")",
              body[-300:])
        check(not target.startswith("4208"), "the chip did not reload its 4208 mV default", target)

        # Put back whatever the board had, so the test leaves no setting behind.
        idx = {"100%": 0, "90%": 1, "80%": 2, "70%": 3}.get(start_pct, 0)
        cmd("settings -setting_name chargeLimit -setting_value %d" % idx, 1.2)
        body = cmd("battery", 1.0)
        check(field(body, "Charge limit") == start_pct,
              "the original limit is restored (" + field(body, "Charge limit") + ")")
    finally:
        s.close()

    print("\n%d passed, %d failed" % (passes, fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
