#!/usr/bin/env python3
"""Task 1, phase 3: the IRAM A/B code overlay (`patch apply|revert|clear|journal`).

The overlay is exercised with a real code payload, not a mock:

    .text
        entry a1, 32      ; 3 bytes: windowed-ABI prologue (callx8 rotates a0)
        movi a2, 1234     ; 3 bytes: the replacement return value
        retw.n            ; 2 bytes: windowed return, not a bare `ret`

assembled with the vendored xtensa-esp32s3-elf-as, i.e. exactly the blob a
future tools/vndpatch.py will produce. The first board run crashed because the
hand-written payload used `ret`; the thunk calls through callx8, so the payload
must enter and leave the windowed ABI like any C function. Applying it to the `rf_scan_step` slot
replaces the function the sweep calls to decide how long to park on a carrier,
so the probe returns 1234 instead of the `rf.park_ms` tunable and the sweep
still runs. Reverting puts the body back.

Checks, in spec order:

  1. with Developer Mode OFF: `patch` refuses, `help` omits it;
  2. `patch list` shows the 8 slots, the build ID and the overlay/gate state;
  3. a valid payload applies with no reboot and the live consumer changes;
  4. revert restores the original body;
  5. a bad magic/version/slot/length/crc/build ID is refused with a reason;
  6. `patch journal` records the applied slot and `patch clear` empties it;
  7. two boots that end with patches applied produce `patches skipped after
     fault` on the second one;
  8. `devmode off` reverts everything, frees the overlay, and `patch list`
     returns to the refusal.

The gate refusal (`patch apply` while a radio loop runs) is not testable from
one serial port: the console command that starts a sweep blocks the console
until it ends. It is reported as SKIP rather than pretended.

    python3 FREEBUFF/tests/t45_patch_overlay.py [port]
"""
import os
import re
import struct
import subprocess
import sys
import time
import zlib

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200
ESPTOOL = os.path.expanduser("~/.platformio/penv/bin/esptool")

REFUSAL = "ERR: developer mode off"
# movi a2, 1234 ; ret
CODE = bytes.fromhex("36410022a4d21df0")

RESULTS = []


