#include "iram_patch.h"
#include "core/dev/build_info.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_memory_utils.h>
#include <globals.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Task 1, phase 3: the IRAM A/B overlay.
//
// One table describes what may be patched. Each entry names the slot, the
// function body the unpatched cell points at, and (for rf_scan_step) a probe
// that can be called through the current cell for verification.
//
// The bodies are ordinary functions in their modules, marked `used` and
// `noinline`, with the public name left as a thunk. That keeps every call site
// on the hook without editing callers, and LTO cannot inline the body into the
// thunk call or drop it while the table holds its address.
//
// The overlay is two 4096-byte banks in executable internal RAM. A slot's live
// bytes are in one bank; applying again writes the other bank and flips the
// cell, so a live patch is never overwritten while it is reachable.

// Bodies (defined in their modules, non-static so the table can name them).
void setMHZ_body(float frequency);
int rfGetRssi_body();
uint32_t rf_scan_step_body();
bool ward_capture_burst_body(uint32_t pulses, uint32_t shortPulses);
uint32_t nrf_capture_raw_body(uint32_t durationMs);
uint16_t ir_transmit_button_body(uint16_t nbits);
bool mitm_build_frame_body(
    const uint8_t *ethDst, const uint8_t *senderMac, const uint8_t *senderIp, const uint8_t *targetMac,
    const uint8_t *targetIp, uint16_t opcode, int repeat
);
uint32_t portal_serve_body(uint32_t durationMs);

// The public wrappers, for the probe and for documentation.
uint32_t rf_scan_step();

struct vnd_slot_def {
    const char *name;
    void *body;
    uint32_t (*probe)(); // nullptr unless the slot takes no arguments
};

static const vnd_slot_def g_slots[VND_SLOT_COUNT] = {
    {"rf_set_freq", (void *)&setMHZ_body, nullptr},
    {"rf_get_rssi", (void *)&rfGetRssi_body, nullptr},
    {"rf_scan_step", (void *)&rf_scan_step_body, &rf_scan_step},
    {"ward_capture_burst", (void *)&ward_capture_burst_body, nullptr},
    {"nrf_capture_raw", (void *)&nrf_capture_raw_body, nullptr},
    {"ir_transmit_button", (void *)&ir_transmit_button_body, nullptr},
    {"mitm_build_frame", (void *)&mitm_build_frame_body, nullptr},
    {"portal_serve", (void *)&portal_serve_body, nullptr},
};

// Writable jump cells. Volatile: the thunk must re-read the cell on every call,
// or a compiler could hoist the address and a patch would not take effect.
static volatile uint32_t g_cells[VND_SLOT_COUNT];
// Which bank a slot's live payload is in (-1 = original body).
static int8_t g_slotBank[VND_SLOT_COUNT] = {-1, -1, -1, -1, -1, -1, -1, -1};
static uint32_t g_slotCrc[VND_SLOT_COUNT] = {0};

// Phase 6: the overlay normally lives in the `vndpatch` flash partition, mapped
// twice (bank A and bank B) as instruction memory. A patch is written to the
// inactive bank with esp_partition_write and the cell is flipped to the mapped
// alias, so no IRAM is spent on patch space in developer mode. The phase-3 IRAM
// allocation remains as a fallback for boards built without that partition.
enum vnd_overlay_kind : uint8_t { VND_OVERLAY_NONE = 0, VND_OVERLAY_FLASH, VND_OVERLAY_IRAM };

static vnd_overlay_kind g_overlayKind = VND_OVERLAY_NONE;
static const esp_partition_t *g_partition = nullptr;
static uint8_t *g_banks[2] = {nullptr, nullptr};
static uint32_t g_slotBytes = VND_IRAM_SLOT_BYTES;
static uint32_t g_bankBytes = VND_IRAM_BANK_BYTES;
static esp_partition_mmap_handle_t g_mapHandles[2] = {0, 0};
static volatile int g_gate = 0;
static portMUX_TYPE g_cellMux = portMUX_INITIALIZER_UNLOCKED;

