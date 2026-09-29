#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "nrf24_js.h"
#include "helpers_js.h"
#include "modules/NRF24/nrf_capture.h"
#include "modules/NRF24/nrf_common.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Accepts "aabbcc", "AA:BB:CC", "aa bb cc" and "0xaabbcc".
static int nrfJsParseHex(const char *s, uint8_t *out, int maxLen) {
    if (s == NULL) return 0;
    int n = 0;
    int hi = -1;
    for (const char *p = s; *p != 0 && n < maxLen; p++) {
        char c = *p;
        if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p++;
            continue;
        }
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue; // separators, whitespace, anything else
        if (hi < 0) hi = v;
        else {
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return n;
}

// Bytes from a hex string, a byte array such as [0xAA, 0x55], or the first
// element of a capture array (a hex string, or an object with a `data` field).
static int nrfJsExtractBytes(JSContext *ctx, JSValue val, uint8_t *out, int maxLen) {
    if (JS_IsString(ctx, val)) {
        String s = js_tocstring_copy(ctx, val);
        return nrfJsParseHex(s.c_str(), out, maxLen);
    }
    if (JS_IsObject(ctx, val)) {
        int len = 0;
        if (JS_ToInt32(ctx, &len, JS_GetPropertyStr(ctx, val, "length")) != 0 || len <= 0) return 0;

        // An array of numbers is a raw byte array: read every element. Treating
        // element 0 as a hex string would turn [0xAA, ...] into the text "170"
        // and parse a single byte out of it (observed as len=1).
        // JS_GetPropertyUint32() allocates, so nothing here may hold a JSValue
        // across it - read each element inline, as ble_js does.
        if (JS_IsNumber(ctx, JS_GetPropertyUint32(ctx, val, 0))) {
            int n = 0;
            for (int i = 0; i < len && n < maxLen; i++) {
                int v = 0;
                if (JS_ToInt32(ctx, &v, JS_GetPropertyUint32(ctx, val, i)) != 0) v = 0;
                out[n++] = (uint8_t)(v & 0xFF);
            }
            return n;
        }

        // Otherwise it is a capture entry: a hex string, or {data: "<hex>"}.
        JSValue first = JS_GetPropertyUint32(ctx, val, 0);
        // Same pattern as dialog_js: the element is used as the receiver of the
        // lookup immediately, and the result is copied into a String (which the
        // C++ heap owns) before anything else touches the JS heap.
        String s;
        if (JS_IsObject(ctx, first)) s = js_tocstring_copy(ctx, JS_GetPropertyStr(ctx, first, "data"));
        else s = js_tocstring_copy(ctx, first);
        return nrfJsParseHex(s.c_str(), out, maxLen);
    }
    return 0;
}

// Convert a "signal" argument into packets. Accepts a hex string, an array of
// byte values (one raw payload), an array of hex strings, or an array of
// {data, channel} objects.
static int nrfJsSignalToPackets(
    JSContext *ctx, JSValue val, uint8_t defaultChannel, NrfCapturedPacket *out, int maxOut
) {
    int count = 0;
    if (JS_IsString(ctx, val)) {
        uint8_t bytes[NRF_CAP_MAX_PAYLOAD];
        String s = js_tocstring_copy(ctx, val);
        int len = nrfJsParseHex(s.c_str(), bytes, NRF_CAP_MAX_PAYLOAD);
        if (len <= 0) return 0;
        NrfCapturedPacket &p = out[count++];
        memset(&p, 0, sizeof(p));
        p.timestamp = millis();
        p.channel = defaultChannel;
        p.freqMhz = (uint16_t)(2400 + defaultChannel);
        p.len = (uint8_t)len;
        memcpy(p.data, bytes, len);
        return count;
    }
    if (!JS_IsObject(ctx, val)) return 0;

    int n = 0;
    if (JS_ToInt32(ctx, &n, JS_GetPropertyStr(ctx, val, "length")) != 0 || n <= 0) return 0;

    // A plain array of numbers is one raw payload, not a list of signals.
    if (JS_IsNumber(ctx, JS_GetPropertyUint32(ctx, val, 0))) {
        uint8_t bytes[NRF_CAP_MAX_PAYLOAD];
        int len = nrfJsExtractBytes(ctx, val, bytes, NRF_CAP_MAX_PAYLOAD);
        if (len <= 0) return 0;
        NrfCapturedPacket &p = out[count++];
        memset(&p, 0, sizeof(p));
        p.timestamp = millis();
        p.channel = defaultChannel;
        p.freqMhz = (uint16_t)(2400 + defaultChannel);
        p.len = (uint8_t)len;
        memcpy(p.data, bytes, len);
        return count;
    }

    for (int i = 0; i < n && count < maxOut; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, val, i);
        uint8_t bytes[NRF_CAP_MAX_PAYLOAD];
        int len = 0;
        uint8_t channel = defaultChannel;
        if (JS_IsObject(ctx, el)) {
            JSValue cv = JS_GetPropertyStr(ctx, el, "channel");
            int c = 0;
            if (JS_ToInt32(ctx, &c, cv) == 0) channel = (uint8_t)c;
            String s = js_tocstring_copy(ctx, JS_GetPropertyStr(ctx, el, "data"));
            len = nrfJsParseHex(s.c_str(), bytes, NRF_CAP_MAX_PAYLOAD);
        } else {
            String s = js_tocstring_copy(ctx, el);
            len = nrfJsParseHex(s.c_str(), bytes, NRF_CAP_MAX_PAYLOAD);
        }
        if (len <= 0) continue;
        NrfCapturedPacket &p = out[count++];
        memset(&p, 0, sizeof(p));
        p.timestamp = millis();
        p.channel = channel;
        p.freqMhz = (uint16_t)(2400 + channel);
        p.len = (uint8_t)len;
        memcpy(p.data, bytes, len);
    }
    return count;
}

