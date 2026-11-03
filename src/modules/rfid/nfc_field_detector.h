#ifndef __NFC_FIELD_DETECTOR_H__
#define __NFC_FIELD_DETECTOR_H__

/**
 * @file nfc_field_detector.h
 * @brief RFID > Field detector: find a live 13.56 MHz NFC field nearby.
 */

/**
 * @brief Live proximity detector for an NFC reader / powered phone.
 *
 * Opens its own PN532 interface (the T-Embed CC1101 has one on I2C), draws a
 * signal meter and returns when BACK is pressed. Receive-only: it never writes
 * to a card and never emulates anything the user asked it to emulate - the
 * target-mode arm is used purely as a field probe.
 *
 * The board must have a PN532-family module configured; anything else says so
 * and returns.
 */
void nfcFieldDetector();

#endif // __NFC_FIELD_DETECTOR_H__
