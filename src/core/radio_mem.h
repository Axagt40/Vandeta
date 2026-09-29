#ifndef __RADIO_MEM_H__
#define __RADIO_MEM_H__
/*
    Internal-DRAM contiguous-block guard for radio bring-up on no-PSRAM boards.

    On boards without PSRAM (e.g. m5stack-cardputer) the Wi-Fi and BLE stacks —
    plus the SD/SPI DMA buffers — all compete for the SAME internal DRAM. What
    actually gates them is not total free heap but the LARGEST CONTIGUOUS
    DMA-capable block. When that block is exhausted, esp_wifi_init /
    esp_bt_controller_init fail deep inside the SDK and leave the driver in a
    half-initialized state, which then crashes (LoadProhibited / abort()) on the
    next operation.

    These helpers let callers refuse to start a radio *before* touching it when
    there isn't enough contiguous DMA memory, turning an unavoidable crash into a
    clean, user-visible error.
*/
#include <esp_heap_caps.h>
#include <stddef.h>
#include "core/wifi/wifi_common.h"
#include <WiFi.h>

// Largest contiguous DMA-capable internal block, in bytes. This is the number
// that gates Wi-Fi/BLE controller init.
static inline size_t radioLargestDmaBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_DMA); }

// Minimum contiguous DMA block required BEFORE bringing Wi-Fi up (checked at the
// scan/menu entry, i.e. before esp_wifi is touched at all).
constexpr size_t RADIO_WIFI_MIN_DMA_BLOCK = 15 * 1024;

// Minimum contiguous DMA block required before bringing the BLE stack up.
// NimBLE still has single DMA allocations, so this stays a necessary condition
// - but on its own it is NOT sufficient (see below).
constexpr size_t RADIO_BLE_MIN_DMA_BLOCK = 15 * 1024;

// TOTAL free internal DRAM NimBLE needs before its bring-up can succeed.
//
// Measured on a T-Embed CC1101 with `RAM_LOG()` around `bleInit()` in
// ble_common.cpp: the scan menu reported heap free 73819 before the call and
// 16459 after it, i.e. controller + host + HCI buffers consume ~56 KB. Note that
// the largest *contiguous* block was only 31732 in that same successful run, so
// the old "15 KB contiguous DMA" test was testing the wrong quantity: it said
// "yes" in states (a running script leaves ~54 KB free internal) where the
// bring-up can never fit, and the user only found out from a generic failure.
//
// The number below is deliberately below the measured 56 KB: a borderline
// attempt that still fails is now cheap, because `bleInit()` (ble_common.cpp)
// unwinds a failed controller bring-up and returns its RAM instead of leaking
// it. Guessing high here would refuse BLE where it would have worked.
constexpr size_t RADIO_BLE_MIN_INTERNAL_FREE = 60 * 1024;

static inline size_t radioFreeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

static inline bool radioHasMemForWifi() {
    // return true; // uncomment to disable it
    return radioLargestDmaBlock() >= RADIO_WIFI_MIN_DMA_BLOCK;
}

static inline bool radioHasMemForBle() {
    // return true; // uncomment to disable it

    // BOTH conditions matter: total free internal DRAM (NimBLE's many buffers)
    // and one contiguous DMA block for its largest single allocation.
    if (radioFreeInternal() >= RADIO_BLE_MIN_INTERNAL_FREE &&
        radioLargestDmaBlock() >= RADIO_BLE_MIN_DMA_BLOCK) {
        return true;
    }
    
    // Not enough memory - try to free WiFi
    Serial.printf(
        "[RAM] Low memory for BLE (free internal %u, DMA block %u), attempting to free WiFi...\n",
        (unsigned)radioFreeInternal(),
        (unsigned)radioLargestDmaBlock()
    );
    
    // Disconnect WiFi if active
    if (WiFi.getMode() != WIFI_MODE_NULL || wifiConnected) {
        wifiDisconnect();
        delay(200);
        #ifdef WIFI_DEINIT_ON_DISCONNECT
        WiFi.mode(WIFI_OFF);
        #endif
        delay(300);
    }
    
    // Recheck after freeing WiFi
    if (radioFreeInternal() >= RADIO_BLE_MIN_INTERNAL_FREE &&
        radioLargestDmaBlock() >= RADIO_BLE_MIN_DMA_BLOCK) {
        Serial.printf(
            "[RAM] WiFi freed, free internal %u, DMA block %u\n",
            (unsigned)radioFreeInternal(),
            (unsigned)radioLargestDmaBlock()
        );
        return true;
    }
    
    // Still not enough - return false, caller shows error
    Serial.printf(
        "[RAM] Still not enough for BLE: free internal %u (need %u), DMA block %u (need %u)\n",
        (unsigned)radioFreeInternal(),
        (unsigned)RADIO_BLE_MIN_INTERNAL_FREE,
        (unsigned)radioLargestDmaBlock(),
        (unsigned)RADIO_BLE_MIN_DMA_BLOCK
    );
    return false;
}

#endif // __RADIO_MEM_H__
