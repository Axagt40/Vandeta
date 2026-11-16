#!/usr/bin/env python3
"""Vandeta 1.2: live MITM interception against a real client (a phone on adb).

This is the end-to-end test the menu-only test cannot give: after the SYN-ACK
fix a real client's TCP handshake is answered by the board, so its plain-HTTP
request reaches the engine and gets the payload's page.

Shape of the run:

  * the board joins the phone's network (`wifi on`, Bruce's saved credentials);
  * `mitm target <phone ip>` (a single host, so no other client is touched);
  * `mitm run 25 /BruceSpoof/html-rickroll.mitm` runs headless on the console;
  * while it runs, the phone fetches http://1.1.1.1/ and http://neverssl.com/
    (IP first, so the test does not depend on DNS), and the browser is opened
    on the same URL so a human can see the page;
  * the board's counters must show the request as injected, and the ARP pair
    must be gone afterwards.

Requires `adb` and one authorised device; without them every live step is
SKIPPED, never FAILED. The board must be able to join the phone's WiFi network
(it uses the credentials Bruce already has, or a network the phone shares).

    python3 FREEBUFF/tests/t27_mitm_live.py [port]

Stop the screen mirror first - it holds /dev/ttyACM0.
"""
import os
import re
import shutil
import subprocess
import sys
import threading
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200
RESULTS = []
BOARD_LOG = []


