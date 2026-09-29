#ifndef __NRF_CAPTURE_H
#define __NRF_CAPTURE_H

// NRF24 raw signal capture, replay and analysis.
//
// This is the engine behind both the JavaScript bindings (nrf24.captureRaw,
// nrf24.replayRaw, ...) and the NRF24 > Raw Capture menu, so the scripted and
// on-device paths share one implementation.
//
// PSEUDO-PROMISCUOUS MODE (CRC off, auto-ack off, minimum address width)
//   The nRF24L01 hardware always filters received packets by address and only
//   hands a packet over when the CRC matches. "Pseudo promiscuous" works around
//   as much of that as the silicon allows:
//     * CRC is disabled        -> payloads with a bad CRC are delivered too
//     * auto-ack is disabled   -> no ACK requirement
//     * the address is reduced to the shortest the chip supports (3 bytes) and
//       set to a fixed, common value, so far fewer bits must match
//     * a fixed 32-byte payload keeps the FIFO readable for any length
//   Together with a channel sweep this picks up a large share of the nRF24
//   traffic on a channel (and a lot of noise). It is *not* true promiscuous
//   mode: the address filter still runs in hardware, so a packet whose leading
//   address bytes do not match is still dropped. Documented as-is on purpose.

#include <Arduino.h>
#include <functional>

/// Capture buffer bounds.
#define NRF_CAP_MAX_PACKETS 64
#define NRF_CAP_MAX_PAYLOAD 32

/// A single received raw packet.
struct NrfCapturedPacket {
    uint32_t timestamp; // millis() when it arrived
    uint16_t freqMhz;   // 2400 + channel
    uint8_t channel;    // 0..125
    uint8_t len;        // payload length actually read (1..32)
    uint8_t data[NRF_CAP_MAX_PAYLOAD];
};

/// Data rates, indexed the same way as the JS setDataRate() argument.
enum NrfDataRate { NRF_RATE_1MBPS = 0, NRF_RATE_2MBPS = 1, NRF_RATE_250KBPS = 2 };

/// Transmit power, indexed the same way as the JS setPowerLevel() argument.
enum NrfPowerLevel { NRF_PWR_MIN = 0, NRF_PWR_LOW = 1, NRF_PWR_HIGH = 2, NRF_PWR_MAX = 3 };

/// Capture configuration.
struct NrfCaptureConfig {
    uint8_t startChannel = 0;
    uint8_t endChannel = 125;
    uint8_t dataRate = NRF_RATE_1MBPS;
    uint8_t power = NRF_PWR_MAX;
    bool promiscuous = true; // CRC off + minimum address width
    bool hop = false;        // step through the channel range while listening
};

/// Frequency analysis result for one channel.
struct NrfChannelActivity {
    uint8_t channel;
    uint16_t freqMhz;
    uint8_t level; // 0..100, mapped from RPD (carrier detected) sampling
};

/// Heuristic analysis of one captured payload.
struct NrfSignalAnalysis {
    int len = 0;
    int uniqueBytes = 0;
    int bitTransitions = 0;
    uint8_t minByte = 0;
    uint8_t maxByte = 0;
    bool repeats = false;       // payload is a repeated short code
    int repeatLength = 0;       // code length when `repeats` is true
    const char *kind = "unknown"; // "RGB", "PT2262", "EV1527", "repeated", "raw"
};

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

/// Bring the NRF24 up (SPI) and apply a capture configuration.
bool nrfCaptureInit(const NrfCaptureConfig &cfg);

/// True once the radio answered SPI.
bool nrfCaptureReady();

/// Put the radio back into the normal pipe mode used by send/receive.
void nrfCaptureRestoreNormal();

/// Current transceiver settings kept by this module (used by the JS setters).
uint8_t nrfCaptureGetChannel();
uint8_t nrfCaptureGetDataRate();
void nrfCaptureSetChannel(uint8_t channel);
void nrfCaptureSetDataRate(uint8_t rate);
void nrfCaptureSetPower(uint8_t level);
void nrfCaptureSetAddress(const uint8_t *addr, uint8_t len);
void nrfCaptureSetFrequencyRange(uint16_t startMhz, uint16_t endMhz);
uint16_t nrfCaptureStartFreq();
uint16_t nrfCaptureEndFreq();

/// Enable/disable the pseudo-promiscuous receive configuration.
bool nrfCaptureSetPromiscuous(bool enable);
bool nrfCaptureIsPromiscuous();

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

