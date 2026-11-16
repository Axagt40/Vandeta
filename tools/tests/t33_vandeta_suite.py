#!/usr/bin/env python3
"""Vandeta suites pass (2026-10-28), driven over USB only.

Checks the parts of this session that can be seen without a keypad or a second
radio:

  * BLE Suite is the Vandeta BLE Suite: title bar VANDETA BLE + SUITE 1.0, the
    new Session Hijack row, none of the old BRUCE / v3.1 header,
  * entering it is fast (no 2 s splash) - measured from the key press to the
    first painted title,
  * the NRF24 RGB Remote page carries the new Hunt unknown remote row,
  * `nrfcap hunt` answers with a RESULT line and leaves the radio usable,
  * leaving BLE > Capture & Replay hands the BLE controller back (the ~57 KB
    that used to stay allocated).

The board is hard-reset first and driven with the stock `option <n>` verb, so no
Developer Options mode is needed. Stop the screen mirror first - it holds the
port.

    python3 FREEBUFF/tests/t33_vandeta_suite.py [port]
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

MAIN_BLE = 1
MAIN_NRF = 3
BLE_SUITE_ROW = 9
BLE_CAPTURE_ROW = 7

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
    s.dtr = True
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


def text(data):
    return b"\n".join(re.findall(rb"[\x20-\x7e]{3,}", data)).decode("ascii", "replace")


def mem_internal(s):
    s.reset_input_buffer()
    send(s, "mem", 1.2)
    out = text(grab(s, 0.5))
    m = re.search(r"internal free[:\s]+([0-9]+)", out, re.I)
    return int(m.group(1)) if m else None


def main():
    hard_reset()
    s = open_port()
    send(s, "display start", 0.5)

    print("BLE Suite chrome")
    send(s, "option %d" % MAIN_BLE, 1.0)
    t0 = time.time()
    send(s, "option %d" % BLE_SUITE_ROW, 0.2)
    seen = b""
    while time.time() - t0 < 5.0:
        seen += s.read(8192)
        if b"VANDETA BLE" in seen:
            break
        time.sleep(0.02)
    entry_ms = int((time.time() - t0) * 1000)
    body = text(seen)
    check("VANDETA BLE" in body, "title bar reads VANDETA BLE")
    check("SUITE 1.0" in body, "version label SUITE 1.0", body[:200])
    check("v3.1" not in body, "old v3.1 header is gone")
    check(entry_ms < 2500, "suite opens in %d ms (was ~2 s splash + draw)" % entry_ms)
    send(s, "nav esc", 0.6)
    send(s, "nav esc", 0.6)

    print("Session Hijack row")
    send(s, "option %d" % MAIN_BLE, 1.0)
    send(s, "option %d" % BLE_SUITE_ROW, 1.2)
    send(s, "nav down", 0.4)
    send(s, "nav down", 0.4)
    send(s, "nav down", 0.4)
    send(s, "nav down", 0.4)
    send(s, "nav down", 0.4)
    send(s, "nav down", 0.4)
    dump = text(grab(s, 0.6))
    check("Session Hijack" in dump, "Session Hijack (HID Keylog) is on the page", dump[:300])
    send(s, "nav esc", 0.6)
    send(s, "nav esc", 0.6)

    print("NRF24 hunt")
    send(s, "option %d" % MAIN_NRF, 1.0)
    send(s, "option 4", 1.2)  # RGB Remote
    dump = text(grab(s, 0.8))
    check("Hunt unknown remote" in dump, "RGB Remote page has the Hunt row", dump[:300])
    send(s, "nav esc", 0.6)
    send(s, "nav esc", 0.6)
    s.reset_input_buffer()
    send(s, "nrfcap hunt 200", 0.3)
    out = text(grab(s, 25))
    check("RESULT hunt" in out, "nrfcap hunt answers with a RESULT line", out[-300:])
    check("found=0" in out, "a hunt with no remote pressed reports found=0")
    s.reset_input_buffer()
    send(s, "nrfcap info", 0.8)
    info = text(grab(s, 0.6))
    check("profile" in info.lower() or "rate" in info.lower(), "radio answers again after the hunt", info[:200])

    print("Capture & Replay teardown")
    before = mem_internal(s)
    send(s, "option %d" % MAIN_BLE, 1.2)
    send(s, "option %d" % BLE_CAPTURE_ROW, 1.5)  # Capture & Replay
    # Sample WHILE the 5 s capture runs. Asking at 6 s used to measure after it
    # had already finished: the page tears the BLE stack down before it waits for
    # a key, so `during` came back at the post-teardown level and the two checks
    # below failed on a firmware that was behaving correctly.
    send(s, "option 0", 2.5)  # Start capture (5 s): this is what brings BLE up
    during = mem_internal(s)
    send(s, "nav sel", 0.6)  # let the summary page finish
    send(s, "nav esc", 1.5)
    send(s, "nav esc", 1.2)
    time.sleep(2.0)
    grab(s, 0.4)  # drain the page's last draws before asking for mem
    after = mem_internal(s)
    print("    internal free before=%s during=%s after=%s" % (before, during, after))
    check(before is not None and during is not None and during < before - 30000,
          "a capture really takes the BLE stack", "%s -> %s" % (before, during))
    check(before is not None and after is not None and after - during > 30000,
          "leaving Capture & Replay returns the BLE stack (~57 KB)",
          "%s -> %s" % (during, after))
    check(after is not None and abs(after - before) < 4000,
          "memory is back to the pre-capture level", "%s vs %s" % (before, after))

    s.close()
    print("\n%d passed / %d failed" % (passes, fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
