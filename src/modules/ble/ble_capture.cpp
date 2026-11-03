#include "ble_capture.h"

#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "ble_common.h"
#include "core/utils.h"
#include <NimBLEDevice.h>
#include <globals.h>

// ============================================================================
// State
// ============================================================================

// The capture table (48 x 48 B) is only touched while a capture page is open, so
// it is allocated from PSRAM on first use (internal heap as the fallback) instead
// of holding internal DRAM in .bss for every session - the same pattern as the
// other per-feature scratch areas (DISCOVERIES 79). Every reader is bounded by
// bleCapCount, and the three writers below ensure the table first, so a null
// pointer can never be indexed.
static BleCapturedPacket *bleCap = nullptr;
static int bleCapCount = 0;

static bool bleCapEnsure() {
    if (bleCap) return true;
    const size_t bytes = sizeof(BleCapturedPacket) * BLE_CAP_MAX_PACKETS;
    bleCap = (BleCapturedPacket *)(psramFound() ? ps_malloc(bytes) : malloc(bytes));
    if (bleCap) memset(bleCap, 0, bytes);
    return bleCap != nullptr;
}
static int bleCapDropped = 0;
static bool bleCapScanning = false;
static bool bleCapInitFailed = false; // latched: stop retrying within this boot
static int bleRemoteType = 0;

// Remote types: index 0 is "All", the rest filter by the Bluetooth SIG
// company identifier in the manufacturer-specific data.
static const BleRemoteTypeInfo bleRemoteTypes[] = {
    {"All",             0xFFFF},
    {"Apple (TV/ATV)",  0x004C},
    {"Samsung",         0x0075},
    {"Xiaomi",          0x02BB},
    {"Amazon",          0x0171},
    {"Google",          0x00E0},
    {"Microsoft",       0x0006},
    {"Nordic (generic)", 0x0059},
};

int bleCaptureRemoteTypeCount() { return sizeof(bleRemoteTypes) / sizeof(bleRemoteTypes[0]); }

const char *bleCaptureRemoteTypeName(int idx) {
    if (idx < 0 || idx >= bleCaptureRemoteTypeCount()) return "All";
    return bleRemoteTypes[idx].name;
}

void bleCaptureSetRemoteType(int idx) {
    if (idx < 0 || idx >= bleCaptureRemoteTypeCount()) idx = 0;
    bleRemoteType = idx;
}

int bleCaptureRemoteType() { return bleRemoteType; }

const char *bleCaptureFolder() { return "/BruceBLE"; }

// ============================================================================
// Capture
// ============================================================================

// Discoverable advertisement callback: store the raw payload. This NimBLE
// build calls the hook NimBLEScanCallbacks (the older
// NimBLEAdvertisedDeviceCallbacks name no longer exists).
class BleCaptureCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *dev) override {
        if (dev == nullptr) return;
        // Out of table memory: count the loss instead of dropping out of the scan.
        if (!bleCapEnsure() || bleCapCount >= BLE_CAP_MAX_PACKETS) {
            bleCapDropped++;
            return;
        }

        const std::vector<uint8_t> &payload = dev->getPayload();
        BleCapturedPacket &p = bleCap[bleCapCount];
        memset(&p, 0, sizeof(p));
        p.timestamp = millis();
        p.rssi = dev->getRSSI();
        p.advType = dev->getAdvType();
        p.connectable = dev->isConnectable();
        p.addrType = dev->getAddressType();

        NimBLEAddress addr = dev->getAddress();
        const uint8_t *bytes = addr.getBase()->val;
        if (bytes != nullptr) memcpy(p.addr, bytes, 6);

        size_t len = payload.size();
        if (len > BLE_CAP_MAX_PAYLOAD) len = BLE_CAP_MAX_PAYLOAD;
        p.len = (uint8_t)len;
        if (len > 0) memcpy(p.data, payload.data(), len);

        bleCapCount++;
    }
};

static BleCaptureCallbacks bleCapCallbacks;

// NimBLEScan::start() does not return until its whole duration has elapsed, so
// a script could never stop a capture it started. Run the scan on its own task
// instead and let bleCaptureStop() cancel it with NimBLEScan::stop(), which
// aborts GAP discovery and releases the semaphore the scanning task waits on.
static TaskHandle_t bleCapTask = nullptr;
static volatile bool bleCapStopReq = false;

