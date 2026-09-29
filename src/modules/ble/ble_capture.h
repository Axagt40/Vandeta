#ifndef __BLE_CAPTURE_H
#define __BLE_CAPTURE_H

// BLE raw advertisement capture, replay and analysis.
//
// Modern BLE remotes (TV/Android TV/Apple TV, RGB and LED controllers, ...) do
// not use a connection: they broadcast advertisement packets that carry a
// vendor payload, often with a rotating address. This module records those raw
// payloads and replays them, which is what the BLE > Capture & Replay menu and
// the ble.captureStart/captureReplay bindings use.
//
// Replay advertises the recorded payload bytes verbatim and, when address
// spoofing is enabled, sets the advertiser address to the captured one
// (NimBLEDevice::setOwnAddr). Spoofing is best effort: the controller may
// refuse a non-canonical address, and outside a capture window the radio is
// shared with the rest of the firmware.

#include <Arduino.h>

#define BLE_CAP_MAX_PACKETS 48
#define BLE_CAP_MAX_PAYLOAD 31

struct BleCapturedPacket {
    uint32_t timestamp;
    uint8_t addr[6];
    uint8_t addrType; // 0 public, 1 random
    int8_t rssi;
    uint8_t advType;
    bool connectable;
    uint8_t len;
    uint8_t data[BLE_CAP_MAX_PAYLOAD];
};

/// A selectable "remote type", used both as a capture filter and as a hint when
/// analysing what was captured.
struct BleRemoteTypeInfo {
    const char *name;
    uint16_t companyId; // 0xFFFF = match anything
};

int bleCaptureRemoteTypeCount();
const char *bleCaptureRemoteTypeName(int idx);
void bleCaptureSetRemoteType(int idx);
int bleCaptureRemoteType();

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

/// Start a passive scan for `durationMs` and record every advertisement.
/// Returns the number of packets captured.
int bleCaptureRun(uint32_t durationMs);

/// Start a background capture that ends after `maxMs` or when
/// bleCaptureStop() is called. Returns false when one is already running.
/// NimBLEScan::start() blocks for its whole duration, so the scan runs on its
/// own task and bleCaptureStop() cancels it from the caller's task.
bool bleCaptureStart(uint32_t maxMs);

/// Stop a background capture and wait for the scan task to end.
/// Returns the number of packets captured so far.
int bleCaptureStop();

/// True while a background capture started with bleCaptureStart() is running.
bool bleCaptureRunning();

int bleCaptureCount();
bool bleCapturePacket(int index, BleCapturedPacket *out);
void bleCaptureClear();

/// Indices whose payload/name/address contains `needle` (case-insensitive).
/// Empty needle returns every index. Also applies the selected remote type.
int bleCaptureFilter(const String &needle, int *outIdx, int maxOut);

/// Channel/address summary: number of distinct advertiser addresses seen.
int bleCaptureAddressCount();

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

/// Advertise the selected packets (cyclically) for `durationMs`.
bool bleCaptureReplay(const int *idx, int count, uint32_t durationMs, bool spoofAddress);

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

bool bleCaptureSave(const String &path, const char *label);
int bleCaptureLoad(const String &path, String *labelOut);
const char *bleCaptureFolder();

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

/// One-line description of a captured packet (adv type, name, company id).
String bleCaptureDescribe(const BleCapturedPacket &p);

/// Detailed scan: list nearby devices with address, rssi, type and payload
/// length. Writes up to `maxOut` entries and returns the count.
struct BleScanEntry {
    char address[18];
    char name[24];
    int8_t rssi;
    uint8_t advType;
    uint8_t addrType; // what ble.connect() wants as its third argument
    uint8_t payloadLen;
    bool connectable;
};
int bleScanDetailed(uint32_t durationMs, BleScanEntry *out, int maxOut);

/// BLE > Capture & Replay menu.
void bleCaptureMenu();

#endif
