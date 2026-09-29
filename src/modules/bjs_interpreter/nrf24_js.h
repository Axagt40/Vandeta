#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#ifndef __NRF24_JS_H__
#define __NRF24_JS_H__

#include "helpers_js.h"

extern "C" {
// Basic module control.
JSValue native_nrf24Begin(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24Init(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24Send(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24Receive(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24SetChannel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24SetDataRate(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24SetPowerLevel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24SetAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24StartListening(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24StopListening(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24Available(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24Read(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24IsConnected(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

// Advanced: pseudo-promiscuous mode, raw capture/replay, analysis.
JSValue native_nrf24SetPromiscuousMode(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24SetFrequencyHopMode(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24ScanAllChannels(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24CaptureRaw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24CaptureMultiFreq(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24ReplayRaw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24SaveSignal(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24LoadSignal(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24AnalyzeSignal(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24SetFrequencyRange(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_nrf24DetectMultiFreqSignals(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
}

#endif
#endif
