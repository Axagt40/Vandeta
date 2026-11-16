#!/usr/bin/env python3
"""BLE Atks page regression.

Drives the real menu through the Developer Options `move`/`press` verbs, so the
test covers the same rows a finger would touch. It needs developer mode on and a
connectable BLE advertiser nearby for both Pairing Storm modes (the host's
`btmgmt advertising on` is enough; the test reports an honest SKIP if none is
found).

Usage: python t50_ble_attacks.py [/dev/ttyACM0]
"""
import json
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
s = serial.Serial(PORT, 115200, timeout=0.2)
s.dtr = True
s.rts = False
time.sleep(1)

passed = failed = skipped = 0


def cmd(text, wait=3.0):
    s.reset_input_buffer()
    s.write((text + "\r\n").encode())
    s.flush()
    end = time.time() + wait
    out = b""
    while time.time() < end:
        chunk = s.read(8192)
        if chunk:
            out += chunk
            if b"# " in out:
                break
    # The prompt can arrive before the page it describes has been drawn, so a
    # `ui select` reply read here sometimes held only the echo. Keep reading
    # for a short quiet period once the command has answered.
    if b"# " in out:
        settle_end = time.time() + 0.5
        while time.time() < settle_end:
            chunk = s.read(8192)
            if chunk:
                out += chunk
                settle_end = time.time() + 0.3
    return out.decode(errors="replace")


def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print("  PASS  %s%s" % (name, ("  " + detail) if detail else ""))
    else:
        failed += 1
        print("  FAIL  %s  %s" % (name, detail))


def skip(name, why):
    global skipped
    skipped += 1
    print("  SKIP  %s  %s" % (name, why))


def spin(seconds, needle, label):
    """Wait up to `seconds` for a serial line, returning all output seen."""
    end = time.time() + seconds
    out = ""
    while time.time() < end:
        chunk = s.read(4096).decode(errors="replace")
        out += chunk
        if needle and needle in out:
            return out
        time.sleep(0.1)
    return out


print("=== setup ===")
setup = cmd("usbdebug on", 3)
check("developer mode is on", "Developer Mode: ON" in setup or "developer mode ON" in setup)
cmd("ui watch on", 2)


def go_home():
    """The board keeps the last screen across a reset, so walk back first."""
    for _ in range(6):
        out = cmd("ui", 2)
        if '"menu_title":"Main Menu"' in out:
            return True
        out = cmd("press back", 2)
        if '"menu_title":"Main Menu"' in out:
            return True
    return False

check("main menu is reachable", go_home())


def pick(label, wait=3.0):
    """Open the row whose label is `label`, found in the UISTATE dump.

    The BLE page is grouped now and its session row ("Disconnect") only exists
    while connected, so a fixed row index is not a stable way in.
    """
    st = None
    m = re.search(r"UISTATE (\{.*\})", cmd("ui", 2))
    if m:
        try:
            st = json.loads(m.group(1))
        except ValueError:
            st = None
    idx = next((o["n"] for o in (st or {}).get("options", []) if o["label"] == label), None)
    if idx is None:
        return "", False
    return cmd("ui select %d" % idx, wait), True


def open_atks():
    """Open BLE > Attacks > BLE Atks from the Main Menu.

    The console sets returnToMenu on every command, so a page that closes
    unwinds all the way to the Main Menu; each attack needs the walk back.
    """
    go_home()
    cmd("ui select 1", 3)  # BLE
    out, ok = pick("Attacks")
    if not ok:
        return out
    out, ok = pick("BLE Atks")
    return out

# BLE is main-menu row 1; the attack pages moved under the Attacks category.
out = cmd("ui select 1", 4)
check("BLE menu opens", "[UI] Bluetooth [sub]" in out)
for row in ("Scan & Detect", "Attacks", "HID & BadUSB", "Tools"):
    check("BLE page carries the %s group" % row, row in out)
check("BLE Atks is no longer a top-level BLE row", '"label":"BLE Atks"' not in out)
out = open_atks()
check("BLE Atks is a flat page under Attacks", "[UI] BLE Atks [regular] 5 option(s)" in out)
for row in ("BLE Cadence", "Pairing Storm", "BLE Flood", "Auth Capture"):
    check("row %s exists" % row, row in out)

print("=== BLE Cadence ===")
out = cmd("ui select 0", 3)
started = out + spin(4, "[BLEUI] cadence start", "cadence start")
check("cadence shows an immediate scan state", "[BLEUI] cadence start 20s" in started, started[-150:])
# Read while the pass runs: the console line is printed before the report
# page opens and cmd() drains the buffer, so sleeping here would drop it.
scan = spin(26, "[BLEUI] cadence done", "cadence done")
cmd("press ok", 4)
check("cadence finishes and reports", "[BLEUI] cadence done" in scan)