uintptr_t vndPatchTarget(uint8_t slot) {
    if (slot >= VND_SLOT_COUNT) return 0;
    uintptr_t addr = (uintptr_t)g_cells[slot];
    if (addr == 0) addr = (uintptr_t)g_slots[slot].body;
    return addr;
}

void vndPatchGateEnter() { g_gate++; }
void vndPatchGateExit() {
    if (g_gate > 0) g_gate--;
}
int vndPatchGateCount() { return g_gate; }

int vndPatchSlotCount() { return VND_SLOT_COUNT; }
const char *vndPatchSlotName(int slot) {
    if (slot < 0 || slot >= VND_SLOT_COUNT) return "?";
    return g_slots[slot].name;
}
bool vndPatchSlotApplied(int slot) {
    if (slot < 0 || slot >= VND_SLOT_COUNT) return false;
    return g_cells[slot] != 0 && (uintptr_t)g_cells[slot] != (uintptr_t)g_slots[slot].body;
}
int vndPatchAppliedCount() {
    int n = 0;
    for (int i = 0; i < VND_SLOT_COUNT; i++)
        if (vndPatchSlotApplied(i)) n++;
    return n;
}

int vndPatchSlotFind(const char *nameOrNumber) {
    if (nameOrNumber == nullptr || *nameOrNumber == 0) return -1;
    for (int i = 0; i < VND_SLOT_COUNT; i++) {
        if (strcmp(nameOrNumber, g_slots[i].name) == 0) return i;
    }
    // A pure number is accepted too, so `patch revert 2` works.
    char *end = nullptr;
    long v = strtol(nameOrNumber, &end, 10);
    if (end != nullptr && *end == 0 && v >= 0 && v < VND_SLOT_COUNT) return (int)v;
    return -1;
}

// ---------------------------------------------------------------------------
// Build ID: the same 8 bytes vndBuildIdBytes() folds from the generated
// build identity (git SHA + dirty + timestamp), so `patch apply` and
// `devflash` refuse a blob built for a different image by the same rule.
// ---------------------------------------------------------------------------
void vndPatchBuildId(uint8_t out[8]) { vndBuildIdBytes(out); }

String vndPatchBuildIdHex() {
    uint8_t id[8];
    vndPatchBuildId(id);
    char buf[17];
    for (int i = 0; i < 8; i++) snprintf(buf + i * 2, 3, "%02x", id[i]);
    buf[16] = 0;
    return String(buf);
}

// ---------------------------------------------------------------------------
// Overlay allocation
// ---------------------------------------------------------------------------
// ESP32-S3's IRAM heap is registered as EXEC|32BIT, not EXEC|8BIT: byte stores
// into IRAM are not a supported capability. The spec's literal
// `MALLOC_CAP_EXEC | MALLOC_CAP_8BIT` therefore returns NULL on this part (found
// by the first board run). Ask for the capability the pool actually has, and
// write the payload with aligned 32-bit stores.
static int g_overlayCaps = 0;
static const uint8_t g_zeroRegion[VND_IRAM_SLOT_BYTES] = {0};

static void overlayWrite(uint8_t *dst, const uint8_t *src, size_t len) {
    const size_t words = (len + 3) / 4;
    volatile uint32_t *d = (volatile uint32_t *)dst;
    for (size_t i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) {
            const size_t k = i * 4 + (size_t)b;
            if (k < len) w |= (uint32_t)src[k] << (8 * b);
        }
        d[i] = w;
    }
}

