#!/usr/bin/env python3
"""Walk Config > System Config > Developer Options on the real device.

Uses the firmware's own `ui` verbs (textual menu dump) so the check is about the
actual option list the firmware renders, not a screenshot. Run with the T-Embed
on USB. DTR must be asserted or the USB-CDC RX stays closed and nothing parses.
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
        self.drain(8)  # the open resets the board; let it boot

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
            # Exact, or a label that carries state after a colon ("Developer
            # Mode: ON"). Substring matching picked "Live Update: config" for
            # "Config" and walked the wrong menu.
            want = needle.lower()
            idx = next(
                (
                    i
                    for i, (lbl, _) in enumerate(rows)
                    if lbl.lower() == want or lbl.lower().startswith(want + ":")
                ),
                None,
            )
            if idx is None:
                raise SystemExit(f"row '{needle}' not found on '{title}': {[r[0] for r in rows]}")
            if active == idx:
                return title, active, rows
            step = "down" if idx > active else "up"
            if abs(idx - active) > len(rows) // 2:  # cheaper to wrap the other way
                step = "up" if step == "down" else "down"
            self.cmd(f"move {step}")
        raise SystemExit(f"could not highlight '{needle}'")

    def press(self, key="ok"):
        return self.parse(self.cmd(f"press {key}"))


def show(step, title, active, rows):
    print(f"\n=== {step}: [{title}] active={active}")
    for lbl, hot in rows:
        print(("  > " if hot else "    ") + lbl)


def main():
    dev = Dev(PORT)
    out = dev.cmd("usbdebug on")
    on_lines = [l for l in out.splitlines() if "Developer Mode" in l or "developer mode" in l]
    print("usbdebug on ->", on_lines[:2])
    if not any("ON" in l for l in on_lines):
        raise SystemExit("could not enable Developer Mode over serial")

    # The device may be sitting in any submenu from the previous run.
    for _ in range(6):
        if dev.state()[0] == "Main Menu":
            break
        dev.press("back")

    dev.to_row("Config")
    title, active, rows = dev.press("ok")
    show("Config", title, active, rows)

    dev.to_row("System Config")
    title, active, rows = dev.press("ok")
    show("System Config", title, active, rows)

    dev.to_row("Developer Options")
    title, active, rows = dev.press("ok")
    show("Developer Options", title, active, rows)

    labels = [r[0] for r in rows]
    toggles = [l for l in labels if re.search(r":\s*(ON|OFF)\s*$", l)]
    # Any toggle that talks about developer / dev mode / usb debugging. The whole
    # point of the merge is that this list has exactly ONE entry.
    dev_toggles = [
        l for l in toggles if re.search(r"\bdev\w*|debug", l, re.IGNORECASE)
    ]
    print("\nrows:", labels)
    print("state-carrying rows:", toggles)
    print("dev-mode toggles:", dev_toggles)
    ok = len(dev_toggles) == 1 and dev_toggles[0].lower().startswith("developer mode")
    print("RESULT:", "PASS" if ok else "FAIL")


if __name__ == "__main__":
    main()
