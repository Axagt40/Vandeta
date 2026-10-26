#include "ir_universal.h"

#include "core/display.h"
#include "core/sd_functions.h"
#include "TV-B-Gone.h" // irSweepPauseIfRequested(), lock/unlock_ir_tx()
#include "custom_ir.h"
#include "ir_ac.h"
#include "ir_utils.h" // setup_ir_pin()
#include <IRutils.h>  // strToDecodeType()
#include <globals.h>

const IrUniversalCategory IR_UNIVERSAL_CATEGORIES[] = {
    {"TV",         "tv.ir"        },
    {"AC",         "ac.ir"        },
    {"Fans",       "fans.ir"      },
    {"Audio",      "audio.ir"     },
    {"Projectors", "projectors.ir"},
    {"LEDs",       "leds.ir"      },
};
const size_t IR_UNIVERSAL_CATEGORY_COUNT =
    sizeof(IR_UNIVERSAL_CATEGORIES) / sizeof(IR_UNIVERSAL_CATEGORIES[0]);

namespace {

// Flipper's universal scenes transmit one candidate per application tick and
// the tick is 100 ms, so back-to-back codes in a sweep are ~100 ms apart. Keep
// that gap: it is what makes a sweep feel (and behave) like Momentum's port.
const uint32_t SWEEP_GAP_MS = 100;

struct Signal {
    String name;
    String type;
    String protocol;
    String address;
    String command;
    String data;
    uint32_t frequency = 0;
    uint8_t bits = 32;

    bool usable() const {
        if (name.length() == 0) return false;
        if (type == "raw") return data.length() > 0 && frequency > 0;
        if (type == "parsed") return protocol.length() > 0;
        return false;
    }

    void reset() { *this = Signal(); }
};

// Read one record. The library format is a header, then records that each start
// with "name:" and end at the next "#" line (or EOF). Long raw data lines are
// the biggest single line in these files (~2 KB) and are read as one String.
bool readRecord(File &f, String &scratch, Signal &s) {
    s.reset();

    while (f.available()) {
        scratch = f.readStringUntil('\n');
        scratch.trim();
        if (scratch.startsWith("name:")) {
            s.name = scratch.substring(5);
            s.name.trim();
            break;
        }
    }
    if (s.name.length() == 0) return false;

    while (f.available()) {
        scratch = f.readStringUntil('\n');
        scratch.trim();
        if (scratch.length() == 0) continue;
        if (scratch.startsWith("#")) break; // end of this record
        if (scratch.startsWith("type:")) {
            s.type = scratch.substring(5);
            s.type.trim();
        } else if (scratch.startsWith("protocol:")) {
            s.protocol = scratch.substring(9);
            s.protocol.trim();
        } else if (scratch.startsWith("address:")) {
            s.address = scratch.substring(8);
            s.address.trim();
        } else if (scratch.startsWith("command:")) {
            s.command = scratch.substring(8);
            s.command.trim();
        } else if (scratch.startsWith("bits:")) {
            s.bits = scratch.substring(5).toInt();
            if (s.bits == 0) s.bits = 32;
        } else if (scratch.startsWith("frequency:")) {
            s.frequency = scratch.substring(10).toInt();
        } else if (scratch.startsWith("data:")) {
            s.data = scratch.substring(5);
            s.data.trim();
        }
        // duty_cycle and anything else is not needed by the sender.
    }
    return true;
}

// Blockwise name scanner. readRecord() is the right tool while a sweep is
// transmitting, but indexing a library with it costs a String plus a
// readStringUntil('\n') for every line - including the ~1.5 KB `data:` lines -
// which measured 2.8 s for ac.ir and 3.4 s for the 257 KB fans.ir before the
// button menu could be drawn. This walks the file in 512-byte blocks and only
// materialises the `name:` values.
class NameScanner {
  public:
    NameScanner(FS *fs, const String &path) {
        if (fs != nullptr) f = (*fs).open(path, FILE_READ);
    }
    ~NameScanner() {
        if (f) f.close();
    }
    bool ok() const { return (bool)f; }

