#!/usr/bin/env python3
"""Wardriver: identify and replay run on saved files, not on live capture.

Builds a real Princeton frame (the registry's `te`/sync/bit timings), writes it
as a wardriver-style raw .sub, uploads it to the card and then:

  subghz wardriver identify ward_test.sub   -> "identified Princeton"
  subghz wardriver identifyall              -> "identified 1/N"
  subghz wardriver replay ward_test.sub     -> "replayed"

The decoded copy (ward_test_dec.sub) is read back and checked for the
Protocol/Key fields readSubFile needs, then everything is deleted again.

    python3 tools/tests/t22_wardriver_identify.py [port]
"""
import os
import re
import subprocess
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200
BOOT_WAIT = 9.0
# tests live in <root>/tools/tests, the client in <root>/tools
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
USB_DEBUG = os.path.join(REPO_ROOT, "FREEBUFF", "tools", "usb_debug.py")
TMP = "/tmp/ward_test.sub"
FOLDER = "/bruce/subghz/wardriver"


def princeton_subcode(serial=0xA1B2C3, button=1, bits=24):
    """te=350 us, sync {1,31}, one {3,1}, zero {1,3}, 24 bits, LSB first."""
    te = 350
    code = (serial << 0) & 0xFFFFFF
    durs = [te, -31 * te]  # sync
    for i in range(bits):
        bit = (code >> i) & 1
        if bit:
            durs += [3 * te, -te]
        else:
            durs += [te, -3 * te]
    return durs


def repeated_frame(durs, gap=10850, times=3):
    """What a capture actually looks like: the same press repeated, the frames
    joined by the inter-frame gap (the wardriver ends a burst on 30 ms idle, so
    repeated presses land in one file)."""
    out = []
    for i in range(times):
        if i:
            out.append(-gap)
        out += durs
    return out


def write_sub(path, durs, freq_hz=433920000, protocol="RAW"):
    with open(path, "w") as f:
        f.write("Filetype: Bruce SubGhz File\n")
        f.write("Version 1\n")
        f.write("Frequency: %d\n" % freq_hz)
        f.write("Preset: 0\n")
        f.write("Protocol: %s\n" % protocol)
        body = " ".join(("%d" if d > 0 else "%d") % d for d in durs)
        f.write("RAW_Data: %s\n" % body)


def strings(data):
    out, seen = [], set()
    for m in re.findall(rb"[\x20-\x7e]{3,}", data):
        t = m.decode(errors="replace")
        if t not in seen:
            seen.add(t)
            out.append(t)
    return out


def main():
    durs = princeton_subcode()
    write_sub(TMP, durs)
    print("synthetic frame: %d transitions, %d us" % (len(durs), sum(abs(d) for d in durs)))

    long_durs = repeated_frame(durs)
    write_sub("/tmp/ward_long.sub", long_durs)
    print("repeated frame: %d transitions, %d us" % (len(long_durs), sum(abs(d) for d in long_durs)))

    for local, name in ((TMP, "ward_test.sub"), ("/tmp/ward_long.sub", "ward_long.sub")):
        up = subprocess.run(
            [sys.executable, USB_DEBUG, PORT, "put", local, FOLDER + "/" + name],
            capture_output=True,
            text=True,
            timeout=180,
        )
        print("upload %s:" % name, (up.stdout or up.stderr).strip().splitlines()[-1] if (up.stdout or up.stderr) else "?")
        if up.returncode != 0:
            return 1

    fails = []

    def check(name, ok, detail=""):
        print("  %-40s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))
        if not ok:
            fails.append(name)

    def open_console():
        # usb_debug.py resets the board when it opens the port, so the console
        # handle is opened and closed around every transfer, never shared.
        ser = serial.Serial(PORT, BAUD, timeout=0.2)
        ser.dtr = True
        ser.rts = False
        time.sleep(BOOT_WAIT)
        ser.reset_input_buffer()
        return ser

    s = open_console()

    def run(cmd, wait=2.0):
        s.write((cmd + "\n").encode())
        s.flush()
        time.sleep(wait)
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < 0.6:
            c = s.read(8192)
            if c:
                buf.extend(c)
        return " ".join(strings(bytes(buf)))

    text = run("subghz wardriver list", 1.0)
    check("capture is listed", "ward_test.sub" in text, text[-100:])

    text = run("subghz wardriver identify ward_test.sub", 3.0)
    check("identify decodes the saved file", "identified Princeton" in text, text[-140:])

    text = run("subghz wardriver identify ward_long.sub", 4.0)
    check("identify handles a repeated press", "identified Princeton" in text, text[-140:])

    text = run("subghz wardriver identifyall", 5.0)
    check("identifyall reports a count", re.search(r"identified \d+/\d+", text) is not None, text[-100:])

    # The console handle must be released first: usb_debug.py resets the board
    # on open, and two readers on the same port steal each other's bytes (the
    # replay below then talks to a board that already rebooted).
    s.close()
    if os.path.exists("/tmp/ward_test_dec.sub"):
        os.remove("/tmp/ward_test_dec.sub")
    down = subprocess.run(
        [sys.executable, USB_DEBUG, PORT, "get", FOLDER + "/ward_test_dec.sub", "/tmp/ward_test_dec.sub"],
        capture_output=True,
        text=True,
        timeout=180,
    )
    decoded = ""
    if down.returncode == 0 and os.path.exists("/tmp/ward_test_dec.sub"):
        decoded = open("/tmp/ward_test_dec.sub").read()
    check(
        "decoded copy is a valid .sub",
        "Protocol: Princeton" in decoded and "Key:" in decoded and "Bit: 24" in decoded,
        decoded.replace("\n", " | ")[:120],
    )

    s = open_console()
    text = run("subghz wardriver replay ward_test.sub", 3.0)
    check("replay transmits the capture", "replayed" in text, text[-100:])

    # A repeated press is ~150 ms of signal, well past the old fixed 2 s TX
    # wait once it is sent three times.
    text = run("subghz wardriver replay ward_long.sub", 8.0)
    check("replay transmits a repeated press", "replayed" in text, text[-100:])

    run("rm " + FOLDER + "/ward_test.sub", 1.0)
    run("rm " + FOLDER + "/ward_test_dec.sub", 1.0)
    run("rm " + FOLDER + "/ward_long.sub", 1.0)
    run("rm " + FOLDER + "/ward_long_dec.sub", 1.0)
    s.close()

    print("\n===== SUMMARY =====")
    if fails:
        print("  FAILED: " + ", ".join(fails))
        return 1
    print("  all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
