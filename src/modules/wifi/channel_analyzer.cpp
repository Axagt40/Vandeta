#if !defined(LITE_VERSION)
#include "channel_analyzer.h"

#include "esp_err.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/odid.h"
#include "core/sound_theme.h"
#include "core/ui_gradient.h"
#include "core/wifi/wifi_common.h"
#include <Arduino.h>
#include <globals.h>

// ---------------------------------------------------------------------------
// The airspace page
//
// A live bar chart answers "how busy is each channel right now". This adds the
// two things a bar chart cannot: history (a packets/sec sparkline) and the
// offenders in the room (one row per device, never one per frame). It is
// receive-only: nothing here transmits or associates.
//
// The channel list comes from bruceConfig.wifiHopPlan (task 1.4), so the sweep
// can be the eleven overlapping channels, the non-overlapping 1/6/11, or 1-13.
// ---------------------------------------------------------------------------

#define CA_MAX_CH 13
#define CA_HIST 96     // sparkline samples, one per full sweep
#define CA_SUSPECTS 4  // offenders listed at once; more than that is a wall of text
#define CA_SUSPECT_TTL_MS 60000

// Load% at/above which a bar is drawn solid (busy) instead of dimmed.
static const uint8_t CA_BUSY_THRESHOLD = 50;

static uint8_t ca_channels[CA_MAX_CH];
static int ca_nch = 0;

static void caBuildPlan() {
    ca_nch = 0;
    switch (bruceConfig.wifiHopPlan) {
        case 1: ca_channels[ca_nch++] = 1; ca_channels[ca_nch++] = 6; ca_channels[ca_nch++] = 11; break;
        case 2: for (int c = 1; c <= 13; c++) ca_channels[ca_nch++] = c; break;
        default: for (int c = 1; c <= 11; c++) ca_channels[ca_nch++] = c; break;
    }
    if (ca_nch == 0) { // never leave the sweep empty
        ca_channels[0] = 1;
        ca_channels[1] = 6;
        ca_channels[2] = 11;
        ca_nch = 3;
    }
}

static String caPlanName() {
    switch (bruceConfig.wifiHopPlan) {
        case 1: return "1,6,11";
        case 2: return "1-13";
        default: return "1-11";
    }
}

// Counters updated from the promiscuous RX callback for the *current* channel.
static volatile uint32_t ca_bytes = 0;
static volatile uint32_t ca_pkts = 0;
static volatile int8_t ca_rssi_peak = -128;

// Airspace history: one entry per full sweep, in packets per second. The bars
// below show the current instant; this shows whether the room was always this
// busy or just became busy, which a live bar chart cannot answer.
static uint32_t ca_hist[CA_HIST];
static int ca_histPos = 0;
static int ca_histCount = 0;

static void ca_histPush(uint32_t pps) {
    ca_hist[ca_histPos] = pps;
    ca_histPos = (ca_histPos + 1) % CA_HIST;
    if (ca_histCount < CA_HIST) ca_histCount++;
}

// ---------------------------------------------------------------------------
// Suspects (tasks 1.2 and 1.3)
//
// One entry per BSSID, not one per frame, so a camera that beacons ten times a
// second is one row and not ten. Everything is a pattern with the evidence next
// to it; the words "camera" and "rogue" are questions, not verdicts.
// ---------------------------------------------------------------------------
struct CaSuspect {
    bool used;
    uint8_t bssid[6];
    char ssid[18];       // last SSID seen from it
    uint16_t beacons;    // beacons from this BSSID
    uint16_t probes;     // probe responses from this BSSID
    uint16_t unseenSsid; // probe responses naming an SSID this BSSID never beacons
    uint8_t ssidHashes;  // how many different SSIDs it has beaconed (bit set, max 8)
    uint8_t distinct;    // popcount of ssidHashes
    bool camera;         // one of the addresses in its frames matched a known prefix
    bool rogue;          // answers probes it never beaconed, or floods SSIDs
    int8_t rssi;
    uint32_t lastSeen;
};