    // Next record name, false at end of file.
    bool next(String &out) {
        if (!f || eof) return false;
        for (;;) {
            if (eof) return false;
            if (atLineStart) {
                if (matchPrefix("name:")) {
                    out = "";
                    int c;
                    while ((c = get()) >= 0 && c != '\n') {
                        if (c != '\r') out += (char)c;
                    }
                    out.trim();
                    return out.length() > 0;
                }
            } else if (get() < 0) {
                return false;
            }
        }
    }

  private:
    File f;
    uint8_t buf[512];
    size_t len = 0;
    size_t pos = 0;
    bool atLineStart = true;
    // A file that does not end in a newline used to leave the scanner spinning
    // on "still at a line start" forever, so EOF has to be latched.
    bool eof = false;

    int get() {
        if (pos >= len) {
            if (!f) {
                eof = true;
                return -1;
            }
            len = f.read(buf, sizeof(buf));
            pos = 0;
            if (len == 0) {
                eof = true;
                return -1;
            }
        }
        uint8_t c = buf[pos++];
        atLineStart = (c == '\n');
        return c;
    }

    // Called at a line start: consume `prefix` when it matches, otherwise drop
    // the rest of the line (that is how `type:`, `data:` ... are skipped).
    bool matchPrefix(const char *prefix) {
        for (size_t i = 0; prefix[i] != '\0'; i++) {
            int c = get();
            if (c < 0) return false;
            if (c != prefix[i]) {
                skipLine();
                return false;
            }
        }
        return true;
    }

    void skipLine() {
        int c;
        while ((c = get()) >= 0) {
            if (c == '\n') return;
        }
    }
};

// `.ir` files store address/command as little-endian byte groups:
// "1C 01 00 00" is 0x0000011C.
uint32_t parseLeBytes(const String &s) {
    uint32_t value = 0;
    uint8_t shift = 0;
    size_t i = 0;
    while (i + 1 < s.length() && shift < 32) {
        if (s[i] == ' ') {
            i++;
            continue;
        }
        char pair[3] = {s[i], s[i + 1], 0};
        value |= ((uint32_t)strtoul(pair, nullptr, 16)) << shift;
        shift += 8;
        i += 2;
    }
    return value;
}

// RCA and NEC42 exist in the Flipper/Momentum libraries but not in
// IRremoteESP8266, so they are built here as raw frames straight from Flipper's
// encoder tables (lib/infrared/encoder_decoder/{rca,nec}/...): same carrier,
// same header, same bit timings, same bit order.
void rawSend(const uint16_t *timings, size_t count, uint16_t khz) {
    IRsend irsend(bruceConfigPins.irTx);
    irsend.begin();
    irsend.sendRaw(timings, count, khz);
}

size_t buildRca(uint32_t address, uint32_t command, uint16_t *raw) {
    // 4-bit address + 8-bit command + inverted halves = 24 bits, LSB first.
    uint32_t data = (address & 0xF) | ((command & 0xFF) << 4) | ((~address & 0xF) << 12) |
                    ((~command & 0xFF) << 16);
    size_t n = 0;
    raw[n++] = 4000;
    raw[n++] = 4000;
    for (uint8_t i = 0; i < 24; i++) {
        raw[n++] = 500;
        raw[n++] = (data >> i) & 1 ? 2000 : 1000;
    }
    return n;
}

size_t buildNec42(uint32_t address, uint32_t command, uint16_t *raw) {
    // 13-bit address + inverted + 8-bit command + inverted = 42 bits, LSB first.
    uint64_t data = (address & 0x1FFF) | ((uint64_t)(~address & 0x1FFF) << 13) |
                    ((uint64_t)(command & 0x3F) << 26) | ((uint64_t)((command & 0xC0) >> 6) << 32) |
                    ((uint64_t)(~command & 0xFF) << 34);
    size_t n = 0;
    raw[n++] = 9000;
    raw[n++] = 4500;
    for (uint8_t i = 0; i < 42; i++) {
        raw[n++] = 560;
        raw[n++] = (data >> i) & 1 ? 1690 : 560;
    }
    return n;
}

// The library files use the Flipper protocol names. sendIRCommand() has a
// dedicated encoder for the NEC family and falls back to IRremoteESP8266 for
// anything else, so "supported" is exactly "one of those, or the two above".
bool protocolSupported(const String &protocol) {
    static const char *dedicated[] = {"NEC",     "NECext", "RC5",    "RC5X",    "RC6",
                                      "Samsung32", "SIRC", "SIRC15", "SIRC20", "Kaseikyo",
                                      "RCA",     "NEC42"};
    for (const char *p : dedicated) {
        if (protocol.equalsIgnoreCase(p)) return true;
    }
    return strToDecodeType(protocol.c_str()) != decode_type_t::UNKNOWN;
}

bool transmit(const Signal &s) {
    if (s.type == "parsed" && !protocolSupported(s.protocol)) return false;

    if (s.type == "parsed" && s.protocol.equalsIgnoreCase("RCA")) {
        uint16_t raw[2 + 48];
        rawSend(raw, buildRca(parseLeBytes(s.address), parseLeBytes(s.command), raw), 38);
        return true;
    }
    if (s.type == "parsed" && s.protocol.equalsIgnoreCase("NEC42")) {
        uint16_t raw[2 + 84];
        rawSend(raw, buildNec42(parseLeBytes(s.address), parseLeBytes(s.command), raw), 38);
        return true;
    }

    IRCode code;
    code.name = s.name;
    code.type = s.type;
    code.frequency = (uint16_t)s.frequency;
    code.bits = s.bits;
    if (s.type == "raw") {
        code.data = s.data;
    } else {
        code.protocol = s.protocol;
        code.address = s.address;
        code.command = s.command;
    }
    // hideDefaultUI=true: the sweep draws its own progress, not a banner per code.
    lock_ir_tx();
    sendIRCommand(&code, true);
    unlock_ir_tx();
    return true;
}

} // namespace

