#ifndef __BLE_COMMON_H__
#define __BLE_COMMON_H__

#include <NimBLEAdvertisedDevice.h>
#include <NimBLEBeacon.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>

#include "core/display.h"
#include <globals.h>

//=============================================================================
// BLE Constants
//=============================================================================

#define SCANTIME 5
#define SCANTYPE ACTIVE
#define SCAN_INT 100
#define SCAN_WINDOW 99

// Maximum number of BLE devices to display to prevent memory issues
#define MAX_DISPLAY_DEVICES 100

// Memory protection: Reduce scan time in low-memory situations
#define SCAN_TIME_REDUCED 3

extern BLEScan *pBLEScan;
extern int scanTime;

void ble_test();
#if 0
#ifdef BOARD_HAS_PSRAM
constexpr bool FORCE_RADIO_TEARDOWN_ON_SWITCH = false;
#else
constexpr bool FORCE_RADIO_TEARDOWN_ON_SWITCH = true;
#endif
#else
constexpr bool FORCE_RADIO_TEARDOWN_ON_SWITCH = false;
#endif

bool ble_scan_setup();
void ble_scan();
void stopBLEStack();

/// Bring the NimBLE stack up and report whether it really came up.
///
/// `NimBLEDevice::init()` returns false when a late step fails. On this board
/// the usual failure is `esp_nimble_hci_init()` with ESP_ERR_NO_MEM, because the
/// HCI buffers have to live in DMA-capable internal RAM and a running script has
/// little of it. What matters is what the failure leaves behind: the BT
/// controller is already initialised AND enabled, and since NimBLEDevice only
/// sets its `m_initialized` flag after a fully successful init, its own
/// `deinit()` is a no-op in that state. Measured on a T-Embed CC1101, one failed
/// attempt took internal free RAM from 47,007 to 12,675 bytes and it never came
/// back - after which every later Wi-Fi/BLE bring-up failed with RAM errors too.
/// This wrapper unwinds the controller so the memory is returned.
///
/// Safe to call when the stack is already up (returns true immediately).
bool bleInit(const char *name = "");

bool bleNotifyRetry(NimBLECharacteristic *chr, const uint8_t *value, size_t length, uint8_t retries = 8);
bool bleNotifyRetry(NimBLECharacteristic *chr, uint8_t retries = 8);

void disPlayBLESend();

#endif
