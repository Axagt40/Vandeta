#include "audio.h"
#include "core/mykeyboard.h"

#if __has_include(<M5Unified.h>)
#include <M5Unified.h>
#endif

#if defined(HAS_NS4168_SPKR)
#include "AudioFileSourceFunction.h"
#include "AudioGeneratorAAC.h"
#include "AudioGeneratorFLAC.h"
#include "AudioGeneratorMIDI.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2SNoDAC.h"
#include <ESP8266Audio.h>
#include <ESP8266SAM.h>
// The Vandeta voice fetches a spoken clip over HTTPS, so it needs the network
// stack, and it caches that clip on whichever filesystem is mounted.
#include "core/sd_functions.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <vector>

void _setup_codec_speaker(bool enable) __attribute__((weak));
void _setup_codec_speaker(bool enable) {}

// Volume control constants
static const float AUDIO_VOLUME_SCALE = 0.1f;
static const float AUDIO_VOLUME_MAX = 100.0f;
// The UI cues are single short notes that have to be heard over a room, and
// the stock 0.1 scale made them ~-26 dBFS: measurable on the I2S line, inaudible
// on the T-Embed's small speaker. The cue path scales its own level by this
// instead (0.5 * the note's balance), so a cue is clearly louder than the old
// blip while a square wave at the top of the volume list still does not clip.
static const float AUDIO_CUE_SCALE = 0.5f;

// Task configuration
static const uint32_t AUDIO_TASK_STACK_SIZE = 16384; // 16KB - increased for complex MP3 files
static const UBaseType_t AUDIO_TASK_PRIORITY = 1;
static const BaseType_t AUDIO_TASK_CORE = 1; // Core 1

// ===== ASYNC PLAYBACK STATE =====
struct AudioPlayerState {
    // Playback objects
    AudioGenerator *generator;
    AudioFileSource *source;
    AudioOutputI2S *output;

    // State management
    PlaybackState state;
    PlaybackMode mode;
    String currentFile;

    // Control flags (volatile for thread safety)
    volatile bool stopRequested;
    volatile bool pauseRequested;
    volatile bool volumeChanged;
    volatile uint8_t newVolume;
    float currentGain; // Store current gain for pause fade

    // Timing info
    unsigned long startTime;
    unsigned long pausedTime;
    unsigned long totalPausedDuration;

    // Task handle
    TaskHandle_t taskHandle;

    // Mutex for thread safety
    SemaphoreHandle_t mutex;

    AudioPlayerState()
        : generator(nullptr), source(nullptr), output(nullptr), state(PLAYBACK_IDLE), mode(PLAYBACK_BLOCKING),
          currentFile(""), stopRequested(false), pauseRequested(false), volumeChanged(false), newVolume(0),
          currentGain(1.0f), startTime(0), pausedTime(0), totalPausedDuration(0), taskHandle(nullptr),
          mutex(nullptr) {
        mutex = xSemaphoreCreateMutex();
    }

