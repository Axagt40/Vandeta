#!/usr/bin/env python3
"""Vandeta 1.3: the connected MITM engine against a real Android victim.

This is the test the twin cannot give: the board is an ordinary client of the
same network (so WPA2 *or* WPA3, and no deauth anywhere), it poisons exactly one
host, and the phone's own traffic decides pass/fail.

What is checked, and why each one matters:

  * the phone still has internet when the payload bridges (portal-redirect has
    `forward on`) - the old behaviour cut every site, which is the bug report;
  * a plain-HTTP request from any host gets the payload page;
  * the OS captive-portal probe gets a 302 to the board (this is the only path a
    redirect has to a modern phone: the OS opens its own sign-in browser);
  * an HTTPS site is *not* silently redirected - it either works (bridged) or
    stops at a certificate warning, which is the honest limit without a CA;
  * a host that is NOT the target keeps its internet for the whole run;
  * the poison actually owns the phone's DNS: the resolver address Android
    handed this network is asked over the wire (raw UDP, no root - see
    tests/phone/dns_raw.sh) and the payload's 0.0.0.0 rule has to answer;
  * after `mitm stop` the target is back online and no ARP pair is left.

    python3 FREEBUFF/tests/t45_mitm_phone_live.py [port]

The board must already have the network's credentials saved (`wifi on` inside
Bruce), and the phone must be on the same network. Nothing outside the lab is
touched: every attack is scoped to the phone's own IP.
"""
import os
import re
import subprocess
import sys
import threading
import time

import serial

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from phone_lab import PhoneLab  # noqa: E402

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200
RESULTS = []
BOARD_LOG = []
PAYLOAD_DIR = "/BruceSpoof"
PAYLOADS = ["portal-redirect.mitm", "evil-mitm.mitm"]


