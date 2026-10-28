#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "i2c_js.h"

#include "core/bus_HAL.h"
#include "helpers_js.h"

// Bus acquired by i2c.begin() and reused by every other native_i2c_* call until the script
// re-runs i2c.begin() with different pins (or a fresh interpreter run acquires it again).
static TwoWire *jsI2CBus = nullptr;

// Returns false with the exception already raised. Callers must return
// JS_EXCEPTION then - continuing after a throw here used to dereference a null
// bus (the throw does not unwind C code by itself).
static bool i2c_require_ready(JSContext *ctx) {
    if (!jsI2CBus) {
        JS_ThrowInternalError(ctx, "i2c not initialized: call i2c.begin(sda,scl,hz) first");
        return false;
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue v = JS_GetPropertyStr(ctx, global, "\xffi2c_ready");
    bool ready = JS_IsBool(v) ? JS_ToBool(ctx, v) : false;
    if (!ready) {
        JS_ThrowInternalError(ctx, "i2c not initialized: call i2c.begin(sda,scl,hz) first");
        return false;
    }
    return true;
}

// A read returns an array of byte numbers: mquickjs has no JS_NewArrayBufferCopy(),
// so an ArrayBuffer cannot be handed back from a binding. Pinned, because
// JS_SetPropertyUint32() grows the backing store and the heap may compact.
static JSValue i2c_bytes_to_array(JSContext *ctx, const uint8_t *buf, size_t len) {
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

JSValue native_i2c_begin(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 3 || !JS_IsNumber(ctx, argv[0]) || !JS_IsNumber(ctx, argv[1]) || !JS_IsNumber(ctx, argv[2])) {
        return JS_ThrowTypeError(ctx, "i2c.begin(sda:int, scl:int, hz:int) is required");
    }
    int sda = 0, scl = 0;
    uint32_t hz = 0;
    JS_ToInt32(ctx, &sda, argv[0]);
    JS_ToInt32(ctx, &scl, argv[1]);
    JS_ToUint32(ctx, &hz, argv[2]);
    if (sda < 0 || scl < 0) return JS_ThrowRangeError(ctx, "i2c.begin: pins must be >= 0");
    if (hz < 1000 || hz > 1000000) return JS_ThrowRangeError(ctx, "i2c.begin: hz must be 1k..1MHz");

    jsI2CBus = acquireI2CBus((int8_t)sda, (int8_t)scl);
    jsI2CBus->setClock(hz);
    jsI2CBus->setTimeOut(50);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "\xffi2c_ready", JS_NewBool(true));
    return JS_NewBool(true);
}

JSValue native_i2c_scan(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (!i2c_require_ready(ctx)) return JS_EXCEPTION;
    // Pinned: JS_SetPropertyUint32() can allocate (the backing store grows), so a
    // plain local would not survive the loop.
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, 127);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    uint32_t idx = 0;
    for (uint8_t a = 1; a < 127; a++) {
        jsI2CBus->beginTransmission(a);
        if (jsI2CBus->endTransmission(true) == 0) {
            JS_SetPropertyUint32(ctx, *arr, idx++, JS_NewInt32(ctx, a));
        }
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

JSValue native_i2c_write(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (!i2c_require_ready(ctx)) return JS_EXCEPTION;
    int addr = 0;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &addr, argv[0]);
    std::vector<uint8_t> tmp;
    const uint8_t *buf = NULL;
    size_t len = 0;
    if (argc < 2) return JS_ThrowTypeError(ctx, "i2c.write: arg1 must be Uint8Array or string");

    if (JS_IsString(ctx, argv[1])) {
        JSCStringBuf sb;
        const char *s = JS_ToCStringLen(ctx, &len, argv[1], &sb);
        if (!s) return JS_ThrowTypeError(ctx, "i2c.write: invalid string argument");
        tmp.assign((const uint8_t *)s, (const uint8_t *)s + len);
        buf = tmp.data();
    } else if (JS_IsTypedArray(ctx, argv[1])) {
        const char *b = JS_GetTypedArrayBuffer(ctx, &len, argv[1]);
        if (!b) return JS_ThrowTypeError(ctx, "i2c.write: invalid typed array argument");
        buf = (const uint8_t *)b;
    } else {
        return JS_ThrowTypeError(ctx, "i2c.write: arg1 must be Uint8Array or string");
    }
    bool sendStop = true;
    if (argc > 2) sendStop = JS_ToBool(ctx, argv[2]);

    jsI2CBus->beginTransmission((uint8_t)addr);
    jsI2CBus->write(buf, (size_t)len);
    uint8_t err = jsI2CBus->endTransmission(sendStop);
    return JS_NewInt32(ctx, err);
}