// Build [{channel,freq,len,timestamp,data}, ...]. Pin everything: the loop
// allocates per packet, which can compact the heap (see the GC discovery note).
static JSValue nrfJsBuildCaptureArray(JSContext *ctx, const NrfCapturedPacket *pkts, int count) {
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, count);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (int i = 0; i < count; i++) {
        JSGCRef obj_ref;
        JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
        *obj = JS_NewObject(ctx);
        if (JS_IsException(*obj)) {
            JS_PopGCRef(ctx, &obj_ref);
            JS_PopGCRef(ctx, &arr_ref);
            return JS_ThrowOutOfMemory(ctx);
        }
        char hex[2 * NRF_CAP_MAX_PAYLOAD + 1];
        for (int b = 0; b < pkts[i].len; b++) snprintf(hex + b * 2, 3, "%02X", pkts[i].data[b]);
        hex[pkts[i].len * 2] = 0;
        JS_SetPropertyStr(ctx, *obj, "channel", JS_NewInt32(ctx, pkts[i].channel));
        JS_SetPropertyStr(ctx, *obj, "freq", JS_NewInt32(ctx, pkts[i].freqMhz));
        JS_SetPropertyStr(ctx, *obj, "len", JS_NewInt32(ctx, pkts[i].len));
        JS_SetPropertyStr(ctx, *obj, "timestamp", JS_NewInt32(ctx, (int32_t)pkts[i].timestamp));
        JS_SetPropertyStr(ctx, *obj, "data", JS_NewString(ctx, hex));
        JS_SetPropertyUint32(ctx, *arr, i, *obj);
        JS_PopGCRef(ctx, &obj_ref);
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

static bool nrfJsEnsureReady(uint8_t channel = 76) {
    if (nrfCaptureReady()) return true;
    NrfCaptureConfig cfg;
    cfg.startChannel = channel;
    cfg.endChannel = 125;
    return nrfCaptureInit(cfg);
}

// Hop mode is remembered here and consumed by captureRaw().
static bool nrfJsHopMode = false;

// ---------------------------------------------------------------------------
// Basic functions (stock + aliases)
// ---------------------------------------------------------------------------

JSValue native_nrf24Begin(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.begin(channel?)  /  nrf24.init(channel?)
    int channel = 76; // default
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &channel, argv[0]);

    NrfCaptureConfig cfg;
    cfg.startChannel = (uint8_t)(channel < 0 ? 0 : (channel > 125 ? 125 : channel));
    cfg.endChannel = 125;
    if (!nrfCaptureInit(cfg)) return JS_NewBool(false);
    nrfCaptureSetChannel((uint8_t)cfg.startChannel);
    return JS_NewBool(true);
}