def record(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("  %-58s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))


def skip(name, detail=""):
    print("  %-58s SKIP%s" % (name, ("  " + detail) if detail else ""))


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


def console(s, cmd, wait=1.0, collect=0.7):
    # Drain first: a slow boot banner can otherwise land inside this capture and
    # make a gated line look like it was printed by `help`.
    s.reset_input_buffer()
    send(s, cmd, wait)
    return strings(grab(s, collect))


def console_long(s, cmd, seconds):
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


def payload_header(build_id, slot, code):
    return (
        b"VNDP"
        + bytes([1])
        + build_id
        + bytes([slot])
        + struct.pack("<H", len(code))
        + struct.pack("<I", zlib.crc32(code) & 0xFFFFFFFF)
    )


def parse_build(lines):
    line = find_line(lines, "build ")
    hexid = line[6:].strip() if line else ""
    if len(hexid) != 16:
        return None
    return bytes.fromhex(hexid)


def main():
    hard_reset()
    s = open_port()
    try:
        print("\n=== 1. gating: Developer Mode OFF ===")
        console(s, "usbdebug off", 2.5)
        out = console(s, "patch list", 1.2)
        record("patch answers exactly the refusal", REFUSAL in "\n".join(out), find_line(out, "ERR"))
        body = "\n".join(console(s, "help", 2.0))
        # "livepatch apply" contains the substring; match the help line itself.
        record("help omits the overlay verbs with dev mode off", "  patch apply" not in body)

        print("\n=== 2. Developer Mode ON: slots, build ID, overlay ===")
        console(s, "usbdebug on", 2.5)
        out = console(s, "tune reset rf.park_ms", 1.0)
        listing = console(s, "patch list", 1.8)
        for t in listing:
            if t.startswith(("build ", "overlay ", "rf_")) or " slot " in t:
                print("    " + t)
        build_id = parse_build(listing)
        record("patch list reports a 16-hex build ID", build_id is not None)
        slots = [t for t in listing if re.match(r"^\w+ slot \d+ (body|patched)$", t)]
        record("patch list shows 8 slots, all at body", len(slots) == 8 and all(t.endswith(" body") for t in slots),
               "found=%d" % len(slots))
        overlay = find_line(listing, "overlay ")
        record("overlay is mapped from the vndpatch partition",
               "B flash mmap @" in overlay and "vndpatch @0x500000" in overlay, overlay)
        record("gate is idle", overlay.endswith("gate 0"), overlay)
        record("help lists the overlay verbs with dev mode on",
               "patch apply" in "\n".join(console(s, "help", 2.0)))

        original = find_line(console(s, "tune get rf.park_ms", 0.9), "rf.park_ms = ")
        m = re.search(r"= (\d+)", original)
        original_ms = m.group(1) if m else "?"
        out = console(s, "patch probe rf_scan_step", 1.0)
        record("probe returns the tunable before patching",
               find_line(out, "probe rf_scan_step = ") == "probe rf_scan_step = " + original_ms, original)

        print("\n=== 3. apply a real code payload, no reboot ===")
        # payload_header() is the 20-byte header; the code follows it.
        blob = payload_header(build_id, 2, CODE) + CODE
        # The long `help` above can still be draining; a command sent into that
        # backlog is fine, but its answer then arrives after a short capture.
        time.sleep(1.5)
        s.reset_input_buffer()
        ret = console(s, "patch apply rf_scan_step " + blob.hex(), 4.0)
        line = find_line(ret, "applied ")
        record("apply accepts a valid VNDP payload",
               line.startswith("applied rf_scan_step slot 2 len 8"), line)
        out = console(s, "patch probe rf_scan_step", 1.0)
        record("the live consumer now returns the patched value",
               find_line(out, "probe rf_scan_step = 1234") == "probe rf_scan_step = 1234", find_line(out, "probe "))

        # The sweep really runs through the patched cell; on a quiet bench the
        # observable is that it completed with the radio intact.
        out = console_long(s, "subghz scan 433000000 434000000", 13.0)
        record("the patched sweep runs and completes",
               any(t.startswith("Scan complete") for t in out) and
               "variant=" in find_line(out, "[RFSCAN] band sweep"), find_line(out, "Scan complete"))
        listing = console(s, "patch list", 1.6)
        record("patch list marks rf_scan_step patched",
               "rf_scan_step slot 2 patched" in "\n".join(listing))

        print("\n=== 4. revert restores the body ===")
        out = console(s, "patch revert rf_scan_step", 1.2)
        record("revert reports the slot", "reverted rf_scan_step" in "\n".join(out), find_line(out, "revert"))
        out = console(s, "patch probe rf_scan_step", 1.0)
        record("the original value is back",
               find_line(out, "probe rf_scan_step = " + original_ms) != "", find_line(out, "probe "))
        record("patch list marks it at body again",
               "rf_scan_step slot 2 body" in "\n".join(console(s, "patch list", 1.4)))

        print("\n=== 5. malformed payloads are refused with a reason ===")
        cases = [
            ("wrong magic", b"XNDP" + blob[4:], "bad header"),
            ("wrong version", blob[:4] + bytes([9]) + blob[5:], "bad header"),
            ("wrong slot id", blob[:13] + bytes([5]) + blob[14:], "payload names a different slot"),
            ("bad crc", blob[:16] + b"\x00\x00\x00\x00" + blob[20:], "crc32 mismatch"),
            ("short blob", blob[:10], "bad header"),
            ("zero length", blob[:14] + b"\x00\x00" + blob[16:], "bad header"),
        ]
        wrong_build = bytearray(blob)
        wrong_build[5] ^= 0xFF
        cases.append(("wrong build ID", bytes(wrong_build), "build ID does not match"))
        for name, bad, reason in cases:
            out = console(s, "patch apply rf_scan_step " + bad.hex(), 1.2)
            record("refuses " + name, reason in "\n".join(out), find_line(out, "ERR"))
        out = console(s, "patch apply rf_scan_step", 1.0)
        record("refuses a missing payload", "patch apply <slot> <file|hex>" in "\n".join(out))
        # A 4 KB flash sector minus the 20-byte header is the phase-6 limit;
        # the length is checked before the code is read, so a short blob with a
        # huge declared length is enough to exercise it.
        too_long = bytearray(payload_header(build_id, 2, b"\x00") + b"\x00")
        too_long[14:16] = struct.pack("<H", 4080)
        out = console(s, "patch apply rf_scan_step " + bytes(too_long).hex(), 1.2)
        record("refuses code larger than a 4 KB slot", "bad header" in "\n".join(out),
               find_line(out, "ERR"))
        out = console(s, "patch apply nope " + blob.hex(), 1.0)
        record("refuses an unknown slot", "unknown slot" in "\n".join(out), find_line(out, "ERR"))
        # A refusal must not have applied anything.
        record("the refusals left the slot at body",
               "rf_scan_step slot 2 body" in "\n".join(console(s, "patch list", 1.4)))

        print("\n=== 6. journal ===")
        console(s, "patch apply rf_scan_step " + blob.hex(), 3.0)
        out = console(s, "patch journal", 2.0)
        record("journal records the applied slot and crc",
               "entry slot 2 rf_scan_step crc" in "\n".join(out), find_line(out, "entry slot"))
        out = console(s, "patch clear", 1.2)
        record("clear reverts and empties the journal", "patches cleared (1 reverted)" in "\n".join(out))
        out = console(s, "patch journal", 1.2)
        body = "\n".join(out)
        record("journal is empty after clear", "pending 0" in body and "entry slot" not in body,
               find_line(out, "journal"))

        print("\n=== 7. two aborted boots skip the journal ===")
        console(s, "patch apply rf_scan_step " + blob.hex(), 3.0)

        def reboot_and_capture():
            s.close()
            hard_reset()
            time.sleep(0.5)
            ns = serial.Serial(PORT, BAUD, timeout=0.2)
            ns.dtr = True
            ns.rts = False
            lines = strings(grab(ns, 11))
            return ns, lines

        # The banner goes out over USB-CDC before the host has reopened the
        # port, so it is not a reliable observable here. The journal is: a boot
        # that finds `pending` bumps `aborts`, and the second one clears it.
        s, boot1 = reboot_and_capture()
        body1 = "\n".join(console(s, "patch journal", 1.2))
        record("first aborted boot is counted, not skipped",
               "aborts 1 pending 0" in body1, find_line(body1.splitlines(), "journal"))
        console(s, "patch apply rf_scan_step " + blob.hex(), 3.0)
        s, boot2 = reboot_and_capture()
        out = console(s, "patch journal", 1.2)
        body = "\n".join(out)
        # The banner itself ends before the host reopens the port, so the
        # cleared journal is the observable that the fault rule fired.
        record("second aborted boot skips the journal instead of trusting it",
               "aborts 0 pending 0" in body and "entry slot" not in body,
               find_line(out, "journal"))

        print("\n=== 8. devmode off releases everything ===")
        console(s, "patch apply rf_scan_step " + blob.hex(), 3.0)
        out = console(s, "usbdebug off", 3.0)
        record("turning dev mode off reverts first", "patches reverted (1)" in "\n".join(out), find_line(out, "revert"))
        out = console(s, "patch list", 1.2)
        record("patch list refuses after off", REFUSAL in "\n".join(out))
        console(s, "usbdebug on", 2.5)
        listing = console(s, "patch list", 1.6)
        record("the overlay is mapped again on the next `on`",
               "B flash mmap @" in find_line(listing, "overlay "), find_line(listing, "overlay "))
        record("no slot is left patched after the off/on cycle",
               "rf_scan_step slot 2 body" in "\n".join(listing))
        console(s, "patch clear", 2.0)
        console(s, "usbdebug off", 3.0)

        print("\n=== 9. gate (not observable from one port) ===")
        skip("apply refuses while a radio loop runs", "needs a second console / UI-driven sweep")
    finally:
        try:
            s.close()
        except Exception:
            pass

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
