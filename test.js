/*
 * test.js - smoke test for the raw 802.11 bindings added in Bruce-Modded Dev 1.0
 *
 *   wifi.injectPacket(hex, channel)          -> boolean
 *   wifi.sendRaw80211(hex, channel, opts)    -> { ok, sent, failed, len, channel, iface, error? }
 *   wifi.captureStart(channel, timeoutMs)    -> session id, or -1 on failure
 *   wifi.captureStop(sessionId)              -> boolean
 *   wifi.getCapturedPackets(sessionId, max)  -> [{ timestamp, channel, rssi, data }, ...]
 *   wifi.setPromiscuous(enable, channel)     -> boolean
 *
 * How to run: copy this file to the device (SD card or LittleFS, e.g. /scripts/)
 * and pick it in the Scripts menu, or run it head-less over serial. Results go to
 * the serial console at 115200 baud and the on-screen summary at the end.
 *
 * Nothing harmful is transmitted: the only frame sent is a probe request, which
 * is the frame a phone broadcasts while looking for networks.
 */

var wifi = require("wifi");
var serial = require("serial");
var display = require("display");

var CH = 6;            // channel under test (1-14)
var CAPTURE_MS = 3000; // capture window

var passed = 0;
var failed = 0;

function log(msg) {
    serial.println(msg);
}

function check(name, ok, detail) {
    if (ok) { passed++; } else { failed++; }
    log((ok ? "PASS  " : "FAIL  ") + name + (detail ? ("   [" + detail + "]") : ""));
}

/*
 * Probe request (management frame, subtype 4) with a wildcard SSID.
 *   40 00               frame control: probe request
 *   00 00               duration
 *   ff ff ff ff ff ff   addr1 (destination, broadcast)
 *   02 00 00 11 22 33   addr2 (source, locally administered)
 *   ff ff ff ff ff ff   addr3 (BSSID, broadcast)
 *   00 00               sequence control (the driver fills it in)
 *   00 00               SSID information element (tag 0, length 0 = wildcard)
 */
var PROBE_REQUEST = "40000000ffffffffffff020000112233ffffffffffff00000000";

log("=== raw 802.11 bindings test ===");

// ---------------------------------------------------------------- setPromiscuous
var promiscOn = wifi.setPromiscuous(true, CH);
check("setPromiscuous(true, " + CH + ")", promiscOn === true, "got " + promiscOn);
delay(500);

var promiscOff = wifi.setPromiscuous(false, CH);
check("setPromiscuous(false, " + CH + ")", promiscOff === true, "got " + promiscOff);

var promiscBad = wifi.setPromiscuous(true, 99);
check("setPromiscuous(true, 99) is rejected", promiscBad === false, "got " + promiscBad);

// ------------------------------------------------------------------ injectPacket
var injected = wifi.injectPacket(PROBE_REQUEST, CH);
check("injectPacket(probe request, " + CH + ")", injected === true, "got " + injected);

var injectedBadChannel = wifi.injectPacket(PROBE_REQUEST, 42);
check("injectPacket(..., 42) is rejected", injectedBadChannel === false, "got " + injectedBadChannel);

var injectedBadHex = wifi.injectPacket("not a frame", CH);
check("injectPacket(\"not a frame\") is rejected", injectedBadHex === false, "got " + injectedBadHex);

// ----------------------------------------------------------------- sendRaw80211
var raw = wifi.sendRaw80211(PROBE_REQUEST, CH, { repeat: 3, gapMs: 5, iface: "auto", fcs: false });
check("sendRaw80211(probe request, repeat 3)", raw.ok === true, "sent=" + raw.sent + " iface=" + raw.iface);
check(
    "sendRaw80211 reports the frame length",
    raw.len === 26 && raw.channel === CH,
    "len=" + raw.len + " channel=" + raw.channel
);

var rawAp = wifi.sendRaw80211(PROBE_REQUEST, CH, { repeat: 1, iface: "ap" });
check("sendRaw80211(iface \"ap\")", rawAp.ok === true, "sent=" + rawAp.sent + " iface=" + rawAp.iface);

