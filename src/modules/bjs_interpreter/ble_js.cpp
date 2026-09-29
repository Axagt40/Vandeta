#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "ble_js.h"
#include "modules/ble/ble_capture.h"
#include "modules/ble/ble_common.h"
#include "modules/ble/ble_spam.h"

#include <NimBLEDevice.h>
#include <NimBLERemoteCharacteristic.h>

// The BLE stack is brought up through bleInit() (modules/ble/ble_common.h).
// A raw NimBLEDevice::init() that fails late leaves the controller initialised
// and enabled, and NimBLEDevice::deinit() cannot undo that, so the memory is
// lost for the rest of the boot; bleInit() releases the controller again.
// A script runs with far less free internal RAM than the on-device BLE menus
// and the HCI buffers must be DMA-capable, so this is the usual failure path.
static bool bleJsStackUp() { return BLEDevice::isInitialized(); }

// Once an attempt has failed there is no point trying again in this boot: the
// controller state would only earn another ESP_ERR_INVALID_STATE. Latching also
// keeps the console readable.
static bool bleJsStackFailed = false;

static bool bleJsEnsureStack(const char *name) {
    if (bleJsStackUp()) return true;
    if (bleJsStackFailed) return false;
    if (bleInit(name == nullptr ? "" : name)) return true;
    bleJsStackFailed = true;
    return false;
}

JSValue native_bleScan(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.scan(timeout_seconds)
    // returns: array of objects [{name, address, rssi, addrType, connectable}, ...]
    int timeout = 5; // default 5 seconds
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) {
        JS_ToInt32(ctx, &timeout, argv[0]);
    }
    if (timeout < 1) timeout = 1;
    if (timeout > 30) timeout = 30;

    if (!bleJsEnsureStack(nullptr)) return JS_NewArray(ctx, 0);
    NimBLEScan *pScan = BLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);

#if __has_include(<NimBLEExtAdvertising.h>)
    BLEScanResults results = pScan->getResults(timeout * 1000, false);
    int count = results.getCount();
#else
    BLEScanResults results = pScan->start(timeout, false);
    int count = results.getCount();
#endif

    // Pinned: the loop below allocates per device, which can compact the heap.
    // See native_wifiGetCapturedPackets() in wifi_js.cpp for the full hazard.
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, 0);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (int i = 0; i < count; i++) {
#if __has_include(<NimBLEExtAdvertising.h>)
        const NimBLEAdvertisedDevice *dev = results.getDevice(i);
#else
        NimBLEAdvertisedDevice dev_obj = results.getDevice(i);
        NimBLEAdvertisedDevice *dev = &dev_obj;
#endif
        JSGCRef obj_ref;
        JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
        *obj = JS_NewObject(ctx);
        if (JS_IsException(*obj)) {
            JS_PopGCRef(ctx, &obj_ref);
            JS_PopGCRef(ctx, &arr_ref);
            return JS_ThrowOutOfMemory(ctx);
        }
        String name = dev->getName().c_str();
        if (name.isEmpty()) name = "<unknown>";
        JS_SetPropertyStr(ctx, *obj, "name", JS_NewString(ctx, name.c_str()));
        JS_SetPropertyStr(ctx, *obj, "address", JS_NewString(ctx, dev->getAddress().toString().c_str()));
        JS_SetPropertyStr(ctx, *obj, "rssi", JS_NewInt32(ctx, dev->getRSSI()));
        // addrType is what ble.connect() wants as its third argument: 0 for a
        // public address, 1 for the random addresses most modern devices use.
        JS_SetPropertyStr(ctx, *obj, "addrType", JS_NewInt32(ctx, dev->getAddressType()));
        JS_SetPropertyStr(ctx, *obj, "connectable", JS_NewBool(dev->isConnectable()));
        JS_SetPropertyUint32(ctx, *arr, i, *obj);
        JS_PopGCRef(ctx, &obj_ref);
    }

    pScan->clearResults();
    return JS_PopGCRef(ctx, &arr_ref);
}

JSValue native_bleAdvertise(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.advertise(name_string)
    const char *name = "Bruce-App";
    JSCStringBuf name_buf;
    if (argc > 0 && JS_IsString(ctx, argv[0])) {
        name = JS_ToCString(ctx, argv[0], &name_buf);
    }

    if (!bleJsEnsureStack(name)) return JS_NewBool(false);
    NimBLEServer *pServer = BLEDevice::createServer();
    pServer->getAdvertising()->start();

    return JS_NewBool(true);
}

JSValue native_bleStopAdvertise(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    NimBLEDevice::getAdvertising()->stop();
    BLEDevice::deinit();
    return JS_UNDEFINED;
}

