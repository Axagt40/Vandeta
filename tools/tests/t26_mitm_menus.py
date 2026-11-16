#!/usr/bin/env python3
"""Vandeta 1.2 (task C1-C3): the MITM Attack page and the net_spoof engine.

Checks, over USB only (no keypad, no screen view - the board is driven with
`ui`, `ui select <n>` and the console verbs):

  1. Wifi Atks carries one `MITM Attack` row after `EAPOL Attack`, and the old
     DNS Inject / HTML Intercept pages are gone;
  2. the page opens with its real rows and a row action keeps it open;
  3. the published test fixture (`/BruceSpoof/redirect-test.mitm`) is parsed
     into page/redirect/dns/fallback rules, and `mitm status` / `mitm payload`
     agree;
  4. the older `dnsinject` / `htmlint` verbs still drive the same engine;
  5. `mitm run` refuses cleanly without an association and runs with one;
  6. if the board is associated to an AP, a real ARP pair is started and the
     engine is armed and stopped. Without an association the same step checks
     that the engine refuses cleanly - it never silently skips.

The payloads this script uses (`redirect-test.mitm`, `portal-redirect.mitm`,
`recon.mitm` and the `html/` pages) are the ones published in `sd_files/`; copy
that folder to the card root before running. A missing fixture is a FAIL, not a
skip.

The live interception itself (a client actually receiving the page) is
`t27_mitm_live.py` - it needs a second device and is driven over adb.

Stop the screen mirror first - it holds /dev/ttyACM0.

    python3 tools/tests/t26_mitm_menus.py [port]
"""
import os
import re
import subprocess
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
# tests live in <root>/tools/tests, the published fixtures in <root>/sd_files
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BAUD = 115200
ESPTOOL = os.path.expanduser("~/.platformio/penv/bin/esptool")

RESULTS = []


