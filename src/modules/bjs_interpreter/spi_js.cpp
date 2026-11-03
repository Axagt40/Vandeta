#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "spi_js.h"

#include "core/bus_HAL.h"

//=============================================================================
// Shared byte-argument helpers (also used by uart_js.cpp)
//=============================================================================

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1; // separators, whitespace, anything else
}

int jsValueToBytes(JSContext *ctx, JSValue val, uint8_t *out, int maxLen) {
    if (!out || maxLen <= 0) return 0;

    if (JS_IsString(ctx, val)) {
        // A hex string is the compact form for binary payloads: pairs of hex
        // digits, every other character is a separator.
        size_t len = 0;
        JSCStringBuf sb;
        const char *s = JS_ToCStringLen(ctx, &len, val, &sb);
        if (!s) return 0;
        int hi = -1, n = 0;
        for (size_t i = 0; i < len && n < maxLen; i++) {
            int v = hexNibble(s[i]);
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else {
                out[n++] = (uint8_t)((hi << 4) | v);
                hi = -1;
            }
        }
        return n; // a dangling nibble is not a byte
    }

    if (JS_IsTypedArray(ctx, val)) {
        size_t len = 0;
        const char *b = JS_GetTypedArrayBuffer(ctx, &len, val);
        if (!b) return 0;
        int n = (int)len > maxLen ? maxLen : (int)len;
        memcpy(out, b, (size_t)n);
        return n;
    }

    if (JS_IsObject(ctx, val)) {
        int len = 0;
        // JS_GetProperty*() can allocate (which may compact the heap), so no
        // JSValue is held in a local across the loop - each one is read inline.
        if (JS_ToInt32(ctx, &len, JS_GetPropertyStr(ctx, val, "length")) != 0 || len <= 0) return 0;
        int n = 0;
        for (int i = 0; i < len && n < maxLen; i++) {
            int v = 0;
            if (JS_ToInt32(ctx, &v, JS_GetPropertyUint32(ctx, val, i)) != 0) v = 0;
            out[n++] = (uint8_t)(v & 0xFF);
        }
        return n;
    }

    return 0;
}