// ============================================================================
// BLE spam bindings.
// The on-screen spam menu blocks until a button is pressed, so it cannot be
// called from a script. These bindings drive the same advertisement builders
// but stop on a deadline.
// ============================================================================

JSValue native_bleSpamModes(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.spamModes()
    // returns: array of selectable spam mode names, indexed as ble.spam() expects
    int count = headlessBleSpamModeCount();
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, count);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (int i = 0; i < count; i++) {
        JS_SetPropertyUint32(ctx, *arr, i, JS_NewString(ctx, headlessBleSpamModeName(i)));
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

JSValue native_bleSpam(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.spam(type?: int | string, seconds?: int)
    //   type: index into ble.spamModes() (default 0), or a mode name such as
    //         "Samsung BLE Spam" / "Random / All"
    // returns: number of advertisement packets sent
    int type = 0;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) {
        JS_ToInt32(ctx, &type, argv[0]);
    } else if (argc > 0 && JS_IsString(ctx, argv[0])) {
        JSCStringBuf nb;
        const char *name = JS_ToCString(ctx, argv[0], &nb);
        type = -1;
        if (name != NULL) {
            int count = headlessBleSpamModeCount();
            for (int i = 0; i < count; i++) {
                if (strcasecmp(name, headlessBleSpamModeName(i)) == 0) {
                    type = i;
                    break;
                }
            }
        }
        if (type < 0) type = 0;
    }

    int seconds = 10;
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &seconds, argv[1]);
    if (seconds < 1) seconds = 1;
    if (seconds > 300) seconds = 300;

    int sent = headlessBleSpam((uint8_t)type, (uint32_t)seconds * 1000);
    return JS_NewInt32(ctx, sent);
}

// ============================================================================
// GATT client bindings: init / setAddress / connect / disconnect / getService
// / getCharacteristic / read / write / notify.
//
// The engine keeps one NimBLEClient plus small tables of the services and
// characteristics the script asked about, so a script refers to them by a
// small integer index. Indices stay valid until the next connect() or
// disconnect(). GATT traffic is synchronous and can take hundreds of
// milliseconds, so scripts should not poll it in a tight loop.
// ============================================================================

#define BLE_JS_MAX_SERVICES 10
#define BLE_JS_MAX_CHARS 24
#define BLE_JS_EVENT_QUEUE 8
#define BLE_JS_EVENT_MAX 64

static NimBLEClient *bleJsClient = nullptr;
static int bleJsServiceCount = 0;
static int bleJsCharCount = 0;
static NimBLERemoteService *bleJsServices[BLE_JS_MAX_SERVICES] = {nullptr};
static NimBLERemoteCharacteristic *bleJsChars[BLE_JS_MAX_CHARS] = {nullptr};

// Notifications arrive on the NimBLE host task, where calling into the JS
// engine would be unsafe. The trampoline only copies the bytes into a lock-free
// ring buffer; ble.pollEvents() drains it from the script's own task.
struct BleJsEvent {
    int characteristic;
    uint16_t len;
    uint8_t data[BLE_JS_EVENT_MAX];
};
static BleJsEvent bleJsEvents[BLE_JS_EVENT_QUEUE];
static volatile int bleJsEvHead = 0;
static volatile int bleJsEvTail = 0;

static void bleJsNotifyTrampoline(int charIdx, uint8_t *data, size_t len) {
    int next = (bleJsEvHead + 1) % BLE_JS_EVENT_QUEUE;
    if (next == bleJsEvTail) return; // queue full: drop rather than block the host task
    BleJsEvent &e = bleJsEvents[bleJsEvHead];
    e.characteristic = charIdx;
    e.len = (uint16_t)(len > BLE_JS_EVENT_MAX ? BLE_JS_EVENT_MAX : len);
    if (e.len > 0 && data != nullptr) memcpy(e.data, data, e.len);
    bleJsEvHead = next;
}

static void bleJsForgetHandles() {
    for (int i = 0; i < bleJsServiceCount; i++) bleJsServices[i] = nullptr;
    for (int i = 0; i < bleJsCharCount; i++) bleJsChars[i] = nullptr;
    bleJsServiceCount = 0;
    bleJsCharCount = 0;
    bleJsEvHead = 0;
    bleJsEvTail = 0;
}

static void bleJsDropClient() {
    if (bleJsClient != nullptr) {
        if (bleJsClient->isConnected()) bleJsClient->disconnect();
        BLEDevice::deleteClient(bleJsClient);
        bleJsClient = nullptr;
    }
    bleJsForgetHandles();
}

// ---------------------------------------------------------------------------
// Small conversions
// ---------------------------------------------------------------------------

static void bleJsToHex(const uint8_t *data, size_t len, char *out) {
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    out[len * 2] = 0;
}