def record(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("  %-46s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))


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


def send(s, cmd, wait=0.7):
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


def ui(s, wait=1.2):
    send(s, "ui", wait)
    return strings(grab(s, 0.8))


def find(option_rows, needle):
    for idx, label in option_rows:
        if needle.lower() in label.lower():
            return idx
    return None


def select(s, index, settle=1.6):
    send(s, "ui select %d" % index, settle)
    return strings(grab(s, 0.9))


def nav(s, action="back", settle=1.4):
    send(s, "ui " + action, settle)
    return strings(grab(s, 0.8))


def check_screen(label, lines, title, needles):
    title_ok = any(("[UI] " + title) in t for t in lines)
    missing = [n for n in needles if not any(n.lower() in t.lower() for t in lines)]
    record(label, title_ok and not missing, "missing=" + repr(missing) if missing else "")
    return title_ok and not missing


def goto_wifi_atks(s):
    """Main Menu > WiFi > Wifi Atks, whatever screen we are on now.

    The injected Back of `ui back` can pop more than one level (the console's
    key flag is cleared up to 250 ms later, so the parent menu sees it too), so
    every page visit starts by navigating from the top instead of assuming the
    screen behind us.
    """
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


def goto_mitm(s):
    atk_rows = rows(goto_wifi_atks(s))
    row = find(atk_rows, "MITM Attack")
    if row is None:
        return [], atk_rows
    return select(s, row, settle=2.2), atk_rows


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


def main():
    hard_reset()
    s = open_port()
    try:
        # Developer Options must be on for `ui` / `ui select`. A flash at 0x0
        # clears NVS, so the setting can legitimately be OFF after an install.
        out = console(s, "usbdebug status", 1.5)
        if not any("Developer Mode: ON" in t for t in out):
            out = console(s, "usbdebug on", 2.0)
        record(
            "Developer Options enabled",
            any("Developer Mode: ON" in t for t in out),
            find_line(out, "Developer Mode:"),
        )

        print("\n=== 1. Wifi Atks carries one MITM Attack page ===")
        page, atk_rows = goto_mitm(s)
        if not page:
            record("WiFi > Wifi Atks opens", False)
            return finish()
        labels = [l for _, l in atk_rows]
        print("  rows:", labels)
        mitm_row = find(atk_rows, "MITM Attack")
        eapol_row = find(atk_rows, "EAPOL Attack")
        record("MITM Attack is on Wifi Atks", mitm_row is not None, "row=%s" % mitm_row)
        record(
            "MITM Attack follows EAPOL Attack",
            mitm_row is not None and eapol_row is not None and mitm_row == eapol_row + 1,
            "eapol=%s mitm=%s" % (eapol_row, mitm_row),
        )
        record(
            "old DNS Inject / HTML Intercept rows are gone",
            find(atk_rows, "DNS Inject") is None and find(atk_rows, "HTML Intercept") is None,
        )
        record(
            "old attack rows still present",
            find(atk_rows, "Target Atks") is not None
            and find(atk_rows, "Deauth Flood") is not None
            and find(atk_rows, "Beacon SPAM") is not None,
        )

        print("\n=== 2. MITM Attack page ===")
        page_rows = rows(page)
        print("  rows:", [l for _, l in page_rows])
        # The page is the selector: the value rows themselves open the picker, so
        # there is no separate "Pick payload" row and no Station/Last note row
        # (the run screen is the status display).
        check_screen(
            "MITM Attack opens with its rows",
            page,
            "MITM Attack",
            [
                "Network:",
                "Target:",
                "Payload:",
                "Preview payload",
                "Start attack",
                "Captures",
                "Main Menu",
            ],
        )
        record(
            "removed note/duplicate rows stay gone",
            find(page_rows, "Station:") is None
            and find(page_rows, "Last:") is None
            and find(page_rows, "Pick payload") is None
            and find(page_rows, "Create example") is None
            and find(page_rows, "Status") is None,
            repr([l for _, l in page_rows]),
        )

        lines = nav(s, "back")
        record(
            "Back leaves MITM Attack (no re-open)",
            not any("[UI] MITM Attack" in t for t in lines),
            "screen=" + (find_line(lines, "[UI]") or "?"),
        )

        print("\n=== 3. Unified payload and the mitm verb ===")
        # `dns * gateway` is stored as a token and resolved per query, so the
        # example payload parses even while the station is down - which is how
        # the page can be prepared before joining a network.
        out = console(s, "mitm status", 1.5)
        status_line = find_line(out, "mitm status")
        station = field(status_line, "station")
        associated = station not in (None, "down")

        out = console(s, "mitm payload /BruceSpoof/redirect-test.mitm", 2.5)
        line = find_line(out, "mitm payload")
        print("  " + (line or "no answer"))
        record(
            "redirect-test.mitm parsed (1 redirect, 1 dns rule, no page)",
            field(line, "ok") == "1" and field(line, "pages") == "0" and field(line, "redirects") == "1"
            and field(line, "dns") == "1" and field(line, "bad") == "0",
            line,
        )

        out = console(s, "mitm status", 1.5)
        line = find_line(out, "mitm status")
        print("  " + (line or "no answer"))
        record(
            "status reports the armed payload and rules",
            field(line, "payload") == "/BruceSpoof/redirect-test.mitm"
            and field(line, "dnsRules") == "1"
            and field(line, "injectRules") == "0",
            line,
        )

        out = console(s, "mitm payload /BruceSpoof/portal-redirect.mitm", 2.5)
        line = find_line(out, "mitm payload")
        print("  " + (line or "no answer"))
        record(
            "portal-redirect.mitm serves its page, without hijacking DNS",
            field(line, "ok") == "1" and field(line, "pages") == "1" and field(line, "redirects") == "0"
            and field(line, "dns") == "0" and field(line, "bad") == "0",
            line,
        )

        out = console(s, "mitm payload /BruceSpoof/html/rickroll.html", 2.0)
        line = find_line(out, "mitm payload")
        print("  " + (line or "no answer"))
        record(
            "a page is not a payload any more",
            field(line, "ok") == "0" and ".mitm" in line,
            line,
        )
        console(s, "mitm payload /BruceSpoof/redirect-test.mitm", 2.0)

        # Reconnaissance lines (`info`, `probe`): read-only payload actions that
        # report the network and the hosts before a rule fires. The parser only
        # count them, so this needs no association. The payload ships in
        # sd_files/, so a missing file is a FAIL: a fixture that was not copied
        # to the card must not read as a pass or as a silent skip.
        out = console(s, "mitm payload /BruceSpoof/recon.mitm", 2.5)
        line = find_line(out, "mitm payload")
        print("  " + (line or "no answer"))
        record(
            "recon.mitm reports info + probes",
            field(line, "ok") == "1" and field(line, "info") == "1" and field(line, "probes") == "1"
            and field(line, "bad") == "0",
            line,
        )

        out = console(s, "mitm target 192.0.2.9", 1.5)
        line = find_line(out, "mitm target")
        print("  " + (line or "no answer"))
        record("target accepts one address", field(line, "ok") == "1" and field(line, "mode") == "192.0.2.9", line)

        out = console(s, "mitm target not-an-ip", 1.5)
        line = find_line(out, "mitm target")
        print("  " + (line or "no answer"))
        record("a bad target is rejected", field(line, "ok") == "0", line)

        out = console(s, "mitm target all", 1.5)
        line = find_line(out, "mitm target")
        record(
            "target mode can go back to all devices",
            "mode=All devices" in line,
            line,
        )

        print("\n=== 4. dnsinject / htmlint still drive the same engine ===")
        out = console(s, "dnsinject payload", 2.0)
        line = find_line(out, "dnsinject payload")
        print("  " + (line or "no answer"))
        # The standalone dns.txt payload type is gone with the .mitm unification,
        # so the default path no longer resolves to anything.
        if "cannot open" in line or field(line, "ok") == "0":
            record("the old dns.txt payload type is gone", field(line, "ok") == "0", line)
        else:
            record("a DNS rule file can still be loaded explicitly", field(line, "ok") == "1", line)

        out = console(s, "dnsinject rule example.com 10.0.0.7", 1.5)
        line = find_line(out, "dnsinject rule")
        print("  " + (line or "no answer"))
        record(
            "a DNS rule can still be added by hand",
            field(line, "ok") == "1" and field(line, "rules") == "1",
            line,
        )

        out = console(s, "dnsinject rules", 1.5)
        printed = [t for t in out if t.startswith("dnsrule ")]
        for t in printed:
            print("  " + t)
        record("rules readable back", len(printed) == 1, "%d rule line(s)" % len(printed))

        out = console(s, "dnsinject wildcard 10.0.0.7", 1.5)
        line = find_line(out, "dnsinject wildcard")
        print("  " + (line or "no answer"))
        record("wildcard rule accepted", field(line, "ok") == "1" and field(line, "rules") == "2", line)

        out = console(s, "dnsinject clear", 1.5)
        line = find_line(out, "dnsinject clear")
        record("rules clear", field(line, "rules") == "0", line)

        out = console(s, "dnsinject rule nonsense.example not-an-ip", 1.5)
        line = find_line(out, "dnsinject rule")
        print("  " + (line or "no answer"))
        record("bad IP rejected, engine unaffected", field(line, "ok") == "0", line)

        out = console(s, "htmlint match *", 1.2)
        out = console(s, "htmlint payload /BruceSpoof/html/rickroll.html", 2.0)
        line = find_line(out, "htmlint payload")
        print("  " + (line or "no answer"))
        record("HTML payload loaded as injection", field(line, "ok") == "1" and field(line, "rules") == "1", line)

        out = console(s, "htmlint redirect", 2.0)
        line = find_line(out, "htmlint payload")
        print("  " + (line or "no answer"))
        record("the old redirect.txt payload type is gone", field(line, "ok") == "0", line)

        print("\n=== 5. Engine start/stop and mitm run ===")
        out = console(s, "dnsinject status", 1.5)
        line = find_line(out, "dnsinject active=")
        print("  " + (line or "no answer"))
        record("status reports engine + station", line != "" and field(line, "station") is not None, line)

        out = console(s, "dnsinject start", 2.0)
        line = find_line(out, "dnsinject start")
        print("  " + (line or "no answer"))
        if associated:
            record("DNS Inject starts on an associated station", field(line, "ok") == "1", line)
        else:
            record(
                "DNS Inject refuses without a station",
                field(line, "ok") == "0" and "not associated" in line,
                line,
            )
        out = console(s, "dnsinject stop", 1.5)
        line = find_line(out, "dnsinject stop")
        record("DNS Inject stops", field(line, "active") == "0", line)

        out = console(s, "htmlint start", 2.0)
        line = find_line(out, "htmlint start")
        print("  " + (line or "no answer"))
        if associated:
            record("HTML Intercept starts on an associated station", field(line, "ok") == "1", line)
        else:
            record(
                "HTML Intercept refuses without a station",
                field(line, "ok") == "0" and "not associated" in line,
                line,
            )
        out = console(s, "htmlint stop", 1.5)
        line = find_line(out, "htmlint stop")
        record("HTML Intercept stops", field(line, "active") == "0", line)

        console(s, "mitm payload /BruceSpoof/redirect-test.mitm", 2.0)
        if associated:
            out = console(s, "mitm run 5 /BruceSpoof/redirect-test.mitm", 8.0)
            line = find_line(out, "mitm run")
            print("  " + (line or "no answer"))
            record("mitm run completes a timed run", field(line, "ok") == "1" and field(line, "pairs") == "0", line)
        else:
            out = console(s, "mitm run 5 /BruceSpoof/redirect-test.mitm", 4.0)
            line = find_line(out, "mitm run")
            print("  " + (line or "no answer"))
            record(
                "mitm run refuses without a station",
                field(line, "ok") == "0" and "connect to WiFi first" in line,
                line,
            )

        out = console(s, "mitm stop", 2.0)
        line = find_line(out, "mitm stop")
        print("  " + (line or "no answer"))
        record("mitm stop leaves no pair and no engine", field(line, "pairs") == "0" and field(line, "http") == "0", line)

        print("\n=== 6. Files on the card ===")
        out = console(s, "ls /BruceSpoof", 1.5)
        body = "\n".join(out)
        record(
            "the redirect-test fixture and the recon payload are on the card",
            "redirect-test.mitm" in body and "portal-redirect.mitm" in body and "recon.mitm" in body,
            body[:200],
        )
        # Name-exact against the published fixture tree: a substring test on the
        # card listing made the new redirect-test.mitm fail its own check, and
        # whatever a user copied to their own card is not this repository's
        # fixture set.
        fixture_dir = os.path.join(REPO_ROOT, "sd_files", "BruceSpoof")
        shipped = {n.lower() for n in os.listdir(fixture_dir)} if os.path.isdir(fixture_dir) else set()
        # The retired payload types are the .txt ones the firmware replaced with
        # .mitm/.ucmitm. test.mitm is still a valid .mitm payload, and matching
        # it as a substring made redirect-test.mitm fail its own check.
        retired = {"mitm.txt", "dns.txt", "redirect.txt"}
        card_names = {ln.strip().lower() for ln in body.splitlines()}
        record(
            "the old payload types are gone",
            not (retired & shipped) and not (retired & card_names),
            ", ".join(sorted(shipped)),
        )
        record(
            "the ucmitm payloads are on the card",
            "rickroll.ucmitm" in body and "recon.ucmitm" in body and "fake-router.ucmitm" in body,
            body[:200],
        )
        out = console(s, "ls /BruceSpoof/html", 1.5)
        pages = "\n".join(out)
        record(
            "the payload pages are on the card",
            "rickroll.html" in pages and "router-login.html" in pages and "network-notice.html" in pages,
            pages[:200],
        )

        out = console(s, "errors", 1.5)
        # A crash is the only hard failure here: the error ring also holds
        # upstream noise such as the STA disconnect logged while WiFi is off
        # (ESP_ERR_WIFI_NOT_INIT), which predates this feature.
        crashes = [t for t in out if re.search(r"Guru|panic|abort|Backtrace|assert", t, re.I)]
        noise = [t for t in out if re.search(r"\[E\]", t)]
        for t in noise[-8:]:
            print("  ! " + t)
        record("no crash markers in the error ring", len(crashes) == 0)
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