JSValue native_nrf24Send(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.send(address_string_hex, data_uint8array)
    if (argc < 2) return JS_NewBool(false);
    if (!NRFradio.isChipConnected()) return JS_NewBool(false);

    const char *addrStr = NULL;
    JSCStringBuf addr_buf;
    if (JS_IsString(ctx, argv[0])) { addrStr = JS_ToCString(ctx, argv[0], &addr_buf); }
    if (!addrStr) return JS_NewBool(false);

    uint8_t address[5] = {0};
    String addrS = String(addrStr);
    addrS.replace(":", "");
    if (addrS.length() >= 10) {
        for (int i = 0; i < 5; i++) {
            address[i] = (uint8_t)strtol(addrS.substring(i * 2, i * 2 + 2).c_str(), NULL, 16);
        }
    }

    // Accept the same payload shapes as the rest of the bindings: a hex string,
    // an array of byte values, or a typed array / Buffer. Requiring only a
    // typed array meant `send(addr, [1,2,3])` silently returned false, and the
    // interpreter's Buffer.from() only takes a string, so there was no easy way
    // to build one from bytes at all.
    uint8_t owned[NRF_CAP_MAX_PAYLOAD];
    const uint8_t *data = NULL;
    size_t dataLen = 0;
    if (JS_IsTypedArray(ctx, argv[1])) {
        const char *rawData = JS_GetTypedArrayBuffer(ctx, &dataLen, argv[1]);
        if (!rawData || dataLen == 0) return JS_NewBool(false);
        data = (const uint8_t *)rawData;
    } else {
        int n = nrfJsExtractBytes(ctx, argv[1], owned, NRF_CAP_MAX_PAYLOAD);
        if (n <= 0) return JS_NewBool(false);
        data = owned;
        dataLen = (size_t)n;
    }
    if (dataLen > 32) dataLen = 32;  // the nRF24 payload is 32 bytes at most

    nrfCaptureRestoreNormal();
    NRFradio.stopListening();
    NRFradio.openWritingPipe(address);
    bool success = NRFradio.write(data, dataLen);
    return JS_NewBool(success);
}

JSValue native_nrf24Receive(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.receive(timeout_ms) -> hex string ("" on timeout)
    int timeout = 5000;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &timeout, argv[0]);
    if (timeout < 100) timeout = 100;
    if (timeout > 30000) timeout = 30000;

    uint8_t address[5] = {0x01, 0x23, 0x45, 0x67, 0x89};
    NRFradio.openReadingPipe(1, address);
    NRFradio.startListening();

    unsigned long start = millis();
    while (millis() - start < (unsigned long)timeout) {
        if (NRFradio.available()) {
            uint8_t buf[32] = {0};
            NRFradio.read(buf, sizeof(buf));
            NRFradio.stopListening();
            String hexStr = "";
            for (int i = 0; i < 32; i++) {
                if (buf[i] == 0 && i > 0) break;
                char hex[3];
                sprintf(hex, "%02X", buf[i]);
                hexStr += hex;
            }
            return JS_NewString(ctx, hexStr.c_str());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    NRFradio.stopListening();
    return JS_NewString(ctx, "");
}

JSValue native_nrf24SetChannel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.setChannel(channel)
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) {
        int channel = 0;
        JS_ToInt32(ctx, &channel, argv[0]);
        if (channel < 0) channel = 0;
        if (channel > 125) channel = 125;
        nrfCaptureSetChannel((uint8_t)channel);
    }
    return JS_UNDEFINED;
}

JSValue native_nrf24IsConnected(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return JS_NewBool(NRFradio.isChipConnected());
}

