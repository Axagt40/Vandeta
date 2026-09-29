#include "nrf_capture.h"

#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/utils.h"
#include "nrf_common.h"

#include <RF24.h>
#include <globals.h>

// ============================================================================
// State
// ============================================================================

static bool nrfCapReady = false;
static NrfCaptureConfig nrfCfg;
static uint8_t nrfCurrChannel = 76;
static uint8_t nrfCurrRate = NRF_RATE_1MBPS;
static uint8_t nrfCurrPower = NRF_PWR_MAX;
static uint16_t nrfStartFreqMhz = 2400;
static uint16_t nrfEndFreqMhz = 2525;
static bool nrfPromiscuous = false;
static bool nrfNormalMode = true;

// Pseudo-promiscuous pipe address: the shortest the chip supports (3 bytes).
static const uint8_t NRF_PROMISC_ADDR[3] = {0xE7, 0xE7, 0xE7};

static NrfCapturedPacket nrfBuf[NRF_CAP_MAX_PACKETS];
static int nrfBufCount = 0;
static int nrfBufDropped = 0;

static rf24_datarate_e nrfRateEnum(uint8_t rate) {
    switch (rate) {
        case NRF_RATE_2MBPS: return RF24_2MBPS;
        case NRF_RATE_250KBPS: return RF24_250KBPS;
        default: return RF24_1MBPS;
    }
}

static rf24_pa_dbm_e nrfPowerEnum(uint8_t power) {
    switch (power) {
        case NRF_PWR_MIN: return RF24_PA_MIN;
        case NRF_PWR_LOW: return RF24_PA_LOW;
        case NRF_PWR_HIGH: return RF24_PA_HIGH;
        default: return RF24_PA_MAX;
    }
}

static uint8_t nrfChannelForFreq(uint16_t mhz) {
    if (mhz < 2400) mhz = 2400;
    if (mhz > 2525) mhz = 2525;
    return (uint8_t)(mhz - 2400);
}

// ============================================================================
// Lifecycle
// ============================================================================

bool nrfCaptureReady() { return nrfCapReady; }

bool nrfCaptureInit(const NrfCaptureConfig &cfg) {
    nrfCfg = cfg;
    if (!nrf_start(NRF_MODE_SPI)) {
        nrfCapReady = false;
        return false;
    }
    nrfCapReady = NRFradio.isChipConnected();
    if (!nrfCapReady) return false;

    nrfCurrChannel = cfg.startChannel;
    nrfCurrRate = cfg.dataRate;
    nrfCurrPower = cfg.power;

    NRFradio.setDataRate(nrfRateEnum(cfg.dataRate));
    NRFradio.setPALevel(nrfPowerEnum(cfg.power));
    NRFradio.setChannel(cfg.startChannel);

    nrfPromiscuous = false;
    nrfNormalMode = true;
    return true;
}

void nrfCaptureRestoreNormal() {
    if (!nrfCapReady) return;
    NRFradio.stopListening();
    NRFradio.setAddressWidth(5);
    NRFradio.setAutoAck(true);
    NRFradio.setCRCLength(RF24_CRC_16);
    NRFradio.disableDynamicPayloads();
    NRFradio.setPayloadSize(NRF_CAP_MAX_PAYLOAD);
    nrfPromiscuous = false;
    nrfNormalMode = true;
}

uint8_t nrfCaptureGetChannel() { return nrfCurrChannel; }
uint8_t nrfCaptureGetDataRate() { return nrfCurrRate; }

void nrfCaptureSetChannel(uint8_t channel) {
    if (channel > 125) channel = 125;
    nrfCurrChannel = channel;
    if (nrfCapReady) NRFradio.setChannel(channel);
}

void nrfCaptureSetDataRate(uint8_t rate) {
    if (rate > NRF_RATE_250KBPS) rate = NRF_RATE_250KBPS;
    nrfCurrRate = rate;
    if (nrfCapReady) NRFradio.setDataRate(nrfRateEnum(rate));
}

void nrfCaptureSetPower(uint8_t level) {
    if (level > NRF_PWR_MAX) level = NRF_PWR_MAX;
    nrfCurrPower = level;
    if (nrfCapReady) NRFradio.setPALevel(nrfPowerEnum(level));
}

void nrfCaptureSetAddress(const uint8_t *addr, uint8_t len) {
    if (!nrfCapReady || addr == nullptr) return;
    if (len < 3 || len > 5) len = 5;
    NRFradio.stopListening();
    NRFradio.openReadingPipe(1, addr);
    NRFradio.openWritingPipe(addr);
    NRFradio.startListening();
}

