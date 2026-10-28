#include "core/wifi/wifi_common.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/powerSave.h"
#include "core/radio_mem.h"
#include "core/ram_profile.h"
#include "core/settings.h"
#include "core/utils.h"
#include "core/wifi/webInterface.h"
#include "core/wifi/wifi_mac.h"
#include "esp_wifi.h"
#include "modules/ble/ble_common.h"
#include <esp_event.h>
#include <esp_netif.h>
#include <globals.h>

static TaskHandle_t timezoneTaskHandle = NULL;
static bool wifiTransitioning = false;

// Scan dwell. The Arduino default is min 100 ms / max 300 ms per channel, and
// the driver waits the MAX on every channel of a 13-channel 2.4 GHz sweep, so a
// scan blocks for ~3.9 s (measured 3.81 s on the board). The MAX is therefore
// the only knob that matters; 150 ms brings the same sweep to ~2.4 s (measured,
// repeatedly) at the cost of the weakest AP: a -90 dBm neighbour appeared in
// 3 of 5 sweeps at 150 ms and in 4 of 4 at 300 ms, so a list that looks short
// needs one more sweep (ESC refreshes). Passive scanning at 200 ms was measured
// too - 3.0 s and no better discovery - so active at 150 ms is the setting.
// The hidden-SSID flag is passed through: it changes what is reported, never
// how long the sweep takes.
#define WIFI_SCAN_MIN_MS 100
#define WIFI_SCAN_MAX_MS 150

int wifiScanNets(bool showHidden) {
    static bool timingsSet = false;
    if (!timingsSet) {
        WiFi.setScanActiveMinTime(WIFI_SCAN_MIN_MS);
        timingsSet = true;
    }
    int16_t nets = WiFi.scanNetworks(false, showHidden, false, WIFI_SCAN_MAX_MS);
    return nets > 0 ? (int)nets : 0;
}

// ---------------------------------------------------------------------------
// Streaming scan: one channel per poll
// ---------------------------------------------------------------------------
// Why not one sweep: the library publishes its AP list in the scan-done handler,
// so during a sweep every WiFi.SSID(i) is empty and the page can only draw once
// the last channel is finished. Scanning a single channel per call puts real
// results in hand after every channel, i.e. the first network is on screen after
// ~150-300 ms and the rest fill in behind it.
//
// The visit order is deliberate: 1/6/11 are the three non-overlapping carriers
// a real deployment uses, so the networks that matter are usually in the first
// half second, and the overlapping ones arrive after.
#define WIFI_SCAN_STREAM_MS 130
#define WIFI_SCAN_APS_MAX 48

static const uint8_t wifiScanChOrder[] = {1, 6, 11, 2, 7, 12, 3, 8, 13, 4, 9, 5, 10};
#define WIFI_SCAN_CH_N ((int)(sizeof(wifiScanChOrder) / sizeof(wifiScanChOrder[0])))

// Allocated on first use (PSRAM when the board has it): the table is only live
// while a scan page is open, so it does not belong in .bss.
static WifiScanAp *wifiScanAps = nullptr;
static int wifiScanApsFound = 0;
static int wifiScanChIdx = 0;
static bool wifiScanInFlight = false;
static bool wifiScanShowHidden = false;

static bool wifiScanApsEnsure() {
    if (wifiScanAps) return true;
    const size_t bytes = sizeof(WifiScanAp) * WIFI_SCAN_APS_MAX;
    wifiScanAps = (WifiScanAp *)(psramFound() ? ps_malloc(bytes) : malloc(bytes));
    return wifiScanAps != nullptr;
}

bool wifiScanStreamStart(bool showHidden) {
    if (!wifiScanApsEnsure()) return false;
    wifiScanShowHidden = showHidden;
    wifiScanApsFound = 0;
    wifiScanChIdx = 0;
    wifiScanInFlight = true;
    return true;
}

