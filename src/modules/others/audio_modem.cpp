#include "audio_modem.h"

#include "audio.h"
#include <globals.h>

namespace AudioModem {

static uint8_t crc8(const uint8_t *d, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

bool send(const String &text, int baud, bool ultrasonic, String &msg) {
    if (baud < 300) baud = 300;
    if (baud > 2400) baud = 2400;
    if (text.length() == 0 || text.length() > 200) {
        msg = "text must be 1..200 bytes";
        return false;
    }
    const int bitMs = max(1, 1000 / baud);
    const unsigned mark = ultrasonic ? 19000 : 1200;
    const unsigned space = ultrasonic ? 21000 : 2200;

    uint8_t frame[256];
    size_t n = 0;
    frame[n++] = 0xA5;
    frame[n++] = 0x5A;
    frame[n++] = (uint8_t)text.length();
    for (size_t i = 0; i < text.length(); i++) frame[n++] = (uint8_t)text[i];
    frame[n] = crc8(frame, n);
    n++;

    // Preamble so the far end can settle its AGC, then the frame.
    for (int i = 0; i < 8; i++) playTone(mark, bitMs);
    for (size_t i = 0; i < n; i++) {
        for (int b = 0; b < 8; b++) playTone((frame[i] >> b) & 1 ? mark : space, bitMs);
    }
    msg = "sent " + String((unsigned)n) + " bytes at " + String(baud) + " baud (" +
          String(ultrasonic ? "ultrasonic" : "audible") + ")";
    return true;
}

bool receive(int seconds, String &msg) {
    // The board does have an SPM1423 microphone; it is used by the Microphone
    // app. What is missing is an FSK demodulator here, so say that instead of
    // denying hardware the board has.
    (void)seconds;
    msg = "audio modem RX is not implemented on this build; the board's SPM1423 "
          "microphone is used by Others > Microphone";
    return false;
}

} // namespace AudioModem
