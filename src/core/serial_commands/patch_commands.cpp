#include "patch_commands.h"

#include "core/dev/iram_patch.h"
#include "core/dev/tunables.h"
#include <globals.h>

// Task 1, phase 2: `patch` selects a precompiled variant by index.
// Task 1, phase 3: the same verb applies/reverts IRAM code payloads.
//
// One command with a `mode` argument, not `patch/list`: this CLI matches whole
// command words, so a slash form never routes (phase 1 learned that with
// `tune`). Same shape as `tune`: mode plus positional arguments.
//
// The spec names the three tables `rf_scan`, `ir_repeat` and `nrf_capture`; the
// tunables table uses the dotted namespace of its other entries
// (`rf.scan_variant`, ...). This map is the only place the two spellings meet -
// there is still one value, in one entry, with one conf key.
//
// A selection is saved immediately: there is no `patch save` verb, and a variant
// that vanished on reboot would not be worth selecting. `tune set` on the same
// entry stays available for scripting, and `tune save` then persists it.
//
// Phase 3 modes are the spec's four (`apply|revert|clear|journal`) plus `probe`,
// which calls a patchable slot through its live cell. The probe exists because
// the acceptance ("confirm the new timing on the next sweep") needs a shape a
// quiet bench can observe without waiting for a carrier; it is the same call
// the real consumer makes, not a parallel path.

typedef struct {
    const char *name; // the spec's short name
    const char *tune; // the tunables entry that owns the index
} variant_table_t;

static const variant_table_t g_variantTables[] = {
    {"rf_scan", "rf.scan_variant"},
    {"ir_repeat", "ir.repeat_variant"},
    {"nrf_capture", "nrf.capture_variant"},
};

static const int g_variantTableCount = (int)(sizeof(g_variantTables) / sizeof(g_variantTables[0]));

static bool patchAllowed() { return bruceConfig.usbDebug != 0; }

static void patchPrintList() {
    for (int i = 0; i < g_variantTableCount; i++) {
        const tune_entry *e = tuneFind(g_variantTables[i].tune);
        if (e == nullptr || !(e->flags & TUNE_ENUM)) continue;
        const char *cur = tuneVariantName(e);
        String line = String(g_variantTables[i].name) + " = " + String(tuneGet(e)) + " " + (cur ? cur : "?") +
                      " [";
        for (int v = 0; v < tuneVariantCount(e); v++) {
            if (v) line += "|";
            line += tuneVariantNameAt(e, v);
        }
        serialDevice->println(line + "]");
    }

    // Phase 3: the code slots live in the same listing, never a second verb.
    serialDevice->println("build " + vndPatchBuildIdHex());
    serialDevice->println(vndPatchOverlayInfo());
    for (int i = 0; i < vndPatchSlotCount(); i++) {
        serialDevice->println(
            String(vndPatchSlotName(i)) + " slot " + String(i) +
            (vndPatchSlotApplied(i) ? " patched" : " body")
        );
    }
}

// Accepts either the short spec name or the tune entry name, so a reader coming
// from `tune list` does not have to know this alias exists.
static const tune_entry *patchFind(const String &name) {
    const tune_entry *e = tuneFind(name.c_str());
    if (e != nullptr && (e->flags & TUNE_ENUM)) return e;
    for (int i = 0; i < g_variantTableCount; i++) {
        if (name == g_variantTables[i].name) {
            e = tuneFind(g_variantTables[i].tune);
            if (e != nullptr && (e->flags & TUNE_ENUM)) return e;
        }
    }
    return nullptr;
}