static bool overlayAllocFlash() {
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)VND_PARTITION_SUBTYPE, VND_PARTITION_LABEL
    );
    if (p == nullptr || p->size < VND_FLASH_MAP_BYTES) return false;

    const void *a = nullptr;
    const void *b = nullptr;
    esp_partition_mmap_handle_t ha = 0;
    esp_partition_mmap_handle_t hb = 0;
    const esp_err_t ea =
        esp_partition_mmap(p, 0, VND_FLASH_BANK_BYTES, ESP_PARTITION_MMAP_INST, &a, &ha);
    const esp_err_t eb = esp_partition_mmap(
        p, VND_FLASH_BANK_BYTES, VND_FLASH_BANK_BYTES, ESP_PARTITION_MMAP_INST, &b, &hb
    );
    // Both banks must be fetchable; the S3 maps instruction windows at 0x4200_0000.
    if (ea != ESP_OK || eb != ESP_OK || a == nullptr || b == nullptr || !esp_ptr_executable(a) ||
        !esp_ptr_executable(b)) {
        if (ea == ESP_OK) esp_partition_munmap(ha);
        if (eb == ESP_OK) esp_partition_munmap(hb);
        return false;
    }

    g_partition = p;
    g_banks[0] = (uint8_t *)a;
    g_banks[1] = (uint8_t *)b;
    g_slotBytes = VND_FLASH_SLOT_BYTES;
    g_bankBytes = VND_FLASH_BANK_BYTES;
    g_mapHandles[0] = ha;
    g_mapHandles[1] = hb;
    g_overlayKind = VND_OVERLAY_FLASH;
    return true;
}

static bool overlayAllocIram() {
    const size_t bytes = 2 * VND_IRAM_BANK_BYTES;
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    g_overlayCaps = 32;
    if (p == nullptr) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_EXEC | MALLOC_CAP_8BIT);
        g_overlayCaps = 8;
    }
    if (p == nullptr) return false;
    if (!esp_ptr_in_iram(p)) {
        // Never run a payload from a region the CPU cannot fetch. Dropping the
        // allocation is a clean refusal instead of a crash later.
        heap_caps_free(p);
        return false;
    }
    for (size_t off = 0; off < bytes; off += VND_IRAM_SLOT_BYTES)
        overlayWrite((uint8_t *)p + off, g_zeroRegion, VND_IRAM_SLOT_BYTES);
    g_banks[0] = (uint8_t *)p;
    g_banks[1] = (uint8_t *)p + VND_IRAM_BANK_BYTES;
    g_slotBytes = VND_IRAM_SLOT_BYTES;
    g_bankBytes = VND_IRAM_BANK_BYTES;
    g_overlayKind = VND_OVERLAY_IRAM;
    return true;
}

static bool overlayAlloc() {
    if (g_overlayKind != VND_OVERLAY_NONE) return true;
    if (overlayAllocFlash()) return true;
    // The static .iram_patch fallback is deliberately not linked in: it would
    // cost 8 KB of IRAM even with developer mode off, which is the opposite of
    // the gating rule. The heap fallback costs nothing until dev mode is on.
    return overlayAllocIram();
}

static void overlayFree() {
    if (g_overlayKind == VND_OVERLAY_FLASH) {
        esp_partition_munmap(g_mapHandles[0]);
        esp_partition_munmap(g_mapHandles[1]);
        g_mapHandles[0] = g_mapHandles[1] = 0;
        g_partition = nullptr;
    } else if (g_overlayKind == VND_OVERLAY_IRAM && g_banks[0] != nullptr) {
        heap_caps_free((void *)g_banks[0]);
    }
    g_banks[0] = g_banks[1] = nullptr;
    g_slotBytes = VND_IRAM_SLOT_BYTES;
    g_bankBytes = VND_IRAM_BANK_BYTES;
    g_overlayKind = VND_OVERLAY_NONE;
}

bool vndPatchActive() { return g_overlayKind != VND_OVERLAY_NONE; }