// Returns the number of bytes decoded, or -1 when the text is not valid hex.
static int bleJsFromHex(const char *s, uint8_t *out, int maxOut) {
    if (s == nullptr) return -1;
    int n = 0;
    while (*s != 0 && n < maxOut) {
        // Allow the usual separators between byte pairs.
        if (*s == ' ' || *s == ':' || *s == '-' || *s == ',') {
            s++;
            continue;
        }
        int hi = -1, lo = -1;
        char c = *s++;
        if (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        else return -1;
        c = *s;
        if (c == 0) return -1; // odd digit count
        s++;
        if (c >= '0' && c <= '9') lo = c - '0';
        else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
        else return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

// Accepts a typed array, an array of byte values, a hex string, or (when
// `asText` is set) a plain string whose UTF-8/latin1 bytes are used verbatim.
static int bleJsBytesFromValue(JSContext *ctx, JSValue val, uint8_t *out, int maxOut, bool asText) {
    if (JS_IsTypedArray(ctx, val)) {
        size_t len = 0;
        const char *buf = JS_GetTypedArrayBuffer(ctx, &len, val);
        if (buf == nullptr) return -1;
        if ((int)len > maxOut) len = (size_t)maxOut;
        memcpy(out, buf, len);
        return (int)len;
    }
    if (JS_IsObject(ctx, val)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, val, "length");
        int len = 0;
        if (JS_ToInt32(ctx, &len, lenVal) != 0 || len <= 0) return -1;
        if (len > maxOut) len = maxOut;
        for (int i = 0; i < len; i++) {
            int b = 0;
            // JS_GetPropertyUint32() allocates, so re-read nothing else in here.
            if (JS_ToInt32(ctx, &b, JS_GetPropertyUint32(ctx, val, i)) != 0) b = 0;
            out[i] = (uint8_t)b;
        }
        return len;
    }
    if (JS_IsString(ctx, val)) {
        size_t len = 0;
        JSCStringBuf sb;
        const char *s = JS_ToCStringLen(ctx, &len, val, &sb);
        if (s == nullptr) return -1;
        if (!asText) {
            int n = bleJsFromHex(s, out, maxOut);
            if (n >= 0) return n;
        }
        if ((int)len > maxOut) len = (size_t)maxOut;
        memcpy(out, s, len);
        return (int)len;
    }
    return -1;
}

static String bleJsAddressOf(const BleCapturedPacket &p) {
    char b[20];
    snprintf(
        b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", p.addr[5], p.addr[4], p.addr[3], p.addr[2], p.addr[1], p.addr[0]
    );
    return String(b);
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

JSValue native_bleInit(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.init(name?)
    // Brings the BLE stack up and clears any previous connection.
    // returns: false when the stack cannot be started (usually not enough free
    //          internal RAM while a script is running - try the BLE menu instead)
    bleJsDropClient();
    JSCStringBuf nb;
    const char *name = "";
    if (argc > 0 && JS_IsString(ctx, argv[0])) name = JS_ToCString(ctx, argv[0], &nb);
    return JS_NewBool(bleJsEnsureStack(name));
}

JSValue native_bleSetAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.setAddress("AA:BB:CC:DD:EE:FF", type?)
    //   type: 0 public (default), 1 random
    if (argc < 1) return JS_NewBool(false);
    int type = 0;
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &type, argv[1]);

    JSCStringBuf sb;
    String addr = JS_IsString(ctx, argv[0]) ? String(JS_ToCString(ctx, argv[0], &sb)) : String();
    if (addr.length() == 0) return JS_NewBool(false);

    // The controller must be up before its address can be changed.
    if (!bleJsEnsureStack(nullptr)) return JS_NewBool(false);
    if (!BLEDevice::setOwnAddrType(type ? BLE_OWN_ADDR_RANDOM : BLE_OWN_ADDR_PUBLIC)) return JS_NewBool(false);
    return JS_NewBool(BLEDevice::setOwnAddr(NimBLEAddress(std::string(addr.c_str()), (uint8_t)type)));
}

JSValue native_bleConnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.connect("AA:BB:CC:DD:EE:FF", timeout_ms?, addrType?)
    //   addrType comes from ble.scan()/scanDetailed(): 0 public, 1 random
    // returns: true once connected and the service list is readable
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_NewBool(false);
    JSCStringBuf sb;
    const char *addrArg = JS_ToCString(ctx, argv[0], &sb);
    String addr = addrArg == nullptr ? String() : String(addrArg);
    if (addr.length() == 0) return JS_NewBool(false);

    int timeout = 5000;
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &timeout, argv[1]);
    if (timeout < 500) timeout = 500;
    if (timeout > 30000) timeout = 30000;
    int addrType = 0;
    if (argc > 2 && JS_IsNumber(ctx, argv[2])) JS_ToInt32(ctx, &addrType, argv[2]);

    bleJsDropClient();
    if (!bleJsEnsureStack(nullptr)) return JS_NewBool(false);

    bleJsClient = BLEDevice::createClient();
    if (bleJsClient == nullptr) return JS_NewBool(false);
    bleJsClient->setConnectTimeout(timeout);

    if (!bleJsClient->connect(NimBLEAddress(std::string(addr.c_str()), (uint8_t)addrType))) {
        bleJsDropClient();
        return JS_NewBool(false);
    }
    // Force service discovery now so getService() is cheap and its failures
    // are attributed to connect() rather than to a later call.
    bleJsClient->getServices(true);
    return JS_NewBool(true);
}

