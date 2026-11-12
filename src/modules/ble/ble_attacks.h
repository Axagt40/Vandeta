#ifndef __BLE_ATTACKS_H__
#define __BLE_ATTACKS_H__

#if !defined(LITE_VERSION)

// Flat BLE attack page, opened from BLE > BLE Atks. It mirrors the shape of
// WiFi > Wifi Atks: one page, one row per action, no nested "more" menu.
//
// The three new attacks complement BLE Spam and BLE Suite instead of repeating
// them: spam sends platform popups, the suite works one selected target, and
// these act on every connectable advertiser in range for a fixed window.
void BleAttacksMenu();

// Repeated connect/disconnect against every connectable advertiser found by a
// short scan. This is a connection-slot / radio stress test, not a stealth
// attack: the target sees a burst of connection attempts.
void BleConnectionStorm();

// WiFi Beacon SPAM for BLE: rotating fake advertising names from funny,
// rickroll, random or a custom base name. It fills nearby scanners with
// devices that do not exist; it does not target a vendor popup.
void BleFlood();

// Connect to each nearby connectable device and request security, then drop the
// link, repeatedly. On devices that accept pairing this produces a stream of
// pairing prompts; devices that reject it simply log failed attempts.
void BlePairingStorm();

// Pair once with a peripheral in pairing mode, write the bond material (local
// address, peer address, LTK/IRK/CSRK, EDIV/RAND) to /BruceBLE/auth_*.key on
// the SD card, and skip devices whose key file already exists. The T-Embed does
// not reconnect; the file is for a Linux host.
void BleAuthCapture();

#endif

#endif
