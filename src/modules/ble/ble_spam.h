#pragma once

#include <Arduino.h>
#include <NimBLEBeacon.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
void spamMenu();

void ibeacon(
    const char *DeviceName = "Bruce iBeacon", const char *BEACON_UUID = "8ec76ea3-6668-48da-9866-75be8bc86f4d",
    int ManufacturerId = 0x4C00
);

// Headless (UI-free) BLE spam drivers for the bjs interpreter. They reuse the
// same advert builders, MAC rotation and timing as the on-screen spam UI, but
// run on a deadline instead of waiting for a key press.

/** @brief Number of selectable spam attack types. */
int headlessBleSpamModeCount();

/** @brief Label of the spam attack type at `index`, or "" when out of range. */
const char *headlessBleSpamModeName(int index);

/**
 * @brief Advertise BLE spam packets for a bounded amount of time.
 *
 * @param attackIndex index into the attack type list (see headlessBleSpamModeName)
 * @param durationMs how long to transmit before returning (capped at 5 minutes)
 * @return number of advertisement packets sent
 */
int headlessBleSpam(uint8_t attackIndex, uint32_t durationMs);
