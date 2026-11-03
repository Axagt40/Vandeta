#include "rf_wardriver.h"

#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "protocols/rf_decoder.h" // rf_decode_ook / rf_decode_keeloq
#include "protocols/rf_encoder.h" // rf_tx_durations (verbatim raw replay)
#include "rf_send.h"             // readSubFile / txSubFile (raw replay)
#include "rf_utils.h"
#include <driver/rmt_rx.h>
#include <globals.h>
#include <math.h>

// ---------------------------------------------------------------------------
// SubGHz Wardriver
//
// Capture primitive: one RMT RX channel with a 20 ms idle threshold, re-armed
// after every burst. A burst therefore ends at the gap between two button
// presses, which is exactly the unit that should become one .sub file.
//
// Why not reuse rf_raw_record(): that one records a single burst for 20 s after
// the first signal and then stops. A wardriver has to run for hours, must never
// grow its memory use, and must not keep the previous burst around - so the
// pulse train is hashed, compared against a ring of recent fingerprints and
// streamed to the card immediately.
// ---------------------------------------------------------------------------

#define WR_RX_SYMBOLS_MAX 128 // 256 pulses per RMT read. A whole KeeLoq press (~140
                              // pulses) then fits in a single read, so not a single pulse
                              // is lost at a read boundary. Falls back to 64 symbols
                              // (128 pulses) when the RMT pool cannot give 128.
#define WR_RX_SYMBOLS_MIN 64 // setup_rf_rx()'s default block size.
                             // A burst is never limited to one read either way: when the
                             // buffer fills, the rest of the burst keeps streaming into
                             // the same file (see the wrStream* section below).
#define WR_IDLE_MAX_NS 30000000ULL // 30 ms of silence ends a burst (the RMT idle ceiling)
                                   // 20 ms used to split NICE (25 ms gap) frames in two and
                                   // to drop the repeats that make a raw replay work.
#define WR_PULSE_MIN_NS 2000ULL    // ignore sub-2 us glitches
#define WR_DEDUPE_SLOTS 64
// One capture is decoded/replayed in RAM, so it has a hard ceiling: 8192 signed
// timings = 32 KB, comfortably inside the internal DRAM left at the menu. A file
// past it is refused with a message instead of aborting the firmware on a failed
// allocation (std::vector growth on this target has no exceptions to catch).
#define WR_MAX_TIMINGS 8192
#define WR_HOP_MS 400 // dwell per band when hopping (a keyfob frame is 50-300 ms)

// The bands worth covering on a walk: EU/Asia 433 and 868, US 315 and 915.
// A capture records the band it was heard on, so replays go back out on it.
static const float WR_BANDS[] = {433.92f, 315.0f, 868.35f, 915.0f};
static const int WR_BAND_COUNT = sizeof(WR_BANDS) / sizeof(WR_BANDS[0]);

struct WardriverStats {
    uint32_t saved = 0;
    uint32_t duplicates = 0;
    uint32_t noise = 0;
    uint16_t lastPulses = 0;
    uint32_t lastDurationMs = 0;
    String lastFile;
};

static uint32_t wrHashes[WR_DEDUPE_SLOTS];
static uint32_t wrHashTimes[WR_DEDUPE_SLOTS];
static int wrHashSlot = 0;
static int wrNextFile = 1;

// FNV-1a over the pulse lengths in 4 us buckets, with the level in the low bit
// so an inverted copy is not mistaken for the same remote. Split into seed /
// feed / finish so a burst that spans several RMT reads hashes as one signal.
static uint32_t wrHashSeed() { return 2166136261u; }

static uint32_t wrHashFeed(uint32_t h, const rmt_symbol_word_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t d0 = s[i].duration0 / 4;
        uint32_t d1 = s[i].duration1 / 4;
        h = (h ^ ((d0 << 1) | (s[i].level0 & 1))) * 16777619u;
        h = (h ^ ((d1 << 1) | (s[i].level1 & 1))) * 16777619u;
    }
    return h;
}

static uint32_t wrHashFinish(uint32_t h, size_t n) { return (h ^ (uint32_t)n) * 16777619u; }

static uint32_t wrHashBurst(const rmt_symbol_word_t *s, size_t n) {
    return wrHashFinish(wrHashFeed(wrHashSeed(), s, n), n);
}

static bool wrSeenRecently(uint32_t hash) {
    uint16_t window = bruceConfig.rfWardriverDedupeSec;
    if (window == 0) return false; // dedupe disabled
    uint32_t now = millis();
    for (int i = 0; i < WR_DEDUPE_SLOTS; i++) {
        if (wrHashes[i] == hash && wrHashes[i] != 0 && (now - wrHashTimes[i]) < (uint32_t)window * 1000u) {
            return true;
        }
    }
    return false;
}

static void wrRememberHash(uint32_t hash) {
    wrHashes[wrHashSlot] = hash;
    wrHashTimes[wrHashSlot] = millis();
    wrHashSlot = (wrHashSlot + 1) % WR_DEDUPE_SLOTS;
}

static bool wrEnsureFolder(FS *fs) {
    if (!fs->exists("/bruce")) fs->mkdir("/bruce");
    if (!fs->exists("/bruce/subghz")) fs->mkdir("/bruce/subghz");
    if (!fs->exists(RF_WARDRIVER_DIR)) fs->mkdir(RF_WARDRIVER_DIR);
    return fs->exists(RF_WARDRIVER_DIR);
}

// Find the first free ward_NNNN.sub once, at start-up, so the save path costs
// nothing per signal.
static void wrScanNextIndex(FS *fs) {
    char name[64];
    wrNextFile = 1;
    while (wrNextFile < 100000) {
        snprintf(name, sizeof(name), RF_WARDRIVER_DIR "/ward_%04d.sub", wrNextFile);
        if (!fs->exists(name)) break;
        wrNextFile++;
    }
}

// Short writes mean the card is full or was pulled: every writer must notice,
// or the UI reports captures that are not on the card.
static bool wrWrote(File &f, const void *data, size_t len) {
    return f.write((const uint8_t *)data, len) == len;
}