static void bleCapTaskFn(void *arg) {
    uint32_t durationMs = (uint32_t)(uintptr_t)arg;
    NimBLEScan *scan = BLEDevice::getScan();
    if (scan != nullptr) {
        bleCapScanning = true;
        // NimBLEScan::start() is ASYNCHRONOUS in this NimBLE-Arduino: it arms
        // discovery and returns immediately (getResults() is the blocking
        // wrapper). The old code called start() and stop() back to back, which
        // cancelled the scan in the same millisecond - every capture reported
        // zero packets and the Packet Monitor's BLE tap stayed empty. So arm the
        // scan, then sit on this task for the window; bleCaptureStop() sets
        // bleCapStopReq (and calls scan->stop()) to end it early.
        if (scan->start(durationMs, false, true)) {
            uint32_t started = millis();
            while (!bleCapStopReq && (millis() - started) < durationMs) {
                vTaskDelay(pdMS_TO_TICKS(25));
            }
        }
        scan->stop();
        scan->clearResults();
        bleCapScanning = false;
    }
    bleCapTask = nullptr; // clear before deleting so stop() stops waiting
    vTaskDelete(nullptr);
}

static bool bleCapConfigureScan() {
    NimBLEScan *scan = BLEDevice::getScan();
    if (scan == nullptr) return false;
    // wantDuplicates = true: every advertisement is recorded, not just the
    // first per address. A remote's replay needs the repeats as well.
    scan->setScanCallbacks(&bleCapCallbacks, true);
    scan->setActiveScan(false); // passive: keep the raw payload untouched
    scan->setInterval(100);
    scan->setWindow(99);
    scan->setDuplicateFilter(false);
    return true;
}

