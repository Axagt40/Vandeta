#ifndef __CORE_DEV_IRAM_PATCH_H__
#define __CORE_DEV_IRAM_PATCH_H__

// Developer-only IRAM A/B code overlay (task 1, phase 3).
//
// The spec's 6-byte prologue (`j vnd_slot_N`) cannot reach RAM from IROM: a `j`
// is PC-relative with a +/-128 KB range and this chip puts flash at 0x4200_0000
// and IRAM around 0x3FC8_0000. Each patchable function is therefore a small
// indirect thunk (l32r + l32i + callx, ~12 bytes, noinline so LTO cannot inline
// it away) that reads a writable 32-bit cell and tail-calls the address in it.
// The cell starts at the original body and is flipped to the overlay copy under
// a critical section. This is the "literal-pool scheme" the spec's STOP AND
// REPORT item anticipated; the exact cost is printed by `patch list`.
//
// With Developer Mode off nothing here is allocated and every cell points at
// the original body, so behaviour is the stock firmware. The only permanent
// costs are the thunks (flash) and the eight cells (32 bytes of .bss).

#include <Arduino.h>
#include <stdint.h>

#define VND_SLOT_COUNT 8
// Phase 6: each slot owns one 4 KB flash sector in each of the two banks, so a
// payload is erased without touching its neighbours. The 64 KB `vndpatch` data
// partition (8 slots x 2 banks x 4 KB) is mapped as two 32 KB instruction
// windows (A and B).
#define VND_FLASH_SLOT_BYTES 4096
#define VND_FLASH_BANK_BYTES (VND_SLOT_COUNT * VND_FLASH_SLOT_BYTES)
#define VND_FLASH_MAP_BYTES (2 * VND_FLASH_BANK_BYTES)
#define VND_PARTITION_LABEL "vndpatch"
#define VND_PARTITION_SUBTYPE 0x40
// Fallback for boards without that partition: the phase-3 8 KB IRAM overlay,
// 512 bytes per slot per bank. It is allocated only in developer mode.
#define VND_IRAM_BANK_BYTES 4096
#define VND_IRAM_SLOT_BYTES (VND_IRAM_BANK_BYTES / VND_SLOT_COUNT)
#define VND_HEADER_BYTES 20
#define VND_PAYLOAD_MAX (VND_FLASH_SLOT_BYTES - VND_HEADER_BYTES)
#define VND_MAGIC0 'V'
#define VND_MAGIC1 'N'
#define VND_MAGIC2 'D'
#define VND_MAGIC3 'P'
#define VND_VERSION 1
#define VND_JOURNAL_PATH "/vndpatch.journal"

enum vnd_slot_id : uint8_t {
    VND_SLOT_RF_SET_FREQ = 0,
    VND_SLOT_RF_GET_RSSI,
    VND_SLOT_RF_SCAN_STEP,
    VND_SLOT_WARD_CAPTURE_BURST,
    VND_SLOT_NRF_CAPTURE_RAW,
    VND_SLOT_IR_TRANSMIT_BUTTON,
    VND_SLOT_MITM_BUILD_FRAME,
    VND_SLOT_PORTAL_SERVE,
};

// Apply result. The reason strings are produced by vndPatchReason().
enum vnd_result : uint8_t {
    VND_OK = 0,
    VND_ERR_OFF,        // developer mode is off
    VND_ERR_SLOT,       // unknown slot name / id
    VND_ERR_HEADER,     // magic/version/length malformed
    VND_ERR_BUILD,      // payload build ID differs from this image
    VND_ERR_SLOT_ID,    // header names a different slot than the command
    VND_ERR_CRC,        // payload crc32 mismatch
    VND_ERR_GATE,       // a radio loop is running and --force was not given
    VND_ERR_ALLOC,      // overlay could not be allocated
    VND_ERR_FLASH,      // partition erase/write failed
};

// The address the given slot currently jumps to. Reads the cell volatile on
// every call; before vndPatchInit() the cell is zero and the table body is
// returned, so an early call is safe.
uintptr_t vndPatchTarget(uint8_t slot);

// Radio-loop gate. `patch apply` refuses while any of these is non-zero.
void vndPatchGateEnter();
void vndPatchGateExit();
int vndPatchGateCount();
struct VndPatchGate {
    VndPatchGate() { vndPatchGateEnter(); }
    ~VndPatchGate() { vndPatchGateExit(); }
    VndPatchGate(const VndPatchGate &) = delete;
    VndPatchGate &operator=(const VndPatchGate &) = delete;
};

// Allocate the overlay when developer mode is on and run the journal's boot
// bookkeeping (two consecutive aborted boots skip the journal). Idempotent.
void vndPatchInit();
// Revert every applied patch, release the overlay and mark the journal clean.
// Called when developer mode is turned off; the journal itself is kept.
void vndPatchShutdown();

bool vndPatchActive();      // overlay resident
int vndPatchAppliedCount(); // slots pointing at the overlay

// Slot metadata for `patch list`.
int vndPatchSlotCount();
const char *vndPatchSlotName(int slot);
bool vndPatchSlotApplied(int slot);
int vndPatchSlotFind(const char *nameOrNumber); // -1 when unknown

// Header + payload validation and the A/B flip. `payload` is the whole blob
// (VND_HEADER_BYTES + code). On failure `err` names the first bad field.
int vndPatchApply(int slot, const uint8_t *payload, size_t len, bool force);
const char *vndPatchReason(int result);

bool vndPatchRevert(int slot);
int vndPatchRevertAll();

// Journal (SD card; survives reboots). Dump returns the human-readable text.
bool vndPatchJournalDump(String &out);
void vndPatchJournalClear();

// 8-byte image ID used to refuse a payload built for another firmware.
void vndPatchBuildId(uint8_t out[8]);
String vndPatchBuildIdHex();

// One-line overlay description for `patch list`.
String vndPatchOverlayInfo();

// Call a slot's zero-argument probe through the current cell (verification
// aid: it proves the flip without needing the radio to be busy). Only
// rf_scan_step has a probe.
int vndPatchProbe(int slot, uint32_t &value);

// Compute the header crc32 (IEEE, same as Python zlib.crc32) over the code.
uint32_t vndPatchCrc32(const uint8_t *data, size_t len);

#endif