static bool wrWriteFile(FS *fs, const String &path, const rmt_symbol_word_t *s, size_t n, float freqMhz) {
    File f = fs->open(path, FILE_WRITE);
    if (!f) return false;

    char line[64];
    int len = snprintf(
        line, sizeof(line), "Filetype: Bruce SubGhz File\nVersion 1\nFrequency: %d\n",
        (int)(freqMhz * 1000000.0f)
    );
    bool ok = wrWrote(f, line, len);
    ok = wrWrote(f, "Preset: 0\nProtocol: RAW\n", 24) && ok;

    // Same RAW_Data layout the recorder and Sub-GHz sender already agree on:
    // positive = carrier on, negative = carrier off, max 512 values per line.
    ok = wrWrote(f, "RAW_Data: ", 10) && ok;
    uint16_t values = 0;
    for (size_t i = 0; i < n; i++) {
        for (int half = 0; half < 2; half++) {
            uint16_t d = half == 0 ? s[i].duration0 : s[i].duration1;
            uint8_t level = half == 0 ? s[i].level0 : s[i].level1;
            if (d == 0) continue;
            len = snprintf(line, sizeof(line), "%s%d ", level ? "" : "-", d);
            ok = wrWrote(f, line, len) && ok;
            if (++values % 512 == 0) ok = wrWrote(f, "\nRAW_Data: ", 11) && ok;
        }
    }
    ok = wrWrote(f, "\n", 1) && ok;
    f.close();
    if (!ok) fs->remove(path); // never leave a half-written capture behind
    return ok;
}

static bool wrSaveBurst(FS *fs, const rmt_symbol_word_t *s, size_t n, WardriverStats &st, float freqMhz) {
    char name[64];
    snprintf(name, sizeof(name), RF_WARDRIVER_DIR "/ward_%04d.sub", wrNextFile);
    if (!wrWriteFile(fs, name, s, n, freqMhz)) return false;
    st.lastFile = String(name);
    wrNextFile++;
    return true;
}

// ---------------------------------------------------------------------------
// Streaming burst writer
//
// An RMT read holds only as many symbols as the driver's buffer (64 by default,
// 128 when the wardriver can get them). One keyfob press can be longer than that
// (KeeLoq ~140 pulses, any press with repeats much longer), and the driver
// simply ends the read when the buffer is full. Treating each read as a whole
// signal therefore wrote truncated files: the press was chopped wherever the
// buffer ran out, and the in-flight pulse at the seam came out short.
//
// Instead, a read that fills the buffer is understood as "this burst is not
// over" and its symbols are appended to the file that was opened for it. The
// file stays open until a read comes back short - only then did the radio see
// the idle gap, so only then is the signal complete. Nothing is buffered in
// RAM, so a long press costs card space and nothing else, and the card is not
// touched at all until a burst is known to be long (short noise bursts still
// take the old path, so they never create files).
// ---------------------------------------------------------------------------
// The CC1101's OOK slicer emits random edges when no carrier is present, and
// those edges are only a few µs long - a 10 s walk collected bursts of ~1000
// two-to-twenty-µs pulses and saved them as "signals". A real OOK remote never
// gets that fast (the shortest common pulse is ~100 µs; 50 µs is already
// generous), so a burst made mostly of sub-50 µs edges is noise, no matter how
// many edges it has.
#define WR_SHORT_PULSE_US 50

static bool wrLooksLikeRemote(uint32_t pulses, uint32_t shortPulses) {
    if (pulses < bruceConfig.rfWardriverMinPulses) return false;
    return (uint64_t)shortPulses * 4ULL < (uint64_t)pulses;
}

struct WrStream {
    File f;
    FS *fs = nullptr;
    String path;
    uint32_t pulses = 0;
    uint32_t shortPulses = 0;
    uint64_t totalNs = 0;
    uint32_t hash = 0;
    uint32_t symbolCount = 0;
    uint16_t valuesOnLine = 0;
    bool wroteAny = false;
    bool failed = false; // a short write: card full or pulled
    bool active = false;
};

// One stream at a time: the wardriver writes signals as they are heard, not a
// log of everything, so there is never a second burst in flight.
static WrStream wrStream;

// Open `path` and write the header; the caller decides the name (the capture
// loop uses the ward_%04d sequence, the self-test uses a scratch path).
static bool wrStreamBeginAt(FS *fs, float freqMhz, const String &path) {
    wrStream.f = fs->open(path, FILE_WRITE);
    if (!wrStream.f) return false;

    char line[64];
    int len = snprintf(
        line, sizeof(line), "Filetype: Bruce SubGhz File\nVersion 1\nFrequency: %d\n",
        (int)(freqMhz * 1000000.0f)
    );
    bool hdr = wrWrote(wrStream.f, line, len);
    hdr = wrWrote(wrStream.f, "Preset: 0\nProtocol: RAW\n", 24) && hdr;
    if (!hdr) { // card full / pulled: do not keep a header-less file
        wrStream.f.close();
        fs->remove(path);
        return false;
    }

    wrStream.fs = fs;
    wrStream.path = path;
    wrStream.pulses = 0;
    wrStream.shortPulses = 0;
    wrStream.totalNs = 0;
    wrStream.hash = wrHashSeed();
    wrStream.symbolCount = 0;
    wrStream.valuesOnLine = 0;
    wrStream.wroteAny = false;
    wrStream.failed = false;
    wrStream.active = true;
    return true;
}

static bool wrStreamBegin(FS *fs, float freqMhz) {
    char name[64];
    snprintf(name, sizeof(name), RF_WARDRIVER_DIR "/ward_%04d.sub", wrNextFile);
    if (!wrStreamBeginAt(fs, freqMhz, String(name))) return false;
    wrNextFile++; // reserve the name even if the burst is later dropped as noise
    return true;
}