    ~AudioPlayerState() {
        // Ensure task is stopped before destruction
        if (taskHandle != nullptr) {
            stopRequested = true;
            // Wait with timeout
            for (int i = 0; i < 200; i++) { // 2 seconds max
                if (taskHandle == nullptr) break;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        cleanup();

        if (mutex) {
            vSemaphoreDelete(mutex);
            mutex = nullptr;
        }
    }

    void cleanup() {
        if (generator) {
            if (generator->isRunning()) { generator->stop(); }
            delete generator;
            generator = nullptr;
        }
        if (source) {
            source->close();
            delete source;
            source = nullptr;
        }
        if (output) {
            output->stop();
            delete output;
            output = nullptr;
        }
    }

    bool lock(TickType_t timeout = portMAX_DELAY) {
        if (mutex) { return xSemaphoreTake(mutex, timeout) == pdTRUE; }
        return false;
    }

    void unlock() {
        if (mutex) { xSemaphoreGive(mutex); }
    }
};

// Global player state
static AudioPlayerState *g_audioPlayer = nullptr;

// Initialize player state
static void initAudioPlayer() {
    if (!g_audioPlayer) { g_audioPlayer = new AudioPlayerState(); }
}

// Internal helper - unified audio output creation
static AudioOutputI2S *createConfiguredAudioOutput() {
    AudioOutputI2S *audioout = new AudioOutputI2S();

    if (!audioout) {
        Serial.println("ERROR: AudioOutputI2S allocation failed");
        return nullptr;
    }

    audioout->SetPinout(BCLK, WCLK, DOUT, MCLK);
    audioout->SetGain(bruceConfig.soundVolume / AUDIO_VOLUME_MAX);

    return audioout;
}

// Helper to validate file existence
static bool validateAudioFile(FS *fs, const String &filepath) {
    if (!fs) {
        Serial.println("ERROR: Invalid filesystem pointer");
        return false;
    }

    if (!fs->exists(filepath.c_str())) {
        Serial.print("ERROR: Audio file not found: ");
        Serial.println(filepath);
        return false;
    }

    File file = fs->open(filepath.c_str(), "r");
    if (!file) {
        Serial.println("ERROR: Cannot open audio file");
        return false;
    }

    bool isFile = !file.isDirectory();
    size_t fileSize = file.size();
    file.close();

    if (!isFile) {
        Serial.println("ERROR: Path is a directory, not a file");
        return false;
    }

    if (fileSize == 0) {
        Serial.println("ERROR: Audio file is empty");
        return false;
    }

    return true;
}

// ===== ASYNC PLAYBACK TASK =====
static void audioPlaybackTask(void *parameter) {
    if (!g_audioPlayer) {
        vTaskDelete(NULL);
        return;
    }

    AudioPlayerState *player = g_audioPlayer;

    Serial.println("Audio playback task started");

    // Main playback loop
    while (player->generator && player->generator->isRunning()) {
        // Check for stop request (prioritized check)
        if (player->stopRequested) {
            Serial.println("Stop requested");
            break;
        }

        // Check for pause request
        if (player->pauseRequested) {
            if (player->lock(pdMS_TO_TICKS(100))) {
                if (player->state == PLAYBACK_PLAYING) {
                    // Fade out volume to prevent pop/crackling
                    for (int i = 10; i >= 0; i--) {
                        player->output->SetGain(player->currentGain * i / 10.0f);
                        vTaskDelay(pdMS_TO_TICKS(8));
                    }

                    // Stop I2S output to prevent buffer loop
                    player->output->stop();

                    player->state = PLAYBACK_PAUSED;
                    player->pausedTime = millis();
                    Serial.println("Playback paused - I2S stopped");
                } else if (player->state == PLAYBACK_PAUSED) {
                    // Restart I2S output
                    if (!player->output->begin()) {
                        Serial.println("ERROR: Failed to restart I2S output");
                        player->pauseRequested = false;
                        player->unlock();
                        break; // Exit task if can't restart
                    }

                    // Restore volume with fade in
                    for (int i = 0; i <= 10; i++) {
                        player->output->SetGain(player->currentGain * i / 10.0f);
                        vTaskDelay(pdMS_TO_TICKS(8));
                    }

                    player->state = PLAYBACK_PLAYING;
                    player->totalPausedDuration += millis() - player->pausedTime;
                    Serial.println("Playback resumed - I2S restarted");
                }
                player->pauseRequested = false;
                player->unlock();
            }
        }

        // Check for volume change request
        if (player->volumeChanged && player->output) {
            if (player->lock(pdMS_TO_TICKS(50))) {
                player->currentGain = player->newVolume / AUDIO_VOLUME_MAX;
                player->output->SetGain(player->currentGain);
                player->volumeChanged = false;
                player->unlock();
                Serial.print("Volume changed to: ");
                Serial.println(player->newVolume);
            }
        }

        // Only loop generator when not paused
        if (player->state == PLAYBACK_PLAYING) {
            if (!player->generator->loop()) {
                Serial.println("Generator loop ended");
                break;
            }
        } else {
            // Paused - yield to prevent busy waiting
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // Yield to prevent watchdog timeout
        taskYIELD();
    }

    // Cleanup after playback
    Serial.println("Audio playback task ending");

    // Use timeout on lock to prevent deadlock
    if (player->lock(pdMS_TO_TICKS(1000))) {
        player->state = PLAYBACK_STOPPING;

        if (player->output) player->output->stop();
        if (player->source) player->source->close();

        player->cleanup();
        player->state = PLAYBACK_IDLE;
        player->currentFile = "";

        // Clear task handle BEFORE unlocking
        player->taskHandle = nullptr;

        player->unlock();
    } else {
        // Couldn't acquire lock - force cleanup anyway
        Serial.println("WARNING: Force cleanup without lock");
        player->taskHandle = nullptr;
    }

    _setup_codec_speaker(false);

    // Delete task (must be last operation)
    vTaskDelete(NULL);
}
// ===== PUBLIC API IMPLEMENTATION =====

bool stopAudioPlayback() {
    initAudioPlayer();

    if (!g_audioPlayer) return false;

    // Quick check without lock
    if (g_audioPlayer->state == PLAYBACK_IDLE) { return false; }

    // Signal stop
    g_audioPlayer->stopRequested = true;

    // Wait for task to finish with timeout (2 seconds)
    unsigned long startWait = millis();
    while (g_audioPlayer->taskHandle != nullptr && (millis() - startWait) < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Force cleanup if timeout
    if (g_audioPlayer->taskHandle != nullptr) {
        Serial.println("WARNING: Stop timeout - forcing cleanup");
        if (g_audioPlayer->lock(pdMS_TO_TICKS(500))) {
            g_audioPlayer->cleanup();
            g_audioPlayer->state = PLAYBACK_IDLE;
            g_audioPlayer->taskHandle = nullptr;
            g_audioPlayer->unlock();
        }
    }

    return true;
}

bool pauseAudioPlayback() {
    initAudioPlayer();

    if (!g_audioPlayer) return false;

    PlaybackState currentState = g_audioPlayer->state;
    if (currentState != PLAYBACK_PLAYING && currentState != PLAYBACK_PAUSED) { return false; }

    g_audioPlayer->pauseRequested = true;

    return true;
}

bool isAudioPlaying() {
    initAudioPlayer();

    if (!g_audioPlayer) return false;

    PlaybackState state = g_audioPlayer->state;
    return (state == PLAYBACK_PLAYING || state == PLAYBACK_PAUSED);
}

AudioPlaybackInfo getAudioPlaybackInfo() {
    initAudioPlayer();

    AudioPlaybackInfo info;
    info.state = PLAYBACK_IDLE;
    info.currentFile = "";
    info.duration = 0;
    info.position = 0;
    info.volume = bruceConfig.soundVolume;
    info.isAsyncMode = false;

    if (!g_audioPlayer) return info;

    if (g_audioPlayer->lock(pdMS_TO_TICKS(100))) {
        info.state = g_audioPlayer->state;
        info.currentFile = g_audioPlayer->currentFile;
        info.volume = bruceConfig.soundVolume;
        info.isAsyncMode = (g_audioPlayer->mode == PLAYBACK_ASYNC);

        // Calculate position
        if (g_audioPlayer->state == PLAYBACK_PLAYING) {
            info.position = millis() - g_audioPlayer->startTime - g_audioPlayer->totalPausedDuration;
        } else if (g_audioPlayer->state == PLAYBACK_PAUSED) {
            info.position =
                g_audioPlayer->pausedTime - g_audioPlayer->startTime - g_audioPlayer->totalPausedDuration;
        }

        g_audioPlayer->unlock();
    }

    return info;
}

void setAudioPlaybackVolume(uint8_t volume) {
    if (volume > 100) volume = 100;

    // Update config
    bruceConfig.setSoundVolume(volume);

    initAudioPlayer();

    // If playing, signal volume change to task
    if (g_audioPlayer && g_audioPlayer->state != PLAYBACK_IDLE) {
        g_audioPlayer->newVolume = volume;
        g_audioPlayer->volumeChanged = true;
    }
}

// ===== HELPER: Start async playback task =====
static bool startAsyncPlayback(
    AudioGenerator *generator, AudioFileSource *source, AudioOutputI2S *output, const String &filename
) {
    initAudioPlayer();

    if (!g_audioPlayer->lock(pdMS_TO_TICKS(1000))) {
        Serial.println("ERROR: Could not acquire lock for async playback");
        delete generator;
        delete source;
        delete output;
        _setup_codec_speaker(false);
        return false;
    }

    // Set up state
    g_audioPlayer->generator = generator;
    g_audioPlayer->source = source;
    g_audioPlayer->output = output;
    g_audioPlayer->state = PLAYBACK_PLAYING;
    g_audioPlayer->mode = PLAYBACK_ASYNC;
    g_audioPlayer->currentFile = filename;
    g_audioPlayer->stopRequested = false;
    g_audioPlayer->pauseRequested = false;
    g_audioPlayer->volumeChanged = false;
    g_audioPlayer->currentGain = bruceConfig.soundVolume / AUDIO_VOLUME_MAX; // Store initial gain
    g_audioPlayer->startTime = millis();
    g_audioPlayer->pausedTime = 0;
    g_audioPlayer->totalPausedDuration = 0;

    g_audioPlayer->unlock();

    // Create task
    BaseType_t result = xTaskCreatePinnedToCore(
        audioPlaybackTask,
        "AudioPlayback",
        AUDIO_TASK_STACK_SIZE,
        NULL,
        AUDIO_TASK_PRIORITY,
        &g_audioPlayer->taskHandle,
        AUDIO_TASK_CORE
    );

    if (result != pdPASS || !g_audioPlayer->taskHandle) {
        Serial.println("ERROR: Failed to create playback task");

        // Cleanup on failure
        if (g_audioPlayer->lock(pdMS_TO_TICKS(500))) {
            g_audioPlayer->cleanup();
            g_audioPlayer->state = PLAYBACK_IDLE;
            g_audioPlayer->taskHandle = nullptr;
            g_audioPlayer->unlock();
        }

        _setup_codec_speaker(false);
        return false;
    }

    return true;
}

// ===== CORE PLAYBACK FUNCTIONS =====

bool playAudioFile(FS *fs, String filepath, PlaybackMode mode) {
    if (!bruceConfig.soundEnabled) return false;

    if (!validateAudioFile(fs, filepath)) { return false; }

    // Stop any current playback
    if (isAudioPlaying()) { stopAudioPlayback(); }

    _setup_codec_speaker(true);

    AudioFileSource *source = new AudioFileSourceFS(*fs, filepath.c_str());
    if (!source) {
        Serial.println("ERROR: Failed to create audio source");
        _setup_codec_speaker(false);
        return false;
    }

    AudioOutputI2S *audioout = createConfiguredAudioOutput();
    if (!audioout) {
        delete source;
        _setup_codec_speaker(false);
        return false;
    }

    AudioGenerator *generator = NULL;

    filepath.toLowerCase();
    if (filepath.endsWith(".txt") || filepath.endsWith(".rtttl")) generator = new AudioGeneratorRTTTL();
    else if (filepath.endsWith(".wav")) generator = new AudioGeneratorWAV();
    else if (filepath.endsWith(".mod")) generator = new AudioGeneratorMOD();
    else if (filepath.endsWith(".opus")) generator = new AudioGeneratorOpus();
    else if (filepath.endsWith(".aac")) generator = new AudioGeneratorAAC();
    else if (filepath.endsWith(".flac")) generator = new AudioGeneratorFLAC();
    else if (filepath.endsWith(".mp3")) {
        generator = new AudioGeneratorMP3();
        source = new AudioFileSourceID3(source);
    }

    if (!generator) {
        Serial.println("ERROR: Unsupported audio format");
        delete source;
        delete audioout;
        _setup_codec_speaker(false);
        return false;
    }

    if (!generator->begin(source, audioout)) {
        Serial.println("ERROR: Failed to begin audio playback");
        delete generator;
        delete source;
        delete audioout;
        _setup_codec_speaker(false);
        return false;
    }

    // === BLOCKING MODE ===
    if (mode == PLAYBACK_BLOCKING) {
        Serial.println("Start audio (blocking)");

        while (generator->isRunning()) {
            if (!generator->loop() || check(AnyKeyPress)) { generator->stop(); }
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        audioout->stop();
        source->close();
        Serial.println("Stop audio");

        delete generator;
        delete source;
        delete audioout;

        _setup_codec_speaker(false);
        return true;
    }

    // === ASYNC MODE ===
    Serial.println("Start audio (async)");
    return startAsyncPlayback(generator, source, audioout, filepath);
}

bool playAudioRTTTLString(String song, PlaybackMode mode) {
    if (!bruceConfig.soundEnabled) return false;

    song.trim();
    if (song == "") {
        Serial.println("ERROR: Empty RTTTL string");
        return false;
    }

    if (isAudioPlaying()) { stopAudioPlayback(); }

    _setup_codec_speaker(true);

    AudioOutputI2S *audioout = createConfiguredAudioOutput();
    if (!audioout) {
        _setup_codec_speaker(false);
        return false;
    }

    AudioGenerator *generator = new AudioGeneratorRTTTL();
    if (!generator) {
        delete audioout;
        _setup_codec_speaker(false);
        return false;
    }

    AudioFileSource *source = new AudioFileSourcePROGMEM(song.c_str(), song.length());
    if (!source) {
        delete generator;
        delete audioout;
        _setup_codec_speaker(false);
        return false;
    }

    if (!generator->begin(source, audioout)) {
        Serial.println("ERROR: Failed to begin RTTTL playback");
        delete generator;
        delete source;
        delete audioout;
        _setup_codec_speaker(false);
        return false;
    }

    // === BLOCKING MODE ===
    if (mode == PLAYBACK_BLOCKING) {
        Serial.println("Start RTTTL (blocking)");

        while (generator->isRunning()) {
            if (!generator->loop() || check(AnyKeyPress)) { generator->stop(); }
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        audioout->stop();
        source->close();
        Serial.println("Stop audio");

        delete generator;
        delete source;
        delete audioout;

        _setup_codec_speaker(false);
        return true;
    }

    // === ASYNC MODE ===
    Serial.println("Start RTTTL (async)");
    return startAsyncPlayback(generator, source, audioout, "RTTTL");
}
// ===== VANDETA VOICE: a real speaker instead of an English robot =====
//
// ESP8266SAM has one voice and one language: English phonemes. Hand it Finnish
// text and it spells the words out with American vowels - "Hei, mitä kuuluu"
// comes back as a robot reading a phone book. So when the device has Wi-Fi we
// ask a natural Finnish speaker for the sentence instead, keep the resulting
// MP3 on the mounted filesystem so a repeated phrase costs no network at all,
// and play it through the same I2S path every other sound uses. SAM stays as
// the offline fallback: no Wi-Fi still means `say` makes a sound.

static const size_t VOICE_CHUNK_CHARS = 170;              // endpoint limit is ~200 bytes of URL text
static const uint32_t VOICE_MAX_CLIP_BYTES = 512 * 1024;  // one sentence, never a whole audiobook
static const char *VOICE_CACHE_DIR = "/vandeta_voice";

// Clips are big enough that internal RAM should not hold them; PSRAM first, and
// a plain malloc only as the fallback for boards that have no PSRAM at all.
static uint8_t *voiceAlloc(size_t bytes) {
    uint8_t *p = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : (uint8_t *)malloc(bytes);
}

static String voiceUrlEncode(const String &in) {
    String out;
    out.reserve(in.length() * 3);
    for (size_t i = 0; i < in.length(); i++) {
        uint8_t c = (uint8_t)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char esc[4];
            snprintf(esc, sizeof(esc), "%%%02X", c);
            out += esc;
        }
    }
    return out;
}

// The endpoint refuses anything past ~200 bytes, and a byte is not a character
// in Finnish. Split on sentence ends first, then punctuation, then spaces, so a
// pause between clips lands where a speaker would breathe.
static void voiceSplitText(const String &text, std::vector<String> &out) {
    size_t start = 0;
    while (start < text.length()) {
        while (start < text.length() && (text[start] == ' ' || text[start] == '\n')) start++;
        if (start >= text.length()) break;

        size_t remaining = text.length() - start;
        size_t take = remaining <= VOICE_CHUNK_CHARS ? remaining : VOICE_CHUNK_CHARS;

        if (take < remaining) {
            size_t floor = start + VOICE_CHUNK_CHARS / 3;
            size_t cut = 0;
            for (size_t i = start + take; i > floor && !cut; i--) {
                char c = text[i];
                if (c == '.' || c == '!' || c == '?' || c == '\n') cut = i + 1;
            }
            for (size_t i = start + take; i > floor && !cut; i--) {
                char c = text[i];
                if (c == ',' || c == ';' || c == ':') cut = i + 1;
            }
            for (size_t i = start + take; i > start && !cut; i--) {
                if (text[i] == ' ') cut = i;
            }
            if (cut > start) take = cut - start;
        }

        String piece = text.substring(start, start + take);
        piece.trim();
        if (piece.length()) out.push_back(piece);
        start += take;
    }
}

// Name a clip after the language and the exact words: a phrase said twice is
// read from storage the second time, and the same phrase in another language is
// a different entry.
static String voiceCachePath(const String &lang, const String &text) {
    uint32_t h = 2166136261u; // FNV-1a
    for (size_t i = 0; i < text.length(); i++) {
        h ^= (uint8_t)text[i];
        h *= 16777619u;
    }
    char name[64];
    snprintf(
        name,
        sizeof(name),
        "%s/%s_%08x_%u.mp3",
        VOICE_CACHE_DIR,
        lang.c_str(),
        (unsigned)h,
        (unsigned)text.length()
    );
    return String(name);
}

static bool voiceReadCache(const String &path, uint8_t **out, size_t *outLen) {
    *out = nullptr;
    *outLen = 0;
    FS *fs = nullptr;
    if (!getFsStorage(fs) || !fs->exists(path)) return false;

    File f = fs->open(path, FILE_READ);
    if (!f) return false;
    size_t size = f.size();
    if (size < 512 || size > VOICE_MAX_CLIP_BYTES) {
        f.close();
        return false;
    }

    uint8_t *buf = voiceAlloc(size);
    if (!buf) {
        f.close();
        return false;
    }
    size_t got = f.read(buf, size);
    f.close();
    // A truncated file or an error page saved by an older build must not reach
    // the decoder; every MP3 frame starts with 0xFF.
    if (got != size || buf[0] != 0xFF) {
        free(buf);
        return false;
    }

    *out = buf;
    *outLen = size;
    return true;
}

static void voiceWriteCache(const String &path, const uint8_t *buf, size_t len) {
    FS *fs = nullptr;
    if (!getFsStorage(fs)) return;
    if (fs->exists(path)) return; // never rewrite a clip that is already right
    if (!fs->exists(VOICE_CACHE_DIR)) fs->mkdir(VOICE_CACHE_DIR);

    File f = fs->open(path, FILE_WRITE);
    if (!f) {
        Serial.println("VOICE: cache write failed");
        return;
    }
    f.write(buf, len);
    f.close();
}

// One HTTPS GET, straight into a PSRAM buffer. The clip is not a secret, so the
// certificate is not pinned: the device ships no CA store and a stale bundle
// would break speech on every certificate rotation.
static bool voiceFetchClip(const String &lang, const String &text, uint8_t **out, size_t *outLen) {
    *out = nullptr;
    *outLen = 0;
    if (WiFi.status() != WL_CONNECTED) return false;

    String url = "https://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=" + lang +
                 "&q=" + voiceUrlEncode(text);

    WiFiClientSecure secure;
    secure.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(6000);
    http.setTimeout(8000);
    // The endpoint answers 403 to a request without a browser user agent.
    http.setUserAgent(
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36"
    );
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(secure, url)) {
        Serial.println("VOICE: could not open the request");
        return false;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("VOICE: HTTP %d\n", code);
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    if (!stream) {
        http.end();
        return false;
    }

    uint8_t *buf = nullptr;
    size_t cap = 0, len = 0;
    uint32_t lastData = millis();
    while (millis() - lastData < 6000) {
        int avail = stream->available();
        if (avail > 0) {
            size_t want = (size_t)avail;
            if (len + want > VOICE_MAX_CLIP_BYTES) want = VOICE_MAX_CLIP_BYTES - len;
            if (want == 0) break;
            if (len + want > cap) {
                size_t newCap = cap ? cap : 8192;
                while (newCap < len + want) newCap *= 2;
                if (newCap > VOICE_MAX_CLIP_BYTES) newCap = VOICE_MAX_CLIP_BYTES;
                uint8_t *grown = voiceAlloc(newCap);
                if (!grown) break;
                if (buf) {
                    memcpy(grown, buf, len);
                    free(buf);
                }
                buf = grown;
                cap = newCap;
            }
            int got = stream->readBytes(buf + len, want);
            if (got <= 0) break;
            len += got;
            lastData = millis(); // data is flowing: restart the idle timer
        } else if (!http.connected()) {
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    http.end();

    // An HTML error page is not speech; a real clip is a pile of MP3 frames.
    if (len < 512 || buf[0] != 0xFF) {
        if (buf) free(buf);
        Serial.println("VOICE: response was not audio");
        return false;
    }

    *out = buf;
    *outLen = len;
    return true;
}

// Blocking, like the SAM call it replaces: one clip, stop on any key.
static bool voicePlayClip(uint8_t *buf, size_t len) {
    AudioOutputI2S *out = createConfiguredAudioOutput();
    if (!out) return false;
    if (!out->begin()) {
        delete out;
        return false;
    }

    AudioFileSourcePROGMEM *src = new AudioFileSourcePROGMEM(buf, len);
    AudioGeneratorMP3 *gen = new AudioGeneratorMP3();
    bool ok = src && gen && gen->begin(src, out);

    if (ok) {
        while (gen->isRunning()) {
            if (!gen->loop() || check(AnyKeyPress)) gen->stop();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    if (gen) {
        if (gen->isRunning()) gen->stop();
        delete gen;
    }
    if (src) {
        src->close();
        delete src;
    }
    out->stop();
    delete out;
    return ok;
}

// Speak `text` in the configured voice. Returns false when nothing was spoken,
// so the caller can still fall back to SAM.
static bool ttsSpeakOnline(const String &text) {
    std::vector<String> parts;
    voiceSplitText(text, parts);
    if (parts.empty()) return false;

    const String lang = voiceLangTag(bruceConfig.voiceLang);
    bool spoke = false;

    for (size_t i = 0; i < parts.size(); i++) {
        // A long text is several clips; this is the only place a press can stop
        // it, since each clip plays to its end.
        if (i > 0 && check(AnyKeyPress)) break;

        String path = voiceCachePath(lang, parts[i]);
        uint8_t *buf = nullptr;
        size_t len = 0;

        if (!voiceReadCache(path, &buf, &len)) {
            if (!voiceFetchClip(lang, parts[i], &buf, &len)) {
                if (buf) free(buf);
                return spoke;
            }
            voiceWriteCache(path, buf, len);
        }

        _setup_codec_speaker(true);
        bool ok = voicePlayClip(buf, len);
        _setup_codec_speaker(false);

        if (buf) free(buf);
        if (!ok) break;
        spoke = true;
    }

    return spoke;
}

bool tts(String text, PlaybackMode mode) {
    if (!bruceConfig.soundEnabled) return false;

    text.trim();
    if (text == "") {
        Serial.println("ERROR: Empty TTS text");
        return false;
    }

    // TTS is always blocking - ESP8266SAM doesn't support async playback
    if (mode == PLAYBACK_ASYNC) { Serial.println("WARNING: TTS doesn't support async mode, using blocking"); }

    // Stop any current playback
    if (isAudioPlaying()) { stopAudioPlayback(); }

    // Natural speech first: SAM only knows English phonemes, so every other
    // language through it is spelling rather than speaking.
    if (bruceConfig.voiceOnline) {
        if (ttsSpeakOnline(text)) return true;
        Serial.println("VOICE: online voice unavailable, falling back to SAM");
    }

    _setup_codec_speaker(true);

    AudioOutputI2S *audioout = createConfiguredAudioOutput();
    if (!audioout) {
        _setup_codec_speaker(false);
        return false;
    }

    if (!audioout->begin()) {
        Serial.println("ERROR: Failed to initialize audio output for TTS");
        delete audioout;
        _setup_codec_speaker(false);
        return false;
    }

    ESP8266SAM *sam = new ESP8266SAM;
    if (!sam) {
        delete audioout;
        _setup_codec_speaker(false);
        return false;
    }

    // TTS synthesis (always blocking)
    sam->Say(audioout, text.c_str());

    delete sam;
    delete audioout;

    _setup_codec_speaker(false);
    return true;
}

bool isAudioFile(const String &filepath) {
    return filepath.endsWith(".opus") || filepath.endsWith(".rtttl") || filepath.endsWith(".txt") ||
           filepath.endsWith(".wav") || filepath.endsWith(".mod") || filepath.endsWith(".mp3") ||
           filepath.endsWith(".aac") || filepath.endsWith(".flac");
}

// allowKeyAbort is false for the sound-theme cue player: it runs on its own
// task, and check(AnyKeyPress) there would consume the press the UI is waiting
// for - a beep must never eat a button.
static void playToneInternal(
    unsigned int frequency, unsigned long duration, short waveType, float amplitude, bool allowKeyAbort
) {
    if (!bruceConfig.soundEnabled) return;

    _setup_codec_speaker(true);

    if (frequency == 0 || duration == 0) {
        if (frequency == 0 && duration > 0) {
            delay(duration);
        }
        _setup_codec_speaker(false);
        return;
    }

    float hz = frequency;

    AudioOutputI2S *out = createConfiguredAudioOutput();
    if (!out) {
        _setup_codec_speaker(false);
        return;
    }

    AudioFileSourceFunction *file = new AudioFileSourceFunction(duration / 1000.0);
    if (!file) {
        delete out;
        _setup_codec_speaker(false);
        return;
    }

    // `amplitude` arrives already scaled (see playTone/playToneAtLevel): one
    // place decides how loud a tone is, not two multiplying constants.
    float volumeScale = amplitude;
    if (volumeScale > 1.0f) volumeScale = 1.0f;
    if (volumeScale < 0.0f) volumeScale = 0.0f;

    if (waveType == 0) {
        file->addAudioGenerators([volumeScale, hz](const float time) {
            float v = (sin(hz * time) >= 0) ? 1.0f : -1.0f;
            v *= volumeScale;
            return v;
        });
    } else if (waveType == 1) {
        file->addAudioGenerators([volumeScale, hz](const float time) {
            float v = sin(TWO_PI * hz * time);
            v *= fmod(time, 1.f);
            v *= volumeScale;
            return v;
        });
    }

    AudioGeneratorWAV *wav = new AudioGeneratorWAV();
    if (!wav) {
        delete file;
        delete out;
        _setup_codec_speaker(false);
        return;
    }

    if (!wav->begin(file, out)) {
        Serial.println("ERROR: Failed to begin tone playback");
        delete wav;
        delete file;
        delete out;
        _setup_codec_speaker(false);
        return;
    }

    while (wav->isRunning()) {
        if (!wav->loop() || (allowKeyAbort && check(AnyKeyPress))) { wav->stop(); }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    delete file;
    delete wav;
    delete out;

    _setup_codec_speaker(false);
}

void playTone(unsigned int frequency, unsigned long duration, short waveType) {
    playToneInternal(
        frequency, duration, waveType, (bruceConfig.soundVolume / AUDIO_VOLUME_MAX) * AUDIO_VOLUME_SCALE, true
    );
}

void playToneAtLevel(unsigned int frequency, unsigned long duration, float level) {
    if (!bruceConfig.soundEnabled) return;
    if (level <= 0.0f) return;
    if (level > 1.0f) level = 1.0f;
    playToneInternal(frequency, duration, 0, level * AUDIO_CUE_SCALE, false);
}

#endif

void _tone(unsigned int frequency, unsigned long duration) {
    if (!bruceConfig.soundEnabled) return;

#if defined(BUZZ_PIN)
    tone(BUZZ_PIN, frequency, duration);
#elif defined(HAS_NS4168_SPKR)
#if __has_include(<M5Unified.h>)
    if (frequency == 0) {
        if (duration > 0) delay(duration);
    } else {
        uint8_t m5vol = (bruceConfig.soundVolume * 255) / AUDIO_VOLUME_MAX;
        M5.Speaker.setVolume(m5vol);
        M5.Speaker.tone(frequency, duration);
        if (duration > 0) delay(duration);
    }
#else
    playTone(frequency, duration, 0);
#endif
#endif
}
