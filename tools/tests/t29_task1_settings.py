#!/usr/bin/env python3
"""Task 1 (2026-10-26) regression pass, driven over USB only.

Checks the parts of the task that need no keypad and no TV:

  * Config has a top-level HID Settings row and Advanced no longer has one,
  * the HID Settings page is still the same page (USB Footprint, no Payload
    Manager),
  * Developer Options has no Hot Reload row and keeps Developer Mode first,
  * switching developer mode on arms hot reload, and load() adds a watch,
  * the new ir.* bindings work (universal list/buttons/code/send/family and
    transmitButton on a .remote file),
  * Vandeta_IR_Suite.js loads, draws its menu and parses payloads.

Menus are read back with `ui` (the firmware's own text state), and every
console session opens the port once - opening it resets the board, so each
scenario runs as one `batch`.

    python3 FREEBUFF/tests/t29_task1_settings.py [port]

Nothing is left on the card: the JS files are uploaded through usb_debug.py
and deleted again, and the bindings test runs with `usb_debug.py run`, which
uploads to /.agenttest.js and removes it.
"""
import os
import re
import subprocess
import sys
import tempfile

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "FREEBUFF", "tools", "usb_debug.py")
TESTS = os.path.join(ROOT, "FREEBUFF", "tests")
BAD_IR = os.path.join(ROOT, "src", "modules", "ir", "ir_suite_src", "Vandeta_IR_Suite.js")

passes = 0
fails = 0


def check(ok, what, detail=""):
    global passes, fails
    if ok:
        passes += 1
        print("  PASS  " + what)
    else:
        fails += 1
        print("  FAIL  " + what + (("  [" + detail + "]") if detail else ""))