void nrfCaptureSetFrequencyRange(uint16_t startMhz, uint16_t endMhz) {
    if (startMhz < 2400) startMhz = 2400;
    if (endMhz > 2525) endMhz = 2525;
    if (endMhz < startMhz) {
        uint16_t t = startMhz;
        startMhz = endMhz;
        endMhz = t;
    }
    nrfStartFreqMhz = startMhz;
    nrfEndFreqMhz = endMhz;
    nrfCfg.startChannel = nrfChannelForFreq(startMhz);
    nrfCfg.endChannel = nrfChannelForFreq(endMhz);
}

uint16_t nrfCaptureStartFreq() { return nrfStartFreqMhz; }
uint16_t nrfCaptureEndFreq() { return nrfEndFreqMhz; }

bool nrfCaptureIsPromiscuous() { return nrfPromiscuous; }

bool nrfCaptureSetPromiscuous(bool enable) {
    if (!nrfCapReady) return false;
    if (enable) {
        // Pseudo-promiscuous receive: CRC off, minimum address width.
        NRFradio.stopListening();
        NRFradio.setAutoAck(false);
        NRFradio.disableCRC();
        NRFradio.setAddressWidth(3);
        NRFradio.setDataRate(nrfRateEnum(nrfCurrRate));
        NRFradio.enableDynamicPayloads();
        NRFradio.openReadingPipe(1, NRF_PROMISC_ADDR);
        NRFradio.flush_rx();
        NRFradio.startListening();
        nrfPromiscuous = true;
        nrfNormalMode = false;
    } else {
        nrfCaptureRestoreNormal();
    }
    return true;
}

// ============================================================================
// Capture
// ============================================================================

void nrfCaptureClear() {
    nrfBufCount = 0;
    nrfBufDropped = 0;
}

int nrfCaptureCount() { return nrfBufCount; }
int nrfCaptureDropped() { return nrfBufDropped; }

int nrfCaptureLoad(const NrfCapturedPacket *pkts, int count) {
    nrfCaptureClear();
    if (pkts == nullptr) return 0;
    for (int i = 0; i < count && nrfBufCount < NRF_CAP_MAX_PACKETS; i++) nrfBuf[nrfBufCount++] = pkts[i];
    return nrfBufCount;
}

bool nrfCapturePacket(int index, NrfCapturedPacket *out) {
    if (index < 0 || index >= nrfBufCount || out == nullptr) return false;
    *out = nrfBuf[index];
    return true;
}

static void nrfStorePacket(const uint8_t *data, uint8_t len, uint8_t channel) {
    if (nrfBufCount >= NRF_CAP_MAX_PACKETS) {
        nrfBufDropped++;
        return;
    }
    NrfCapturedPacket &p = nrfBuf[nrfBufCount++];
    p.timestamp = millis();
    p.channel = channel;
    p.freqMhz = (uint16_t)(2400 + channel);
    p.len = len > NRF_CAP_MAX_PAYLOAD ? NRF_CAP_MAX_PAYLOAD : (len == 0 ? 1 : len);
    memset(p.data, 0, sizeof(p.data));
    memcpy(p.data, data, p.len);
}

static uint8_t nrfReadOne(uint8_t *buf) {
    uint8_t len = 0;
    if (!NRFradio.available()) return 0;
    if (!nrfNormalMode) {
        len = NRFradio.getDynamicPayloadSize();
        if (len == 0 || len > NRF_CAP_MAX_PAYLOAD) len = NRF_CAP_MAX_PAYLOAD;
    } else {
        len = NRF_CAP_MAX_PAYLOAD;
    }
    memset(buf, 0, NRF_CAP_MAX_PAYLOAD);
    NRFradio.read(buf, len);
    return len;
}

