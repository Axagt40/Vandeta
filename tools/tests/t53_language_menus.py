#!/usr/bin/env python3
"""Vandeta 1.5: the language switch and the menus under it.

Opening the USB port resets the board, so every screen is read in its own
`batch` session. That is deliberate: a page that waits for a keypad press would
otherwise stall a single long batch and hide the pages after it.

  1. switch to each shipped language through Config > System Config > Language
     and check the root menu's painted text (`tr` in UISTATE);
  2. in Spanish, open all 15 root menus and check each one still renders its
     rows - this is the "every menu under a language" pass;
  3. confirm the choice survives the next session (bruce.conf), then restore
     English.

    python3 tools/tests/t53_language_menus.py [port]
"""
import json
import os
import subprocess
import sys
import tempfile

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "FREEBUFF", "tools", "usb_debug.py")

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
        [sys.executable, TOOL, PORT] + list(args), capture_output=True, text=True, timeout=timeout
    )
    return (r.stdout or "") + (r.stderr or "")


def batch(commands, timeout=180):
    fd, path = tempfile.mkstemp(prefix="t53_", suffix=".cmd")
    try:
        with os.fdopen(fd, "w") as fh:
            fh.write("\n".join(commands) + "\n")
        return tool("batch", path, timeout=timeout)
    finally:
        os.remove(path)


def states(out):
    found = []
    for line in out.replace("\r", "").splitlines():
        if line.lstrip().startswith("UISTATE "):
            try:
                found.append(json.loads(line.lstrip()[8:]))
            except ValueError:
                pass
    return found


def screen(commands, timeout=180):
    found = states(batch(commands, timeout))
    return found[-1] if found else {}


def labels(st):
    return [o.get("label", "") for o in st.get("options", [])]


def painted(st):
    return [o.get("tr", o.get("label", "")) for o in st.get("options", [])]


def find(st, text):
    for i, label in enumerate(labels(st)):
        if label == text or label.startswith(text):
            return i
    return -1


def switch(index, timeout=180):
    """Config > System Config > Language > <index>, then back to the root."""
    cfg = screen(["ui"])
    cfg_row = find(cfg, "Config")
    conf = screen(["ui", "ui select %d" % cfg_row])
    sys_row = find(conf, "System Config")
    system = screen(["ui", "ui select %d" % cfg_row, "ui select %d" % sys_row])
    lang_row = find(system, "Language")
    if cfg_row < 0 or sys_row < 0 or lang_row < 0:
        return {}, "rows cfg=%d sys=%d lang=%d" % (cfg_row, sys_row, lang_row)
    after = screen(
        [
            "ui",
            "ui select %d" % cfg_row,
            "ui select %d" % sys_row,
            "ui select %d" % lang_row,
            "ui select %d" % index,
            "press back",
            "press back",
            "press back",
            "ui",
        ],
        timeout,
    )
    return after, ""


def main():
    out = tool("exec", "usbdebug status")
    if "Developer Mode: ON" not in out:
        tool("exec", "usbdebug on")

    print("\n== English baseline ==")
    root = screen(["ui"])
    names = labels(root)
    check(len(names) == 15, "root menu has 15 rows", str(names))

    cases = [
        (1, "Espanol", {"Files": "Archivos", "Clock": "Reloj", "Others": "Otros"}),
        (2, "Francais", {"Files": "Fichiers", "Clock": "Horloge", "Others": "Autres"}),
        (3, "Deutsch", {"Files": "Dateien", "Clock": "Uhr", "Others": "Mehr"}),
        (4, "Portugues", {"Files": "Arquivos", "Clock": "Relogio", "Others": "Outros"}),
    ]

    for index, name, expect in cases:
        print("\n== %s ==" % name)
        after, err = switch(index)
        tr = dict(zip(labels(after), painted(after)))
        check(len(tr) == 15, "%s root painted" % name, err or str(len(tr)))
        for en, local in expect.items():
            check(tr.get(en) == local, "%s: %r -> %r" % (name, en, local), str(tr.get(en)))

        # The Config page is where the switch lives, so it is checked in every
        # language; the full 15-menu walk runs in Spanish below.
        conf = screen(["ui", "ui select 14"])
        tr = dict(zip(labels(conf), painted(conf)))
        check(
            tr.get("System Config") == {"Espanol": "Config sistema", "Francais": "Config systeme",
                                        "Deutsch": "Systemkonfig", "Portugues": "Config sistema"}[name],
            "%s: Config rows are painted" % name,
            str(tr.get("System Config")),
        )

        if name == "Espanol":
            print("\n-- every root menu in Spanish --")
            for i, menu in enumerate(names):
                st = screen(["ui", "ui select %d" % i], timeout=60)
                title = st.get("menu_title", "")
                rows = st.get("options", [])
                if title == menu or (title == "Main Menu" and menu in ("Files", "Apps", "Clock", "GPS")):
                    check(True, "%s opens (%d rows)" % (menu, len(rows)))
                    empty = [x for x in painted(st) if not x.strip()]
                    check(not empty, "%s paints every row" % menu)
                else:
                    check(False, "%s opens" % menu, "title=%r" % title)

    print("\n== persistence ==")
    root = screen(["ui"])
    tr = dict(zip(labels(root), painted(root)))
    check(tr.get("Files") == "Arquivos", "Portuguese survives the reboot", str(tr.get("Files")))

    print("\n== restore English ==")
    after, err = switch(0)
    tr = dict(zip(labels(after), painted(after)))
    check(tr.get("Files") == "Files", "root reads English again", str(tr.get("Files")))

    print("\n%d passed, %d failed" % (passes, fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