static void wifiScanApAdd(const String &ssid, const uint8_t *bssid, int rssi, int ch, int enc) {
    if (wifiScanApsFound >= WIFI_SCAN_APS_MAX) return;
    // One row per AP: a beacon heard twice inside a channel's dwell, or a hidden
    // network that answers twice, must not add a second entry.
    for (int i = 0; i < wifiScanApsFound; i++) {
        if (memcmp(wifiScanAps[i].bssid, bssid, 6) == 0) return;
    }
    WifiScanAp &ap = wifiScanAps[wifiScanApsFound++];
    memset(&ap, 0, sizeof(ap));
    strncpy(ap.ssid, ssid.c_str(), sizeof(ap.ssid) - 1);
    memcpy(ap.bssid, bssid, 6);
    ap.rssi = (int8_t)(rssi < -127 ? -127 : (rssi > 0 ? 0 : rssi));
    ap.ch = (uint8_t)ch;
    ap.enc = (uint8_t)enc;
}

int wifiScanStreamPoll() {
    if (!wifiScanInFlight) return wifiScanApsFound;
    if (wifiScanChIdx >= WIFI_SCAN_CH_N) {
        wifiScanInFlight = false;
        return wifiScanApsFound;
    }

    const uint8_t ch = wifiScanChOrder[wifiScanChIdx++];
    const int n = WiFi.scanNetworks(false, wifiScanShowHidden, false, WIFI_SCAN_STREAM_MS, ch);
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            const uint8_t *bssid = WiFi.BSSID(i);
            if (bssid == nullptr) continue;
            wifiScanApAdd(WiFi.SSID(i), bssid, WiFi.RSSI(i), WiFi.channel(i), (int)WiFi.encryptionType(i));
        }
    }
    // The library's list belongs to this channel; the collected rows live in our
    // own table, so nothing is lost by dropping it here.
    WiFi.scanDelete();

    if (wifiScanChIdx >= WIFI_SCAN_CH_N) wifiScanInFlight = false;
    return wifiScanApsFound;
}

bool wifiScanStreamDone() { return !wifiScanInFlight; }
int wifiScanStreamVisited() { return wifiScanChIdx; }
int wifiScanStreamChannelCount() { return WIFI_SCAN_CH_N; }

void wifiScanStreamStop() {
    wifiScanInFlight = false;
    WiFi.scanDelete();
}

int wifiScanStreamCount() { return wifiScanApsFound; }

const WifiScanAp *wifiScanStreamAp(int index) {
    if (!wifiScanAps || index < 0 || index >= wifiScanApsFound) return nullptr;
    return &wifiScanAps[index];
}

void wifiScanStreamSortByRssi() {
    if (!wifiScanAps) return;
    for (int i = 1; i < wifiScanApsFound; i++) { // insertion sort, <= 48 entries
        const WifiScanAp key = wifiScanAps[i];
        int j = i - 1;
        while (j >= 0 && wifiScanAps[j].rssi < key.rssi) {
            wifiScanAps[j + 1] = wifiScanAps[j];
            j--;
        }
        wifiScanAps[j + 1] = key;
    }
}

// Wi-Fi and BLE share the same DMA-capable internal DRAM, and this board does
// not have enough of it for both at once. When a Wi-Fi bring-up is refused for
// lack of a contiguous block, BLE is almost always the reason; give its memory
// back and retry once before showing the "Low RAM" error. This is what lets
// NetCut and the other Wi-Fi tools run after a BLE feature in the same boot
// instead of failing on RAM until the next reboot. stopBLEStack() is a no-op
// when BLE is not up, so it is safe to call unconditionally here.
bool wifiEnsureRadioMem() {
    if (radioHasMemForWifi()) return true;
    Serial.printf(
        "[RAM] Low memory for WiFi (free internal %u, DMA block %u), releasing BLE...\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA)
    );
    // BLE is the usual reason the contiguous DMA block is gone, but on this
    // board the Web UI's ports and any stale capture/promiscuous session also
    // hold it. Free all three before giving up: this is what lets the scripted
    // interception path arm while a BLE feature was used earlier in the boot.
    stopBLEStack();
    cleanlyStopWebUiForWiFiFeature();
    vTaskDelay(150 / portTICK_PERIOD_MS);
    if (radioHasMemForWifi()) return true;

    // Last resort: a promiscuous capture left running pins driver RX buffers.
    // Stopping it is safe here (the caller is about to bring Wi-Fi up anyway).
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    vTaskDelay(120 / portTICK_PERIOD_MS);
    return radioHasMemForWifi();
}