// Common loop for both fixed-channel and hopping captures.
static int nrfCaptureLoop(
    uint32_t durationMs, const uint8_t *channels, int channelCount, uint32_t hopIntervalMs, bool hop,
    const NrfCaptureProgressFn &progress
) {
    if (!nrfCapReady) return 0;
    if (durationMs == 0) durationMs = 1000;
    if (durationMs > 300000) durationMs = 300000;
    if (hopIntervalMs < 5) hopIntervalMs = 5;

    // Capture always needs the promiscuous receive configuration.
    nrfCaptureSetPromiscuous(true);
    NRFradio.flush_rx();

    // The sweep must not move the user's configured channel: a capture that
    // hops needs its own cursor, and the radio is put back on nrfCurrChannel
    // before returning. Otherwise `scan`/`capture` in sequence listens on the
    // wrong frequency (observed as a capture of 0 packets right after a scan).
    uint8_t entryChannel = nrfCurrChannel;
    uint8_t hopChannel = nrfCurrChannel;

    uint32_t start = millis();
    uint32_t lastHop = start;
    uint32_t lastProgress = start;
    int hopIndex = 0;
    uint8_t buf[NRF_CAP_MAX_PAYLOAD];

    if (channelCount > 0) NRFradio.setChannel(channels[0]);
    else NRFradio.setChannel(nrfCurrChannel);

    while ((int32_t)(start + durationMs - millis()) > 0) {
        uint8_t len = nrfReadOne(buf);
        if (len > 0) {
            uint8_t ch = channels && channelCount > 0 ? channels[hopIndex % channelCount] : hopChannel;
            nrfStorePacket(buf, len, ch);
        }

        if (hop && (millis() - lastHop) >= hopIntervalMs) {
            lastHop = millis();
            if (channels && channelCount > 0) {
                hopIndex = (hopIndex + 1) % channelCount;
                NRFradio.setChannel(channels[hopIndex]);
            } else {
                hopChannel++;
                if (hopChannel > nrfCfg.endChannel || hopChannel < nrfCfg.startChannel)
                    hopChannel = nrfCfg.startChannel;
                NRFradio.setChannel(hopChannel);
            }
        }

        // Report to the on-device screen (if any) at a rate that costs the
        // sampling loop almost nothing. A false return is an explicit abort.
        if (progress && (millis() - lastProgress) >= 100) {
            lastProgress = millis();
            NrfCaptureProgress p;
            p.elapsedMs = millis() - start;
            p.totalMs = durationMs;
            p.units = nrfBufCount;
            p.scanning = false;
            if (!progress(p)) break;
        }
        vTaskDelay(1);
    }

    nrfCaptureSetChannel(entryChannel);
    return nrfBufCount;
}

int nrfCaptureRun(uint32_t durationMs, bool forceHop, uint32_t hopIntervalMs, const NrfCaptureProgressFn &progress) {
    if (!nrfCapReady) return 0;
    nrfCaptureClear();
    bool hop = forceHop || nrfCfg.hop;
    return nrfCaptureLoop(durationMs, nullptr, 0, hopIntervalMs, hop, progress);
}

int nrfCaptureChannelSummary(uint8_t *channelsOut, uint8_t *countsOut, int maxOut) {
    int n = 0;
    for (int i = 0; i < nrfBufCount && n < maxOut; i++) {
        int found = -1;
        for (int j = 0; j < n; j++) {
            if (channelsOut[j] == nrfBuf[i].channel) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            if (countsOut[found] < 255) countsOut[found]++;
        } else {
            channelsOut[n] = nrfBuf[i].channel;
            countsOut[n] = 1;
            n++;
        }
    }
    // Sort by count, descending (simple insertion sort, n is tiny).
    for (int i = 1; i < n; i++) {
        uint8_t c = channelsOut[i], k = countsOut[i];
        int j = i - 1;
        while (j >= 0 && countsOut[j] < k) {
            channelsOut[j + 1] = channelsOut[j];
            countsOut[j + 1] = countsOut[j];
            j--;
        }
        channelsOut[j + 1] = c;
        countsOut[j + 1] = k;
    }
    return n;
}

int nrfFilterMultiFreqChannels(uint8_t *channelsOut, int maxOut) {
    if (channelsOut == nullptr || maxOut <= 0) return 0;
    uint8_t chans[NRF_CAP_MAX_PACKETS];
    uint8_t counts[NRF_CAP_MAX_PACKETS];
    int n = nrfCaptureChannelSummary(chans, counts, maxOut);
    int written = 0;
    // Only channels that actually carried more than one packet count as
    // "carrying a signal" rather than noise.
    for (int i = 0; i < n && written < maxOut; i++) {
        if (counts[i] >= 2) channelsOut[written++] = chans[i];
    }
    return written;
}

// ============================================================================
// Replay
// ============================================================================

bool nrfReplayRaw(const uint8_t *data, uint8_t len, uint8_t channel, uint8_t repeat, uint8_t rate) {
    if (!nrfCapReady || data == nullptr || len == 0) return false;
    if (len > NRF_CAP_MAX_PAYLOAD) len = NRF_CAP_MAX_PAYLOAD;
    if (channel > 125) channel = 125;
    if (repeat == 0) repeat = 1;
    if (repeat > 64) repeat = 64;

    // Transmit with the same framing the capture used: CRC off keeps the 32-byte
    // payload clocked out even when it does not look like a valid nRF24 frame,
    // which is what an RF remote's decoder sees.
    NRFradio.stopListening();
    NRFradio.setAutoAck(false);
    NRFradio.disableCRC();
    NRFradio.setPayloadSize(NRF_CAP_MAX_PAYLOAD);
    NRFradio.setDataRate(nrfRateEnum(rate));
    NRFradio.setPALevel(nrfPowerEnum(nrfCurrPower));
    NRFradio.setChannel(channel);
    NRFradio.openWritingPipe(NRF_PROMISC_ADDR);
    NRFradio.flush_tx();

    uint8_t buf[NRF_CAP_MAX_PAYLOAD];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, data, len);

    bool any = false;
    for (uint8_t i = 0; i < repeat; i++) {
        if (NRFradio.writeFast(buf, NRF_CAP_MAX_PAYLOAD)) any = true;
        vTaskDelay(1);
    }
    NRFradio.txStandBy();
    return any;
}

