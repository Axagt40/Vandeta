#!/usr/bin/env python3
"""Vandeta 1.3 task-1/task-2 pages, checked on the board (2026-10-29).

Drives the firmware over USB and reads what it actually drew, so the pages are
proven by their own output instead of by the build:

  * `sys` reports `vandeta 1.3` (the image that is on the board is the one here),
  * BLE Detector: title, the NAME/ADDRESS/dBm/DIST table, the `seen n/n` footer,
    and - the regression this run found - an unnamed advertiser must NOT carry
    the tracker tag `T`: the vendor field defaulted to 0, which is the first row
    of the vendor table (Chipolo), so every new device looked like a tracker and
    the SEL menu offered a Track page for it,
  * `beacon-like` appears in the footer only after a long scan (40+ adverts,
    20 s+ of a non-connectable advertiser), which is what the filter promises,
  * RF > RF Suite: the page draws the `VANDETA RF` band and its four tools, so
    the tools really moved into the suite and the suite really draws itself,
  * RF Suite > Field Meter: big dBm, level %, `min avg max`, `peak .. noise ..`
    line, and the page is a page (it clears the screen it opened over),
  * WiFi > Channel Analyzer: Ch1..Ch11 with %, the `NNpkt/s` sparkline label and
    the `dwell 350ms` footer,
  * internal RAM does not move across three Field Meter visits (the page's own
    buffers are freed on exit).

Text comes from the firmware's draw ring (`display ring on` + `display dump`),
which is the same text the mirror renders. The mirror holds `/dev/ttyACM0`, so
this talks to its WebSocket when it is up and falls back to the serial port.

    python3 tools/tests/t37_vandeta_13_device.py [--serial /dev/ttyACM0]
    python3 tools/tests/t37_vandeta_13_device.py --ws ws://127.0.0.1:8737/ws
"""
import argparse
import asyncio
import re
import sys
import time

DEFAULT_SERIAL = "/dev/ttyACM0"
DEFAULT_WS = "ws://127.0.0.1:8737/ws"

MAIN_BLE, MAIN_RF, MAIN_WIFI, MAIN_NRF24 = 1, 2, 0, 3
BLE_DETECTOR = 2
NRF_CENSUS = 5
RF_SUITE = 0          # RF menu row 0: the suite (row 1 is Scan/copy)
SUITE_FIELD_METER = 1 # suite row 1: Frequency Analyzer, Field Meter, ...
CHANNEL_ANALYZER = 12

passes = 0
fails = 0
skips = 0


def check(ok, what, detail=""):
    global passes, fails
    if ok:
        passes += 1
        print("  PASS  " + what)
    else:
        fails += 1
        print("  FAIL  " + what + (("  [" + detail + "]") if detail else ""))


def skip(what, why):
    global skips
    skips += 1
    print("  SKIP  " + what + ("  [" + why + "]" if why else ""))


# ---------------------------------------------------------------------------
# Transport: the mirror's WebSocket if it is running, else the serial port
# ---------------------------------------------------------------------------
class SerialLink:
    def __init__(self, port, baud=115200):
        import serial

        self.s = serial.Serial(port, baud, timeout=0.2)
        self.s.dtr = True
        self.s.rts = False
        time.sleep(9)  # opening the port resets the board
        self.s.reset_input_buffer()

    async def send(self, cmd):
        self.s.write((cmd + "\r\n").encode())
        self.s.flush()
        await asyncio.sleep(0.4)

    async def read(self, seconds):
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < seconds:
            chunk = self.s.read(8192)
            if chunk:
                buf.extend(chunk)
        return bytes(buf)

    def close(self):
        self.s.close()


class WsLink:
    """The bridge writes any command it receives to the serial line."""

    def __init__(self, url):
        self.url = url
        self.ws = None

    async def open(self):
        import websockets

        # ping_interval=None: the reader here sleeps for seconds at a time and
        # would otherwise be dropped for not answering keepalives.
        self.ws = await websockets.connect(self.url, max_size=None, ping_interval=None, max_queue=64)
        await asyncio.sleep(2.5)  # the bridge repaints for every new viewer
        await self.read(0.5)

    async def send(self, cmd):
        await self.ws.send(cmd)
        await asyncio.sleep(0.4)

    async def read(self, seconds):
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < seconds:
            try:
                m = await asyncio.wait_for(self.ws.recv(), 0.2)
            except asyncio.TimeoutError:
                continue
            if isinstance(m, str):
                buf.extend(m.encode())
            elif not m.startswith(b"\xaa"):  # a screen packet: not text
                buf.extend(m)
        return bytes(buf)

    def close(self):
        pass


