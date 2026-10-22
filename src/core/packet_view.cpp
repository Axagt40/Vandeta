#include "packet_view.h"

#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/wifi/webInterface.h"
#include "core/wifi/wifi_common.h"
#include "core/radio_mem.h"
#include "modules/NRF24/nrf_capture.h"
#include "modules/ble/ble_capture.h"
#include "modules/rf/rf_utils.h"

#include <NimBLEDevice.h>
#include <algorithm>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <globals.h>
#include <nvs_flash.h>
#include <vector>

// ---------------------------------------------------------------------------
// Task 17 - real-time packet monitoring dashboard  (phase 2: live capture)
//
// The dashboard is a bounded, thread-safe ring of RADIO METADATA. Each radio
// tap parses only what is needed to describe a frame (source, channel,
// transmitter address, type/subtype, length, RSSI, and any SSID/name the
// radio advertises openly). Nothing here decrypts, reassembles or extracts
// payload contents.
//
// Producers: the WiFi promiscuous callback (WiFi task context), the BLE scan
// task, an NRF24 capture task, and the CC1101 pulse ISR. Consumer: the main
// loop. All access to the ring goes through a tiny critical section.
// ---------------------------------------------------------------------------

#define PV_ROWS 6
#define PV_RING 64

enum { PV_CAT_MGMT = 0, PV_CAT_DATA, PV_CAT_CTRL, PV_CAT_ADV, PV_CAT_OTHER, PV_CAT_COUNT };

struct PvEntry {
    uint32_t seq; // monotonically increasing, for ordering across the wrap
    uint32_t t;   // millis() when captured
    uint8_t src;  // PacketSource
    int16_t ch;   // WiFi channel, NRF channel, or SubGHz frequency in MHz
    uint8_t addr[6];
    uint8_t cat;
    uint16_t len;
    int8_t rssi;
    char label[10]; // frame type/subtype, e.g. BEACON / ADV_IND / PULSE
    char text[20];  // open SSID / device name when advertised
};

// The ring is pure scratch (64 x 56 = 3.5 KB) and used to sit in .bss, i.e. it
// cost internal DRAM from boot even for users who never open the packet
// monitor. It is allocated when the monitor starts, from PSRAM when available
// (internal heap as the fallback on a board without it). pvFeed() is also
// reachable from the CC1101 pulse ISR, which must never allocate - hence the
// null guards rather than lazy allocation inside the producer.
static PvEntry *pvRing = nullptr;
static volatile uint16_t pvWrite = 0;  // next slot to write
static volatile uint32_t pvSeq = 0;    // total ever produced
static portMUX_TYPE pvMux = portMUX_INITIALIZER_UNLOCKED;

