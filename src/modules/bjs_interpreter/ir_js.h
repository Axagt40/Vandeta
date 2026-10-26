#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#ifndef __IR_JS_H__
#define __IR_JS_H__

#include "helpers_js.h"

extern "C" {
JSValue native_irTransmitFile(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irTransmit(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irRead(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irReadRaw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irTransmitRaw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irUniversalList(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irUniversalButtons(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irUniversalCode(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irUniversalSend(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irUniversalFamily(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_irTransmitButton(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
}

#endif
#endif