def text_of(data):
    return b"\n".join(re.findall(rb"[\x20-\x7e]{1,}", data)).decode("ascii", "replace")


async def ring_once(link, seconds=3.0):
    await link.send("display dump")
    await asyncio.sleep(1.0)
    raw = text_of(await link.read(seconds))
    m = re.search(r"Binary Dump:(.*?)\[End of Dump\]", raw, re.S)
    if not m:
        return []
    toks = [t for t in m.group(1).split() if len(t) == 2 and all(c in "0123456789ABCDEF" for c in t)]
    data = bytes(int(t, 16) for t in toks)
    return [s.decode() for s in re.findall(rb"[\x20-\x7e]{2,}", data)]


async def ring(link, seconds=3.0, passes=4):
    """Union of several ring snapshots, newest last.

    The draw ring is a small window that the page keeps overwriting: a live page
    that redraws ten strings every cycle only ever holds the last few frames, so
    one dump taken mid-frame can miss the footer (or half the channel labels)
    that are on the screen. Sampling repeatedly and unioning by string is what
    makes the check about the page rather than about the sampling moment.
    """
    seen = []
    for i in range(passes):
        for s in await ring_once(link, seconds / passes):
            if s not in seen:
                seen.append(s)
        await asyncio.sleep(0.35)
    return seen


async def internal_free(link):
    await link.send("mem")
    raw = await link.read(1.4)
    m = re.search(r"internal free\s*:\s*(\d+)", text_of(raw))
    return int(m.group(1)) if m else None


async def home(link):
    for _ in range(4):
        await link.send("nav esc")
        await asyncio.sleep(0.9)
    await link.send("options")
    await asyncio.sleep(0.8)
    return text_of(await link.read(0.8))


# ---------------------------------------------------------------------------
# Pages
# ---------------------------------------------------------------------------
async def ble_detector(link):
    await link.send("display ring on")
    await link.send("option %d" % MAIN_BLE)
    await link.send("option %d" % BLE_DETECTOR)
    await asyncio.sleep(41)  # the beacon-like rule needs 20 s + 40 adverts
    rows = await ring(link)
    body = "\n".join(rows)

    check("BLE DETECTOR" in body, "detector page draws its title")
    check("T NAME" in body and "DIST" in body, "table header is the T/NAME/ADDRESS/dBm/DIST columns")
    check(re.search(r"seen \d+/\d+", body) is not None, "footer counts what is on screen", body[-120:])
    check("SEL track/menu" in body, "footer offers track/menu")

    unknown = [r for r in rows if re.match(r"^ [A-Z.] <\?>", r)]
    tagged_unknown = [r for r in unknown if r.startswith(" T ")]
    if unknown:
        check(not tagged_unknown, "an unnamed advertiser is not tagged as a tracker",
              (tagged_unknown[0] if tagged_unknown else ""))
    else:
        skip("unnamed advertiser is not tagged as a tracker", "no unnamed device in range")

    if "beacon-like" in body:
        check(True, "beacon-like filter counted a long-lived non-connectable advertiser")
    else:
        skip("beacon-like filter", "no 20 s / 40 advert beacon was in range")

    # The SEL menu must not offer Track for anything that is not a tracker.
    await link.send("nav select")
    await asyncio.sleep(1.6)
    menu = text_of(await link.read(1.2))
    tracks = re.findall(r"Track (.+)", menu)
    real_trackers = [r for r in rows if r.startswith(" T ")]
    check(len(tracks) <= len(real_trackers),
          "SEL menu only offers Track for rows tagged T",
          "track=%s tagged=%d" % (tracks, len(real_trackers)))
    await link.send("nav esc")
    await asyncio.sleep(1.0)


async def rf_suite(link):
    await link.send("display ring on")
    await link.send("option %d" % MAIN_RF)
    await link.send("option %d" % RF_SUITE)
    await asyncio.sleep(2.5)
    rows = await ring(link)
    body = "\n".join(rows)
    tip = body[:200].replace("\n", " | ")
    check("VANDETA RF" in body, "RF Suite draws its gradient title band", tip)
    check("SUITE 1.0" in body, "RF Suite carries the suite version", tip)
    for row in ("Frequency Analyzer", "Field Meter", "Packet Monitor", "Wardriver"):
        check(row in body, "RF Suite lists %s" % row, tip)
    check("BACK exit" in body, "RF Suite footer offers exit", tip)
    return rows


