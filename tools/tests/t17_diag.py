#!/usr/bin/env python3
"""Task 17 diagnostics: watch the WiFi Packet Monitor stream and device health.

Prints stream byte counts over time, any crash markers, the mem report and the
dashboards's own text so we can tell a crash from a quiet (deduped) stream.

    python3 tools/tests/t17_diag.py [port]
"""
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200

CRASH = ("assert", "rst:0x", "Guru Meditation", "abort()", "Backtrace", "panic", "Stack canary")
WANT = ("PACKET MONITOR", "Pkts", "P/s", "showing", "WIFI", "BEACON", "PROBE", "MGMT", "DATA",
        "TIME RADIO", "Heap", "free", "PSRAM")


def open_port():
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    time.sleep(9)
    ser.reset_input_buffer()
    return ser


def send(ser, cmd, wait=0.4):
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    time.sleep(wait)


def parse_options(data):
    out = []
    for line in data.decode(errors="replace").splitlines():
        line = line.rstrip()
        if "Options available" in line:
            out = []
            continue
        m = re.match(r"\s*>?\s*(\d+)\s+-\s+(.*)$", line)
        if m:
            out.append((int(m.group(1)), m.group(2).strip()))
    return out


def grab(ser, seconds):
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        c = ser.read(8192)
        if c:
            buf.extend(c)
    return bytes(buf)


def report(label, data):
    text = data.decode(errors="replace")
    crashes = [l for l in text.splitlines() if any(k in l for k in CRASH)]
    wants = []
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        s = m.decode(errors="replace")
        if any(k in s for k in WANT) and s not in wants:
            wants.append(s)
    print(f"\n-- {label}: {len(data)} bytes --")
    if crashes:
        print("  !! CRASH MARKERS:")
        for c in crashes:
            print("    " + c.strip()[:100])
    for w in wants:
        print("  " + w)
    if len(data) < 3000:
        alls = []
        for m in re.findall(rb"[\x20-\x7e]{3,}", data):
            s = m.decode(errors="replace")
            if s not in alls:
                alls.append(s)
        print("  [all strings] " + " | ".join(alls[:40]))


def main():
    ser = open_port()
    try:
        send(ser, "display stop")
        send(ser, "display start")
        for _ in range(4):
            send(ser, "nav esc", 0.3)
        send(ser, "nav select", 1.2)  # enter WiFi
        send(ser, "nav next", 0.4)
        send(ser, "nav prev", 0.4)
        opts = parse_options(grab(ser, 2.0))
        idx = next((i for i, l in opts if l == "Packet Monitor"), None)
        print("Packet Monitor index:", idx)
        if idx is None:
            print("rows:", opts)
            return
        for _ in range(idx):
            send(ser, "nav next", 0.25)
        send(ser, "nav sel", 2.0)

        send(ser, "display stop")
        send(ser, "display start")
        send(ser, "nav next", 0.3)
        send(ser, "nav prev", 0.3)

        for i in range(4):
            report(f"t+{i*4}s stream", grab(ser, 4.0))
            send(ser, "mem", 0.8)
            report(f"t+{i*4}s mem", grab(ser, 1.2))
    finally:
        ser.close()


if __name__ == "__main__":
    main()