// Diagnostic for the two hand-encoded protocols: the frame the sender will put
// on the wire, space separated microseconds.
String irUniversalRawFrame(const String &protocol, uint32_t address, uint32_t command) {
    uint16_t raw[2 + 84];
    size_t n = 0;
    if (protocol.equalsIgnoreCase("RCA")) n = buildRca(address, command, raw);
    else if (protocol.equalsIgnoreCase("NEC42")) n = buildNec42(address, command, raw);
    else return "";

    String out;
    for (size_t i = 0; i < n; i++) {
        if (i) out += ' ';
        out += String(raw[i]);
    }
    return out;
}

String irUniversalPath(const char *file) {
    String p = String(IR_UNIVERSAL_DIR) + "/" + String(file);
    return p;
}

bool irUniversalAvailable(FS *fs, const char *file) {
    if (fs == nullptr) return false;
    return (*fs).exists(irUniversalPath(file));
}

std::vector<IrUniversalButton> irUniversalButtonList(FS *fs, const String &path) {
    std::vector<IrUniversalButton> out;
    NameScanner sc(fs, path);
    if (!sc.ok()) return out;
    out.reserve(16);

    String name;
    while (sc.next(name)) {
        bool seen = false;
        for (IrUniversalButton &b : out) {
            if (b.name == name) {
                b.count++;
                seen = true;
                break;
            }
        }
        if (!seen && out.size() < 64) out.push_back({name, 1});
    }
    return out;
}

std::vector<String> irUniversalButtons(FS *fs, const String &path) {
    std::vector<String> names;
    for (const IrUniversalButton &b : irUniversalButtonList(fs, path)) names.push_back(b.name);
    return names;
}