esp_err_t wifiRawTx(wifi_interface_t ifx, const void *frame, int len, uint8_t retries) {
    esp_err_t err = esp_wifi_80211_tx(ifx, frame, len, false);
    for (uint8_t i = 0; err == ESP_ERR_NO_MEM && i < retries; i++) {
        vTaskDelay(1); // let the driver drain TX buffers and retry
        err = esp_wifi_80211_tx(ifx, frame, len, false);
    }
    return err;
}

void ensureWifiPlatform() {
    static bool netifInitialized = false;
    static bool eventLoopCreated = false;
    static portMUX_TYPE platformMux = portMUX_INITIALIZER_UNLOCKED;

    portENTER_CRITICAL(&platformMux);
    bool needNetif = !netifInitialized;
    bool needLoop = !eventLoopCreated;
    portEXIT_CRITICAL(&platformMux);

    if (needNetif) {
        ESP_ERROR_CHECK(esp_netif_init());
        portENTER_CRITICAL(&platformMux);
        netifInitialized = true;
        portEXIT_CRITICAL(&platformMux);
    }

    if (needLoop) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_ERR_INVALID_STATE) { ESP_ERROR_CHECK(err); }
        portENTER_CRITICAL(&platformMux);
        eventLoopCreated = true;
        portEXIT_CRITICAL(&platformMux);
    }
}

// ---------------------------------------------------------------------------
// Scan page
// ---------------------------------------------------------------------------
// The old page printed "Scanning.." and then blocked for the whole sweep, so
// nothing could be drawn until the last channel was done (the library only
// publishes its AP list in the scan-done handler). This one polls one channel at
// a time and appends a row the moment a network is found: the first SSID is on
// screen after ~150-300 ms, and Sel stops waiting for the remaining channels.

static String wifiEncName(int type) {
    switch (type) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA/PSK";
        case WIFI_AUTH_WPA2_PSK: return "WPA2/PSK";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2/PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2/Enterprise";
        case WIFI_AUTH_WPA3_PSK: return "WPA3/PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3/PSK";
        default: return "Unknown";
    }
}

// One live row: "#SSID  -45 ch6". The RSSI and channel are what identify a row on
// a narrow screen, so the SSID is what gets shortened when it does not fit.
static String wifiScanRowText(const WifiScanAp &ap) {
    const String prefix = (ap.enc == WIFI_AUTH_OPEN) ? "" : "#";
    const String tail = " " + String(ap.rssi) + " ch" + String(ap.ch);
    const int maxWidth = tftWidth - 2 * BORDER_PAD_X;
    String ssid = (ap.ssid[0] == '\0') ? String("(hidden)") : String(ap.ssid);
    while (ssid.length() > 2 && tft.textWidth(prefix + ssid + tail, FP) > maxWidth) ssid.remove(ssid.length() - 1);
    return prefix + ssid + tail;
}

// What the scan page + picker handed back to wifiConnectMenu().
enum {
    WIFI_PICK_BACK,   // the user left (Main Menu / Back): keep the caller's policy
    WIFI_PICK_NET,    // outIdx indexes the collected table, strongest first
    WIFI_PICK_HIDDEN, // the user wants to type an SSID by hand
    WIFI_PICK_RESCAN, // Esc in the picker: run a fresh sweep
};

