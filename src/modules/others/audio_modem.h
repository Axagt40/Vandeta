#ifndef __AUDIO_MODEM_H__
#define __AUDIO_MODEM_H__

#include <Arduino.h>

// Task 4: an FSK data modem over the speaker (and the microphone where the
// board has one). No radios, no pairing: two devices that can hear each other
// can exchange a framed, checksummed payload.
//
// Frame: [0xA5][0x5A][len u8][payload][crc8]. Each byte is sent LSB first as
// mark/space tones; the ultrasonic mode moves both tones above 18 kHz.
namespace AudioModem {

bool send(const String &text, int baud, bool ultrasonic, String &msg);

// Receive is not implemented on this build: the T-Embed CC1101 does have an
// SPM1423 microphone (used by the Microphone app), but the modem has no FSK
// demodulator, so receive() reports that instead of denying the hardware.
bool receive(int seconds, String &msg);

} // namespace AudioModem

#endif
