#!/usr/bin/env python3
"""Task 06 verification: drive the menus over USB and print the text the firmware
actually draws, so the breadcrumb / counter / hint bar can be checked without eyes.

Stop the screen mirror first - it holds /dev/ttyACM0.

    python3 tools/tests/t6_ui_capture.py [port]
"""
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200


def open_port():
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    time.sleep(8)  # opening the port resets the board; let it boot
    ser.reset_input_buffer()
    return ser


def send(ser, cmd, wait=0.4):
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    time.sleep(wait)


def repaint(ser):
    """tft_logger deduplicates, so a fresh viewer needs a stop/start + a nudge."""
    send(ser, "display stop")
    send(ser, "display start")
    send(ser, "nav next", 0.25)
    send(ser, "nav prev", 0.25)


def grab(ser, seconds=2.5):
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        chunk = ser.read(8192)
        if chunk:
            buf.extend(chunk)
    return bytes(buf)


def texts(data):
    out, seen = [], set()
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        s = m.decode(errors="replace")
        if s not in seen:
            seen.add(s)
            out.append(s)
    return out


def show(label, data):
    print(f"\n===== {label} =====")
    for s in texts(data):
        print("  " + s)


def main():
    ser = open_port()
    try:
        print("main menu")
        repaint(ser)
        show("MAIN MENU (no breadcrumb/footer expected)", grab(ser))

        # main menu starts on WiFi; step back once to wrap onto Config (last row)
        send(ser, "nav prev", 0.5)
        send(ser, "nav select", 1.0)
        repaint(ser)
        show("CONFIG SUBMENU - breadcrumb 'Config'", grab(ser))

        # open "Display & UI" -> breadcrumb must grow to "Config > Display & UI"
        send(ser, "nav select", 1.5)
        repaint(ser)
        show("DISPLAY & UI - breadcrumb 'Config > Display & UI'", grab(ser))

        send(ser, "nav next", 0.5)
        show("DISPLAY & UI after one move (counter should be 2/6)", grab(ser))

        # ESC back up one level
        send(ser, "nav esc", 1.0)
        repaint(ser)
        show("AFTER BACK (breadcrumb should shrink to 'Config')", grab(ser))
    finally:
        ser.close()


if __name__ == "__main__":
    main()
