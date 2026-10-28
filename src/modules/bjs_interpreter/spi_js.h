#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#ifndef __SPI_JS_H__
#define __SPI_JS_H__

#include "helpers_js.h"

/* Shared byte-argument helpers.
 *
 * They live with the SPI binding because it was the first user, and uart_js.cpp
 * includes this header for the same two functions. Both follow the mquickjs
 * rule from FREEBUFF/DISCOVERIES.md: the heap is compacting and the C stack is
 * never scanned, so anything that allocates between building a JS value and
 * returning it has to be pinned with JS_PushGCRef(). */

/* Parse a JS payload into `out`: a hex string ("AA55" / "AA 55"), an array of
 * numbers ([0xAA, 0x55]) or a Uint8Array. Returns the byte count, 0 on a bad
 * argument. Never throws - callers report the failure they care about. */
int jsValueToBytes(JSContext *ctx, JSValue val, uint8_t *out, int maxLen);

/* Build a JS array of byte numbers. Returns JS_EXCEPTION (already raised) when
 * the array cannot be allocated. */
JSValue jsBytesToArray(JSContext *ctx, const uint8_t *buf, size_t len);

extern "C" {
JSValue native_spi_begin(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_spi_write(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_spi_transfer(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_spi_end(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
}

#endif
#endif