print("=== BLE Flood ===")
open_atks()
out = cmd("ui select 2", 3)
started = out + spin(2, "[UI] BLE Flood [sub]", "flood picker")
check("BLE Flood offers the SSID-spam style modes",
      all(x in started for x in ("Funny Names", "Rickroll", "Random Names", "Custom Name")), started[-300:])
out = cmd("ui select 0", 3)
started = out + spin(3, "[BLEATK] bleflood start", "flood start")
check("BLE Flood starts", "[BLEATK] bleflood start mode=0" in started)
adv = spin(24, "[BLEATK] bleflood packets=", "flood result")
m = re.search(r"bleflood packets=(\d+)", adv)
check("BLE Flood sends packets", bool(m) and int(m.group(1)) > 0, adv[-200:])
time.sleep(0.8)
cmd("press ok", 4)

print("=== Pairing Storm page ===")
open_atks()
out = cmd("ui select 1", 3)
page = out + spin(2, "[UI] Pairing Storm [sub]", "storm page")
check("Pairing Storm opens a start page", "[UI] Pairing Storm [sub]" in page, page[-200:])
check("page offers Start Attack", "Start Attack" in page)
check("page defaults to connect + pair", "Mode: connect + pair" in page, page[-200:])

# Connect-only: flip the mode row, then start from the same page.
cmd("move down", 2)
cmd("press ok", 2)
state = cmd("ui", 2)
check("mode toggles to connect only", "Mode: connect only" in state, state[-200:])
cmd("move up", 2)
out = cmd("press ok", 3)
started = out + spin(3, "[BLEATK] storm start", "storm start")
check("connect-only storm starts", "[BLEATK] storm start" in started)
storm = spin(32, "[BLEATK] storm targets=", "storm result")
m = re.search(r"storm targets=(\d+) attempts=(\d+) links=(\d+)", storm)
if not m or int(m.group(1)) == 0:
    skip("connect-only storm reaches a connectable target", "no connectable advertiser in range")
else:
    check("connect-only storm attempts links", int(m.group(2)) > 0, m.group(0))
time.sleep(1.2)  # let the result page clear the key flags before pressing it
cmd("press ok", 4)

print("=== Pairing Storm (pair mode) ===")
open_atks()
out = cmd("ui select 1", 3)
out += spin(2, "[UI] Pairing Storm [sub]", "storm page")
cmd("move down", 2)  # cursor starts on Start Attack again
cmd("press ok", 2)   # toggle back to connect + pair
state = cmd("ui", 2)
check("mode toggles back to connect + pair", "Mode: connect + pair" in state, state[-200:])
cmd("move up", 2)
out = cmd("press ok", 3)
started = out + spin(3, "[BLEATK] pairing start", "pairing start")
check("pairing storm starts", "[BLEATK] pairing start" in started)
pair = spin(90, "[BLEATK] pairing targets=", "pairing result")  # a stalled SMP can outlast the 20s window
m = re.search(r"pairing targets=(\d+) attempts=(\d+) links=(\d+) security=(\d+) known=(\d+)", pair)
if not m or int(m.group(1)) == 0:
    skip("pairing storm reaches a connectable target", "no connectable advertiser in range")
else:
    check("pairing storm opens links", int(m.group(3)) > 0, m.group(0))
    check("pairing storm reports stored-auth count", "known=" in m.group(0), m.group(0))
time.sleep(1.2)  # result page clears the key flags on entry
cmd("press ok", 4)

print("=== Auth Capture (direct, no submenu) ===")
open_atks()
out = cmd("ui select 3", 3)
started = out + spin(2, "[BLEATK] auth capture start", "capture start")
check("Auth Capture launches with one press", "[BLEATK] auth capture start" in started)
check("Auth Capture has no nested menu", "Auth Capture [sub]" not in started, started[-200:])
# Auth Capture pairs with every fresh target in turn, 8 s connect timeout each,
# so a room with four new devices legitimately takes over 30 s to report.
# Five fresh targets at an 8 s connect timeout each already took 40 s, so the
# old 45 s window reported "no outcome" for a run that was still going.
capture = out + spin(80, "[BLEATK] auth capture done", "capture result")
check("capture scans and skips already saved devices",
      "[BLEATK] auth capture targets=" in capture and "[BLEATK] auth capture new=" in capture, capture[-300:])
check("capture reports an outcome", "[BLEATK] auth capture done" in capture, capture[-220:])
cmd("press ok", 4)
cmd("press back", 3)
cmd("press back", 3)

print("\n%d passed, %d failed, %d skipped" % (passed, failed, skipped))
s.close()
sys.exit(1 if failed else 0)