int irUniversalSend(
    FS *fs, const String &path, const String &button, int maxSends, bool showUI, volatile bool *stop,
    int totalHint
) {
    if (fs == nullptr) return 0;

    File f = (*fs).open(path, FILE_READ);
    if (!f) {
        Serial.println("ir universal: cannot open " + path);
        return 0;
    }

    // Size the progress bar without a second pass over the library: the button
    // menu already knows how many codes carry each name. The old code rescanned
    // the whole file before the first code went out, which is why a sweep felt
    // like it had not started. Fall back to the blockwise list when no hint is
    // given (console callers do not have one).
    int total = totalHint;
    if (total <= 0) {
        for (const IrUniversalButton &b : irUniversalButtonList(fs, path)) {
            if (b.name == button) {
                total = b.count;
                break;
            }
        }
    }
    if (total <= 0) total = 1; // keep map() away from a zero range

    setup_ir_pin(bruceConfigPins.irTx, OUTPUT);
    init_ir_tx_mutex(); // the same lock TV-B-Gone takes, so sweeps cannot overlap

    int sent = 0, skipped = 0;
    String scratch;
    Signal s;
    int index = 0;
    while (readRecord(f, scratch, s)) {
        if (!s.usable() || s.name != button) continue;
        if (stop != nullptr && *stop) break;
        if (maxSends > 0 && sent >= maxSends) break;

        // An AC sweep is 30+ long frames (585 timings each) and easily runs
        // past the dimmer timeout, and the sweep itself presses no key - so
        // checkPowerSaveTime() dimmed and then blanked the panel in the middle
        // of it. That is the "blank screen when going through the AC signals".
        // Hold the idle timer off for as long as the sweep is transmitting.
        previousMillis = millis();
        if (showUI) {
            progressHandler(index, total, button + " " + String(index + 1) + "/" + String(total));
        }
        if (transmit(s)) sent++;
        else skipped++;
        index++;
        previousMillis = millis();

        // Same keys as TV-B-Gone: SEL pauses, ESC cancels. On the console the
        // sweep just runs to the end (or `max`).
        if (showUI && irSweepPauseIfRequested()) break;
        delay(SWEEP_GAP_MS);
    }
    f.close();

    if (skipped) Serial.println("ir universal: " + String(skipped) + " codes skipped");
    Serial.println("ir universal: " + String(sent) + "/" + String(total) + " sent for '" + button + "'");
    return sent;
}

// ---------------------------------------------------------------------------
// Single candidates, for the Vandeta IR Suite script
// ---------------------------------------------------------------------------
// irUniversalSend() above answers "sweep this button". A bruter needs the codes
// one at a time: send #N, remember what #N was, replay the last few. These
// three helpers expose exactly that without duplicating the record parser.

// Random access used to be the slow part of a brute force: every call re-read
// the library from the first record, so code 300 cost a full pass (measured
// ~1.2 s on the 99 KB tv.ir) while code 20 cost almost nothing - a sweep got
// slower the further it walked, which made it feel far slower than the on-device
// universal remote, whose sweep reads the file once front to back.
//
// Now the library is read into RAM **once** (PSRAM when the board has it), the
// record start offsets are taken from that copy, and every candidate is parsed
// out of RAM. Measured on the board this took the first send from 1,680 ms to
// ~130 ms (the indexing pass no longer walks the card byte by byte) and removed
// the ~700 ms stalls that showed up mid-sweep when the FAT driver hiccuped: a
// sweep now touches the card zero times after the first code.
//
// tv.ir is 99 KB and offsets are 4 bytes per candidate, so the whole index for
// the 295 Power codes costs ~100 KB of PSRAM, of which the board has 7.8 MB.
struct ButtonIndex {
    String path;
    String button;
    size_t fileSize = 0;
    uint8_t *blob = nullptr;      // the library as read, NUL terminated
    size_t blobLen = 0;
    std::vector<uint32_t> offset; // record start offsets inside blob
};

static ButtonIndex s_buttonIndex;

static void freeIndex() {
    if (s_buttonIndex.blob != nullptr) {
        free(s_buttonIndex.blob);
        s_buttonIndex.blob = nullptr;
    }
    s_buttonIndex.blobLen = 0;
    s_buttonIndex.offset.clear();
    s_buttonIndex.path = "";
    s_buttonIndex.button = "";
    s_buttonIndex.fileSize = 0;
}