def record(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("  %-52s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))


def skip(name, detail=""):
    RESULTS.append((name, None, detail))
    print("  %-52s SKIP%s" % (name, ("  " + detail) if detail else ""))


# --------------------------------------------------------------------- board
def open_port():
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    s.dtr = True
    s.rts = False
    time.sleep(9)
    s.reset_input_buffer()
    return s


def send(s, cmd, wait=1.0):
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
    return [t.decode(errors="replace").rstrip() for t in bytes(buf).split(b"\n") if t.strip()]


def reader(s, seconds):
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        c = s.read(4096)
        if c:
            buf.extend(c)
            while b"\n" in buf:
                line, _, rest = buf.partition(b"\n")
                buf = bytearray(rest)
                text = line.decode(errors="replace").rstrip()
                if text:
                    BOARD_LOG.append(text)
                    print("  | " + text)


def board_wifi_ip(s, tries=3, settle=12.0):
    """Bring the board's station up and return its address ("" if it never came).

    A single `wifi on` is not enough: after a reset the join sometimes needs a
    second attempt, and a fixed sleep is a coin flip (measured 2026-11-02).
    """
    for _ in range(tries):
        send(s, "wifi on", settle)
        send(s, "info", 2.0)
        for line in grab(s, 1.5):
            m = re.search(r"Ip: (\d+\.\d+\.\d+\.\d+)", line)
            if m and m.group(1) != "0.0.0.0":
                return m.group(1)
    return ""


def last(lines, prefix):
    for t in reversed(lines):
        if t.startswith(prefix):
            return t
    return ""


def field(line, key):
    m = re.search(r"\b%s=(\S+)" % re.escape(key), line)
    return m.group(1) if m else None


# ----------------------------------------------------------------------- host
def lan_iface(target_ip):
    """Which of this PC's interfaces would carry traffic to <target_ip>. None if none.

    Scoping means "another client of the SAME network is untouched". This bench's
    PC sends everything out a preferred default route on a different subnet (a
    192.168.157.x uplink with a randomized gateway MAC), so a bare curl measured
    an unrelated link - and failed during evil-mitm, because that uplink runs
    through the phone under test. Asking the kernel which interface reaches the
    target gives the interface the claim is actually about.
    """
    r = subprocess.run(["ip", "route", "get", target_ip], capture_output=True, text=True)
    m = re.search(r"\bdev (\S+)", r.stdout)
    return m.group(1) if m else None


def internet_ok(iface=None, timeout=6):
    """Does THIS PC still have its own internet? Proves the attack is scoped.

    With an interface, the request is forced out that interface, so the answer
    describes the route that shares the target's network rather than whatever
    default route the host happens to prefer.
    """
    bind = ["--interface", iface] if iface else []
    for url in ("http://connectivitycheck.gstatic.com/generate_204", "http://1.1.1.1/"):
        r = subprocess.run(
            ["curl", "-s", "-m", str(timeout), "-o", "/dev/null", "-w", "%{http_code}"]
            + bind + [url],
            capture_output=True, text=True,
        )
        if r.stdout.strip() in ("204", "200", "301", "302"):
            return True
    return False


def preferred_gateway():
    """The gateway of this PC's preferred default route."""
    r = subprocess.run(["ip", "-4", "route", "show", "default"], capture_output=True, text=True)
    m = re.search(r"^default via (\d+\.\d+\.\d+\.\d+)", r.stdout, re.M)
    return m.group(1) if m else None


def uplink_risk(phone_ip):
    """True when this PC's internet does not go out the target's network.

    That is not a detail: the evil-mitm payload answers every name with
    0.0.0.0, which is exactly how it cuts the victim's internet. If this PC
    reaches the internet through the phone (a hotspot on the phone, a tether),
    the same run cuts THIS machine's name resolution - and a remote coding
    session dies with it. Measured 2026-11-02: the run dropped the session and
    the "this PC is still online" check failed, on a 192.168.157.x uplink whose
    gateway MAC is randomized (not the board's 44:b1:76:...).
    """
    gw = preferred_gateway()
    if not gw:
        return False, None
    same = gw.rsplit(".", 1)[0] == phone_ip.rsplit(".", 1)[0]
    return (not same), gw


def scoping(name, lan, before):
    """Record (or skip) the "another client is untouched" claim honestly.

    A skip here is information, not a pass: the claim cannot be tested from a
    host that has no route into the target's network, or that was already
    offline before the run started.
    """
    if lan is None:
        skip(name, "this PC has no interface on the target's network")
    elif not before:
        skip(name, "this PC had no internet on %s before the run" % lan)
    else:
        record(name, internet_ok(lan), "via %s" % lan)


def http_probe(phone, url):
    """HTTP probe with the hostname pinned to its HOST-resolved address.

    The phone's own resolver lives in the app netns and a run-as process keeps
    the shell netns, so curl there cannot resolve; the address is pinned
    instead. The traffic still takes the poisoned path, which is what the test
    is about. The DNS answer itself can only be checked with root on the phone.
    """
    host = url.split("://", 1)[1].split("/", 1)[0]
    ip = phone.resolve(host)
    return phone.probe("http", url, ip or "")


def https_probe(phone, url):
    host = url.split("://", 1)[1].split("/", 1)[0]
    ip = phone.resolve(host)
    return phone.probe("https", url, ip or "")


def wait_for_marker(phone, url, want="rickroll", tries=5, gap=5.0):
    """Retry an HTTP probe until the page appears, reporting when it did.

    The first requests of a run can still slip past while the target's ARP
    entry is being rewritten, so a single early probe answers a different
    question ("was I intercepted in the first 8 s?") than the one that matters
    ("does the target get the page?"). Returns (ok, seconds_until_first_hit,
    last_result).
    """
    last = {}
    for i in range(tries):
        last = http_probe(phone, url)
        if last.get("http.marker") == want:
            return True, i * gap, last
        time.sleep(gap)
    return False, None, last


def wait_for_internet(phone, url="https://github.com/", tries=8, gap=6.0):
    """Poll until the phone is back online (the restore takes a moment)."""
    last = {}
    for i in range(tries):
        last = https_probe(phone, url)
        if last.get("https.status") in ("200", "301", "302"):
            return True, i * gap, last
        time.sleep(gap)
    return False, None, last


def main():
    phone = PhoneLab()
    if not phone.present():
        skip("connected MITM against the phone", "no adb device")
        return finish()
    if not phone.termux_ready():
        skip("connected MITM against the phone", "Termux not installed/usable")
        return finish()

    ip = phone.ip()
    if not ip:
        skip("connected MITM against the phone", "phone has no WiFi address")
        return finish()
    print("  phone: %s  ssid=%s  mac=%s" % (ip, phone.ssid(), phone.unique_mac()))

    # The phone must not fall back to mobile data, or "no internet" is invisible.
    phone.mobile_data(False)
    phone.private_dns("off")

    # The phone's own resolver, before any attack: the MITM claim is that the
    # poison answers *this* address, so the address has to be known and healthy
    # first. Raw UDP, no root - the app resolver needs netd and refuses a
    # run-as context, which is why this check used to be a skip.
    resolver = phone.resolver()
    # Some home routers answer NODATA for certain names (example.com does on
    # this one), so the baseline takes the first name that actually answers
    # rather than blaming the network for a resolver quirk.
    base_dns = {}
    for _h in ("luksia.fi", "example.com"):
        base_dns = phone.dns_raw(_h, resolver) if resolver else {}
        if base_dns.get("dnsraw.ip") not in (None, "none", "0.0.0.0"):
            break
    record(
        "phone resolves through its own resolver before the attack",
        base_dns.get("dnsraw.ip") not in (None, "none", "0.0.0.0"),
        "resolver=%s ip=%s" % (resolver, base_dns.get("dnsraw.ip")),
    )
    # Scoping is only meaningful on the interface that actually shares the
    # target's network, and only if it was online to begin with.
    lan = lan_iface(ip)
    pc_online_before = bool(lan) and internet_ok(lan)

    # Refuse, by default, to run a payload that can cut the link this session is
    # using. The override exists for a bench where the PC's uplink is known to be
    # independent even though the gateway is on another subnet.
    risk, gw = uplink_risk(ip)
    if risk and not os.environ.get("T45_ALLOW_UPLINK_RISK"):
        print("  WARNING: this PC's internet goes out via %s, not the target's %s.x network."
              % (gw, ip.rsplit(".", 1)[0]))
        print("           evil-mitm answers DNS with 0.0.0.0 for the phone. If this PC reaches")
        print("           the internet through that phone, this run cuts this PC's resolvers too")
        print("           (and can drop a remote session).")
        print("           Move this PC onto the target's network, or set T45_ALLOW_UPLINK_RISK=1.")
        skip("connected MITM run",
             "this PC's uplink is via %s - refusing to cut the bench's own internet" % gw)
        return finish()

    s = open_port()
    try:
        send(s, "usbdebug status", 1.2)
        if not any("Developer Mode: ON" in t for t in grab(s, 1.2)):
            send(s, "usbdebug on", 1.5)
            grab(s, 1.0)

        # The board has to be an associated client for any of this to work.
        board_ip = board_wifi_ip(s)
        record("board is associated (same network as the phone)", bool(board_ip), board_ip or "no address")
        if not board_ip:
            return finish()

        # The payloads are project files; keep the card's copy current.
        # usb_debug needs the port exclusively - leave it first or the upload
        # blocks until it times out with the console still open here.
        s.close()
        for name in PAYLOADS:
            local = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "sd_files", "BruceSpoof", name)
            subprocess.run(
                [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "usb_debug.py"),
                 PORT, "put", os.path.normpath(local), PAYLOAD_DIR + "/" + name],
                capture_output=True, text=True, timeout=120,
            )
        # usb_debug opens the port too, so the console has to be taken again -
        # and every one of those opens RESET the board, which drops the station.
        # Without the `wifi on` below the run failed instantly with
        # "connect to WiFi first" while `info` had shown an address a minute
        # earlier (measured 2026-11-02).
        s.close()
        s = open_port()
        send(s, "usbdebug on", 1.5)
        grab(s, 1.0)
        board_ip = board_wifi_ip(s)
        record("board is associated again after the upload", bool(board_ip), board_ip or "no address")
        if not board_ip:
            return finish()

        send(s, "mitm payload %s/portal-redirect.mitm" % PAYLOAD_DIR, 2.5)
        line = last(grab(s, 1.0), "mitm payload")
        record("portal-redirect.mitm parses", field(line, "ok") == "1", line or "no answer")
        record("it arms a page rule", field(line, "pages") == "1", line or "")

        send(s, "mitm target " + ip, 1.5)
        line = last(grab(s, 0.8), "mitm target")
        record("target is the phone only", field(line, "ok") == "1" and ip in (line or ""), line or "")

        before = http_probe(phone, "http://luksia.fi/")
        print("  phone before: http=%s" % before.get("http.status"))

        print("\n=== run A: portal-redirect (rickroll + captive-portal prompt), 75 s")
        send(s, "mitm run 75 %s/portal-redirect.mitm" % PAYLOAD_DIR, 0.2)
        t = threading.Thread(target=reader, args=(s, 78), daemon=True)
        t.start()
        time.sleep(8)

        # 1. plain HTTP, by name and by IP
        ok, at, p_http = wait_for_marker(phone, "http://luksia.fi/")
        record(
            "phone: a plain-HTTP site shows the payload page",
            ok,
            "status=%s marker=%s takeover<=%ss"
            % (p_http.get("http.status"), p_http.get("http.marker"), at),
        )
        ok_ip, at_ip, p_ip = wait_for_marker(phone, "http://1.1.1.1/")
        record(
            "phone: an IP-only request shows the payload page too",
            ok_ip,
            "status=%s marker=%s takeover<=%ss"
            % (p_ip.get("http.status"), p_ip.get("http.marker"), at_ip),
        )

        # 2. the captive-portal probe: 302 to the board is what makes the OS
        #    offer its own sign-in browser (and thus the page) by itself.
        p_portal = phone.probe("portal", "", phone.resolve("connectivitycheck.gstatic.com") or "")
        code = p_portal.get("portal.gstatic.code")
        loc = p_portal.get("portal.gstatic.location", "")
        record(
            "phone: OS captive probe answers 302, not 204",
            code == "302",
            "code=%s location=%s" % (code, loc),
        )
        record("phone: probe redirect points at the board", board_ip in loc, loc or "no location")

        # 3. HTTPS is bridged: the phone keeps its internet (the reported bug
        #    was that the poison killed every site).
        p_https = https_probe(phone, "https://github.com/")
        record(
            "phone: an HTTPS site still reaches the internet (bridged)",
            p_https.get("https.status") in ("200", "301", "302"),
            "status=%s trusted=%s" % (p_https.get("https.status"), p_https.get("https.trusted")),
        )
        if p_https.get("https.status") not in ("200", "301", "302"):
            print("  note: HTTPS did not load - a typed HTTPS URL cannot be")
            print("        redirected without a certificate the phone trusts.")

        # 4. the payload bridges DNS, so the phone's resolver still answers for
        #    real instead of being pinned to 0.0.0.0.
        dr_a = phone.dns_raw("luksia.fi", resolver)
        record(
            "portal-redirect: DNS is bridged (a real address, not 0.0.0.0)",
            dr_a.get("dnsraw.ip") not in (None, "none", "0.0.0.0"),
            "ip=%s" % dr_a.get("dnsraw.ip"),
        )

        # 5. scoping: this PC is on the same network and must be untouched.
        scoping("scoping: this PC keeps its internet during the run", lan, pc_online_before)

        t.join()
        summary = ""
        for ln in reversed(BOARD_LOG):
            if ln.startswith("mitm run ok="):
                summary = ln
                break
        print("  " + (summary or "no summary"))
        m = re.search(r"http (\d+)/(\d+)", summary or "")
        record("board counted the injected requests", bool(m) and int(m.group(1)) > 0, m.group(0) if m else "")
        record("no ARP pair left after the run", field(summary, "pairs") == "0", summary or "")

        # 5. the phone is back on the real network. The restore re-ARPs both
        # ends immediately, but the phone's neighbour table settles a moment
        # later, so give it a beat before calling it a failure.
        back, back_at, after = wait_for_internet(phone)
        record(
            "phone: internet is back after the run",
            back,
            "status=%s after<=%ss" % (after.get("https.status"), back_at),
        )

        print("\n=== run B: evil-mitm (no internet, no deauth), 60 s")
        send(s, "mitm payload %s/evil-mitm.mitm" % PAYLOAD_DIR, 2.5)
        line = last(grab(s, 1.0), "mitm payload")
        record("evil-mitm.mitm parses", field(line, "ok") == "1", line or "no answer")
        send(s, "mitm target " + ip, 1.5)
        grab(s, 0.8)
        send(s, "mitm run 60 %s/evil-mitm.mitm" % PAYLOAD_DIR, 0.2)
        t = threading.Thread(target=reader, args=(s, 63), daemon=True)
        t.start()
        time.sleep(9)

        # The check that used to be a skip: ask the resolver Android gave this
        # network, over the wire, and let the poison answer.
        dr = phone.dns_raw("luksia.fi", resolver)
        record(
            "evil-mitm: the phone's own resolver answers 0.0.0.0",
            dr.get("dnsraw.ip") == "0.0.0.0",
            "resolver=%s ip=%s" % (resolver, dr.get("dnsraw.ip")),
        )
        p_http2 = http_probe(phone, "http://luksia.fi/")
        record(
            "evil-mitm: plain HTTP gets nothing",
            p_http2.get("http.marker") in ("none", None) and p_http2.get("http.status") in ("none", "000", None),
            "status=%s marker=%s" % (p_http2.get("http.status"), p_http2.get("http.marker")),
        )
        p_https2 = https_probe(phone, "https://github.com/")
        record(
            "evil-mitm: HTTPS fails (looks like no internet)",
            p_https2.get("https.status") in ("none", "000", None),
            "status=%s" % p_https2.get("https.status"),
        )
        scoping("evil-mitm: this PC is still online", lan, pc_online_before)
        t.join()

        send(s, "mitm stop", 2.0)
        line = last(grab(s, 1.0), "mitm stop")
        record("mitm stop clears every engine", field(line, "pairs") == "0", line or "")
        back2, back2_at, restored = wait_for_internet(phone)
        record(
            "phone: internet restored after evil-mitm",
            back2,
            "status=%s after<=%ss" % (restored.get("https.status"), back2_at),
        )

        send(s, "errors", 1.5)
        out = grab(s, 1.0)
        crashes = [t for t in out if re.search(r"Guru|panic|abort|Backtrace|assert", t, re.I)]
        record("no crash markers", len(crashes) == 0, "; ".join(crashes[:2]))
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
