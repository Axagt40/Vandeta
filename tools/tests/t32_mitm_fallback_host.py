#!/usr/bin/env python3
"""1.4: host check for the MITM transport-fallback barrier.

The barrier lives in src/modules/wifi/net_spoof.cpp, which cannot be compiled on
the host (it includes esp_wifi/lwip). This script extracts the real
`_mitmFallbackGuard()` from that file, compiles it against stub flags, and then
checks the source-level wiring that only a device could otherwise exercise:

  * the guard is true for a DNS-only, an HTTP-only and an HTTPS-only rule set,
    and false when nothing is armed;
  * QUIC/DoQ (UDP 443, 853), DNS-over-TLS (TCP 853) and AAAA/SVCB/HTTPS
    (qtype 28/64/65) all consult it;
  * a dropped datagram is answered with ICMP port-unreachable instead of being
    blackholed;
  * IPv6 frames are dropped while it is armed;
  * the counters exist and are exported.

Extraction, not a re-implementation: if the C++ changes, this test sees it.
No device, no network.

    python3 FREEBUFF/tests/t32_mitm_fallback_host.py
"""

import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC = os.path.join(ROOT, "src", "modules", "wifi", "net_spoof.cpp")
HDR = os.path.join(ROOT, "src", "modules", "wifi", "net_spoof.h")

HARNESS = r"""
#include <stdbool.h>
#include <stdio.h>

static bool s_httpActive = false;
static bool s_httpsActive = false;
static bool s_dnsActive = false;

%s

int main(void) {
    const char *names[3] = {"http", "https", "dns"};
    bool flags[3];
    for (int combo = 0; combo < 8; combo++) {
        flags[0] = (combo & 1) != 0;
        flags[1] = (combo & 2) != 0;
        flags[2] = (combo & 4) != 0;
        s_httpActive = flags[0];
        s_httpsActive = flags[1];
        s_dnsActive = flags[2];
        printf("%%d%%d%%d %%d\n", flags[0], flags[1], flags[2], _mitmFallbackGuard() ? 1 : 0);
    }
    return 0;
}
"""


def grab_guard(text):
    m = re.search(r"static\s+bool\s+_mitmFallbackGuard\s*\(\s*\)\s*\{", text)
    if not m:
        sys.exit("_mitmFallbackGuard() not found in %s" % SRC)
    i = text.find("{", m.start())
    depth = 0
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[m.start(): i + 1]
        i += 1
    sys.exit("unbalanced braces in _mitmFallbackGuard")


def main():
    src = open(SRC, encoding="utf-8").read()
    hdr = open(HDR, encoding="utf-8").read()
    func = grab_guard(src)

    path = "/tmp/t32_mitm_fallback.cpp"
    open(path, "w", encoding="utf-8").write(HARNESS % func)
    cc = subprocess.run(
        ["g++", "-std=c++17", "-Wall", "-Wextra", "-o", "/tmp/t32_mitm_fallback", path],
        capture_output=True,
        text=True,
    )
    if cc.returncode != 0:
        print(cc.stderr)
        sys.exit("compile failed")

    out = subprocess.run(["/tmp/t32_mitm_fallback"], capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit("harness failed: %s" % out.stderr)
    table = {}
    for line in out.stdout.strip().splitlines():
        bits, value = line.split()
        table[bits] = value == "1"

    failures = []
    checks = []

    def check(name, ok, detail=""):
        checks.append(name)
        print(("PASS  " if ok else "FAIL  ") + name + (("  (" + detail + ")") if detail else ""))
        if not ok:
            failures.append(name)

    # --- the truth table of the extracted function -------------------------
    check("nothing armed -> barrier off", table.get("000") is False, str(table.get("000")))
    check("DNS-only (dnsSpoof / `dns * <ip>`) -> barrier on", table.get("001") is True, str(table.get("001")))
    check("HTTP-only -> barrier on", table.get("100") is True, str(table.get("100")))
    check("HTTPS-only -> barrier on", table.get("010") is True, str(table.get("010")))
    check("observe is not a term of the guard", "s_observe" not in func)

    # --- the wiring -------------------------------------------------------
    uses = len(re.findall(r"_mitmFallbackGuard\s*\(\s*\)", src)) - 1  # minus the definition
    check("guard is consulted at least 3 times", uses >= 3, "%d use(s)" % uses)

    check(
        "QUIC/DoQ drop is gated by the barrier",
        re.search(r"dstPort\s*==\s*443\s*\|\|\s*dstPort\s*==\s*853\)\s*&&\s*_mitmFallbackGuard\(\)", src)
        is not None,
    )
    check(
        "DoT drop is gated by the barrier",
        re.search(r"dstPort\s*==\s*853\s*&&\s*_mitmFallbackGuard\(\)", src) is not None,
    )
    check(
        "AAAA/SVCB/HTTPS suppression is gated by the barrier",
        re.search(
            r"qtype\s*==\s*28\s*\|\|\s*qtype\s*==\s*64\s*\|\|\s*qtype\s*==\s*65\)\s*&&\s*_mitmFallbackGuard\(\)",
            src,
        )
        is not None,
    )
    check(
        "dropped datagrams get ICMP port-unreachable",
        "_sendIcmpPortUnreachable(scratch + 6" in src and "icmp[0] = 3;" in src and "icmp[1] = 3;" in src,
    )
    check("QUIC drops are counted", "s_quicDropped++" in src)
    check("DoT drops are counted", "s_dotDropped++" in src)
    check("suppressed answers are counted", "s_svcbSuppressed++" in src)
    check(
        "IPv6 from the victim is dropped while armed",
        re.search(r"etherType\s*==\s*0x86DD", src) is not None and "s_ipv6Dropped++" in src,
    )

    for name, getter in (
        ("quic", "netspoof_quic_dropped"),
        ("dot", "netspoof_dot_dropped"),
        ("svcb", "netspoof_svcb_suppressed"),
        ("ipv6", "netspoof_ipv6_dropped"),
    ):
        check("%s counter is exported" % name, getter + "()" in src and getter + "();" in hdr)

    print("\n%d passed, %d failed" % (len(checks) - len(failures), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
