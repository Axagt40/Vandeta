#include "ble_detector.h"

#include "ble_common.h"
#include "core/odid.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/radio_mem.h"
#include "core/sound_theme.h"
#include "core/ui_gradient.h"
#include <NimBLEDevice.h>
#include <algorithm>
#include <ctype.h>
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

// HCI advertising-report type for a non-connectable undirected advert: the PDU
// that never accepts a connection. Spelled out locally instead of pulling in the
// controller's HCI enum, which is not exported through the NimBLE headers.
#define BLE_DET_ADV_NONCONN 3
#define BLE_DET_ADV_SCAN_RSP 4 // a reply to our scan request, not the device's own beacon

struct BleDetEntry {
    uint8_t addr[6];
    uint8_t addrType;
    uint8_t advType;      // HCI advertising type: 3 = non-connectable, never pairs
    uint16_t company;     // manufacturer data company id (0 = none)
    int8_t vendor;        // index into bleDetVendors, -1 = not recognised
    char name[20];
    int8_t rssi;          // strongest advertisement seen
    int8_t rssiMin;       // weakest / strongest since the device was first listed;
    int8_t rssiMax;       // 0 in either means "no sample yet" (real RSSI is negative)
    uint16_t seen;
    uint32_t firstSeen;
    uint32_t lastSeen;
    bool used;
    bool apple; // Apple Continuity advert (iPhone/iPad/Watch/AirPods)
};

// The device table is only live while the detector page is open, so it comes from
// PSRAM on first use (internal heap as the fallback) rather than holding 1.4 KB of
// internal DRAM in .bss for the whole session (DISCOVERIES 79). The allocation
// happens once in bleDetectorRun(), before the first scan is armed; the callback
// only ever reads the pointer.
#define BLE_DET_BYTES (sizeof(BleDetEntry) * BLE_DET_MAX)
static BleDetEntry *bleDet = nullptr;
static int bleDetCount = 0;

static bool bleDetEnsure() {
    if (bleDet) return true;
    bleDet = (BleDetEntry *)(psramFound() ? ps_malloc(BLE_DET_BYTES) : malloc(BLE_DET_BYTES));
    if (bleDet) {
        memset(bleDet, 0, BLE_DET_BYTES);
        // Zero is a valid vendor index (the first row of the table), so an
        // all-zero table would label every device as that first vendor until a
        // rule matched. "Not recognised" has to be written, not memset.
        for (int i = 0; i < BLE_DET_MAX; i++) bleDet[i].vendor = -1;
    }
    return bleDet != nullptr;
}
static uint32_t bleDetTotal = 0;
static uint32_t bleDetStartMs = 0;
static portMUX_TYPE bleDetMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// Who is this?  (task 1.1)
//
// Apple decodes its own Continuity payload (bleDetAppleLabel below); every other
// tracker puts a 16-bit service UUID or a company id in the advertisement, and
// that is what this table keys on. One table, one place to update when a vendor
// changes its frame - the old code recognised only Apple, so a Tile, a SmartTag
// and a Chipolo all drew as "<?>" and were indistinguishable from each other.
//
// Sources for the ids, so the next person does not have to guess:
//   0xFEED  Tile service                     (Bluetooth SIG assigned numbers)
//   0xFD5A  Samsung SmartTag service         (Samsung "SmartThings Find")
//   0xFE2C  Google Fast Pair service         (Fast Pair spec, "Find My Device")
//   0xFDF0  ASHA hearing-aid service         (Android hearing aid audio spec)
//   0x004C  Apple, type 0x12 = Find My network accessory (AirTag and clones)
//   0x0075  Samsung Electronics company id
//   0x00E0  Google company id
// A clone advertising Apple's Find My frame is reported as "FindMy tag" and not
// as an AirTag: the frame says what protocol it speaks, never who made it.
struct BleDetVendor {
    const char *label;     // what the row says when the advert carries no name
    const char *nameMatch; // lowercase substring of the advertised name, or nullptr
    uint16_t company;      // company id, 0 = any
    uint16_t service;      // 16-bit service UUID, 0 = any
    uint8_t appleType;     // Apple Continuity message type, 0 = any
    char letter;           // one-character tag in the table
    bool tracker;          // offers the Track page
};