bool nrfReplayMultiFreq(
    const uint8_t *data, uint8_t len, const uint8_t *channels, int channelCount, uint8_t repeat
) {
    if (channels == nullptr || channelCount <= 0) return false;
    bool ok = false;
    for (int i = 0; i < channelCount; i++) {
        if (nrfReplayRaw(data, len, channels[i], repeat, nrfCurrRate)) ok = true;
    }
    return ok;
}

// ============================================================================
// Channel scanning
// ============================================================================

int nrfScanChannelActivity(
    uint8_t startCh, uint8_t endCh, uint32_t dwellMs, NrfChannelActivity *out, int maxOut,
    const NrfCaptureProgressFn &progress
) {
    if (!nrfCapReady || out == nullptr || maxOut <= 0) return 0;
    if (endCh > 125) endCh = 125;
    if (startCh > endCh) {
        uint8_t t = startCh;
        startCh = endCh;
        endCh = t;
    }
    if (dwellMs < 1) dwellMs = 1;

    // Receive mode with CRC off + minimum address filtering: both the carrier
    // (testCarrier) and the received-power detector (testRPD) are then a
    // reasonable proxy for "energy on this channel".
    nrfCaptureSetPromiscuous(true);

    // A scan visits every channel; put the radio back where the caller had it
    // so a capture started right after the scan listens on the right frequency.
    uint8_t entryChannel = nrfCurrChannel;

    int written = 0;
    const int samples = 200;
    int channelTotal = (int)endCh - (int)startCh + 1;
    uint32_t start = millis();
    for (uint8_t ch = startCh; ch <= endCh && written < maxOut; ch++) {
        NRFradio.setChannel(ch);
        NRFradio.flush_rx();
        int hits = 0;
        for (int i = 0; i < samples; i++) {
            if (NRFradio.testRPD() || NRFradio.testCarrier()) hits++;
            delayMicroseconds(dwellMs * 10);
        }
        out[written].channel = ch;
        out[written].freqMhz = (uint16_t)(2400 + ch);
        out[written].level = (uint8_t)((hits * 100) / samples);
        written++;

        if (progress) {
            NrfCaptureProgress p;
            p.elapsedMs = millis() - start;
            p.totalMs = 0; // duration unknown up front: the scan is counted in channels
            p.units = written;
            p.scanning = true;
            if (!progress(p)) break;
        }
    }
    nrfCaptureSetChannel(entryChannel);
    return written;
}

int nrfFindBusyChannels(
    uint8_t startCh, uint8_t endCh, uint32_t dwellMs, uint8_t thresholdPct, uint8_t *out, int maxOut
) {
    NrfChannelActivity act[126];
    int n = nrfScanChannelActivity(startCh, endCh, dwellMs, act, 126);
    int written = 0;
    // Report most active first.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n && written < maxOut; i++) {
            if (act[i].level < thresholdPct) continue;
            bool dup = false;
            for (int j = 0; j < written; j++)
                if (out[j] == act[i].channel) dup = true;
            if (dup) continue;
            if (pass == 0) {
                // first pass writes in level order
                int best = i;
                for (int j = i; j < n; j++)
                    if (act[j].level > act[best].level && act[j].level >= thresholdPct) best = j;
                if (act[best].level < thresholdPct) continue;
                out[written++] = act[best].channel;
                act[best].level = 0;
            }
        }
        if (written > 0) break;
    }
    return written;
}

// ============================================================================
// Files
// ============================================================================

const char *nrfCaptureFolder() { return "/BruceNRF24"; }

static FS *nrfFs() {
    FS *fs = setupSdCard() ? (FS *)&SD : (FS *)&LittleFS;
    return fs;
}

static String nrfEnsureFolder(FS *fs) {
    String folder = nrfCaptureFolder();
    if (fs && !fs->exists(folder)) fs->mkdir(folder);
    return folder;
}

static String nrfNormalizePath(const String &path, const char *defExt = ".nrf") {
    String p = path;
    p.trim();
    if (p.length() == 0) return p;
    if (!p.startsWith("/")) p = String(nrfCaptureFolder()) + "/" + p;
    if (p.indexOf('.') == -1) p += defExt;
    return p;
}

