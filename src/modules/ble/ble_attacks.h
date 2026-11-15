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

// WiFi Beacon SPAM for BLE: rotating fake advertising names from funny,
// rickroll, random or a custom base name. It fills nearby scanners with
// devices that do not exist; it does not target a vendor popup.
void BleFlood();

// The Pairing Storm page behind that row has Start Attack plus a mode toggle:
// connect + disconnect, or connect + request security + disconnect. Pairing
// mode reuses auth already on the board (NimBLE bonds, or /BruceBLE/auth_*.key
// files from Auth Capture) so known devices use their stored LTK.

// Pair once with a peripheral in pairing mode, write the bond material (local
// address, peer address, LTK/IRK/CSRK, EDIV/RAND) to /BruceBLE/auth_*.key on
// the SD card, and skip devices whose key file already exists. The T-Embed does
// not reconnect; the file is for a Linux host.
void BleAuthCapture();

#endif

#endif