static const BleDetVendor bleDetVendors[] = {
    // Remote ID first: a drone that also carries some vendor's company id must
    // still read as a drone, and the 0xFFFA service id is unambiguous.
    {"Drone", nullptr, 0, 0xFFFA, 0, 'D', false},
    // Chipolo speaks Apple's iBeacon frame, so its company id is Apple's and the
    // only honest discriminator is the name it advertises while pairable.
    {"Chipolo", "chipolo", 0, 0, 0, 'T', true},
    {"Tile", nullptr, 0, 0xFEED, 0, 'T', true},
    {"SmartTag", nullptr, 0, 0xFD5A, 0, 'T', true},
    {"FindMy tag", nullptr, 0x004C, 0, 0x12, 'T', true},
    {"Fast Pair", nullptr, 0, 0xFE2C, 0, 'G', false},
    {"Hearing aid", nullptr, 0, 0xFDF0, 0, 'H', false},
    {"Samsung", nullptr, 0x0075, 0, 0, 'S', false},
    {"Google", nullptr, 0x00E0, 0, 0, 'G', false},
};
#define BLE_DET_VENDOR_COUNT ((int)(sizeof(bleDetVendors) / sizeof(bleDetVendors[0])))

// Case-insensitive substring, without building a lowercase copy on every
// advertisement: this runs on the NimBLE host task for every packet.
static bool bleDetNameHas(const String &name, const char *needle) {
    const size_t n = strlen(needle);
    if (n == 0 || name.length() < n) return false;
    for (size_t i = 0; i + n <= (size_t)name.length(); i++) {
        size_t j = 0;
        while (j < n && tolower((unsigned char)name[i + j]) == needle[j]) j++;
        if (j == n) return true;
    }
    return false;
}

static bool bleDetHasService(const NimBLEAdvertisedDevice *dev, uint16_t uuid16) {
    if (dev == nullptr || uuid16 == 0) return false;
    const NimBLEUUID want(uuid16);
    if (dev->haveServiceUUID()) {
        const uint8_t count = dev->getServiceUUIDCount();
        for (uint8_t i = 0; i < count; i++)
            if (dev->getServiceUUID(i).equals(want)) return true;
    }
    // Trackers often put the same id in service data instead of the UUID list.
    if (dev->haveServiceData()) {
        const uint8_t count = dev->getServiceDataCount();
        for (uint8_t i = 0; i < count; i++)
            if (dev->getServiceDataUUID(i).equals(want)) return true;
    }
    return false;
}

// First matching rule wins, so the table above must stay ordered most specific
// first (a name match beats a company-id match).
static int bleDetVendorMatch(
    const NimBLEAdvertisedDevice *dev, const String &name, uint16_t company, bool haveMfr, uint8_t appleType
) {
    for (int i = 0; i < BLE_DET_VENDOR_COUNT; i++) {
        const BleDetVendor &v = bleDetVendors[i];
        if (v.company && (!haveMfr || v.company != company)) continue;
        if (v.appleType && v.appleType != appleType) continue;
        if (v.service && !bleDetHasService(dev, v.service)) continue;
        if (v.nameMatch && !bleDetNameHas(name, v.nameMatch)) continue;
        return i;
    }
    return -1;
}

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

// ---------------------------------------------------------------------------
// Remote ID (task 1.2)
//
// ASTM F3411 over BLE: service data on 0xFFFA, laid out as
//   [0x0D application code][counter][message pack]
// and the pack itself as
//   [0xF? proto|type][25 single message size][count][message * count]
// Each 25-byte message starts with [proto:4|type:4], and every multi-byte field
// is big-endian. The field order below is taken from the reference decoder
// (opendroneid-core-c, opendroneid.h `*_encoded` structs: bit fields are LSb
// first), not from a blog: Location, byte 1 = SpeedMult:1, EWDirection:1,
// HeightType:1, Reserved:1, Status:4; lat/lon are int32 * 1e-7; altitudes are
// uint16 with (0.5 m steps, -1000 m offset).
//
// Only what the screen can honestly show is decoded: identity, aircraft type,
// status, position, altitude, speed and the operator's position/id. There is no
// GPS on this board, so no "N m away" claim is made from position - the only
// distance figure is the RSSI estimate every BLE row already carries.
struct BleDetDrone {
    bool valid;
    uint8_t addr[6];
    uint8_t addrType;
    OdidData info;
    int8_t rssi;
    uint32_t ms; // last ODID frame
};