bool nrfSaveSignal(const String &path, const NrfCapturedPacket *pkts, int count, const char *label) {
    FS *fs = nrfFs();
    if (fs == nullptr) return false;
    nrfEnsureFolder(fs);
    String full = nrfNormalizePath(path);
    if (full.length() == 0) return false;

    File f = fs->open(full, FILE_WRITE);
    if (!f) return false;
    f.println("# Bruce NRF24 signal v1");
    f.println(String("label=") + (label ? label : ""));
    f.println(String("count=") + String(count));
    for (int i = 0; i < count; i++) {
        String hex;
        for (int b = 0; b < pkts[i].len; b++) {
            char h[3];
            snprintf(h, sizeof(h), "%02X", pkts[i].data[b]);
            hex += h;
        }
        f.println(
            String("pkt ch=") + String(pkts[i].channel) + " freq=" + String(pkts[i].freqMhz) +
            " ts=" + String(pkts[i].timestamp) + " len=" + String(pkts[i].len) + " data=" + hex
        );
    }
    f.close();
    return true;
}

int nrfLoadSignal(const String &path, NrfCapturedPacket *out, int maxOut, String *labelOut) {
    FS *fs = nrfFs();
    if (fs == nullptr || out == nullptr || maxOut <= 0) return 0;
    String full = nrfNormalizePath(path);
    if (!fs->exists(full)) return 0;

    File f = fs->open(full, FILE_READ);
    if (!f) return 0;

    int n = 0;
    while (f.available() && n < maxOut) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        if (line.startsWith("label=") && labelOut) {
            *labelOut = line.substring(6);
            continue;
        }
        if (!line.startsWith("pkt ")) continue;

        NrfCapturedPacket p;
        memset(&p, 0, sizeof(p));
        int ch = 0, len = 0;
        unsigned long ts = 0, freq = 0;
        char hex[2 * NRF_CAP_MAX_PAYLOAD + 4] = {0};
        // pkt ch=N freq=N ts=N len=N data=HEX
        sscanf(
            line.c_str(), "pkt ch=%d freq=%lu ts=%lu len=%d data=%128s", &ch, &freq, &ts, &len, hex
        );
        if (len <= 0 || len > NRF_CAP_MAX_PAYLOAD) continue;
        p.channel = (uint8_t)ch;
        p.freqMhz = (uint16_t)freq;
        p.timestamp = (uint32_t)ts;
        p.len = (uint8_t)len;
        for (int i = 0; i < len; i++) {
            char b[3] = {hex[i * 2], hex[i * 2 + 1], 0};
            if (hex[i * 2] == 0 || hex[i * 2 + 1] == 0) break;
            p.data[i] = (uint8_t)strtol(b, nullptr, 16);
        }
        out[n++] = p;
    }
    f.close();
    return n;
}

// ============================================================================
// Analysis
// ============================================================================

NrfSignalAnalysis nrfAnalyzeSignal(const uint8_t *data, uint8_t len) {
    NrfSignalAnalysis a;
    if (data == nullptr || len == 0) return a;
    if (len > NRF_CAP_MAX_PAYLOAD) len = NRF_CAP_MAX_PAYLOAD;
    a.len = len;
    a.minByte = 0xFF;
    a.maxByte = 0x00;

    bool seen[256] = {false};
    int transitions = 0;
    int prevBit = -1;
    for (int i = 0; i < len; i++) {
        if (data[i] < a.minByte) a.minByte = data[i];
        if (data[i] > a.maxByte) a.maxByte = data[i];
        seen[data[i]] = true;
        for (int b = 7; b >= 0; b--) {
            int bit = (data[i] >> b) & 1;
            if (prevBit >= 0 && bit != prevBit) transitions++;
            prevBit = bit;
        }
    }
    for (int i = 0; i < 256; i++)
        if (seen[i]) a.uniqueBytes++;
    a.bitTransitions = transitions;

    // Repeat detection: is the payload a whole number of copies of a shorter code?
    for (int codeLen = 1; codeLen <= len / 2; codeLen++) {
        if (len % codeLen != 0) continue;
        bool match = true;
        for (int i = codeLen; i < len && match; i++) {
            if (data[i] != data[i % codeLen]) match = false;
        }
        if (match) {
            a.repeats = true;
            a.repeatLength = codeLen;
            break;
        }
    }

    if (a.repeats && a.repeatLength <= 4) a.kind = "EV1527";
    else if (a.repeats && a.repeatLength <= 8) a.kind = "PT2262";
    else if (a.repeats) a.kind = "repeated";
    else a.kind = "raw";
    return a;
}