JSValue native_bleDisconnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.disconnect() -- also invalidates every service/char index
    bleJsDropClient();
    return JS_NewBool(true);
}

JSValue native_bleIsConnected(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.isConnected()
    return JS_NewBool(bleJsClient != nullptr && bleJsClient->isConnected());
}

JSValue native_bleAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.address() -- this device's own BLE address, "" when the stack
    // is not up (reading it would touch an uninitialised host and assert)
    if (!bleJsStackUp()) return JS_NewString(ctx, "");
    return JS_NewString(ctx, BLEDevice::getAddress().toString().c_str());
}

static bool bleJsServiceIndex(const char *uuid, int *outIdx) {
    if (bleJsClient == nullptr || !bleJsClient->isConnected()) return false;
    NimBLERemoteService *svc = bleJsClient->getService(NimBLEUUID(std::string(uuid)));
    if (svc == nullptr) return false;
    for (int i = 0; i < bleJsServiceCount; i++) {
        if (bleJsServices[i] == svc) {
            *outIdx = i;
            return true;
        }
    }
    if (bleJsServiceCount >= BLE_JS_MAX_SERVICES) return false;
    bleJsServices[bleJsServiceCount] = svc;
    *outIdx = bleJsServiceCount++;
    return true;
}

static bool bleJsCharIndex(NimBLERemoteService *svc, const char *uuid, int *outIdx) {
    if (svc == nullptr) return false;
    NimBLERemoteCharacteristic *ch = svc->getCharacteristic(NimBLEUUID(std::string(uuid)));
    if (ch == nullptr) return false;
    for (int i = 0; i < bleJsCharCount; i++) {
        if (bleJsChars[i] == ch) {
            *outIdx = i;
            return true;
        }
    }
    if (bleJsCharCount >= BLE_JS_MAX_CHARS) return false;
    bleJsChars[bleJsCharCount] = ch;
    *outIdx = bleJsCharCount++;
    return true;
}

JSValue native_bleGetService(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.getService("180f")   (full or 16-bit UUID, case insensitive)
    // returns: service index for getCharacteristic()/characteristics(), or -1
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_NewInt32(ctx, -1);
    JSCStringBuf sb;
    const char *uuid = JS_ToCString(ctx, argv[0], &sb);
    int idx = -1;
    if (uuid == nullptr || !bleJsServiceIndex(uuid, &idx)) return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, idx);
}

