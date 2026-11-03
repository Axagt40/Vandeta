#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#ifndef __WIFI_JS_H__
#define __WIFI_JS_H__

#include "helpers_js.h"

extern "C" {
JSValue native_wifiConnected(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiConnectDialog(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiConnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiScan(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiDisconnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_httpFetch(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiMACAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_ipAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// WiFi attack bindings (headless, time-bounded)
JSValue native_wifiDeauth(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiDeauthAll(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiBeaconSpam(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiSniffHandshake(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// Raw 802.11 injection / capture bindings
JSValue native_wifiInjectPacket(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiSendRaw80211(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiCaptureStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiCaptureStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiGetCapturedPackets(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiSetPromiscuous(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// Captive portal (evil twin) binding. Generic on purpose: the script supplies
// the page or redirect, so it backs rickrolls and custom portals alike.
JSValue native_wifiPortal(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// Application layer injection: forged DNS replies and HTTP payloads inside
// hand-built 802.11 data frames, plus the decoder that reads the values
// (ports, seq/ack, DNS id) back out of a captured frame.
JSValue native_wifiInjectDns(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiInjectHttp(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiInjectHttpRedirect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiInjectHtml(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_wifiPacketInfo(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// Releases capture buffers / promiscuous mode when the interpreter exits.
void wifi_js_cleanup();
}

#endif
#endif
