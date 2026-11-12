#include "devflash_commands.h"

#include "core/dev/build_info.h"
#include "core/dev/devflash.h"
#include <globals.h>

// Task 1, phase 4: `devflash` moves an app-only image into the inactive OTA
// partition. It is one command with a `mode` argument, like `tune` and `patch`:
// this CLI matches whole command words, so a slash form never routes.
//
// The text form (`chunk <offset> <hex>`) works on any link, including the UART
// where the framed protocol does not exist. The fast path is the framed one in
// usbdebug.cpp (FR_OTA_*), which the host tool uses over USB.

static bool devflashAllowed() { return bruceConfig.usbDebug != 0; }

static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parseU32(const String &s, uint32_t &out) {
    if (s.length() == 0) return false;
    const char *p = s.c_str();
    char *end = nullptr;
    unsigned long v = strtoul(p, &end, 0);
    if (end == p || (end != nullptr && *end != 0)) return false;
    out = (uint32_t)v;
    return true;
}

// 16 hex chars -> the 8-byte folded build ID. Anything else is "not given".
static bool parseBuildId(const String &s, uint8_t out[8]) {
    if (s.length() != 16) return false;
    for (int i = 0; i < 8; i++) {
        const int hi = nibble(s[i * 2]);
        const int lo = nibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static uint32_t devflashCallback(cmd *c) {
    Command cmd(c);
    String mode = cmd.getArgument(0).getValue();
    mode.trim();

    if (!devflashAllowed()) {
        serialDevice->println("ERR: developer mode off");
        return true;
    }

    if (mode.length() == 0 || mode == "status") {
        serialDevice->println("build " + String(vndBuildId()));
        serialDevice->println(
            String("devflash ") + (DevFlash::active() ? "open" : "idle") + " " + String(DevFlash::written()) +
            "/" + String(DevFlash::expectedSize()) + " bytes"
        );
        serialDevice->println("devflash begin <size> <crc32> [buildid] [--force] | chunk <offset> <hex> | end | abort");
        return true;
    }

    if (mode == "begin") {
        uint32_t size = 0, crc = 0;
        if (!parseU32(cmd.getArgument(1).getValue(), size) || !parseU32(cmd.getArgument(2).getValue(), crc)) {
            serialDevice->println("ERR: devflash begin <size> <crc32> [buildid] [--force]");
            return true;
        }
        String idArg = cmd.getArgument(3).getValue();
        String forceArg = cmd.getArgument(4).getValue();
        idArg.trim();
        forceArg.trim();
        uint8_t id[8];
        const bool haveId = parseBuildId(idArg, id);
        const bool force = forceArg == "--force" || forceArg == "force";
        String msg;
        DevFlash::begin(size, crc, haveId ? id : nullptr, force, msg);
        serialDevice->println(msg);
        return true;
    }

    if (mode == "chunk") {
        uint32_t offset = 0;
        if (!parseU32(cmd.getArgument(1).getValue(), offset)) {
            serialDevice->println("ERR: devflash chunk <offset> <hex>");
            return true;
        }
        String hex = cmd.getArgument(2).getValue();
        hex.trim();
        if (hex.length() == 0 || (hex.length() & 1) != 0) {
            serialDevice->println("ERR: chunk payload must be an even-length hex string");
            return true;
        }
        if (hex.length() > 4096) {
            serialDevice->println("ERR: chunk too large (max 2048 bytes)");
            return true;
        }
        static uint8_t buf[2048];
        const size_t n = hex.length() / 2;
        for (size_t i = 0; i < n; i++) {
            const int hi = nibble(hex[i * 2]);
            const int lo = nibble(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                serialDevice->println("ERR: bad hex in chunk");
                return true;
            }
            buf[i] = (uint8_t)((hi << 4) | lo);
        }
        String msg;
        DevFlash::writeChunk(offset, buf, n, msg);
        serialDevice->println(msg);
        return true;
    }

    if (mode == "end") {
        String msg;
        DevFlash::end(msg);
        serialDevice->println(msg);
        return true;
    }

    if (mode == "abort") {
        String msg;
        DevFlash::abort(msg);
        serialDevice->println(msg);
        return true;
    }

    serialDevice->println("ERR: devflash begin|chunk|end|abort|status");
    return true;
}

void createDevFlashCommands(SimpleCLI *cli) {
    Command devflash = cli->addCommand("devflash", devflashCallback);
    devflash.addPosArg("mode", "status");
    devflash.addPosArg("a", "");
    devflash.addPosArg("b", "");
    devflash.addPosArg("c", "");
    devflash.addPosArg("d", "");
}