JSValue native_bleServices(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.services() -- UUID strings of all services on the peer
    if (bleJsClient == nullptr || !bleJsClient->isConnected()) return JS_NewArray(ctx, 0);
    const std::vector<NimBLERemoteService *> &svcs = bleJsClient->getServices();

    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, 0);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (size_t i = 0; i < svcs.size(); i++) {
        if (svcs[i] == nullptr) continue;
        JS_SetPropertyUint32(ctx, *arr, i, JS_NewString(ctx, svcs[i]->getUUID().toString().c_str()));
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

JSValue native_bleCharacteristics(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.characteristics(serviceIndex | "180f")
    // returns: [{uuid, handle, index, readable, writable, notify}, ...]
    if (argc < 1) return JS_NewArray(ctx, 0);

    NimBLERemoteService *svc = nullptr;
    if (JS_IsNumber(ctx, argv[0])) {
        int idx = -1;
        JS_ToInt32(ctx, &idx, argv[0]);
        if (idx >= 0 && idx < bleJsServiceCount) svc = bleJsServices[idx];
    } else if (JS_IsString(ctx, argv[0])) {
        JSCStringBuf sb;
        const char *uuid = JS_ToCString(ctx, argv[0], &sb);
        int idx = -1;
        if (uuid != nullptr && bleJsServiceIndex(uuid, &idx)) svc = bleJsServices[idx];
    }
    if (svc == nullptr) return JS_NewArray(ctx, 0);

    const std::vector<NimBLERemoteCharacteristic *> &chars = svc->getCharacteristics(true);

    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, 0);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (size_t i = 0; i < chars.size(); i++) {
        NimBLERemoteCharacteristic *ch = chars[i];
        if (ch == nullptr) continue;
        int charIdx = -1;
        bleJsCharIndex(svc, ch->getUUID().toString().c_str(), &charIdx);

        JSGCRef obj_ref;
        JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
        *obj = JS_NewObject(ctx);
        if (JS_IsException(*obj)) {
            JS_PopGCRef(ctx, &obj_ref);
            JS_PopGCRef(ctx, &arr_ref);
            return JS_ThrowOutOfMemory(ctx);
        }
        JS_SetPropertyStr(ctx, *obj, "uuid", JS_NewString(ctx, ch->getUUID().toString().c_str()));
        JS_SetPropertyStr(ctx, *obj, "handle", JS_NewInt32(ctx, ch->getHandle()));
        JS_SetPropertyStr(ctx, *obj, "index", JS_NewInt32(ctx, charIdx));
        JS_SetPropertyStr(ctx, *obj, "readable", JS_NewBool(ch->canRead()));
        JS_SetPropertyStr(ctx, *obj, "writable", JS_NewBool(ch->canWrite() || ch->canWriteNoResponse()));
        JS_SetPropertyStr(ctx, *obj, "notify", JS_NewBool(ch->canNotify() || ch->canIndicate()));
        JS_SetPropertyUint32(ctx, *arr, i, *obj);
        JS_PopGCRef(ctx, &obj_ref);
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

JSValue native_bleGetCharacteristic(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.getCharacteristic(serviceIndex, "2a19")
    //        ble.getCharacteristic("2a19")   (searches every discovered service)
    // returns: characteristic index for read/write/notify, or -1
    if (argc < 1) return JS_NewInt32(ctx, -1);

    int idx = -1;
    if (argc >= 2) {
        int svcIdx = -1;
        if (!JS_IsNumber(ctx, argv[0])) return JS_NewInt32(ctx, -1);
        JS_ToInt32(ctx, &svcIdx, argv[0]);
        if (svcIdx < 0 || svcIdx >= bleJsServiceCount) return JS_NewInt32(ctx, -1);
        if (!JS_IsString(ctx, argv[1])) return JS_NewInt32(ctx, -1);
        JSCStringBuf sb;
        const char *uuid = JS_ToCString(ctx, argv[1], &sb);
        if (uuid == nullptr || !bleJsCharIndex(bleJsServices[svcIdx], uuid, &idx)) return JS_NewInt32(ctx, -1);
        return JS_NewInt32(ctx, idx);
    }

    if (!JS_IsString(ctx, argv[0])) return JS_NewInt32(ctx, -1);
    JSCStringBuf sb;
    String uuid = String(JS_ToCString(ctx, argv[0], &sb));

    // Walk every service the peer exposes, registering them as we go so the
    // single-argument form works without a prior getService() call.
    if (bleJsClient != nullptr && bleJsClient->isConnected()) {
        const std::vector<NimBLERemoteService *> &svcs = bleJsClient->getServices();
        for (size_t i = 0; i < svcs.size(); i++) {
            if (svcs[i] == nullptr) continue;
            if (bleJsCharIndex(svcs[i], uuid.c_str(), &idx)) return JS_NewInt32(ctx, idx);
        }
    }
    return JS_NewInt32(ctx, -1);
}

JSValue native_bleRead(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.read(charIndex, asText?)
    // returns: hex string by default, a latin1 string when asText is true,
    //          or null when the read fails
    if (argc < 1 || !JS_IsNumber(ctx, argv[0])) return JS_NULL;
    int idx = -1;
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || idx >= bleJsCharCount || bleJsChars[idx] == nullptr) return JS_NULL;
    bool asText = false;
    if (argc > 1) asText = JS_ToBool(ctx, argv[1]) != 0;

    NimBLEAttValue v = bleJsChars[idx]->readValue();
    if (v.length() == 0) return JS_NewString(ctx, "");
    if (asText) return buffer_latin1_to_string(ctx, v.data(), v.length());
    // Static rather than stack: the interpreter task stack is small and a read
    // can hold the full 512-byte attribute value.
    static char readHex[2 * 512 + 1];
    size_t len = v.length() > 512 ? 512 : v.length();
    bleJsToHex(v.data(), len, readHex);
    return JS_NewString(ctx, readHex);
}

JSValue native_bleWrite(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.write(charIndex, data, asText?, withResponse?)
    //   data: hex string ("0102FF"), array of byte values, or typed array.
    //         When asText is true a plain string is written as its raw bytes.
    if (argc < 2 || !JS_IsNumber(ctx, argv[0])) return JS_NewBool(false);
    int idx = -1;
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || idx >= bleJsCharCount || bleJsChars[idx] == nullptr) return JS_NewBool(false);
    bool asText = false;
    if (argc > 2 && !JS_IsUndefined(argv[2])) asText = JS_ToBool(ctx, argv[2]) != 0;
    bool response = false;
    if (argc > 3 && !JS_IsUndefined(argv[3])) response = JS_ToBool(ctx, argv[3]) != 0;

    static uint8_t writeBuf[512];
    int len = bleJsBytesFromValue(ctx, argv[1], writeBuf, sizeof(writeBuf), asText);
    if (len <= 0) return JS_NewBool(false);
    return JS_NewBool(bleJsChars[idx]->writeValue(writeBuf, (size_t)len, response));
}