static BleDetDrone bleDetDrone = {};
static portMUX_TYPE bleDetDroneMux = portMUX_INITIALIZER_UNLOCKED;

// Applies one advertisement's service data. The decoding itself lives in
// core/odid.cpp, shared with the Wi-Fi beacon side of the airspace page, so the
// byte layout is written down exactly once. Returns true when it really was an
// ODID pack we could read, so the caller can name the row.
static bool bleDetOdid(const std::string &sd, const uint8_t *addr, uint8_t addrType, int8_t rssi) {
    OdidData info;
    if (!odidDecodeServiceData((const uint8_t *)sd.data(), sd.size(), info)) return false;
    BleDetDrone d = {};
    d.valid = true;
    d.addrType = addrType;
    memcpy(d.addr, addr, 6);
    d.info = info;
    d.rssi = rssi;
    d.ms = millis();
    bool wasKnown = false;
    portENTER_CRITICAL(&bleDetDroneMux);
    // A drone that keeps advertising is one event, not one beep per advert.
    wasKnown = bleDetDrone.valid && bleDetDrone.addrType == addrType && memcmp(bleDetDrone.addr, addr, 6) == 0;
    bleDetDrone = d;
    portEXIT_CRITICAL(&bleDetDroneMux);
    if (!wasKnown) soundPlay(CUE_REMOTE_ID);
    return true;
}

