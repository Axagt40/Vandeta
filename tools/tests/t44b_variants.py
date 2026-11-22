#!/usr/bin/env python3
"""Task 1, phase 2: the variant tables (`patch`), over USB only.

Checks, in the order the spec lists them:

  1. with Developer Mode OFF, `patch` answers exactly `ERR: developer mode off`
     and `help` does not mention it;
  2. `patch list` shows the three tables, their current index and their variant
     names, and the defaults are the behaviour phase 1 shipped (index 2 =
     adaptive, 0 = fixed, 0 = single);
  3. every index of every table can be selected without a reboot, is stored in
     the phase 1 tunables entry (one value, not a second mechanism) and is named
     there too;
  4. a bad index, a non-numeric index, an unknown table and a missing index are
     refused with a reason instead of silently selecting something;
  5. a selection still applies with Developer Mode off - only the verbs are
     gated (checked on the live consumer, not by trusting the code path):
       * rf_scan   -> `subghz scan` prints `variant=<name>` and runs the sweep
       * ir_repeat -> `ir tx` prints the repeat count the variant produces
                      (adaptive_len on a 13-bit RC5 frame repeats more than the
                      same setting on 32-bit NEC; burst says `burst repeats`)
       * nrf_capture -> `nrfcap capture` reports the layout it ran
  6. the selections are cleaned back to their defaults at the end.

The RF sweep can only prove "the selected variant ran and the radio survived"
on a quiet bench: whether a carrier is parked on or reported first needs a real
signal in the band. That limitation is stated in the summary instead of being
claimed as measured.

    python3 tools/tests/t44b_variants.py [port]
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
    print("  %-56s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))


def skip(name, detail=""):
    print("  %-56s SKIP%s" % (name, ("  " + detail) if detail else ""))


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


def console(s, cmd, wait=1.0, collect=0.7):
    send(s, cmd, wait)
    return strings(grab(s, collect))


def console_long(s, cmd, seconds):
    """For a verb that runs a sweep: the budget is the wait."""
    s.write((cmd + "\r\n").encode())
    s.flush()
    return strings(grab(s, seconds))


def find_line(lines, prefix):
    for t in reversed(lines):
        if t.startswith(prefix):
            return t
    return ""


def field(line, key):
    m = re.search(r"\b%s=(\S+)" % re.escape(key), line)
    return m.group(1) if m else None


REFUSAL = "ERR: developer mode off"

# The spec's short name, the tunables entry that owns the index, its variants in
# index order, and the default index (the behaviour phase 1 shipped).
TABLES = [
    ("rf_scan", "rf.scan_variant", ["seq", "park", "adaptive"], 2),
    ("ir_repeat", "ir.repeat_variant", ["fixed", "adaptive_len", "burst"], 0),
    ("nrf_capture", "nrf.capture_variant", ["single", "hop", "locked"], 0),
]


def patch(s, name, index, wait=1.2):
    return console(s, "patch %s %d" % (name, index), wait)


def main():
    hard_reset()
    s = open_port()
    try:
        print("\n=== 1. gating: Developer Mode OFF ===")
        console(s, "usbdebug off", 2.5)
        out = console(s, "usbdebug status", 1.5)
        record("Developer Mode reported OFF", any("Developer Mode: OFF" in t for t in out))

        refused = True
        for cmd in ("patch list", "patch rf_scan 1", "patch", "patch ir_repeat 2"):
            out = console(s, cmd, 1.0)
            if REFUSAL not in "\n".join(out):
                refused = False
                print("  ! %-22s -> %r" % (cmd, out[-3:]))
        record("every patch verb answers ERR: developer mode off", refused)

        body = "\n".join(console(s, "help", 2.0))
        record("help does not list patch with dev mode off", "patch list" not in body)
        record("help still lists the stock verbs", "settings" in body)

        print("\n=== 2. Developer Mode ON: the tables ===")
        console(s, "usbdebug on", 2.5)
        # `patch` saves the selection, so a previous run's choice is persisted.
        # Reset the entries to their defaults before comparing against them.
        for _, entry, _, _ in TABLES:
            console(s, "tune reset " + entry, 0.8)
        out = console(s, "patch list", 1.6)
        lines = [t for t in out if re.match(r"^(rf_scan|ir_repeat|nrf_capture) = ", t)]
        for t in lines:
            print("    " + t)
        record("patch list shows the three tables", len(lines) == 3, "found=%d" % len(lines))
        for name, entry, variants, default in TABLES:
            line = find_line(out, name + " = ")
            ok = bool(line) and line.startswith("%s = %d %s [" % (name, default, variants[default])) and \
                "|".join(variants) in line
            record("%-12s lists index, name and variants" % name, ok, line)
        record("help lists patch with dev mode on", "patch list" in "\n".join(console(s, "help", 2.0)))

        print("\n=== 3. select every index, no reboot ===")
        for name, entry, variants, default in TABLES:
            for idx, variant in enumerate(variants):
                out = patch(s, name, idx)
                echo = find_line(out, "%s = " % name)
                ok = bool(echo) and echo.startswith("%s = %d %s" % (name, idx, variant)) and "saved" in echo
                record("%s -> %d %s" % (name, idx, variant), ok, echo)
                got = console(s, "tune get " + entry, 0.9)
                val = find_line(got, entry + " = ")
                record("  stored in %s, not a second copy" % entry,
                       bool(val) and val.startswith("%s = %d (%s)" % (entry, idx, variant)), val)
        # A variant entry must name its choices in `tune list` as well, or the
        # index is unreadable there.
        listing = console(s, "tune list", 1.8)
        record("tune list names the variants",
               all(any(("variants: " + "|".join(v)) in t for t in listing) for _, _, v, _ in TABLES))

        print("\n=== 4. refusals ===")
        out = console(s, "patch rf_scan 3", 1.0)
        record("an out-of-range index is refused", "ERR: index must be 0..2" in "\n".join(out), find_line(out, "ERR"))
        out = console(s, "patch rf_scan x", 1.0)
        record("a non-numeric index is refused", "ERR: index must be 0..2" in "\n".join(out), find_line(out, "ERR"))
        out = console(s, "patch nope 0", 1.0)
        record("an unknown table is refused", "ERR: unknown variant table" in "\n".join(out), find_line(out, "ERR"))
        out = console(s, "patch rf_scan", 1.0)
        record("a missing index is refused", "ERR: patch <name> <index>" in "\n".join(out), find_line(out, "ERR"))
        # The refusal must not have moved the selection.
        val = find_line(console(s, "tune get rf.scan_variant", 0.9), "rf.scan_variant = ")
        record("refusals left the selection alone", val.startswith("rf.scan_variant = 2"), val)

        print("\n=== 5. IR repeat variants (live consumer) ===")
        console(s, "settings irTxRepeats 3", 1.6)
        # `settings <name>` also dumps the whole config as pretty JSON, so the
        # key is matched inside the object as well as in the plain read line.
        body = "\n".join(console(s, "settings irTxRepeats", 1.2))
        m = re.search(r'"irTxRepeats":\s*(\d+)', body) or re.search(r"irTxRepeats = (\d+)", body)
        record("irTxRepeats = 3 for the comparison", bool(m) and m.group(1) == "3",
               m.group(0) if m else body[:60])
        # A read used to fall through to the pin setter with an empty value, so
        # `settings irTxRepeats` silently set it to 0. The send below would then
        # report no repeats at all, which is exactly how that bug was found.
        record("a settings read does not clear the value", "irTxRepeats = 3" in body, body[:60])

        patch(s, "ir_repeat", 0)
        out = console(s, "ir tx NEC 04000000 08000000", 1.6)
        line = find_line(out, "Sent NEC Command")
        record("fixed sends irTxRepeats frames", "(1 initial + 3 repeats)" in line, line)

        patch(s, "ir_repeat", 2)
        out = console(s, "ir tx NEC 04000000 08000000", 1.6)
        line = find_line(out, "Sent NEC Command")
        record("burst sends one native repeat train", "(1 initial + 3 burst repeats)" in line, line)

        patch(s, "ir_repeat", 1)
        # adaptive_len: ceil(3 * 32 / 13) = 8 on an RC5 frame, ceil(3*32/32)=3 on NEC.
        out = console(s, "ir tx RC5 04000000 08000000", 1.6)
        line = find_line(out, "Sent RC5 Command")
        record("adaptive_len scales with a 13-bit frame", "(1 initial + 8 repeats)" in line, line)
        out = console(s, "ir tx NEC 04000000 08000000", 1.6)
        line = find_line(out, "Sent NEC Command")
        record("adaptive_len keeps 3 on a 32-bit frame", "(1 initial + 3 repeats)" in line, line)
        console(s, "patch ir_repeat 0", 1.0)
        console(s, "settings irTxRepeats 0", 1.2)

        print("\n=== 6. nRF capture variants (live consumer) ===")
        for idx, variant in enumerate(["single", "hop", "locked"]):
            patch(s, "nrf_capture", idx)
            out = console(s, "nrfcap capture 400", 1.6)
            body = "\n".join(out)
            if "ERR NRF24 not connected" in body:
                skip("nrf_capture -> %s" % variant, "no nRF24 on this unit")
                continue
            line = find_line(out, "RESULT capture")
            record("nrf_capture -> %s runs and reports it" % variant,
                   field(line, "variant") == variant and field(line, "count") is not None, line)
        console(s, "patch nrf_capture 0", 1.0)

        print("\n=== 7. a selection applies with Developer Mode OFF ===")
        patch(s, "rf_scan", 0)
        console(s, "usbdebug off", 2.5)
        # `subghz scan` takes Hz and divides by 1e6 itself (433000000 -> 433 MHz).
        out = console_long(s, "subghz scan 433000000 434000000", 12.5)
        line = find_line(out, "[RFSCAN] band sweep")
        record("subghz scan runs the seq variant with dev mode off",
               "variant=seq" in line, line)
        record("the sweep completed (radio not wedged)", any(t.startswith("Scan complete") for t in out),
               find_line(out, "Scan complete"))

        print("\n=== 8. cleanup: back to the defaults ===")
        console(s, "usbdebug on", 2.5)
        for name, entry, variants, default in TABLES:
            out = patch(s, name, default)
            record("%s reset to %d %s" % (name, default, variants[default]),
                   ("= %d %s" % (default, variants[default])) in find_line(out, name + " = "))
        console(s, "usbdebug off", 2.5)
    finally:
        s.close()

    passed = sum(1 for _, ok, _ in RESULTS if ok)
    print("\n%d passed, %d failed" % (passed, len(RESULTS) - passed))
    if passed != len(RESULTS):
        print("\nFAILURES:")
        for name, ok, detail in RESULTS:
            if not ok:
                print("  - %s  %s" % (name, detail))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