String vndPatchOverlayInfo() {
    String s = "overlay ";
    if (g_overlayKind == VND_OVERLAY_NONE) {
        s += "not allocated (developer mode off or allocation failed)";
        return s;
    }
    if (g_overlayKind == VND_OVERLAY_FLASH) {
        s += String(VND_FLASH_MAP_BYTES) + " B flash mmap @" + String((uintptr_t)g_banks[0], HEX) +
             " (slot " + String(VND_FLASH_SLOT_BYTES) + " B, 2 banks, " + VND_PARTITION_LABEL + " @0x" +
             String((uint32_t)g_partition->address, HEX) + "), " + String(VND_SLOT_COUNT) + " slots, " +
             String(vndPatchAppliedCount()) + " applied, gate " + String((int)g_gate);
        return s;
    }
    s += String(2 * VND_IRAM_BANK_BYTES) + " B heap @" + String((uintptr_t)g_banks[0], HEX) + " (exec" +
         String(g_overlayCaps) + "), " + String(VND_SLOT_COUNT) + " slots, " +
         String(vndPatchAppliedCount()) + " applied, gate " + String((int)g_gate);
    return s;
}

// ---------------------------------------------------------------------------
// Journal: /vndpatch.journal on SD, rewritten whole on every change.
//   VNDJ 1
//   build <16 hex>
//   aborts <n>
//   pending <0|1>
//   entry <slot> <crc> <epoch>
// `pending` is 1 while at least one patch is applied. A boot that finds it 1
// knows the previous run ended without reverting, counts an abort, and after
// two consecutive aborts clears the journal and reports it.
// ---------------------------------------------------------------------------
struct vnd_journal {
    uint32_t aborts;
    uint8_t pending;
    int entries;
    struct {
        uint8_t slot;
        uint32_t crc;
        uint32_t epoch;
    } e[VND_SLOT_COUNT];
};

static void journalDefaults(vnd_journal &j) {
    memset(&j, 0, sizeof(j));
    j.pending = 0;
}

static bool journalRead(vnd_journal &j) {
    journalDefaults(j);
    if (!sdcardMounted) return false;
    File f = SD.open(VND_JOURNAL_PATH, FILE_READ);
    if (!f) return false;
    bool magic = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.startsWith("VNDJ ")) {
            magic = true;
        } else if (line.startsWith("aborts ")) {
            j.aborts = (uint32_t)line.substring(7).toInt();
        } else if (line.startsWith("pending ")) {
            j.pending = (uint8_t)line.substring(8).toInt();
        } else if (line.startsWith("entry ")) {
            int slot = 0;
            unsigned crc = 0, epoch = 0;
            if (sscanf(line.c_str(), "entry %d %x %u", &slot, &crc, &epoch) == 3 &&
                slot >= 0 && slot < VND_SLOT_COUNT && j.entries < VND_SLOT_COUNT) {
                j.e[j.entries].slot = (uint8_t)slot;
                j.e[j.entries].crc = crc;
                j.e[j.entries].epoch = epoch;
                j.entries++;
            }
        }
    }
    f.close();
    if (!magic) journalDefaults(j);
    return magic;
}

static bool journalWrite(const vnd_journal &j) {
    if (!sdcardMounted) return false;
    File f = SD.open(VND_JOURNAL_PATH, FILE_WRITE);
    if (!f) return false;
    f.println("VNDJ 1");
    f.println("build " + vndPatchBuildIdHex());
    f.println("aborts " + String(j.aborts));
    f.println("pending " + String((int)j.pending));
    for (int i = 0; i < j.entries; i++) {
        f.println(
            "entry " + String((int)j.e[i].slot) + " " + String(j.e[i].crc, HEX) + " " + String(j.e[i].epoch)
        );
    }
    f.close();
    return true;
}

static void journalSetPending(bool pending) {
    vnd_journal j;
    if (!journalRead(j)) journalDefaults(j);
    j.pending = pending ? 1 : 0;
    if (!pending) {
        // A clean state (revert all / dev mode off) ends the abort streak and
        // forgets the entries: the journal records live patches only.
        j.aborts = 0;
        j.entries = 0;
    }
    journalWrite(j);
}

