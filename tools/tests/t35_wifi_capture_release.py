#!/usr/bin/env python3
"""T35 - a scripted WiFi capture gives the driver back when it stops.

The 2026-10-27 build kept the WiFi driver's DMA buffers allocated after
`wifi.captureStop()` (25,171 bytes). `raw_release_driver_if_idle()` in
`wifi_atks.cpp` now deinits the driver when the capture was the thing that
brought it up. This measures that on the board, over the text console, with two
back-to-back capture cycles:

    mem -> js /t35cap.js -> mem -> js /t35cap.js -> mem

PASS when
  * each cycle reports a session id and `stop=true`,
  * the second cycle brings the driver up again - `wifi_atk_setWifi()` prints
    its `[HEAP] Wifi atk start` line only when `WiFi.getMode()` is NULL, so that
    line proves the first stop really removed the driver rather than leaving the
    Arduino wrapper thinking it is still up,
  * `internal free` after the second cycle is within 2 KB of after the first,
    i.e. a capture no longer costs 25 KB per run.

The first WiFi use of a boot permanently initialises lwIP / the default event
loop (~18 KB of internal DRAM), and that cost is paid before the first capture
starts - which is why the check is cycle-to-cycle, not against a fresh boot.

The probe script is uploaded first by this host script (upload, run, delete), so
nothing is left on the card.

    python3 FREEBUFF/tests/t35_wifi_capture_release.py [port]
"""
import os
import re
import subprocess
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200
PROBE = os.path.join(HERE, "t35_wifi_capture_probe.js")
REMOTE = "/t35cap.js"
USB_DEBUG = os.path.join(HERE, "..", "tools", "usb_debug.py")

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


def upload_probe():
    """put the probe on the device (this opens the port and resets the board)."""
    subprocess.run(
        [sys.executable, USB_DEBUG, PORT, "put", PROBE, REMOTE],
        capture_output=True,
        timeout=120,
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


def run_probe(s):
    """one capture cycle; returns the script's console text."""
    s.reset_input_buffer()
    send(s, "js " + REMOTE, 0.3)
    seen = b""
    t0 = time.time()
    while time.time() - t0 < 30:
        c = s.read(8192)
        if c:
            seen += c
        if b"Exiting interpreter" in seen:
            break
    time.sleep(0.5)
    return text(seen)


def main():
    upload_probe()
    s = open_port()

    boot = mem_internal(s)
    first = run_probe(s)
    after_first = mem_internal(s)
    second = run_probe(s)
    after_second = mem_internal(s)

    cap1 = re.search(r"cap=(-?[0-9]+)", first)
    cap2 = re.search(r"cap=(-?[0-9]+)", second)
    check(cap1 is not None and int(cap1.group(1)) > 0, "first captureStart returns a session id", first[-300:])
    check("stop=true" in first, "first captureStop reports success", first[-300:])
    check("T35 done" in first and "T35 done" in second, "both cycles ran to the end")
    check(cap2 is not None and int(cap2.group(1)) > 0, "second captureStart returns a session id", second[-300:])
    check("stop=true" in second, "second captureStop reports success", second[-300:])
    check(first.count("[HEAP] Wifi atk start") == 1 and second.count("[HEAP] Wifi atk start") == 1,
          "the second cycle brings the driver up again (so the first stop removed it)",
          "first=%d second=%d" % (first.count("[HEAP] Wifi atk start"),
                                  second.count("[HEAP] Wifi atk start")))
    print("    internal free boot=%s after1=%s after2=%s" % (boot, after_first, after_second))
    check(after_first is not None and after_second is not None, "mem answers after both cycles",
          "%s -> %s" % (after_first, after_second))
    if after_first is not None and after_second is not None:
        check(after_second >= after_first - 2048,
              "a capture costs nothing per run (second cycle within 2 KB of the first)",
              "after1=%s after2=%s delta=%s" % (after_first, after_second, after_second - after_first))

    send(s, "file delete " + REMOTE, 0.6)
    s.close()
    print("\n%d passed / %d failed" % (passes, fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
