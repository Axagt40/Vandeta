#include "tunables.h"

#include <globals.h>

// Storage owned by this table. Entries flagged TUNE_BACKED point into
// bruceConfig instead, so the value the existing menus and the existing
// bruce.conf keys already carry stays the single source of truth - a second
// copy is how two settings for one thing appear.
static int32_t g_rf_noise_floor = -93;
static int32_t g_rf_park_ms = 15000;
static int32_t g_rf_scan_dwell_ms = 70;
static int32_t g_rf_spi_hold_us = 0;
static int32_t g_nrf_cap_ms = 3000;
static int32_t g_nrf_repeat = 3;
static int32_t g_ir_sweep_delay_ms = 100;
static int32_t g_ir_lock_replays = 5;
static int32_t g_mitm_fallback_delay_ms = 10000;
static int32_t g_mitm_arp_restore_on_stop = 1;
static int32_t g_portal_hold_ms = 600000;
static int32_t g_scan_chan_dwell_ms = 250;

// Task 1, phase 2: variant selections. These are indices, not magnitudes, and
// every default is the behaviour phase 1 shipped, so a flash changes nothing.
static int32_t g_rf_scan_variant = 2;     // adaptive
static int32_t g_ir_repeat_variant = 0;   // fixed
static int32_t g_nrf_capture_variant = 0; // single

// The names are the spec's, in index order; the arrays are null-terminated so
// tuneVariantCount() can count them without a second bookkeeping number.
static const char *const g_rf_scan_variants[] = {"seq", "park", "adaptive", nullptr};
static const char *const g_ir_repeat_variants[] = {"fixed", "adaptive_len", "burst", nullptr};
static const char *const g_nrf_capture_variants[] = {"single", "hop", "locked", nullptr};

static tune_entry g_tunes[] = {
    // RF scan / wardriver front end.
    // -120..0 dBm: below -120 nothing is a signal, above 0 no part reports it.
    // Wired: rf_scan.cpp's rfScanCarrierRssi() is the carrier threshold (was
    // RF_SCAN_CARRIER_RSSI). Default is the value that constant had.
    {"rf.noise_floor", TUNE_I32, &g_rf_noise_floor, -120, 0, -93, TUNE_LIVE},
    // Wired: rf_scan.cpp parks a sweep-found frequency for this long (was the
    // RF_SCAN_HOLD_MS constant). Default is the value the constant had.
    {"rf.park_ms", TUNE_I32, &g_rf_park_ms, 0, 60000, 15000, TUNE_LIVE},
    // Wired: rf_scan.cpp listens per frequency for this long between hops (was
    // RF_SCAN_DWELL_MS). 70 ms keeps a full 57-frequency sweep at ~4 s.
    {"rf.scan_dwell_ms", TUNE_I32, &g_rf_scan_dwell_ms, 1, 2000, 70, TUNE_LIVE},
    // Wired: rf_utils.cpp's rfGetRssi() waits this long after taking the bus.
    // Default 0 = no added delay, i.e. the behaviour before this entry existed.
    {"rf.spi_hold_us", TUNE_I32, &g_rf_spi_hold_us, 0, 1000, 0, TUNE_LIVE},

    // Wardriver: these two are the values the RF > Wardriver menu already edits,
    // so they are exposed here rather than duplicated.
    {"ward.min_pulses", TUNE_U8, &bruceConfig.rfWardriverMinPulses, 1, 255, 16, TUNE_LIVE | TUNE_BACKED},
    // Wired: rf_wardriver.cpp's wrSeenRecently() compares against it, in ms.
    // This is the value the old rfWardriverDedupeSec key held (30 s); that key
    // was retired with the move, and fromFile() migrates a config that still
    // carries it (seconds -> ms) so there is one source, not two.
    {"ward.dup_window_ms", TUNE_U32, &bruceConfig.rfWardriverDedupeMs, 0, 600000, 30000, TUNE_LIVE | TUNE_BACKED},
    {"ward.band_hop", TUNE_BOOL, &bruceConfig.rfWardriverHop, 0, 1, 0, TUNE_LIVE | TUNE_BACKED},

    // nRF24 capture/replay.
    // Wired: nrf_commands.cpp's capture uses it when no window is typed.
    {"nrf.cap_ms", TUNE_I32, &g_nrf_cap_ms, 100, 60000, 3000, TUNE_LIVE},
    // Wired: nrf_commands.cpp's replay uses it when no repeat count is typed.
    {"nrf.repeat", TUNE_I32, &g_nrf_repeat, 1, 20, 3, TUNE_LIVE},

    // IR.
    // Wired: ir_universal.cpp's sweepGapMs() reads it between candidates.
    {"ir.sweep_delay_ms", TUNE_I32, &g_ir_sweep_delay_ms, 0, 2000, 100, TUNE_LIVE},
    // Wired: ir_suite.cpp writes it into the suite's LOCK_CODES literal before
    // the script runs (the script itself is generated at build time). Min 1:
    // zero would leave the narrowing replay nothing to re-send and loop forever.
    {"ir.lock_replays", TUNE_I32, &g_ir_lock_replays, 1, 50, 10, TUNE_LIVE},

    // MITM / portal.
    // Wired: mitm_menu.cpp reads it when a `fallback` line names no time (was the
    // MITM_FALLBACK_DEFAULT_MS constant). Default is the value that constant had.
    {"mitm.fallback_delay_ms", TUNE_I32, &g_mitm_fallback_delay_ms, 0, 60000, 10000, TUNE_LIVE},
    // Wired: net_spoof.cpp's cleanup restores the ARP pairs only when this is on.
    {"mitm.arp_restore_on_stop", TUNE_BOOL, &g_mitm_arp_restore_on_stop, 0, 1, 1, TUNE_LIVE},
    // Wired: mitm_uc.cpp uses it when the run names no duration (was
    // WIFI_PORTAL_MAX_MS). Default is the value that constant had.
    {"portal.hold_ms", TUNE_I32, &g_portal_hold_ms, 1000, 600000, 600000, TUNE_LIVE},

    // Wired: wifi_common.cpp's streaming scan listens this long per channel (was
    // WIFI_SCAN_STREAM_MS). The one blocking full-sweep MAX is a different knob
    // and keeps its own constant. Default is the value that constant had.
    {"scan.chan_dwell_ms", TUNE_I32, &g_scan_chan_dwell_ms, 10, 5000, 130, TUNE_LIVE},

    // Variant tables (phase 2). A selection is a normal TUNE_LIVE entry, which
    // is the point: one value, one conf key, one reader path (`tune` and `patch`
    // are two verbs over the same entry). Default index = the behaviour that
    // existed before the table.
    {
        "rf.scan_variant", TUNE_U8, &g_rf_scan_variant, 0, 2, 2, TUNE_LIVE | TUNE_ENUM, g_rf_scan_variants
    },
    {
        "ir.repeat_variant", TUNE_U8, &g_ir_repeat_variant, 0, 2, 0, TUNE_LIVE | TUNE_ENUM,
        g_ir_repeat_variants
    },
    {
        "nrf.capture_variant", TUNE_U8, &g_nrf_capture_variant, 0, 2, 0, TUNE_LIVE | TUNE_ENUM,
        g_nrf_capture_variants
    },
};

