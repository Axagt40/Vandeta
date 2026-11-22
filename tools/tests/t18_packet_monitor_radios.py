#!/usr/bin/env python3
"""Task 18 verification: the WiFi and BLE taps of the Packet Monitor.

Both taps used to answer "No WiFi radio" / "No BLE radio":

  * the WiFi driver had no internal DRAM left for its static RX buffers
    (ESP_ERR_NO_MEM - "Expected to init 4 rx buffer, actual is 3"),
  * NimBLE needs ~56 KB of free internal DRAM, and
  * the BLE capture task cancelled its own scan in the same millisecond because
    this NimBLE-Arduino's NimBLEScan::start() is asynchronous, so even a radio
    that came up collected nothing.

This script hard-resets the board, drives the real menus over USB (the `option
<n>` verb selects a row directly, so no key counting is involved) and prints the
dashboard's own counter line. A pass means packets were counted on screen, not
just that no error box appeared.

Stop the screen mirror first - it holds /dev/ttyACM0.

    python3 tools/tests/t18_packet_monitor_radios.py [port]
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

# Row indices on the board's menus (main menu / WiFi / Bluetooth).
WIFI_ROW = 0
WIFI_MONITOR_ROW = 11
BLE_ROW = 1
BLE_MONITOR_ROW = 7


def hard_reset():
    """Power-on-equivalent reset, so every run starts at the main menu."""
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
    time.sleep(9)  # the board boots while we wait
    s.reset_input_buffer()
    return s


def send(s, cmd, wait=0.6):
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
    out, seen = [], set()
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        t = m.decode(errors="replace")
        if t not in seen:
            seen.add(t)
            out.append(t)
    return [t for t in out if not t.startswith(("COMMAND", "# "))]


def select(s, row, expected, settle=1.8):
    send(s, "option %d" % row, settle)
    text = [t for t in strings(grab(s, 1.0)) if "Selected option" in t]
    print("  option %d -> %s" % (row, text[-1] if text else "no answer"))
    return any(("Selected option %d: %s" % (row, expected)) in t for t in text)


def sample(s, label, seconds):
    print(f"\n===== {label} =====")
    data = grab(s, seconds)
    lines = strings(data)
    missing = [t for t in lines if "No " in t and "radio" in t]
    if missing:
        print("  !! the dashboard reported a missing radio:", missing)
        return False
    keep = ("Pkts", "P/s", "showing", "WIFI", "BLE", "ADV", "BEACON", "PROBE",
            "DATA", "MGMT", "CTRL", "PAUSED", "RUN", "TURN")
    seen = [t for t in lines if any(k in t for k in keep)]
    for t in seen[-12:]:
        print("  " + t)
    counts = [int(m.group(1)) for t in seen for m in [re.search(r"Pkts (\d+)", t)] if m]
    highest = max(counts) if counts else 0
    print(f"  packets counted: {highest}")
    print("  RESULT:", "PASS - live capture" if highest > 0 else "FAIL - radio up, no packets")
    return highest > 0


def main():
    hard_reset()
    s = open_port()
    results = {}
    try:
        # Radio-memory baseline before anything is brought up.
        send(s, "mem", 1.5)
        print("===== RADIO MEMORY AT THE MENU =====")
        for line in strings(grab(s, 2.0)):
            if any(k in line for k in ("internal", "dma", "radio", "heap")):
                print("  " + line)

        # Arm the screen stream. Both of these return to the main menu, which is
        # where the index-based navigation below starts.
        send(s, "display stop")
        send(s, "display start")

        if select(s, WIFI_ROW, "WiFi"):
            if select(s, WIFI_MONITOR_ROW, "Packet Monitor", settle=2.5):
                results["wifi"] = sample(s, "WiFi PACKET MONITOR", 8.0)
            else:
                results["wifi"] = False
        else:
            results["wifi"] = False
        send(s, "nav esc", 1.0)
        send(s, "nav esc", 1.0)

        if select(s, BLE_ROW, "BLE"):
            if select(s, BLE_MONITOR_ROW, "Packet Monitor", settle=2.5):
                results["ble"] = sample(s, "BLE PACKET MONITOR", 10.0)
            else:
                results["ble"] = False
        else:
            results["ble"] = False
        send(s, "nav esc", 1.0)
        send(s, "nav esc", 1.0)
    finally:
        s.close()

    print("\n===== SUMMARY =====")
    for k, v in results.items():
        print(f"  {k:<4} {'PASS' if v else 'FAIL'}")
    return 0 if results and all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
