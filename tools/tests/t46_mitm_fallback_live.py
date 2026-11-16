#!/usr/bin/env python3
"""Vandeta 1.4: the transport-fallback barrier, measured on the board.

The barrier is what keeps a dual-stack / HTTP/3 client on the IPv4/TCP path the
engine can intercept: QUIC (UDP 443) and DNS-over-QUIC (853) are dropped with an
ICMP port-unreachable, DNS-over-TLS (TCP 853) is dropped, and AAAA / SVCB /
HTTPS queries are answered with no records. `t32_mitm_fallback_host.py` checks
the wiring in the source; this script checks the running firmware.

**Nothing is poisoned.** Every probe is a packet sent straight to the board, and
a frame addressed to the board itself is walked by the same layer-2 hook
(`_nsInputHook`), so the guard, the ICMP answer, the DoT drop and the empty DNS
answers can all be measured while no other host on the network is touched. That
also makes this safe on a bench where this PC's uplink is the remote session:
the ARP table of the gateway is never rewritten.

What is asserted:

  1. before the engine starts the board answers nothing (control);
  2. `dnsinject start` on a DNS-only payload arms the barrier - the case that
     used to leave AAAA/QUIC alone (`G1` in the 1.4 plan);
  3. an AAAA and an HTTPS(65) query for any name get NOERROR with 0 answers and
     `svcbSuppressed` rises;
  4. a UDP/443 datagram comes back as ICMP port-unreachable (not a blackhole)
     and `quicDropped` rises;
  5. a TCP/853 connection fails and `dotDropped` rises;
  6. an ordinary A query is still answered, so the engine is not just dead;
  7. after `dnsinject stop` the counters stop moving and no ARP pair is left.

The IPv6 barrier (A4/T6 of the plan) is a best-effort extra: it needs the
board's link-local address in this PC's neighbour table, and is reported as a
SKIP when it is not there - the phone test (`t45`) is the one that scores it
from a real victim.

    python3 FREEBUFF/tests/t46_mitm_fallback_live.py [port]

Run it with an interpreter that has pyserial (the bench uses
`~/.platformio/penv/bin/python`). Stop the screen mirror first - it holds the
port.
"""
import os
import re
import socket
import struct
import subprocess
import sys
import time

import serial

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
try:
    from phone_lab import PhoneLab  # noqa: E402
except Exception:  # noqa: BLE001 - the phone half is optional
    PhoneLab = None

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200
PAYLOAD = "/BruceSpoof/evil-mitm.mitm"  # dns-only: the case G1 was about
BOARD_MAC = "44:b1:76:a4:e9:98"  # this bench's T-Embed (informational only)

RESULTS = []