// Append one RMT read. Same RAW_Data layout as wrWriteFile: positive = carrier
// on, negative = carrier off, 512 values per line.
static void wrStreamAppend(const rmt_symbol_word_t *s, size_t n) {
    char line[24];
    for (size_t i = 0; i < n; i++) {
        for (int half = 0; half < 2; half++) {
            uint16_t d = half == 0 ? s[i].duration0 : s[i].duration1;
            uint8_t level = half == 0 ? s[i].level0 : s[i].level1;
            if (d == 0) continue;
            if (wrStream.valuesOnLine == 0) {
                const char *pre = wrStream.wroteAny ? "\nRAW_Data: " : "RAW_Data: ";
                if (!wrWrote(wrStream.f, pre, strlen(pre))) wrStream.failed = true;
                wrStream.wroteAny = true;
            }
            int len = snprintf(line, sizeof(line), "%s%u ", level ? "" : "-", (unsigned)d);
            if (!wrWrote(wrStream.f, line, len)) wrStream.failed = true;
            if (++wrStream.valuesOnLine >= 512) wrStream.valuesOnLine = 0;
        }
    }
    wrStream.hash = wrHashFeed(wrStream.hash, s, n);
}

// Close the burst. `keep` == false deletes the file (too short to be a remote,
// or an identical repeat of something heard moments ago).
// Returns false when the capture could not be stored completely (the file is
// then deleted, so a truncated recording is never mistaken for a good one).
static bool wrStreamEnd(WardriverStats &st, bool keep) {
    if (!wrStream.active) return false;
    bool ok = !wrStream.failed;
    if (wrStream.wroteAny) ok = wrWrote(wrStream.f, "\n", 1) && ok;
    wrStream.f.close();
    keep = keep && ok;
    if (!keep) {
        if (wrStream.fs != nullptr) wrStream.fs->remove(wrStream.path);
    } else {
        st.lastFile = wrStream.path;
        st.lastPulses = (uint16_t)(wrStream.pulses > 65535u ? 65535u : wrStream.pulses);
        st.lastDurationMs = (uint32_t)(wrStream.totalNs / 1000000ULL);
    }
    wrStream.active = false;
    wrStream.fs = nullptr;
    wrStream.path = "";
    return keep;
}

static void wrDraw(bool full, const WardriverStats &st, bool armed, float freqMhz, bool hopping) {
    drawMainBorder(full);
    tft.setTextSize(1);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setCursor(10, 27);
    tft.print("SUBGHz WARDRIVER");
    tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
    tft.drawRightString(String(freqMhz, 2) + " MHz" + (hopping ? " HOP" : ""), tftWidth - 10, 27, 1);
    tft.drawFastHLine(10, 38, tftWidth - 20, bruceConfig.priColor);

    // The counters are the point of the screen: big, and only redrawn when the
    // band is repainted so the number never flickers.
    tft.setTextSize(2);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setCursor(12, 46);
    tft.print("SIGNALS ");
    tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
    tft.print(st.saved);

    tft.setTextSize(1);
    tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
    tft.setCursor(12, 70);
    tft.print("duplicates skipped " + String(st.duplicates));
    tft.setCursor(12, 82);
    tft.print("noise dropped     " + String(st.noise));

    tft.drawFastHLine(10, 96, tftWidth - 20, bruceConfig.secColor);
    tft.setCursor(10, 100);
    if (st.saved) {
        tft.print("last: " + String(st.lastPulses) + " pulses, " + String(st.lastDurationMs) + " ms");
    } else {
        tft.print("last: waiting for a signal");
    }
    tft.setCursor(10, 112);
    tft.print(armed ? "listening..." : "radio stopped");

    tft.drawFastHLine(10, tftHeight - 24, tftWidth - 20, bruceConfig.priColor);
    tft.setCursor(10, tftHeight - 21);
    tft.print(String(st.lastFile.length() ? st.lastFile : String(RF_WARDRIVER_DIR)).substring(0, 50));
    tft.setCursor(10, tftHeight - 10);
    tft.print("SEL stop    BACK exit");
}

// ---------------------------------------------------------------------------
// Session browser: replay a capture, or hand it to the decoders
//
// The capture screen only writes raw .sub files. Everything a session is worth
// is on the card afterwards: each capture can be replayed exactly as recorded
// (the same RAW_Data path Files > .sub uses), or run through the OOK/KeeLoq
// decoders once and written out a second time as a protocol code, which is what
// makes it reusable later. The raw file is never modified or removed.
// ---------------------------------------------------------------------------

#define WR_LIST_MAX 40

struct WrFile {
    String path;
    String name;
};

// One shared table for every browser entry point. Four separate statics here
// cost ~8 KB of internal DRAM (.bss), which is the resource that gates the radios
// on this board (DISCOVERIES 64).
static WrFile wrFiles[WR_LIST_MAX];