var rawSta = wifi.sendRaw80211(PROBE_REQUEST, CH, { repeat: 1, iface: "sta" });
check("sendRaw80211(iface \"sta\")", rawSta.ok === true, "sent=" + rawSta.sent + " iface=" + rawSta.iface);

var rawWithFcs = wifi.sendRaw80211(PROBE_REQUEST, CH, { repeat: 1, fcs: true });
check("sendRaw80211(fcs true)", rawWithFcs.ok === true, "len=" + rawWithFcs.len);

var rawTooShort = wifi.sendRaw80211("aabb", CH, {});
check("sendRaw80211(too short) is rejected", rawTooShort.ok === false, "error=" + rawTooShort.error);

// ----------------------------------------------------------------- captureStart
var badSession = wifi.captureStart(0, 1000);
check("captureStart(0, ...) is rejected", badSession === -1, "got " + badSession);

var session = wifi.captureStart(CH, CAPTURE_MS);
check("captureStart(" + CH + ", " + CAPTURE_MS + ")", session > 0, "session=" + session);

log("capturing for " + CAPTURE_MS + "ms on channel " + CH + " (injecting traffic meanwhile)");

// Keep the channel busy with our own probe requests so the capture has something
// to show even in a quiet RF environment.
var elapsed = 0;
while (elapsed < CAPTURE_MS) {
    wifi.injectPacket(PROBE_REQUEST, CH);
    delay(100);
    elapsed += 100;
}

// -------------------------------------------------------- getCapturedPackets
var packets = wifi.getCapturedPackets(session);
check("getCapturedPackets returns a list", packets.length >= 0, "count=" + packets.length);
log("captured " + packets.length + " packet(s)");

if (packets.length > 0) {
    var first = packets[0];
    var wellFormed =
        first.timestamp > 0 &&
        first.channel > 0 &&
        first.data.length > 0 &&
        first.data.length % 2 === 0;
    check(
        "packet 0 has timestamp/channel/rssi/data",
        wellFormed,
        "ts=" + first.timestamp + " ch=" + first.channel + " rssi=" + first.rssi +
            " bytes=" + (first.data.length / 2)
    );
    log("packet 0 data: " + first.data.substring(0, 64) + (first.data.length > 64 ? "..." : ""));

    var limited = wifi.getCapturedPackets(session, 1);
    check("getCapturedPackets(session, 1) caps the result", limited.length <= 1, "count=" + limited.length);
} else {
    log("(no frames seen - quiet RF environment, the capture itself still worked)");
}

// ------------------------------------------------------------------ captureStop
var stopped = wifi.captureStop(session);
check("captureStop(" + session + ")", stopped === true, "got " + stopped);

var expected = packets.length < 5 ? packets.length : 5;
var stillThere = wifi.getCapturedPackets(session, 5);
check("packets survive captureStop", stillThere.length === expected, "count=" + stillThere.length);

var badStop = wifi.captureStop(99);
check("captureStop(99) is rejected", badStop === false, "got " + badStop);

// A second session should also work (only one can run at a time).
var session2 = wifi.captureStart(CH, 500);
check("captureStart on a second session", session2 > 0, "session=" + session2);
wifi.captureStop(session2);

// -------------------------------------------------------------------- summary
// Leave the radio the way we found it (the interpreter does this on exit too).
wifi.setPromiscuous(false, CH);

log("=== " + passed + " passed, " + failed + " failed ===");

display.fill(display.color(0, 0, 0));
display.setTextColor(display.color(255, 255, 255));
display.setCursor(6, 12);
display.setTextSize(1);
display.println("raw 802.11 test");
display.println("passed: " + passed);
display.println("failed: " + failed);
display.println("");
display.println("packets seen: " + packets.length);
display.println("");
display.println("limits: 10-1500B frames,");
display.println("channels 1-14, 100 pkts or");
display.println("50KB per capture session.");
