#!/usr/bin/env python3
"""Check the Vandeta 1.1 screens exist on the flashed board.

Walks the real menus with the firmware's own `ui` verbs and asserts the rows
that 1.1 added are present:

  Bluetooth  > BLE Detector
  Infrared   > Universal Remote > AC > Brand remotes, Library codes
  RF         > Wardriver

Nothing is started (no scan, no capture) - this is a menu check, so it leaves
the radios and the UI where it found them. Run with the T-Embed on USB.
"""

import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"


class Dev:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.s.dtr = True
        time.sleep(0.3)
        self.drain(8)

    def drain(self, quiet=0.5, limit=8.0):
        end = time.time() + limit
        last = time.time()
        buf = b""
        while time.time() < end and time.time() - last < quiet:
            chunk = self.s.read(8192)
            if chunk:
                buf += chunk
                last = time.time()
        return buf.decode("utf-8", "replace")

    def cmd(self, line):
        self.s.write((line + "\n").encode())
        self.s.flush()
        return self.drain(0.6)

    @staticmethod
    def parse(text):
        blocks = text.split("[UI] ")
        if len(blocks) < 2:
            return None, None, []
        block = "[UI] " + blocks[-1]
        rows = []
        title = active = None
        for ln in block.splitlines():
            m = re.match(r"\[UI\] (.*) \[(\w+)\] (\d+) option\(s\), active=(-?\d+)", ln)
            if m:
                title, active = m.group(1), int(m.group(4))
            m = re.match(r"^\s*([ >])(\d+) - (.*)$", ln)
            if m:
                rows.append((m.group(3).strip(), m.group(1) == ">"))
        return title, active, rows

    def state(self):
        title, active, rows = self.parse(self.cmd("ui"))
        if title is None:
            raise SystemExit("no UI dump - is Developer Mode on and the port awake?")
        return title, active, rows

    def to_row(self, needle):
        for _ in range(40):
            title, active, rows = self.state()
            want = needle.lower()
            idx = next(
                (i for i, (lbl, _) in enumerate(rows)
                 if lbl.lower() == want or lbl.lower().startswith(want + ":")),
                None,
            )
            if idx is None:
                raise SystemExit(f"row '{needle}' not found on '{title}': {[r[0] for r in rows]}")
            if active == idx:
                return title, active, rows
            step = "down" if idx > active else "up"
            if abs(idx - active) > len(rows) // 2:
                step = "up" if step == "down" else "down"
            self.cmd(f"move {step}")
        raise SystemExit(f"could not highlight '{needle}'")

    def press(self, key="ok"):
        return self.parse(self.cmd(f"press {key}"))

    def open(self, needle):
        self.to_row(needle)
        title, active, rows = self.press("ok")
        if title is None:
            raise SystemExit(f"'{needle}' did not open a menu")
        return title, active, rows

    def back_to_main(self):
        for _ in range(8):
            if self.state()[0] == "Main Menu":
                return
            self.press("back")


def require(rows, needle, where):
    labels = [r[0] for r in rows]
    hit = [l for l in labels if l.lower() == needle.lower() or l.lower().startswith(needle.lower() + ":")]
    print(f"  {where}: {'FOUND' if hit else 'MISSING'} '{needle}'")
    return bool(hit)


def main():
    dev = Dev(PORT)
    out = dev.cmd("usbdebug on")
    if not any("ON" in l for l in out.splitlines() if "Developer Mode" in l):
        raise SystemExit("could not enable Developer Mode over serial")

    dev.back_to_main()
    results = {}

    title, _, rows = dev.open("BLE")
    print(f"\n=== {title}")
    results["BLE Detector (BLE)"] = require(rows, "BLE Detector", "BLE")
    dev.press("back")
    dev.back_to_main()

    _, _, rows = dev.open("IR")
    _, _, uni_rows = dev.open("Universal Remote")
    print("\n=== Universal Remote")
    if require(uni_rows, "AC", "Universal Remote"):
        _, _, ac_rows = dev.open("AC")
        print("=== AC")
        results["Brand remotes (AC)"] = require(ac_rows, "Brand remotes", "AC")
        results["Library codes (AC)"] = require(ac_rows, "Library codes", "AC")
        dev.press("back")
    else:
        results["Brand remotes (AC)"] = False
        results["Library codes (AC)"] = False
    dev.back_to_main()

    _, _, rows = dev.open("RF")
    print("\n=== RF")
    results["Wardriver (RF)"] = require(rows, "Wardriver", "RF")
    dev.press("back")
    dev.back_to_main()

    print("\nsummary:")
    for k, v in results.items():
        print(f"  {'PASS' if v else 'FAIL'}  {k}")
    ok = all(results.values())
    print("RESULT:", "PASS" if ok else "FAIL")
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