static bool pvRingAlloc() {
    if (pvRing) return true;
    const size_t bytes = sizeof(PvEntry) * PV_RING;
    pvRing = (PvEntry *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!pvRing) pvRing = (PvEntry *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    return pvRing != nullptr;
}

// Session counters, unaffected by the ring wrapping.
static volatile uint32_t pvSrcTotal[4] = {0, 0, 0, 0};
static volatile uint32_t pvCatTotal[PV_CAT_COUNT] = {0, 0, 0, 0, 0};
static volatile uint32_t pvChHist[128] = {0};

static uint32_t pvStartMs = 0;
static uint32_t pvLastSampleMs = 0;
static uint32_t pvLastSampleTotal = 0;
static uint32_t pvDirtySeq = 0; // last seq the dashboard already drew
static float pvPps = 0.0f;

// --- filters ---------------------------------------------------------------
struct PvFilter {
    int radio = -1;   // -1 any, else PacketSource
    int cat = -1;     // -1 any, else PV_CAT_*
    int channel = -1; // -1 any
    uint8_t oui[3] = {0, 0, 0};
    bool hasOui = false;
    String text;
    bool onlyNew = false;
};
static PvFilter pvFilter;
static bool pvDirty = true;
static bool pvPaused = false;

// --- WiFi raw frames for the pcap export -----------------------------------
#define PV_PCAP_MAX 48
#define PV_PCAP_SNAP 512
struct PvPcapSlot {
    uint32_t sec;
    uint32_t usec;
    uint16_t len;
    uint8_t data[PV_PCAP_SNAP];
};
static PvPcapSlot *pvPcap = nullptr;
static volatile uint16_t pvPcapWrite = 0;
static volatile uint16_t pvPcapTotal = 0;

// ===========================================================================
// Ring
// ===========================================================================

static void pvResetAll() {
    // Allocates on the first call only; called from packetMonitor() before any
    // producer is started, so no ISR can be looking at the ring yet.
    pvRingAlloc();
    portENTER_CRITICAL(&pvMux);
    pvWrite = 0;
    pvSeq = 0;
    for (int i = 0; i < 4; i++) pvSrcTotal[i] = 0;
    for (int i = 0; i < PV_CAT_COUNT; i++) pvCatTotal[i] = 0;
    for (int i = 0; i < 128; i++) pvChHist[i] = 0;
    portEXIT_CRITICAL(&pvMux);
    pvPcapWrite = 0;
    pvPcapTotal = 0;
    pvStartMs = millis();
    pvLastSampleMs = pvStartMs;
    pvLastSampleTotal = 0;
    pvDirtySeq = 0;
    pvPps = 0.0f;
    pvDirty = true;
}

static void pvFeed(
    uint8_t src, int16_t ch, const uint8_t *addr, uint8_t cat, uint16_t len, int8_t rssi, const char *label,
    const char *text
) {
    portENTER_CRITICAL(&pvMux);
    if (pvRing) {
        PvEntry &e = pvRing[pvWrite];
        e.seq = ++pvSeq;
        e.t = millis();
        e.src = src;
        e.ch = ch;
        if (addr) memcpy(e.addr, addr, 6);
        else memset(e.addr, 0, 6);
        e.cat = cat;
        e.len = len;
        e.rssi = rssi;
        strlcpy(e.label, label ? label : "", sizeof(e.label));
        strlcpy(e.text, text ? text : "", sizeof(e.text));
        pvWrite = (uint16_t)((pvWrite + 1) % PV_RING);
    }

    if (src < 4) pvSrcTotal[src]++;
    if (cat < PV_CAT_COUNT) pvCatTotal[cat]++;
    if (ch >= 0 && ch < 128) pvChHist[ch]++;
    portEXIT_CRITICAL(&pvMux);
}

// Copy one logical entry (0 = oldest available) out of the ring.
static bool pvGetLogical(uint32_t index, PvEntry *out) {
    bool ok = false;
    if (!pvRing) return false;
    portENTER_CRITICAL(&pvMux);
    uint32_t avail = pvSeq < PV_RING ? pvSeq : PV_RING;
    if (index < avail) {
        uint16_t slot = (uint16_t)((pvWrite + PV_RING - avail + index) % PV_RING);
        *out = pvRing[slot];
        ok = true;
    }
    portEXIT_CRITICAL(&pvMux);
    return ok;
}

static uint32_t pvAvailable() { return pvSeq < PV_RING ? pvSeq : PV_RING; }

// ===========================================================================
// Formatting helpers (kept identical to the approved shell layout)
// ===========================================================================

static const char *pvSourceName(PacketSource s) {
    switch (s) {
        case PKT_SRC_WIFI: return "WiFi";
        case PKT_SRC_BLE: return "BLE";
        case PKT_SRC_NRF24: return "NRF24";
        case PKT_SRC_SUBGHZ: return "SubGHz";
    }
    return "?";
}

static const char *pvSourceShort(PacketSource s) {
    switch (s) {
        case PKT_SRC_WIFI: return "WIFI";
        case PKT_SRC_BLE: return "BLE";
        case PKT_SRC_NRF24: return "NRF";
        case PKT_SRC_SUBGHZ: return "SUB";
    }
    return "?";
}

static const char *pvCatName(int c) {
    switch (c) {
        case PV_CAT_MGMT: return "mgmt";
        case PV_CAT_DATA: return "data";
        case PV_CAT_CTRL: return "ctrl";
        case PV_CAT_ADV: return "adv";
        default: return "other";
    }
}

static String pvPadR(String s, int w) {
    while ((int)s.length() < w) s += " ";
    return s.substring(0, w);
}

static String pvPadL(String s, int w) {
    while ((int)s.length() < w) s = " " + s;
    return s.substring(max(0, (int)s.length() - w));
}

static String pvHeaderRow() {
    return pvPadL("TIME", 6) + " " + pvPadR("RADIO", 5) + " " + pvPadL("CH", 3) + " " + pvPadR("ADDRESS", 14) + " " +
           pvPadR("TYPE", 8) + " " + pvPadL("LEN", 4) + " " + pvPadL("RSSI", 5);
}

// "AA:BB:CC:DD:EE" (14 chars) - five bytes is all that fits the column.
static void pvAddrString(const uint8_t *a, char *out, size_t n) {
    if (!a) {
        strlcpy(out, "--:--:--:--:--", n);
        return;
    }
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4]);
}

static String pvRowString(const PvEntry &e) {
    char addr[20];
    pvAddrString(e.addr, addr, sizeof(addr));

    String time;
    uint32_t rel = (e.t >= pvStartMs) ? (e.t - pvStartMs) : 0;
    time = String(rel / 1000.0f, 1) + "s";

    String ch = "--";
    if (e.ch > 0) ch = String(e.ch);

    String rssi = (e.rssi == 0 && e.src == PKT_SRC_SUBGHZ) ? "---" : String(e.rssi);

    return pvPadL(time, 6) + " " + pvPadR(pvSourceShort((PacketSource)e.src), 5) + " " + pvPadL(ch, 3) + " " +
           pvPadR(addr, 14) + " " + pvPadR(e.label, 8) + " " + pvPadL(String(e.len), 4) + " " + pvPadL(rssi, 5);
}

static void pvDrawLine(const String &s, int x, int y, uint16_t fg) {
    tft.setTextSize(FP);
    tft.setTextColor(fg, bruceConfig.bgColor);
    tft.setCursor(x, y);
    tft.print(s);
}

// ===========================================================================
// Filtering / visible list (newest first)
// ===========================================================================

static bool pvOuiMatch(const uint8_t *addr) {
    if (!pvFilter.hasOui || !addr) return true;
    return addr[0] == pvFilter.oui[0] && addr[1] == pvFilter.oui[1] && addr[2] == pvFilter.oui[2];
}

// True when no *older* entry has the same radio+address+type.
static bool pvIsFirstOccurrence(uint32_t logicalIndex, const PvEntry &e) {
    for (uint32_t i = 0; i < logicalIndex; i++) {
        PvEntry o;
        if (!pvGetLogical(i, &o)) break;
        if (o.src == e.src && o.cat == e.cat && memcmp(o.addr, e.addr, 6) == 0) return false;
    }
    return true;
}