JSValue native_i2c_read(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (!i2c_require_ready(ctx)) return JS_EXCEPTION;
    int addr = 0;
    int len = 0;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &addr, argv[0]);
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &len, argv[1]);
    if (addr < 1 || addr > 127) return JS_ThrowRangeError(ctx, "i2c.read: addr must be 1..127");
    if (len < 1 || len > 255) return JS_ThrowRangeError(ctx, "i2c.read: len must be 1..255");

    size_t got = jsI2CBus->requestFrom((uint8_t)addr, (uint8_t)len, (uint8_t)true);
    uint8_t buf[255];
    size_t n = 0;
    while (n < got && jsI2CBus->available()) buf[n++] = (uint8_t)jsI2CBus->read();
    return i2c_bytes_to_array(ctx, buf, n);
}

JSValue native_i2c_write_read(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (!i2c_require_ready(ctx)) return JS_EXCEPTION;
    int addr = 0;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &addr, argv[0]);
    std::vector<uint8_t> tmp;
    const uint8_t *wbuf = NULL;
    size_t wlen = 0;
    if (argc < 3) return JS_ThrowTypeError(ctx, "i2c.writeRead: arg1 must be Uint8Array or string");

    if (JS_IsString(ctx, argv[1])) {
        JSCStringBuf sb;
        const char *s = JS_ToCStringLen(ctx, &wlen, argv[1], &sb);
        if (!s) return JS_ThrowTypeError(ctx, "i2c.writeRead: invalid string argument");
        tmp.assign((const uint8_t *)s, (const uint8_t *)s + wlen);
        wbuf = tmp.data();
    } else if (JS_IsTypedArray(ctx, argv[1])) {
        const char *b = JS_GetTypedArrayBuffer(ctx, &wlen, argv[1]);
        if (!b) return JS_ThrowTypeError(ctx, "i2c.writeRead: invalid typed array argument");
        wbuf = (const uint8_t *)b;
    } else {
        return JS_ThrowTypeError(ctx, "i2c.writeRead: arg1 must be Uint8Array or string");
    }
    int rlen = 0;
    if (JS_IsNumber(ctx, argv[2])) JS_ToInt32(ctx, &rlen, argv[2]);
    int wait = 0;
    if (argc > 3 && JS_IsNumber(ctx, argv[3])) JS_ToInt32(ctx, &wait, argv[3]);

    if (addr < 1 || addr > 127) return JS_ThrowRangeError(ctx, "i2c.writeRead: addr must be 1..127");
    if (rlen < 1 || rlen > 255) return JS_ThrowRangeError(ctx, "i2c.writeRead: read length must be 1..255");
    if (wait < 0) wait = 0;

    jsI2CBus->beginTransmission((uint8_t)addr);
    jsI2CBus->write(wbuf, (size_t)wlen);
    uint8_t err = jsI2CBus->endTransmission(false);
    if (err != 0) { return JS_NewInt32(ctx, -err); }
    if (wait > 0) delay(wait);
    size_t got = jsI2CBus->requestFrom((uint8_t)addr, (uint8_t)rlen, (uint8_t)true);
    uint8_t buf[255];
    size_t n = 0;
    while (n < got && jsI2CBus->available()) buf[n++] = (uint8_t)jsI2CBus->read();
    return i2c_bytes_to_array(ctx, buf, n);
}

#endif