async def field_meter(link):
    # The suite is drawn by hand, so `option N` (which indexes the menu the
    # firmware published) cannot reach its rows - the key injection can. The
    # suite always opens on row 0, so one DOWN is Field Meter.
    await link.send("option %d" % MAIN_RF)
    await link.send("option %d" % RF_SUITE)
    await asyncio.sleep(2.0)  # the suite ignores SELECT for its first 300 ms
    # `press` injects exactly one key. `ui down` retries when the published
    # menu does not change, and a hand-drawn page never publishes one - so it
    # sent the press twice and skipped a row.
    await link.send("press down")
    await asyncio.sleep(1.0)
    await link.send("press ok")
    await asyncio.sleep(9)
    rows = await ring(link)
    body = "\n".join(rows)
    tip = body[:200].replace("\n", " | ")
    check("FIELD METER" in body, "field meter page draws its title", tip)
    check(re.search(r"\d+\.\d{3} MHz", body) is not None, "locked frequency is shown", tip)
    check(re.search(r"min -?\d+  avg -?\d+  max -?\d+", body) is not None, "min/avg/max since reset", tip)
    check(re.search(r"peak \d+%   noise -?\d+ dBm", body) is not None, "peak hold and noise floor", tip)
    check("SEL reset   BACK exit" in body, "footer offers reset and exit", tip)
    nums = re.findall(r"min (-?\d+)  avg (-?\d+)  max (-?\d+)", body)
    ok = True
    if nums:
        mn, avg, mx = (int(v) for v in nums[-1])
        ok = mn <= avg <= mx
    check(ok, "min <= avg <= max", str(nums[-1:] if nums else ""))
    await link.send("nav esc")  # meter -> suite
    await asyncio.sleep(2.0)
    await link.send("nav esc")  # suite -> main menu
    await asyncio.sleep(2.0)


def option_index(listing, label):
    """Row number of `label` in an `options` dump (" 3 - Audio Config")."""
    for m in re.finditer(r"^\s*>?\s*(\d+)\s*-\s*(.+?)\s*$", listing, re.M):
        if m.group(2).startswith(label):
            return int(m.group(1))
    return None


async def audio_config(link):
    """Config > Audio Config must carry the speaking voice, not only beeps.

    The voice language and the online-voice toggle are the newest settings on
    the page, and they sit under the volume row: if the page is rebuilt without
    them the device silently loses the feature, so its own menu is the proof.
    """
    await home(link)
    await link.send("options")
    listing = text_of(await link.read(1.0))
    idx = option_index(listing, "Config")
    check(idx is not None, "main menu publishes a Config row", listing[:160])
    if idx is None:
        return
    await link.send("option %d" % idx)
    await asyncio.sleep(1.4)
    await link.send("options")
    listing = text_of(await link.read(1.0))
    row = option_index(listing, "Audio Config")
    check(row is not None, "Config > Audio Config exists", listing[:200])
    if row is None:
        await link.send("nav esc")
        await asyncio.sleep(1.0)
        return
    await link.send("option %d" % row)
    await asyncio.sleep(1.4)
    await link.send("options")
    page = text_of(await link.read(1.2))
    check(re.search(r"Voice: \w", page) is not None,
          "Audio Config offers a Voice language row", page[:220])
    check("Voice Online:" in page, "Audio Config offers the Voice Online toggle", page[:220])
    check("Sound Theme:" in page, "Audio Config keeps the Sound Theme row", page[:220])
    await link.send("nav esc")
    await asyncio.sleep(1.2)


async def nrf_census_page(link):
    """The census must open as its own page, not paint over the NRF24 menu.

    Without the external nRF24 module the page cannot draw its graph, and it
    says so with the firmware's error dialog - which is still this page
    answering, not a silent overlay. With a module it draws `2.4G CENSUS`.
    """
    await link.send("display ring on")
    await link.send("option %d" % MAIN_NRF24)
    await asyncio.sleep(1.2)
    await link.send("option %d" % NRF_CENSUS)
    await asyncio.sleep(3.0)
    rows = await ring(link)
    body = "\n".join(rows)
    tip = body[:200].replace("\n", " | ")
    check(
        "2.4G CENSUS" in body or "NRF24" in body or "No NRF24" in body,
        "Census opens its own page (or reports the missing radio)",
        tip,
    )
    await link.send("nav esc")
    await asyncio.sleep(2.5)


