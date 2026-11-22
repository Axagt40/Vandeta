#!/usr/bin/env python3
"""Host-side checks for the Bad IR TV brute: full-button remotes, SEL detection
and the open_mirror payload.

Why this exists
---------------
Three separate things looked like one bug ("brute force works, but the remote it
saves cannot open the input menu, and SEL needs to be held").

1. tv.ir - the universal library the sweep walks - holds only six button names:
   Power, Vol_up, Vol_dn, Mute, Ch_next, Ch_prev. There is no Up, no Ok and no
   Input in it at all, so a remote rebuilt from tv.ir can never press them no
   matter how the sweep went. The library files under sd_files/infrared/TVs/ do
   have them, but 161 of those files share just 26 distinct Power codes: the
   Power frame identifies a *family* (protocol + address), never a model. The
   merged family remotes (sd_files/infrared/families, built by
   tools/build_ir_families.py) are the union of every model in a family, so one
   Power code now resolves to arrows + OK + Input/Source.

   This test proves the lookup closes: for every Power code in tv.ir that has a
   family, the exact path Vandeta_IR_Suite.js derives from protocol+address exists on the
   card, and that file really carries the navigation and source buttons.

2. The sweep polled keyboard.getSelPress() exactly once, after a blocking IR
   send. A tap that landed while the frame was going out was lost, which is why
   SEL had to be held or hammered. The script now samples for a window; this
   test guards the shape of that loop (a single-shot check must not come back).

3. "open_mirror" is a payload file, and its button names have to survive the
   script's own ALIAS table - INPUT must reach the remote as "Input", because
   that is the name the family remotes use.

Run:  python3 tools/tests/t30_badir_family_test.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BADIR = os.path.join(ROOT, "src", "modules", "ir", "ir_suite_src", "Vandeta_IR_Suite.js")
TV_LIB = os.path.join(ROOT, "sd_files", "infrared", "Universal", "tv.ir")
FAMILIES = os.path.join(ROOT, "sd_files", "infrared", "families")
MODELS = os.path.join(ROOT, "sd_files", "infrared", "TVs")
PAYLOAD = os.path.join(ROOT, "sd_files", "BruceIR", "payloads", "open_mirror.txt")

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)
        print("FAIL  %s" % what)


def parse_ir(path):
    """Flipper-style .ir -> list of records, same reading rules as the firmware."""
    records = []
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("name:"):
                if cur:
                    records.append(cur)
                cur = {"name": line[5:].strip()}
            elif cur is not None and line and not line.startswith("#") and ":" in line:
                key, value = line.split(":", 1)
                cur[key.strip().lower()] = value.strip()
    if cur:
        records.append(cur)
    return records


def family_path(protocol, address):
    """The exact string Vandeta_IR_Suite.js builds - kept in sync with the JS regex."""
    tag = "%s_%s" % (protocol, address.replace(" ", "-"))
    tag = re.sub(r"[^A-Za-z0-9_.-]", "_", tag)
    return os.path.join(FAMILIES, "FAM_%s.ir" % tag)


def main():
    for path in (BADIR, TV_LIB, PAYLOAD):
        if not os.path.exists(path):
            print("missing file: %s" % path, file=sys.stderr)
            return 2
    if not os.path.isdir(FAMILIES):
        print("missing family library: %s (run tools/build_ir_families.py)" % FAMILIES, file=sys.stderr)
        return 2

    # ---------------------------------------------------------------- 1. library
    families = sorted(f for f in os.listdir(FAMILIES) if f.endswith(".ir"))
    check(len(families) >= 40, "family library has %d merged remotes" % len(families))

    model_files = []
    for brand in sorted(os.listdir(MODELS)):
        brand_dir = os.path.join(MODELS, brand)
        if os.path.isdir(brand_dir):
            model_files += [
                os.path.join(brand_dir, f) for f in sorted(os.listdir(brand_dir)) if f.endswith(".ir")
            ]
    check(len(model_files) > 150, "%d per-model remotes in the library" % len(model_files))

    # every family file is a parseable .ir with unique names and real commands
    total_buttons = 0
    for name in families:
        records = parse_ir(os.path.join(FAMILIES, name))
        check(len(records) > 0, "%s is not empty" % name)
        names = [r["name"] for r in records]
        check(len(names) == len(set(names)), "%s has no duplicate button names" % name)
        bad = [
            r["name"]
            for r in records
            if not r.get("protocol") or not r.get("address") or not r.get("command")
        ]
        check(not bad, "%s has no record missing protocol/address/command (%s)" % (name, bad[:3]))
        check(
            "raw" not in {r.get("type", "parsed") for r in records},
            "%s contains only decoded records (a raw dump cannot be re-sent by name)" % name,
        )
        total_buttons += len(records)

    # ------------------------------------------------- 2. the lookup actually hits
    tv = parse_ir(TV_LIB)
    powers = [r for r in tv if r["name"] == "Power"]
    check(len(powers) > 100, "tv.ir holds %d Power codes to sweep" % len(powers))

    hits = 0
    have_nav = 0
    have_input = 0
    gaps = set()
    for rec in powers:
        path = family_path(rec.get("protocol", ""), rec.get("address", ""))
        if not os.path.exists(path):
            continue
        hits += 1
        buttons = {r["name"] for r in parse_ir(path)}
        nav = all(n in buttons for n in ("Up", "Down", "Left", "Right", "Ok"))
        inp = "Input" in buttons
        have_nav += 1 if nav else 0
        have_input += 1 if inp else 0
        if not (nav and inp):
            gaps.add(os.path.basename(path))

    check(hits >= 20, "%d of the %d swept Power codes resolve to a family remote" % (hits, len(powers)))
    # The library itself is uneven: some model files are volume and digits only,
    # so no merge can invent arrows for them. What must hold is that the families
    # the sweep actually meets are mostly complete, and that the gaps are SAID
    # (capabilityText), not silently ignored.
    check(have_nav >= hits * 0.6, "%d/%d resolved families have arrows + OK" % (have_nav, hits))
    check(have_input >= hits * 0.55, "%d/%d resolved families have Input" % (have_input, hits))
    print(
        "      %d/%d resolved families are incomplete: %s"
        % (len(gaps), hits, ", ".join(sorted(gaps)) or "none")
    )

    # the four families that cover most of the library must be complete, or the
    # whole point of the change is lost
    for big in (
        "FAM_Samsung32_07-00-00-00.ir",
        "FAM_NEC_04-00-00-00.ir",
        "FAM_SIRC_01-00-00-00.ir",
        "FAM_RC6_00-00-00-00.ir",
    ):
        two = os.path.join(FAMILIES, big)
        check(os.path.exists(two), "%s exists" % big)
        if not os.path.exists(two):
            continue
        buttons = {r["name"] for r in parse_ir(two)}
        check(
            all(n in buttons for n in ("Up", "Down", "Left", "Right", "Ok", "Input")),
            "%s can press arrows, OK and Input" % big,
        )

    # the same union of buttons is not reachable from tv.ir alone - this is the
    # regression guard for "it only ever saved Power/Vol/Ch".
    tv_names = {r["name"] for r in tv}
    for absent in ("Up", "Down", "Ok", "Input"):
        check(absent not in tv_names, "tv.ir still has no %s button (family remotes are required)" % absent)

    # a family must not collapse two different commands onto the same name: the
    # alternates are what sendButton() falls back to.
    multi = 0
    for name in families:
        names = [r["name"] for r in parse_ir(os.path.join(FAMILIES, name))]
        if any(n.endswith("_2") for n in names):
            multi += 1
    check(multi > 0, "%d families keep alternative codes as NAME_2/NAME_3" % multi)

    # ------------------------------------------------------- 3. bad_ir.js shape
    src = open(BADIR, "r", encoding="utf-8").read()
    check("function awaitReaction(" in src, "Vandeta_IR_Suite.js samples the keys in a window (awaitReaction)")
    check(
        src.count("awaitReaction(") >= 4,
        "both sweeps and both narrowing passes use the window (%d call sites)" % src.count("awaitReaction(")
    )
    # the single-shot check after a send is what lost taps - it must be gone from
    # the sweep loops. (The menu's own SELECT handling is a different, deliberate
    # single read and is not matched here.)
    stray = re.findall(r"ir\.universalSend\([^\n]*\n\s*if \(keyboard\.getSelPress\(\)\)", src)
    check(not stray, "no sweep still decides on one getSelPress() right after the send")
    # ESC has to be tested before any-key, or holding ESC would count as a hit
    body = src.split("function awaitReaction(", 1)[1].split("\n}", 1)[0]
    check(
        body.index("getEscPress") < body.index("getAnyPress"),
        "awaitReaction() checks ESC before the any-key confirmation",
    )
    check("keyboard.getSelPress()" in body, "awaitReaction() accepts SELECT")
    check(
        re.search(r"if \(storage\.exists\(card\(fam\)\)\)", src) is not None,
        "saveUniversal() prefers the merged family remote",
    )
    check("function sendButton(" in src, "payload runner can fall back to NAME_2/NAME_3")
    check("function familySweep(" in src, "the fast family sweep is in the menu path")
    check(
        re.search(r"function familySweep\([^)]*\)[\s\S]*?awaitReaction\(", src) is not None,
        "the family sweep also waits in a window",
    )
    # Sweep pacing must stay in step with IR > Universal Remote (100 ms between
    # candidates in ir_universal.cpp), and the lock list must hold the 12 codes
    # the user asked for - the replay is where a fast sweep is made usable.
    check(
        re.search(r"var SWEEP_GAP_MS = 100;", src) is not None,
        "Vandeta IR Suite sweeps at the universal remote's 100 ms cadence",
    )
    check(
        re.search(r"var LOCK_CODES = 10;", src) is not None
        and re.search(r"if \(lastSent\.length > LOCK_CODES\) lastSent\.shift\(\);", src) is not None,
        "the last 10 codes are locked before narrowing",
    )
    # a replay must be a separate, countable event: a 0.5 s pause, then the send,
    # then the full listening window (both narrowing passes)
    check(
        re.search(r"var NARROW_PAUSE_MS = 500;", src) is not None
        and src.count("waitOrEsc(NARROW_PAUSE_MS)") == 2,
        "both narrowing passes pause 0.5 s before each replay send",
    )
    check("function capabilityText(" in src, "a saved remote reports what it cannot press")
    # the menu gained a row; it still has to fit the T-Embed's 170 px panel
    menu_rows = src.split("var MENU = [", 1)[1].split("];", 1)[0].count("label:")
    top = int(re.search(r"var MENU_TOP = (\d+);", src).group(1))
    row_h = int(re.search(r"var ROW_H = (\d+);", src).group(1))
    check(
        top + menu_rows * row_h <= 170,
        "the %d menu rows end at y=%d on a 170 px panel" % (menu_rows, top + menu_rows * row_h),
    )
    check(
        src.count("capabilityText(") >= 4,
        "every save path names the missing buttons (%d uses)" % src.count("capabilityText("),
    )
    check(
        'SOURCE: "Input"' in src and 'INPUT: "Input"' in src,
        "SOURCE and INPUT both map to the family remote's Input button",
    )

    # --------------------------------------------------------- 4. the payload
    text = open(PAYLOAD, "r", encoding="utf-8").read()
    # the ALIAS table packs several entries per line, so match anywhere inside it
    alias_block = src.split("var ALIAS = {", 1)[1].split("\n};", 1)[0]
    alias = dict(re.findall(r'([A-Z0-9_+/-]+):\s*"([^"]+)"', alias_block))
    steps = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        head = line.split()[0].upper()
        if head in ("DELAY", "REM"):
            continue
        steps.append(alias.get(head, line.split()[0]))
    # HOME first: the Sony keeps the DLNA/mirror player on screen otherwise and
    # ignores the input switch, so the sequence that works is HOME > INPUT > 1.
    check(
        steps == ["Home", "Input", "1"],
        "open_mirror maps HOME>INPUT>1 to %s" % (steps,),
    )
    check(len(families) > 0 and total_buttons > 500,
          "family remotes carry %d buttons in total" % total_buttons)

    print()
    print("%d checks, %d failures" % (checks, len(failures)))
    for failure in failures:
        print("  - %s" % failure)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