static std::vector<int> pvVisibleLogical() {
    std::vector<int> out;
    uint32_t n = pvAvailable();
    for (uint32_t i = 0; i < n; i++) {
        PvEntry e;
        if (!pvGetLogical(i, &e)) continue;
        if (pvFilter.radio >= 0 && e.src != pvFilter.radio) continue;
        if (pvFilter.cat >= 0 && e.cat != pvFilter.cat) continue;
        if (pvFilter.channel >= 0 && e.ch != pvFilter.channel) continue;
        if (!pvOuiMatch(e.addr)) continue;
        if (pvFilter.text.length() > 0) {
            String hay = String(e.text) + " " + e.label;
            hay.toLowerCase();
            String needle = pvFilter.text;
            needle.toLowerCase();
            if (hay.indexOf(needle) == -1) continue;
        }
        if (pvFilter.onlyNew && !pvIsFirstOccurrence(i, e)) continue;
        out.push_back((int)i); // oldest..newest
    }
    std::reverse(out.begin(), out.end()); // newest first
    return out;
}

static int pvBusiestChannel() {
    int best = -1;
    uint32_t bestCount = 0;
    for (int i = 0; i < 128; i++) {
        if (pvChHist[i] > bestCount) {
            bestCount = pvChHist[i];
            best = i;
        }
    }
    return best;
}

// ===========================================================================
// WiFi tap
// ===========================================================================

static uint8_t pvWifiCh = 1;
static uint8_t pvWifiChIdx = 0;
static const uint8_t PV_WIFI_CHANNELS[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static bool pvWifiUp = false;
static bool pvWifiEventLoopOurs = false;

static String pvWifiSsid(const uint8_t *frame, int len) {
    if (len < 36) return "";
    int offset = 36;
    while (offset + 1 < len) {
        uint8_t tag = frame[offset];
        uint8_t tagLen = frame[offset + 1];
        if (offset + 2 + tagLen > len) break;
        if (tag == 0x00) {
            String ssid = "";
            for (int i = 0; i < tagLen; i++) {
                uint8_t c = frame[offset + 2 + i];
                if (c >= 32 && c < 127) ssid += (char)c;
            }
            return ssid;
        }
        offset += 2 + tagLen;
    }
    return "";
}

static void pvWifiCb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!buf) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;

    uint16_t fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    uint8_t ft = (fc >> 2) & 0x03;
    uint8_t st = (fc >> 4) & 0x0F;

    const uint8_t *addr2 = frame + 10; // transmitter
    const char *label = "WIFI";
    uint8_t cat = PV_CAT_OTHER;
    String ssid = "";

    if (ft == 0) {
        cat = PV_CAT_MGMT;
        switch (st) {
            case 4: label = "PROBE_REQ"; break;
            case 5: label = "PROBE_RSP"; ssid = pvWifiSsid(frame, len); break;
            case 8: label = "BEACON"; ssid = pvWifiSsid(frame, len); break;
            case 11: label = "AUTH"; break;
            case 12: label = "DEAUTH"; break;
            case 13: label = "ACTION"; break;
            default: label = "MGMT"; break;
        }
    } else if (ft == 2) {
        cat = PV_CAT_DATA;
        label = "DATA";
    } else if (ft == 1) {
        cat = PV_CAT_CTRL;
        label = "CTRL";
    }

    pvFeed(PKT_SRC_WIFI, pvWifiCh, addr2, cat, (uint16_t)len, pkt->rx_ctrl.rssi, label, ssid.c_str());

    // Keep a bounded copy of the raw frame for the pcap export.
    if (pvPcap) {
        uint16_t slot = pvPcapWrite;
        PvPcapSlot &s = pvPcap[slot];
        s.sec = pkt->rx_ctrl.timestamp / 1000000ULL;
        s.usec = pkt->rx_ctrl.timestamp % 1000000ULL;
        uint16_t copy = len > PV_PCAP_SNAP ? PV_PCAP_SNAP : (uint16_t)len;
        s.len = copy;
        memcpy(s.data, frame, copy);
        pvPcapWrite = (uint16_t)((pvPcapWrite + 1) % PV_PCAP_MAX);
        pvPcapTotal++;
    }
}