static CaSuspect caSuspects[CA_SUSPECTS];
static portMUX_TYPE caMux = portMUX_INITIALIZER_UNLOCKED;

// Set when a row first becomes a camera or a rogue AP, played once by the sweep
// loop: a cue belongs on the page that is drawing, not in the RX callback.
static volatile bool caNewSuspect = false;

// Remote ID seen in a beacon (task 1.2). Same decoder the BLE detector uses.
static OdidData caDrone;
static bool caDroneValid = false;
static uint32_t caDroneMs = 0;

// Flock Safety camera prefixes. Sourced, not guessed: the list published by the
// flock-you / flock-finder work (@NitekryDPaul's promiscuous-mode analysis, plus
// B4:1E:52 which is Flock's own IEEE assignment and the FS Ext Battery packs).
// Camera OUIs are a fingerprint, not a certificate - a camera can be re-flashed
// onto any chipset, so this is evidence, never a claim. Add prefixes here (three
// bytes) when a confirmed sighting supplies a new one.
static const uint8_t CA_CAMERA_OUI[][3] = {
    {0xB4, 0x1E, 0x52}, // Flock Safety's own registration
    {0x70, 0xC9, 0x4E}, {0x3C, 0x91, 0x80}, {0xD8, 0xF3, 0xBC}, {0x80, 0x30, 0x49}, {0xB8, 0x35, 0x32},
    {0x14, 0x5A, 0xFC}, {0x74, 0x4C, 0xA1}, {0x08, 0x3A, 0x88}, {0x9C, 0x2F, 0x9D}, {0xC0, 0x35, 0x32},
    {0x94, 0x08, 0x53}, {0xE4, 0xAA, 0xEA}, {0xF4, 0x6A, 0xDD}, {0xF8, 0xA2, 0xD6}, {0x24, 0xB2, 0xB9},
    {0x00, 0xF4, 0x8D}, {0xD0, 0x39, 0x57}, {0xE8, 0xD0, 0xFC}, {0xE0, 0x4F, 0x43}, {0xB8, 0x1E, 0xA4},
    {0x70, 0x08, 0x94}, {0x58, 0x8E, 0x81}, {0xEC, 0x1B, 0xBD}, {0x3C, 0x71, 0xBF}, {0x58, 0x00, 0xE3},
    {0xA4, 0xCF, 0x12}, {0x5C, 0x93, 0xA2}, {0x64, 0x6E, 0x69}, {0x04, 0x0D, 0x84}, {0xF0, 0x82, 0xC0},
    {0x1C, 0x34, 0xF1}, {0x38, 0x5B, 0x44}, {0x94, 0x34, 0x69}, {0xB4, 0xE3, 0xF9},
};
#define CA_CAMERA_OUI_COUNT ((int)(sizeof(CA_CAMERA_OUI) / sizeof(CA_CAMERA_OUI[0])))

static bool caIsCameraMac(const uint8_t *m) {
    for (int i = 0; i < CA_CAMERA_OUI_COUNT; i++)
        if (m[0] == CA_CAMERA_OUI[i][0] && m[1] == CA_CAMERA_OUI[i][1] && m[2] == CA_CAMERA_OUI[i][2]) return true;
    return false;
}

static uint8_t caPopcount8(uint8_t v) {
    uint8_t n = 0;
    while (v) {
        n += v & 1;
        v >>= 1;
    }
    return n;
}