static bool bleDetIsDrone(const uint8_t *addr, uint8_t addrType) {
    bool yes = false;
    portENTER_CRITICAL(&bleDetDroneMux);
    yes = bleDetDrone.valid && bleDetDrone.addrType == addrType && memcmp(bleDetDrone.addr, addr, 6) == 0;
    portEXIT_CRITICAL(&bleDetDroneMux);
    return yes;
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

// The tag column: a recognised vendor's own letter wins, so a Tile, a SmartTag
// and a Chipolo stop looking like every other BLE advertiser.
static char bleDetLetter(const BleDetEntry &e) {
    if (e.vendor >= 0 && e.vendor < BLE_DET_VENDOR_COUNT) return bleDetVendors[e.vendor].letter;
    return bleDetVendorLetter(e.company);
}

// Skimmer-style beacon: an advertisement that is non-connectable by design and
// keeps coming for a long time without ever offering a connection. That is a
// pattern, not proof - plenty of beacons are honest - so the row gets a marker
// and the footer a count, never a claim.
static bool bleDetBeaconLike(const BleDetEntry &e, uint32_t now) {
    if (e.advType != BLE_DET_ADV_NONCONN) return false;
    if (e.seen < 40) return false;
    return (now - e.firstSeen) >= 20000 && (now - e.lastSeen) <= BLE_DET_HOLD_MS;
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
        if (bleDet == nullptr) return; // no table: nothing to record into

        uint8_t addr[6] = {0};
        const uint8_t *raw = dev->getAddress().getBase()->val;
        if (raw == nullptr) return;
        memcpy(addr, raw, 6);

        uint16_t company = 0;
        bool apple = false;
        uint8_t appleType = 0;
        String name = dev->getName().c_str();
        bool haveMfr = dev->haveManufacturerData();
        if (haveMfr) {
            // This NimBLE-Arduino hands back a std::string here, not a vector.
            const std::string &m = dev->getManufacturerData();
            if (m.size() >= 2)
                company = (uint16_t)(uint8_t)m[0] | ((uint16_t)(uint8_t)m[1] << 8);
            if (m.size() >= 3) appleType = (uint8_t)m[2];
            if (company == 0x004C) {
                apple = true;
                if (name.length() == 0) name = bleDetAppleLabel(m);
            }
        }

        // Task 1.1: name the non-Apple trackers. A matched rule also supplies a
        // name when the advertisement carries none, and marks which rows get the
        // Track page (only things that actually are trackers, not every beacon).
        const int vendor = bleDetVendorMatch(dev, name, company, haveMfr, appleType);
        if (name.length() == 0 && vendor >= 0) name = bleDetVendors[vendor].label;

        // Remote ID: a compliant drone advertises no name - the identity is
        // inside the ODID message pack in its 0xFFFA service data. Only a pack
        // that parses turns the row into "Drone <id>"; a device that merely
        // carries the UUID still reads as Drone with no id after it.
        if (vendor >= 0 && strcmp(bleDetVendors[vendor].label, "Drone") == 0) {
            const std::string sd = dev->getServiceData(NimBLEUUID((uint16_t)0xFFFA));
            if (bleDetOdid(sd, addr, dev->getAddressType(), (int8_t)dev->getRSSI())) {
                char id[21] = {0};
                portENTER_CRITICAL(&bleDetDroneMux);
                memcpy(id, bleDetDrone.info.uasId, sizeof(id));
                portEXIT_CRITICAL(&bleDetDroneMux);
                name = String("Drone ") + (id[0] ? String(id).substring(0, 10) : String("ODID"));
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
            } else {
                // Full: the advertiser that has been quiet longest loses its
                // slot. Its row has to be zeroed, not just re-addressed - the
                // old code kept the previous device's name, RSSI and seen count,
                // so a new device appeared wearing a departed one's identity.
                slot = oldest;
            }
            memset(&bleDet[slot], 0, sizeof(BleDetEntry));
            bleDet[slot].vendor = -1; // see bleDetEnsure(): 0 would mean "Chipolo"
            bleDet[slot].used = true;
            bleDet[slot].firstSeen = now;
            memcpy(bleDet[slot].addr, addr, 6);
            bleDet[slot].addrType = dev->getAddressType();
        }

        BleDetEntry &e = bleDet[slot];
        // Not every packet carries every field: a scan response usually has no
        // manufacturer data at all. Writing the zeros from one of those over the
        // identity learned from the advertisement made a named Tile turn back
        // into "<?>" between frames.
        if (haveMfr) {
            e.company = company;
            e.apple = apple;
        }
        if (vendor >= 0) e.vendor = (int8_t)vendor;
        // The beacon test is about the device's own advertisement; a scan
        // response has its own PDU type and must not overwrite it.
        if (dev->getAdvType() != BLE_DET_ADV_SCAN_RSP) e.advType = dev->getAdvType();
        e.seen = (e.seen < 0xFFFF) ? e.seen + 1 : e.seen;
        e.lastSeen = now;
        // A fresh slot has rssi == 0 from the memset above, and every real BLE
        // RSSI is negative, so 0 doubles as "no sample yet". Comparing straight
        // against e.rssi kept every row at 0 dBm (and the distance column at
        // "--"), because no negative value ever beat the initial zero.
        if (e.rssi == 0 || rssi > e.rssi) e.rssi = rssi;
        if (e.rssiMin == 0 || rssi < e.rssiMin) e.rssiMin = rssi;
        if (e.rssiMax == 0 || rssi > e.rssiMax) e.rssiMax = rssi;
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

// Selected row, held by address rather than by list index: the list is re-sorted
// by RSSI every frame, so an index would slide onto a different device while the
// user was looking at it.
static uint8_t bleDetSelAddr[6];
static uint8_t bleDetSelType = 0;
static bool bleDetSelValid = false;

// Snapshot + sort in one place: the draw and the cursor both need the same list,
// and two different notions of "row 3" is how a selection lands on the wrong
// device when the order changes between two frames.
static int bleDetSnapshot(BleDetEntry *snap, int *order, int &n) {
    portENTER_CRITICAL(&bleDetMux);
    n = bleDetCount;
    memcpy(snap, bleDet, sizeof(BleDetEntry) * n);
    portEXIT_CRITICAL(&bleDetMux);
    const uint32_t now = millis();
    int vis = 0;
    for (int i = 0; i < n; i++)
        if (snap[i].used && (now - snap[i].lastSeen) <= BLE_DET_HOLD_MS) order[vis++] = i;
    std::sort(order, order + vis, [&](int a, int b) { return snap[a].rssi > snap[b].rssi; });
    return vis;
}

static int bleDetFindVisible(
    const BleDetEntry *snap, const int *order, int vis, const uint8_t *addr, uint8_t type
) {
    for (int i = 0; i < vis; i++) {
        const BleDetEntry &e = snap[order[i]];
        if (e.addrType == type && memcmp(e.addr, addr, 6) == 0) return i;
    }
    return -1;
}

static void bleDetDraw(bool full, int count, uint32_t total, uint32_t uptimeMs, bool paused) {
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
    // must never hold up the BLE host task. The count has to be read inside the
    // same section, or a new advertiser between the two reads is copied past the
    // end of the valid entries.
    BleDetEntry snap[BLE_DET_MAX];
    int order[BLE_DET_MAX];
    int n = 0;
    const int vis = bleDetSnapshot(snap, order, n);
    const uint32_t now = millis();

    int flagged = 0;
    for (int i = 0; i < vis; i++)
        if (bleDetBeaconLike(snap[order[i]], now)) flagged++;

    const int selIdx = bleDetSelValid ? bleDetFindVisible(snap, order, vis, bleDetSelAddr, bleDetSelType) : -1;
    if (bleDetSelValid && selIdx < 0) bleDetSelValid = false; // the device left
    const int pages = bleDetPageCount(vis);
    const int page = (selIdx >= 0) ? (selIdx / BLE_DET_ROWS) : 0;
    const int first = page * BLE_DET_ROWS;

    for (int r = 0; r < BLE_DET_ROWS; r++) {
        int y = 55 + r * 13;
        // Clear the whole band: names change length as advertisers rotate.
        tft.fillRect(6, y - 2, tftWidth - 12, 12, bruceConfig.bgColor);
        String line;
        int idx = first + r;
        if (idx < vis) {
            const BleDetEntry &e = snap[order[idx]];
            bool isNew = (millis() - e.lastSeen) < BLE_DET_NEW_MS;
            bool beaconLike = bleDetBeaconLike(e, now);
            bool selected = (idx == selIdx);
            String nm = e.name[0] ? String(e.name) : String("<?>");
            line = String(" ") + bleDetLetter(e) + " " + bleDetPadR(nm, 16) + " " +
                   bleDetAddrString(e.addr) + " " + bleDetPadL(String(e.rssi), 4) + " " +
                   bleDetPadL(bleDetDistance(e.rssi), 5);
            if (selected) {
                // Inverted row, the way a Flipper highlights the current entry.
                tft.fillRect(6, y - 2, tftWidth - 12, 12, bruceConfig.priColor);
                tft.setTextColor(bruceConfig.bgColor, bruceConfig.priColor);
                tft.setTextSize(1);
                tft.setCursor(8, y);
                tft.print(line);
            } else {
                bleDetLine(line, 8, y, isNew ? bruceConfig.priColor : bruceConfig.secColor);
            }
            // Left-edge marker: a non-connectable beacon that never stops. The
            // marker sits outside the text so the columns keep their width.
            if (beaconLike)
                tft.fillRect(2, y, 3, 8, selected ? bruceConfig.bgColor : bruceConfig.secColor);
        }
    }
    if (vis == 0) bleDetLine("   scanning...", 8, 55, bruceConfig.secColor);

    tft.drawFastHLine(10, tftHeight - 24, tftWidth - 20, bruceConfig.priColor);
    String footer = String("seen ") + String(vis) + "/" + String(n);
    if (pages > 1) footer += String("  pg ") + String(page + 1) + "/" + String(pages);
    if (flagged > 0) footer += "  " + String(flagged) + " beacon-like";
    // A drone is the one row worth calling out from the footer: it is a legal
    // broadcast, not a suspect, so it is marked DRONE and never "threat".
    portENTER_CRITICAL(&bleDetDroneMux);
    const bool droneNow = bleDetDrone.valid && (millis() - bleDetDrone.ms) < 30000;
    portEXIT_CRITICAL(&bleDetDroneMux);
    if (droneNow) footer += "  DRONE";
    footer += "  up " + String(uptimeMs / 1000) + "s";
    bleDetLine(footer, 10, tftHeight - 21, bruceConfig.secColor);
    bleDetLine(
        pages > 1 ? String("TURN pg  SEL track/menu  BACK exit") : String("SEL track/menu  BACK exit"), 10,
        tftHeight - 10, bruceConfig.secColor
    );
}

// ---------------------------------------------------------------------------
// Track page (task 1.1)
// ---------------------------------------------------------------------------

// Reads one entry by address. The callback keeps updating the table while this
// page is open - that is the whole point - so every value here is live, not a
// copy taken when the page was opened.
static bool bleDetFind(const uint8_t *addr, uint8_t type, BleDetEntry &out) {
    bool found = false;
    portENTER_CRITICAL(&bleDetMux);
    if (bleDet != nullptr) {
        for (int i = 0; i < bleDetCount; i++) {
            if (bleDet[i].used && bleDet[i].addrType == type && memcmp(bleDet[i].addr, addr, 6) == 0) {
                out = bleDet[i];
                found = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&bleDetMux);
    return found;
}

// -95 dBm (edge of usable) to -40 dBm (on top of it). A relative bar, like the
// distance figure: RSSI depends on the antenna and the body in the way.
static int bleDetSignalPct(int8_t rssi) {
    if (rssi == 0) return 0;
    int v = ((int)rssi + 95) * 100 / 55;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static void bleDetTrackDraw(const BleDetEntry &e, bool gone, const char *status) {
    drawMainBorder();
    tft.setTextSize(1);
    String title = String("TRACK ") + bleDetLetter(e) + " ";
    if (e.name[0]) title += e.name;
    else title += "<?>";
    bleDetLine(title, 10, 26, bruceConfig.priColor);
    bleDetLine(bleDetAddrString(e.addr), 10, 37, bruceConfig.secColor);

    // The one number you can read at arm's length.
    char big[8];
    snprintf(big, sizeof(big), "%d", (int)e.rssi);
    tft.setTextColor(e.rssi == 0 ? bruceConfig.secColor : bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(3);
    tft.setCursor(12, 54);
    tft.print(big);
    tft.setTextSize(1);
    bleDetLine("dBm", 12 + (int)strlen(big) * 18 + 4, 66, bruceConfig.secColor);

    // Distance and the since-listed range, right-hand side.
    tft.setTextSize(2);
    tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
    tft.drawRightString(bleDetDistance(e.rssi), tftWidth - 12, 58, 1);
    tft.setTextSize(1);
    bleDetLine(
        String("min ") + String(e.rssiMin) + "  max " + String(e.rssiMax) + "  n " + String(e.seen), tftWidth - 150,
        80, bruceConfig.secColor
    );

    // Proximity bar in the theme ramp: longer is closer, and the shape of the
    // bar is readable even when the digits are not.
    const int barX = 12, barY = 96, barW = tftWidth - 24, barH = 18;
    tft.drawRect(barX - 1, barY - 1, barW + 2, barH + 2, bruceConfig.secColor);
    tft.fillRect(barX, barY, barW, barH, bruceConfig.bgColor);
    const int pct = bleDetSignalPct(e.rssi);
    if (pct > 0) uiGradientFillRect(barX, barY, barW * pct / 100, barH);

    bleDetLine(gone ? "not advertising - holding last" : status, 12, 122, bruceConfig.secColor);
    bleDetLine("held by address; random addresses look like new devices", 12, 134, bruceConfig.secColor);
    bleDetLine("BACK exit", 12, tftHeight - 10, bruceConfig.secColor);
}

static void bleDetTrack(NimBLEScan *scan, const uint8_t *addr, uint8_t type, bool tracker) {
    BleDetEntry e;
    if (!bleDetFind(addr, type, e)) return;

    // The key that opened this page is still down: without this the first poll
    // reads SEL as pressed and the page closes before it ever drew.
    while (check(SelPress) || check(EscPress) || check(PrevPress) || check(NextPress)) vTaskDelay(pdMS_TO_TICKS(10));
    delay(120);

    int8_t ref = 0;         // RSSI about a second ago, for the trend
    uint32_t refMs = millis();
    uint32_t lastDraw = 0;
    uint32_t lastCue = 0;
    uint32_t lastScanMs = millis();
    bool gone = false;
    const char *status = "steady";

    while (true) {
        // The scan window expires after 3 s; keep re-arming it here too, or the
        // numbers freeze the moment the user opens this page.
        if (scan->isScanning()) {
            lastScanMs = millis();
        } else if (millis() - lastScanMs >= BLE_DET_RESTART_MS) {
            lastScanMs = millis();
            bleDetArmScan(scan);
        }

        BleDetEntry cur;
        const bool have = bleDetFind(addr, type, cur);
        if (have) {
            e = cur;
            gone = false;
        } else {
            gone = true;
        }

        if (millis() - refMs >= 800) {
            // A 2 dB step is inside the CC1101/NimBLE noise, so only a clear
            // rise counts: a tracker's cue that fires at random is worse than
            // no cue at all.
            const bool rising = ref != 0 && e.rssi != 0 && e.rssi > ref + 2;
            const bool falling = ref != 0 && e.rssi != 0 && e.rssi < ref - 2;
            status = rising ? "signal rising" : (falling ? "signal falling" : "steady");
            if (rising && tracker && millis() - lastCue > 2500) {
                soundPlay(CUE_TRACKER);
                lastCue = millis();
            }
            ref = e.rssi;
            refMs = millis();
        }

        if (millis() - lastDraw >= 150) {
            bleDetTrackDraw(e, gone, status);
            lastDraw = millis();
        }

        if (check(EscPress) || check(SelPress)) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // Swallow the release, so the detector loop does not see the same press.
    while (check(SelPress) || check(EscPress)) vTaskDelay(pdMS_TO_TICKS(10));
}

// ---------------------------------------------------------------------------
// Remote ID detail page (task 1.2)
// ---------------------------------------------------------------------------

// Everything here is what the drone itself broadcast - position, altitude and
// the pilot's position come from its own frames, never from a guess. The one
// derived figure is the RSSI distance, which is marked with a '~' like every
// other distance on this page.
static void bleDetDroneDraw(const BleDetDrone &d, uint32_t ageMs) {
    drawMainBorder();
    tft.setTextSize(1);
    bleDetLine("REMOTE ID", 10, 26, bruceConfig.priColor);
    bleDetLine(
        String(odidStatusName(d.info.status)) + "  " + odidUaTypeName(d.info.uaType), 10, 38,
        d.info.status == 3 ? bruceConfig.priColor : bruceConfig.secColor
    );
    tft.drawFastHLine(10, 48, tftWidth - 20, bruceConfig.secColor);

    bleDetLine(
        d.info.uasId[0] ? String("id  ") + d.info.uasId : String("id  (none broadcast)"), 10, 56,
        bruceConfig.secColor
    );
    bleDetLine(
        d.info.opId[0] ? String("op  ") + d.info.opId : String("op  (none broadcast)"), 10, 68,
        bruceConfig.secColor
    );

    char lat[24], lon[24];
    snprintf(lat, sizeof(lat), "%.6f", d.info.lat);
    snprintf(lon, sizeof(lon), "%.6f", d.info.lon);
    bleDetLine(String("drone ") + lat + ", " + lon, 10, 82, bruceConfig.secColor);
    snprintf(lat, sizeof(lat), "%.6f", d.info.opLat);
    snprintf(lon, sizeof(lon), "%.6f", d.info.opLon);
    bleDetLine(String("pilot ") + lat + ", " + lon, 10, 94, bruceConfig.secColor);

    char alt[48];
    snprintf(
        alt, sizeof(alt), "alt %.0fm geo %.0fm h  spd %.1f m/s", d.info.altGeo, d.info.height, d.info.speedH
    );
    bleDetLine(alt, 10, 108, bruceConfig.secColor);
    snprintf(alt, sizeof(alt), "rssi %d  ~%s away", d.rssi, bleDetDistance(d.rssi).c_str());
    bleDetLine(alt, 10, 120, bruceConfig.secColor);

    bleDetLine(
        ageMs < 5000 ? String("live") : String("last frame ") + String(ageMs / 1000) + "s ago", 10, 134,
        bruceConfig.secColor
    );
    bleDetLine("BACK exit", 10, tftHeight - 10, bruceConfig.secColor);
}

static void bleDetDronePage() {
    while (check(SelPress) || check(EscPress) || check(PrevPress) || check(NextPress))
        vTaskDelay(pdMS_TO_TICKS(10));
    delay(120);
    while (true) {
        BleDetDrone copy;
        portENTER_CRITICAL(&bleDetDroneMux);
        copy = bleDetDrone;
        portEXIT_CRITICAL(&bleDetDroneMux);
        bleDetDroneDraw(copy, millis() - copy.ms);
        if (check(EscPress) || check(SelPress)) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    while (check(SelPress) || check(EscPress)) vTaskDelay(pdMS_TO_TICKS(10));
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

    // Table first: a detector that cannot record devices is better refused before
    // the BLE stack is brought up than after.
    if (!bleDetEnsure()) {
        displayError("BLE: out of RAM", true);
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
    memset(bleDet, 0, BLE_DET_BYTES);
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
    uint32_t lastDraw = 0;
    uint32_t lastScanMs = millis();
    bleDetSelValid = false;

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

        // `full` forces a repaint for a cursor move or a menu return: while
        // paused the 400 ms timer is the only redraw path, and the old gate made
        // the highlight invisible until Hold was turned off again.
        if (!paused && (full || millis() - lastDraw > 400)) {
            portENTER_CRITICAL(&bleDetMux);
            int count = bleDetCount;
            uint32_t total = bleDetTotal;
            portEXIT_CRITICAL(&bleDetMux);
            bleDetDraw(full, count, total, millis() - bleDetStartMs, paused);
            full = false;
            lastDraw = millis();
        }

        // Cursor: the encoder (or up/down) moves one row and the page follows the
        // selection. Before this the knob paged a list the user could not point
        // at, so opening a specific AirTag to watch its signal was impossible.
        const bool prev = check(PrevPress) || check(UpPress);
        const bool next = check(NextPress) || check(DownPress);
        if (prev || next) {
            BleDetEntry snap[BLE_DET_MAX];
            int order[BLE_DET_MAX];
            int n = 0;
            const int vis = bleDetSnapshot(snap, order, n);
            if (vis > 0) {
                int idx = bleDetSelValid ? bleDetFindVisible(snap, order, vis, bleDetSelAddr, bleDetSelType) : -1;
                if (prev) idx = (idx <= 0) ? vis - 1 : idx - 1;
                else idx = (idx < 0 || idx + 1 >= vis) ? 0 : idx + 1;
                const BleDetEntry &e = snap[order[idx]];
                memcpy(bleDetSelAddr, e.addr, 6);
                bleDetSelType = e.addrType;
                bleDetSelValid = true;
                soundPlay(CUE_NAV_TICK);
            }
            lastDraw = millis() - 500; // repaint immediately, paused or not
            full = true;
        }

        if (check(SelPress)) {
            BleDetEntry snap[BLE_DET_MAX];
            int order[BLE_DET_MAX];
            int n = 0;
            const int vis = bleDetSnapshot(snap, order, n);
            const int idx = bleDetSelValid ? bleDetFindVisible(snap, order, vis, bleDetSelAddr, bleDetSelType) : -1;

            uint8_t trackAddr[6] = {0};
            uint8_t trackType = 0;
            bool canTrack = false;
            String trackName = "device";
            if (idx >= 0) {
                const BleDetEntry &e = snap[order[idx]];
                memcpy(trackAddr, e.addr, 6);
                trackType = e.addrType;
                if (e.name[0]) trackName = e.name;
                canTrack = e.vendor >= 0 && e.vendor < BLE_DET_VENDOR_COUNT && bleDetVendors[e.vendor].tracker;
            }

            std::vector<Option> opts;
            // A decoded Remote ID pack gets its own page: identity, pilot
            // position and altitude, all of it broadcast by the drone itself.
            if (idx >= 0 && bleDetIsDrone(trackAddr, trackType))
                opts.emplace_back("Drone detail", [&]() {
                    bleDetDronePage();
                    full = true;
                });
            // The Track page is offered on recognised trackers only: a live RSSI
            // bar on an arbitrary beacon is noise, not information.
            if (canTrack)
                opts.emplace_back(String("Track ") + trackName, [&]() {
                    bleDetTrack(scan, trackAddr, trackType, true);
                    full = true;
                });
            opts.emplace_back(paused ? "Resume" : "Hold", [&]() {
                paused = !paused;
                full = true;
            });
            opts.emplace_back("Clear list", [&]() {
                portENTER_CRITICAL(&bleDetMux);
                memset(bleDet, 0, BLE_DET_BYTES);
                bleDetCount = 0;
                bleDetTotal = 0;
                portEXIT_CRITICAL(&bleDetMux);
                bleDetStartMs = millis();
                bleDetSelValid = false;
                full = true;
            });
            opts.emplace_back("Exit", []() {});
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
    if (bleDet != nullptr) memset(bleDet, 0, BLE_DET_BYTES);
    bleDetCount = 0;
    portEXIT_CRITICAL(&bleDetMux);
    returnToMenu = true;
}
