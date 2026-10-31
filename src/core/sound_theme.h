#ifndef __SOUND_THEME_H__
#define __SOUND_THEME_H__

#include <Arduino.h>

// The cue set. Every page that wants feedback asks for one of these by name
// instead of inventing its own beep, so the whole UI sounds like one device.
enum SoundCue : uint8_t {
    CUE_SELECT = 0, // a row was chosen
    CUE_BACK,       // left a page / cancelled
    CUE_NAV_TICK,   // cursor moved one row
    CUE_ERROR,      // an action failed
    CUE_REC_START,  // capture or transmit started
    CUE_REC_STOP,   // ... and stopped
    CUE_TRACKER,    // a BLE tracker's signal is rising
    CUE_REMOTE_ID,  // a Remote ID beacon was decoded
    CUE_SUSPECT,    // rogue AP / camera / skimmer pattern
    CUE_READ,       // a code was received (IR) or a tag/card was read (RFID)
    CUE_COUNT
};

enum SoundTheme : uint8_t {
    SOUND_THEME_VANDETA = 0, // the note set below
    SOUND_THEME_CLASSIC = 1, // exactly the old single 5 kHz blip
    SOUND_THEME_OFF = 2,     // silent cues; the audio player still works
    SOUND_THEME_COUNT
};

// name for the settings row
const char *soundThemeName(int theme);

// schedule a cue and return immediately. Never allocates in the caller, never
// blocks, never plays if sound is muted, the theme is OFF, a radio needs
// silence, or the audio player owns the speaker.
void soundPlay(SoundCue cue);

// Hold cues off while the IR LED or the RF radio is transmitting: a beep in
// the middle of a capture becomes part of the capture. Push/pop, not a bool,
// because paths can nest.
void soundSuppressPush();
void soundSuppressPop();
bool soundIsSuppressed();

#endif