// Finds (or opens) the row for this BSSID. Caller holds caMux.
static CaSuspect *caSuspectFor(const uint8_t *bssid, uint32_t now) {
    for (int i = 0; i < CA_SUSPECTS; i++)
        if (caSuspects[i].used && memcmp(caSuspects[i].bssid, bssid, 6) == 0) return &caSuspects[i];
    for (int i = 0; i < CA_SUSPECTS; i++) {
        if (!caSuspects[i].used || (now - caSuspects[i].lastSeen) > CA_SUSPECT_TTL_MS) {
            memset(&caSuspects[i], 0, sizeof(CaSuspect));
            caSuspects[i].used = true;
            memcpy(caSuspects[i].bssid, bssid, 6);
            return &caSuspects[i];
        }
    }
    // Full: the row that has been quiet longest is the one worth losing.
    int oldest = 0;
    for (int i = 1; i < CA_SUSPECTS; i++)
        if (caSuspects[i].lastSeen < caSuspects[oldest].lastSeen) oldest = i;
    memset(&caSuspects[oldest], 0, sizeof(CaSuspect));
    caSuspects[oldest].used = true;
    memcpy(caSuspects[oldest].bssid, bssid, 6);
    return &caSuspects[oldest];
}

static void caNoteSsid(CaSuspect *s, const String &ssid) {
    // A tiny set of SSID hashes, so "one BSSID beacons eight SSIDs" can be
    // counted without holding eight strings.
    if (ssid.length() == 0) return;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < ssid.length(); i++) h = (h ^ (uint8_t)ssid[i]) * 16777619u;
    const uint8_t bit = (uint8_t)(1u << (h & 7));
    s->ssidHashes |= bit;
    s->distinct = caPopcount8(s->ssidHashes);
}

// Every management frame from an AP passes through here: beacons and probe
// responses carry the SSID and every vendor IE, which is where both the Remote
// ID beacon and the "one BSSID, many SSIDs" signature live.
static void caParseApFrame(const uint8_t *f, int len, int8_t rssi, int channel) {
    if (len < 36) return; // 24 header + 12 beacon fixed fields
    const uint8_t subtype = (f[0] >> 4) & 0x0F;
    if (subtype != 8 && subtype != 5) return; // beacon / probe response
    const uint8_t *bssid = f + 16;            // addr3 is the BSSID for both
    const bool beacon = (subtype == 8);

    String ssid;
    OdidData drone;
    bool haveDrone = false;
    int off = 36; // timestamp(8) + interval(2) + capability(2)
    while (off + 2 <= len) {
        const uint8_t id = f[off];
        const uint8_t ilen = f[off + 1];
        if (off + 2 + ilen > len) break;
        const uint8_t *p = f + off + 2;
        if (id == 0 && ilen <= 32) {
            ssid = "";
            for (uint8_t i = 0; i < ilen; i++) ssid += (char)p[i]; // 0 length = hidden
        } else if (id == 221 && ilen >= 7 && p[0] == 0xFA && p[1] == 0x0B && p[2] == 0xBC) {
            // ASD-STAN Remote ID in a beacon: OUI FA:0B:BC, 0x0D, counter, pack.
            if (odidDecodeBeaconVendorIe(p, ilen, drone)) haveDrone = true;
        }
        off += 2 + ilen;
    }

    const uint32_t now = millis();
    portENTER_CRITICAL(&caMux);
    if (haveDrone) {
        caDrone = drone;
        caDroneValid = true;
        caDroneMs = now;
    }
    CaSuspect *s = caSuspectFor(bssid, now);
    if (s != nullptr) {
        s->lastSeen = now;
        s->rssi = rssi;
        if (ssid.length()) {
            // Only the first 17 chars are drawn, but the full SSID is what the
            // rogue test compares against.
            strncpy(s->ssid, ssid.c_str(), sizeof(s->ssid) - 1);
            s->ssid[sizeof(s->ssid) - 1] = 0;
        }
        if (beacon) {
            s->beacons++;
            caNoteSsid(s, ssid);
        } else {
            s->probes++;
            if (ssid.length() && s->distinct == 0) s->unseenSsid++; // answered before it ever beaconed
        }
        // A camera's MAC can appear as the transmitter OR as the receiver
        // (addr1) of a frame - the receivers are awake far more often, which is
        // what makes this usable at all.
        bool wasSuspect = s->camera || s->rogue;
        if (!s->camera && (caIsCameraMac(bssid) || caIsCameraMac(f + 4) || caIsCameraMac(f + 10))) s->camera = true;
        // PineAP answers probes for SSIDs it never beacons, and floods many
        // SSIDs from one BSSID. Either is a pattern worth a row; neither is
        // proof.
        s->rogue = (s->probes >= 5 && s->beacons == 0) || s->distinct >= 4 || s->unseenSsid >= 3;
        if (!wasSuspect && (s->camera || s->rogue)) caNewSuspect = true;
    }
    (void)channel;
    portEXIT_CRITICAL(&caMux);
}

