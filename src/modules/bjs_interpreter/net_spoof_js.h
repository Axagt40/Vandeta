#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#ifndef __NET_SPOOF_JS_H__
#define __NET_SPOOF_JS_H__

#include "helpers_js.h"

extern "C" {
// arpSpoof.*
JSValue native_arpSpoofStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_arpSpoofStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_arpSpoofGetStatus(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_arpSpoofGetTargets(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// dnsSpoof.*
JSValue native_dnsSpoofStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_dnsSpoofStartAll(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_dnsSpoofStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_dnsSpoofGetStatus(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_dnsSpoofClearRules(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// httpInterceptor.*
JSValue native_httpInterceptorStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_httpInterceptorStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_httpInterceptorAddRedirect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_httpInterceptorAddInjection(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_httpInterceptorClearRules(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_httpInterceptorGetInterceptedData(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

/// Restores ARP mappings and tears the interception down when a script exits.
void netspoof_js_cleanup();
}

#endif
#endif