def tool(*args, timeout=180):
    r = subprocess.run(
        [sys.executable, TOOL, PORT] + list(args),
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def batch(commands, timeout=240):
    fd, path = tempfile.mkstemp(prefix="t29_", suffix=".cmd")
    try:
        with os.fdopen(fd, "w") as fh:
            fh.write("\n".join(commands) + "\n")
        return tool("batch", path, timeout=timeout)[1]
    finally:
        os.remove(path)


def block(out, title=None):
    """Return (title, rows) for the LAST [UI] dump in `out`."""
    lines = out.replace("\r", "").splitlines()
    start = None
    for i, line in enumerate(lines):
        if line.lstrip().startswith("[UI] "):
            start = i
    if start is None:
        return "", []
    head = lines[start].strip()
    rows = []
    for line in lines[start + 1 :]:
        if line.lstrip().startswith("UISTATE"):
            break
        m = re.match(r"\s*[> ]?(\d+) - (.*)$", line)
        if m:
            rows.append(m.group(2).strip())
    return head, rows


def ui_labels(commands):
    out = batch(commands)
    head, rows = block(out)
    return head, rows, out


def main():
    rc, out = tool("exec", "usbdebug status", timeout=120)
    if "Developer Mode: ON" not in out:
        print("device is not in developer mode - run `usbdebug on` first")
        return 1

    print("\n== Config menu ==")
    # learn where Config is from the main menu
    rc, out = tool("exec", "ui", timeout=120)
    head, rows = block(out)
    cfg = rows.index("Config") if "Config" in rows else None
    check(cfg is not None, "main menu has a Config row", str(rows))

    head, rows, out = ui_labels(["ui select %d" % cfg])
    check("Config" in head, "opened Config (%s)" % head)
    check("HID Settings" in rows, "Config has a top-level HID Settings row")
    check("System Config" in rows, "Config still has System Config")
    hid = rows.index("HID Settings") if "HID Settings" in rows else 0
    system = rows.index("System Config") if "System Config" in rows else 0

    print("\n== HID Settings page ==")
    head, rows, out = ui_labels(["ui select %d" % cfg, "ui select %d" % hid])
    check("HID Settings" in head, "HID Settings opens from Config (%s)" % head)
    check("USB Footprint" in rows, "page is the real HID Settings page")
    check(not any("Payload Manager" in r for r in rows), "no Payload Manager row")

    # System Config holds both Advanced and Developer Options: read it once and
    # address the rows by index instead of assuming their position.
    head, sys_rows, out = ui_labels(["ui select %d" % cfg, "ui select %d" % system])
    check("System Config" in head, "opened System Config (%s)" % head)
    adv = sys_rows.index("Advanced") if "Advanced" in sys_rows else None
    dev = sys_rows.index("Developer Options") if "Developer Options" in sys_rows else None
    check(adv is not None, "System Config still lists Advanced")
    check(dev is not None, "System Config still lists Developer Options")

    print("\n== Advanced menu ==")
    head, rows, out = ui_labels(
        ["ui select %d" % cfg, "ui select %d" % system, "ui select %d" % (adv or 0)]
    )
    check("Advanced" in head, "opened Advanced (%s)" % head)
    check(not any("HID Settings" in r for r in rows), "Advanced no longer lists HID Settings", str(rows))

    print("\n== Developer Options page ==")
    head, rows, out = ui_labels(
        ["ui select %d" % cfg, "ui select %d" % system, "ui select %d" % (dev or 0)]
    )
    check("Developer Options" in head, "Developer Options page opens (%s)" % head)
    check(any("Developer Mode" in r for r in rows), "master row is Developer Mode")
    check(not any("Hot Reload" in r for r in rows), "no Hot Reload row left", str(rows))
    check(any("Show Log Ring" in r for r in rows), "debug rows stay (Show Log Ring)")

    print("\n== hot reload arms itself ==")
    watch = os.path.join(TESTS, "t29_watch.js")
    rc, out = tool("put", watch, "/.t29_watch.js", timeout=120)
    check("DONE /.t29_watch.js" in out, "uploaded the watch script", out[-200:])
    # `js` hands the script to the interpreter task and returns, so the next
    # console command would run before `load()` ever did. `ir rx 3` blocks this
    # CLI for three seconds, which is plenty for the watch script to finish.
    out = batch(["js /.t29_watch.js", "ir rx 3", "hotreload list"])
    check("WATCH ARMED" in out, "load() ran the watch script")
    check("Hot reload: ON" in out, "hot reload is ON in developer mode", out[-400:])
    check("/.t29_watch_target.js" in out, "load() added the target to the watch list")
    tool("exec", "file delete /.t29_watch.js", timeout=60)
    tool("exec", "file delete /.t29_watch_target.js", timeout=60)

    print("\n== ir.* bindings ==")
    rc, out = tool("run", os.path.join(TESTS, "t29_badir_bindings.js"), timeout=420)
    for line in out.replace("\r", "").splitlines():
        if line.startswith("PASS "):
            check(True, line[5:])
        elif line.startswith("FAIL "):
            check(False, line[5:])
    m = re.search(r"RESULT pass=(\d+) fail=(\d+)", out)
    check(m is not None, "bindings test printed a result", out[-400:])
    if m:
        check(int(m.group(2)) == 0, "bindings test has no failures (%s)" % m.group(0))

    print("\n== Vandeta_IR_Suite.js logic ==")
    src = open(BAD_IR, encoding="utf-8").read()
    if "mainMenu();" not in src:
        check(False, "Vandeta_IR_Suite.js still calls mainMenu() at the end")
        return 1
    harness = """
// ---- t29 harness: exercise the script without its blocking menu ----
var pass2 = 0, fail2 = 0;
function c2(ok, what) { if (ok) { pass2++; console.log("PASS " + what); } else { fail2++; console.log("FAIL " + what); } }
c2(typeof parsePayload == "function", "parsePayload defined");
c2(typeof recordText == "function", "recordText defined");
c2(CATS.length == 6, "six remote types like Universal Remote (" + CATS.length + ")");
c2(MENU[0].label == "Bruteforce remotes", "one bruteforce row (" + MENU[0].label + ")");
c2(triggerOf(CATS[1]) == "Off", "an AC is swept by its Off codes");
c2(triggerOf(CATS[5]) == "Power_on", "an LED strip is swept by Power_on");
drawMenu();
c2(true, "main menu renders");
var cmds = parsePayload("DELAY 250\\nUP\\nDOWN\\nOK\\n# comment\\nBADNAME\\n");
c2(cmds.length == 5, "parsePayload keeps 5 commands (" + cmds.length + ")");
c2(cmds[0].delay == 250, "DELAY 250 parsed");
c2(cmds[1].name == "Up", "UP aliased to Up");
c2(cmds[2].name == "Down", "DOWN aliased to Down");
c2(cmds[3].name == "Ok", "OK aliased to Ok");
c2(cmds[4].name == "BADNAME", "unknown button name kept");
var rec = recordText(["Power", "parsed", "NEC", "04 00 00 00", "08 00 00 00", "", "32", "0"]);
c2(rec.indexOf("name: Power") == 0, "recordText writes the name");
c2(rec.indexOf("protocol: NEC") > 0, "recordText writes the protocol");
c2(rec.indexOf("#") > 0, "recordText closes the record");

// Saving a remote: the only blocking part is dialog.info, so stub it. If the
// module object is not writable, say so and move on instead of hanging.
var canStub = false;
try {
    var infoOrig = dialog.info;
    dialog.info = function (a, b) {};
    canStub = (dialog.info !== infoOrig);
    dialog.info = infoOrig;
} catch (saveErr) {
    // mquickjs: a second global `catch (e)` is a syntax error, so this one is
    // deliberately named differently from the HAVE_SD block in Vandeta_IR_Suite.js.
    canStub = false;
}
if (!canStub) {
    console.log("SKIP save test: dialog.info is not writable");
} else {
    var stubOrig = dialog.info;
    dialog.info = function (a, b) {};
    var tvCat = CATS[0];
    saveLibraryCode(tvCat, triggerOf(tvCat), 0);
    dialog.info = stubOrig;
    c2(selectedRemote.indexOf("/BruceIR/IRSuite_TV_") == 0, "saveLibraryCode writes into /BruceIR as .ir (" + selectedRemote + ")");
    c2(selectedRemote.slice(-3) == ".ir", "saved remote keeps the .ir extension");
    var saved = storage.read({ fs: "sd", path: selectedRemote });
    c2(saved.indexOf("Filetype: Bruce IR File") == 0, "saved remote has the recorded filetype");
    c2(saved.indexOf("name: Power") > 0, "saved remote contains Power");
    c2(saved.indexOf("protocol: ") > 0, "saved remote contains a protocol");
    c2(ir.transmitButton(selectedRemote, "Power"), "the saved remote speaks through transmitButton");
    storage.remove({ fs: "sd", path: selectedRemote });
    selectedRemote = "";
    storage.write({ fs: "sd", path: CONFIG_FILE }, "", "write");
}
console.log("RESULT2 pass=" + pass2 + " fail=" + fail2);
"""
    trimmed = src.rsplit("mainMenu();", 1)[0] + harness
    fd, path = tempfile.mkstemp(prefix="t29_badir_", suffix=".js")
    try:
        with os.fdopen(fd, "w") as fh:
            fh.write(trimmed)
        rc, out = tool("run", path, timeout=420)
    finally:
        os.remove(path)
    for line in out.replace("\r", "").splitlines():
        if line.startswith("PASS "):
            check(True, "Vandeta IR Suite: " + line[5:])
        elif line.startswith("FAIL "):
            check(False, "Vandeta IR Suite: " + line[5:])
    m = re.search(r"RESULT2 pass=(\d+) fail=(\d+)", out)
    check(m is not None, "Vandeta IR Suite harness printed a result", out[-500:])
    if m:
        check(int(m.group(2)) == 0, "Vandeta IR Suite harness has no failures (%s)" % m.group(0))

    print("\n%d passed, %d failed" % (passes, fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