static int wrListFiles(FS *fs, WrFile *out, int maxOut) {
    int n = 0;
    File dir = fs->open(RF_WARDRIVER_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    File e = dir.openNextFile();
    while (e && n < maxOut) {
        if (!e.isDirectory()) {
            String p = e.path();
            if (p.length() == 0) p = String(RF_WARDRIVER_DIR) + "/" + e.name();
            if (p.endsWith(".sub") && !p.endsWith("_dec.sub")) {
                out[n].path = p;
                out[n].name = p.substring(p.lastIndexOf('/') + 1);
                n++;
            }
        }
        e = dir.openNextFile();
    }
    dir.close();
    return n;
}

// Signed durations (HIGH > 0, LOW < 0) plus the file's own frequency.
static bool wrReadDurations(
    FS *fs, const String &path, std::vector<int> &durs, float &freqMhz, bool *tooLong = nullptr
) {
    durs.clear();
    freqMhz = 0;
    if (tooLong != nullptr) *tooLong = false;
    File f = fs->open(path, FILE_READ);
    if (!f) return false;

    String line;
    while (f.available()) {
        line = f.readStringUntil('\n');
        if (line.startsWith("Frequency:")) {
            freqMhz = line.substring(10).toFloat() / 1000000.0f;
        } else if (line.startsWith("RAW_Data:") || line.startsWith("Data_RAW:")) {
            String data = line.substring(line.indexOf(':') + 1);
            int i = 0;
            while (i < (int)data.length()) {
                while (i < (int)data.length() && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r')) i++;
                if (i >= (int)data.length()) break;
                int start = i;
                if (data[i] == '-' || data[i] == '+') i++;
                while (i < (int)data.length() && data[i] >= '0' && data[i] <= '9') i++;
                if (i > start) {
                    // Clamp: a corrupt line can carry INT_MIN, and -INT_MIN is
                    // undefined behaviour in the decoders and the TX packer.
                    long v = data.substring(start, i).toInt();
                    if (v > 1000000L) v = 1000000L;
                    if (v < -1000000L) v = -1000000L;
                    if (durs.size() < WR_MAX_TIMINGS) durs.push_back((int)v);
                }
            }
        }
    }
    f.close();
    if (durs.size() >= WR_MAX_TIMINGS) {
        // Refuse rather than decode/replay a truncated understanding of the file.
        durs.clear();
        if (tooLong != nullptr) *tooLong = true;
        return false;
    }
    return durs.size() >= 4;
}

// Write the decoded form next to the raw capture: ward_0007.sub -> ward_0007_dec.sub
static bool wrWriteDecoded(FS *fs, const String &rawPath, const RfCodes &code, float freqMhz) {
    String base = rawPath.substring(0, rawPath.length() - 4); // strip ".sub"
    String out = base + "_dec.sub";
    File f = fs->open(out, FILE_WRITE);
    if (!f) return false;

    char line[96];
    int len = snprintf(line, sizeof(line), "Filetype: Bruce SubGhz File\nVersion 1\n");
    bool ok = wrWrote(f, line, len);
    len = snprintf(line, sizeof(line), "Frequency: %u\n", (unsigned)(freqMhz * 1000000.0f));
    ok = wrWrote(f, line, len) && ok;
    len = snprintf(
        line, sizeof(line), "Preset: %s\nProtocol: %s\nTE: %d\nBit: %d\n",
        code.preset.length() ? code.preset.c_str() : "FuriHalSubGhzPresetOok650Async", code.protocol.c_str(),
        code.te, code.Bit
    );
    ok = wrWrote(f, line, len) && ok;
    len = snprintf(line, sizeof(line), "Key: %016llX\n", (unsigned long long)code.key);
    ok = wrWrote(f, line, len) && ok;
    if (code.mf_name.length()) {
        String mf = "Manufacturer: " + code.mf_name + "\n";
        ok = wrWrote(f, mf.c_str(), mf.length()) && ok;
    }
    f.close();
    if (!ok) fs->remove(out); // a half-written decoded file is worse than none
    return ok;
}

// Decode one capture. Returns the protocol name, or empty when it is not a
// signal the registry knows (the raw file stays exactly as it was).
static String wrIdentifyFile(FS *fs, const String &path, String *decodedPath) {
    std::vector<int> durs;
    float freq = 0;
    if (!wrReadDurations(fs, path, durs, freq)) return "";

    RfCodes code;
    bool decoded = rf_decode_ook(durs, code) || rf_decode_keeloq(durs, code);
    if (!decoded) {
        // The OOK decoder closes a frame on two similar inter-frame gaps, and a
        // wardriver capture is whatever the RMT buffer caught: often exactly one
        // press, with no second gap. Stitch the frame behind itself with candidate
        // gaps until a protocol matches. The gap is not free-form: the decoder
        // divides it by the protocol's sync factor to recover `te`, so the sweep
        // covers the usual sync*te products (Princeton 31*350, CAME 36*700,
        // Nice 36*700, KeeLoq 10*400, ...).
        static const int gaps[] = {10850, 25200, 7000, 14000, 21000, 33600, 42000, 70000, 5000, 4000, 9000, 12000};
        // The stitch triples the capture in RAM; only attempt it when the result
        // still fits the timings ceiling, or a long capture would exhaust the
        // heap inside the identify-all loop.
        if (durs.size() * 3 + 2 <= WR_MAX_TIMINGS) {
            for (int gap : gaps) {
                std::vector<int> repeated;
                repeated.reserve(durs.size() * 3 + 2);
                repeated.insert(repeated.end(), durs.begin(), durs.end());
                repeated.push_back(-gap);
                repeated.insert(repeated.end(), durs.begin(), durs.end());
                repeated.push_back(-gap);
                repeated.insert(repeated.end(), durs.begin(), durs.end());
                if (rf_decode_ook(repeated, code) || rf_decode_keeloq(repeated, code)) {
                    decoded = true;
                    break;
                }
            }
        }
    }
    if (!decoded) return "";
    if (code.protocol.length() == 0) return "";
    if (!wrWriteDecoded(fs, path, code, freq > 0 ? freq : bruceConfig.rfWardriverFreq)) return "";
    if (decodedPath != nullptr) {
        *decodedPath = path.substring(0, path.length() - 4) + "_dec.sub";
    }
    return code.protocol;
}

// Every capture in the folder, not just the ones the browser table can hold.
static int wrCountFiles(FS *fs) {
    int n = 0;
    File dir = fs->open(RF_WARDRIVER_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    File e = dir.openNextFile();
    while (e) {
        if (!e.isDirectory()) {
            String p = e.path();
            if (p.length() == 0) p = e.name();
            if (p.endsWith(".sub") && !p.endsWith("_dec.sub")) n++;
        }
        e = dir.openNextFile();
    }
    dir.close();
    return n;
}

static void wrIdentifyAll(FS *fs) {
    int total = wrCountFiles(fs);
    int n = wrListFiles(fs, wrFiles, WR_LIST_MAX);
    if (n == 0) {
        displayError("No captures to identify", true);
        return;
    }

    int ok = 0;
    for (int i = 0; i < n; i++) {
        progressHandler(i, n, "Identifying " + String(i + 1) + "/" + String(n));
        if (wrIdentifyFile(fs, wrFiles[i].path, nullptr).length()) ok++;
        // The decoders are pure CPU on a few hundred ints; yield so the UI and
        // the console keep running during a long session.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    progressHandler(n, n, "Done");
    // Say so when the folder holds more than one pass can take, instead of
    // reporting "40/40" as if that were everything on the card.
    String more = (total > n) ? String("\n") + String(n) + " of " + String(total) + " files" : String("");
    displayInfo(
        String(ok) + "/" + String(n) + " decoded\n" + String(n - ok) + " kept as raw only" + more, true
    );
}

static void wrPlaybackMenu(FS *fs) {
    // `selected` survives the rebuild so "Identify all" or a delete does not send
    // the cursor back to the top of the list.
    int selected = 0;
    while (true) {
        int n = wrListFiles(fs, wrFiles, WR_LIST_MAX);
        options.clear();
        if (n == 0) {
            displayError("No captures yet", true);
            return;
        }
        if (selected >= n) selected = n - 1;
        for (int i = 0; i < n; i++) {
            // Copy the path: the shared wrFiles[] table is refilled by
            // "Identify all", so a captured index would then point at a
            // different file (replay/identify/delete would hit the wrong one).
            String name = wrFiles[i].name;
            String path = wrFiles[i].path;
            options.push_back({name, [fs, path, name]() {
                                   int sub = 0;
                                   while (true) {
                                       options.clear();
                                       options = {
                                           {"Replay as captured",
                                            [path]() { rfWardriverReplay(path); }              },
                                           {"Identify & save",
                                            [fs, path]() {
                                                String dec;
                                                String proto = wrIdentifyFile(fs, path, &dec);
                                                if (proto.length())
                                                    displayInfo("Identified: " + proto, true);
                                                else displayInfo("Not recognised - kept as raw", true);
                                            }                    },
                                           {"Delete capture",
                                            [fs, path]() {
                                                fs->remove(path);
                                                fs->remove(path.substring(0, path.length() - 4) + "_dec.sub");
                                                displayInfo("Deleted", true);
                                            }                    },
                                           {"Back", []() {}},
                                       };
                                       int sel = loopOptions(options, MENU_TYPE_SUBMENU, name.c_str(), sub);
                                       if (sel == -1 || sel == (int)options.size() - 1) return;
                                       sub = sel;
                                   }
                               }});
        }
        options.push_back({"Identify all", [fs]() { wrIdentifyAll(fs); }});
        options.push_back({"Back", []() {}});

        int sel = loopOptions(options, MENU_TYPE_SUBMENU, "Wardriver Captures", selected);
        if (sel == -1 || sel == (int)options.size() - 1) return;
        selected = sel;
    }
}

// Headless proof that the dedupe + file writer work, without needing a remote
// in range: two identical bursts and one different one through exactly the
// production path, into a scratch folder. Used by `subghz wardriver selftest`.
String rfWardriverSelfTest() {
    FS *fs = nullptr;
    if (!getFsStorage(fs) || fs == nullptr) return "no storage";
    const char *dir = "/bruce/subghz/_wardtest";
    if (!fs->exists("/bruce")) fs->mkdir("/bruce");
    if (!fs->exists("/bruce/subghz")) fs->mkdir("/bruce/subghz");
    if (!fs->exists(dir)) fs->mkdir(dir);

    // A plausible 24-pulse frame: NEC-ish header then 10 bits.
    rmt_symbol_word_t a[12];
    for (int i = 0; i < 12; i++) {
        a[i].level0 = 1;
        a[i].level1 = 0;
        a[i].duration0 = (i == 0) ? 9000 : ((i % 2) ? 560 : 1690);
        a[i].duration1 = (i == 0) ? 4500 : 560;
    }
    rmt_symbol_word_t b[12];
    memcpy(b, a, sizeof(a));
    b[5].duration0 = 1700; // one different bit makes it a different signal

    for (int i = 0; i < WR_DEDUPE_SLOTS; i++) {
        wrHashes[i] = 0;
        wrHashTimes[i] = 0;
    }

    int saved = 0, dup = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t h = wrHashBurst(a, 12);
        if (wrSeenRecently(h)) {
            dup++;
            continue;
        }
        wrRememberHash(h);
        if (wrWriteFile(fs, String(dir) + "/a" + String(pass) + ".sub", a, 12, 433.92f)) saved++;
    }
    uint32_t hb = wrHashBurst(b, 12);
    if (!wrSeenRecently(hb)) {
        wrRememberHash(hb);
        if (wrWriteFile(fs, String(dir) + "/b.sub", b, 12, 433.92f)) saved++;
    }

    int files = 0;
    File d = fs->open(dir);
    if (d && d.isDirectory()) {
        File e = d.openNextFile();
        while (e) {
            files++;
            e = d.openNextFile();
        }
        d.close();
    }
    // Streaming writer: one press that needs three RMT reads (the buffer holds
    // 128 pulses) must land in a single file with every pulse, not three files
    // and not a file cut at the read boundary. 144 symbols => 288 RAW values.
    rmt_symbol_word_t big[64];
    for (int i = 0; i < 64; i++) {
        big[i].level0 = 1;
        big[i].level1 = 0;
        big[i].duration0 = (i % 2) ? 350 : 1050;
        big[i].duration1 = (i % 2) ? 1050 : 350;
    }
    String streamPath = String(dir) + "/s.sub";
    bool streamOk = false;
    WardriverStats sst;
    if (wrStreamBeginAt(fs, 433.92f, streamPath)) {
        wrStream.pulses = 0;
        wrStream.totalNs = 0;
        wrStream.symbolCount = 0;
        for (int read = 0; read < 3; read++) {
            size_t cnt = (read == 2) ? 16 : 64;
            wrStream.pulses += cnt * 2;
            wrStream.symbolCount += cnt;
            wrStreamAppend(big, cnt);
        }
        wrStreamEnd(sst, true);

        uint32_t values = 0;
        File f = fs->open(streamPath, FILE_READ);
        if (f) {
            while (f.available()) {
                String line = f.readStringUntil('\n');
                if (!line.startsWith("RAW_Data:")) continue;
                bool inToken = false;
                for (size_t c = 9; c < line.length(); c++) {
                    if (line[c] == ' ') {
                        inToken = false;
                    } else if (!inToken) {
                        inToken = true;
                        values++;
                    }
                }
            }
            f.close();
        }
        streamOk = (values == 288);
    }
    fs->remove(streamPath);

    // Leave nothing behind on the card.
    fs->remove(String(dir) + "/a0.sub");
    fs->remove(String(dir) + "/a1.sub");
    fs->remove(String(dir) + "/b.sub");
    fs->rmdir(dir);

    // Slicer hash (mostly sub-50 µs edges) must not be saved as a remote, while
    // a normal OOK burst of the same length must be.
    bool noiseOk = !wrLooksLikeRemote(1000, 900) && wrLooksLikeRemote(60, 2);

    bool ok = (saved == 2 && dup == 1 && files == 2 && streamOk && noiseOk);
    String out = String("saved=") + saved + " duplicates=" + dup + " files=" + files +
                 " stream=" + (streamOk ? "ok" : "FAIL") + " noise=" + (noiseOk ? "ok" : "FAIL") +
                 " hash=0x" + String(wrHashBurst(a, 12), HEX) +
                 (ok ? " PASS" : " FAIL");
    Serial.println("[wardriver selftest] " + out);
    return out;
}

// --- console helpers (same code paths as the on-device menus) ----------------

String rfWardriverListFiles() {
    FS *fs = nullptr;
    if (!getFsStorage(fs) || fs == nullptr) return "";
    int n = wrListFiles(fs, wrFiles, WR_LIST_MAX);
    String out;
    for (int i = 0; i < n; i++) {
        out += wrFiles[i].name;
        if (i + 1 < n) out += "\n";
    }
    return out;
}

String rfWardriverIdentify(const String &path) {
    FS *fs = nullptr;
    if (!getFsStorage(fs) || fs == nullptr) return "";
    String p = path;
    if (!p.startsWith("/")) p = String(RF_WARDRIVER_DIR) + "/" + p;
    return wrIdentifyFile(fs, p, nullptr);
}

bool rfWardriverReplay(const String &path) {
    FS *fs = nullptr;
    if (!getFsStorage(fs) || fs == nullptr) return false;
    String p = path;
    if (!p.startsWith("/")) p = String(RF_WARDRIVER_DIR) + "/" + p;

    // Raw captures go back out exactly as recorded: the pulse train is read from
    // RAW_Data and replayed through the RMT TX motor. (The generic .sub path
    // would refuse them - a wardriver file carries no protocol preset, and
    // replaying it through the protocol encoder would re-encode, not replay.)
    std::vector<int> durs;
    float freq = 0;
    bool tooLong = false;
    if (wrReadDurations(fs, p, durs, freq, &tooLong)) {
        if (freq <= 0) freq = bruceConfig.rfWardriverFreq;
        if (!initRfModule("tx", freq)) return false;
        bool ok = true;
        for (int i = 0; i < 3 && ok; i++) ok = rf_tx_durations(durs);
        deinitRfModule();
        return ok;
    }

    if (tooLong) {
        // Deliberately not handed to the generic sender: that path would build
        // the same oversized list from the RAW text and risk the heap anyway.
        displayError("Capture too large to replay", true);
        return false;
    }

    // No RAW_Data: this is one of the decoded copies, so send it as a protocol.
    RfCodes code;
    if (!readSubFile(fs, p, code)) return false;
    return txSubFile(code, true);
}

String rfWardriverIdentifyAllFiles() {
    FS *fs = nullptr;
    if (!getFsStorage(fs) || fs == nullptr) return "no storage";
    int total = wrCountFiles(fs);
    int n = wrListFiles(fs, wrFiles, WR_LIST_MAX);
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (wrIdentifyFile(fs, wrFiles[i].path, nullptr).length()) ok++;
    }
    String out = String(ok) + "/" + String(n);
    if (total > n) out += String(" (of ") + String(total) + " in folder)";
    return out;
}

void rfWardriverMenu() {
    while (true) {
        options.clear();
        options = {
            {"Start capture",
             []() { rfWardriverRun(); }                                                     },
            {"Captures (replay/identify)",
             []() {
                 FS *fs = nullptr;
                 if (!getFsStorage(fs) || fs == nullptr) {
                     displayError("No storage", true);
                     return;
                 }
                 wrPlaybackMenu(fs);
             }                                                                               },
            {"Identify all captures",
             []() {
                 FS *fs = nullptr;
                 if (!getFsStorage(fs) || fs == nullptr) {
                     displayError("No storage", true);
                     return;
                 }
                 wrIdentifyAll(fs);
             }                                                                               },
            {"Capture settings",
             []() { rfWardriverConfigMenu(); }                                              },
            {"Back", []() {}},
        };

        int sel = loopOptions(options, MENU_TYPE_SUBMENU, "SubGHz Wardriver");
        if (sel == -1 || sel == (int)options.size() - 1) return;
    }
}

void rfWardriverConfigMenu() {
    // Keep the cursor where it is: rebuilding the menu without an index sent it
    // back to the top after every toggle (turning hop bands on jumped away from
    // the row that was just changed, so it looked like the setting was lost).
    int selected = 0;
    while (true) {
        options.clear();
        options = {
            {String("Frequency: ") + String(bruceConfig.rfWardriverFreq, 2) + " MHz",
             []() {
                 options.clear();
                 const int n = sizeof(subghz_frequency_list) / sizeof(subghz_frequency_list[0]);
                 int cur = 0;
                 for (int i = 0; i < n; i++) {
                     if (fabsf(subghz_frequency_list[i] - bruceConfig.rfWardriverFreq) < 0.01f) cur = i;
                     options.push_back(
                         {String(subghz_frequency_list[i], 2) + "MHz",
                          [i]() { bruceConfig.setWardriverFreq(subghz_frequency_list[i]); },
                          fabsf(subghz_frequency_list[i] - bruceConfig.rfWardriverFreq) < 0.01f}
                     );
                 }
                 loopOptions(options, MENU_TYPE_SUBMENU, "Wardriver Frequency", cur);
             }                                                                                       },
            {String("Duplicate window: ") + String(bruceConfig.rfWardriverDedupeSec) + " s",
             []() {
                 String v = num_keyboard(String(bruceConfig.rfWardriverDedupeSec), 3, "Dedupe seconds (0=off):");
                 if (v.length() > 0 && v != "\x1B") bruceConfig.setWardriverDedupeSec((uint16_t)v.toInt());
             }                                                                                       },
            {String("Min pulses: ") + String(bruceConfig.rfWardriverMinPulses),
             []() {
                 String v = num_keyboard(String(bruceConfig.rfWardriverMinPulses), 2, "Min pulses per signal:");
                 if (v.length() > 0 && v != "\x1B") bruceConfig.setWardriverMinPulses((uint8_t)v.toInt());
             }                                                                                       },
            {String("Hop bands: ") + (bruceConfig.rfWardriverHop ? "433/315/868/915" : "OFF (fixed)"),
             []() { bruceConfig.setWardriverHop(!bruceConfig.rfWardriverHop); }},
            {"Save folder", []() { displayInfo(RF_WARDRIVER_DIR, true); }},
            {"Back",        []() {}},
        };

        int sel = loopOptions(options, MENU_TYPE_SUBMENU, "Wardriver Config", selected);
        if (sel == -1 || sel == (int)options.size() - 1) return;
        selected = sel;
    }
}

void rfWardriverRun(int maxSeconds) {
    FS *fs = nullptr;
    if (!getFsStorage(fs) || fs == nullptr) {
        displayError("No storage", true);
        return;
    }
    if (!wrEnsureFolder(fs)) {
        displayError("Cannot create wardriver folder", true);
        return;
    }
    if (bruceConfigPins.rfModule != CC1101_SPI_MODULE) {
        // The M5 RF module has a GPIO recorder in rf_raw_record; a continuous
        // wardrive needs the CC1101's RMT path (GDO0 edge capture).
        displayError("Wardriver needs the CC1101", true);
        return;
    }

    wrScanNextIndex(fs);
    for (int i = 0; i < WR_DEDUPE_SLOTS; i++) {
        wrHashes[i] = 0;
        wrHashTimes[i] = 0;
    }
    WardriverStats st;

    // setup_rf_rx() also brings the module up (initRfModule("rx", freq)); it
    // reads the frequency from bruceConfigPins, so point that at the wardriver
    // setting first and restore it afterwards.
    float previousFreq = bruceConfigPins.rfFreq;
    bruceConfigPins.rfFreq = bruceConfig.rfWardriverFreq;

    int rxSymbols = WR_RX_SYMBOLS_MAX;
    rmt_channel_handle_t rxCh = setup_rf_rx(rxSymbols);
    if (rxCh == nullptr) {
        rxSymbols = WR_RX_SYMBOLS_MIN;
        rxCh = setup_rf_rx(rxSymbols);
    }
    if (rxCh == nullptr) {
        bruceConfigPins.rfFreq = previousFreq;
        displayError("RF module not found", true);
        return;
    }
    setMHZ(bruceConfig.rfWardriverFreq);

    QueueHandle_t queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (queue == nullptr) {
        rmt_del_channel(rxCh);
        bruceConfigPins.rfFreq = previousFreq;
        deinitRfModule();
        displayError("No RAM for RX queue", true);
        return;
    }

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = [](rmt_channel_handle_t, const rmt_rx_done_event_data_t *edata, void *user) {
        QueueHandle_t q = (QueueHandle_t)user;
        BaseType_t wake = pdFALSE;
        xQueueSendFromISR(q, edata, &wake);
        return wake == pdTRUE;
    };
    rmt_rx_register_event_callbacks(rxCh, &cbs, queue);
    rmt_enable(rxCh);

    rmt_receive_config_t rxcfg = {};
    rxcfg.signal_range_min_ns = WR_PULSE_MIN_NS;
    rxcfg.signal_range_max_ns = WR_IDLE_MAX_NS;

    // Two buffers: the radio is re-armed on the idle one *before* the card is
    // touched, so the pulses that arrive while a file is being written are not
    // lost. Processing a read while the next one lands in the other buffer is
    // what makes long bursts continuous instead of piecemeal.
    static rmt_symbol_word_t symbols[2][WR_RX_SYMBOLS_MAX];
    int bufIdx = 0;
    const size_t rxBytes = (size_t)rxSymbols * sizeof(rmt_symbol_word_t);
    rmt_rx_done_event_data_t rxData;

    bool full = true;
    bool running = true;
    uint32_t lastDraw = 0;
    uint32_t startedMs = millis();
    uint32_t lastHop = millis();

    // Hop mode starts on the configured band and then walks the list; fixed
    // mode just stays where the settings say.
    int bandIdx = 0;
    for (int i = 0; i < WR_BAND_COUNT; i++) {
        if (fabsf(WR_BANDS[i] - bruceConfig.rfWardriverFreq) < 0.01f) bandIdx = i;
    }
    float curFreq = bruceConfig.rfWardriverHop ? WR_BANDS[bandIdx] : bruceConfig.rfWardriverFreq;

    // The receive result was ignored before: if the channel refused to arm, the
    // screen said "listening..." forever while nothing was captured.
    bool rxFailed = (rmt_receive(rxCh, symbols[bufIdx], rxBytes, &rxcfg) != ESP_OK);
    if (rxFailed) {
        rmt_disable(rxCh);
        rmt_del_channel(rxCh);
        vQueueDelete(queue);
        deinitRfModule();
        bruceConfigPins.rfFreq = previousFreq;
        displayError("RF receiver failed to start", true);
        return;
    }

    // SEL launched this screen and is still held: wait for the release, or the
    // main loop below stops the wardriver on its very first pass.
    while (check(SelPress) || check(EscPress)) vTaskDelay(pdMS_TO_TICKS(10));
    delay(150);

    while (running) {
        if (xQueueReceive(queue, &rxData, pdMS_TO_TICKS(20)) == pdPASS) {
            const rmt_symbol_word_t *burst = rxData.received_symbols;
            size_t n = rxData.num_symbols;
            bool bufferFull = (n >= (size_t)rxSymbols);

            // Re-arm first, on the other buffer: a full buffer means the burst
            // continues, and an SD write here would drop the pulses that arrive
            // while the file is open.
            if (running) {
                bufIdx ^= 1;
                if (rmt_receive(rxCh, symbols[bufIdx], rxBytes, &rxcfg) != ESP_OK) {
                    // Keep the burst that is already on the card, then stop with
                    // a message instead of pretending to listen.
                    rxFailed = true;
                    running = false;
                }
            }

            uint32_t pulses = 0;
            uint32_t shortPulses = 0;
            uint64_t totalNs = 0;
            for (size_t i = 0; i < n; i++) {
                if (burst[i].duration0) {
                    pulses++;
                    totalNs += burst[i].duration0;
                    if (burst[i].duration0 < WR_SHORT_PULSE_US) shortPulses++;
                }
                if (burst[i].duration1) {
                    pulses++;
                    totalNs += burst[i].duration1;
                    if (burst[i].duration1 < WR_SHORT_PULSE_US) shortPulses++;
                }
            }

            if (wrStream.active) {
                // Continuation of the burst that is already being written.
                wrStream.pulses += pulses;
                wrStream.shortPulses += shortPulses;
                wrStream.totalNs += totalNs;
                wrStream.symbolCount += n;
                wrStreamAppend(burst, n);
                if (!bufferFull) {
                    // Short read: the radio saw the idle gap, so the signal is
                    // complete. Too few pulses is still noise, and an exact
                    // repeat of a signal heard moments ago is not stamped twice.
                    if (!wrLooksLikeRemote(wrStream.pulses, wrStream.shortPulses)) {
                        wrStreamEnd(st, false);
                        st.noise++;
                    } else {
                        uint32_t h = wrHashFinish(wrStream.hash, wrStream.symbolCount);
                        if (wrSeenRecently(h)) {
                            wrStreamEnd(st, false);
                            st.duplicates++;
                        } else {
                            // Only a completely written file counts as saved; a
                            // full card must not be reported as a capture (and the
                            // hash is only remembered once the bytes are on it).
                            if (wrStreamEnd(st, true)) {
                                wrRememberHash(h);
                                st.saved++;
                            } else {
                                st.noise++;
                            }
                        }
                    }
                    full = true; // the file name line changed
                }
            } else if (bufferFull) {
                // First read already filled the buffer: a long signal (car
                // keyfob, doorbell with repeats). Start the file and keep
                // appending until a read comes back short.
                if (!wrStreamBegin(fs, curFreq)) {
                    displayError("SD write failed", true);
                    running = false;
                } else {
                    wrStream.pulses = pulses;
                    wrStream.shortPulses = shortPulses;
                    wrStream.totalNs = totalNs;
                    wrStream.symbolCount = n;
                    wrStreamAppend(burst, n);
                }
            } else if (totalNs > 300000ULL && wrLooksLikeRemote(pulses, shortPulses)) {
                uint32_t h = wrHashBurst(burst, n);
                if (wrSeenRecently(h)) {
                    st.duplicates++;
                } else {
                    wrRememberHash(h);
                    st.lastPulses = (uint16_t)pulses;
                    st.lastDurationMs = (uint32_t)(totalNs / 1000000ULL);
                    if (wrSaveBurst(fs, burst, n, st, curFreq)) {
                        st.saved++;
                    } else {
                        displayError("SD write failed", true);
                        running = false;
                    }
                    full = true; // the file name line changed
                }
            } else if (pulses > 0) {
                st.noise++;
            }
        }

        // Never hop in the middle of a burst: the tail would be recorded on a
        // different band than the header says, and replaying the file would
        // then send half of it on the wrong frequency.
        if (bruceConfig.rfWardriverHop && !wrStream.active && millis() - lastHop > WR_HOP_MS) {
            bandIdx = (bandIdx + 1) % WR_BAND_COUNT;
            curFreq = WR_BANDS[bandIdx];
            setMHZ(curFreq);
            lastHop = millis();
            full = true;
        }

        if (full || millis() - lastDraw > 1000) {
            wrDraw(full, st, true, curFreq, bruceConfig.rfWardriverHop);
            full = false;
            lastDraw = millis();
        }

        if (check(SelPress)) running = false;
        if (check(EscPress)) running = false;
        if (maxSeconds > 0 && millis() - startedMs >= (uint32_t)maxSeconds * 1000u) running = false;
    }

    // Stopping (SEL/BACK) can land in the middle of a long signal. Close what
    // was written: it is a real capture up to the moment the user stopped, not
    // a half-written file.
    if (wrStream.active) {
        if (wrLooksLikeRemote(wrStream.pulses, wrStream.shortPulses)) {
            if (wrStreamEnd(st, true)) st.saved++;
            else st.noise++;
        } else {
            wrStreamEnd(st, false);
            st.noise++;
        }
    }

    rmt_disable(rxCh);
    rmt_del_channel(rxCh);
    vQueueDelete(queue);
    deinitRfModule();
    bruceConfigPins.rfFreq = previousFreq;

    if (rxFailed) displayError("RF receiver stopped", true);

    Serial.printf(
        "[wardriver] %u saved, %u duplicates, %u noise in %us (rx %d symbols)\n",
        (unsigned)st.saved,
        (unsigned)st.duplicates,
        (unsigned)st.noise,
        (unsigned)((millis() - startedMs) / 1000),
        rxSymbols
    );
    returnToMenu = true;
}