static const int g_tuneCount = (int)(sizeof(g_tunes) / sizeof(g_tunes[0]));

int tuneCount() { return g_tuneCount; }

const tune_entry *tuneAt(int i) {
    if (i < 0 || i >= g_tuneCount) return nullptr;
    return &g_tunes[i];
}

const tune_entry *tuneFind(const char *name) {
    if (name == nullptr) return nullptr;
    for (int i = 0; i < g_tuneCount; i++) {
        if (strcmp(g_tunes[i].name, name) == 0) return &g_tunes[i];
    }
    return nullptr;
}

const char *tuneTypeName(uint8_t type) {
    switch (type) {
        case TUNE_U8: return "u8";
        case TUNE_U16: return "u16";
        case TUNE_U32: return "u32";
        case TUNE_I32: return "i32";
        case TUNE_BOOL: return "bool";
        default: return "?";
    }
}

int32_t tuneGet(const tune_entry *e) {
    if (e == nullptr || e->ptr == nullptr) return 0;
    switch (e->type) {
        case TUNE_U8: return (int32_t) * (uint8_t *)e->ptr;
        case TUNE_U16: return (int32_t) * (uint16_t *)e->ptr;
        case TUNE_U32: return (int32_t) * (uint32_t *)e->ptr;
        case TUNE_I32: return *(int32_t *)e->ptr;
        case TUNE_BOOL: return *(bool *)e->ptr ? 1 : 0;
        default: return 0;
    }
}

int32_t tuneSet(tune_entry *e, int32_t value, bool *clamped) {
    if (clamped != nullptr) *clamped = false;
    if (e == nullptr || e->ptr == nullptr) return 0;

    int32_t v = value;
    if (v < e->min) {
        v = e->min;
        if (clamped != nullptr) *clamped = true;
    }
    if (v > e->max) {
        v = e->max;
        if (clamped != nullptr) *clamped = true;
    }

    switch (e->type) {
        case TUNE_U8: *(uint8_t *)e->ptr = (uint8_t)v; break;
        case TUNE_U16: *(uint16_t *)e->ptr = (uint16_t)v; break;
        case TUNE_U32: *(uint32_t *)e->ptr = (uint32_t)v; break;
        case TUNE_I32: *(int32_t *)e->ptr = v; break;
        case TUNE_BOOL: *(bool *)e->ptr = (v != 0); break;
        default: break;
    }
    return tuneGet(e);
}

void tuneReset(tune_entry *e) { tuneSet(e, e ? e->def : 0, nullptr); }

void tuneResetAll() {
    for (int i = 0; i < g_tuneCount; i++) tuneReset(&g_tunes[i]);
}

bool tuneSetByName(const char *name, int32_t value) {
    tune_entry *e = (tune_entry *)tuneFind(name);
    if (e == nullptr) return false;
    tuneSet(e, value, nullptr);
    return true;
}

int32_t tuneGetByName(const char *name, bool *found) {
    const tune_entry *e = tuneFind(name);
    if (found != nullptr) *found = (e != nullptr);
    return tuneGet(e);
}

int tuneVariantCount(const tune_entry *e) {
    if (e == nullptr || !(e->flags & TUNE_ENUM) || e->variants == nullptr) return 0;
    int n = 0;
    while (e->variants[n] != nullptr) n++;
    return n;
}

const char *tuneVariantNameAt(const tune_entry *e, int32_t index) {
    int n = tuneVariantCount(e);
    if (index < 0 || index >= n) return nullptr;
    return e->variants[index];
}

const char *tuneVariantName(const tune_entry *e) { return tuneVariantNameAt(e, tuneGet(e)); }