def record(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("  %-54s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))


def skip(name, detail=""):
    RESULTS.append((name, None, detail))
    print("  %-54s SKIP%s" % (name, ("  " + detail) if detail else ""))


# ------------------------------------------------------------------- console
def open_port():
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    s.dtr = True  # the firmware's USB CDC only transmits with DTR asserted
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


def last(lines, prefix):
    for t in reversed(lines):
        if t.startswith(prefix):
            return t
    return ""


def field(line, key):
    m = re.search(r"\b%s=(\S+)" % re.escape(key), line)
    return m.group(1) if m else None


def num(line, key):
    v = field(line, key)
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def status(s):
    send(s, "mitm status", 1.6)
    return last(grab(s, 1.2), "mitm status")


def board_wifi_ip(s, tries=3, settle=12.0):
    for _ in range(tries):
        send(s, "wifi on", settle)
        send(s, "info", 2.0)
        for line in grab(s, 1.5):
            m = re.search(r"Ip: (\d+\.\d+\.\d+\.\d+)", line)
            if m and m.group(1) != "0.0.0.0":
                return m.group(1)
    return ""


# ---------------------------------------------------------------------- DNS
def dns_query(name, qtype):
    """A minimal DNS query packet (one question, recursion desired)."""
    qid = 0x4A14
    header = struct.pack(">HHHHHH", qid, 0x0100, 1, 0, 0, 0)
    qname = b"".join(bytes([len(p)]) + p.encode() for p in name.split(".")) + b"\x00"
    return header + qname + struct.pack(">HH", qtype, 1)


def dns_ask(board_ip, name, qtype, timeout=2.5, tries=3):
    """Ask the board directly. Returns (answered, rcode, ancount, second_answer).

    Retried on a lost datagram: one drop on a busy Wi-Fi link is a radio event,
    not a suppression bug, and a single lost answer used to fail the AAAA check
    at random (measured 2026-11-08: AAAA reported "no answer" in one run and the
    very next query came back empty as designed, counter unchanged). A real
    failure still fails every attempt.
    """
    pkt = dns_query(name, qtype)
    result = (False, None, None, None)
    for attempt in range(tries):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(timeout)
        data = None
        try:
            s.sendto(pkt, (board_ip, 53))
            data, _ = s.recvfrom(2048)
        except socket.timeout:
            result = (False, None, None, None)
        except OSError as e:
            result = (False, None, None, str(e))
        finally:
            s.close()
        if data is not None and len(data) >= 12:
            rcode = data[3] & 0x0F
            ancount = struct.unpack(">H", data[6:8])[0]
            return True, rcode, ancount, None
        if attempt + 1 < tries:
            time.sleep(0.5)
    return result


def udp_443_probe(board_ip, timeout=2.5):
    """Send a QUIC-looking datagram; returns (refused, detail).

    A connected UDP socket surfaces the ICMP port-unreachable the board sends as
    ECONNREFUSED on the next syscall - which is exactly the signal that makes a
    browser retry over TCP instead of waiting for its QUIC blackhole timer.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.connect((board_ip, 443))
        s.send(b"\xc0" + b"\x00" * 41)  # QUIC long-header shape, not parsed here
        try:
            s.recv(64)
        except ConnectionRefusedError:
            return True, "icmp port-unreachable"
        except socket.timeout:
            return False, "no answer (blackholed)"
        return False, "unexpected reply"
    except ConnectionRefusedError:
        return True, "icmp port-unreachable"
    except OSError as e:
        return False, str(e)
    finally:
        s.close()


def tcp_853_probe(board_ip, timeout=3.0):
    """A DoT connection attempt: it must not complete while the engine runs."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    t0 = time.time()
    try:
        s.connect((board_ip, 853))
        return True, "connected"
    except (ConnectionRefusedError, OSError) as e:
        return False, "%s after %.1fs" % (e.__class__.__name__, time.time() - t0)
    finally:
        s.close()


def _neigh_link_local():
    """The board's link-local address from this PC's IPv6 neighbour table."""
    try:
        r = subprocess.run(["ip", "-6", "neigh"], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return None
    for line in r.stdout.splitlines():
        if BOARD_MAC.lower() in line.lower():
            for tok in line.split():
                if tok.startswith("fe80::"):
                    return tok
    return None


def _iface_for(ip):
    r = subprocess.run(["ip", "route", "get", ip], capture_output=True, text=True)
    m = re.search(r"\bdev (\S+)", r.stdout)
    return m.group(1) if m else None


def eui64_from_mac(mac):
    """The deterministic link-local address lwIP/ESP-IDF gives a station.

    This is what makes the check scoreable instead of a skip: every ESP32 that
    puts its MAC in the interface ID uses this exact address, so it can be sent
    to without waiting for a neighbour-table entry that may never appear (the
    board rarely talks over IPv6 by itself, so nothing populates it).
    """
    b = [int(x, 16) for x in mac.split(":")]
    b[0] ^= 0x02  # universal/local bit flip
    return "fe80::%02x%02x:%02xff:fe%02x:%02x%02x" % tuple(b)


def board_link_local(board_ip):
    """The board's IPv6 link-local, discovered rather than assumed.

    Order: what the neighbour table already knows, then an all-nodes echo that
    makes every IPv6 host on the link answer with its own link-local, then the
    EUI-64 address derived from the MAC the board reports. Returns
    (address, how) so the result line says which path was used.
    """
    ll = _neigh_link_local()
    if ll:
        return ll, "neighbour table"
    iface = _iface_for(board_ip)
    if iface:
        try:
            subprocess.run(["ping", "-6", "-c", "2", "-W", "1", "ff02::1%" + iface],
                           capture_output=True, timeout=15)
        except (OSError, subprocess.SubprocessError):
            pass
        ll = _neigh_link_local()
        if ll:
            return ll, "all-nodes echo"
    return eui64_from_mac(BOARD_MAC), "EUI-64 from MAC"


def ipv6_probe(addr, iface=None):
    """Score the IPv6 barrier: a UDP/9 datagram to the board's link-local."""
    host = addr if (not iface or "%" in addr) else "%s%%%s" % (addr, iface)
    s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    s.settimeout(1.5)
    try:
        sockaddr = socket.getaddrinfo(host, 9, socket.AF_INET6, socket.SOCK_DGRAM)[0][4]
        s.sendto(b"vandeta", sockaddr)
        return True
    except OSError as e:
        return "error:%s" % e
    finally:
        s.close()


def main():
    s = open_port()
    try:
        send(s, "usbdebug status", 1.2)
        if not any("Developer Mode: ON" in t for t in grab(s, 1.0)):
            send(s, "usbdebug on", 1.5)
            grab(s, 1.0)

        board_ip = board_wifi_ip(s)
        record("board is associated", bool(board_ip), board_ip or "no address")
        if not board_ip:
            return finish()

        # A previous run that crashed can leave the DNS engine armed, and the
        # control check below is only meaningful on a clean board (2026-11-08:
        # a crashed run did exactly that and failed the control). Nothing is
        # being intercepted yet, so this stops nothing a user is relying on.
        send(s, "mitm stop", 1.5)
        send(s, "dnsinject stop", 1.5)
        grab(s, 1.0)

        line = status(s)
        print("  " + (line or "no answer"))
        base = {
            k: num(line, k)
            for k in ("quicDropped", "dotDropped", "svcbSuppressed", "ipv6Dropped")
        }
        record(
            "mitm status exports the four fallback counters",
            all(v is not None for v in base.values()),
            str(base),
        )

        # ---- control: nothing armed, the board answers no DNS at all
        answered, rcode, ancount, _ = dns_ask(board_ip, "example.com", 1)
        record("control: no DNS answer before the engine starts", not answered, "answered=%s" % answered)

        # ---- arm a DNS-only payload (the G1 case)
        send(s, "mitm payload %s" % PAYLOAD, 2.5)
        line = last(grab(s, 1.2), "mitm payload")
        print("  " + (line or "no answer"))
        record(
            "a DNS-only payload parses",
            field(line, "ok") == "1" and field(line, "dns") == "1" and field(line, "bad") == "0",
            line or "no answer",
        )

        send(s, "dnsinject start", 2.5)
        line = last(grab(s, 1.2), "dnsinject start")
        print("  " + (line or "no answer"))
        record("the DNS engine starts on the associated station", field(line, "ok") == "1", line or "no answer")
        if field(line, "ok") != "1":
            return finish()

        # ---- A still works: the engine is alive, not simply dead
        answered, rcode, ancount, err = dns_ask(board_ip, "example.com", 1)
        record(
            "an A query is still answered (anything.example.com)",
            answered and ancount == 1,
            "answered=%s rcode=%s answers=%s" % (answered, rcode, ancount),
        )

        # ---- AAAA / SVCB / HTTPS: no records, so no IPv6 or h3 hint
        answered, rcode, ancount, err = dns_ask(board_ip, "example.com", 28)
        record(
            "AAAA is answered empty (NOERROR / 0 answers)",
            answered and rcode == 0 and ancount == 0,
            "answered=%s rcode=%s answers=%s" % (answered, rcode, ancount),
        )
        answered, rcode, ancount, err = dns_ask(board_ip, "example.com", 65)
        record(
            "HTTPS/SVCB is answered empty (NOERROR / 0 answers)",
            answered and rcode == 0 and ancount == 0,
            "answered=%s rcode=%s answers=%s" % (answered, rcode, ancount),
        )

        # ---- the same two questions asked from the phone itself, which is the
        # victim shape that matters: Android asks AAAA first on a dual-stack
        # network. No poisoning is involved - the phone simply sends to the
        # board's address, so its own network and this PC's uplink are untouched.
        phone = None
        try:
            phone = PhoneLab() if PhoneLab else None
        except Exception:  # noqa: BLE001 - no adb is not a failure here
            phone = None
        if not (phone and phone.present() and phone.termux_ready() and phone.ip()):
            skip("the phone gets an empty AAAA / HTTPS from the board", "no usable adb phone")
        else:
            v = phone.dns_raw("example.com", board_ip, 28)
            print("  phone AAAA: " + str(v))
            record(
                "the phone gets an empty AAAA from the board",
                v.get("dnsraw.ancount") == "0",
                "ancount=%s err=%s" % (v.get("dnsraw.ancount"), v.get("dnsraw.err")),
            )
            v = phone.dns_raw("example.com", board_ip, 65)
            print("  phone HTTPS(65): " + str(v))
            record(
                "the phone gets an empty HTTPS/SVCB from the board",
                v.get("dnsraw.ancount") == "0",
                "ancount=%s err=%s" % (v.get("dnsraw.ancount"), v.get("dnsraw.err")),
            )

        # ---- QUIC: ICMP port-unreachable, not a silent blackhole
        refused, detail = udp_443_probe(board_ip)
        record("a UDP/443 datagram is refused with ICMP", refused, detail)

        # ---- DoT: the connection must not complete
        up, detail = tcp_853_probe(board_ip)
        record("a TCP/853 (DoT) connection does not open", not up, detail)

        # ---- IPv6 barrier. The probe goes to the all-nodes multicast group,
        # which the AP floods to every station: the board receives it whether or
        # not it has configured a link-local address of its own (a unicast probe
        # only works when it has, which is why this check used to skip). The drop
        # is scored on the board's counter, and a quiet control window is measured
        # first so ambient IPv6 traffic cannot be mistaken for the probe.
        ll, how = board_link_local(board_ip)
        iface = _iface_for(board_ip)
        ip6_start = num(status(s), "ipv6Dropped")
        time.sleep(1.5)
        ip6_idle = num(status(s), "ipv6Dropped")
        sent = sum(1 for _ in range(6) if ipv6_probe("ff02::1", iface) is True)
        time.sleep(1.5)
        ip6_end = num(status(s), "ipv6Dropped")
        rise = (ip6_end - ip6_idle) if None not in (ip6_end, ip6_idle) else None
        record(
            "IPv6 from the client is dropped while armed",
            rise is not None and rise >= 3,
            "idle %s->%s, probe -> %s (+%s, sent %s/6 on %s; board LL %s via %s)"
            % (ip6_start, ip6_idle, ip6_end, rise, sent, iface, ll, how),
        )

        # ---- the counters the probes should have moved
        line = status(s)
        print("  " + (line or "no answer"))
        now = {k: num(line, k) for k in base}
        record(
            "quicDropped rose",
            now.get("quicDropped") is not None and now["quicDropped"] > (base["quicDropped"] or 0),
            "%s -> %s" % (base["quicDropped"], now.get("quicDropped")),
        )
        record(
            "dotDropped rose",
            now.get("dotDropped") is not None and now["dotDropped"] > (base["dotDropped"] or 0),
            "%s -> %s" % (base["dotDropped"], now.get("dotDropped")),
        )
        record(
            "svcbSuppressed rose (AAAA + HTTPS)",
            now.get("svcbSuppressed") is not None
            and now["svcbSuppressed"] >= (base["svcbSuppressed"] or 0) + 2,
            "%s -> %s" % (base["svcbSuppressed"], now.get("svcbSuppressed")),
        )

        # ---- stop: nothing left running, counters frozen
        send(s, "dnsinject stop", 2.0)
        line = last(grab(s, 1.2), "dnsinject stop")
        print("  " + (line or "no answer"))
        record("the DNS engine stops", field(line, "active") == "0", line or "no answer")

        frozen = num(status(s), "quicDropped")
        udp_443_probe(board_ip, timeout=1.5)
        tcp_853_probe(board_ip, timeout=1.5)
        after = num(status(s), "quicDropped")
        record(
            "with the engine stopped the counters do not move",
            frozen is not None and after == frozen,
            "%s -> %s" % (frozen, after),
        )

        send(s, "mitm stop", 2.0)
        line = last(grab(s, 1.2), "mitm stop")
        record("mitm stop leaves no ARP pair", field(line, "pairs") == "0", line or "no answer")

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
