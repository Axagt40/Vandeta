#include "sound_theme.h"
#include "config.h"
#include "globals.h" // the bruceConfig instance the settings live in
#include "modules/others/audio.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>

// One cue is a short run of notes, played by playCueSequence() as a single
// waveform so the lengths and gaps written here are the ones actually heard.
// `level` is the note's own balance inside the cue (0-100), NOT a volume
// setting: loudness comes from bruceConfig.soundVolume alone, read at play
// time, so there is one volume in the firmware and no second gain hiding here.
static const AudioCueNote VANDETA_CUES[CUE_COUNT][4] = {
    /* SELECT     */ {{1800, 30, 0, 80}, {2600, 45, 0, 80}, {0, 0, 0, 0}, {0, 0, 0, 0}},
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

// MOMENTUM is the Flipper/Momentum notification voice, not a set of invented
// high clicks. These are the notes from Momentum's notification_messages.c:
// success = C5 E5 G5 C6 at 50 ms each, error = C5 twice at 100 ms separated by
// a 100 ms pause, semi-success = C4 E4 G4 C5. The cue player sends one waveform
// per cue now, so those 50 ms notes really are 50 ms: the old 120 ms floor
// stretched the 200 ms success chirp to about 640 ms, which is why it sounded
// like a different device.
static const AudioCueNote MOMENTUM_CUES[CUE_COUNT][4] = {
    /* SELECT     */ {{523, 50, 0, 85}, {659, 50, 0, 85}, {784, 50, 0, 88}, {1047, 50, 0, 90}}, // success
    /* BACK       */ {{262, 50, 0, 78}, {330, 50, 0, 78}, {392, 50, 0, 82}, {523, 50, 0, 85}},  // semi-success
    /* NAV_TICK   */ {{1047, 12, 0, 70}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},             // click
    /* ERROR      */ {{523, 100, 100, 85}, {523, 100, 0, 90}, {0, 0, 0, 0}, {0, 0, 0, 0}},      // error
    /* REC_START  */ {{523, 50, 0, 85}, {659, 50, 0, 85}, {784, 50, 0, 88}, {1047, 50, 0, 90}}, // success
    /* REC_STOP   */ {{1047, 50, 0, 85}, {784, 50, 0, 85}, {523, 70, 0, 85}, {0, 0, 0, 0}},     // G5 E5 C5
    /* TRACKER    */ {{523, 50, 0, 85}, {659, 50, 0, 85}, {784, 50, 0, 88}, {1047, 50, 0, 90}}, // success
    /* REMOTE_ID  */ {{1047, 60, 30, 82}, {784, 60, 30, 82}, {1047, 90, 0, 88}, {0, 0, 0, 0}},  // C6 G5 C6
    /* SUSPECT    */ {{523, 100, 100, 85}, {523, 100, 0, 90}, {0, 0, 0, 0}, {0, 0, 0, 0}},      // error
    /* READ       */ {{523, 50, 0, 85}, {659, 50, 0, 88}, {0, 0, 0, 0}, {0, 0, 0, 0}},          // C5 E5
};

// CLASSIC is the blip this firmware made before the theme existed: a single
// 5000 Hz note per event, so turning it on is exactly the old behaviour.
static const AudioCueNote CLASSIC_CUES[CUE_COUNT][4] = {
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

        const AudioCueNote *notes;
        switch (bruceConfig.soundTheme) {
            case SOUND_THEME_CLASSIC: notes = CLASSIC_CUES[cue]; break;
            case SOUND_THEME_MOMENTUM: notes = MOMENTUM_CUES[cue]; break;
            default: notes = VANDETA_CUES[cue]; break;
        }

        int count = 0;
        while (count < 4 && notes[count].freq != 0) count++;
        if (count == 0) continue;

#if defined(HAS_NS4168_SPKR)
        // One waveform for the whole cue: one I2S start and one drain, so a
        // 50 ms note is 50 ms and the next note starts on time. Blocks for the
        // cue's length, which is why it runs on this task and not in a caller.
        playCueSequence(notes, count, volumeCurve(bruceConfig.soundVolume));
#elif defined(BUZZ_PIN)
        for (int i = 0; i < count; i++) {
            if (!cueAllowed()) break; // muted or a radio started mid-cue
            // tone(BUZZ_PIN, ...) has no gain, so the level is ignored on these
            // boards. Do not fake one by shortening the note.
            tone(BUZZ_PIN, notes[i].freq, notes[i].ms);
            vTaskDelay(pdMS_TO_TICKS(notes[i].ms));
            if (notes[i].gap) vTaskDelay(pdMS_TO_TICKS(notes[i].gap));
        }
#else
        (void)notes;
        (void)count;
#endif
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
