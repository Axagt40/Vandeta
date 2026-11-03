/**
 * @file nfc_field_detector.cpp
 * @brief RFID > Field detector.
 *
 * WHAT THIS IS
 * ------------
 * A proximity probe for a live 13.56 MHz field: put the board against a door
 * reader, a payment terminal or a phone and the page says FIELD. It is the same
 * job Flipper's NFC field detector does, built out of the only radio this board
 * has - the embedded PN532 - so it is deliberately approximate.
 *
 * HOW IT DETECTS A FIELD (and why it is a proximity detector, not a meter)
 * -----------------------------------------------------------------------
 * The PN532 cannot measure a coil voltage on this board: it is on I2C and its
 * analogue front end is not exposed. Two things are used instead, in order:
 *
 *  1. A tag that answers `InListPassiveTarget`. A card only powers up inside a
 *     reader's field, so an answer proves a field is present (and that the
 *     nearby reader is actually polling).
 *  2. Target mode (`TgInitAsTarget`, ISO14443A PICC). With the chip armed as a
 *     card, a nearby reader that polls activates it. That is the case a passive
 *     tag poll cannot see - a reader with no card in its field.
 *
 * Both are binary events, so the "strength" on screen is EVENTS PER SECOND over
 * a rolling 2 s window, not decibels. It rises as the antenna gets closer and as
 * the reader polls faster; it does not compare two readers. That is why the file
 * says proximity detector: the number is a rate, not field strength.
 *
 * Arm-and-release is intentional. `TgInitAsTarget` blocks until a reader
 * activates the chip, so each attempt is bounded by a short IRQ timeout and is
 * released again afterwards. That keeps BACK responsive and the screen alive at
 * ~10 Hz without a task - the same reason `ble_detector.cpp` avoids one.
 */

#include "nfc_field_detector.h"

#include "PN532.h" // also exposes Adafruit_PN532's internals (private->public hack)
#include "core/display.h"
#include "core/utils.h"
#include <globals.h>

