#!/usr/bin/env python3
"""IR Suite 1.1 - every remote type, run on the board over USB.

The suite is no longer a TV tool: IR > IR Suite > Bruteforce remotes offers the
same six types IR > Universal Remote sweeps (TV, AC, Fans, Audio, Projectors,
LEDs), and inside a type the brand / remote / family sweeps. This test loads the
embedded script into the interpreter with its blocking `mainMenu()` removed and
drives the real functions, so what is checked is what the device will run:

  * the six types are found and each has the right trigger button
    (TV/Fans/Audio/Projectors Power, AC Off, LEDs Power_on),
  * each library groups into usable families (protocol + address),
  * a non-TV remote is rebuilt from its own library and really transmits,
  * a TV remote still prefers the merged family file (full button set),
  * ac.ir - one protocol + address for the whole file - is swept code by code
    instead of collapsing to a single send.

Nothing stays on the card: the harness deletes every file it writes.

    python3 tools/tests/t41_ir_suite_types.py [port]
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SUITE = os.path.join(ROOT, "src", "modules", "ir", "ir_suite_src", "Vandeta_IR_Suite.js")
TOOL = os.path.join(ROOT, "FREEBUFF", "tools", "usb_debug.py")
PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"

passes = 0
fails = 0

HARNESS = r"""
// ---- t41: the type routing, on the board ----
var pass4 = 0, fail4 = 0;
function c4(ok, what) {
    if (ok) { pass4++; console.log("PASS " + what); }
    else { fail4++; console.log("FAIL " + what); }
}

// The wheel must skip, never lock. On the board a detent also raises
// AnyKeyPress (InputHandler() sets it whenever the encoder moves), so a window
// that reads the key flags before the rotary counter is the bug that made a
// roll replay the last ten codes and the next roll save one of them.
var canStubWheel = false;
try {
    var rotOrig = keyboard.getRotarySteps;
    keyboard.getRotarySteps = function () { return 0; };
    canStubWheel = (keyboard.getRotarySteps !== rotOrig);
    keyboard.getRotarySteps = rotOrig;
} catch (errW) {
    canStubWheel = false;
}
if (!canStubWheel) {
    console.log("SKIP wheel checks: keyboard.getRotarySteps is not writable");
} else {
    var anyOrig = keyboard.getAnyPress;
    var selOrig = keyboard.getSelPress;
    var rotKeep = keyboard.getRotarySteps;
    keyboard.getAnyPress = function () { return true; }; // what a detent leaves behind
    keyboard.getSelPress = function () { return false; };
    keyboard.getRotarySteps = function () { return -1; };
    var rw = awaitReaction(60);
    c4(rw == 2, "a wheel detent is a skip, not a confirmation (got " + rw + ")");
    c4(rollPending == -1, "the skip keeps the detent for the sweep (got " + rollPending + ")");
    rollPending = 0;
    keyboard.getRotarySteps = function () { return 0; };
    var rk = awaitReaction(60);
    c4(rk == 1, "a real key still confirms (got " + rk + ")");
    keyboard.getRotarySteps = rotKeep;
    keyboard.getAnyPress = anyOrig;
    keyboard.getSelPress = selOrig;
    rollPending = 0;
    // firmware convention: positive = previous/up, so the delta is negative
    rollDir = 0; rollMult = 1;
    c4(rollMove(1) < 0, "one detent up walks the candidate list up (" + rollMove(1) + ")");
    rollDir = 0; rollMult = 1;
    c4(rollMove(-1) > 0, "one detent down walks the candidate list down (" + rollMove(-1) + ")");
    rollDir = 0; rollMult = 1;
}

c4(CATS.length == 6, "six remote types like Universal Remote");
c4(MENU[0].label == "Bruteforce remotes", "menu row 0 is Bruteforce remotes (" + MENU[0].label + ")");
c4(MENU.length == 5, "menu has 5 rows (" + MENU.length + ")");

// every type has its library, and the trigger is the action a sweep can see
var WANT = ["Power", "Off", "Power", "Power", "Power", "Power_on"];
for (var i = 0; i < CATS.length; i++) {
    var cat = CATS[i];
    c4(libReady(cat), cat.label + " library found: " + libPath(cat));
    var trig = triggerOf(cat);
    c4(trig == WANT[i], cat.label + " trigger is " + WANT[i] + " (got '" + trig + "')");
    var total = triggerCount(cat, trig);
    // Grouping all 295 TV codes costs ~6.6 s on the board, which the USB runner
    // reads as a silent script; the first 60 are enough to prove the binding
    // walks the library (the full-library grouping is covered by the host
    // harness in tools/irsuite_host_check.js).
    var probe = total > 60 ? 60 : total;
    var fams = familyIndexes(cat, trig, probe);
    c4(total > 0 && fams.length > 0, cat.label + ": " + total + " " + trig + " codes, " + fams.length + " families in the first " + probe);
    c4(fams.length <= probe, cat.label + ": families do not exceed the codes read");
}