// Keep the callback minimal: just accumulate. Airtime is estimated in the loop.
static void IRAM_ATTR ca_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (!pkt) return;
    ca_pkts++;
    ca_bytes += pkt->rx_ctrl.sig_len;
    if (pkt->rx_ctrl.rssi > ca_rssi_peak) ca_rssi_peak = pkt->rx_ctrl.rssi;
    // Management frames are the only ones with SSIDs and vendor IEs.
    if (type == WIFI_PKT_MGMT && pkt->rx_ctrl.sig_len > 36)
        caParseApFrame(pkt->payload, pkt->rx_ctrl.sig_len, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel);
}

// True when this page brought the driver up itself, so it must also take it down.
// esp_wifi_init() and the Arduino WiFi layer keep separate bookkeeping: Arduino's
// `lowLevelInitDone` stays false when the driver was initialised here, which means
// `WiFi.mode(WIFI_OFF)` is a no-op afterwards (espWiFiStop() returns early) and
// the driver plus its ~46 KB of internal DRAM stay up for the rest of the session.
// That is what made BLE Detector refuse with "RAM 24K/15K - restart" right after
// this page. When the driver was already up (a Wi-Fi feature initialised it
// first) init returns ESP_ERR_WIFI_INIT_STATE, we do not own it, and Arduino's own
// stop path is the one that frees it.
static bool ca_owns_wifi = false;

// Tolerant WiFi bring-up. Unlike the sniffer (ESP_ERROR_CHECK), we never abort:
// if WiFi is already initialised/started we get ESP_ERR_WIFI_INIT_STATE (or
// similar) and just carry on — promiscuous mode works regardless.
static bool ca_start_wifi() {
    ensureWifiPlatform();
    nvs_flash_init();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e == ESP_OK) ca_owns_wifi = true;
    if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE)
        Serial.printf("[ChAnalyzer] wifi_init: %s\n", esp_err_to_name(e));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE)
        Serial.printf("[ChAnalyzer] wifi_start: %s\n", esp_err_to_name(e));
    // Capture every frame type so the airtime estimate reflects real load; the
    // management frames also feed the suspect table.
    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(ca_rx_cb);
    // Checked, not assumed: if promiscuous mode fails the counters stay at zero
    // and an empty band looks exactly like a working analyzer with a quiet room.
    e = esp_wifi_set_promiscuous(true);
    if (e != ESP_OK) {
        Serial.printf("[ChAnalyzer] promiscuous: %s\n", esp_err_to_name(e));
        return false;
    }
    // The sweep below only means anything if the radio obeys the channel change.
    e = esp_wifi_set_channel(ca_channels[0], WIFI_SECOND_CHAN_NONE);
    if (e != ESP_OK) {
        Serial.printf("[ChAnalyzer] set_channel: %s\n", esp_err_to_name(e));
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        return false;
    }
    return true;
}

static void ca_stop_wifi() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
    wifiDisconnect();
    vTaskDelay(1 / portTICK_RATE_MS);
    // Hand the driver back when this page was the one that brought it up: an
    // idle driver is 46 KB of internal DRAM that the next radio (BLE needs 60 KB)
    // cannot get, and nothing else will ever free it.
    if (ca_owns_wifi) {
        esp_wifi_deinit();
        ca_owns_wifi = false;
    }
}