static void journalAdd(int slot, uint32_t crc) {
    vnd_journal j;
    if (!journalRead(j)) journalDefaults(j);
    j.pending = 1;
    // Do NOT reset aborts here: the fault rule counts consecutive boots that
    // ended with a patch applied, and re-applying after the first boot is
    // exactly how the second abort is reached.
    bool replaced = false;
    for (int i = 0; i < j.entries; i++) {
        if (j.e[i].slot == slot) {
            j.e[i].crc = crc;
            j.e[i].epoch = (uint32_t)time(nullptr);
            replaced = true;
        }
    }
    if (!replaced && j.entries < VND_SLOT_COUNT) {
        int i = j.entries++;
        j.e[i].slot = (uint8_t)slot;
        j.e[i].crc = crc;
        j.e[i].epoch = (uint32_t)time(nullptr);
    }
    journalWrite(j);
}

bool vndPatchJournalDump(String &out) {
    vnd_journal j;
    bool present = journalRead(j);
    out = "journal " + String(present ? "present" : "empty") + " build " + vndPatchBuildIdHex() + " aborts " +
          String(j.aborts) + " pending " + String((int)j.pending);
    for (int i = 0; i < j.entries; i++) {
        out += "\nentry slot " + String((int)j.e[i].slot) + " " + vndPatchSlotName(j.e[i].slot) + " crc " +
               String(j.e[i].crc, HEX) + " epoch " + String(j.e[i].epoch);
    }
    if (!sdcardMounted) out += "\njournal: no SD card";
    return present;
}

void vndPatchJournalClear() {
    vnd_journal j;
    journalDefaults(j);
    journalWrite(j);
}

// ---------------------------------------------------------------------------
// Boot / shutdown
// ---------------------------------------------------------------------------
void vndPatchInit() {
    if (bruceConfig.usbDebug == 0) return;
    overlayAlloc(); // failure is reported by `patch list`, not fatal

    // Journal bookkeeping: a pending flag at boot means the board restarted
    // with patches applied. Two in a row is a fault loop, so the journal is
    // cleared instead of being trusted.
    vnd_journal j;
    if (journalRead(j) && j.pending) {
        j.pending = 0;
        j.aborts++;
        if (j.aborts >= 2) {
            j.aborts = 0;
            j.entries = 0;
            journalWrite(j);
            Serial.println("patches skipped after fault");
        } else {
            journalWrite(j);
        }
    }
}

void vndPatchShutdown() {
    int reverted = vndPatchRevertAll();
    overlayFree();
    journalSetPending(false);
    if (reverted > 0) Serial.println("patches reverted (" + String(reverted) + ")");
}

// ---------------------------------------------------------------------------
// Payload application
// ---------------------------------------------------------------------------
uint32_t vndPatchCrc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

const char *vndPatchReason(int result) {
    switch (result) {
        case VND_OK: return "ok";
        case VND_ERR_OFF: return "developer mode off";
        case VND_ERR_SLOT: return "unknown slot";
        case VND_ERR_HEADER: return "bad header (magic VNDP, version 1, 20-byte header)";
        case VND_ERR_BUILD: return "payload build ID does not match this image";
        case VND_ERR_SLOT_ID: return "payload names a different slot";
        case VND_ERR_CRC: return "payload crc32 mismatch";
        case VND_ERR_GATE: return "a radio loop is running (use --force)";
        case VND_ERR_ALLOC: return "overlay not allocated";
        case VND_ERR_FLASH: return "vndpatch partition write failed";
        default: return "?";
    }
}

