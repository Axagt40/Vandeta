#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "uart_js.h"

#include "spi_js.h" // jsValueToBytes / jsBytesToArray (shared byte helpers)

//=============================================================================
// UART on Serial1
//
// Serial (USB-CDC) is the console: a script must not steal it. Serial1 is the
// spare hardware UART, so a script picks its own RX/TX pins and baud rate and
// talks to an external module. uart.end() gives the pins back.
//=============================================================================

static bool jsUartReady = false;

static bool jsUartRequireReady(JSContext *ctx) {
    if (jsUartReady) return true;
    JS_ThrowInternalError(ctx, "uart not initialized: call uart.begin(baud,rx,tx) first");
    return false;
}

JSValue native_uart_begin(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 3 || !JS_IsNumber(ctx, argv[0]) || !JS_IsNumber(ctx, argv[1]) || !JS_IsNumber(ctx, argv[2])) {
        return JS_ThrowTypeError(ctx, "uart.begin(baud:int, rx:int, tx:int) is required");
    }
    int baud = 0, rx = -1, tx = -1;
    JS_ToInt32(ctx, &baud, argv[0]);
    JS_ToInt32(ctx, &rx, argv[1]);
    JS_ToInt32(ctx, &tx, argv[2]);

    if (baud < 300 || baud > 3000000) return JS_ThrowRangeError(ctx, "uart.begin: baud must be 300..3000000");
    if (rx < 0 || tx < 0) return JS_ThrowRangeError(ctx, "uart.begin: rx and tx pins must be >= 0");
    if (rx == tx) return JS_ThrowRangeError(ctx, "uart.begin: rx and tx must be different pins");

    Serial1.begin((unsigned long)baud, SERIAL_8N1, (int8_t)rx, (int8_t)tx);
    jsUartReady = true;

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "\xffuart_ready", JS_NewBool(true));
    return JS_NewBool(true);
}

JSValue native_uart_write(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "uart.write(value) requires a string, array or Uint8Array");
    if (!jsUartRequireReady(ctx)) return JS_EXCEPTION;

    // A string is text here, not hex: a UART is usually driven with AT commands
    // and newline-terminated lines. Binary payloads come in as an array or a
    // Uint8Array and are written verbatim.
    if (JS_IsString(ctx, argv[0])) {
        size_t len = 0;
        JSCStringBuf sb;
        const char *s = JS_ToCStringLen(ctx, &len, argv[0], &sb);
        if (!s) return JS_ThrowTypeError(ctx, "uart.write: invalid string argument");
        size_t written = Serial1.write((const uint8_t *)s, len);
        return JS_NewInt32(ctx, (int)written);
    }

    uint8_t buf[256];
    int len = jsValueToBytes(ctx, argv[0], buf, (int)sizeof(buf));
    if (len <= 0) return JS_ThrowTypeError(ctx, "uart.write: argument must be a string, array or Uint8Array");
    size_t written = Serial1.write(buf, (size_t)len);
    return JS_NewInt32(ctx, (int)written);
}

JSValue native_uart_available(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (!jsUartRequireReady(ctx)) return JS_EXCEPTION;
    return JS_NewInt32(ctx, Serial1.available());
}

JSValue native_uart_read(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (!jsUartRequireReady(ctx)) return JS_EXCEPTION;

    int max = 64;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &max, argv[0]);
    if (max < 1) max = 1;
    if (max > 256) max = 256; // one buffer, no big allocations from a script

    uint8_t buf[256];
    int n = 0;
    // Never blocks: it drains what the driver already has. A script that wants
    // to wait polls uart.available() in its own loop (or uses delay()).
    while (n < max && Serial1.available() > 0) {
        int c = Serial1.read();
        if (c < 0) break;
        buf[n++] = (uint8_t)c;
    }
    return jsBytesToArray(ctx, buf, (size_t)n);
}

JSValue native_uart_flush(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (!jsUartRequireReady(ctx)) return JS_EXCEPTION;
    Serial1.flush();
    return JS_NewBool(true);
}

JSValue native_uart_end(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (jsUartReady) {
        Serial1.flush();
        Serial1.end();
    }
    jsUartReady = false;
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "\xffuart_ready", JS_NewBool(false));
    return JS_NewBool(true);
}

#endif
