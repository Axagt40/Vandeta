#!/usr/bin/env python3
"""Task 1, phase 1: the RAM tunables table (`tune`), over USB only.

Checks, in the order the spec lists them:

  1. with Developer Mode OFF every verb answers exactly `ERR: developer mode off`
     and `help` does not mention `tune` at all;
  2. with it ON, `tune list` prints the whole set and `tune get` reads one back;
  3. `tune set` clamps into [min,max] and says so, rejects unknown names and a
     missing value;
  4. `tune reset [name]` returns one entry / the whole set to its default;
  5. `tune save` + `tune load` round-trip through bruce.conf;
  6. a tuning set with dev mode on still applies with it off - only the verbs are
     gated, which is the point of a tuning (checked by toggling the switch, not by
     trusting the code path);
  7. a BACKED entry (`ward.min_pulses`) is the same value the RF > Wardriver menu
     and `settings` already use, not a second copy.

Reboot persistence and the corrupt-config fallback need a power cycle, so they are
`t1_reboot.py`'s job (see the phase 1 report) rather than silently claimed here.

    python3 FREEBUFF/tests/t44_tune.py [port]
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
    print("  %-52s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))


def hard_reset():
    """Power-on-equivalent reset so the run starts at the main menu."""
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


def console(s, cmd, wait=1.0):
    send(s, cmd, wait)
    return strings(grab(s, 0.7))


def find_line(lines, prefix):
    for t in reversed(lines):
        if t.startswith(prefix):
            return t
    return ""


def field(line, key):
    m = re.search(r"\b%s=(\S+)" % re.escape(key), line)
    return m.group(1) if m else None


ENTRY = re.compile(r"^([a-z0-9]+\.[a-z0-9_]+) = (-?\d+) \(")
VALUE = re.compile(r"^([a-z0-9]+\.[a-z0-9_]+) = (-?\d+)")


def entries(lines):
    found = []
    for t in lines:
        m = ENTRY.match(t)
        if m:
            found.append((m.group(1), int(m.group(2))))
    return found


def value_of(lines, name):
    for t in lines:
        m = VALUE.match(t)
        if m and m.group(1) == name:
            return int(m.group(2))
    return None


REFUSAL = "ERR: developer mode off"


def main():
    hard_reset()
    s = open_port()
    try:
        print("\n=== 1. gating: Developer Mode OFF ===")
        console(s, "usbdebug off", 2.5)
        out = console(s, "usbdebug status", 1.5)
        record("Developer Mode reported OFF", any("Developer Mode: OFF" in t for t in out), find_line(out, "Developer Mode:"))

        refused = True
        for cmd in ("tune list", "tune get rf.park_ms", "tune set rf.park_ms 300", "tune reset rf.park_ms",
                    "tune save", "tune load"):
            out = console(s, cmd, 1.0)
            if REFUSAL not in "\n".join(out):
                refused = False
                print("  ! %-22s -> %r" % (cmd, out[-3:]))
        record("every tune verb answers ERR: developer mode off", refused)

        out = console(s, "help", 2.0)
        body = "\n".join(out)
        record("help does not list tune with dev mode off", "tune list" not in body)
        record("help still lists the stock verbs", "settings" in body and "mitm" not in body.split("\n")[0])

        print("\n=== 2. Developer Mode ON: the table ===")
        console(s, "usbdebug on", 2.5)
        # The board keeps whatever `tuning` object bruce.conf last saved (that is
        # the feature), so the defaults can only be read after a reset.
        console(s, "tune reset", 1.5)
        lines = console(s, "tune list", 1.6)
        found = entries(lines)
        print("  entries:", len(found))
        for name, val in found:
            print("    %-26s %s" % (name, val))
        wanted = ["rf.noise_floor", "rf.park_ms", "rf.scan_dwell_ms", "rf.spi_hold_us",
                  "ward.min_pulses", "ward.dup_window_ms", "ward.mode",
                  "ward.hop_ms_slow", "ward.hop_ms_fast", "ward.wide_cap_ms",
                  "ward.wide_squelch_dbm",
                  "nrf.cap_ms", "nrf.repeat", "ir.sweep_delay_ms", "ir.lock_replays",
                  "mitm.fallback_delay_ms", "mitm.arp_restore_on_stop",
                  "portal.hold_ms", "scan.chan_dwell_ms"]
        names = [n for n, _ in found]
        missing = [n for n in wanted if n not in names]
        record("all 19 listed entries are present", not missing, "missing=%r" % missing if missing else "")
        record("defaults match the table", dict(found).get("rf.park_ms") == 15000 and dict(found).get("portal.hold_ms") == 600000
               and dict(found).get("scan.chan_dwell_ms") == 130 and dict(found).get("ir.lock_replays") == 10
               and dict(found).get("ward.mode") == 0 and dict(found).get("ward.hop_ms_slow") == 400
               and dict(found).get("ward.hop_ms_fast") == 120 and dict(found).get("ward.wide_cap_ms") == 2000,
               "park=%s portal=%s scan=%s ir=%s ward=%s/%s/%s/%s" % (dict(found).get("rf.park_ms"), dict(found).get("portal.hold_ms"),
                                                   dict(found).get("scan.chan_dwell_ms"), dict(found).get("ir.lock_replays"),
                                                   dict(found).get("ward.mode"), dict(found).get("ward.hop_ms_slow"),
                                                   dict(found).get("ward.hop_ms_fast"), dict(found).get("ward.wide_cap_ms")))
        # Phase 1b is finished: every entry now has a consumer, so nothing may
        # advertise itself as "stored only" any more.
        record("no entry is stored only (all 19 wired)", not any("stored only" in t for t in lines))
        record("backed entries name their own conf key", any("own conf key" in t for t in lines))

        out = console(s, "tune get rf.park_ms", 1.0)
        record("tune get reads one entry", value_of(out, "rf.park_ms") == 15000, find_line(out, "rf.park_ms"))
        out = console(s, "tune get nope.nope", 1.0)
        record("tune get rejects an unknown name", "ERR: unknown tunable" in "\n".join(out), find_line(out, "ERR"))

        out = console(s, "help", 2.0)
        record("help lists tune with dev mode on", "tune list" in "\n".join(out))

        print("\n=== 3. set / clamp / reject ===")
        out = console(s, "tune set rf.park_ms 420", 1.0)
        record("set takes effect and reads back", value_of(out, "rf.park_ms") == 420, find_line(out, "rf.park_ms"))
        out = console(s, "tune get rf.park_ms", 1.0)
        record("the new value is the one stored", value_of(out, "rf.park_ms") == 420, find_line(out, "rf.park_ms"))

        out = console(s, "tune set rf.park_ms 999999", 1.0)
        line = find_line(out, "rf.park_ms")
        record("an over-range value clamps to max and says so", value_of(out, "rf.park_ms") == 60000 and "clamped" in line, line)
        out = console(s, "tune set nrf.cap_ms 5", 1.0)
        record(
            "an under-range value clamps to min",
            value_of(out, "nrf.cap_ms") == 100 and "clamped" in find_line(out, "nrf.cap_ms"),
            find_line(out, "nrf.cap_ms"),
        )
        # A negative number has to be quoted: this CLI reads a leading '-' as an
        # option flag, so `tune set rf.park_ms -5` is refused as an unknown
        # argument. That matters for the signed entries (rf.noise_floor is the
        # one in this set), so the quoted form is what gets tested.
        out = console(s, 'tune set rf.noise_floor "-95"', 1.0)
        record("a quoted negative value is accepted", value_of(out, "rf.noise_floor") == -95, find_line(out, "rf.noise_floor"))
        console(s, "tune reset rf.noise_floor", 1.0)

        out = console(s, "tune set nope.nope 1", 1.0)
        record("set rejects an unknown name", "ERR: unknown tunable" in "\n".join(out))
        out = console(s, "tune set rf.park_ms", 1.0)
        record("set without a value is refused", "ERR: tune set" in "\n".join(out))

        print("\n=== 4. reset ===")
        out = console(s, "tune reset rf.park_ms", 1.0)
        record("reset returns one entry to its default", value_of(out, "rf.park_ms") == 15000 and "default" in find_line(out, "rf.park_ms"),
               find_line(out, "rf.park_ms"))
        console(s, "tune set scan.chan_dwell_ms 500", 1.0)
        out = console(s, "tune reset", 1.2)
        record("reset with no name resets the whole set", "tuning reset to defaults" in "\n".join(out), find_line(out, "tuning"))

        print("\n=== 5. save / load round-trip ===")
        console(s, "tune set rf.park_ms 777", 1.0)
        out = console(s, "tune save", 2.5)
        record("save reports the stored entry count", "tuning saved" in "\n".join(out), find_line(out, "tuning"))
        console(s, "tune reset rf.park_ms", 1.0)
        out = console(s, "tune load", 3.0)
        record("load reports the reloaded entries", "tuning reloaded" in "\n".join(out), find_line(out, "tuning"))
        out = console(s, "tune get rf.park_ms", 1.0)
        record("the saved value came back after a reset", value_of(out, "rf.park_ms") == 777, find_line(out, "rf.park_ms"))

        print("\n=== 6. values apply with dev mode off ===")
        console(s, "usbdebug off", 2.5)
        out = console(s, "tune get rf.park_ms", 1.0)
        record("the verb is gated again", REFUSAL in "\n".join(out))
        # The value is not gated: `settings` dumps the whole config a stock build
        # would load, so the `tuning` object has to be in it.
        out = console(s, "settings", 3.0)
        body = "\n".join(out)
        record("bruce.conf carries the tuning object while dev mode is off", '"tuning"' in body and '"rf.park_ms": 777' in body)
        console(s, "usbdebug on", 2.5)
        out = console(s, "tune get rf.park_ms", 1.0)
        record("and the value survived the toggle", value_of(out, "rf.park_ms") == 777, find_line(out, "rf.park_ms"))

        print("\n=== 7. a BACKED entry is the same value, not a copy ===")
        out = console(s, "tune set ward.min_pulses 24", 1.0)
        record("ward.min_pulses set through tune", value_of(out, "ward.min_pulses") == 24, find_line(out, "ward.min_pulses"))
        out = console(s, "settings rfWardriverMinPulses", 1.5)
        line = find_line(out, "rfWardriverMinPulses")
        record("settings sees the same 24 (one source)", line.endswith("24"), line)

        # Same check for the entry whose old conf key (rfWardriverDedupeSec) was
        # retired: the ms key must be the one the table writes and settings reads.
        out = console(s, "tune set ward.dup_window_ms 45000", 1.0)
        record("ward.dup_window_ms set through tune", value_of(out, "ward.dup_window_ms") == 45000, find_line(out, "ward.dup_window_ms"))
        out = console(s, "settings rfWardriverDedupeMs", 1.5)
        line = find_line(out, "rfWardriverDedupeMs")
        record("settings sees the same 45000 ms (old seconds key retired)", line.endswith("45000"), line)
        out = console(s, "settings rfWardriverDedupeSec", 1.5)
        record("the retired seconds key is refused by settings",
               "Invalid field name: rfWardriverDedupeSec" in "\n".join(out), find_line(out, "Invalid field"))
        console(s, "tune reset ward.dup_window_ms", 1.0)

        out = console(s, "tune set ward.min_pulses 16", 1.0)
        out = console(s, "tune set rf.park_ms 250", 1.0)
        console(s, "tune save", 2.5)

        out = console(s, "mem", 1.5)
        for t in out[:6]:
            print("  " + t)

        out = console(s, "errors", 1.5)
        crashes = [t for t in out if re.search(r"Guru|panic|abort|Backtrace|assert", t, re.I)]
        record("no crash markers in the error ring", len(crashes) == 0)
    finally:
        s.close()

    return finish()


def finish():
    print("\n===== SUMMARY =====")
    passed = sum(1 for _, ok, _ in RESULTS if ok is True)
    failed = [n for n, ok, _ in RESULTS if ok is False]
    print("  %d passed, %d failed" % (passed, len(failed)))
    for n in failed:
        print("  FAILED: " + n)
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