int nrfChooseReplayChannels(const NrfCapturedPacket *pkts, int count, uint8_t *channelsOut, int maxOut) {
    if (pkts == nullptr || channelsOut == nullptr || maxOut <= 0) return 0;
    uint8_t chans[NRF_CAP_MAX_PACKETS];
    uint8_t counts[NRF_CAP_MAX_PACKETS];
    int n = 0;
    for (int i = 0; i < count; i++) {
        int found = -1;
        for (int j = 0; j < n; j++)
            if (chans[j] == pkts[i].channel) found = j;
        if (found >= 0) {
            if (counts[found] < 255) counts[found]++;
        } else {
            chans[n] = pkts[i].channel;
            counts[n] = 1;
            n++;
        }
    }
    for (int i = 1; i < n; i++) {
        uint8_t c = chans[i], k = counts[i];
        int j = i - 1;
        while (j >= 0 && counts[j] < k) {
            chans[j + 1] = chans[j];
            counts[j + 1] = counts[j];
            j--;
        }
        chans[j + 1] = c;
        counts[j + 1] = k;
    }
    int written = 0;
    for (int i = 0; i < n && written < maxOut; i++) {
        if (i < 2 || counts[i] == counts[0]) channelsOut[written++] = chans[i];
    }
    return written;
}

// ============================================================================
// Shared menu helpers
// ============================================================================

static String nrfHexOf(const uint8_t *data, uint8_t len) {
    String hex;
    for (int i = 0; i < len; i++) {
        char h[3];
        snprintf(h, sizeof(h), "%02X", data[i]);
        hex += h;
    }
    return hex;
}

/// Drop key presses that arrived while a process was running. Without this a
/// press that dismisses a result screen also fires on the menu that is rebuilt
/// behind it (SEL dismissing "Packets: 12" would immediately re-run whatever
/// entry is highlighted next).
static void nrfClearPendingKeys() {
    EscPress = false;
    SelPress = false;
    PrevPress = false;
    NextPress = false;
    UpPress = false;
    DownPress = false;
    AnyKeyPress = false;
}

/// Stock-style "dismiss the result screen" wait, hardened against stale presses.
static void nrfWaitAnyKey() {
    nrfClearPendingKeys();
    while (!check(AnyKeyPress)) vTaskDelay(pdMS_TO_TICKS(20));
    nrfClearPendingKeys();
}

/// Live screen for a running capture / channel scan. The counter is repainted a
/// few times a second, so the process looks alive instead of frozen, and the
/// "Press BACK to stop" hint is literally true: only Back aborts, every other
/// press is ignored on purpose.
struct NrfRunScreen {
    String title;
    uint32_t lastPaint = 0;
    bool aborted = false;

    void begin(const String &titleText, const String &subLine) {
        title = titleText;
        drawMainBorderWithTitle(title);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        if (subLine.length()) tft.drawString(subLine.substring(0, 34), 10, 44);
        tft.drawString("Press BACK to stop", 10, tftHeight - 20);
        nrfClearPendingKeys();
        lastPaint = millis();
    }

    /// Progress sink for nrfCaptureRun() / nrfScanChannelActivity().
    bool update(const NrfCaptureProgress &p) {
        if (check(EscPress)) {
            aborted = true;
            return false;
        }
        uint32_t now = millis();
        if (now - lastPaint < 150) return true;
        lastPaint = now;
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.fillRect(8, 66, tftWidth - 16, 32, bruceConfig.bgColor);
        if (p.scanning) {
            tft.drawString(String("Scanned: ") + String(p.units) + " ch", 10, 66);
            tft.drawString("2400-2525MHz", 10, 82);
        } else {
            tft.drawString(String("Packets: ") + String(p.units), 10, 66);
            uint32_t left = p.totalMs > p.elapsedMs ? (p.totalMs - p.elapsedMs + 999) / 1000 : 0;
            tft.drawString(String("Left: ") + String(left) + "s", 10, 82);
        }
        return true;
    }

    /// Result screen for when the process finished (or was stopped).
    void finish(const String *lines, int count) {
        drawMainBorderWithTitle(title);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        int y = 44;
        if (aborted) {
            tft.drawString("Stopped", 10, y);
            y += 18;
        }
        for (int i = 0; i < count && y < tftHeight - 26; i++) {
            tft.drawString(lines[i].substring(0, 34), 10, y);
            y += 15;
        }
        tft.drawString("Press any key", 10, tftHeight - 20);
        nrfWaitAnyKey();
    }
};

static void nrfShowLines(const String &title, const String *lines, int count) {
    drawMainBorderWithTitle(title);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    int y = 42;
    for (int i = 0; i < count && y < tftHeight - 26; i++) {
        tft.drawString(lines[i].substring(0, 40), 8, y);
        y += 15;
    }
    tft.drawString("Press any key", 8, tftHeight - 20);
    nrfWaitAnyKey();
}