// One line out of the RAM copy, trimmed, NUL-free. Same rules the file reader
// uses, so a record parses identically either way.
static bool memLine(const uint8_t *b, size_t len, size_t &pos, String &out) {
    if (pos >= len) return false;
    size_t start = pos;
    while (pos < len && b[pos] != '\n') pos++;
    out = String((const char *)(b + start), (unsigned)(pos - start));
    if (pos < len) pos++;
    out.trim();
    return true;
}

// readRecord() over the RAM copy.
static bool readRecordMem(const uint8_t *b, size_t len, size_t &pos, Signal &s) {
    s.reset();
    String line;
    bool have = false;
    while (memLine(b, len, pos, line)) {
        if (line.startsWith("name:")) {
            s.name = line.substring(5);
            s.name.trim();
            have = true;
            break;
        }
    }
    if (!have || s.name.length() == 0) return false;

    while (memLine(b, len, pos, line)) {
        if (line.length() == 0) continue;
        if (line.startsWith("#")) break;
        if (line.startsWith("type:")) {
            s.type = line.substring(5);
            s.type.trim();
        } else if (line.startsWith("protocol:")) {
            s.protocol = line.substring(9);
            s.protocol.trim();
        } else if (line.startsWith("address:")) {
            s.address = line.substring(8);
            s.address.trim();
        } else if (line.startsWith("command:")) {
            s.command = line.substring(8);
            s.command.trim();
        } else if (line.startsWith("bits:")) {
            s.bits = line.substring(5).toInt();
            if (s.bits == 0) s.bits = 32;
        } else if (line.startsWith("frequency:")) {
            s.frequency = line.substring(10).toInt();
        } else if (line.startsWith("data:")) {
            s.data = line.substring(5);
            s.data.trim();
        }
    }
    return true;
}

// Load the library into RAM and index every usable record named `button`.
static bool indexButton(FS *fs, const String &path, const String &button) {
    freeIndex();
    if (fs == nullptr) return false;

    s_buttonIndex.path = path;
    s_buttonIndex.button = button;

    File f = (*fs).open(path, FILE_READ);
    if (!f) return false;
    size_t size = f.size();

    uint8_t *buf = (uint8_t *)(psramFound() ? ps_malloc(size + 1) : malloc(size + 1));
    if (buf == nullptr) {
        f.close();
        return false;
    }
    size_t got = f.read(buf, size);
    f.close();
    buf[got] = 0;

    s_buttonIndex.blob = buf;
    s_buttonIndex.blobLen = got;
    s_buttonIndex.fileSize = got;

    size_t pos = 0;
    Signal s;
    while (pos < s_buttonIndex.blobLen) {
        size_t start = pos;
        if (!readRecordMem(s_buttonIndex.blob, s_buttonIndex.blobLen, pos, s)) break;
        if (s.usable() && s.name == button) s_buttonIndex.offset.push_back((uint32_t)start);
    }
    return !s_buttonIndex.offset.empty();
}

// True when the RAM copy already covers this library+button.
static bool buttonIndexReady(const String &path, const String &button) {
    return s_buttonIndex.blob != nullptr && s_buttonIndex.path == path && s_buttonIndex.button == button;
}

static void copyRecord(const Signal &s, uint16_t index, IrUniversalCode &out) {
    out.name = s.name;
    out.type = s.type;
    out.protocol = s.protocol;
    out.address = s.address;
    out.command = s.command;
    out.data = s.data;
    out.bits = s.bits;
    out.frequency = s.frequency;
    out.index = index;
}

// One record, parsed out of the RAM copy. No card access at all once indexed.
static bool readCandidate(FS *fs, const String &path, const String &button, int index, Signal &s) {
    if (fs == nullptr || index < 0) return false;
    if (!buttonIndexReady(path, button) && !indexButton(fs, path, button)) return false;
    if (index >= (int)s_buttonIndex.offset.size()) return false;

    size_t pos = s_buttonIndex.offset[index];
    return readRecordMem(s_buttonIndex.blob, s_buttonIndex.blobLen, pos, s) && s.usable() &&
           s.name == button;
}

bool irUniversalCodeAt(FS *fs, const String &path, const String &button, int index, IrUniversalCode &out) {
    Signal s;
    if (!readCandidate(fs, path, button, index, s)) return false;
    copyRecord(s, (uint16_t)index, out);
    return true;
}