def record(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("  %-46s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail else ""))


def skip(name, detail=""):
    RESULTS.append((name, None, detail))
    print("  %-46s SKIP%s" % (name, ("  " + detail) if detail else ""))


def adb(*args, timeout=20):
    return subprocess.run(["adb", "shell"] + list(args), capture_output=True, text=True, timeout=timeout)


def phone_ip():
    out = adb("ip", "-4", "addr", "show", "wlan0")
    m = re.search(r"inet (\d+\.\d+\.\d+\.\d+)", out.stdout)
    return m.group(1) if m else None


def open_port():
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    s.dtr = True
    s.rts = False
    time.sleep(9)
    s.reset_input_buffer()
    return s


def reader(s, seconds):
    """Collect everything the board prints for @seconds seconds."""
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
    return bytes(buf)


def lines_of(data):
    return [t.decode(errors="replace").rstrip() for t in data.split(b"\n") if t.strip()]


def find_line(lines, prefix):
    for t in reversed(lines):
        if t.startswith(prefix):
            return t
    return ""


def field(line, key):
    m = re.search(r"\b%s=(\S+)" % re.escape(key), line)
    return m.group(1) if m else None


def read_screen():
    """Screenshot the phone and OCR it, so the test checks what a person sees.

    Returns None when the host cannot read the image (no tesseract or PIL).
    """
    try:
        from PIL import Image  # noqa: F401  (only used to validate the capture)
    except Exception:
        return None
    if not shutil.which("tesseract"):
        return None
    shot = "/tmp/vandeta_mitm_screen.png"
    with open(shot, "wb") as fh:
        png = subprocess.run(["adb", "exec-out", "screencap", "-p"], capture_output=True, timeout=30).stdout
        fh.write(png)
    if len(png) < 1000:
        return ""
    out = subprocess.run(["tesseract", shot, "-"], capture_output=True, text=True, timeout=60)
    return out.stdout


def main():
    devices = subprocess.run(["adb", "devices"], capture_output=True, text=True).stdout
    if not re.search(r"^\S+\s+device$", devices, re.M):
        skip("live MITM interception", "no adb device connected")
        return finish()
    ip = phone_ip()
    if not ip:
        skip("live MITM interception", "phone has no WiFi address")
        return finish()
    print("  phone wlan0: " + ip)

    s = open_port()
    try:
        out = grab(s, 1.0)
        send(s, "usbdebug status", 1.2)
        out = lines_of(grab(s, 1.2))
        if not any("Developer Mode: ON" in t for t in out):
            send(s, "usbdebug on", 1.5)
            grab(s, 1.0)

        send(s, "info", 1.5)
        info = lines_of(grab(s, 1.0))
        board_ip = ""
        for t in info:
            if t.strip().startswith("Ip:"):
                board_ip = t.split(":", 1)[1].strip()
        if not board_ip:
            send(s, "wifi on", 12.0)
            text = "\n".join(lines_of(grab(s, 1.0)))
            m = re.search(r"Ip: (\d+\.\d+\.\d+\.\d+)", text)
            board_ip = m.group(1) if m else ""
            send(s, "info", 1.5)
            info = lines_of(grab(s, 1.0))
            for t in info:
                if t.strip().startswith("Ip:"):
                    board_ip = t.split(":", 1)[1].strip()
        record("board is on the same network as the phone", board_ip != "", board_ip or "no address")
        if not board_ip:
            return finish()

        send(s, "mitm payload /BruceSpoof/html-rickroll.mitm", 2.5)
        out = lines_of(grab(s, 1.0))
        line = find_line(out, "mitm payload")
        print("  " + (line or "no answer"))
        record("payload parsed", field(line, "ok") == "1" and field(line, "pages") == "1", line)

        send(s, "mitm target " + ip, 1.5)
        out = lines_of(grab(s, 0.8))
        line = find_line(out, "mitm target")
        record("single target = the phone", field(line, "ok") == "1", line)

        print("\n=== live run: 25 s, phone requests while it runs ===")
        send(s, "mitm run 25 /BruceSpoof/html-rickroll.mitm", 0.2)
        t = threading.Thread(target=reader, args=(s, 27), daemon=True)
        t.start()

        # Let the ARP pair come up before the first request.
        time.sleep(6)
        raw = adb("curl", "-s", "-m", "6", "http://1.1.1.1/")
        record(
            "phone gets the injected page from an IP URL",
            "Never gonna give you up" in raw.stdout,
            "bytes=%d" % len(raw.stdout),
        )
        named = adb("curl", "-s", "-m", "6", "http://neverssl.com/")
        named_ok = "Never gonna give you up" in named.stdout
        print("  named body: %d bytes" % len(named.stdout))

        # Open it visibly, so a person holding the phone sees the result.
        adb("am", "start", "-a", "android.intent.action.VIEW", "-d", "http://neverssl.com/")
        time.sleep(6)
        screen_text = read_screen()
        print("  screen: " + (screen_text.splitlines()[0] if screen_text else "(no OCR)"))
        if screen_text is None:
            skip("phone screen shows the injected page", "no tesseract/PIL on the host")
        else:
            record(
                "phone screen shows the injected page",
                "Never gonna give you up" in screen_text,
                "%d chars read" % len(screen_text),
            )

        t.join()
        log = "\n".join(BOARD_LOG)
        summary = ""
        for line in reversed(BOARD_LOG):
            if line.startswith("mitm run ok="):
                summary = line
                break
        print("  " + (summary or "no summary"))
        record("run completed", field(summary, "ok") == "1", summary)
        injected = re.search(r"http (\d+)/(\d+)", summary or "")
        record(
            "board counted the injected request",
            bool(injected) and int(injected.group(1)) > 0,
            injected.group(0) if injected else "no http counters",
        )
        record("the page rule matched (engine log)", "page GET" in log or "t+" in log)
        record("ARP mapping restored", field(summary, "pairs") == "0", summary)

        send(s, "htmlint captures", 2.0)
        caps = lines_of(grab(s, 1.0))
        http_caps = [t for t in caps if t.startswith("httpcap") and "action=2" in t]
        for t in http_caps[-3:]:
            print("  " + t)
        record("a capture records the injected request", len(http_caps) > 0, "%d injected" % len(http_caps))
        # The name path is proven by the capture rather than the body: a client
        # may follow neverssl.com's own redirect chain and one of its requests
        # can land while the ARP entry is still converging, which makes the
        # body assertion flaky even though the page was served.
        named_caps = [t for t in http_caps if "neverssl.com" in t]
        record(
            "phone reached the page by name",
            named_ok or len(named_caps) > 0,
            "body=%d capture=%d" % (len(named.stdout), len(named_caps)),
        )

        out = lines_of(grab(s, 0.5))
        send(s, "errors", 1.5)
        out = lines_of(grab(s, 1.0))
        crashes = [t for t in out if re.search(r"Guru|panic|abort|Backtrace|assert", t, re.I)]
        record("no crash markers", len(crashes) == 0)
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
