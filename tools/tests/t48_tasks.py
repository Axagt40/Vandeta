#!/usr/bin/env python3
"""t48_tasks.py - device checks for the task 2-6 work.

    ~/.venv/bin/python t48_tasks.py [/dev/ttyACM0]

Covers: the `rfbits` editor and de Bruijn generator, the blue-team dashboard,
the fingerprint capture, the audio modem, and the Task 6 `info` IP fix.
"""
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
passed = failed = 0


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

    def cmd(self, line, wait=2.0):
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

# --- Task 2: bit editor -----------------------------------------------------
out = d.cmd("rfbits set 1010 350 433.92")
check("rfbits set", "1010" in out, out)
out = d.cmd("rfbits flip 0")
check("rfbits flip", "0010" in out, out)
out = d.cmd("rfbits trim 1")
check("rfbits trim", "010" in out, out)
out = d.cmd("rfbits stretch 10")
check("rfbits stretch te", "385" in out, out)
out = d.cmd("rfbits shorten 10")
check("rfbits shorten te", "346" in out, out)
out = d.cmd("rfbits repeat 2")
check("rfbits repeat", "repeat 2" in out, out)
out = d.cmd("rfbits flip 99")
check("rfbits out of range refused", "ERR" in out, out)

out = d.cmd("rfbits debruijn 4", wait=8.0)
check("debruijn sequence is 2^4", "16 bits" in out, out)
check("debruijn report", "forced" in out, out)

# --- Task 3/5: dashboard + fingerprints -------------------------------------
out = d.cmd("bluewatch 2", wait=12.0)
check("bluewatch summary", "counter-surveillance:" in out, out)
check("bluewatch counts deauth", "deauth/disassoc" in out, out)
check("bluewatch counts remote id", "remote id" in out, out)

out = d.cmd("wififp 2", wait=12.0)
check("wififp summary", "fingerprints:" in out, out)

# --- Task 4: IR learn-and-normalize ----------------------------------------
out = d.cmd("irnorm 2", wait=8.0)
check("irnorm reports an honest timeout", "ERR: no IR signal in 2s" in out, out)

# --- Task 3: BLE tracker rotation cadence ----------------------------------
out = d.cmd("blecadence 6", wait=20.0)
check("blecadence summary", "ble tracker cadence:" in out, out)
check("blecadence counts advertisements", "advertisements" in out, out)

# --- Task 5: 802.1X identity harvest ---------------------------------------
out = d.cmd("eapolid 5", wait=12.0)
check("eapolid summary", "eapol: " in out and "EAPOL frames" in out, out)
check("eapolid states its limit", "outer identity only" in out, out)

# --- Task 4: modem ----------------------------------------------------------
out = d.cmd("modem send hello 1200", wait=10.0)
check("modem sends a frame", "OK sent" in out, out)
out = d.cmd("modem recv 1")
check("modem recv reports TX-only honestly", "ERR: audio modem RX is not implemented" in out, out)

# --- Task 6: info never claims a connection without an address --------------
out = d.cmd("info")
check("info has no empty-IP connection", not re.search(r"Wifi: connected\s*\n\s*Ip: \s*\n", out), out)

d.close()
print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
