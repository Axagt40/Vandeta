#pragma once

#include <SPIFFS.h>
// Keep SPIFFS first

// Playback modes
enum PlaybackMode {
    PLAYBACK_BLOCKING = 0, // Original behavior
    PLAYBACK_ASYNC = 1     // Non-blocking (runs in task)
};

// Playback state
enum PlaybackState {
    PLAYBACK_IDLE = 0,
    PLAYBACK_PLAYING,
    PLAYBACK_PAUSED,
    PLAYBACK_STOPPING,
    PLAYBACK_ERROR
};

// Playback info structure (for GUI)
struct AudioPlaybackInfo {
    PlaybackState state;
    String currentFile;
    unsigned long duration; // Total duration in ms (0 if unknown) ACTUALLY NOT IMPLEMENTED
    unsigned long position; // Current position in ms
    uint8_t volume;
    bool isAsyncMode;
};

// Existing functions
bool playAudioFile(FS *fs, String filepath, PlaybackMode mode = PLAYBACK_BLOCKING);
bool playAudioRTTTLString(String song, PlaybackMode mode = PLAYBACK_BLOCKING);
bool tts(String text, PlaybackMode mode = PLAYBACK_BLOCKING);
bool isAudioFile(const String &filePath);
void playTone(unsigned int frequency, unsigned long duration = 0UL, short waveType = 0);

// One note of a UI cue. Shared here so the theme tables and the player agree on
// the layout instead of keeping two copies of the same struct.
struct AudioCueNote {
    uint16_t freq;  // Hz, 0 terminates the run
    uint16_t ms;    // how long the note sounds
    uint16_t gap;   // silence after it, before the next note
    uint8_t level;  // 0-100, this note against the others in the same cue
};

// Play a whole cue as ONE waveform: exact note lengths and gaps, a short
// attack/release so the speaker does not click, one I2S start and one drain
// instead of one session per note. `level` is the settings volume curve (0..1);
// each note's own `level` is applied on top. Blocks for the cue's length, so it
// belongs on the cue task, and it never reads the button flags.
void playCueSequence(const AudioCueNote *notes, int count, float level);

void _tone(unsigned int frequency, unsigned long duration = 0UL);

// NEW: Async playback control API
bool stopAudioPlayback();                    // Stop current playback
bool pauseAudioPlayback();                   // Pause/resume toggle
bool isAudioPlaying();                       // Check if audio is playing
AudioPlaybackInfo getAudioPlaybackInfo();    // Get current state for GUI
void setAudioPlaybackVolume(uint8_t volume); // Change volume during playback