int vndPatchApply(int slot, const uint8_t *payload, size_t len, bool force) {
    if (bruceConfig.usbDebug == 0) return VND_ERR_OFF;
    if (slot < 0 || slot >= VND_SLOT_COUNT) return VND_ERR_SLOT;
    if (!overlayAlloc()) return VND_ERR_ALLOC;
    if (!force && g_gate != 0) return VND_ERR_GATE;

    if (payload == nullptr || len < VND_HEADER_BYTES) return VND_ERR_HEADER;
    if (payload[0] != VND_MAGIC0 || payload[1] != VND_MAGIC1 || payload[2] != VND_MAGIC2 ||
        payload[3] != VND_MAGIC3)
        return VND_ERR_HEADER;
    const uint8_t version = payload[4];
    if (version != VND_VERSION) return VND_ERR_HEADER;
    uint8_t buildId[8];
    vndPatchBuildId(buildId);
    if (memcmp(payload + 5, buildId, 8) != 0) return VND_ERR_BUILD;
    const uint8_t payloadSlot = payload[13];
    const uint16_t codeLen = (uint16_t)payload[14] | ((uint16_t)payload[15] << 8);
    const uint32_t crc = (uint32_t)payload[16] | ((uint32_t)payload[17] << 8) |
                         ((uint32_t)payload[18] << 16) | ((uint32_t)payload[19] << 24);
    if (payloadSlot != slot) return VND_ERR_SLOT_ID;
    const uint32_t slotCapacity =
        (g_overlayKind == VND_OVERLAY_FLASH) ? VND_FLASH_SLOT_BYTES - VND_HEADER_BYTES : VND_IRAM_SLOT_BYTES;
    if (codeLen == 0 || codeLen > slotCapacity) return VND_ERR_HEADER;
    if (len < (size_t)VND_HEADER_BYTES + codeLen) return VND_ERR_HEADER;
    const uint8_t *code = payload + VND_HEADER_BYTES;
    if (vndPatchCrc32(code, codeLen) != crc) return VND_ERR_CRC;

    // Write the bank this slot is NOT live in, then flip the cell. The flip is
    // a single 32-bit store, done with interrupts masked so a radio loop cannot
    // observe the new target while the bytes are still being written.
    const int nextBank = (g_slotBank[slot] == 0) ? 1 : 0;
    uint8_t *dst = g_banks[nextBank] + (size_t)slot * g_slotBytes;
    if (g_overlayKind == VND_OVERLAY_FLASH) {
        // A NOR sector must be erased before it is written; each slot owns one
        // 4 KB sector per bank, so the erase cannot touch a neighbour. The
        // IDF writer suspends the cache around the operation itself.
        const size_t off = (size_t)nextBank * g_bankBytes + (size_t)slot * g_slotBytes;
        esp_err_t e = esp_partition_erase_range(g_partition, off, VND_FLASH_SLOT_BYTES);
        if (e == ESP_OK) e = esp_partition_write(g_partition, off, code, codeLen);
        if (e != ESP_OK) return VND_ERR_FLASH;
    } else {
        overlayWrite(dst, g_zeroRegion, VND_IRAM_SLOT_BYTES); // zero the slot, word-wise
        overlayWrite(dst, code, codeLen);
    }

    portENTER_CRITICAL(&g_cellMux);
    g_cells[slot] = (uint32_t)(uintptr_t)dst;
    portEXIT_CRITICAL(&g_cellMux);

    g_slotBank[slot] = (int8_t)nextBank;
    g_slotCrc[slot] = crc;
    journalAdd(slot, crc);
    return VND_OK;
}

bool vndPatchRevert(int slot) {
    if (slot < 0 || slot >= VND_SLOT_COUNT) return false;
    bool was = vndPatchSlotApplied(slot);
    portENTER_CRITICAL(&g_cellMux);
    g_cells[slot] = (uint32_t)(uintptr_t)g_slots[slot].body;
    portEXIT_CRITICAL(&g_cellMux);
    g_slotBank[slot] = -1;
    g_slotCrc[slot] = 0;
    journalSetPending(vndPatchAppliedCount() > 0);
    return was;
}

int vndPatchRevertAll() {
    int n = 0;
    for (int i = 0; i < VND_SLOT_COUNT; i++) {
        if (vndPatchSlotApplied(i)) n++;
        g_cells[i] = (uint32_t)(uintptr_t)g_slots[i].body;
        g_slotBank[i] = -1;
        g_slotCrc[i] = 0;
    }
    return n;
}

int vndPatchProbe(int slot, uint32_t &value) {
    if (slot < 0 || slot >= VND_SLOT_COUNT || g_slots[slot].probe == nullptr) return VND_ERR_SLOT;
    // Deliberately through the public wrapper: this is the same cell the real
    // consumer uses, so a successful probe is a successful live patch.
    value = g_slots[slot].probe();
    return VND_OK;
}
