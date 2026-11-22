#!/usr/bin/env python3
"""Task 17 verification: find the Packet Monitor row in the WiFi menu over USB,
open it and print the text the dashboard draws.

Stop the screen mirror first - it holds /dev/ttyACM0.

    python3 tools/tests/t17_ui_capture.py [port]
"""
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200


def open_port():
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    time.sleep(8)  # opening the port resets the board
    ser.reset_input_buffer()
    return ser


def send(ser, cmd, wait=0.4):
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    time.sleep(wait)


def read_until(ser, needle, timeout=3.0):
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < timeout:
        chunk = ser.read(8192)
        if chunk:
            buf.extend(chunk)
            if needle.encode() in buf:
                break
    return bytes(buf)


def grab(ser, seconds=2.5):
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        chunk = ser.read(8192)
        if chunk:
            buf.extend(chunk)
    return bytes(buf)


def parse_options(data):
    """Return the LAST "Options available" block in the buffer."""
    out = []
    for line in data.decode(errors="replace").splitlines():
        line = line.rstrip()
        if "Options available" in line:
            out = []  # a new dump - start over
            continue
        m = re.match(r"\s*>?\s*(\d+)\s+-\s+(.*)$", line)
        if m:
            out.append((int(m.group(1)), m.group(2).strip()))
    return out


def texts(data):
    out, seen = [], set()
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        s = m.decode(errors="replace")
        if s not in seen:
            seen.add(s)
            out.append(s)
    return out


def main():
    ser = open_port()
    try:
        send(ser, "display stop")
        send(ser, "display start")

        # Main menu order is fixed: WiFi(0) BLE(1) RF(2) ... Use the RF menu,
        # whose rows are static (the WiFi rows shift with the WiFi state).
        send(ser, "nav next", 0.3)    # WiFi -> BLE
        send(ser, "nav next", 0.3)    # BLE  -> RF
        send(ser, "nav select", 1.0)  # enter RF
        # nudge twice (net zero) so the console dumps the RF row list
        send(ser, "nav next", 0.4)
        send(ser, "nav prev", 0.4)
        data = grab(ser, 2.5)
        opts = parse_options(data)
        print("RF menu rows:")
        for i, l in opts:
            print(f"  {i:>2} - {l}")

        idx = next((i for i, l in opts if l == "Packet Monitor"), None)
        print(f"\nPacket Monitor index: {idx}")
        if idx is None:
            print("!! row not found")
            return

        for _ in range(idx):
            send(ser, "nav next", 0.25)
        send(ser, "nav sel", 1.5)

        send(ser, "display stop")
        send(ser, "display start")
        send(ser, "nav next", 0.3)  # move one row (net zero after prev)
        send(ser, "nav prev", 0.3)
        show("PACKET MONITOR DASHBOARD", grab(ser, 3.0))

        # open the dashboard menu
        send(ser, "nav sel", 1.2)
        send(ser, "display stop")
        send(ser, "display start")
        send(ser, "nav next", 0.3)
        send(ser, "nav prev", 0.3)
        show("DASHBOARD MENU (SEL)", grab(ser, 3.0))
    finally:
        ser.close()


def show(label, data):
    print(f"\n===== {label} =====")
    kept = [s for s in texts(data) if not s.startswith(("COMMAND", "# ", "Actual Menu", "Options available"))]
    for s in kept:
        print("  " + s)


if __name__ == "__main__":
    main()