static bool pvWifiStart() {
    cleanlyStopWebUiForWiFiFeature();

    // The driver allocates its RX/TX buffers from internal DRAM and fails with
    // ESP_ERR_NO_MEM ("Expected to init 4 rx buffer, actual is 3") when another
    // radio or a stale capture holds it. Release BLE / the Web UI first; this is
    // the same guard every other Wi-Fi tool uses.
    if (!wifiEnsureRadioMem()) {
        Serial.printf(
            "[PktMon] not enough internal RAM for the WiFi driver (free %u, largest DMA %u)\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA)
        );
        return false;
    }

    // Deliberately NOT ensureWifiPlatform(): raw promiscuous capture needs the
    // WiFi driver, not the lwIP/event-loop stack, and creating that stack costs
    // ~27 KB of internal DRAM that is never returned (esp_netif_init has no
    // deinit path here). With it, leaving this screen left too little RAM for
    // the BLE tap straight afterwards (measured: 83,775 -> 56,603 bytes free,
    // NimBLE needs ~56 KB and failed by a few KB).
    //
    // The default event loop is different: the driver posts a HOME_CHANNEL_CHANGE
    // event on every hop and logs "failed to post WiFi event=43" when no loop
    // exists. Create one for the capture and delete it on the way out, so the
    // log stays clean and the RAM goes back to the next radio.
    if (esp_event_loop_create_default() == ESP_OK) pvWifiEventLoopOurs = true;
    nvs_flash_init();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e == ESP_ERR_WIFI_INIT_STATE) e = ESP_OK; // already up: reuse it
    if (e != ESP_OK) {
        Serial.printf("[PktMon] wifi_init: %s\n", esp_err_to_name(e));
        if (pvWifiEventLoopOurs) {
            esp_event_loop_delete_default();
            pvWifiEventLoopOurs = false;
        }
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    // Another feature may have started the driver in AP mode; a started driver
    // refuses the mode change with ESP_ERR_WIFI_STATE, which is not fatal here.
    esp_err_t modeErr = esp_wifi_set_mode(WIFI_MODE_STA);
    e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE && e != ESP_ERR_WIFI_STATE) {
        Serial.printf("[PktMon] wifi_start: %s\n", esp_err_to_name(e));
        esp_wifi_deinit();
        if (pvWifiEventLoopOurs) {
            esp_event_loop_delete_default();
            pvWifiEventLoopOurs = false;
        }
        return false;
    }
    if (modeErr != ESP_OK && modeErr != ESP_ERR_WIFI_STATE && modeErr != ESP_ERR_WIFI_INIT_STATE)
        Serial.printf("[PktMon] wifi_mode: %s\n", esp_err_to_name(modeErr));

    if (!pvPcap) {
        pvPcap = (PvPcapSlot *)heap_caps_malloc(sizeof(PvPcapSlot) * PV_PCAP_MAX, MALLOC_CAP_SPIRAM);
        if (!pvPcap) pvPcap = (PvPcapSlot *)heap_caps_malloc(sizeof(PvPcapSlot) * PV_PCAP_MAX, MALLOC_CAP_8BIT);
    }
    pvPcapWrite = 0;
    pvPcapTotal = 0;

    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(pvWifiCb);
    pvWifiChIdx = 0;
    pvWifiCh = PV_WIFI_CHANNELS[0];
    esp_wifi_set_channel(pvWifiCh, WIFI_SECOND_CHAN_NONE);
    e = esp_wifi_set_promiscuous(true);
    if (e != ESP_OK) {
        Serial.printf("[PktMon] promiscuous: %s\n", esp_err_to_name(e));
        esp_wifi_set_promiscuous_rx_cb(NULL);
        esp_wifi_stop();
        esp_wifi_deinit();
        if (pvWifiEventLoopOurs) {
            esp_event_loop_delete_default();
            pvWifiEventLoopOurs = false;
        }
        return false;
    }
    pvWifiUp = true;
    return true;
}

static void pvWifiStop() {
    if (!pvWifiUp) return;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
    // The bring-up above used the raw esp_wifi_* API, so the Arduino WiFi
    // object's own mode is still NULL and wifiDisconnect()'s WiFi.mode(WIFI_OFF)
    // does nothing. Without an explicit deinit the driver kept ~60 KB of
    // internal DRAM after leaving this screen (measured: 83,823 -> 23,779
    // bytes free), and the next radio - the BLE tap - could never start.
    wifiDisconnect();
    esp_err_t e = esp_wifi_deinit();
    if (e != ESP_OK && e != ESP_ERR_WIFI_NOT_INIT)
        Serial.printf("[PktMon] wifi_deinit: %s\n", esp_err_to_name(e));
    if (pvWifiEventLoopOurs) {
        esp_event_loop_delete_default();
        pvWifiEventLoopOurs = false;
    }
    pvWifiUp = false;
    if (pvPcap) {
        heap_caps_free(pvPcap);
        pvPcap = nullptr;
    }
}

// Hop the WiFi channel periodically. Never touches the user's other radios.
static void pvWifiHop() {
    if (!pvWifiUp || pvPaused) return;
    static uint32_t lastHop = 0;
    if (millis() - lastHop < 350) return;
    lastHop = millis();
    pvWifiChIdx = (uint8_t)((pvWifiChIdx + 1) % (sizeof(PV_WIFI_CHANNELS) / sizeof(PV_WIFI_CHANNELS[0])));
    pvWifiCh = PV_WIFI_CHANNELS[pvWifiChIdx];
    esp_wifi_set_channel(pvWifiCh, WIFI_SECOND_CHAN_NONE);
}

// ===========================================================================
// BLE tap (reuses the tested capture engine)
// ===========================================================================

static int pvBleSeen = 0;
static bool pvBleInit = false;

static void pvBleName(const uint8_t *data, uint8_t len, char *out, size_t n) {
    out[0] = 0;
    for (int b = 0; b + 1 < len;) {
        uint8_t adLen = data[b];
        if (adLen == 0) break;
        if (b + adLen >= BLE_CAP_MAX_PAYLOAD + 1) break;
        uint8_t adType = data[b + 1];
        if ((adType == 0x08 || adType == 0x09) && adLen >= 2) {
            int nameLen = adLen - 1;
            if (nameLen > (int)n - 1) nameLen = n - 1;
            memcpy(out, &data[b + 2], nameLen);
            out[nameLen] = 0;
            return;
        }
        b += adLen + 1;
    }
}

static bool pvBleStart() {
    pvBleSeen = 0;
    // NimBLE needs ~56 KB of free internal DRAM; radioHasMemForBle() also drops
    // Wi-Fi first, because only one of the two radios can be up on this board.
    if (!BLEDevice::isInitialized() && !radioHasMemForBle()) {
        Serial.printf(
            "[PktMon] not enough internal RAM for the BLE stack (free %u, largest DMA %u)\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA)
        );
    }
    pvBleInit = bleCaptureStart(30000);
    return pvBleInit;
}

static void pvBlePoll() {
    if (!pvBleInit) return;
    if (!bleCaptureRunning()) {
        // Scan window ended - start another one and keep going.
        pvBleSeen = 0;
        if (!bleCaptureStart(30000)) pvBleInit = false;
        return;
    }
    int n = bleCaptureCount();
    for (int i = pvBleSeen; i < n; i++) {
        BleCapturedPacket p;
        if (!bleCapturePacket(i, &p)) continue;
        char name[20];
        pvBleName(p.data, p.len, name, sizeof(name));
        pvFeed(PKT_SRC_BLE, -1, p.addr, PV_CAT_ADV, p.len, p.rssi, "ADV", name);
    }
    if (n > pvBleSeen) pvBleSeen = n;
}

