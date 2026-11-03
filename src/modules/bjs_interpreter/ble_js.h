#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#ifndef __BLE_JS_H__
#define __BLE_JS_H__

#include "helpers_js.h"

extern "C" {
JSValue native_bleScan(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleAdvertise(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleStopAdvertise(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// BLE spam bindings (headless, time-bounded)
JSValue native_bleSpam(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleSpamModes(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// GATT client: init / setAddress / connect / disconnect / getService /
// getCharacteristic / read / write / notify
JSValue native_bleInit(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleSetAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleConnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleDisconnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleIsConnected(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleServices(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleGetService(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleCharacteristics(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleGetCharacteristic(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleRead(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleWrite(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleNotify(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_blePollEvents(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// Capture / replay of raw advertisements (shared with the BLE menu)
JSValue native_bleCaptureStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleCaptureStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleCaptureRunning(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleCaptureCount(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleCaptureClear(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleCaptureList(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleCaptureReplay(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleCaptureSave(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleCaptureLoad(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleCaptureAnalyze(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleRemoteTypes(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleSetRemoteType(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_bleScanDetailed(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// Stops any background capture and drops the GATT client when the interpreter
// exits. Declared inside extern "C" like the other module cleanups: the
// generated stdlib header pulls this file in from inside an extern "C" block,
// so the include guard means this is the only linkage interpreter.cpp sees.
void ble_js_cleanup();
}

#endif
#endif
