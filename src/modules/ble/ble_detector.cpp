#include "ble_detector.h"

#include "ble_common.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/radio_mem.h"
#include <NimBLEDevice.h>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <globals.h>

// ---------------------------------------------------------------------------
// BLE Detector (task 1: "BLE Detector, like the Momentum ESP32 port, restyled")
//
// A live radar table over one continuous active scan. NimBLEScan::start() is
// asynchronous in this NimBLE-Arduino (getResults() is the blocking wrapper), so
// the UI loop arms a scan window and re-arms it when it expires - no scanning
// task and no blocking call. The scan callback runs on the NimBLE host task and
// feeds the fixed table that the UI samples under a critical section.
//
// The table is static and small on purpose: internal DRAM is the scarce
// resource on this board and BLE bring-up needs ~56 KB free (DISCOVERIES 64).
// ---------------------------------------------------------------------------

#define BLE_DET_MAX 32        // devices tracked at once
#define BLE_DET_ROWS 8        // rows that fit the 170 px panel
#define BLE_DET_HOLD_MS 15000 // keep a device listed this long after its last packet
#define BLE_DET_NEW_MS 5000   // "new" styling window
#define BLE_DET_WINDOW_MS 3000
#define BLE_DET_RESTART_MS 150 // pause between windows, so the controller's duplicate cache resets

struct BleDetEntry {
    uint8_t addr[6];
    uint8_t addrType;
    uint16_t company; // manufacturer data company id (0 = none)
    char name[20];
    int8_t rssi; // strongest advertisement seen
    uint16_t seen;
    uint32_t lastSeen;
    bool used;
    bool apple; // Apple Continuity advert (iPhone/iPad/Watch/AirPods)
};

static BleDetEntry bleDet[BLE_DET_MAX];
static int bleDetCount = 0;
static uint32_t bleDetTotal = 0;
static uint32_t bleDetStartMs = 0;
static portMUX_TYPE bleDetMux = portMUX_INITIALIZER_UNLOCKED;

// Row style for the table. Kept local so the detector does not depend on the
// packet monitor's helpers.
static void bleDetLine(const String &s, int x, int y, uint16_t fg) {
    tft.setTextSize(1);
    tft.setTextColor(fg, bruceConfig.bgColor);
    tft.setCursor(x, y);
    tft.print(s);
}

static String bleDetPadR(const String &s, int width) {
    String out = s;
    if ((int)out.length() > width) out = out.substring(0, width);
    while ((int)out.length() < width) out += ' ';
    return out;
}

static String bleDetPadL(const String &s, int width) {
    String out = s;
    if ((int)out.length() > width) out = out.substring(out.length() - width);
    while ((int)out.length() < width) out = " " + out;
    return out;
}

static String bleDetAddrString(const uint8_t *a) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
    return String(buf);
}

// Log-distance estimate. -59 dBm is the usual 1 m reference for a BLE beacon;
// it is an estimate, which is all a detector can honestly show.
static String bleDetDistance(int8_t rssi) {
    if (rssi == 0) return "  --";
    float m = powf(10.0f, (-59.0f - (float)rssi) / 20.0f);
    if (m >= 10.0f) return ">10m";
    return String(m, 1) + "m";
}

// A/Apple, G/Google, M/Microsoft, B/BLE-SIG member data, . plain advertisement.
static char bleDetVendorLetter(uint16_t company) {
    switch (company) {
        case 0x004C: return 'A';
        case 0x00E0: return 'G';
        case 0x0006: return 'M';
        case 0x0000: return '.';
        default: return 'B';
    }
}

// Apple devices never advertise a name: an iPhone is only an Apple Continuity
// payload (company 0x004C) whose first byte after the company id says what kind
// of message it is. Without this decode every Apple row read "<?>" and the
// phone was impossible to tell apart from any other Apple device - which is
// what "the detector does not see my iPhone" looked like on the screen.
static const char *bleDetAppleLabel(const std::string &m) {
    if (m.size() < 3) return "Apple";
    switch ((uint8_t)m[2]) {
        case 0x01: return "Apple iBeacon";
        case 0x05: return "Apple AirDrop";
        case 0x07: return "Apple AirPods";
        case 0x09: return "Apple AirPlay";
        case 0x0A: return "Apple Watch";
        case 0x0B: return "Apple iCloud";
        case 0x0C: return "Apple Handoff";
        case 0x0D: return "Apple Tether";
        case 0x0E: return "Apple Nearby";
        case 0x0F: return "Apple Nearby";
        case 0x10: return "Apple Nearby";
        case 0x12: return "Apple Find My";
        default: return "Apple device";
    }
}

class BleDetCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *dev) override {
        if (dev == nullptr) return;

        uint8_t addr[6] = {0};
        const uint8_t *raw = dev->getAddress().getBase()->val;
        if (raw == nullptr) return;
        memcpy(addr, raw, 6);

        uint16_t company = 0;
        bool apple = false;
        String name = dev->getName().c_str();
        if (dev->haveManufacturerData()) {
            // This NimBLE-Arduino hands back a std::string here, not a vector.
            const std::string &m = dev->getManufacturerData();
            if (m.size() >= 2)
                company = (uint16_t)(uint8_t)m[0] | ((uint16_t)(uint8_t)m[1] << 8);
            if (company == 0x004C) {
                apple = true;
                if (name.length() == 0) name = bleDetAppleLabel(m);
            }
        }
        int8_t rssi = (int8_t)dev->getRSSI();
        uint32_t now = millis();

        portENTER_CRITICAL(&bleDetMux);
        bleDetTotal++;

        int slot = -1;
        int oldest = 0;
        for (int i = 0; i < bleDetCount; i++) {
            if (memcmp(bleDet[i].addr, addr, 6) == 0 && bleDet[i].addrType == dev->getAddressType()) {
                slot = i;
                break;
            }
            if (bleDet[i].lastSeen < bleDet[oldest].lastSeen) oldest = i;
        }
        if (slot < 0) {
            if (bleDetCount < BLE_DET_MAX) {
                slot = bleDetCount++;
                memset(&bleDet[slot], 0, sizeof(BleDetEntry));
            } else {
                // Full: the weakest/oldest advertiser loses its slot.
                slot = oldest;
            }
            bleDet[slot].used = true;
            memcpy(bleDet[slot].addr, addr, 6);
            bleDet[slot].addrType = dev->getAddressType();
        }

        BleDetEntry &e = bleDet[slot];
        e.company = company;
        e.apple = apple;
        e.seen = (e.seen < 0xFFFF) ? e.seen + 1 : e.seen;
        e.lastSeen = now;
        // A fresh slot has rssi == 0 from the memset above, and every real BLE
        // RSSI is negative, so 0 doubles as "no sample yet". Comparing straight
        // against e.rssi kept every row at 0 dBm (and the distance column at
        // "--"), because no negative value ever beat the initial zero.
        if (e.rssi == 0 || rssi > e.rssi) e.rssi = rssi;
        if (e.name[0] == 0 && name.length()) {
            strncpy(e.name, name.c_str(), sizeof(e.name) - 1);
            e.name[sizeof(e.name) - 1] = 0;
        }
        portEXIT_CRITICAL(&bleDetMux);
    }
};

static BleDetCallbacks bleDetCallbacks;