static void nrfDrawFrequencyAnalysis(const String &title) {
    uint8_t chans[16];
    uint8_t counts[16];
    int n = nrfCaptureChannelSummary(chans, counts, 16);
    drawMainBorderWithTitle(title);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    if (n == 0) {
        tft.drawString("No packets captured", 8, 48);
    } else {
        int maxCount = 1;
        for (int i = 0; i < n; i++)
            if (counts[i] > maxCount) maxCount = counts[i];
        int barX = 96;
        int barW = tft.width() - barX - 12;
        if (barW < 20) barW = 20;
        int y = 40;
        for (int i = 0; i < n && i < 6; i++) {
            tft.drawString(String(2400 + chans[i]) + "MHz x" + String(counts[i]), 8, y);
            int w = (int)((int32_t)counts[i] * barW / maxCount);
            if (w < 1) w = 1;
            tft.fillRect(barX, y + 2, w, 8, bruceConfig.priColor);
            y += 15;
        }
    }
    tft.drawString("Press any key", 8, tftHeight - 20);
    nrfWaitAnyKey();
}

// ============================================================================
// Raw capture menu
// ============================================================================

static void nrfEnsureInit() {
    if (nrfCaptureReady()) return;
    NrfCaptureConfig cfg;
    nrfCaptureInit(cfg);
}

static const char *nrfRateName(uint8_t r) {
    switch (r) {
        case NRF_RATE_2MBPS: return "2M";
        case NRF_RATE_250KBPS: return "250k";
        default: return "1M";
    }
}

static void nrfMenuCapture(uint32_t ms, bool hop) {
    NrfRunScreen screen;
    screen.begin(
        "NRF Capture",
        hop ? String("Hopping 2400-2525MHz")
            : String("Channel ") + String(2400 + nrfCaptureGetChannel()) + "MHz"
    );
    nrfCaptureRun(ms, hop, 20, [&screen](const NrfCaptureProgress &p) { return screen.update(p); });

    String lines[3];
    lines[0] = String("Packets: ") + String(nrfCaptureCount()) + " / dropped " + String(nrfCaptureDropped());
    uint8_t chans[4];
    uint8_t counts[4];
    int m = nrfCaptureChannelSummary(chans, counts, 4);
    if (m > 0) lines[1] = String("Top: ") + String(2400 + chans[0]) + "MHz x" + String(counts[0]);
    else lines[1] = "No activity";
    lines[2] = hop ? String("Hopped 0-125") : String("Ch ") + String(2400 + nrfCaptureGetChannel()) + "MHz";
    screen.finish(lines, 3);
}

static void nrfMenuSaveCapture() {
    if (nrfCaptureCount() == 0) {
        displayError("Nothing captured", true);
        return;
    }
    String name = keyboard("capture", 20, "Signal name:");
    if (name.length() == 0 || name == "\x1B") return;
    NrfCapturedPacket pkts[NRF_CAP_MAX_PACKETS];
    int got = 0;
    for (int i = 0; i < nrfCaptureCount() && got < NRF_CAP_MAX_PACKETS; i++)
        if (nrfCapturePacket(i, &pkts[got])) got++;
    if (nrfSaveSignal(name, pkts, got, "nrf24 menu")) displayInfo("Saved " + name, true);
    else displayError("Save failed", true);
}

static void nrfMenuReplayCapture() {
    if (nrfCaptureCount() == 0) {
        displayError("Nothing captured", true);
        return;
    }
    uint8_t chans[4];
    NrfCapturedPacket all[NRF_CAP_MAX_PACKETS];
    int got = 0;
    for (int i = 0; i < nrfCaptureCount() && got < NRF_CAP_MAX_PACKETS; i++)
        if (nrfCapturePacket(i, &all[got])) got++;
    int nc = nrfChooseReplayChannels(all, got, chans, 4);
    if (nc == 0) {
        displayError("No channel to replay on", true);
        return;
    }

    NrfRunScreen screen;
    screen.begin("Replay", String(nc) + " freq(s) x5 repeats");
    bool ok = false;
    int sent = 0;
    for (int c = 0; c < nc && !screen.aborted; c++) {
        for (int i = 0; i < got; i++) {
            if (all[i].channel != chans[c]) continue;
            if (check(EscPress)) {
                screen.aborted = true;
                break;
            }
            if (nrfReplayRaw(all[i].data, all[i].len, chans[c], 5, nrfCaptureGetDataRate())) ok = true;
            sent++;
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.fillRect(8, 66, tftWidth - 16, 16, bruceConfig.bgColor);
            tft.drawString(String("Sent: ") + String(sent), 10, 66);
            break;
        }
    }

    String lines[2];
    lines[0] = String("Sent ") + String(sent) + " on " + String(nc) + " freq(s)";
    lines[1] = ok ? String("Rate: ") + nrfRateName(nrfCaptureGetDataRate()) : String("Replay failed");
    screen.finish(lines, 2);
}