JSValue jsBytesToArray(JSContext *ctx, const uint8_t *buf, size_t len) {
    // Pinned: JS_SetPropertyUint32() grows the backing store, which can move
    // the array, so a plain local would not survive the loop.
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, (int)len);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (size_t i = 0; i < len; i++) {
        JS_SetPropertyUint32(ctx, *arr, (uint32_t)i, JS_NewInt32(ctx, buf[i]));
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

//=============================================================================
// SPI
//=============================================================================

// One acquired bus at a time, reused by write()/transfer() until spi.end().
// acquireSPIBus() never hands out a bus that would fight the display or the SD
// card: those pins return their own instance, anything else the auxiliary bus.
static SPIClass *jsSpiBus = nullptr;
static int8_t jsSpiCsPin = -1;
static uint32_t jsSpiHz = 1000000;

static void jsSpiCsLow() {
    if (jsSpiCsPin >= 0) digitalWrite((uint8_t)jsSpiCsPin, LOW);
}

static void jsSpiCsHigh() {
    if (jsSpiCsPin >= 0) digitalWrite((uint8_t)jsSpiCsPin, HIGH);
}

// Starts a transaction; the transaction is ended by the caller right after the
// bytes, so a script that draws between calls cannot leave the shared bus busy.
static bool jsSpiPrepare(JSContext *ctx) {
    if (!jsSpiBus) {
        JS_ThrowInternalError(ctx, "spi not initialized: call spi.begin(sck,miso,mosi,cs,hz) first");
        return false;
    }
    jsSpiBus->beginTransaction(SPISettings(jsSpiHz, MSBFIRST, SPI_MODE0));
    return true;
}

JSValue native_spi_begin(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 3 || !JS_IsNumber(ctx, argv[0]) || !JS_IsNumber(ctx, argv[1]) || !JS_IsNumber(ctx, argv[2])) {
        return JS_ThrowTypeError(ctx, "spi.begin(sck:int, miso:int, mosi:int, cs?:int, hz?:int) is required");
    }
    int sck = -1, miso = -1, mosi = -1, cs = -1;
    uint32_t hz = 1000000;
    JS_ToInt32(ctx, &sck, argv[0]);
    JS_ToInt32(ctx, &miso, argv[1]);
    JS_ToInt32(ctx, &mosi, argv[2]);
    if (argc > 3 && JS_IsNumber(ctx, argv[3])) JS_ToInt32(ctx, &cs, argv[3]);
    if (argc > 4 && JS_IsNumber(ctx, argv[4])) JS_ToUint32(ctx, &hz, argv[4]);

    if (sck < 0 || miso < 0 || mosi < 0) return JS_ThrowRangeError(ctx, "spi.begin: pins must be >= 0");
    if (cs < -1) return JS_ThrowRangeError(ctx, "spi.begin: cs must be a pin or omitted");
    if (hz < 1000 || hz > 80000000) return JS_ThrowRangeError(ctx, "spi.begin: hz must be 1k..80MHz");

    jsSpiBus = acquireSPIBus((gpio_num_t)sck, (gpio_num_t)miso, (gpio_num_t)mosi);
    if (!jsSpiBus) {
        return JS_ThrowInternalError(ctx, "spi.begin: no free SPI bus for those pins (display, SD card "
                                          "and the auxiliary bus are all taken)");
    }
    // AUX_SPI is ours to (re)start; the display's and the SD card's instances are
    // already configured for the whole program lifetime and must not be begun
    // again here.
    if (jsSpiBus == &AUX_SPI) jsSpiBus->begin((int8_t)sck, (int8_t)miso, (int8_t)mosi, (int8_t)cs);

    jsSpiCsPin = (int8_t)cs;
    if (cs >= 0) {
        pinMode((uint8_t)cs, OUTPUT);
        digitalWrite((uint8_t)cs, HIGH);
    }
    jsSpiHz = hz;

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "\xffspi_ready", JS_NewBool(true));
    return JS_NewBool(true);
}

JSValue native_spi_write(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "spi.write(bytes) requires a hex string, array or Uint8Array");
    uint8_t buf[256];
    int len = jsValueToBytes(ctx, argv[0], buf, (int)sizeof(buf));
    if (len <= 0) return JS_ThrowTypeError(ctx, "spi.write: argument must be a hex string, array or Uint8Array");
    if (!jsSpiPrepare(ctx)) return JS_EXCEPTION;

    jsSpiCsLow();
    jsSpiBus->writeBytes(buf, (size_t)len);
    jsSpiCsHigh();
    jsSpiBus->endTransaction();
    return JS_NewInt32(ctx, len);
}

JSValue native_spi_transfer(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "spi.transfer(bytes) requires a hex string, array or Uint8Array");
    uint8_t out[256];
    int len = jsValueToBytes(ctx, argv[0], out, (int)sizeof(out));
    if (len <= 0) return JS_ThrowTypeError(ctx, "spi.transfer: argument must be a hex string, array or Uint8Array");
    if (!jsSpiPrepare(ctx)) return JS_EXCEPTION;

    uint8_t in[256];
    jsSpiCsLow();
    for (int i = 0; i < len; i++) in[i] = jsSpiBus->transfer(out[i]);
    jsSpiCsHigh();
    jsSpiBus->endTransaction();
    return jsBytesToArray(ctx, in, (size_t)len);
}

JSValue native_spi_end(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (jsSpiBus) {
        // Only release the auxiliary bus; the display's and the SD card's
        // instances keep running for the whole program lifetime.
        if (jsSpiBus == &AUX_SPI) jsSpiBus->end();
        jsSpiBus = nullptr;
    }
    jsSpiCsPin = -1;
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "\xffspi_ready", JS_NewBool(false));
    return JS_NewBool(true);
}

#endif