static void pvBleStop() {
    if (pvBleInit) bleCaptureStop();
    pvBleInit = false;
}

// ===========================================================================
// NRF24 tap (background task; pseudo-promiscuous, channel hopping)
// ===========================================================================

static TaskHandle_t pvNrfTask = nullptr;
static volatile bool pvNrfRun = false;

static void pvNrfTaskFn(void *) {
    NrfCaptureConfig cfg;
    cfg.startChannel = 0;
    cfg.endChannel = 125;
    cfg.dataRate = NRF_RATE_1MBPS;
    cfg.power = NRF_PWR_MAX;
    cfg.promiscuous = true;
    cfg.hop = true;

    if (nrfCaptureInit(cfg)) {
        while (pvNrfRun) {
            nrfCaptureClear();
            nrfCaptureRun(400, true, 25);
            int n = nrfCaptureCount();
            for (int i = 0; i < n; i++) {
                NrfCapturedPacket p;
                if (!nrfCapturePacket(i, &p)) continue;
                pvFeed(PKT_SRC_NRF24, p.channel, nullptr, PV_CAT_OTHER, p.len, 0, "NRF", "");
            }
        }
        nrfCaptureRestoreNormal();
    }
    pvNrfTask = nullptr;
    vTaskDelete(nullptr);
}

static bool pvNrfStart() {
    if (pvNrfTask) return true;
    pvNrfRun = true;
    if (xTaskCreatePinnedToCore(
            pvNrfTaskFn, "pvNrf", 4096, nullptr, 1, &pvNrfTask, 1
        ) != pdPASS) {
        pvNrfRun = false;
        pvNrfTask = nullptr;
        return false;
    }
    return true;
}