/// Progress reported while a capture or a channel scan runs.
struct NrfCaptureProgress {
    uint32_t elapsedMs = 0;
    uint32_t totalMs = 0; // 0 while the total is unknown (channel scan)
    int units = 0;        // packets captured, or channels scanned
    bool scanning = false; // true = channel scan, false = capture
};

/// Live progress callback used by the on-device menus. Called about every
/// 100 ms while a capture or a channel scan runs; returning false aborts the
/// operation early, which is how the "Press BACK to stop" hint is honoured - a
/// Back press is the one input that interrupts a running capture, every other
/// button is ignored.
typedef std::function<bool(const NrfCaptureProgress &p)> NrfCaptureProgressFn;

/// Listen for `durationMs`, filling the internal buffer. When the configured
/// config has `hop` set (or `forceHop` is true) the radio steps through the
/// configured channel range every `hopIntervalMs`.
/// Returns the number of packets captured.
int nrfCaptureRun(
    uint32_t durationMs, bool forceHop = false, uint32_t hopIntervalMs = 20,
    const NrfCaptureProgressFn &progress = nullptr
);

int nrfCaptureCount();
int nrfCaptureDropped();
bool nrfCapturePacket(int index, NrfCapturedPacket *out);
void nrfCaptureClear();
/// Replace the capture buffer with packets loaded from a file (for replay).
int nrfCaptureLoad(const NrfCapturedPacket *pkts, int count);

/// Group the capture by channel: returns the number of distinct channels seen
/// and writes their ids/levels. Used by the frequency-analysis display.
int nrfCaptureChannelSummary(uint8_t *channelsOut, uint8_t *countsOut, int maxOut);

/// Detect channels that carry correlated traffic (the two frequencies an RGB
/// remote transmits on). Writes pairs of channels into `pairsOut`
/// (2 * maxPairs entries) and returns the number of pairs found.
int nrfFilterMultiFreqChannels(uint8_t *channelsOut, int maxOut);

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

/// Transmit one payload on one channel `repeat` times.
bool nrfReplayRaw(const uint8_t *data, uint8_t len, uint8_t channel, uint8_t repeat, uint8_t rate);

/// Transmit one payload on several channels (dual-frequency replay).
bool nrfReplayMultiFreq(const uint8_t *data, uint8_t len, const uint8_t *channels, int channelCount, uint8_t repeat);

// ---------------------------------------------------------------------------
// Channel scanning
// ---------------------------------------------------------------------------

/// Sample the carrier-detect flag on every channel in [startCh, endCh] and map
/// it to a 0..100 activity level. Returns the number of channels written.
int nrfScanChannelActivity(
    uint8_t startCh, uint8_t endCh, uint32_t dwellMs, NrfChannelActivity *out, int maxOut,
    const NrfCaptureProgressFn &progress = nullptr
);

/// Scan across the configured range and return the channels whose activity is
/// above `thresholdPct`, most active first.
int nrfFindBusyChannels(uint8_t startCh, uint8_t endCh, uint32_t dwellMs, uint8_t thresholdPct, uint8_t *out, int maxOut);

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

/// Save a capture as a text signal profile. `/BruceNRF24/` is created on demand.
bool nrfSaveSignal(const String &path, const NrfCapturedPacket *pkts, int count, const char *label);

/// Load a profile into `out`. Returns packets loaded (0 on error) and sets
/// `labelOut` when non-null.
int nrfLoadSignal(const String &path, NrfCapturedPacket *out, int maxOut, String *labelOut);

/// Default folder for signal profiles.
const char *nrfCaptureFolder();

// ---------------------------------------------------------------------------
// Analysis / RGB helpers
// ---------------------------------------------------------------------------

/// Analyse one payload (protocol guess, repeat detection, bit statistics).
NrfSignalAnalysis nrfAnalyzeSignal(const uint8_t *data, uint8_t len);

/// Pick the replay channels for a capture: the two channels with the most
/// packets when two are present, otherwise the single busiest one.
int nrfChooseReplayChannels(const NrfCapturedPacket *pkts, int count, uint8_t *channelsOut, int maxOut);

// ---------------------------------------------------------------------------
// On-device menus
// ---------------------------------------------------------------------------

/// NRF24 > Raw Capture: capture, list, save/load, replay and analyse.
void nrfRawCaptureMenu();

#endif