bool irUniversalSendAt(FS *fs, const String &path, const String &button, int index) {
    Signal s;
    if (!readCandidate(fs, path, button, index, s)) return false;

    setup_ir_pin(bruceConfigPins.irTx, OUTPUT);
    init_ir_tx_mutex();
    previousMillis = millis(); // do not dim mid-sweep
    bool sent = transmit(s);
    previousMillis = millis();
    return sent;
}

std::vector<IrUniversalCode> irUniversalFamily(
    FS *fs, const String &path, const String &protocol, const String &address, int max
) {
    std::vector<IrUniversalCode> out;
    if (fs == nullptr || protocol.length() == 0) return out;
    File f = (*fs).open(path, FILE_READ);
    if (!f) return out;

    String scratch;
    Signal s;
    uint16_t seen = 0;
    while (readRecord(f, scratch, s)) {
        if (!s.usable()) continue;
        seen++;
        if (!s.protocol.equalsIgnoreCase(protocol)) continue;
        // The library writes the address as four space separated LE bytes. Compare
        // it the way it is stored, trimmed and case-insensitively.
        if (!s.address.equalsIgnoreCase(address)) continue;
        IrUniversalCode c;
        copyRecord(s, seen, c);
        out.push_back(c);
        if (max > 0 && (int)out.size() >= max) break;
    }
    f.close();
    return out;
}

// The button list of one library file (the classic universal-remote sweep).
static void irUniversalLibraryMenu(FS *fs, const IrUniversalCategory &c) {
    // One blockwise pass: names *and* their candidate counts, so the sweep can
    // show "n/total" at once.
    std::vector<IrUniversalButton> names = irUniversalButtonList(fs, irUniversalPath(c.file));
    if (names.empty()) {
        displayError("No codes in " + String(c.label), true);
        return;
    }
    std::vector<Option> buttons;
    for (const IrUniversalButton &n : names) {
        String name = n.name;
        int count = n.count;
        buttons.push_back({name, [fs, c, name, count]() {
                               irUniversalSend(fs, irUniversalPath(c.file), name, 0, true, nullptr, count);
                           }});
    }
    buttons.push_back({"Back", []() {}});
    loopOptions(buttons, MENU_TYPE_SUBMENU, c.label);
}

void irUniversalMenu() {
    FS *fs = nullptr;
    if (!getFsStorage(fs) || fs == nullptr) {
        displayError("No storage", true);
        return;
    }

    std::vector<Option> categories;
    for (size_t i = 0; i < IR_UNIVERSAL_CATEGORY_COUNT; i++) {
        const IrUniversalCategory &c = IR_UNIVERSAL_CATEGORIES[i];
        bool present = irUniversalAvailable(fs, c.file);
        String label = String(c.label) + (present ? "" : " (missing)");
        categories.push_back({label, [fs, c, present]() {
                                  // Air conditioners are not button remotes: every
                                  // frame carries power+mode+temp+fan. The AC page
                                  // therefore also offers per-brand remotes built
                                  // from IRremoteESP8266's AC classes; the library
                                  // entry stays for the captured model.
                                  if (strcmp(c.label, "AC") == 0) {
                                      std::vector<Option> acPage = {
                                          {"Brand remotes", []() { irAcMenu(); }},
                                          {"Library codes",
                                           [fs, c, present]() {
                                               if (!present) {
                                                   displayError(
                                                       String(c.label) + " library not on the card", true
                                                   );
                                                   return;
                                               }
                                               irUniversalLibraryMenu(fs, c);
                                           }                                    },
                                          {"Back", []() {}},
                                      };
                                      loopOptions(acPage, MENU_TYPE_SUBMENU, "AC");
                                      return;
                                  }
                                  if (!present) {
                                      displayError(String(c.label) + " library not on the card", true);
                                      return;
                                  }
                                  irUniversalLibraryMenu(fs, c);
                              }});
    }
    categories.push_back({"Back", []() {}});
    loopOptions(categories, MENU_TYPE_SUBMENU, "Universal Remote");
}
