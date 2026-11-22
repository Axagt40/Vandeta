#!/usr/bin/env python3
"""t46_devflash.py - device test for the phase 4 upload path.

Run on the host with the board on USB and Developer Mode on:

    ~/.venv/bin/python t46_devflash.py [/dev/ttyACM0]

It does not flash the board: with a single-app partition table `devflash` must
refuse with an explicit reason, and the test asserts that reason instead of
writing anything. On an OTA-capable build the same script reports the begin
result and stops before sending data unless --write is given.
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
WRITE = "--write" in sys.argv

passed = 0
failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"PASS {name}")
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


class Dev:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.2)
        time.sleep(0.3)
        self.drain()

    def drain(self):
        self.s.reset_input_buffer()
        time.sleep(0.15)

    def cmd(self, line, wait=1.4):
        self.drain()
        self.s.write(line.encode() + b"\n")
        deadline = time.time() + wait
        buf = b""
        while time.time() < deadline:
            b = self.s.read(4096)
            if b:
                buf += b
                if b"# " in buf:
                    break
        return buf.decode(errors="replace")

    def close(self):
        self.s.close()


d = Dev(PORT)

out = d.cmd("info")
check("info prints build", "build: " in out, out)
out = d.cmd("devflash")
check("status prints build id", "build " in out, out)
check("status is idle", "devflash idle" in out, out)

# Gate: everything must refuse with dev mode off, and help must not list it.
d.cmd("usbdebug off")
out = d.cmd("devflash status")
check("dev mode off refuses", "ERR: developer mode off" in out, out)
out = d.cmd("help")
check("help omits devflash with dev mode off", "devflash" not in out, out)
d.cmd("usbdebug on")

# Malformed begins are refused with one line and never start a transfer.
out = d.cmd("devflash begin 0 0")
check("zero size refused", "ERR: image size is 0" in out, out)

out = d.cmd("devflash begin 1000 12345678 0000000000000000")
check("build id mismatch refused", "ERR: build ID mismatch" in out or "ERR: no inactive OTA partition" in out, out)

out = d.cmd("devflash chunk 0 00")
check("chunk without begin refused", "ERR: no transfer open" in out, out)

out = d.cmd("devflash abort")
check("abort is harmless", "OK nothing to abort" in out, out)

# Real begin: on this single-app image it must name the missing OTA slot.
out = d.cmd("devflash begin 1024 00000000", wait=3.0)
if "no inactive OTA partition" in out:
    check("single-app table reported, not guessed", True)
elif "OK begin" in out:
    check("begin accepted on an OTA-capable image", True)
    if not WRITE:
        d.cmd("devflash abort")
        print("note: --write not given, transfer aborted after begin")
else:
    check("begin either starts or names the blocker", False, out)

d.close()
print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