JSValue native_nrf24Init(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return native_nrf24Begin(ctx, this_val, argc, argv);
}

JSValue native_nrf24SetDataRate(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.setDataRate(rate) 0=1Mbps, 1=2Mbps, 2=250kbps
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) {
        int rate = 0;
        JS_ToInt32(ctx, &rate, argv[0]);
        if (rate < 0) rate = 0;
        if (rate > 2) rate = 2;
        nrfCaptureSetDataRate((uint8_t)rate);
    }
    return JS_NewBool(true);
}

JSValue native_nrf24SetPowerLevel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.setPowerLevel(level) 0=MIN .. 3=MAX
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) {
        int level = 3;
        JS_ToInt32(ctx, &level, argv[0]);
        if (level < 0) level = 0;
        if (level > 3) level = 3;
        nrfCaptureSetPower((uint8_t)level);
    }
    return JS_NewBool(true);
}

JSValue native_nrf24SetAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.setAddress("010A0B0C0D")  (3..5 bytes)
    if (argc > 0 && JS_IsString(ctx, argv[0])) {
        String s = js_tocstring_copy(ctx, argv[0]);
        uint8_t addr[5] = {0};
        int n = nrfJsParseHex(s.c_str(), addr, 5);
        if (n >= 3) nrfCaptureSetAddress(addr, (uint8_t)n);
    }
    return JS_NewBool(true);
}

JSValue native_nrf24StartListening(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    NRFradio.startListening();
    return JS_NewBool(true);
}

JSValue native_nrf24StopListening(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    NRFradio.stopListening();
    return JS_NewBool(true);
}

JSValue native_nrf24Available(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return JS_NewBool(NRFradio.available());
}

JSValue native_nrf24Read(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.read() -> hex string of the oldest received payload
    if (!NRFradio.available()) return JS_NewString(ctx, "");
    uint8_t buf[NRF_CAP_MAX_PAYLOAD];
    memset(buf, 0, sizeof(buf));
    uint8_t len = NRFradio.getDynamicPayloadSize();
    if (len == 0 || len > NRF_CAP_MAX_PAYLOAD) len = NRF_CAP_MAX_PAYLOAD;
    NRFradio.read(buf, len);
    char hex[2 * NRF_CAP_MAX_PAYLOAD + 1];
    for (int i = 0; i < len; i++) snprintf(hex + i * 2, 3, "%02X", buf[i]);
    hex[len * 2] = 0;
    return JS_NewString(ctx, hex);
}

// ---------------------------------------------------------------------------
// Advanced (pseudo-promiscuous) functions
// ---------------------------------------------------------------------------

JSValue native_nrf24SetPromiscuousMode(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.setPromiscuousMode(enable)
    bool enable = true;
    if (argc > 0) enable = JS_ToBool(ctx, argv[0]) == 1;
    if (!nrfJsEnsureReady(nrfCaptureGetChannel())) return JS_NewBool(false);
    return JS_NewBool(nrfCaptureSetPromiscuous(enable));
}

JSValue native_nrf24SetFrequencyHopMode(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.setFrequencyHopMode(enable)
    nrfJsHopMode = (argc > 0) ? (JS_ToBool(ctx, argv[0]) == 1) : true;
    return JS_NewBool(true);
}