static bool isHexString(const String &s) {
    if (s.length() == 0 || (s.length() & 1) != 0) return false;
    const char *p = s.c_str();
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) return false; // not a payload spelling
    for (size_t i = 0; i < s.length(); i++) {
        char c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Loads a payload from a hex string or from a file on SD. Returns the number of
// bytes or -1, with `err` set.
static int patchLoadPayload(const String &src, uint8_t *buf, size_t cap, String &err) {
    if (isHexString(src)) {
        if (src.length() / 2 > cap) {
            err = "payload too large";
            return -1;
        }
        for (size_t i = 0; i < src.length(); i += 2) {
            int hi = hexNibble(src[i]);
            int lo = hexNibble(src[i + 1]);
            if (hi < 0 || lo < 0) {
                err = "bad hex";
                return -1;
            }
            buf[i / 2] = (uint8_t)((hi << 4) | lo);
        }
        return (int)(src.length() / 2);
    }

    if (!sdcardMounted) {
        err = "not a hex payload and no SD card for a file path";
        return -1;
    }
    File f = SD.open(src, FILE_READ);
    if (!f) {
        err = "cannot open " + src;
        return -1;
    }
    const size_t n = f.size();
    if (n > cap) {
        f.close();
        err = "payload too large";
        return -1;
    }
    const int got = (int)f.read(buf, n);
    f.close();
    if (got <= 0) {
        err = "empty payload";
        return -1;
    }
    return got;
}

static bool patchOverlayMode(const String &mode, const String &arg1, const String &arg2, const String &arg3) {
    if (mode == "apply") {
        const int slot = vndPatchSlotFind(arg1.c_str());
        if (slot < 0) {
            serialDevice->println("ERR: unknown slot '" + arg1 + "' (try 'patch list')");
            return true;
        }
        if (arg2.length() == 0) {
            serialDevice->println("ERR: patch apply <slot> <file|hex> [--force]");
            return true;
        }
        // heap, not .bss: the cap is one 4 KB flash slot plus the header, and
        // putting that in every build would regress the non-dev image by ~3.5 KB
        const size_t cap = VND_HEADER_BYTES + VND_PAYLOAD_MAX + 64;
        uint8_t *payload = (uint8_t *)malloc(cap);
        if (payload == nullptr) {
            serialDevice->println("ERR: out of memory for payload");
            return true;
        }
        String err;
        const int n = patchLoadPayload(arg2, payload, cap, err);
        if (n < 0) {
            serialDevice->println("ERR: " + err);
            free(payload);
            return true;
        }
        const bool force = arg3 == "--force" || arg3 == "force";
        const int rc = vndPatchApply(slot, payload, (size_t)n, force);
        if (rc != VND_OK) {
            serialDevice->println("ERR: " + String(vndPatchReason(rc)));
            free(payload);
            return true;
        }
        uint16_t len = (uint16_t)payload[14] | ((uint16_t)payload[15] << 8);
        uint32_t crc = (uint32_t)payload[16] | ((uint32_t)payload[17] << 8) |
                       ((uint32_t)payload[18] << 16) | ((uint32_t)payload[19] << 24);
        serialDevice->println(
            "applied " + String(vndPatchSlotName(slot)) + " slot " + String(slot) + " len " + String(len) +
            " crc " + String(crc, HEX) + " (live, journaled)"
        );
        free(payload);
        return true;
    }

    if (mode == "revert") {
        if (arg1 == "all" || arg1.length() == 0) {
            const int n = vndPatchRevertAll();
            if (n > 0) vndPatchJournalClear();
            serialDevice->println("reverted " + String(n) + " patch(es)");
            return true;
        }
        const int slot = vndPatchSlotFind(arg1.c_str());
        if (slot < 0) {
            serialDevice->println("ERR: unknown slot '" + arg1 + "' (try 'patch list')");
            return true;
        }
        const bool was = vndPatchRevert(slot);
        serialDevice->println(
            String(was ? "reverted " : "already at body ") + vndPatchSlotName(slot)
        );
        return true;
    }

    if (mode == "clear") {
        const int n = vndPatchRevertAll();
        vndPatchJournalClear();
        serialDevice->println(
            "patches cleared (" + String(n) + " reverted), journal emptied"
        );
        return true;
    }

    if (mode == "journal") {
        String out;
        vndPatchJournalDump(out);
        serialDevice->println(out);
        return true;
    }

    if (mode == "probe") {
        const int slot = vndPatchSlotFind(arg1.c_str());
        if (slot < 0) {
            serialDevice->println("ERR: unknown slot '" + arg1 + "' (try 'patch list')");
            return true;
        }
        uint32_t value = 0;
        const int rc = vndPatchProbe(slot, value);
        if (rc != VND_OK) {
            serialDevice->println("ERR: " + String(vndPatchSlotName(slot)) + " has no probe");
            return true;
        }
        serialDevice->println("probe " + String(vndPatchSlotName(slot)) + " = " + String(value));
        return true;
    }

    return false;
}

uint32_t patchCallback(cmd *c) {
    if (!patchAllowed()) {
        serialDevice->println("ERR: developer mode off");
        return true;
    }

    Command cmd(c);
    String mode = cmd.getArgument("mode").getValue();
    String arg1 = cmd.getArgument("a").getValue();
    String arg2 = cmd.getArgument("b").getValue();
    String arg3 = cmd.getArgument("c").getValue();
    mode.trim();
    arg1.trim();
    arg2.trim();
    arg3.trim();

    if (mode.length() == 0 || mode == "list") {
        patchPrintList();
        return true;
    }

    if (mode == "apply" || mode == "revert" || mode == "clear" || mode == "journal" || mode == "probe") {
        return patchOverlayMode(mode, arg1, arg2, arg3);
    }

    const tune_entry *e = patchFind(mode);
    if (e == nullptr) {
        serialDevice->println(
            "ERR: unknown variant table '" + mode + "' (try 'patch list', or apply|revert|clear|journal|probe)"
        );
        return false;
    }
    if (arg1.length() == 0) {
        serialDevice->println("ERR: patch <name> <index> (or 'patch list')");
        return false;
    }

    // An index is a choice, not a magnitude: a typo must be refused rather than
    // clamped onto the neighbouring variant the way `tune set` clamps a scalar.
    const int n = tuneVariantCount(e);
    const int32_t idx = arg1.toInt();
    if (arg1 != String(idx) || idx < 0 || idx >= n) {
        serialDevice->println("ERR: index must be 0.." + String(n - 1) + " for " + mode);
        return false;
    }

    bool clamped = false;
    tuneSet((tune_entry *)e, idx, &clamped);
    bruceConfig.saveFile(); // a selection has to survive the reboot to be useful
    serialDevice->println(String(mode) + " = " + String(idx) + " " + tuneVariantName(e) + " (live, saved)");
    return true;
}

void createPatchCommands(SimpleCLI *cli) {
    Command patch = cli->addCommand("patch", patchCallback);
    patch.addPosArg("mode", "");
    patch.addPosArg("a", "");
    patch.addPosArg("b", "");
    patch.addPosArg("c", "");
}
