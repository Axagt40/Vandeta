#include "sound_theme.h"
#include "config.h"
#include "globals.h" // the bruceConfig instance the settings live in
#include "modules/others/audio.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>

// One cue is a short run of notes. `level` is the note's own balance inside the
// cue (0-100), NOT a volume setting: loudness comes from bruceConfig.soundVolume
// alone, read at play time, so there is one volume in the firmware and no
// second gain hiding in here.
struct SoundNote {
    uint16_t freq; // Hz, 0 terminates the run
    uint16_t ms;   // length
    uint16_t gap;  // silence after it
    uint8_t level; // 0-100, this note against the others in the same cue
};

static const SoundNote VANDETA_CUES[CUE_COUNT][4] = {
    /* SELECT     */ {{1800, 30, 2, 80}, {2600, 45, 0, 80}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* BACK       */ {{1200, 45, 0, 70}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* NAV_TICK   */ {{2400, 12, 0, 65}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* ERROR      */ {{420, 90, 40, 85}, {300, 130, 0, 85}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* REC_START  */ {{1500, 40, 6, 75}, {2000, 45, 0, 75}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* REC_STOP   */ {{2000, 40, 6, 75}, {1500, 45, 0, 75}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* TRACKER    */ {{1200, 55, 12, 70}, {1600, 55, 12, 78}, {2100, 80, 0, 88}, {0, 0, 0, 0}},
    /* REMOTE_ID  */ {{2600, 65, 45, 88}, {2600, 65, 45, 88}, {3200, 110, 0, 88}, {0, 0, 0, 0}},
    /* SUSPECT    */ {{700, 110, 50, 82}, {520, 160, 0, 88}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    // Two crisp rising notes: short enough to land between two IR frames or two
    // tag polls, and clearly different from SELECT so a capture is audible
    // without looking at the screen.
    /* READ       */ {{2200, 26, 3, 80}, {3000, 38, 0, 90}, {0, 0, 0, 0}, {0, 0, 0, 0}},
};

// MOMENTUM is not a set of invented high clicks: it is the Flipper/Momentum
// notification voice. Those sequences are real notes from Momentum's
// notification_messages.c - success is C5 E5 G5 C6 at 50 ms each, error is C5
// twice at 100 ms, and semi-success is C4 E4 G4 C5. The old table used 2.2-3.6
// kHz clicks, which is why it sounded like a different device entirely.
//
// The four-note limit matches the longest sequence this cue player can hold;
// the two short five-note sequences are shortened at the tail without changing
// their opening interval, which is the part the ear identifies.
static const SoundNote MOMENTUM_CUES[CUE_COUNT][4] = {
    /* SELECT     */ {{523, 50, 0, 80}, {659, 50, 0, 80}, {784, 50, 0, 82}, {1047, 70, 0, 85}}, // C5 E5 G5 C6
    /* BACK       */ {{784, 45, 0, 75}, {523, 60, 0, 78}, {0, 0, 0, 0}, {0, 0, 0, 0}},          // G5 C5
    /* NAV_TICK   */ {{1047, 14, 0, 75}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},             // short C6
    /* ERROR      */ {{523, 100, 0, 85}, {523, 100, 0, 85}, {0, 0, 0, 0}, {0, 0, 0, 0}},        // C5 x2
    /* REC_START  */ {{523, 50, 0, 80}, {659, 50, 0, 80}, {784, 70, 0, 82}, {0, 0, 0, 0}},     // C5 E5 G5
    /* REC_STOP   */ {{784, 50, 0, 80}, {659, 50, 0, 80}, {523, 70, 0, 80}, {0, 0, 0, 0}},     // G5 E5 C5
    /* TRACKER    */ {{523, 50, 0, 78}, {659, 50, 0, 80}, {784, 50, 0, 82}, {1047, 80, 0, 85}}, // success
    /* REMOTE_ID  */ {{1047, 60, 30, 82}, {784, 60, 30, 82}, {1047, 90, 0, 85}, {0, 0, 0, 0}},  // C6 G5 C6
    /* SUSPECT    */ {{523, 100, 0, 82}, {523, 100, 0, 85}, {0, 0, 0, 0}, {0, 0, 0, 0}},        // error
    /* READ       */ {{523, 50, 0, 80}, {659, 60, 0, 85}, {0, 0, 0, 0}, {0, 0, 0, 0}},          // C5 E5
};

// CLASSIC is the blip this firmware made before the theme existed: a single
// 5000 Hz note per event, so turning it on is exactly the old behaviour.
static const SoundNote CLASSIC_CUES[CUE_COUNT][4] = {
    /* SELECT     */ {{5000, 50, 0, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* BACK       */ {{5000, 50, 0, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* NAV_TICK   */ {{5000, 50, 0, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* ERROR      */ {{5000, 50, 0, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* REC_START  */ {{5000, 50, 0, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* REC_STOP   */ {{5000, 50, 0, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* TRACKER    */ {{5000, 50, 0, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* REMOTE_ID  */ {{5000, 50, 0, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* SUSPECT    */ {{5000, 50, 0, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    /* READ       */ {{5000, 50, 0, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
};

static const UBaseType_t QUEUE_DEPTH = 4; // a queue of one would drop every tick
// 6 KB: the cue task sits inside ESP8266Audio's generator loop, and the measured
// high-water mark on the board was already 2.6 KB of 4 KB - too close to the
// edge to leave alone.
static const uint32_t TASK_STACK = 8192;
static const UBaseType_t TASK_PRIORITY = 1;
static const BaseType_t TASK_CORE = 0; // keep off the core that draws the UI

static QueueHandle_t cueQueue = nullptr;
static portMUX_TYPE suppressMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t suppressDepth = 0;
// When the outermost push happened. Every push/pop pair in the tree is balanced,
// but one early return inside a transmit path is enough to leave the whole
// firmware silent until the next reboot - and silence is impossible to diagnose
// from the outside. A suppression older than this is treated as stale and
// dropped: no transmission lasts 15 s, so a genuine one is never cut short.
static uint32_t suppressSince = 0;
static const uint32_t SUPPRESS_STALE_MS = 15000;

// Linear amplitude looks like a broken control at the bottom of the range: the
// first few steps of a 10-100 % list are 2 % apart in amplitude and nobody can
// hear that. This curve spreads the steps out and the floor keeps 10 % audible
// on a small speaker in a noisy room. It reads soundVolume, it never stores it.
static float volumeCurve(int volumePercent) {
    if (volumePercent <= 0) return 0.0f;
    if (volumePercent > 100) volumePercent = 100;
    float linear = volumePercent / 100.0f;
    float shaped = powf(linear, 1.6f);
    if (shaped < 0.05f) shaped = 0.05f; // floor, not silence
    return shaped;
}

bool soundIsSuppressed() {
    bool stale = false;
    portENTER_CRITICAL(&suppressMux);
    if (suppressDepth > 0 && (millis() - suppressSince) > SUPPRESS_STALE_MS) {
        suppressDepth = 0; // a push whose pop never ran: do not stay muted
        stale = true;
    }
    uint8_t depth = suppressDepth;
    portEXIT_CRITICAL(&suppressMux);
    if (stale) Serial.println("sound: stale suppression cleared");
    return depth > 0;
}

void soundSuppressPush() {
    portENTER_CRITICAL(&suppressMux);
    if (suppressDepth == 0) suppressSince = millis();
    if (suppressDepth < 255) suppressDepth++;
    portEXIT_CRITICAL(&suppressMux);
}

void soundSuppressPop() {
    portENTER_CRITICAL(&suppressMux);
    if (suppressDepth > 0) suppressDepth--;
    portEXIT_CRITICAL(&suppressMux);
}

const char *soundThemeName(int theme) {
    switch (theme) {
        case SOUND_THEME_VANDETA: return "VANDETA";
        case SOUND_THEME_CLASSIC: return "CLASSIC";
        case SOUND_THEME_MOMENTUM: return "MOMENTUM";
        case SOUND_THEME_OFF: return "OFF";
        default: return "VANDETA";
    }
}

// Everything that must be true for a cue to be heard, re-checked before every
// note: the user can mute or change the theme while a cue is running.
static bool cueAllowed() {
    if (!bruceConfig.soundEnabled) return false;
    if (bruceConfig.soundTheme == SOUND_THEME_OFF) return false;
    if (soundIsSuppressed()) return false;
#if defined(HAS_NS4168_SPKR)
    if (isAudioPlaying()) return false; // the player owns the codec
#endif
    return true;
}

static void soundTask(void *) {
    for (;;) {
        uint8_t cue = 0;
        if (xQueueReceive(cueQueue, &cue, portMAX_DELAY) != pdTRUE) continue;
        if (cue >= CUE_COUNT) continue;
        if (!cueAllowed()) continue;

        const SoundNote *notes;
        switch (bruceConfig.soundTheme) {
            case SOUND_THEME_CLASSIC: notes = CLASSIC_CUES[cue]; break;
            case SOUND_THEME_MOMENTUM: notes = MOMENTUM_CUES[cue]; break;
            default: notes = VANDETA_CUES[cue]; break;
        }

        for (int i = 0; i < 4 && notes[i].freq != 0; i++) {
            if (!cueAllowed()) break; // muted or a radio started mid-cue
            float level = volumeCurve(bruceConfig.soundVolume) * (notes[i].level / 100.0f);
            if (level <= 0.0f) break;
#if defined(HAS_NS4168_SPKR)
            // The speaker is the NS4168 over I2S, there is no buzzer pin here.
            // This blocks for the note's length, which is why it runs on this
            // task and never in the caller.
            playToneAtLevel(notes[i].freq, notes[i].ms, level);
#elif defined(BUZZ_PIN)
            // tone(BUZZ_PIN, ...) has no gain, so the level is ignored on these
            // boards. Do not fake one by shortening the note.
            (void)level;
            tone(BUZZ_PIN, notes[i].freq, notes[i].ms);
            vTaskDelay(pdMS_TO_TICKS(notes[i].ms));
#else
            (void)level;
            break;
#endif
            if (notes[i].gap) vTaskDelay(pdMS_TO_TICKS(notes[i].gap));
        }
    }
}

void soundPlay(SoundCue cue) {
    if (cue >= CUE_COUNT) return;
    if (!bruceConfig.soundEnabled || bruceConfig.soundTheme == SOUND_THEME_OFF) return;
    if (soundIsSuppressed()) return;

    if (cueQueue == nullptr) {
        cueQueue = xQueueCreate(QUEUE_DEPTH, sizeof(uint8_t));
        if (cueQueue == nullptr) return; // no cue is better than a crash
        if (xTaskCreate(soundTask, "soundCue", TASK_STACK, nullptr, TASK_PRIORITY, nullptr) != pdPASS) {
            vQueueDelete(cueQueue);
            cueQueue = nullptr;
            return;
        }
    }

    // Never wait for room: feedback that arrives late is worse than no feedback.
    uint8_t value = cue;
    xQueueSend(cueQueue, &value, 0);
}