static void caAddrString(const uint8_t *a, char *out, size_t n) {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

static int caSuspectCount() {
    int n = 0;
    const uint32_t now = millis();
    portENTER_CRITICAL(&caMux);
    for (int i = 0; i < CA_SUSPECTS; i++)
        if (caSuspects[i].used && (now - caSuspects[i].lastSeen) <= CA_SUSPECT_TTL_MS &&
            (caSuspects[i].camera || caSuspects[i].rogue))
            n++;
    portEXIT_CRITICAL(&caMux);
    return n;
}

// One page, one row per offender, with the evidence beside the name. Pressing
// SEL on the analyzer opens this; BACK returns to the bars.
static void caSuspectPage() {
    // check() consumes the press, so this loop leaves at the first read while
    // the key is still down; the delay covers the rest of the hold, or the
    // page's own first `check(SelPress)` closes it again instantly.
    while (check(SelPress)) vTaskDelay(pdMS_TO_TICKS(10));
    delay(150);
    while (true) {
        CaSuspect snap[CA_SUSPECTS];
        OdidData drone;
        bool haveDrone;
        const uint32_t now = millis();
        portENTER_CRITICAL(&caMux);
        memcpy(snap, caSuspects, sizeof(caSuspects));
        drone = caDrone;
        haveDrone = caDroneValid && (now - caDroneMs) < 30000;
        portEXIT_CRITICAL(&caMux);

        drawMainBorder();
        tft.setTextSize(1);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.setCursor(10, 26);
        tft.print("SUSPECTS IN THE AIR");
        tft.drawFastHLine(10, 38, tftWidth - 20, bruceConfig.secColor);

        int y = 46;
        int shown = 0;
        for (int i = 0; i < CA_SUSPECTS && y < tftHeight - 24; i++) {
            if (!snap[i].used) continue;
            if (!(snap[i].camera || snap[i].rogue)) continue;
            if ((now - snap[i].lastSeen) > CA_SUSPECT_TTL_MS) continue;
            char mac[18];
            caAddrString(snap[i].bssid, mac, sizeof(mac));
            String line = String(snap[i].camera ? "CAM " : "ROG ") + mac;
            tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
            tft.setCursor(10, y);
            tft.print(line);
            char detail[48];
            if (snap[i].camera)
                snprintf(detail, sizeof(detail), "%s %ddBm", snap[i].ssid[0] ? snap[i].ssid : "-", snap[i].rssi);
            else
                snprintf(
                    detail, sizeof(detail), "%u ssid %u probe %s", (unsigned)snap[i].distinct,
                    (unsigned)snap[i].probes, snap[i].ssid[0] ? snap[i].ssid : "-"
                );
            tft.setCursor(10, y + 10);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.print(detail);
            y += 24;
            shown++;
        }
        if (haveDrone) {
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.setCursor(10, y);
            tft.print(String("DRONE ") + odidStatusName(drone.status));
            char d2[48];
            snprintf(
                d2, sizeof(d2), "%.0fm geo %.0fm h  %.0f,%.0f", drone.altGeo, drone.height, drone.lat, drone.lon
            );
            tft.setCursor(10, y + 10);
            tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
            tft.print(d2);
            shown++;
        }
        if (shown == 0) {
            tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
            tft.setCursor(10, 52);
            tft.print("nothing suspicious heard yet");
        }
        tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
        tft.setCursor(10, tftHeight - 22);
        tft.print("a pattern, not a verdict");
        tft.setCursor(10, tftHeight - 10);
        tft.print("BACK exit");

        // BACK is the only way out: SEL opened this page, and the injected/key
        // press that did it can still read as pressed on the first pass, which
        // closed the page instantly and dropped the user back on the bars.
        if (check(EscPress)) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    while (check(SelPress) || check(EscPress)) vTaskDelay(pdMS_TO_TICKS(10));
}

static void ca_draw(const uint8_t *load, const uint8_t *peak, const int8_t *rssi, uint8_t curCh, uint16_t dwell) {
    drawMainBorder(false);

    const int x0 = 8;                           // left of bars
    const int top = 26;                         // below title
    const int footerY = tftHeight - 2 * LH * FP; // leave room for footer
    const int sparkH = 20;
    const int sparkY = footerY - sparkH - 4;
    const int bottom = sparkY - 2;               // bars stop above the sparkline
    const int avail = bottom - top;
    const int rowH = avail / ca_nch;
    const int labelW = 30; // "Ch11"
    const int valW = 30;   // " 100%"
    const int barX = x0 + labelW;
    const int barW = tftWidth - barX - valW - 6;

    tft.setTextSize(1);
    // Which plan is being swept, top right: without it "why is channel 13
    // missing" is a mystery rather than a setting.
    tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
    tft.drawRightString(caPlanName(), tftWidth - 8, 12, 1);

    for (int i = 0; i < ca_nch; i++) {
        uint8_t ch = ca_channels[i];
        int y = top + i * rowH;
        bool isCur = (ch == curCh);

        // label
        tft.setTextColor(
            isCur ? bruceConfig.bgColor : bruceConfig.priColor,
            isCur ? bruceConfig.priColor : bruceConfig.bgColor
        );
        tft.drawString("Ch" + String(ch), x0, y, 1);

        // bar frame
        int bh = rowH - 3;
        if (bh < 4) bh = 4;
        tft.drawRect(barX, y, barW, bh, bruceConfig.priColor);

        // filled portion ~ load%
        int fillW = (barW - 2) * load[ch] / 100;

        // solid above threshold, dimmed below
        uint16_t c = (load[ch] >= CA_BUSY_THRESHOLD) ? bruceConfig.priColor : TFT_DARKGREY;
        tft.fillRect(barX + 1, y + 1, fillW, bh - 2, c);
        // clear remaining area
        tft.fillRect(barX + 1 + fillW, y + 1, barW - 2 - fillW, bh - 2, bruceConfig.bgColor);

        // peak-hold marker
        int peakX = barX + 1 + (barW - 2) * peak[ch] / 100;
        if (peakX > barX + 1) tft.drawFastVLine(peakX, y + 1, bh - 2, TFT_RED);

        // value
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.drawString(String(load[ch]) + "%", barX + barW + 4, y, 1);
    }

    // ---- packets/sec sparkline (task 1.4) --------------------------------
    // Newest on the right. Scaled to the window's own maximum, so a quiet room
    // still shows its shape instead of a flat line at the bottom.
    if (ca_histCount > 0) {
        uint32_t histMax = 1;
        for (int i = 0; i < ca_histCount; i++)
            if (ca_hist[i] > histMax) histMax = ca_hist[i];

        const int sx = x0, sw = tftWidth - 2 * x0;
        tft.drawFastHLine(sx, sparkY + sparkH, sw, bruceConfig.secColor);
        const int cols = sw / 2;
        const int first = ca_histCount < cols ? 0 : ca_histPos; // ring start
        for (int c = 0; c < cols && c < ca_histCount; c++) {
            const uint32_t v = ca_hist[(first + c) % CA_HIST];
            int h = (int)(v * (uint32_t)(sparkH - 1) / histMax);
            if (v > 0 && h < 1) h = 1;
            if (h <= 0) continue;
            // Rightmost column is the newest sample.
            const int x = sx + sw - 1 - (cols - 1 - c) * 2;
            tft.fillRect(x - 1, sparkY + sparkH - h, 2, h, uiGradientColorAt(x));
        }
        tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
        tft.drawString(String(histMax) + "pkt/s", sx + sw - 46, sparkY - 8, 1);
    }

    // footer: current channel detail + signal meter + dwell
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    // A channel with no frame in its window has no RSSI; printing 0 dBm made an
    // empty channel look like the strongest signal on the screen.
    String sig = (rssi[curCh] == -128) ? String("--") : String(rssi[curCh]) + "dBm";
    String foot = "Ch" + String(curCh) + " " + String(load[curCh]) + "% pk" + String(peak[curCh]) + "% " + sig +
                  " dwell " + String(dwell) + "ms  ";
    // The suspect count is the only thing on this footer that is not about the
    // current instant: one row per offender lives behind SEL.
    const int suspects = caSuspectCount();
    if (suspects > 0) foot += String(suspects) + " suspect";
    tft.drawString(foot, x0, footerY, 1);
}

void channel_analyzer_setup() {
    returnToMenu = false;

    uint8_t load[16] = {0};
    uint8_t peak[16] = {0};
    int8_t rssi[16];
    for (int i = 0; i < 16; i++) rssi[i] = -128;

    uint16_t dwell = 350; // ms per channel, adjustable with Up/Down
    int idx = 0;

    caBuildPlan();

    // One sparkline sample per full sweep of the plan.
    uint32_t sweepPkts = 0;
    uint32_t sweepStart = millis();

    // Fresh suspect table for this visit: a camera seen last time is not in this
    // room, and the list says "in the air" - so it must be.
    portENTER_CRITICAL(&caMux);
    memset(caSuspects, 0, sizeof(caSuspects));
    caDroneValid = false;
    portEXIT_CRITICAL(&caMux);

    if (!ca_start_wifi()) {
        displayError("WiFi busy - exit WiFi first", true);
        returnToMenu = true;
        return;
    }

    // Fresh history for this visit: a sparkline that starts with the previous
    // session's samples would describe a room the user was not in.
    ca_histCount = 0;
    ca_histPos = 0;

    tft.fillScreen(bruceConfig.bgColor);
    drawMainBorderWithTitle("Channel Analyzer");
    tft.setTextSize(FP);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    padprintln("");
    padprintln(" sweeping " + caPlanName() + " ...");
    delay(1000);
    drawMainBorder(true);
    for (;;) {
        if (returnToMenu) break;
        if (check(EscPress)) {
            returnToMenu = true;
            break;
        }
        if (check(UpPress) && dwell < 1000) dwell += 100;  // longer dwell = more accurate
        if (check(DownPress) && dwell > 150) dwell -= 100; // shorter dwell = faster sweep
        if (check(SelPress)) {
            caSuspectPage();
            while (check(SelPress)) vTaskDelay(pdMS_TO_TICKS(10)); // release before re-entering
            drawMainBorder(true); // the suspect page painted over the bars
        }

        uint8_t ch = ca_channels[idx];
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

        // reset counters for this dwell window
        ca_bytes = 0;
        ca_pkts = 0;
        ca_rssi_peak = -128;

        uint32_t t0 = millis();
        while (millis() - t0 < dwell) {
            if (check(EscPress)) {
                returnToMenu = true;
                break;
            }
            vTaskDelay(20 / portTICK_PERIOD_MS);
        }
        if (returnToMenu) break;

        // Estimate airtime utilisation: bytes at a conservative ~6Mbps baseline
        // plus per-frame preamble/IFS overhead. Clamp to 0-100%.
        uint32_t airtime_us = (ca_bytes * 8UL) / 6UL + ca_pkts * 60UL;
        uint32_t dwell_us = (uint32_t)dwell * 1000UL;
        uint32_t l = dwell_us ? (airtime_us * 100UL / dwell_us) : 0;
        if (l > 100) l = 100;

        load[ch] = (uint8_t)l;
        if (load[ch] > peak[ch]) peak[ch] = load[ch];
        // Keep -128 as "nothing heard": ca_draw prints it as "--" rather than
        // the old 0 dBm, which is indistinguishable from a transmitter on top
        // of the board.
        rssi[ch] = ca_rssi_peak;

        sweepPkts += ca_pkts;

        ca_draw(load, peak, rssi, ch, dwell);
        if (caNewSuspect) {
            caNewSuspect = false;
            soundPlay(CUE_SUSPECT);
        }

        idx = (idx + 1) % ca_nch;
        if (idx == 0) {
            // A sweep just ended: its average rate is the history sample.
            const uint32_t elapsed = millis() - sweepStart;
            ca_histPush(elapsed ? sweepPkts * 1000UL / elapsed : 0);
            sweepPkts = 0;
            sweepStart = millis();
        }
    }

    ca_stop_wifi();
}

#endif