static void pvNrfStop() {
    if (!pvNrfTask) return;
    pvNrfRun = false;
    for (int i = 0; i < 300 && pvNrfTask != nullptr; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (pvNrfTask) {
        vTaskDelete(pvNrfTask);
        pvNrfTask = nullptr;
    }
}

// ===========================================================================
// Sub-GHz tap (CC1101 pulse/edge detection on GDO0)
// ===========================================================================

static volatile unsigned long pvSubLastEdge = 0;
static volatile unsigned long pvSubPeriod = 0;
static volatile bool pvSubNewPulse = false;

// Minimal, ISR-safe: no flash access, no C++ objects.
static void IRAM_ATTR pvSubPulse() {
    unsigned long now = micros();
    unsigned long period = now - pvSubLastEdge;
    pvSubLastEdge = now;
    if (period >= 20 && period <= 1000000UL) {
        pvSubPeriod = period;
        pvSubNewPulse = true;
    }
}

static bool pvSubStart() {
    if (bruceConfigPins.rfModule != CC1101_SPI_MODULE) return false;
    if (!initRfModule("rx", 433.92)) return false;
    ELECHOUSE_cc1101.setRxBW(58);
    ELECHOUSE_cc1101.setModulation(2);
    ELECHOUSE_cc1101.setDcFilterOff(true);
    pvSubLastEdge = micros();
    pvSubNewPulse = false;
    attachInterrupt(digitalPinToInterrupt(bruceConfigPins.CC1101_bus.io0), pvSubPulse, RISING);
    return true;
}

static void pvSubStop() {
    detachInterrupt(digitalPinToInterrupt(bruceConfigPins.CC1101_bus.io0));
    deinitRfModule();
}

// Accumulate pulses into one "packet" per burst (a gap of >60 ms ends it).
static void pvSubPoll() {
    static uint32_t lastPulseMs = 0;
    static int pulseCount = 0;
    static float lastFreq = 0;

    if (pvSubNewPulse) {
        pvSubNewPulse = false;
        lastPulseMs = millis();
        unsigned long dur = pvSubPeriod;
        lastFreq = dur ? (1000000.0f / dur) : 0;
        pulseCount++;
    }
    if (pulseCount > 0 && (millis() - lastPulseMs) > 60) {
        int16_t ch = (int16_t)(lastFreq / 1000.0f); // instantaneous frequency in MHz
        char label[10];
        snprintf(label, sizeof(label), "PULSE");
        char text[20];
        snprintf(text, sizeof(text), "%.1fkHz", lastFreq / 1000.0f);
        pvFeed(PKT_SRC_SUBGHZ, ch, nullptr, PV_CAT_OTHER, (uint16_t)pulseCount, 0, label, text);
        pulseCount = 0;
    }
}

// ===========================================================================
// Source lifecycle
// ===========================================================================

static PacketSource pvActive = PKT_SRC_WIFI;
static bool pvActiveOn = false;

static void pvStopSource(PacketSource s) {
    switch (s) {
        case PKT_SRC_WIFI: pvWifiStop(); break;
        case PKT_SRC_BLE: pvBleStop(); break;
        case PKT_SRC_NRF24: pvNrfStop(); break;
        case PKT_SRC_SUBGHZ: pvSubStop(); break;
    }
}

static void pvStartSource(PacketSource s) {
    bool ok = false;
    switch (s) {
        case PKT_SRC_WIFI: ok = pvWifiStart(); break;
        case PKT_SRC_BLE: ok = pvBleStart(); break;
        case PKT_SRC_NRF24: ok = pvNrfStart(); break;
        case PKT_SRC_SUBGHZ: ok = pvSubStart(); break;
    }
    pvActiveOn = ok;
    if (!ok) { displayError(String("No ") + pvSourceName(s) + " radio", true); }
}

static void pvPollSource() {
    if (!pvActiveOn) return;
    switch (pvActive) {
        case PKT_SRC_WIFI: pvWifiHop(); break;
        case PKT_SRC_BLE: pvBlePoll(); break;
        case PKT_SRC_SUBGHZ: pvSubPoll(); break;
        case PKT_SRC_NRF24: break; // the task feeds the ring itself
    }
}

// ===========================================================================
// Filters / Counters / Export screens
// ===========================================================================

static void pvShowThenWait(const String &title, const std::vector<String> &lines, const String &note) {
    drawMainBorder(true);
    pvDrawLine(title, 10, 28, bruceConfig.priColor);
    tft.drawFastHLine(10, 38, tftWidth - 20, bruceConfig.priColor);

    int y = 44;
    for (const String &l : lines) {
        pvDrawLine(l, 10, y, bruceConfig.secColor);
        y += 13;
    }
    tft.drawFastHLine(10, tftHeight - 30, tftWidth - 20, bruceConfig.priColor);
    // Body text uses secColor: the default theme's priColor (0xA80F) is magenta
    // and small magenta text on black is hard to read.
    pvDrawLine(note, 10, tftHeight - 25, bruceConfig.secColor);
    pvDrawLine("BACK to return", 10, tftHeight - 14, bruceConfig.secColor);

    while (!check(AnyKeyPress)) vTaskDelay(pdMS_TO_TICKS(10));
    check(AnyKeyPress);
}

static void pvCountersScreen(PacketSource s) {
    (void)s;
    portENTER_CRITICAL(&pvMux);
    uint32_t st[4] = {pvSrcTotal[0], pvSrcTotal[1], pvSrcTotal[2], pvSrcTotal[3]};
    uint32_t ct[PV_CAT_COUNT] = {pvCatTotal[0], pvCatTotal[1], pvCatTotal[2], pvCatTotal[3], pvCatTotal[4]};
    portEXIT_CRITICAL(&pvMux);

    uint32_t total = st[0] + st[1] + st[2] + st[3];
    int busy = pvBusiestChannel();

    // Coarse histogram of the six busiest channels (packed as "ch:n" pairs).
    String hist = "";
    for (int ch = 0; ch < 128; ch++) {
        if (pvChHist[ch] == 0) continue;
        if (hist.length() < 22) hist += String(ch) + ":" + String(pvChHist[ch]) + " ";
    }

    std::vector<String> lines = {
        String("Source       : ") + pvSourceName(s),
        String("Packets      : ") + String(total),
        String("Packets/sec  : ") + String(pvPps, 1),
        String("Busiest ch   : ") + String(busy),
        String("Per radio    : WIFI ") + String(st[0]) + "  BLE " + String(st[1]) + "  NRF " + String(st[2]) +
            "  SUB " + String(st[3]),
        String("Per type     : mgmt ") + String(ct[PV_CAT_MGMT]) + "  data " + String(ct[PV_CAT_DATA]) + "  ctrl " +
            String(ct[PV_CAT_CTRL]) + "  adv " + String(ct[PV_CAT_ADV]),
        String("Channel hist : ") + (hist.length() ? hist : "no data yet"),
    };
    pvShowThenWait("COUNTERS", lines, String("Live totals for this session"));
}

static void pvFiltersMenu(PacketSource s) {
    (void)s;
    std::vector<Option> opts = {
        {String("Radio: ") + (pvFilter.radio < 0 ? "Any" : pvSourceName((PacketSource)pvFilter.radio)),
         []() { pvFilter.radio = pvFilter.radio >= 3 ? -1 : pvFilter.radio + 1; pvDirty = true; }},
        {String("Type: ") + (pvFilter.cat < 0 ? "Any" : pvCatName(pvFilter.cat)),
         []() { pvFilter.cat = pvFilter.cat >= PV_CAT_COUNT - 1 ? -1 : pvFilter.cat + 1; pvDirty = true; }},
        {String("MAC/OUI: ") + (pvFilter.hasOui ? String("set") : "--"),
         []() {
             String v = keyboard("", 17, "OUI (AA:BB:CC) or blank:");
             if (v == "\x1B") return;
             v.trim();
             if (v.length() == 0) { pvFilter.hasOui = false; pvDirty = true; return; }
             unsigned int a[3] = {0, 0, 0};
             if (sscanf(v.c_str(), "%x:%x:%x", &a[0], &a[1], &a[2]) == 3) {
                 for (int i = 0; i < 3; i++) pvFilter.oui[i] = (uint8_t)a[i];
                 pvFilter.hasOui = true;
                 pvDirty = true;
             } else {
                 displayError("Bad OUI", true);
             }
         }},
        {String("Channel: ") + (pvFilter.channel < 0 ? "Any" : String(pvFilter.channel)),
         []() {
             String v = keyboard("", 5, "Channel (-1 = any):");
             if (v == "\x1B") return;
             v.trim();
             int c = v.toInt();
             pvFilter.channel = (v.length() == 0 || c < 0) ? -1 : c;
             pvDirty = true;
         }},
        {String("Text: ") + (pvFilter.text.length() ? pvFilter.text.substring(0, 10) : "--"),
         []() {
             String v = keyboard(pvFilter.text, 18, "Search text (SSID/name):");
             if (v == "\x1B") return;
             pvFilter.text = v;
             pvDirty = true;
         }},
        {String("Only new: ") + (pvFilter.onlyNew ? "On" : "Off"),
         []() { pvFilter.onlyNew = !pvFilter.onlyNew; pvDirty = true; }},
        {"Reset filters",
         []() {
             pvFilter = PvFilter();
             pvDirty = true;
         }},
        {"Back", []() { yield(); }},
    };
    loopOptions(opts, MENU_TYPE_SUBMENU, "Filters");
}

static String pvDestFor(bool pcap) { return pcap ? "/packets.pcap" : "/packets.csv"; }

static bool pvExportCsv(const String &path) {
    FS *fs = setupSdCard() ? (FS *)&SD : (FS *)&LittleFS;
    if (!fs) return false;
    File f = fs->open(path, FILE_WRITE);
    if (!f) return false;
    f.println("time_ms,radio,channel,address,type,category,len,rssi,text");
    uint32_t n = pvAvailable();
    for (uint32_t i = 0; i < n; i++) {
        PvEntry e;
        if (!pvGetLogical(i, &e)) continue;
        char addr[20];
        pvAddrString(e.addr, addr, sizeof(addr));
        String line = String(e.t) + "," + pvSourceShort((PacketSource)e.src) + "," + String(e.ch) + "," + addr + "," +
                      e.label + "," + pvCatName(e.cat) + "," + String(e.len) + "," + String(e.rssi) + "," + e.text;
        f.println(line);
    }
    f.close();
    return true;
}

static bool pvExportPcap(const String &path) {
    if (!pvPcap || pvPcapTotal == 0) return false;
    FS *fs = setupSdCard() ? (FS *)&SD : (FS *)&LittleFS;
    if (!fs) return false;
    File f = fs->open(path, FILE_WRITE);
    if (!f) return false;

    // Standard pcap global header, link type 105 (IEEE 802.11).
    uint32_t magic = 0xa1b2c3d4, thiszone = 0, sigfigs = 0, snaplen = PV_PCAP_SNAP, network = 105;
    uint16_t vmaj = 2, vmin = 4;
    f.write((uint8_t *)&magic, 4);
    f.write((uint8_t *)&vmaj, 2);
    f.write((uint8_t *)&vmin, 2);
    f.write((uint8_t *)&thiszone, 4);
    f.write((uint8_t *)&sigfigs, 4);
    f.write((uint8_t *)&snaplen, 4);
    f.write((uint8_t *)&network, 4);

    uint32_t avail = pvPcapTotal < PV_PCAP_MAX ? pvPcapTotal : PV_PCAP_MAX;
    for (uint32_t i = 0; i < avail; i++) {
        uint16_t slot = (uint16_t)((pvPcapWrite + PV_PCAP_MAX - avail + i) % PV_PCAP_MAX);
        PvPcapSlot &s = pvPcap[slot];
        if (s.len == 0) continue;
        uint32_t incl = s.len, orig = s.len;
        f.write((uint8_t *)&s.sec, 4);
        f.write((uint8_t *)&s.usec, 4);
        f.write((uint8_t *)&incl, 4);
        f.write((uint8_t *)&orig, 4);
        f.write(s.data, s.len);
    }
    f.close();
    return true;
}

static void pvExportMenu(PacketSource s) {
    (void)s;
    static bool pcap = false;
    // The approved phase-1 rows are kept verbatim: CSV / PCAP / Destination /
    // Export now / Back. The two Format rows select the active format and the
    // Destination row reflects it.
    std::vector<Option> opts = {
        {"Format: CSV", []() { pcap = false; }                                   },
        {"Format: PCAP", []() { pcap = true; }                                  },
        {String("Destination: ") + pvDestFor(pcap), []() { displayInfo("Fixed path for now", true); }},
        {"Export now",
         []() {
             String path = pvDestFor(pcap);
             bool ok = pcap ? pvExportPcap(path) : pvExportCsv(path);
             if (ok) displayInfo(String("Saved ") + path, true);
             else displayError(pcap ? "No WiFi frames for PCAP" : "Export failed", true);
         }                                                                         },
        {"Back", []() { yield(); }                                              },
    };
    loopOptions(opts, MENU_TYPE_SUBMENU, "Export");
}

// ===========================================================================
// Dashboard
// ===========================================================================

static int pvSel = 0; // index into the visible (newest-first) list
static int pvWin = 0; // first visible list index drawn

// Full repaint (clear the screen) is needed whenever something else has drawn
// over the dashboard: opening the monitor, and every time a submenu closes.
// It must not be a function-local static, or the second time the monitor is
// opened the border/background are never cleared and the previous screen shows
// through the live rows (unreadable overlay).
static bool pvFullPaint = true;

static void pvDrawDashboard(PacketSource source, int sel, int win, std::vector<int> &vis) {
    // Clear only on a full paint; live repaints overwrite the same padded rows
    // in place, which keeps the USB screen stream from flooding.
    drawMainBorder(pvFullPaint);
    pvFullPaint = false;

    pvDrawLine("PACKET MONITOR", 10, 28, bruceConfig.priColor);
    tft.setTextSize(FP);
    tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
    tft.drawRightString(pvSourceName(source), tftWidth - 10, 28, 1);
    tft.drawFastHLine(10, 38, tftWidth - 20, bruceConfig.priColor);

    // secColor for the column header too - priColor is magenta by default.
    pvDrawLine(pvHeaderRow(), 8, 41, bruceConfig.secColor);

    for (int r = 0; r < PV_ROWS; r++) {
        int y = 54 + r * 13;
        int listIdx = win + r;
        bool selected = (listIdx == sel);
        String line;
        if (listIdx < (int)vis.size()) {
            PvEntry e;
            if (pvGetLogical(vis[listIdx], &e)) line = pvRowString(e);
            else line = pvPadR("", 56);
        } else {
            line = pvPadR("", 56);
        }
        String prefixed = (selected ? ">" : " ") + line;
        // Clear the whole row band first: the highlight bar is wider than the
        // text, so without this a moved highlight leaves a magenta smear that
        // makes the neighbouring rows hard to read.
        tft.fillRect(6, y - 2, tftWidth - 12, 12, bruceConfig.bgColor);
        if (selected) {
            tft.fillRect(6, y - 2, tftWidth - 12, 12, bruceConfig.secColor);
            tft.setTextSize(FP);
            tft.setTextColor(bruceConfig.bgColor, bruceConfig.secColor);
            tft.setCursor(8, y);
            tft.print(prefixed);
        } else {
            pvDrawLine(prefixed, 8, y, bruceConfig.secColor);
        }
    }

    tft.drawFastHLine(10, tftHeight - 34, tftWidth - 20, bruceConfig.priColor);

    portENTER_CRITICAL(&pvMux);
    uint32_t total = pvSrcTotal[0] + pvSrcTotal[1] + pvSrcTotal[2] + pvSrcTotal[3];
    portEXIT_CRITICAL(&pvMux);

    String ch = (source == PKT_SRC_WIFI) ? String(pvWifiCh) : "--";
    String status = "Pkts " + String(total) + "   P/s " + String(pvPps, 1) + "   Ch " + ch + "   " +
                    String(pvPaused ? "PAUSED" : "RUN");
    pvDrawLine(status, 10, tftHeight - 30, bruceConfig.secColor);

    int shown = (int)vis.size();
    String shownLine = String("showing ") + String(shown) + (pvActiveOn ? "" : "   (radio off)");
    pvDrawLine(shownLine, 10, tftHeight - 20, bruceConfig.secColor);
    pvDrawLine("TURN scroll    SEL menu    BACK exit", 10, tftHeight - 10, bruceConfig.secColor);
}

static void pvDashboardMenu(PacketSource &source, bool &paused, bool &needRestart, bool &needClear) {
    std::vector<Option> opts = {
        {"Filters", [&]() { pvFiltersMenu(source); }},
        {"Counters", [&]() { pvCountersScreen(source); }},
        {"Export", [&]() { pvExportMenu(source); }},
        {paused ? "Resume" : "Pause", [&]() { paused = !paused; }},
        {"Change source",
         [&]() {
             int next = ((int)source + 1) % 4;
             source = (PacketSource)next;
             needRestart = true;
         }},
        {"Clear", [&]() { needClear = true; }},
        {"Close menu", [&]() { yield(); }},
    };
    loopOptions(opts, MENU_TYPE_SUBMENU, "Packet Monitor");
}

void packetMonitor(PacketSource source) {
    pvFullPaint = true;
    pvFilter = PvFilter();
    pvPaused = false;
    pvSel = 0;
    pvWin = 0;
    pvResetAll();

    pvActive = source;
    pvStartSource(source);

    bool redraw = true;
    uint32_t lastDraw = 0;
    std::vector<int> vis;

    while (true) {
        pvPollSource();

        if (!pvPaused) {
            portENTER_CRITICAL(&pvMux);
            uint32_t seq = pvSeq;
            portEXIT_CRITICAL(&pvMux);
            if (seq != pvDirtySeq) {
                pvDirtySeq = seq;
                pvDirty = true;
            }
        }

        // packets/second, sampled once per second
        uint32_t now = millis();
        if (now - pvLastSampleMs >= 1000) {
            portENTER_CRITICAL(&pvMux);
            uint32_t seq = pvSeq;
            portEXIT_CRITICAL(&pvMux);
            pvPps = (seq - pvLastSampleTotal) * 1000.0f / (float)(now - pvLastSampleMs);
            pvLastSampleMs = now;
            pvLastSampleTotal = seq;
            if (!pvPaused) pvDirty = true;
        }

        if (check(EscPress)) break;

        if (check(PrevPress) || check(UpPress)) {
            if (pvSel > 0) pvSel--;
            redraw = true;
        }
        if (check(NextPress) || check(DownPress)) {
            pvSel++;
            redraw = true;
        }
        if (check(SelPress)) {
            while (check(SelPress)) vTaskDelay(pdMS_TO_TICKS(1));
            bool needRestart = false, needClear = false;
            pvDashboardMenu(source, pvPaused, needRestart, needClear);
            if (needClear) {
                pvResetAll();
                pvSel = 0;
                pvWin = 0;
            }
            if (needRestart) {
                pvStopSource(pvActive);
                pvActive = source;
                pvSel = 0;
                pvWin = 0;
                pvResetAll();
                pvStartSource(source);
            }
            check(AnyKeyPress);
            // loopOptions() painted the menu over the dashboard: without a full
            // repaint the half-overwritten list rows stay on screen.
            pvFullPaint = true;
            redraw = true;
        }

        // Live data repaints are throttled: the mirror streams every changed
        // cell, so a full redraw per packet would lag the whole UI. Key-driven
        // navigation still redraws immediately.
        bool liveThrottle = (millis() - lastDraw) >= (pvPaused ? 150 : 300);
        if (redraw || (pvDirty && liveThrottle)) {
            vis = pvVisibleLogical();
            if (pvSel >= (int)vis.size()) pvSel = vis.empty() ? 0 : (int)vis.size() - 1;
            if (pvSel < 0) pvSel = 0;
            if (pvSel < pvWin) pvWin = pvSel;
            if (pvSel >= pvWin + PV_ROWS) pvWin = pvSel - PV_ROWS + 1;
            if (pvWin < 0) pvWin = 0;
            pvDrawDashboard(source, pvSel, pvWin, vis);
            redraw = false;
            pvDirty = false;
            lastDraw = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }

    pvStopSource(pvActive);
    pvActiveOn = false;
    check(AnyKeyPress);
}