static void nrfMenuAnalyze() {
    if (nrfCaptureCount() == 0) {
        displayError("Nothing captured", true);
        return;
    }
    NrfCapturedPacket p;
    if (!nrfCapturePacket(0, &p)) return;
    NrfSignalAnalysis a = nrfAnalyzeSignal(p.data, p.len);
    String lines[5];
    lines[0] = String("len=") + String(a.len) + " unique=" + String(a.uniqueBytes);
    lines[1] = String("transitions=") + String(a.bitTransitions);
    lines[2] = String("kind=") + a.kind;
    lines[3] = String("repeats=") + (a.repeats ? "yes" : "no") + " code=" + String(a.repeatLength);
    lines[4] = nrfHexOf(p.data, p.len).substring(0, 32);
    nrfShowLines("Analyze", lines, 5);
}

void nrfRawCaptureMenu() {
    nrfEnsureInit();
AGAIN:
    options = {
        {String("Capture 3s ch") + String(nrfCaptureGetChannel()),
         []() { nrfMenuCapture(3000, false); }                                                   },
        {"Capture hop 0-125",
         []() { nrfMenuCapture(3000, true); }                                                    },
        {"Channel scan",
         []() {
             if (!nrfCaptureReady()) {
                 displayError("NRF24 not found", true);
                 return;
             }
             NrfRunScreen screen;
             screen.begin("Channel Scan", "2400-2525MHz, 126 channels");
             NrfChannelActivity act[126];
             int n = nrfScanChannelActivity(0, 125, 2, act, 126, [&screen](const NrfCaptureProgress &p) {
                 return screen.update(p);
             });
             String lines[5];
             int written = 0;
             for (int i = 0; i < n && written < 5; i++) {
                 if (act[i].level == 0) continue;
                 lines[written++] = String(act[i].freqMhz) + "MHz lvl=" + String(act[i].level);
             }
             if (written == 0) lines[written++] = "No carrier detected";
             screen.finish(lines, written);
         }                                                                                       },
        {"Multi-freq detect",
         []() {
             NrfRunScreen screen;
             screen.begin("Multi-Freq", "Capturing 4s, hopping all channels");
             nrfCaptureRun(4000, true, 20, [&screen](const NrfCaptureProgress &p) {
                 return screen.update(p);
             });
             uint8_t chans[8];
             int n = nrfFilterMultiFreqChannels(chans, 8);
             String lines[4];
             lines[0] = String("Signals: ") + String(n);
             lines[1] = n >= 2 ? String("Dual-frequency: YES") : String("Dual-frequency: no");
             for (int i = 0; i < n && i + 2 < 4; i++) lines[i + 2] = String(2400 + chans[i]) + "MHz";
             screen.finish(lines, n + 2 > 4 ? 4 : n + 2);
         }                                                                                       },
        {"Frequency analysis", []() { nrfDrawFrequencyAnalysis("Freq Analysis"); }               },
        {"List capture",
         []() {
             String lines[6];
             int written = 0;
             for (int i = 0; i < nrfCaptureCount() && written < 6; i++) {
                 NrfCapturedPacket p;
                 if (!nrfCapturePacket(i, &p)) continue;
                 lines[written++] = String(2400 + p.channel) + "M len" + String(p.len) + " " +
                                    nrfHexOf(p.data, p.len).substring(0, 16);
             }
             if (written == 0) lines[written++] = "Empty";
             nrfShowLines("Capture", lines, written);
         }                                                                                       },
        {"Save capture", []() { nrfMenuSaveCapture(); }                                          },
        {"Replay capture", []() { nrfMenuReplayCapture(); }                                      },
        {"Analyze packet", []() { nrfMenuAnalyze(); }                                            },
        {"Frequency range",
         []() {
             String s = num_keyboard(String(nrfCaptureStartFreq()), 4, "Start MHz:");
             if (s.length() == 0 || s == "\x1B") return;
             String e = num_keyboard(String(nrfCaptureEndFreq()), 4, "End MHz:");
             if (e.length() == 0 || e == "\x1B") return;
             nrfCaptureSetFrequencyRange((uint16_t)s.toInt(), (uint16_t)e.toInt());
         }                                                                                       },
        {String("Promiscuous: ") + (nrfCaptureIsPromiscuous() ? "ON" : "OFF"),
         []() { nrfCaptureSetPromiscuous(!nrfCaptureIsPromiscuous()); }                          },
        {String("Rate: ") + nrfRateName(nrfCaptureGetDataRate()),
         []() { nrfCaptureSetDataRate((nrfCaptureGetDataRate() + 1) % 3); }                      },
    };
    addOptionToMainMenu();
    int selected = loopOptions(options, MENU_TYPE_SUBMENU, "NRF24 Capture");
    // A negative index is the physical Back button: leave this page instead of
    // rebuilding it, which used to look like "Back reloads the current menu".
    if (selected < 0) return;
    // "Main Menu" (pushed by addOptionToMainMenu) bubbles all the way out.
    if (returnToMenu) return;
    // Any other entry: rebuild, so the Rate/Promiscuous labels stay accurate.
    goto AGAIN;
}