JSValue native_nrf24ScanAllChannels(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.scanAllChannels() -> [{channel, freq, level}, ...] (level > 0 only)
    if (!nrfJsEnsureReady(nrfCaptureGetChannel())) return JS_NewArray(ctx, 0);

    int dwell = 2;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &dwell, argv[0]);

    static NrfChannelActivity act[126];
    int n = nrfScanChannelActivity(0, 125, dwell < 1 ? 1 : dwell, act, 126);

    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, 0);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    int written = 0;
    for (int i = 0; i < n; i++) {
        if (act[i].level == 0) continue;
        JSGCRef obj_ref;
        JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
        *obj = JS_NewObject(ctx);
        if (JS_IsException(*obj)) {
            JS_PopGCRef(ctx, &obj_ref);
            JS_PopGCRef(ctx, &arr_ref);
            return JS_ThrowOutOfMemory(ctx);
        }
        JS_SetPropertyStr(ctx, *obj, "channel", JS_NewInt32(ctx, act[i].channel));
        JS_SetPropertyStr(ctx, *obj, "freq", JS_NewInt32(ctx, act[i].freqMhz));
        JS_SetPropertyStr(ctx, *obj, "level", JS_NewInt32(ctx, act[i].level));
        JS_SetPropertyUint32(ctx, *arr, written++, *obj);
        JS_PopGCRef(ctx, &obj_ref);
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

JSValue native_nrf24CaptureRaw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.captureRaw(durationMs) -> [{channel,freq,len,timestamp,data}, ...]
    if (!nrfJsEnsureReady(nrfCaptureGetChannel())) return JS_NewArray(ctx, 0);
    int ms = 1000;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &ms, argv[0]);
    if (ms < 200) ms = 200;
    if (ms > 60000) ms = 60000;
    // 40 ms per channel: the 20 ms default only gives a burst a handful of
    // samples per channel and misses the code almost every time (measured).
    int n = nrfCaptureRun((uint32_t)ms, nrfJsHopMode, 40);
    static NrfCapturedPacket pkts[NRF_CAP_MAX_PACKETS];
    int got = 0;
    for (int i = 0; i < n && got < NRF_CAP_MAX_PACKETS; i++) {
        if (nrfCapturePacket(i, &pkts[got])) got++;
    }
    return nrfJsBuildCaptureArray(ctx, pkts, got);
}

JSValue native_nrf24CaptureMultiFreq(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.captureMultiFreq(durationMs) -> capture while hopping the range
    if (!nrfJsEnsureReady(nrfCaptureGetChannel())) return JS_NewArray(ctx, 0);
    int ms = 1500;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &ms, argv[0]);
    if (ms < 200) ms = 200;
    if (ms > 60000) ms = 60000;
    int n = nrfCaptureRun((uint32_t)ms, true, 40);
    static NrfCapturedPacket pkts[NRF_CAP_MAX_PACKETS];
    int got = 0;
    for (int i = 0; i < n && got < NRF_CAP_MAX_PACKETS; i++) {
        if (nrfCapturePacket(i, &pkts[got])) got++;
    }
    return nrfJsBuildCaptureArray(ctx, pkts, got);
}

JSValue native_nrf24ReplayRaw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.replayRaw(signalData, channel?, repeat?)
    if (argc < 1) return JS_NewBool(false);
    if (!nrfJsEnsureReady(nrfCaptureGetChannel())) return JS_NewBool(false);

    uint8_t bytes[NRF_CAP_MAX_PAYLOAD];
    int len = nrfJsExtractBytes(ctx, argv[0], bytes, NRF_CAP_MAX_PAYLOAD);
    if (len <= 0) return JS_NewBool(false);

    int channel = nrfCaptureGetChannel();
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &channel, argv[1]);
    int repeat = 3;
    if (argc > 2 && JS_IsNumber(ctx, argv[2])) JS_ToInt32(ctx, &repeat, argv[2]);
    if (repeat < 1) repeat = 1;
    if (repeat > 64) repeat = 64;

    bool ok = nrfReplayRaw(bytes, (uint8_t)len, (uint8_t)channel, (uint8_t)repeat, nrfCaptureGetDataRate());
    return JS_NewBool(ok);
}

JSValue native_nrf24SaveSignal(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.saveSignal(name, signalData)
    if (argc < 2) return JS_NewBool(false);
    String name = js_tocstring_copy(ctx, argv[0]);

    static NrfCapturedPacket pkts[NRF_CAP_MAX_PACKETS];
    int count = nrfJsSignalToPackets(ctx, argv[1], nrfCaptureGetChannel(), pkts, NRF_CAP_MAX_PACKETS);
    if (count <= 0) return JS_NewBool(false);
    return JS_NewBool(nrfSaveSignal(name, pkts, count, "nrf24.js"));
}

