#ifndef __CAST_JS_H__
#define __CAST_JS_H__
#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "helpers_js.h"

// C linkage: mqjs_stdlib.c is compiled as C and takes these as function
// pointers in the JSPropDef tables, exactly like every other *_js.h here.
extern "C" {

// "Post this image to the TV" support: the two discovery protocols a TV answers
// without an app (DLNA/UPnP over SSDP, Chromecast over mDNS), the HTTP server
// that hands the image to the TV, the AP a TV can be put on, and the CASTV2
// sender itself. Registered on the existing `wifi` object in mqjs_stdlib.c.

// wifi.ssdp(type?, timeoutMs?) -> ["ip|friendlyName|http://ip:port/ctrl", ...]
JSValue native_wifiSsdp(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// wifi.mdnsCast(timeoutMs?) -> ["ip|8009|Friendly name", ...]
JSValue native_wifiMdnsCast(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// wifi.serveFile(path, port?) -> "http://ip:port/name" ("" on failure)
JSValue native_wifiServeFile(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiServeStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// wifi.apStart(ssid, password?, channel?) -> "192.168.4.1" ("" on failure)
// An empty password makes an open AP: a TV joins it by tapping the SSID, with
// no on-screen keyboard, which is the only part of a TV's Wi-Fi setup that IR
// cannot type.
JSValue native_wifiApStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiApStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// wifi.castImage(ip, url, contentType?) -> "ok" or the reason it failed
JSValue native_wifiCastImage(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

} // extern "C"

#endif
#endif