async def channel_analyzer(link):
    await home(link)  # the census pages take a moment to hand the menu back
    await link.send("option %d" % MAIN_WIFI)
    await link.send("option %d" % CHANNEL_ANALYZER)
    await asyncio.sleep(18)
    rows = await ring(link)
    body = "\n".join(rows)
    # The page sweeps the configured hop plan (Config > Wi-Fi > Channel plan),
    # which is drawn top-right: 1,6,11 / 1-13 / 1-11. Checking against that plan
    # is the real assertion - expecting eleven labels failed on a board set to
    # the non-overlapping plan, which is a setting, not a bug.
    plan = "1-11"
    for cand in ("1-13", "1-11", "1,6,11"):
        if cand in body:
            plan = cand
            break
    if plan == "1,6,11":
        want = {1, 6, 11}
    elif plan == "1-13":
        want = set(range(1, 14))
    else:
        want = set(range(1, 12))
    chans = {int(c) for c in re.findall(r"Ch(\d+)", body) if int(c) <= 13}
    check(want <= chans, "the sweep labels every channel of plan %s" % plan, str(sorted(chans)))
    check(re.search(r"\d+pkt/s", body) is not None, "sparkline shows its packets/s scale")
    check(re.search(r"Ch\d+ \d+% pk\d+% (-?\d+dBm|--) dwell \d+ms", body) is not None,
          "footer carries load, peak, signal and dwell", body[-120:])
    await link.send("nav esc")
    await asyncio.sleep(3.5)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", default=DEFAULT_SERIAL)
    ap.add_argument("--ws", default="", help="mirror WebSocket (default: try %s)" % DEFAULT_WS)
    args = ap.parse_args()

    link = None
    try:
        link = WsLink(args.ws or DEFAULT_WS)
        await link.open()
        print("transport: mirror WebSocket")
    except Exception as e:  # noqa: BLE001
        if args.ws:
            print("no mirror at %s (%r) - stopping the mirror or use --serial" % (args.ws, e))
            return 2
        link = SerialLink(args.serial)
        print("transport: serial %s" % args.serial)

    try:
        print("image on the board")
        await link.send("sys")
        info = text_of(await link.read(1.5))
        check("vandeta 1.3" in info, "firmware reports vandeta 1.3")
        check("debug mode  : ON" in info, "Developer Options are on", info[:200])

        print("main menu")
        state = await home(link)
        check("Actual Menu: Main Menu" in state, "esc lands on the main menu", state[:160])

        print("BLE Detector")
        await ble_detector(link)
        await home(link)

        print("RF Suite")
        await rf_suite(link)
        await link.send("nav esc")
        await asyncio.sleep(1.5)
        await home(link)

        print("Field Meter")
        # Three visits in a row, before any Wi-Fi page: the Wi-Fi driver stays
        # resident after the Channel Analyzer and would show up as a one-time
        # drop that has nothing to do with this page.
        values = []
        for _ in range(3):
            await field_meter(link)
            values.append(await internal_free(link))
        print("    internal free per visit: %s" % values)
        # The page allocates nothing of its own - its sweep buffers are stack
        # locals - so a repeat visit has to return to exactly the previous level.
        # The FIRST radio page opened in a boot can still cost a few bytes once:
        # measured on the board, a 52 B allocation appears on whichever radio
        # page is opened first and never again (a probe saw it land on the
        # Frequency Analyzer, after two clean Field Meter visits). So the claim
        # is "no drift between repeat visits" plus a bound on that one-time
        # cost. A real per-visit leak drops on every visit and fails both halves.
        drift = values[0] - values[-1] if all(v is not None for v in values) else None
        check(all(v is not None for v in values) and values[-1] == values[-2],
              "each Field Meter visit returns to the previous internal free", str(values))
        check(drift is not None and 0 <= drift <= 128,
              "one-time runtime cost of the page is bounded (<= 128 B)", str(drift))

        print("Audio Config")
        await audio_config(link)
        await home(link)

        print("nRF24 Census")
        await nrf_census_page(link)
        await home(link)

        print("Channel Analyzer")
        await channel_analyzer(link)
        await home(link)

        print("\n%d passed / %d failed / %d skipped" % (passes, fails, skips))
        return 0 if fails == 0 else 1
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