JSValue native_nrf24LoadSignal(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.loadSignal(name) -> [{channel,freq,len,timestamp,data}, ...]
    if (argc < 1) return JS_NewArray(ctx, 0);
    String name = js_tocstring_copy(ctx, argv[0]);
    static NrfCapturedPacket pkts[NRF_CAP_MAX_PACKETS];
    int n = nrfLoadSignal(name, pkts, NRF_CAP_MAX_PACKETS, nullptr);
    return nrfJsBuildCaptureArray(ctx, pkts, n);
}

JSValue native_nrf24AnalyzeSignal(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.analyzeSignal(signalData) -> analysis object
    uint8_t bytes[NRF_CAP_MAX_PAYLOAD];
    int len = (argc > 0) ? nrfJsExtractBytes(ctx, argv[0], bytes, NRF_CAP_MAX_PAYLOAD) : 0;
    NrfSignalAnalysis a = nrfAnalyzeSignal(bytes, (uint8_t)len);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "len", JS_NewInt32(ctx, a.len));
    JS_SetPropertyStr(ctx, obj, "uniqueBytes", JS_NewInt32(ctx, a.uniqueBytes));
    JS_SetPropertyStr(ctx, obj, "bitTransitions", JS_NewInt32(ctx, a.bitTransitions));
    JS_SetPropertyStr(ctx, obj, "minByte", JS_NewInt32(ctx, a.minByte));
    JS_SetPropertyStr(ctx, obj, "maxByte", JS_NewInt32(ctx, a.maxByte));
    JS_SetPropertyStr(ctx, obj, "repeats", JS_NewBool(a.repeats));
    JS_SetPropertyStr(ctx, obj, "repeatLength", JS_NewInt32(ctx, a.repeatLength));
    JS_SetPropertyStr(ctx, obj, "kind", JS_NewString(ctx, a.kind));
    return obj;
}

JSValue native_nrf24SetFrequencyRange(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.setFrequencyRange(startMhz, endMhz)
    int start = 2400;
    int end = 2525;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &start, argv[0]);
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &end, argv[1]);
    nrfCaptureSetFrequencyRange((uint16_t)start, (uint16_t)end);
    return JS_NewBool(true);
}

JSValue native_nrf24DetectMultiFreqSignals(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: nrf24.detectMultiFreqSignals(durationMs?) ->
    //   { channels:[...], freqs:[...], pairs:<n>, multiFreq:<bool> }
    if (!nrfJsEnsureReady(nrfCaptureGetChannel())) {
        return JS_ThrowOutOfMemory(ctx);
    }
    int ms = 3000;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &ms, argv[0]);
    if (ms < 200) ms = 200;
    if (ms > 60000) ms = 60000;

    nrfCaptureRun((uint32_t)ms, true, 40);

    uint8_t channels[8];
    int n = nrfFilterMultiFreqChannels(channels, 8);

    JSGCRef ch_ref;
    JSValue *chArr = JS_PushGCRef(ctx, &ch_ref);
    *chArr = JS_NewArray(ctx, n);
    JSGCRef fq_ref;
    JSValue *fqArr = JS_PushGCRef(ctx, &fq_ref);
    *fqArr = JS_NewArray(ctx, n);
    if (JS_IsException(*chArr) || JS_IsException(*fqArr)) {
        JS_PopGCRef(ctx, &fq_ref);
        JS_PopGCRef(ctx, &ch_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (int i = 0; i < n; i++) {
        JS_SetPropertyUint32(ctx, *chArr, i, JS_NewInt32(ctx, channels[i]));
        JS_SetPropertyUint32(ctx, *fqArr, i, JS_NewInt32(ctx, 2400 + channels[i]));
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "channels", *chArr);
    JS_SetPropertyStr(ctx, obj, "freqs", *fqArr);
    JS_SetPropertyStr(ctx, obj, "pairs", JS_NewInt32(ctx, n));
    JS_SetPropertyStr(ctx, obj, "multiFreq", JS_NewBool(n >= 2));
    JS_PopGCRef(ctx, &fq_ref);
    JS_PopGCRef(ctx, &ch_ref);
    return obj;
}

#endif