JSValue native_bleNotify(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.notify(charIndex, enable = true, callback?)
    //   The callback is stored as the global __bleNotify and invoked by
    //   ble.pollEvents() with {characteristic, length, hex, data}.
    if (argc < 1 || !JS_IsNumber(ctx, argv[0])) return JS_NewBool(false);
    int idx = -1;
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || idx >= bleJsCharCount || bleJsChars[idx] == nullptr) return JS_NewBool(false);

    bool enable = true;
    if (argc > 1 && !JS_IsUndefined(argv[1])) enable = JS_ToBool(ctx, argv[1]) != 0;

    if (argc > 2 && JS_IsFunction(ctx, argv[2])) {
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bleNotify", argv[2]);
    }

    if (!enable) return JS_NewBool(bleJsChars[idx]->unsubscribe());

    int captured = idx;
    return JS_NewBool(bleJsChars[idx]->subscribe(
        true,
        [captured](NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
            bleJsNotifyTrampoline(captured, data, len);
        }
    ));
}

JSValue native_blePollEvents(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.pollEvents()
    // Drains queued notifications, calling __bleNotify (set by ble.notify).
    // returns: the number of events dispatched
    int dispatched = 0;

    JSGCRef cb_ref, global_ref;
    bool haveCb = false;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue *globalRef = JS_PushGCRef(ctx, &global_ref);
    *globalRef = global;
    JSValue *cbRef = JS_PushGCRef(ctx, &cb_ref);
    *cbRef = JS_GetPropertyStr(ctx, global, "__bleNotify");
    haveCb = JS_IsFunction(ctx, *cbRef) != 0;

    while (bleJsEvTail != bleJsEvHead) {
        BleJsEvent &e = bleJsEvents[bleJsEvTail];
        if (haveCb) {
            char hex[2 * BLE_JS_EVENT_MAX + 1];
            bleJsToHex(e.data, e.len, hex);
            JSGCRef arg_ref;
            JSValue *arg = JS_PushGCRef(ctx, &arg_ref);
            *arg = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, *arg, "characteristic", JS_NewInt32(ctx, e.characteristic));
            JS_SetPropertyStr(ctx, *arg, "length", JS_NewInt32(ctx, e.len));
            JS_SetPropertyStr(ctx, *arg, "hex", JS_NewString(ctx, hex));
            JS_SetPropertyStr(ctx, *arg, "data", buffer_latin1_to_string(ctx, e.data, e.len));

            // The engine may move objects during JS_StackCheck(), so every
            // pushed value is read back from its GC ref afterwards.
            if (JS_StackCheck(ctx, 3) == 0) {
                JS_PushArg(ctx, *arg);
                JS_PushArg(ctx, *cbRef);
                JS_PushArg(ctx, JS_UNDEFINED);
                JSValue ret = JS_Call(ctx, 1);
                if (JS_IsException(ret)) js_fatal_error_handler(ctx);
            }
            JS_PopGCRef(ctx, &arg_ref);
        }
        bleJsEvTail = (bleJsEvTail + 1) % BLE_JS_EVENT_QUEUE;
        dispatched++;
    }

    JS_PopGCRef(ctx, &cb_ref);
    JS_PopGCRef(ctx, &global_ref);
    return JS_NewInt32(ctx, dispatched);
}

// ============================================================================
// Capture / replay bindings -- thin wrappers over modules/ble/ble_capture.*
// so what a script records is exactly what the Capture & Replay menu sees.
// ============================================================================

JSValue native_bleCaptureStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.captureStart(maxSeconds?)
    // The scan runs in the background; call ble.captureStop() to end it early.
    int seconds = 60;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &seconds, argv[0]);
    if (seconds < 1) seconds = 1;
    if (seconds > 600) seconds = 600;
    return JS_NewBool(bleCaptureStart((uint32_t)seconds * 1000));
}

JSValue native_bleCaptureStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.captureStop()  -- returns the number of packets captured
    return JS_NewInt32(ctx, bleCaptureStop());
}