namespace {

/// How long one target-mode arm waits for a reader before it is released.
/// Short enough to keep the UI at ~10 Hz, long enough to catch a reader that
/// polls a few times a second.
constexpr uint16_t FIELD_ARM_TIMEOUT_MS = 120;

/// How long one passive tag poll waits for an answer.
constexpr uint16_t FIELD_TAG_TIMEOUT_MS = 50;

/// Rolling window for the events-per-second meter.
constexpr uint32_t FIELD_WINDOW_MS = 2000;

/// Events in the window are capped here: 12 blocks on screen, so nothing above
/// that changes the picture.
constexpr int FIELD_MAX_EVENTS = 12;

/// IRQ-aware wait, mirroring PN532.cpp's helper: the T-Embed's PN532 is wired to
/// an IRQ pin, and polling it in 1 ms steps is much cheaper than the Adafruit
/// library's timeout path (which is what makes a short arm timeout practical).
bool waitReadyIrq(Adafruit_PN532 &nfc, uint16_t timeoutMs) {
    if (nfc._irq >= 0) {
        uint32_t start = millis();
        while (digitalRead(nfc._irq) != LOW) {
            if ((millis() - start) > timeoutMs) return false;
            delay(1);
            yield();
        }
        return true;
    }
    return nfc.waitready(timeoutMs);
}

bool sendCmdIrq(Adafruit_PN532 &nfc, uint8_t *cmd, uint8_t cmdLen, uint16_t timeoutMs) {
    nfc.writecommand(cmd, cmdLen);
    delay(1);
    if (!waitReadyIrq(nfc, timeoutMs)) return false;
    if (!nfc.readack()) return false;
    delay(1);
    return waitReadyIrq(nfc, timeoutMs);
}

/// fISO14443-4_PICC must be set before TgInitAsTarget will accept mode 0x04
/// (UM0701-02 7.2.9). Same bytes as PN532.cpp's emulate() path.
bool setParametersPicc(Adafruit_PN532 &nfc) {
    uint8_t cmd[] = {PN532_COMMAND_SETPARAMETERS, 0x20};
    if (!sendCmdIrq(nfc, cmd, sizeof(cmd), 1000)) return false;
    uint8_t frame[8] = {0};
    nfc.readdata(frame, sizeof(frame));
    return true;
}

/**
 * @brief Arm the chip as an ISO14443A card and see whether a reader activates it.
 *
 * The frame is the proven one from PN532.cpp (same SENS_RES / SEL_RES / lengths);
 * only the timeout differs, because here a timeout is the answer we are looking
 * for rather than a failure. Returns true when a reader actually activated the
 * target. Whatever happened, the chip is released before returning so the next
 * pass (and any menu opened afterwards) starts from a known state.
 */
bool probeFieldAsTarget(Adafruit_PN532 &nfc) {
    uint8_t target[] = {
        PN532_COMMAND_TGINITASTARGET,
        0x04, // MODE: PICC only (requires fISO14443-4_PICC, set once above)
        0x04, 0x00, // SENS_RES: 4-byte UID, standard frame
        0x11, 0x22, 0x33, // NFCID1T
        0x20, // SEL_RES: ISO14443-4 PICC
        0x01, 0xFE, 0xA2, 0xA8, 0x00, 0x00, 0x00, 0x00, // NFCID2t (unused here)
        0x01, 0xFE, 0xA2, 0xA8, 0x00, 0x00, 0x00, 0x00, // PAD (unused here)
        0xFF, 0xFF, // system code
        0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // NFCID3t
        0x00, // general bytes length: no NFC-DEP capability advertised
        0x0E, // historical bytes length
        0x42, 0x52, 0x55, 0x43, 0x45, 0x20, 0x46, 0x49,
        0x52, 0x4D, 0x57, 0x41, 0x52, 0x45, // "BRUCE FIRMWARE"
    };

    bool activated = false;
    if (sendCmdIrq(nfc, target, sizeof(target), FIELD_ARM_TIMEOUT_MS)) {
        // Adafruit reads 8 bytes here; reading more can stall the I2C link.
        uint8_t frame[8] = {0};
        nfc.readdata(frame, sizeof(frame));
        if (frame[6] == 0x15) {
            activated = true;
        } else if (frame[6] == (PN532_COMMAND_TGINITASTARGET + 1)) {
            // 0x00 / 0x08 = activated as 106 kbps PICC. Status 0x04 is the
            // bench-known "armed then self-released" case on this unit, not a
            // field, so it is not counted.
            uint8_t status = frame[7];
            activated = (status == 0x00 || status == 0x08 || status == 0x12 || status == 0x15 || status == 0x22);
        }
    }

    // Always leave target mode: a chip left armed answers the next menu's
    // reader commands with "already in target mode".
    nfc.inRelease();
    return activated;
}

/// Poll for a tag that a nearby reader has powered up.
bool probeTag(Adafruit_PN532 &nfc, uint8_t *uid, uint8_t &uidLen) {
    uidLen = 0;
    if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, FIELD_TAG_TIMEOUT_MS)) return false;
    return uidLen > 0;
}

String uidToHex(const uint8_t *uid, uint8_t len) {
    String s;
    for (uint8_t i = 0; i < len; i++) {
        char b[3];
        snprintf(b, sizeof(b), "%02X", uid[i]);
        s += b;
    }
    return s;
}

/// Draw the ten-block meter; `level` is 0..FIELD_MAX_EVENTS.
void drawFieldMeter(int level) {
    const int blocks = 10;
    const int bw = 22;
    const int gap = 6;
    const int total = blocks * bw + (blocks - 1) * gap;
    const int x0 = (tftWidth - total) / 2;
    const int y = 104;
    for (int i = 0; i < blocks; i++) {
        int x = x0 + i * (bw + gap);
        bool on = i < level;
        // Two colours so the bar reads as a scale rather than a solid block.
        uint16_t color = on ? (i < blocks * 2 / 3 ? bruceConfig.priColor : bruceConfig.secColor)
                            : TFT_DARKGREY;
        tft.drawRect(x, y, bw, 14, color);
        if (on) tft.fillRect(x + 2, y + 2, bw - 4, 10, color);
    }
}

} // namespace

