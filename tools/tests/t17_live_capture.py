#!/usr/bin/env python3
"""Task 17 phase 2 verification: open the Packet Monitor and confirm live capture.

Navigates main -> RF -> Packet Monitor, then samples the streaming TFT log for a
few seconds and prints the dashboard lines (counter + rows).

    python3 FREEBUFF/tests/t17_live_capture.py [port]
"""
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200


def open_port():
    # Match the mirror/console handshake: assert nothing before opening (DTR/RTS
    # are wired to reset/boot on this chip), then wait for the CLI to boot.
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    time.sleep(9)  # opening the port resets the board
    ser.reset_input_buffer()
    return ser


def send(ser, cmd, wait=0.4):
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    time.sleep(wait)


def grab(ser, seconds=3.0):
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        chunk = ser.read(8192)
        if chunk:
            buf.extend(chunk)
    return bytes(buf)


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


def texts(data):
    out, seen = [], set()
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        s = m.decode(errors="replace")
        if s not in seen:
            seen.add(s)
            out.append(s)
    return out


def interesting(data):
    keep = ("PACKET", "Pkts", "P/s", "showing", "WIFI", "BEACON", "PROBE", "DATA", "MGMT",
            "TIME RADIO", "TURN", "PAUSED", "RUN", "FW", "radio")
    lines = []
    for s in texts(data):
        if s.startswith(("COMMAND", "# ", "Actual Menu", "Options available")):
            continue
        if any(k in s for k in keep):
            lines.append(s)
    return lines


def main():
    ser = open_port()
    try:
        send(ser, "display stop")
        send(ser, "display start")

        # Return to the main menu from wherever a previous run left the device.
        for _ in range(4):
            send(ser, "nav esc", 0.3)

        # Main menu order: WiFi(0) BLE(1) RF(2) ... Open WiFi (the default
        # selection) so this exercises the live 802.11 capture.
        send(ser, "nav select", 1.2)
        send(ser, "nav next", 0.4)
        send(ser, "nav prev", 0.4)
        opts = parse_options(grab(ser, 2.0))
        idx = next((i for i, l in opts if l == "Packet Monitor"), None)
        print("Packet Monitor row index:", idx)
        if idx is None:
            print("!! not found; RF rows were:", opts)
            return
        for _ in range(idx):
            send(ser, "nav next", 0.25)
        send(ser, "nav sel", 2.0)

        # Fresh viewer + a nudge so the dashboard repaints and streams.
        send(ser, "display stop")
        send(ser, "display start")
        send(ser, "nav next", 0.3)
        send(ser, "nav prev", 0.3)
        print("\n===== sample 1 (t+0s) =====")
        for l in interesting(grab(ser, 4.0)):
            print("  " + l)

        print("\n===== sample 2 (t+4s) =====")
        for l in interesting(grab(ser, 4.0)):
            print("  " + l)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