// Arm one scan window. This returns as soon as the controller has been asked to
// scan, and advertisements then arrive in bleDetCallbacks on the NimBLE host
// task; the UI loop re-arms the window once the controller stops scanning.
static void bleDetArmScan(NimBLEScan *scan) {
    if (scan == nullptr) return;
    if (scan->start(BLE_DET_WINDOW_MS, false, true)) return;

    // Busy (a connection is in progress) or the host is not in sync yet. The UI
    // loop retries every BLE_DET_RESTART_MS, so only say it once in a while -
    // this shares the USB console that the host tools read commands from.
    static uint32_t lastWarnMs = 0;
    if (millis() - lastWarnMs > 2000) {
        lastWarnMs = millis();
        Serial.println("[BLEDET] scan arm failed, will retry");
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Devices beyond the eight rows that fit the panel are reachable by turning
// the encoder (Prev/Next). Before this the table simply stopped drawing at row
// 8 of an RSSI-sorted list, so a phone that was further away than eight other
// advertisers looked like it had not been detected at all.
static int bleDetPageCount(int vis) {
    if (vis <= BLE_DET_ROWS) return 1;
    return (vis + BLE_DET_ROWS - 1) / BLE_DET_ROWS;
}

static void bleDetDraw(bool full, int count, uint32_t total, uint32_t uptimeMs, bool paused, int page) {
    drawMainBorder(full);
    tft.setTextSize(1);
    bleDetLine("BLE DETECTOR", 10, 27, bruceConfig.priColor);
    tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
    tft.drawRightString(
        String(count) + " dev  " + String(total) + " adv" + (paused ? "  HOLD" : ""), tftWidth - 10, 27, 1
    );
    tft.drawFastHLine(10, 37, tftWidth - 20, bruceConfig.priColor);

    bleDetLine(" T NAME             ADDRESS           dBm  DIST", 8, 41, bruceConfig.secColor);
    tft.drawFastHLine(10, 51, tftWidth - 20, bruceConfig.secColor);

    // Snapshot under the critical section, then draw outside it: a slow redraw
    // must never hold up the BLE host task.
    BleDetEntry snap[BLE_DET_MAX];
    int n = bleDetCount;
    portENTER_CRITICAL(&bleDetMux);
    memcpy(snap, bleDet, sizeof(BleDetEntry) * n);
    portEXIT_CRITICAL(&bleDetMux);

    int order[BLE_DET_MAX];
    int vis = 0;
    for (int i = 0; i < n; i++) {
        if (snap[i].used && (millis() - snap[i].lastSeen) <= BLE_DET_HOLD_MS) order[vis++] = i;
    }
    std::sort(order, order + vis, [&](int a, int b) { return snap[a].rssi > snap[b].rssi; });

    int pages = bleDetPageCount(vis);
    if (page < 0) page = 0;
    if (page >= pages) page = pages - 1;
    int first = page * BLE_DET_ROWS;

    for (int r = 0; r < BLE_DET_ROWS; r++) {
        int y = 55 + r * 13;
        // Clear the whole band: names change length as advertisers rotate.
        tft.fillRect(6, y - 2, tftWidth - 12, 12, bruceConfig.bgColor);
        String line;
        int idx = first + r;
        if (idx < vis) {
            const BleDetEntry &e = snap[order[idx]];
            bool isNew = (millis() - e.lastSeen) < BLE_DET_NEW_MS;
            String nm = e.name[0] ? String(e.name) : String("<?>");
            line = String(" ") + bleDetVendorLetter(e.company) + " " + bleDetPadR(nm, 16) + " " +
                   bleDetAddrString(e.addr) + " " + bleDetPadL(String(e.rssi), 4) + " " +
                   bleDetPadL(bleDetDistance(e.rssi), 5);
            bleDetLine(line, 8, y, isNew ? bruceConfig.priColor : bruceConfig.secColor);
        }
    }
    if (vis == 0) bleDetLine("   scanning...", 8, 55, bruceConfig.secColor);

    tft.drawFastHLine(10, tftHeight - 24, tftWidth - 20, bruceConfig.priColor);
    String footer = String("seen ") + String(vis) + "/" + String(n);
    if (pages > 1) footer += String("  pg ") + String(page + 1) + "/" + String(pages);
    footer += "  up " + String(uptimeMs / 1000) + "s";
    bleDetLine(footer, 10, tftHeight - 21, bruceConfig.secColor);
    bleDetLine(
        pages > 1 ? String("TURN pg  SEL menu  BACK exit") : String("SEL menu    BACK exit"), 10,
        tftHeight - 10, bruceConfig.secColor
    );
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void bleDetectorRun() {
    if (!NimBLEDevice::isInitialized() && !radioHasMemForBle()) {
        // Say how short it is instead of the generic "free WiFi/SD first": the
        // guard has already released WiFi and the web UI by this point, so what
        // is left is real fragmentation from the apps that ran before.
        char msg[40];
        snprintf(
            msg, sizeof(msg), "RAM %uK/%uK - restart", (unsigned)(radioFreeInternal() / 1024),
            (unsigned)(radioLargestDmaBlock() / 1024)
        );
        displayError(String(msg), true);
        return;
    }

    bool weStarted = !NimBLEDevice::isInitialized();
    if (weStarted && !bleInit("Vandeta-BLE")) {
        displayError("BLE init failed (low RAM)", true);
        return;
    }

    NimBLEScan *scan = BLEDevice::getScan();
    if (scan == nullptr) {
        displayError("No BLE scan object", true);
        if (weStarted) stopBLEStack();
        return;
    }

    // Active scan: the scan response is where most devices put their name.
    // wantDuplicates = true so the RSSI of a known device keeps updating.
    scan->setScanCallbacks(&bleDetCallbacks, true);
    scan->setActiveScan(true);
    scan->setInterval(SCAN_INT);
    scan->setWindow(SCAN_WINDOW);
    scan->setDuplicateFilter(false);
    scan->clearResults();

    portENTER_CRITICAL(&bleDetMux);
    memset(bleDet, 0, sizeof(bleDet));
    bleDetCount = 0;
    bleDetTotal = 0;
    portEXIT_CRITICAL(&bleDetMux);
    bleDetStartMs = millis();

    // Arm the first window here. The scan used to run on a 4 KB task of its own,
    // but once the BLE stack is up internal DRAM is too fragmented to allocate
    // that stack: xTaskCreatePinnedToCore() returned pdFALSE and the screen only
    // ever said "Failed to start scan". Driving the (non-blocking) scan from the
    // UI loop needs no extra stack at all.
    bleDetArmScan(scan);

    bool full = true;
    bool paused = false;
    int page = 0;
    uint32_t lastDraw = 0;
    uint32_t lastScanMs = millis();

    // The key that launched the app is still down when the scan starts; without
    // this the first poll reads SEL as pressed and jumps straight into the menu
    // (and an obvious ESC then closes the screen before it ever drew).
    while (check(SelPress) || check(EscPress)) vTaskDelay(pdMS_TO_TICKS(10));
    delay(150);

    while (true) {
        // Keep a window armed. The old code called start() and then stop() back
        // to back, which cancelled the scan in the same millisecond and left the
        // table empty; and because start() never blocks, nothing advanced the
        // window either. Re-arm whenever the controller is no longer scanning.
        if (scan->isScanning()) {
            lastScanMs = millis();
        } else if (millis() - lastScanMs >= BLE_DET_RESTART_MS) {
            lastScanMs = millis();
            bleDetArmScan(scan);
        }

        if (!paused && (millis() - lastDraw > 400)) {
            portENTER_CRITICAL(&bleDetMux);
            int count = bleDetCount;
            uint32_t total = bleDetTotal;
            portEXIT_CRITICAL(&bleDetMux);
            bleDetDraw(full, count, total, millis() - bleDetStartMs, paused, page);
            full = false;
            lastDraw = millis();
        }

        // Page through the table with the encoder (Prev/Next), so devices past
        // the eighth row are reachable instead of invisible.
        if (check(PrevPress) || check(UpPress)) {
            portENTER_CRITICAL(&bleDetMux);
            int live = 0;
            for (int i = 0; i < bleDetCount; i++)
                if (bleDet[i].used) live++;
            portEXIT_CRITICAL(&bleDetMux);
            int pages = bleDetPageCount(live);
            page = (page <= 0) ? pages - 1 : page - 1;
            lastDraw = millis() - 500; // repaint immediately, paused or not
            full = true;
        }
        if (check(NextPress) || check(DownPress)) {
            portENTER_CRITICAL(&bleDetMux);
            int live = 0;
            for (int i = 0; i < bleDetCount; i++)
                if (bleDet[i].used) live++;
            portEXIT_CRITICAL(&bleDetMux);
            int pages = bleDetPageCount(live);
            page = (page + 1 >= pages) ? 0 : page + 1;
            lastDraw = millis() - 500;
            full = true;
        }

        if (check(SelPress)) {
            std::vector<Option> opts = {
                {paused ? "Resume" : "Hold",
                 [&]() {
                     paused = !paused;
                     full = true;
                 }},
                {"Clear list",
                 [&]() {
                     portENTER_CRITICAL(&bleDetMux);
                     memset(bleDet, 0, sizeof(bleDet));
                     bleDetCount = 0;
                     bleDetTotal = 0;
                     portEXIT_CRITICAL(&bleDetMux);
                     bleDetStartMs = millis();
                     page = 0;
                     full = true;
                 }},
                {"Exit", []() {}},
            };
            loopOptions(opts, MENU_TYPE_SUBMENU, "BLE Detector");
            full = true; // the menu painted over the table
        }

        if (check(EscPress)) break;
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    // Teardown: the scan lives in the controller/host task, so a single stop()
    // is enough before stopBLEStack() pulls the radio out from under it.
    scan->stop();
    scan->clearResults();
    if (weStarted) stopBLEStack();

    portENTER_CRITICAL(&bleDetMux);
    memset(bleDet, 0, sizeof(bleDet));
    bleDetCount = 0;
    portEXIT_CRITICAL(&bleDetMux);
    returnToMenu = true;
}