static int wifiScanNetworkMenu(int &outIdx) {
    outIdx = -1;
    if (!wifiScanStreamStart(false)) {
        displayError("Low RAM: free BLE/SD first", true);
        return WIFI_PICK_BACK;
    }

    drawMainBorderWithTitle("WiFi Scan");
    const int rowH = tft.fontHeight(FP) + 2;
    const int statusY = BORDER_PAD_Y + FM * LH + 4; // under the title (see 76)
    const int listTop = statusY + rowH + 4;
    int drawn = 0;
    bool stopEarly = false;

    while (true) {
        if (!wifiScanStreamDone()) {
            wifiScanStreamPoll(); // one channel; the rows below land ~200 ms later
            stopEarly = check(SelPress);
        }

        const int found = wifiScanStreamCount();
        tft.fillRect(0, statusY, tftWidth, rowH, bruceConfig.bgColor);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.setTextSize(FP);
        tft.setCursor(BORDER_PAD_X, statusY);
        const String netsTxt = String(found) + (found == 1 ? " net" : " nets");
        if (wifiScanStreamDone() || stopEarly)
            tft.print(netsTxt + " - Sel: choose");
        else
            tft.print(
                "ch " + String(wifiScanStreamVisited()) + "/" + String(wifiScanStreamChannelCount()) + " - " + netsTxt
            );

        // Append-only painting: a row is drawn once, when its network is found, so
        // the list grows downward without the flicker a clear-and-repaint per
        // channel would cause.
        while (drawn < found) {
            const WifiScanAp *ap = wifiScanStreamAp(drawn);
            const int y = listTop + drawn * rowH;
            if (ap == nullptr || y + rowH > tftHeight - 4) {
                drawn = found; // no room left: the picker below lists the rest
                break;
            }
            tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
            tft.setCursor(BORDER_PAD_X, y);
            tft.print(wifiScanRowText(*ap));
            drawn++;
        }

        if (stopEarly || wifiScanStreamDone()) break;

        // Esc stops waiting for the remaining channels and opens the picker with
        // what is already there (Esc in the picker is the rescan).
        if (check(EscPress)) { stopEarly = true; break; }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    wifiScanStreamStop();
    wifiScanStreamSortByRssi();

    int selIdx = -1;
    bool selHidden = false;

    options = {};
    for (int i = 0; i < wifiScanStreamCount() && options.size() < 250; i++) {
        const WifiScanAp *ap = wifiScanStreamAp(i);
        if (ap == nullptr) break;
        const String encryptionPrefix = (ap->enc == WIFI_AUTH_OPEN) ? "" : "#";
        const String optionText = encryptionPrefix + String(ap->ssid) + "(" + String(ap->rssi) + "|" +
                                  wifiEncName(ap->enc) + "|ch." + String(ap->ch) + ")";
        const int captured = i;
        options.push_back({optionText.c_str(), [&selIdx, captured]() { selIdx = captured; }});
    }
    options.push_back({"Hidden SSID", [&selHidden]() { selHidden = true; }});
    addOptionToMainMenu();

    loopOptions(options);
    options.clear();

    if (returnToMenu) return WIFI_PICK_BACK;
    if (selHidden) return WIFI_PICK_HIDDEN;
    if (selIdx >= 0) {
        outIdx = selIdx;
        return WIFI_PICK_NET;
    }
    if (check(EscPress)) return WIFI_PICK_RESCAN;
    return WIFI_PICK_BACK;
}

bool _wifiConnect(const String &ssid, int encryption, uint8_t channel, const uint8_t *bssid) {
    String password = bruceConfig.getWifiPassword(ssid);
    if (password == "" && encryption > 0) { password = keyboard(password, 63, "Network Password:", true); }
    if (password == "\x1B") return false;
    bool connected = _connectToWifiNetwork(ssid, password, channel, bssid);
    bool retry = false;

    while (!connected) {
        wakeUpScreen();

        options = {
            {"Retry",  [&]() { retry = true; } },
            {"Cancel", [&]() { retry = false; }},
        };
        loopOptions(options);

        if (!retry) {
            wifiDisconnect();
            return false;
        }

        password = keyboard(password, 63, "Network Password:", true);
        if (password == "\x1B") {
            wifiDisconnect();
            return false;
        }
        // Retry without the scan hint: the AP may have moved to another channel
        // since the scan, and a direct association to a stale BSSID fails at once.
        connected = _connectToWifiNetwork(ssid, password);
    }

    if (connected) {
        wifiConnected = true;
        wifiIP = WiFi.localIP().toString();
        bruceConfig.addWifiCredential(ssid, password);

        // Start timezone update in background if not already running
        if (timezoneTaskHandle == NULL) {
            xTaskCreate(updateTimezoneTask, "updateTimezone", 4096, NULL, 1, &timezoneTaskHandle);
        }
    }

    delay(200);
    return connected;
}

bool _connectToWifiNetwork(const String &ssid, const String &pwd, uint8_t channel, const uint8_t *bssid) {
    if (FORCE_RADIO_TEARDOWN_ON_SWITCH) {
        if (BLEConnected) {
            displayWarning("Board with no PSRAM, closing BLE Stack");
            vTaskDelay(700 / portTICK_PERIOD_MS);
        }
        stopBLEStack();
        vTaskDelay(300 / portTICK_PERIOD_MS);
    }

    RAM_LOG("wifi pre-mode"); // Wi-Fi is already up from the menu scan by this point
    drawMainBorderWithTitle("WiFi Connect");
    padprintln("");
    padprint("Connecting to: " + ssid + ".");
    WiFi.mode(WIFI_MODE_STA);
    RAM_LOG("wifi post-mode");
    vTaskDelay(10 / portTICK_PERIOD_MS);
    // A channel + BSSID taken from the scan makes the driver associate directly.
    // Without them it first sweeps all 13 channels looking for the SSID, which is
    // most of the "Connecting to..." wait; with them the association starts on the
    // first try. channel 0 / bssid nullptr is exactly the old behaviour.
    WiFi.begin(ssid, pwd, channel, bssid);

    // Poll every 60 ms instead of the old 500 ms step: the association usually
    // lands in a few hundred ms and this notices as soon as it does, while keeping
    // the same 10 s ceiling and the same one dot per ~half second of printing.
    const unsigned long started = millis();
    unsigned long lastDot = started - 400; // print the first dot immediately
    while (!WiFi.isConnected()) {
        if ((unsigned long)(millis() - lastDot) >= 400) {
            lastDot = millis();
            if (tft.getCursorX() >= tftWidth - 12) {
                padprintln("");
                padprint("");
            }
#ifdef HAS_SCREEN
            tft.print(".");
#else
            Serial.print(".");
#endif
        }

        if ((unsigned long)(millis() - started) > 10000) {
            displayError("Wifi Offline");
            vTaskDelay(500 / portTICK_RATE_MS);
            break;
        }

        vTaskDelay(60 / portTICK_RATE_MS);
    }

    return WiFi.isConnected();
}

bool _setupAP() {
    IPAddress AP_GATEWAY(172, 0, 0, 1);
    WiFi.softAPConfig(AP_GATEWAY, AP_GATEWAY, IPAddress(255, 255, 255, 0));
    WiFi.softAP(bruceConfig.wifiAp.ssid, bruceConfig.wifiAp.pwd, 6, 0, 4, false);
    wifiIP = WiFi.softAPIP().toString(); // update global var
    Serial.println("IP: " + wifiIP);
    wifiConnected = true;
    return true;
}

void wifiDisconnect() {
    wifiTransitioning = true;

    wifi_mode_t mode = WiFi.getMode();
    if (mode & WIFI_MODE_AP) {
        WiFi.softAPdisconnect();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    if (mode & WIFI_MODE_STA) {
        WiFi.disconnect(false, true);
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    if (mode != WIFI_MODE_NULL) {
        WiFi.mode(WIFI_OFF);
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    wifiConnected = false;
    wifiTransitioning = false;
}

bool wifiConnectMenu(wifi_mode_t mode) {
    if (WiFi.isConnected()) return false; // safeguard

    if (FORCE_RADIO_TEARDOWN_ON_SWITCH) {
        stopBLEStack();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    // Check if WiFi is in transition
    if (wifiTransitioning) {
        displayTextLine("WiFi busy, please wait...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        return false;
    }

    switch (mode) {
        case WIFI_AP: // access point
            WiFi.mode(WIFI_AP);
            return _setupAP();
            break;

        case WIFI_STA: { // station mode
            if (!wifiEnsureRadioMem()) {
                displayError("Low RAM: free BLE/SD first", true);
                return false;
            }
            WiFi.mode(WIFI_MODE_STA);

            // wifiMACMenu();
            applyConfiguredMAC();

            bool refresh_scan = false;
            do {
                int pickedIdx = -1;
                const int picked = wifiScanNetworkMenu(pickedIdx);

                if (picked == WIFI_PICK_RESCAN) {
                    refresh_scan = true;
                } else if (picked == WIFI_PICK_HIDDEN) {
                    String __ssid = keyboard("", 32, "Your SSID");
                    if (__ssid != "\x1B") _wifiConnect(__ssid.c_str(), 8);
                    refresh_scan = false;
                } else if (picked == WIFI_PICK_NET) {
                    // The channel and BSSID from the scan are the connect fast path.
                    const WifiScanAp *ap = wifiScanStreamAp(pickedIdx);
                    if (ap != nullptr) _wifiConnect(ap->ssid, ap->enc, ap->ch, ap->bssid);
                    refresh_scan = false;
                } else {
                    refresh_scan = false;
                }
            } while (refresh_scan);
        } break;

        case WIFI_AP_STA: // repeater mode
                          // _setupRepeater();
            break;

        default: // error handling
            Serial.println("Unknown wifi mode: " + String(mode));
            break;
    }

    if (returnToMenu) {
        wifiDisconnect(); // Forced turning off the wifi module if exiting back to the menu
        return false;
    }
    return wifiConnected;
}

void wifiConnectTask(void *pvParameters) {
    if (WiFi.isConnected()) return;

    if (FORCE_RADIO_TEARDOWN_ON_SWITCH) {
        stopBLEStack();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    // Check if WiFi is in transition
    if (wifiTransitioning) {
        vTaskDelete(NULL);
        return;
    }

    // No-PSRAM guard: don't bring Wi-Fi up if the contiguous DMA block is too
    // small (e.g. BLE already active) — the scan would half-init the driver and
    // crash on teardown. Silent bail: this is a background auto-connect task.
    if (!wifiEnsureRadioMem()) {
        vTaskDelete(NULL);
        return;
    }

    WiFi.mode(WIFI_MODE_STA);
    int nets = wifiScanNets();
    String ssid;
    String pwd;

    for (int i = 0; i < nets; i++) {
        ssid = WiFi.SSID(i);
        pwd = bruceConfig.getWifiPassword(ssid);
        if (pwd == "") continue;

        // Channel + BSSID from the scan: associate directly instead of sweeping all
        // 13 channels for the SSID first, and fail fast on an AP that is gone.
        WiFi.begin(ssid, pwd, WiFi.channel(i), WiFi.BSSID(i));
        for (int i = 0; i < 50; i++) {
            if (WiFi.isConnected()) {
                wifiConnected = true;
                wifiIP = WiFi.localIP().toString();

                // Start timezone update in background if not already running
                if (timezoneTaskHandle == NULL) {
                    xTaskCreate(updateTimezoneTask, "updateTimezone", 4096, NULL, 1, &timezoneTaskHandle);
                }
                drawStatusBar();
                break;
            }
            vTaskDelay(100 / portTICK_RATE_MS);
        }
    }
    WiFi.scanDelete();

    vTaskDelete(NULL);
    return;
}

String checkMAC() { return String(WiFi.macAddress()); }

bool wifiConnecttoKnownNet(void) {
    if (WiFi.isConnected()) return true; // safeguard

    if (FORCE_RADIO_TEARDOWN_ON_SWITCH) {
        stopBLEStack();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    // Check if WiFi is in transition
    if (wifiTransitioning) {
        displayTextLine("WiFi busy, please wait...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        return false;
    }

    // No-PSRAM guard: refuse before the scan brings Wi-Fi up in low memory.
    if (!wifiEnsureRadioMem()) {
        displayError("Low RAM: free BLE/SD first", true);
        return false;
    }

    bool result = false;
    int nets;
    // WiFi.mode(WIFI_MODE_STA);
    displayTextLine("Scanning Networks..");
    WiFi.disconnect(true, true);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    nets = wifiScanNets();
    for (int i = 0; i < nets; i++) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
        String ssid = WiFi.SSID(i);
        String password = bruceConfig.getWifiPassword(ssid);
        if (password != "") {
            Serial.println("Connecting to: " + ssid);
            result = _connectToWifiNetwork(ssid, password, (uint8_t)WiFi.channel(i), WiFi.BSSID(i));
        }
        // Maybe it finds a known network and can't connect, then try the next
        // until it gets connected (or not)
        if (result) {
            Serial.println("Connected to: " + ssid);
            break;
        }
    }
    if (WiFi.isConnected()) {
        wifiConnected = true;
        wifiIP = WiFi.localIP().toString();

        // Start timezone update in background if not already running
        if (timezoneTaskHandle == NULL) {
            xTaskCreate(updateTimezoneTask, "updateTimezone", 4096, NULL, 1, &timezoneTaskHandle);
        }
    }
    return result;
}

void updateTimezoneTask(void *pvParameters) {
    // Wait a bit for connection to stabilize before updating timezone
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    // Only update timezone if WiFi is still connected
    if (WiFi.isConnected() && wifiConnected) { updateClockTimezone(); }

    // Clear the task handle before deleting
    timezoneTaskHandle = NULL;
    vTaskDelete(NULL);
}