// ac.ir is one protocol + address for the whole file, so its family sweep has to
// fall back to the codes; familyIndexes must say so (1), which is what triggers
// the fallback in librarySweep().
var acTotal = triggerCount(CATS[1], "Off");
c4(familyIndexes(CATS[1], "Off", acTotal).length == 1,
   "ac.ir is a single family, so the family sweep falls back to " + acTotal + " codes");

// Saving needs dialog.info to return; stub it and write into /BruceIR.
var canStub = false;
try {
    var infoOrig4 = dialog.info;
    dialog.info = function (a, b) {};
    canStub = (dialog.info !== infoOrig4);
    dialog.info = infoOrig4;
} catch (err4) {
    canStub = false;
}
if (!canStub) {
    console.log("SKIP save checks: dialog.info is not writable");
} else {
    var infoStub = dialog.info;
    dialog.info = function (a, b) {};

    // a non-TV remote comes out of its own library
    saveLibraryCode(CATS[1], "Off", 0);
    var acPath = selectedRemote;
    c4(acPath.indexOf("/BruceIR/IRSuite_AC_") == 0, "an AC remote is saved as " + acPath);
    var acBody = storage.read({ fs: "sd", path: acPath });
    c4(acBody.indexOf("Filetype: Bruce IR File") == 0, "the AC remote has the recorded filetype");
    c4(acBody.indexOf("from /infrared/Universal/ac.ir") > 0, "the AC remote names its library");
    c4(acBody.indexOf("name: ") > 0, "the AC remote carries buttons");
    c4(ir.transmitButton(acPath, acBody.split("\nname: ")[1].split("\n")[0]),
       "the saved AC remote transmits a button");
    storage.remove({ fs: "sd", path: acPath });

    // a TV remote still prefers the merged family file
    saveLibraryCode(CATS[0], "Power", 0);
    var tvPath = selectedRemote;
    c4(tvPath.indexOf("/BruceIR/IRSuite_TV_") == 0, "a TV remote is saved as " + tvPath);
    var tvBody = storage.read({ fs: "sd", path: tvPath });
    var tvButtons = tvBody.split("\nname: ").length - 1;
    c4(tvBody.indexOf("Filetype: Bruce IR File") == 0, "the TV remote has the recorded filetype");
    c4(tvButtons > 5, "the TV remote is a full button set (" + tvButtons + " buttons)");
    storage.remove({ fs: "sd", path: tvPath });

    dialog.info = infoStub;
    selectedRemote = "";
    storage.write({ fs: "sd", path: CONFIG_FILE }, "", "write");
}
console.log("RESULT4 pass=" + pass4 + " fail=" + fail4);
"""


def check(ok, what, detail=""):
    global passes, fails
    if ok:
        passes += 1
        print("  PASS  " + what)
    else:
        fails += 1
        print("  FAIL  " + what + (("  [" + detail + "]") if detail else ""))


def main():
    if not os.path.exists(SUITE):
        print("missing suite: %s" % SUITE, file=sys.stderr)
        return 2
    if not os.path.exists(TOOL):
        print("missing tool: %s" % TOOL, file=sys.stderr)
        return 2

    src = open(SUITE, encoding="utf-8").read()
    if "mainMenu();" not in src:
        print("the suite no longer calls mainMenu()", file=sys.stderr)
        return 2
    trimmed = src.rsplit("mainMenu();", 1)[0] + HARNESS

    fd, path = tempfile.mkstemp(prefix="t41_irsuite_", suffix=".js")
    try:
        with os.fdopen(fd, "w") as fh:
            fh.write(trimmed)
        proc = subprocess.run(
            [sys.executable, TOOL, PORT, "run", path],
            capture_output=True,
            text=True,
            timeout=300,
        )
    finally:
        os.remove(path)
    out = (proc.stdout or "") + (proc.stderr or "")

    for line in out.replace("\r", "").splitlines():
        if line.startswith("PASS "):
            check(True, line[5:])
        elif line.startswith("FAIL "):
            check(False, line[5:])
    m = re.search(r"RESULT4 pass=(\d+) fail=(\d+)", out)
    check(m is not None, "the harness printed a result", out[-500:])
    if m:
        check(int(m.group(2)) == 0, "no harness failures (%s)" % m.group(0))

    print("\n%d passed, %d failed" % (passes, fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