JSValue native_bleCaptureRunning(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.captureRunning()
    return JS_NewBool(bleCaptureRunning());
}

JSValue native_bleCaptureCount(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.captureCount()
    return JS_NewInt32(ctx, bleCaptureCount());
}

JSValue native_bleCaptureClear(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.captureClear()
    bleCaptureClear();
    return JS_UNDEFINED;
}

JSValue native_bleCaptureList(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.captureList(filter?, max?)
    // returns: [{index, description, address, rssi, length, hex, name}, ...]
    String filter;
    if (argc > 0 && JS_IsString(ctx, argv[0])) {
        JSCStringBuf fb;
        const char *f = JS_ToCString(ctx, argv[0], &fb);
        if (f != nullptr) filter = String(f);
    }
    int max = 20;
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &max, argv[1]);
    if (max <= 0 || max > BLE_CAP_MAX_PACKETS) max = BLE_CAP_MAX_PACKETS;

    static int idx[BLE_CAP_MAX_PACKETS];
    int n = bleCaptureFilter(filter, idx, BLE_CAP_MAX_PACKETS);
    if (n > max) n = max;

    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, 0);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    int written = 0;
    for (int i = 0; i < n; i++) {
        BleCapturedPacket p;
        if (!bleCapturePacket(idx[i], &p)) continue;
        char hex[2 * BLE_CAP_MAX_PAYLOAD + 1];
        bleJsToHex(p.data, p.len, hex);

        JSGCRef obj_ref;
        JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
        *obj = JS_NewObject(ctx);
        if (JS_IsException(*obj)) {
            JS_PopGCRef(ctx, &obj_ref);
            JS_PopGCRef(ctx, &arr_ref);
            return JS_ThrowOutOfMemory(ctx);
        }
        JS_SetPropertyStr(ctx, *obj, "index", JS_NewInt32(ctx, idx[i]));
        JS_SetPropertyStr(ctx, *obj, "description", JS_NewString(ctx, bleCaptureDescribe(p).c_str()));
        JS_SetPropertyStr(ctx, *obj, "address", JS_NewString(ctx, bleJsAddressOf(p).c_str()));
        JS_SetPropertyStr(ctx, *obj, "rssi", JS_NewInt32(ctx, p.rssi));
        JS_SetPropertyStr(ctx, *obj, "length", JS_NewInt32(ctx, p.len));
        JS_SetPropertyStr(ctx, *obj, "hex", JS_NewString(ctx, hex));
        JS_SetPropertyStr(ctx, *obj, "connectable", JS_NewBool(p.connectable));
        JS_SetPropertyUint32(ctx, *arr, written++, *obj);
        JS_PopGCRef(ctx, &obj_ref);
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

JSValue native_bleCaptureReplay(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.captureReplay(seconds?, spoofAddress?)
    // Advertises the captured packets verbatim, cycling until the time is up.
    int seconds = 3;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &seconds, argv[0]);
    if (seconds < 1) seconds = 1;
    if (seconds > 60) seconds = 60;
    bool spoof = true;
    if (argc > 1 && !JS_IsUndefined(argv[1])) spoof = JS_ToBool(ctx, argv[1]) != 0;

    static int idx[BLE_CAP_MAX_PACKETS];
    int n = bleCaptureFilter("", idx, BLE_CAP_MAX_PACKETS);
    if (n == 0) n = bleCaptureCount();
    return JS_NewBool(bleCaptureReplay(idx, n, (uint32_t)seconds * 1000, spoof));
}

JSValue native_bleCaptureSave(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.captureSave("name" | "/BruceBLE/name.ble", label?)
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_NewBool(false);
    JSCStringBuf pb;
    const char *p = JS_ToCString(ctx, argv[0], &pb);
    String path = p == nullptr ? String() : String(p);
    if (path.length() == 0) return JS_NewBool(false);
    JSCStringBuf lb;
    const char *label = "js";
    if (argc > 1 && JS_IsString(ctx, argv[1])) label = JS_ToCString(ctx, argv[1], &lb);
    return JS_NewBool(bleCaptureSave(path, label));
}

JSValue native_bleCaptureLoad(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.captureLoad("name" | "/BruceBLE/name.ble") -> packets loaded
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    JSCStringBuf pb;
    const char *p = JS_ToCString(ctx, argv[0], &pb);
    if (p == nullptr) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, bleCaptureLoad(String(p), nullptr));
}

