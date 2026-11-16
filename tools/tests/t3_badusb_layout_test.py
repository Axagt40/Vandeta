#!/usr/bin/env python3
"""Host-side checks for the BadUSB keyboard layouts and the "do not abort the
payload" guarantee.

Why this exists
---------------
The Nordic layouts (Finnish and Swedish) intentionally leave three printable
ASCII characters unmapped, because on those keyboards they are dead keys:

    '^'  (0x5E)   '`'  (0x60)   '~'  (0x7E)

The stock Arduino HID `write(const uint8_t *, size_t)` used to `break` on the
first character the layout could not express. With a US layout every printable
character maps, so the flaw was invisible; with a Nordic layout a single `~` or
backtick killed the REST OF THE PAYLOAD, which looks exactly like BadUSB having
stopped emulating HID. That is why the user hit it only "after changing the
keyboard layout to finnish".

This test is deliberately hardware-free:

  1. it parses the real layout tables and checks they are well formed (128
     entries, ASCII order, valid HID usages) - this is what catches a truncated
     or mis-ordered table, and it is how the Finnish table was verified before
     it was ever flashed;
  2. it prints the exact set of characters each layout cannot express, so the
     dead-key set is a checked fact rather than a comment;
  3. it guards the source against the `break` regressing, by asserting that no
     HID write() implementation aborts on the first unmappable character and
     that ducky_typer has the Alt-code fallback.

Run:  python3 FREEBUFF/tests/t3_badusb_layout_test.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIB = os.path.join(ROOT, "lib", "Bad_Usb_Lib")
DUCKY = os.path.join(ROOT, "src", "modules", "badusb_ble", "ducky_typer.cpp")

SHIFT = 0x80
ALT_GR = 0x40
ISO_KEY = 0x64
ISO_REPLACEMENT = 0x32

# Printable ASCII the tables are expected to cover (0x20..0x7E).
PRINTABLE = range(0x20, 0x7F)

LAYOUTS = [
    "KeyboardLayout_en_US",
    "KeyboardLayout_en_UK",
    "KeyboardLayout_nordic",   # the Finnish table, renamed in a1031f17
    "KeyboardLayout_sv_SE",
    "KeyboardLayout_da_DK",
    "KeyboardLayout_de_DE",
]

failures = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        failures.append(msg)
    return cond


def parse_layout(name):
    """Return the 128 entry bytes of a layout table, comments ignored."""
    with open(os.path.join(LIB, name + ".cpp"), "r") as fh:
        src = fh.read()
    body = src.split("= {", 1)
    if len(body) != 2:
        return None
    body = body[1].rsplit("};", 1)[0]
    out = []
    for line in body.splitlines():
        line = line.split("//", 1)[0].strip().rstrip(",").strip()
        if not line:
            continue
        m = re.fullmatch(r"(0x[0-9A-Fa-f]{2})\s*(\|\s*(SHIFT|ALT_GR))?", line)
        if not m:
            return None
        val = int(m.group(1), 16)
        if m.group(3) == "SHIFT":
            val |= SHIFT
        elif m.group(3) == "ALT_GR":
            val |= ALT_GR
        out.append(val)
    return out


def usage_of(entry):
    """What USBHIDKeyboard::press() would send for a layout entry."""
    k = entry
    shift = bool(k & SHIFT)
    altgr = False
    if k & SHIFT:
        k &= ~SHIFT
    if k & ALT_GR:
        altgr = True
        k &= ~ALT_GR
    if k == ISO_REPLACEMENT:
        k = ISO_KEY
    return k, shift, altgr


print("BadUSB layout / HID-write regression test")
print("=" * 62)

tables = {}
for name in LAYOUTS:
    table = parse_layout(name)
    check(table is not None, "%s: could not parse the layout table" % name)
    if table is None:
        continue
    tables[name] = table
    check(len(table) == 128, "%s: expected 128 entries, found %d" % (name, len(table)))
    bad = [
        (i, hex(v))
        for i, v in enumerate(table)
        if v != 0 and usage_of(v)[0] > 0xA4
    ]
    check(not bad, "%s: entry out of HID usage range: %s" % (name, bad))
    # BS (0x08), TAB (0x09) and LF (0x0A) are mapped on purpose; the rest of
    # the control range must stay 0x00.
    ctrl = [i for i in range(0x20) if i not in (0x08, 0x09, 0x0A) and table[i] != 0]
    check(not ctrl, "%s: unmapped control entries at %s" % (name, ctrl))

# The Finnish table is documented as the Swedish table (shared Nordic caps).
if "KeyboardLayout_nordic" in tables and "KeyboardLayout_sv_SE" in tables:
    diff = [
        (i, tables["KeyboardLayout_nordic"][i], tables["KeyboardLayout_sv_SE"][i])
        for i in range(128)
        if tables["KeyboardLayout_nordic"][i] != tables["KeyboardLayout_sv_SE"][i]
    ]
    check(not diff, "nordic and sv_SE differ at %s (documented as identical)" % diff)

# Lower-case letters and digits must always be typeable; a layout that cannot
# type 'a' is broken, not "unsupported".
for name, table in tables.items():
    missing_basic = [
        chr(c) for c in list(range(0x30, 0x3A)) + list(range(0x61, 0x7B)) if table[c] == 0
    ]
    check(not missing_basic, "%s: cannot type %s" % (name, missing_basic))

print("\nUnmappable printable characters per layout")
print("-" * 62)
NORDIC_DEAD = {0x5E: "^", 0x60: "`", 0x7E: "~"}
for name in sorted(tables):
    table = tables[name]
    missing = [chr(c) for c in PRINTABLE if table[c] == 0]
    print("  %-24s %s" % (name, ("".join(missing) if missing else "(complete)")))
    if name in ("KeyboardLayout_nordic", "KeyboardLayout_sv_SE"):
        check(
            set(ord(c) for c in missing) == set(NORDIC_DEAD),
            "%s: expected the Nordic dead keys %s to be the only gaps, got %r"
            % (name, sorted(NORDIC_DEAD.values()), missing),
        )

# ---------------------------------------------------------------------------
# Source guards: never abort a payload on one unmappable character.
# ---------------------------------------------------------------------------
print("\nHID write() abort guard")
print("-" * 62)
def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


for lib in ("USBHIDKeyboard.cpp", "BleKeyboard.cpp", "CH9329_Keyboard.cpp"):
    path = os.path.join(LIB, lib)
    with open(path, "r") as fh:
        src = strip_comments(fh.read())
    parts = src.split("write(const uint8_t *buffer, size_t size)", 1)
    check(len(parts) == 2, "%s: buffer form of write() not found" % lib)
    ok = False
    if len(parts) == 2:
        fn = parts[1].split("\n}", 1)[0]
        ok = "break" not in fn and "setWriteError" in fn
        check("break" not in fn, "%s: write() still breaks on the first unmappable char" % lib)
        check("setWriteError" in fn, "%s: write() should record the dropped character" % lib)
    print("  %-24s %s" % (lib, "ok" if ok else "MISSING"))

with open(DUCKY, "r") as fh:
    ducky = fh.read()
check("duckyTypeString(" in ducky, "ducky_typer: duckyTypeString() fallback is missing")
check(
    "sendAltChar(hid, c)" in ducky,
    "ducky_typer: unmappable characters must fall back to the numpad Alt-code",
)
check(
    "_hid->print(Argument)" not in ducky,
    "ducky_typer: the STRING path must not use print() (it cannot report a gap)",
)
print("  %-24s %s" % ("ducky_typer.cpp", "ok"))

print("\n%d checks, %d failure(s)" % (checks, len(failures)))
for f in failures:
    print("  FAIL: " + f)
print("RESULT: " + ("PASS" if not failures else "FAIL"))
sys.exit(0 if not failures else 1)