bool bleCaptureStart(uint32_t maxMs) {
    if (bleCapTask != nullptr) return false;
    if (maxMs < 200) maxMs = 200;
    if (maxMs > 600000) maxMs = 600000;
    // Allocate the table before the scan starts, not from the scan callback: a
    // capture that cannot store anything should refuse to start.
    if (!bleCapEnsure()) {
        serialDevice->println("[BLE] capture table: out of RAM");
        return false;
    }

    bleCapStopReq = false;
    bleCaptureClear();
    // init() must run at most once per boot, and a failed attempt has to be
    // remembered so a retry does not earn another ESP_ERR_INVALID_STATE (and a
    // NimBLE assert when the stack is half built). bleInit() unwinds the
    // controller when it fails, so the internal RAM the attempt took is handed
    // back instead of being lost for the rest of the boot.
    if (!BLEDevice::isInitialized()) {
        if (bleCapInitFailed) {
            serialDevice->println("[BLE] stack unavailable (out of internal RAM) - use the BLE menu");
            return false;
        }
        if (!bleInit("")) {
            bleCapInitFailed = true;
            serialDevice->println("[BLE] init failed (out of DMA-capable internal RAM) - use the BLE menu");
            return false;
        }
    }
    if (!bleCapConfigureScan()) return false;

    bleCapScanning = true;
    if (xTaskCreatePinnedToCore(
            bleCapTaskFn, "bleCap", 4096, (void *)(uintptr_t)maxMs, 1, &bleCapTask, 1
        ) != pdPASS) {
        bleCapScanning = false;
        bleCapTask = nullptr;
        return false;
    }
    // Let the task reach scan->start() so an immediate stop() still cancels it.
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

int bleCaptureStop() {
    if (bleCapTask == nullptr) return bleCapCount;
    bleCapStopReq = true;
    NimBLEScan *scan = BLEDevice::getScan();
    if (scan != nullptr) scan->stop();
    // The scan task needs a moment to unwind; 3 s is far more than it takes.
    for (int i = 0; i < 300 && bleCapTask != nullptr; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (bleCapTask != nullptr) {
        vTaskDelete(bleCapTask);
        bleCapTask = nullptr;
        bleCapScanning = false;
    }
    return bleCapCount;
}

bool bleCaptureRunning() { return bleCapTask != nullptr; }

int bleCaptureRun(uint32_t durationMs) {
    if (!bleCaptureStart(durationMs)) return bleCapCount;
    uint32_t waited = 0;
    while (bleCaptureRunning() && waited < durationMs + 2000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
    if (bleCaptureRunning()) bleCaptureStop();
    return bleCapCount;
}

int bleCaptureCount() { return bleCapCount; }

bool bleCapturePacket(int index, BleCapturedPacket *out) {
    if (index < 0 || index >= bleCapCount || out == nullptr) return false;
    *out = bleCap[index];
    return true;
}

void bleCaptureClear() {
    bleCapCount = 0;
    bleCapDropped = 0;
}

// Extract the Bluetooth SIG company id from a raw advertisement payload.
static uint16_t bleCompanyIdOf(const BleCapturedPacket &p) {
    for (int i = 0; i + 3 < p.len;) {
        uint8_t adLen = p.data[i];
        if (adLen == 0) break;
        if (i + adLen >= BLE_CAP_MAX_PAYLOAD + 1) break;
        uint8_t adType = p.data[i + 1];
        if (adType == 0xFF && adLen >= 3) {
            // Manufacturer specific data: 2-byte little-endian company id.
            return (uint16_t)(p.data[i + 2] | (p.data[i + 3] << 8));
        }
        i += adLen + 1;
    }
    return 0xFFFF;
}

int bleCaptureFilter(const String &needle, int *outIdx, int maxOut) {
    if (outIdx == nullptr || maxOut <= 0) return 0;
    uint16_t wantedCompany = bleRemoteTypes[bleRemoteType].companyId;
    String lower = needle;
    lower.toLowerCase();

    int written = 0;
    for (int i = 0; i < bleCapCount && written < maxOut; i++) {
        if (wantedCompany != 0xFFFF && bleCompanyIdOf(bleCap[i]) != wantedCompany) continue;
        if (lower.length() > 0) {
            String desc = bleCaptureDescribe(bleCap[i]);
            desc.toLowerCase();
            if (desc.indexOf(lower) == -1) continue;
        }
        outIdx[written++] = i;
    }
    return written;
}

int bleCaptureAddressCount() {
    char addrs[BLE_CAP_MAX_PACKETS][18];
    int n = 0;
    for (int i = 0; i < bleCapCount; i++) {
        char buf[18];
        snprintf(
            buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", bleCap[i].addr[5], bleCap[i].addr[4],
            bleCap[i].addr[3], bleCap[i].addr[2], bleCap[i].addr[1], bleCap[i].addr[0]
        );
        bool dup = false;
        for (int j = 0; j < n; j++)
            if (strcmp(addrs[j], buf) == 0) dup = true;
        if (!dup && n < BLE_CAP_MAX_PACKETS) strlcpy(addrs[n++], buf, 18);
    }
    return n;
}

// ============================================================================
// Replay
// ============================================================================

bool bleCaptureReplay(const int *idx, int count, uint32_t durationMs, bool spoofAddress) {
    if (idx == nullptr || count <= 0 || bleCapCount == 0) return false;
    if (durationMs < 200) durationMs = 200;
    if (durationMs > 60000) durationMs = 60000;

    if (!BLEDevice::isInitialized()) {
        if (bleCapInitFailed) return false;
        if (!bleInit("")) {
            bleCapInitFailed = true;
            return false;
        }
    }
    NimBLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->stop();

    uint32_t end = millis() + durationMs;
    int i = 0;
    while ((int32_t)(end - millis()) > 0) {
        int packetIndex = idx[i % count];
        if (packetIndex < 0 || packetIndex >= bleCapCount) {
            i++;
            continue;
        }
        BleCapturedPacket &p = bleCap[packetIndex];

        if (spoofAddress) {
            // Best effort: some controllers refuse an arbitrary address.
            NimBLEDevice::setOwnAddr(p.addr);
            NimBLEDevice::setOwnAddrType(p.addrType ? BLE_OWN_ADDR_RANDOM : BLE_OWN_ADDR_PUBLIC);
        }

        NimBLEAdvertisementData advData;
        if (p.len > 0) advData.addData(p.data, p.len);
        adv->setAdvertisementData(advData);
        adv->start();
        delay(120);
        adv->stop();

        i++;
    }
    return true;
}

// ============================================================================
// Files
// ============================================================================

static FS *bleFs() { return setupSdCard() ? (FS *)&SD : (FS *)&LittleFS; }

static String bleNormalizePath(const String &path) {
    String p = path;
    p.trim();
    if (p.length() == 0) return p;
    if (!p.startsWith("/")) p = String(bleCaptureFolder()) + "/" + p;
    if (p.indexOf('.') == -1) p += ".ble";
    return p;
}

bool bleCaptureSave(const String &path, const char *label) {
    FS *fs = bleFs();
    if (fs == nullptr) return false;
    if (!fs->exists(bleCaptureFolder())) fs->mkdir(bleCaptureFolder());
    String full = bleNormalizePath(path);
    if (full.length() == 0) return false;

    File f = fs->open(full, FILE_WRITE);
    if (!f) return false;
    f.println("# Bruce BLE capture v1");
    f.println(String("label=") + (label ? label : ""));
    f.println(String("count=") + String(bleCapCount));
    for (int i = 0; i < bleCapCount; i++) {
        String hex;
        for (int b = 0; b < bleCap[i].len; b++) {
            char h[3];
            snprintf(h, sizeof(h), "%02X", bleCap[i].data[b]);
            hex += h;
        }
        char addr[18];
        snprintf(
            addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X", bleCap[i].addr[5], bleCap[i].addr[4],
            bleCap[i].addr[3], bleCap[i].addr[2], bleCap[i].addr[1], bleCap[i].addr[0]
        );
        f.println(
            String("pkt addr=") + addr + " type=" + String(bleCap[i].addrType) + " rssi=" + String(bleCap[i].rssi) +
            " advtype=" + String(bleCap[i].advType) + " conn=" + (bleCap[i].connectable ? "1" : "0") +
            " len=" + String(bleCap[i].len) + " data=" + hex
        );
    }
    f.close();
    return true;
}

int bleCaptureLoad(const String &path, String *labelOut) {
    FS *fs = bleFs();
    if (fs == nullptr) return 0;
    String full = bleNormalizePath(path);
    if (!fs->exists(full)) return 0;
    File f = fs->open(full, FILE_READ);
    if (!f) return 0;
    if (!bleCapEnsure()) {
        f.close();
        return 0;
    }

    bleCaptureClear();
    while (f.available() && bleCapCount < BLE_CAP_MAX_PACKETS) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        if (line.startsWith("label=")) {
            if (labelOut) *labelOut = line.substring(6);
            continue;
        }
        if (!line.startsWith("pkt ")) continue;

        char addr[24] = {0};
        char hex[2 * BLE_CAP_MAX_PAYLOAD + 4] = {0};
        int type = 0, rssi = 0, advtype = 0, conn = 0, len = 0;
        sscanf(
            line.c_str(), "pkt addr=%23s type=%d rssi=%d advtype=%d conn=%d len=%d data=%64s", addr, &type, &rssi,
            &advtype, &conn, &len, hex
        );
        if (len <= 0 || len > BLE_CAP_MAX_PAYLOAD) continue;

        BleCapturedPacket &p = bleCap[bleCapCount];
        memset(&p, 0, sizeof(p));
        p.timestamp = millis();
        p.addrType = (uint8_t)type;
        p.rssi = (int8_t)rssi;
        p.advType = (uint8_t)advtype;
        p.connectable = conn != 0;
        p.len = (uint8_t)len;

        unsigned int a[6];
        if (sscanf(addr, "%x:%x:%x:%x:%x:%x", &a[5], &a[4], &a[3], &a[2], &a[1], &a[0]) == 6) {
            for (int i = 0; i < 6; i++) p.addr[i] = (uint8_t)a[i];
        }
        for (int i = 0; i < len; i++) {
            if (hex[i * 2] == 0 || hex[i * 2 + 1] == 0) break;
            char b[3] = {hex[i * 2], hex[i * 2 + 1], 0};
            p.data[i] = (uint8_t)strtol(b, nullptr, 16);
        }
        bleCapCount++;
    }
    f.close();
    return bleCapCount;
}

// ============================================================================
// Analysis
// ============================================================================

String bleCaptureDescribe(const BleCapturedPacket &p) {
    char addr[20];
    snprintf(
        addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X", p.addr[5], p.addr[4], p.addr[3], p.addr[2], p.addr[1],
        p.addr[0]
    );

    const char *advTypeName = "ADV";
    switch (p.advType) {
        case 0: advTypeName = "ADV_IND"; break;
        case 1: advTypeName = "ADV_DIRECT"; break;
        case 2: advTypeName = "ADV_NONCONN"; break;
        case 3: advTypeName = "SCAN_RSP"; break;
        case 4: advTypeName = "ADV_SCAN_IND"; break;
        case 5: advTypeName = "ADV_EXT"; break;
        case 6: advTypeName = "ADV_PERIODIC"; break;
        default: break;
    }

    uint16_t company = bleCompanyIdOf(p);
    char companyStr[16];
    if (company == 0xFFFF) strlcpy(companyStr, "n/a", sizeof(companyStr));
    else snprintf(companyStr, sizeof(companyStr), "0x%04X", company);

    return String(addr) + " " + advTypeName + " rssi=" + String(p.rssi) + " adv=" + String(p.len) +
           "B co=" + companyStr + (p.connectable ? " conn" : "");
}

int bleScanDetailed(uint32_t durationMs, BleScanEntry *out, int maxOut) {
    if (out == nullptr || maxOut <= 0) return 0;
    static int captured = 0;
    captured = bleCaptureRun(durationMs);
    int n = captured < maxOut ? captured : maxOut;
    for (int i = 0; i < n; i++) {
        BleCapturedPacket p;
        if (!bleCapturePacket(i, &p)) continue;
        memset(&out[i], 0, sizeof(BleScanEntry));
        snprintf(
            out[i].address, sizeof(out[i].address), "%02X:%02X:%02X:%02X:%02X:%02X", p.addr[5], p.addr[4], p.addr[3],
            p.addr[2], p.addr[1], p.addr[0]
        );
        // Pull a printable name out of the AD structures if there is one.
        out[i].name[0] = 0;
        for (int b = 0; b + 1 < p.len;) {
            uint8_t adLen = p.data[b];
            if (adLen == 0) break;
            // Bound against the length this packet actually carried, not only
            // against the array: an advertiser that sends a truncated AD
            // structure (adLen pointing past the end of its payload) used to
            // pass a MAX_PAYLOAD check and copy bytes left over from the
            // previous packet in the same slot, so the row showed a name that
            // was never advertised.
            if (b + 1 + adLen > p.len) break;
            uint8_t adType = p.data[b + 1];
            if ((adType == 0x08 || adType == 0x09) && adLen >= 2) {
                int nameLen = adLen - 1;
                if (nameLen > (int)sizeof(out[i].name) - 1) nameLen = sizeof(out[i].name) - 1;
                memcpy(out[i].name, &p.data[b + 2], nameLen);
                out[i].name[nameLen] = 0;
                break;
            }
            b += adLen + 1;
        }
        out[i].rssi = p.rssi;
        out[i].advType = p.advType;
        out[i].addrType = p.addrType;
        out[i].payloadLen = p.len;
        out[i].connectable = p.connectable;
    }
    return n;
}

// ============================================================================
// Menu
// ============================================================================

static String bleHexOf(const uint8_t *data, uint8_t len) {
    String hex;
    for (int i = 0; i < len; i++) {
        char h[3];
        snprintf(h, sizeof(h), "%02X", data[i]);
        hex += h;
    }
    return hex;
}

/// Drop presses that queued up while a process was running, so the press that
/// dismisses a result screen cannot also fire on the menu rebuilt behind it.
static void bleClearPendingKeys() {
    EscPress = false;
    SelPress = false;
    PrevPress = false;
    NextPress = false;
    UpPress = false;
    DownPress = false;
    AnyKeyPress = false;
}

static void bleWaitAnyKey() {
    bleClearPendingKeys();
    while (!check(AnyKeyPress)) vTaskDelay(pdMS_TO_TICKS(20));
    bleClearPendingKeys();
}

static void bleShowLines(const String &title, const String *lines, int count) {
    drawMainBorderWithTitle(title);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    int y = 40;
    for (int i = 0; i < count && y < tftHeight - 26; i++) {
        tft.drawString(lines[i].substring(0, 40), 8, y);
        y += 14;
    }
    tft.drawString("Press any key", 8, tftHeight - 20);
    bleWaitAnyKey();
}

/// Live passive scan: the packet counter updates while the scan runs and only
/// Back stops it early (BLE capture runs on its own task, so stopping is
/// immediate and the packets gathered so far are kept).
static void bleMenuStartCapture() {
    const uint32_t scanMs = 5000;
    if (!bleCaptureStart(scanMs)) {
        displayError("BLE capture already running", true);
        return;
    }

    drawMainBorderWithTitle("BLE Capture");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawString("Passive scan 5s", 10, 44);
    tft.drawString("Press BACK to stop", 10, tftHeight - 20);
    bleClearPendingKeys();

    uint32_t start = millis();
    uint32_t lastPaint = 0;
    bool aborted = false;
    while (bleCaptureRunning() && (millis() - start) < scanMs + 300) {
        if (check(EscPress)) {
            aborted = true;
            bleCaptureStop();
            break;
        }
        if (millis() - lastPaint >= 150) {
            lastPaint = millis();
            uint32_t elapsed = millis() - start;
            uint32_t left = elapsed < scanMs ? (scanMs - elapsed + 999) / 1000 : 0;
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.fillRect(8, 66, tftWidth - 16, 32, bruceConfig.bgColor);
            tft.drawString(String("Packets: ") + String(bleCaptureCount()), 10, 66);
            tft.drawString(String("Left: ") + String(left) + "s", 10, 82);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    // The scan task owns the scan object; it must be gone before the stack is
    // pulled down, or the teardown races a task that is still calling into
    // NimBLE. bleCaptureStop() waits for the task, so this is the ordering that
    // matters when the page is closed with a capture still running.
    if (bleCaptureRunning()) bleCaptureStop();

    drawMainBorderWithTitle("BLE Capture");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    int y = 44;
    if (aborted) {
        tft.drawString("Stopped", 10, y);
        y += 16;
    }
    tft.drawString(String("Packets: ") + String(bleCaptureCount()), 10, y);
    tft.drawString(String("Advertisers: ") + String(bleCaptureAddressCount()), 10, y + 16);
    tft.drawString(String("Type: ") + bleCaptureRemoteTypeName(bleCaptureRemoteType()), 10, y + 32);
    tft.drawString("Press any key", 10, tftHeight - 20);
    bleWaitAnyKey();
}

static void bleMenuShowPackets(const String &filter) {
    static int idx[BLE_CAP_MAX_PACKETS];
    int n = bleCaptureFilter(filter, idx, BLE_CAP_MAX_PACKETS);
    String lines[7];
    int written = 0;
    for (int i = 0; i < n && written < 6; i++) {
        BleCapturedPacket p;
        if (!bleCapturePacket(idx[i], &p)) continue;
        lines[written++] = bleCaptureDescribe(p).substring(0, 40);
    }
    if (written == 0) {
        lines[written++] = "No packets match";
    } else {
        // Read the count BEFORE the increment: `lines[written++] = ... written ...`
        // reads and writes `written` in one unsequenced expression, which is
        // undefined behaviour (gcc reports it as -Wsequence-point).
        int shown = written;
        lines[written++] = String("Showing ") + String(shown) + "/" + String(n);
    }
    bleShowLines("Packets", lines, written);
}

static void bleMenuAnalyze() {
    if (bleCaptureCount() == 0) {
        displayError("Nothing captured", true);
        return;
    }
    BleCapturedPacket p;
    if (!bleCapturePacket(0, &p)) return;
    String lines[5];
    lines[0] = bleCaptureDescribe(p).substring(0, 40);
    lines[1] = String("payload=") + String(p.len) + "B";
    lines[2] = bleHexOf(p.data, p.len).substring(0, 30);
    lines[3] = String("addrType=") + (p.addrType ? "random" : "public");
    lines[4] = String("connectable=") + (p.connectable ? "yes" : "no");
    bleShowLines("Analyze", lines, 5);
}

static void bleMenuDetailedScan() {
    // 24 entries x 56 B = 1.3 KB moved out of .bss; the menu is the only user,
    // so the table is allocated here (PSRAM first) instead of being resident.
    static BleScanEntry *entries = nullptr;
    if (!entries) {
        entries = (BleScanEntry *)(psramFound() ? ps_malloc(sizeof(BleScanEntry) * 24)
                                               : malloc(sizeof(BleScanEntry) * 24));
    }
    if (!entries) {
        displayError("Low RAM for BLE scan", true);
        return;
    }
    drawMainBorderWithTitle("BLE Scan");
    tft.drawString("Scanning 5s...", 8, 48);
    int n = bleScanDetailed(5000, entries, 24);
    String lines[7];
    int written = 0;
    for (int i = 0; i < n && written < 6; i++) {
        String name = entries[i].name[0] ? String(entries[i].name) : String(entries[i].address);
        lines[written++] = name.substring(0, 14) + " " + String(entries[i].rssi) + "dBm " +
                           String(entries[i].payloadLen) + "B" + (entries[i].connectable ? " c" : "");
    }
    if (written == 0) lines[written++] = "No devices found";
    else lines[written++] = String("Total: ") + String(n);
    bleShowLines("BLE Scan", lines, written);
}

void bleCaptureMenu() {
    static String filter = "";
    // Leaving this page used to keep the BLE controller up: BLE Detector hands
    // back ~106 KB of internal RAM after its teardown, this page handed back
    // ~49 KB, i.e. the whole stack stayed held (measured 2026-10-27). Remember
    // whether the stack was already up when we arrived, so a caller that owns it
    // (BLE Suite, the packet-monitor tap) is not torn down underneath itself.
    const bool bleWasUpOnEntry = NimBLEDevice::isInitialized();
AGAIN:
    options = {
        {"Start capture (5s)", []() { bleMenuStartCapture(); }                                    },
        {"Show packets", []() { bleMenuShowPackets(filter); }                                     },
        {"Filter packets",
         []() {
             String f = keyboard(filter, 20, "Filter (name/addr):");
             if (f != "\x1B") filter = f;
         }                                                                                          },
        {"Save capture",
         []() {
             if (bleCaptureCount() == 0) {
                 displayError("Nothing captured", true);
                 return;
             }
             String name = keyboard("ble_capture", 20, "File name:");
             if (name.length() == 0 || name == "\x1B") return;
             if (bleCaptureSave(name, "ble menu")) displayInfo("Saved " + name, true);
             else displayError("Save failed", true);
         }                                                                                          },
        {"Load capture",
         []() {
             FS *fs = bleFs();
             if (fs == nullptr) return;
             if (!fs->exists(bleCaptureFolder())) fs->mkdir(bleCaptureFolder());
             String path = loopSD(*fs, true, "*", bleCaptureFolder());
             if (path.length() == 0 || path == "\x1B") return;
             int n = bleCaptureLoad(path, nullptr);
             displayInfo(String("Loaded ") + String(n) + " packets", true);
         }                                                                                          },
        {"Replay capture",
         []() {
             if (bleCaptureCount() == 0) {
                 displayError("Nothing captured", true);
                 return;
             }
             static int idx[BLE_CAP_MAX_PACKETS];
             int n = bleCaptureFilter("", idx, BLE_CAP_MAX_PACKETS);
             if (n == 0) n = bleCaptureCount();
             drawMainBorderWithTitle("BLE Replay");
             tft.drawString(String("Replaying ") + String(n) + " packets", 8, 48);
             bool ok = bleCaptureReplay(idx, n, 3000, true);
             if (ok) displayInfo("Replayed", true);
             else displayError("Replay failed", true);
         }                                                                                          },
        {"Analyze packet", []() { bleMenuAnalyze(); }                                              },
        {"Detailed scan", []() { bleMenuDetailedScan(); }                                          },
        {String("Remote type: ") + bleCaptureRemoteTypeName(bleCaptureRemoteType()),
         []() { bleCaptureSetRemoteType((bleCaptureRemoteType() + 1) % bleCaptureRemoteTypeCount()); }},
    };
    addOptionToMainMenu();
    int selected = loopOptions(options, MENU_TYPE_SUBMENU, "Capture & Replay");
    // A negative index is the physical Back button: leave this page instead of
    // rebuilding it, which used to look like "Back reloads the current menu".
    if (selected < 0) goto DONE;
    if (returnToMenu) goto DONE; // "Main Menu" entry
    // Any other entry: rebuild, so the Remote type label stays accurate.
    goto AGAIN;

DONE:
    if (bleCaptureRunning()) bleCaptureStop();
    // Only tear down what this page brought up. stopBLEStack() is idempotent and
    // no-ops when NimBLE is already down, so a second Back cannot double-free.
    if (!bleWasUpOnEntry) stopBLEStack();
}