void nfcFieldDetector() {
    // Same construction pattern as EMVReader / TagOMatic: only the PN532 family
    // can do this, and the T-Embed's module is the I2C one.
    PN532 *rfid = nullptr;
    switch (bruceConfigPins.rfidModule) {
        case PN532_I2C_MODULE: rfid = new PN532(PN532::CONNECTION_TYPE::I2C); break;
#ifdef M5STICK
        case PN532_I2C_SPI_MODULE: rfid = new PN532(PN532::CONNECTION_TYPE::I2C_SPI); break;
#endif
        case PN532_SPI_MODULE: rfid = new PN532(PN532::CONNECTION_TYPE::SPI); break;
        default: break;
    }
    if (rfid == nullptr) {
        displayError("No PN532 module", true);
        return;
    }
    if (!rfid->begin()) {
        delete rfid;
        displayError("PN532 not found", true);
        return;
    }
    Adafruit_PN532 &nfc = rfid->nfc;

    // Needed once for target mode; if it fails the probe still runs, it just
    // cannot report a reader that has no card in its field.
    setParametersPicc(nfc);

    drawMainBorderWithTitle("NFC Field");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(FP);
    tft.drawCentreString("Move near a reader", tftWidth / 2, 50, 1);

    uint32_t events[FIELD_MAX_EVENTS] = {0};
    int eventCount = 0;
    bool field = false;
    bool lastField = false;
    String lastUid = "";
    String lastState = "";
    int lastLevel = -1;
    uint32_t lastDraw = 0;

    while (true) {
        if (check(EscPress)) break;

        uint8_t uid[7];
        uint8_t uidLen = 0;
        bool tag = probeTag(nfc, uid, uidLen);
        bool activated = false;
        if (!tag) activated = probeFieldAsTarget(nfc);

        // A tag answering or a reader activating the target both mean a field.
        if (tag || activated) {
            if (eventCount < FIELD_MAX_EVENTS) {
                events[eventCount++] = millis();
            } else {
                // Full: drop the oldest so the window keeps sliding.
                for (int i = 1; i < FIELD_MAX_EVENTS; i++) events[i - 1] = events[i];
                events[FIELD_MAX_EVENTS - 1] = millis();
            }
        }
        if (tag) lastUid = uidToHex(uid, uidLen);

        // Age the events out of the window.
        uint32_t now = millis();
        int kept = 0;
        for (int i = 0; i < eventCount; i++) {
            if ((now - events[i]) <= FIELD_WINDOW_MS) events[kept++] = events[i];
        }
        eventCount = kept;
        field = eventCount > 0;

        if (now - lastDraw < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        lastDraw = now;

        String state = field ? String("FIELD") : String("quiet");
        if (state != lastState || field != lastField) {
            lastState = state;
            lastField = field;
            tft.fillRect(6, 66, tftWidth - 12, 32, bruceConfig.bgColor);
            tft.setTextSize(FG);
            tft.setTextColor(field ? bruceConfig.priColor : bruceConfig.secColor, bruceConfig.bgColor);
            tft.drawCentreString(state, tftWidth / 2, 68, SMOOTH_FONT);
        }

        if (eventCount != lastLevel) {
            lastLevel = eventCount;
            drawFieldMeter(eventCount > 10 ? 10 : eventCount);
            tft.setTextSize(FP);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.fillRect(6, 86, tftWidth - 12, 14, bruceConfig.bgColor);
            tft.drawCentreString(
                String(eventCount) + " hit" + (eventCount == 1 ? "" : "s") + "/2s",
                tftWidth / 2,
                87,
                1
            );
        }

        tft.setTextSize(FP);
        tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
        tft.fillRect(6, 124, tftWidth - 12, 14, bruceConfig.bgColor);
        if (lastUid.length()) tft.drawCentreString("tag " + lastUid, tftWidth / 2, 124, 1);
        else tft.drawCentreString("no tag answering", tftWidth / 2, 124, 1);

        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.drawCentreString("BACK exit", tftWidth / 2, tftHeight - 18, 1);

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Leave the chip in reader mode, not target mode: the next RFID page calls
    // begin() itself, but a chip left armed would answer its first poll with a
    // target-mode error instead of a scan.
    nfc.inRelease();
    delete rfid;
    tft.fillScreen(bruceConfig.bgColor);
}