JSValue native_bleCaptureAnalyze(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.captureAnalyze(index?)
    // returns: {index, description, address, rssi, length, hex, companyId,
    //           advType, connectable, addrType} or null
    int idx = 0;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &idx, argv[0]);
    BleCapturedPacket p;
    if (!bleCapturePacket(idx, &p)) return JS_NULL;

    char hex[2 * BLE_CAP_MAX_PAYLOAD + 1];
    bleJsToHex(p.data, p.len, hex);

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "index", JS_NewInt32(ctx, idx));
    JS_SetPropertyStr(ctx, *obj, "description", JS_NewString(ctx, bleCaptureDescribe(p).c_str()));
    JS_SetPropertyStr(ctx, *obj, "address", JS_NewString(ctx, bleJsAddressOf(p).c_str()));
    JS_SetPropertyStr(ctx, *obj, "rssi", JS_NewInt32(ctx, p.rssi));
    JS_SetPropertyStr(ctx, *obj, "length", JS_NewInt32(ctx, p.len));
    JS_SetPropertyStr(ctx, *obj, "hex", JS_NewString(ctx, hex));
    JS_SetPropertyStr(ctx, *obj, "advType", JS_NewInt32(ctx, p.advType));
    JS_SetPropertyStr(ctx, *obj, "addrType", JS_NewInt32(ctx, p.addrType));
    JS_SetPropertyStr(ctx, *obj, "connectable", JS_NewBool(p.connectable));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_bleRemoteTypes(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.remoteTypes() -- selectable capture-filter names
    int count = bleCaptureRemoteTypeCount();
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, count);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (int i = 0; i < count; i++) {
        JS_SetPropertyUint32(ctx, *arr, i, JS_NewString(ctx, bleCaptureRemoteTypeName(i)));
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

JSValue native_bleSetRemoteType(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.setRemoteType(index | "Samsung")
    if (argc < 1) return JS_NewInt32(ctx, bleCaptureRemoteType());
    int idx = -1;
    if (JS_IsNumber(ctx, argv[0])) {
        JS_ToInt32(ctx, &idx, argv[0]);
    } else if (JS_IsString(ctx, argv[0])) {
        JSCStringBuf nb;
        const char *name = JS_ToCString(ctx, argv[0], &nb);
        if (name != nullptr) {
            for (int i = 0; i < bleCaptureRemoteTypeCount(); i++) {
                if (strcasecmp(name, bleCaptureRemoteTypeName(i)) == 0) {
                    idx = i;
                    break;
                }
            }
        }
    }
    if (idx < 0) return JS_NewInt32(ctx, bleCaptureRemoteType());
    bleCaptureSetRemoteType(idx);
    return JS_NewInt32(ctx, bleCaptureRemoteType());
}

// ============================================================================
// Teardown
// ============================================================================

/// Release everything a script may have left running: a background capture and
/// the GATT client (whose attributes keep the NimBLE host task alive).
void ble_js_cleanup() {
    if (bleCaptureRunning()) bleCaptureStop();
    bleJsDropClient();
    bleJsStackFailed = false; // the next run gets a fresh attempt
}

JSValue native_bleScanDetailed(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ble.scanDetailed(seconds?, max?)
    // returns: [{address, name, rssi, advType, payloadLen, connectable}, ...]
    int seconds = 3;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &seconds, argv[0]);
    if (seconds < 1) seconds = 1;
    if (seconds > 30) seconds = 30;
    int max = 24;
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &max, argv[1]);
    if (max <= 0 || max > 64) max = 64;

    static BleScanEntry entries[64];
    int n = bleScanDetailed((uint32_t)seconds * 1000, entries, max);

    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, 0);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (int i = 0; i < n; i++) {
        JSGCRef obj_ref;
        JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
        *obj = JS_NewObject(ctx);
        if (JS_IsException(*obj)) {
            JS_PopGCRef(ctx, &obj_ref);
            JS_PopGCRef(ctx, &arr_ref);
            return JS_ThrowOutOfMemory(ctx);
        }
        JS_SetPropertyStr(ctx, *obj, "address", JS_NewString(ctx, entries[i].address));
        JS_SetPropertyStr(ctx, *obj, "name", JS_NewString(ctx, entries[i].name));
        JS_SetPropertyStr(ctx, *obj, "rssi", JS_NewInt32(ctx, entries[i].rssi));
        JS_SetPropertyStr(ctx, *obj, "advType", JS_NewInt32(ctx, entries[i].advType));
        JS_SetPropertyStr(ctx, *obj, "addrType", JS_NewInt32(ctx, entries[i].addrType));
        JS_SetPropertyStr(ctx, *obj, "payloadLen", JS_NewInt32(ctx, entries[i].payloadLen));
        JS_SetPropertyStr(ctx, *obj, "connectable", JS_NewBool(entries[i].connectable));
        JS_SetPropertyUint32(ctx, *arr, i, *obj);
        JS_PopGCRef(ctx, &obj_ref);
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

#endif
