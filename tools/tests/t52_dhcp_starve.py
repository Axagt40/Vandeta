#!/usr/bin/env python3
"""DHCP starvation in WiFi > Wifi Atks > Target Atks, over USB.

What this checks on the board:

  1. the console group still routes and its usage lists `dhcpstarve`;
  2. argument refusals: bad BSSID, out-of-range channel, missing SSID;
  3. an unreachable AP fails through the real join path and says so
     (`ok=0 error=join failed (password/range)`), leaving WiFi off;
  4. the Target Atks menu carries a `DHCP Starvation` row, next to the other
     per-AP attacks, and it is reachable from the scanned AP list;
  5. no crash markers afterwards.

What this CANNOT check by itself: the starvation run. It was verified manually
against an isolated phone hotspot on 2026-11-15: 253 distinct leases were
committed (the whole /24) and the server then answered 200 requests with
"Out of addresses to offer" - see CHANGES rows 219-220. This script still does
not open the row, because opening it starts a real starvation that runs until
BACK against the AP that was selected.

The row's label and position are checked; the attack itself is not run here.

    python3 tools/tests/t52_dhcp_starve.py [port]
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

RESULTS = []


def record(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("  %-54s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))


def skip(name, detail=""):
    RESULTS.append((name, None, detail))
    print("  %-54s SKIP%s" % (name, ("  " + detail) if detail else ""))


def hard_reset():
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
    time.sleep(9)
    s.reset_input_buffer()
    return s


def send(s, cmd, wait=0.8):
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
    out = []
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        t = m.decode(errors="replace").rstrip()
        if t and t not in out:
            out.append(t)
    return out


ROW = re.compile(r"^\s*[> ]\s*(\d+) - (.*)$")


def rows(text_lines):
    found = []
    for t in text_lines:
        m = ROW.match(t)
        if m:
            found.append((int(m.group(1)), m.group(2).strip()))
    return found


def find(option_rows, needle):
    for idx, label in option_rows:
        if needle.lower() in label.lower():
            return idx
    return None


def ui(s, wait=1.2):
    send(s, "ui", wait)
    return strings(grab(s, 0.8))


def select(s, index, settle=1.6):
    send(s, "ui select %d" % index, settle)
    return strings(grab(s, 0.9))


def nav(s, action="back", settle=1.4):
    send(s, "ui " + action, settle)
    return strings(grab(s, 0.8))


def console(s, cmd, wait=1.2):
    send(s, cmd, wait)
    return strings(grab(s, 0.8))


def find_line(lines, prefix):
    for t in reversed(lines):
        if t.startswith(prefix):
            return t
    return ""


def field(line, key):
    m = re.search(r"\b%s=(\S+)" % re.escape(key), line)
    return m.group(1) if m else None


def goto_wifi_atks(s):
    """Main Menu > WiFi > Wifi Atks, whatever screen we are on now."""
    lines = ui(s)
    wifi = find(rows(lines), "WiFi")
    if wifi is None:
        return []
    lines = select(s, wifi)
    # Wifi Atks moved under the new Attacks category on the WiFi page.
    attacks = find(rows(lines), "Attacks")
    if attacks is None:
        return []
    lines = select(s, attacks, settle=1.2)
    atks = find(rows(lines), "Wifi Atks")
    if atks is None:
        return []
    return select(s, atks, settle=2.0)


def main():
    hard_reset()
    s = open_port()
    try:
        out = console(s, "usbdebug status", 1.5)
        if not any("Developer Mode: ON" in t for t in out):
            out = console(s, "usbdebug on", 2.0)
        record(
            "Developer Options enabled",
            any("Developer Mode: ON" in t for t in out),
            find_line(out, "Developer Mode:"),
        )

        print("\n=== 1. the console group lists the new mode ===")
        # A bare unknown mode is how the group prints its usage; a bare
        # `dhcpstarve` goes straight to the BSSID check, like every other mode.
        # The group prints its usage only after the mode-independent BSSID and
        # channel checks, so an unknown mode needs a valid pair to reach it.
        out = console(s, "moreatk nosuchmode AA:BB:CC:DD:EE:FF 6", 1.2)
        modes = find_line(out, "modes:")
        record("usage lists dhcpstarve", "dhcpstarve" in modes, modes)
        record("usage does not list a sweep mode", "dhcpstarveall" not in modes, modes)
        record(
            "the other targeted modes are still listed",
            all(m in modes for m in ("eapollogoff", "badmsg", "auth", "disassoc", "chanswitch")),
            modes,
        )

        print("\n=== 2. argument refusals ===")
        out = console(s, "moreatk dhcpstarve zz:zz 6 vandeta-nope 2", 1.2)
        line = find_line(out, "ERR bad bssid")
        record("a malformed BSSID is refused", bool(line), line)

        out = console(s, "moreatk dhcpstarve AA:BB:CC:DD:EE:FF 99 vandeta-nope 2", 1.2)
        line = find_line(out, "ERR channel must be 1-14")
        record("an out-of-range channel is refused", bool(line), line)

        out = console(s, "moreatk dhcpstarve AA:BB:CC:DD:EE:FF 6", 1.2)
        line = find_line(out, "ERR dhcpstarve needs")
        record("a missing SSID is refused with the usage line", bool(line), line)

        print("\n=== 3. an unreachable AP fails through the real join path ===")
        out = console(s, "moreatk dhcpstarve 02:00:00:00:00:01 6 vandeta-no-such-ap 2", 14.0)
        line = find_line(out, "RESULT name=dhcpstarve")
        print("  " + (line or "no answer"))
        record(
            "join failure is reported, not counted as frames",
            field(line, "ok") == "0" and "join failed" in line and field(line, "frames") == "0",
            line,
        )
        out = console(s, "info", 1.5)
        wifi_line = find_line(out, "Wifi:")
        record(
            "WiFi is left off after the failed join",
            "not connected" in wifi_line.lower(),
            wifi_line,
        )

        print("\n=== 4. Target Atks carries the row ===")
        atks = goto_wifi_atks(s)
        target_row = find(rows(atks), "Target Atks")
        record("Wifi Atks opens and carries Target Atks", target_row is not None, "row=%s" % target_row)
        if target_row is None:
            return finish()

        lines = select(s, target_row, 3.5)
        # The scan takes a couple of seconds; poll until rows that look like
        # scanned APs (label + rssi|enc|ch.N) are on screen instead of trusting
        # the first read, which can still be the previous page's text.
        ap_rows = []
        for _ in range(5):
            ap_rows = [r for r in rows(lines) if re.search(r"\|ch\.\d+|<Hidden SSID>", r[1])]
            if ap_rows:
                break
            send(s, "ui", 1.2)
            lines = strings(grab(s, 0.8))
        print("  AP rows:", [l for _, l in ap_rows][:6])
        if not ap_rows:
            skip("an AP is in range to open Target Atks for", "scan found none")
            return finish()

        page = select(s, ap_rows[0][0], 2.2)
        labels = [l for _, l in rows(page)]
        print("  Target Atks rows:", labels)
        record("Target Atks has a DHCP Starvation row", find(rows(page), "DHCP Starvation") is not None, repr(labels))
        dhcp_row = find(rows(page), "DHCP Starvation")
        dis_row = find(rows(page), "Disassociation Attack")
        record(
            "it follows the per-AP frame attacks",
            dhcp_row is not None and dis_row is not None and dhcp_row == dis_row + 1,
            "disassoc=%s dhcp=%s" % (dis_row, dhcp_row),
        )
        record(
            "the older per-AP attacks are still there",
            find(rows(page), "EAPOL Logoff") is not None
            and find(rows(page), "Authentication Attack") is not None
            and find(rows(page), "Clone Portal") is not None,
        )
        # Deliberately do not select the row: it starts a real starvation that
        # runs until BACK against the AP that was opened.
        skip("opening the row (runs until BACK)", "would attack the live LAN")

        nav(s, "back")
        nav(s, "back")

        print("\n=== 5. no crash markers ===")
        out = console(s, "errors", 1.5)
        crashes = [t for t in out if re.search(r"Guru|panic|abort|Backtrace|assert", t, re.I)]
        record("no crash markers in the error ring", len(crashes) == 0, repr(crashes[:3]))
    finally:
        s.close()

    return finish()


def finish():
    print("\n===== SUMMARY =====")
    passed = sum(1 for _, ok, _ in RESULTS if ok is True)
    failed = [n for n, ok, _ in RESULTS if ok is False]
    skipped = [n for n, ok, _ in RESULTS if ok is None]
    print("  %d passed, %d failed, %d skipped" % (passed, len(failed), len(skipped)))
    for n in failed:
        print("  FAILED: " + n)
    for n in skipped:
        print("  SKIPPED: " + n)
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
