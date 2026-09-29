#include "usbdebug.h"

#include "core/sd_functions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <globals.h>

/*********************************************************************
**  Developer Options engine (previously "USB Debugging")
**
**  Everything in here is inert unless bruceConfig.usbDebug is set. The
**  module never owns the console: it borrows it.
**    - text mode : UsbDebug::pollSerial() returns false immediately, so
**                  the stock SimpleCLI path (src/core/serialcmds.cpp) keeps
**                  reading the port exactly as before. This matters because
**                  `storage write` and `js run_from_buffer` take the port
**                  over in raw mode.
**    - binary mode: UsbDebug::pollSerial() owns the port and parses framed
**                  messages, and the stock path is skipped for that byte run.
*********************************************************************/

namespace UsbDebug {

// ---------------------------------------------------------------------------
// Binary protocol constants
// ---------------------------------------------------------------------------
#define USBDBG_MAGIC0 0xA5
#define USBDBG_MAGIC1 0x5A
// A frame is 5 header bytes + payload + 2 CRC bytes. USBDBG_MAX_PAYLOAD must
// stay above USBDBG_CHUNK + 4 (the 4 byte offset a data chunk carries) or the
// transfer frames get truncated. Both must fit the USB-CDC RX queue that
// begin() resizes to USBDBG_RX_QUEUE.
#define USBDBG_MAX_PAYLOAD 2100
#define USBDBG_CHUNK 2048
// The RX queue only has to hold one whole frame plus a little slack: the
// protocol is request/response, so the host never has two frames in flight.
// A 8192 byte queue worked but wasted ~5.8 KB of internal RAM, which is the
// resource Wi-Fi/BLE bring-up competes for on this board.
#define USBDBG_RX_QUEUE (USBDBG_MAX_PAYLOAD + 256)

// request types (responses are request | 0x80)
enum : uint8_t {
    FR_PING = 0x01,
    FR_EXEC = 0x02,
    FR_PUT_BEGIN = 0x04,
    FR_PUT_DATA = 0x05,
    FR_PUT_END = 0x06,
    FR_GET_BEGIN = 0x07,
    FR_GET_DATA = 0x08,
    FR_SET_MODE = 0x09, // 0 = text, 1 = binary
    FR_EXEC_BATCH = 0x0A,
    FR_INFO = 0x0B,
};

enum : uint8_t {
    FR_MSG = 0x81,        // generic message / pong / error text
    FR_EXEC_OK = 0x82,    // argv executed
    FR_PUT_ACK = 0x85,    // one data chunk accepted
    FR_PUT_DONE = 0x86,   // upload finished
    FR_GET_INFO = 0x87,   // download started
    FR_GET_DATA_R = 0x88, // one data chunk
    FR_GET_DONE = 0x89,   // download finished
    FR_MODE_R = 0x8A,     // protocol switch acknowledged
    FR_INFO_R = 0x8B,     // key=value system information
};

const char *UsbDebug_verbs =
    "cd pwd usbdebug batch queue history complete hotreload liveupdate errors log proc sys mem proto";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static String s_cwd = "/";
static bool s_batchMode = false;
static bool s_batchBusy = false;
static bool s_hotReload = false;
static bool s_errorStream = false;
static bool s_binary = false;

#define USBDBG_HISTORY 16
static String s_history[USBDBG_HISTORY];
static size_t s_histHead = 0;  // next slot to write
static size_t s_histCount = 0; // filled slots

#define USBDBG_QUEUE_MAX 32
static String s_queue[USBDBG_QUEUE_MAX];
static size_t s_queueCount = 0;

#define USBDBG_WATCH_MAX 8
struct Watch {
    String path;
    size_t size = 0;
    time_t when = 0;
    bool valid = false;
};
static Watch s_watches[USBDBG_WATCH_MAX];
static size_t s_watchCount = 0;
static uint32_t s_lastPoll = 0;

#define USBDBG_ERR_LINES 24
#define USBDBG_ERR_LEN 180
static char s_errRing[USBDBG_ERR_LINES][USBDBG_ERR_LEN];
static volatile size_t s_errHead = 0;   // next slot to write
static volatile size_t s_errCount = 0;  // valid lines
static volatile size_t s_errStreamed = 0; // how many already printed
static portMUX_TYPE s_errMux = portMUX_INITIALIZER_UNLOCKED;

// binary parser
static uint8_t s_rxBuf[USBDBG_MAX_PAYLOAD + 8];
static size_t s_rxLen = 0;
static size_t s_rxWant = 0;

static File s_putFile;
static String s_putPath;
static uint32_t s_putSize = 0;
static uint32_t s_putWritten = 0;

static bool s_logHookInstalled = false;
static vprintf_like_t s_prevLog = nullptr;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static void outln(const String &s) { serialDevice->println(s); }

// Normalise an absolute path: collapse //, /./ and resolve /../
static String normalize(const String &in) {
    String path = in;
    if (path.length() == 0) return "/";
    if (!path.startsWith("/")) path = "/" + path;

    std::vector<String> parts;
    int start = 1;
    while (start <= (int)path.length()) {
        int slash = path.indexOf('/', start);
        String seg = slash < 0 ? path.substring(start) : path.substring(start, slash);
        if (seg.length() > 0 && seg != ".") {
            if (seg == "..") {
                if (parts.size() > 0) parts.pop_back();
            } else {
                parts.push_back(seg);
            }
        }
        if (slash < 0) break;
        start = slash + 1;
    }

    String result;
    for (size_t i = 0; i < parts.size(); i++) result += "/" + parts[i];
    if (result.length() == 0) result = "/";
    return result;
}

static uint16_t crc16Update(uint16_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
        }
    }
    return crc;
}

static uint16_t crc16(const uint8_t *data, size_t len) { return crc16Update(0xFFFF, data, len); }

static void put16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static void put32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------------------
// Error / log ring (fed from the ESP log hook, safe from any task)
// ---------------------------------------------------------------------------
static void recordLogLine(const char *line) {
    if (!line || !line[0]) return;
    // Only keep things that look like failures: ESP-IDF prefixes errors with
    // "E (" on the formatted line; plain text hits are matched too.
    bool isErr = (line[0] == 'E' && (line[1] == ' ' || line[1] == '(')) ||
                 strstr(line, "ERROR") || strstr(line, "error:") || strstr(line, "abort") ||
                 strstr(line, "assert");
    if (!isErr) return;

    portENTER_CRITICAL(&s_errMux);
    size_t slot = s_errHead;
    strncpy(s_errRing[slot], line, USBDBG_ERR_LEN - 1);
    s_errRing[slot][USBDBG_ERR_LEN - 1] = '\0';
    s_errHead = (s_errHead + 1) % USBDBG_ERR_LINES;
    if (s_errCount < USBDBG_ERR_LINES) s_errCount = s_errCount + 1;
    portEXIT_CRITICAL(&s_errMux);
}

static int usbDebugLogVprintf(const char *fmt, va_list args) {
    // Cheap gate first: when the mode is off this costs one global read per
    // log line and no formatting at all.
    if (bruceConfig.usbDebug != 0) {
        char buf[USBDBG_ERR_LEN];
        va_list copy;
        va_copy(copy, args);
        int n = vsnprintf(buf, sizeof(buf), fmt, copy);
        va_end(copy);
        if (n > 0) recordLogLine(buf);
    }
    if (s_prevLog) return s_prevLog(fmt, args);
    return vprintf(fmt, args);
}

static void installLogHook() {
    if (s_logHookInstalled) return;
    s_prevLog = esp_log_set_vprintf(usbDebugLogVprintf);
    s_logHookInstalled = true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
bool enabled() { return bruceConfig.usbDebug != 0; }

bool devMode() { return bruceConfig.usbDebugDevMode != 0; }

void applyDevMode(bool on) {
    if (on) {
        setCpuFrequencyMhz(240); // speed over stability
        outln("[USBDBG] developer mode ON (cpu=" + String(getCpuFrequencyMhz()) + "MHz)");
    } else {
        outln("[USBDBG] developer mode OFF");
    }
}

void begin() {
    // Safety property: the console ALWAYS starts in text mode, even if the
    // binary protocol was used last session. A mis-set host can therefore
    // never lock itself out of the device.
    s_binary = false;
    s_cwd = "/";

    // Always install the hook: the error ring must be live the moment the
    // user flips the setting on, and the hook itself does nothing while the
    // mode is off.
    installLogHook();

    if (!enabled()) return;

    // The framed protocol moves up to USBDBG_MAX_PAYLOAD+7 bytes in a single
    // burst. The stock USB-CDC RX queue is SAFE_STACK_BUFFER_SIZE/4 (1024 bytes
    // on a PSRAM board), which is smaller than one full frame, so the ISR drops
    // the tail and the frame never assembles. One resize here fixes that; it is
    // sized to the largest frame plus slack rather than a round 8 KB so the
    // internal RAM the queues cost stays small.
    Serial.setRxBufferSize(USBDBG_RX_QUEUE);

    outln("");
    outln("[USBDBG] Developer Options mode ENABLED");
    outln("[USBDBG] protocol=text  developer=" + String(devMode() ? "on" : "off"));
    outln("[USBDBG] verbs: " + String(UsbDebug_verbs));
    outln("[USBDBG] 'usbdebug status' for details, 'help' for the stock CLI");

    if (devMode()) applyDevMode(true);
}

// ---------------------------------------------------------------------------
// Working directory
// ---------------------------------------------------------------------------
String cwd() { return s_cwd; }

String resolve(const String &path) {
    // With the mode off this must be byte-identical to the stock behaviour
    // ("prepend a slash to relative paths"), so the setting never changes
    // how the normal CLI resolves a path.
    if (!enabled()) {
        if (path.length() == 0) return path;
        if (path.startsWith("/")) return path;
        return "/" + path;
    }

    if (path.length() == 0) return s_cwd;
    if (path.startsWith("/")) return normalize(path);
    if (s_cwd == "/") return normalize("/" + path);
    return normalize(s_cwd + "/" + path);
}

bool changeDir(const String &path) {
    String target = resolve(path);
    FS *fs;
    if (!getFsStorage(fs)) return false;
    File f = fs->open(target);
    if (!f || !f.isDirectory()) {
        if (f) f.close();
        return false;
    }
    f.close();
    s_cwd = target;
    return true;
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
void remember(const String &line) {
    String cmd = line;
    cmd.trim();
    if (cmd.length() == 0) return;
    if (cmd == "history") return; // don't let the list pollute itself
    s_history[s_histHead] = cmd;
    s_histHead = (s_histHead + 1) % USBDBG_HISTORY;
    if (s_histCount < USBDBG_HISTORY) s_histCount++;
}

String expandHistory(const String &line) {
    String cmd = line;
    cmd.trim();
    if (s_histCount == 0) return line;

    if (cmd == "!!") {
        size_t last = (s_histHead + USBDBG_HISTORY - 1) % USBDBG_HISTORY;
        return s_history[last];
    }
    if (cmd.startsWith("!") && cmd.length() > 1) {
        long n = cmd.substring(1).toInt();
        if (n > 0 && (size_t)n <= s_histCount) {
            // !1 is the oldest still held
            size_t first = (s_histHead + USBDBG_HISTORY - s_histCount) % USBDBG_HISTORY;
            return s_history[(first + n - 1) % USBDBG_HISTORY];
        }
    }
    return line;
}

void printHistory(size_t limit) {
    if (s_histCount == 0) {
        outln("History: empty");
        return;
    }
    size_t count = s_histCount;
    if (limit > 0 && limit < count) count = limit;
    size_t first = (s_histHead + USBDBG_HISTORY - s_histCount) % USBDBG_HISTORY;
    size_t skip = s_histCount - count;
    for (size_t i = 0; i < count; i++) {
        size_t idx = (first + skip + i) % USBDBG_HISTORY;
        outln("  " + String(i + 1) + "\t" + s_history[idx]);
    }
}

// ---------------------------------------------------------------------------
// Autocomplete
// ---------------------------------------------------------------------------
static const char *UsbDebug_stockVerbs =
    "ls cat md5 crc32 rm mkdir rmdir storage rename copy stat info free uptime date i2c help "
    "nav option options optionsJSON display loader led clock power gpio ir subghz music_player "
    "tone say webui wifi sniffer arp badusb js settings factory_reset";

String complete(const String &prefix) {
    String result;
    String needles = String(UsbDebug_verbs) + " " + String(UsbDebug_stockVerbs);
    int start = 0;
    while (start >= 0 && start < (int)needles.length()) {
        int sp = needles.indexOf(' ', start);
        String verb = sp < 0 ? needles.substring(start) : needles.substring(start, sp);
        if (verb.length() > 0 && (prefix.length() == 0 || verb.startsWith(prefix))) {
            if (result.length() > 0) result += " ";
            result += verb;
        }
        if (sp < 0) break;
        start = sp + 1;
    }
    return result;
}

const char *verbList() { return UsbDebug_verbs; }

// ---------------------------------------------------------------------------
// Command queue / batch execution
// ---------------------------------------------------------------------------
void queueCommand(const String &line) {
    if (s_queueCount >= USBDBG_QUEUE_MAX) {
        outln("Queue full (" + String(USBDBG_QUEUE_MAX) + " max)");
        return;
    }
    String cmd = line;
    cmd.trim();
    if (cmd.length() == 0) return;
    s_queue[s_queueCount++] = cmd;
}

void clearQueue() {
    s_queueCount = 0;
    s_batchBusy = false;
}

void setBatchMode(bool on) { s_batchMode = on; }

bool batchMode() { return s_batchMode; }

bool batchBusy() { return s_batchBusy; }

// Runs at most one queued command; called from the serial task, outside any
// SimpleCLI parse, so re-entering the CLI here is safe.
static void pumpQueue() {
    if (s_batchBusy) return;
    if (s_queueCount == 0) {
        if (s_batchMode) s_batchMode = false; // batch finished
        return;
    }
    String cmd = s_queue[0];
    for (size_t i = 1; i < s_queueCount; i++) s_queue[i - 1] = s_queue[i];
    s_queueCount--;

    s_batchBusy = true;
    outln("[USBDBG] batch> " + cmd);
    serialCli.parse(cmd);
    s_batchBusy = false;

    if (s_queueCount == 0) {
        outln("[USBDBG] batch done");
        s_batchMode = false;
    }
}

// ---------------------------------------------------------------------------
// Hot reload
// ---------------------------------------------------------------------------
void setHotReload(bool on) { s_hotReload = on; }

bool hotReload() { return s_hotReload; }

bool addWatch(const String &path) {
    if (s_watchCount >= USBDBG_WATCH_MAX) return false;
    String abs = resolve(path);
    FS *fs;
    if (!getFsStorage(fs)) return false;
    if (!fs->exists(abs)) return false;

    for (size_t i = 0; i < s_watchCount; i++) {
        if (s_watches[i].path == abs) return true; // already watched
    }
    File f = fs->open(abs);
    if (!f) return false;
    Watch &w = s_watches[s_watchCount++];
    w.path = abs;
    w.size = f.size();
    w.when = f.getLastWrite();
    w.valid = true;
    f.close();
    s_hotReload = true;
    return true;
}

bool removeWatch(const String &path) {
    String abs = resolve(path);
    for (size_t i = 0; i < s_watchCount; i++) {
        if (s_watches[i].path == abs) {
            for (size_t j = i + 1; j < s_watchCount; j++) s_watches[j - 1] = s_watches[j];
            s_watchCount--;
            if (s_watchCount == 0) s_hotReload = false;
            return true;
        }
    }
    return false;
}

void clearWatches() {
    s_watchCount = 0;
    s_hotReload = false;
}

void listWatches() {
    if (s_watchCount == 0) {
        outln("Hot reload: no files watched");
        return;
    }
    outln("Hot reload: " + String(s_hotReload ? "ON" : "OFF") + ", " + String(s_watchCount) + " file(s)");
    for (size_t i = 0; i < s_watchCount; i++) {
        outln("  " + s_watches[i].path + "  (" + String((uint32_t)s_watches[i].size) + " B)");
    }
}

static void pollWatches() {
    if (!s_hotReload || s_watchCount == 0) return;
    uint32_t interval = devMode() ? 100 : 500; // developer mode polls faster
    if (millis() - s_lastPoll < interval) return;
    s_lastPoll = millis();

    FS *fs;
    if (!getFsStorage(fs)) return;

    for (size_t i = 0; i < s_watchCount; i++) {
        File f = fs->open(s_watches[i].path);
        if (!f) continue;
        size_t size = f.size();
        time_t when = f.getLastWrite();
        f.close();
        if (size != s_watches[i].size || when != s_watches[i].when) {
            s_watches[i].size = size;
            s_watches[i].when = when;
            outln(
                "[USBDBG] hot reload: " + s_watches[i].path + " changed (" + String((uint32_t)size) + " B)"
            );
            queueCommand("js " + s_watches[i].path);
        }
    }
}

// ---------------------------------------------------------------------------
// Live update (apply a change without reflashing)
// ---------------------------------------------------------------------------
String liveUpdate(const String &what) {
    if (what == "config" || what == "settings") {
        bruceConfig.fromFile(false);
        return "config reloaded from /bruce.conf (no reboot)";
    }
    if (what == "assets" || what == "theme") {
        bruceConfig.openThemeFile(bruceConfig.themeFS(), bruceConfig.themePath, true);
        return "theme/assets reloaded";
    }
    if (what.startsWith("script:")) {
        String path = resolve(what.substring(7));
        queueCommand("js " + path);
        return "script queued for execution: " + path;
    }
    if (what == "status") {
        String s = "live update: config=reloadable assets=reloadable scripts=reloadable\n";
        s += "firmware image: NOT reloadable over serial (the Launcher owns the\n";
        s += "partition table; use esptool against the brucem slot instead)";
        return s;
    }
    return "usage: liveupdate config|assets|script:<path>|status";
}

// ---------------------------------------------------------------------------
// Error stream
// ---------------------------------------------------------------------------
void pushError(const String &msg) { recordLogLine(msg.c_str()); }

void setErrorStream(bool on) {
    s_errorStream = on;
    if (on) {
        s_errStreamed = s_errCount; // only stream what arrives from now on
        outln("[USBDBG] error stream ON");
    } else {
        outln("[USBDBG] error stream OFF");
    }
}

bool errorStream() { return s_errorStream; }

void printErrors(bool clearRing) {
    portENTER_CRITICAL(&s_errMux);
    size_t count = s_errCount;
    portEXIT_CRITICAL(&s_errMux);

    if (count == 0) {
        outln("Log ring: empty (no error-level lines captured)");
        return;
    }
    outln("Log ring: " + String(count) + " line(s), newest last");
    size_t start = (s_errHead + USBDBG_ERR_LINES - count) % USBDBG_ERR_LINES;
    for (size_t i = 0; i < count; i++) {
        char line[USBDBG_ERR_LEN];
        portENTER_CRITICAL(&s_errMux);
        strncpy(line, s_errRing[(start + i) % USBDBG_ERR_LINES], USBDBG_ERR_LEN);
        line[USBDBG_ERR_LEN - 1] = '\0';
        portEXIT_CRITICAL(&s_errMux);
        outln(String("  ") + line);
    }
    if (clearRing) {
        portENTER_CRITICAL(&s_errMux);
        s_errCount = 0;
        s_errHead = 0;
        portEXIT_CRITICAL(&s_errMux);
    }
}

static void streamErrors() {
    if (!s_errorStream) return;
    size_t count = s_errCount;
    if (count == s_errStreamed) return;
    if (count < s_errStreamed) s_errStreamed = 0; // ring was cleared

    while (s_errStreamed < count) {
        size_t idx = (s_errHead + USBDBG_ERR_LINES - (count - s_errStreamed)) % USBDBG_ERR_LINES;
        char line[USBDBG_ERR_LEN];
        portENTER_CRITICAL(&s_errMux);
        strncpy(line, s_errRing[idx], USBDBG_ERR_LEN);
        line[USBDBG_ERR_LEN - 1] = '\0';
        portEXIT_CRITICAL(&s_errMux);
        outln(String("[ERR] ") + line);
        s_errStreamed = s_errStreamed + 1;
    }
}

// ---------------------------------------------------------------------------
// Text / frame output
// ---------------------------------------------------------------------------
void sendFrame(uint8_t type, const uint8_t *payload, size_t len) {
    if (len > USBDBG_MAX_PAYLOAD) len = USBDBG_MAX_PAYLOAD;
    uint8_t head[5];
    head[0] = USBDBG_MAGIC0;
    head[1] = USBDBG_MAGIC1;
    head[2] = type;
    put16(head + 3, (uint16_t)len);
    serialDevice->write(head, 5);
    if (len > 0) serialDevice->write((uint8_t *)payload, len);

    // Stream the CRC over the header and payload instead of copying the whole
    // frame into a second buffer: this runs from the serial task, whose stack
    // is what the rest of the firmware also has to fit into.
    uint16_t crc = crc16Update(0xFFFF, head + 2, 3);
    if (len > 0) crc = crc16Update(crc, payload, len);
    uint8_t tail[2] = {(uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8)};
    serialDevice->write(tail, 2);
    serialDevice->flush();
}

void sendTextFrame(uint8_t type, const String &text) {
    sendFrame(type, (const uint8_t *)text.c_str(), text.length());
}

void sendText(const String &s) {
    if (s_binary) sendTextFrame(FR_MSG, "[TXT] " + s);
    else outln(s);
}

bool binaryMode() { return s_binary; }

void setBinaryMode(bool on) {
    s_rxLen = 0;
    s_rxWant = 0;

    if (on) {
        // Announce in TEXT before the switch, because the entry point into
        // the framed protocol is the text command itself (text mode does not
        // inspect incoming bytes, so a frame sent while in text mode would be
        // parsed as a command line). The host waits for this line, then
        // starts framing.
        s_binary = false;
        outln("[USBDBG] binary protocol ON - frames are A5 5A type len payload crc16");
        serialDevice->flush();
        s_binary = true;
    } else {
        s_binary = false;
        outln("[USBDBG] binary protocol OFF (text CLI)");
    }
    // Deliberately NOT persisted here: this switches per session, and
    // bruceConfig.saveFile() dumps the whole config to the console and writes
    // the flash. `settings usbDebugBinary 1` records a preference instead.
}

// ---------------------------------------------------------------------------
// Binary frame handling
// ---------------------------------------------------------------------------
static void handleFrame(uint8_t type, const uint8_t *payload, size_t len) {
    switch (type) {
        case FR_PING: sendTextFrame(FR_MSG, "PONG"); break;

        case FR_INFO: {
            String info = "version=" + String(BRUCE_VERSION) + "\n";
            info += "device=" + String(DEVICE_NAME) + "\n";
            info += "chip=" + String(ESP.getChipModel()) + "\n";
            info += "cpu_mhz=" + String(getCpuFrequencyMhz()) + "\n";
            info += "flash=" + String(ESP.getFlashChipSize()) + "\n";
            info += "heap_free=" + String(ESP.getFreeHeap()) + "\n";
            info += "psram_free=" + String(ESP.getFreePsram()) + "\n";
            // BLE brings its HCI buffers up out of DMA-capable internal RAM, so
            // these two are what decide whether ble.init() can succeed from a
            // script (see the BLE section of the JS API docs).
            info += "dma_free=" + String(heap_caps_get_free_size(MALLOC_CAP_DMA)) + "\n";
            info += "dma_largest=" + String(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)) + "\n";
            info += "cwd=" + cwd() + "\n";
            info += "usbdebug=1\n";
            info += "protocol=binary\n";
            info += "max_frame=" + String(USBDBG_MAX_PAYLOAD) + "\n";
            sendTextFrame(FR_INFO_R, info);
        } break;

        case FR_EXEC: {
            String cmd;
            cmd.reserve(len + 1);
            for (size_t i = 0; i < len; i++) cmd += (char)payload[i];
            cmd.trim();
            if (cmd.length() == 0) {
                sendTextFrame(FR_EXEC_OK, "ERR empty command");
                break;
            }
            remember(cmd);
            String expanded = expandHistory(cmd);
            bool ok = serialCli.parse(expanded);
            sendTextFrame(FR_EXEC_OK, ok ? "OK " + expanded : "ERR " + expanded);
        } break;

        case FR_EXEC_BATCH: {
            // payload = newline separated commands
            String all;
            all.reserve(len + 1);
            for (size_t i = 0; i < len; i++) all += (char)payload[i];
            int start = 0;
            int count = 0;
            while (start < (int)all.length()) {
                int nl = all.indexOf('\n', start);
                String one = nl < 0 ? all.substring(start) : all.substring(start, nl);
                queueCommand(one);
                count++;
                if (nl < 0) break;
                start = nl + 1;
            }
            sendTextFrame(FR_EXEC_OK, "QUEUED " + String(count));
        } break;

        case FR_SET_MODE: {
            bool toBinary = (len > 0 && payload[0] == 1);
            if (toBinary) {
                sendTextFrame(FR_MODE_R, "MODE already binary");
                break;
            }
            // Reply while still framed, then drop back to the text CLI.
            sendTextFrame(FR_MODE_R, "MODE text");
            s_binary = false;
            outln("[USBDBG] binary protocol OFF (text CLI)");
        } break;

        case FR_PUT_BEGIN: {
            // payload = path \0 size(4 LE)
            const char *path = (const char *)payload;
            size_t plen = strnlen(path, len);
            if (plen + 5 > len) {
                sendFrame(FR_PUT_ACK, (const uint8_t *)"ERR header", 10);
                break;
            }
            s_putPath = resolve(String(path));
            s_putSize = get32(payload + plen + 1);
            s_putWritten = 0;
            FS *fs;
            if (!getFsStorage(fs)) {
                sendTextFrame(FR_PUT_ACK, "ERR no filesystem");
                break;
            }
            s_putFile = fs->open(s_putPath, FILE_WRITE, true);
            if (!s_putFile) {
                sendTextFrame(FR_PUT_ACK, "ERR open " + s_putPath);
                break;
            }
            sendTextFrame(FR_PUT_ACK, "READY " + s_putPath + " " + String(s_putSize));
        } break;

        case FR_PUT_DATA: {
            if (!s_putFile) {
                sendTextFrame(FR_PUT_ACK, "ERR no transfer");
                break;
            }
            if (len < 4) {
                sendTextFrame(FR_PUT_ACK, "ERR short chunk");
                break;
            }
            uint32_t offset = get32(payload);
            if (offset != s_putWritten) {
                sendTextFrame(
                    FR_PUT_ACK, "ERR offset want " + String(s_putWritten) + " got " + String(offset)
                );
                break;
            }
            size_t n = s_putFile.write(payload + 4, len - 4);
            s_putWritten += n;
            String resp = "CHUNK " + String(s_putWritten) + "/" + String(s_putSize);
            sendTextFrame(FR_PUT_ACK, resp);
        } break;

        case FR_PUT_END: {
            if (s_putFile) {
                s_putFile.close();
                sendTextFrame(
                    FR_PUT_DONE, "DONE " + s_putPath + " " + String(s_putWritten) + " bytes"
                );
                s_putFile = File();
            } else {
                sendTextFrame(FR_PUT_DONE, "ERR no transfer");
            }
        } break;

        case FR_GET_BEGIN: {
            // Stat only: the host then PULLS the file with FR_GET_DATA frames.
            // Pulling keeps every device-side read small and bounded, which
            // matters because the link has no flow control and the serial task
            // must stay responsive.
            String path;
            path.reserve(len + 1);
            for (size_t i = 0; i < len; i++) path += (char)payload[i];
            String abs = resolve(path);
            FS *fs;
            if (!getFsStorage(fs) || !fs->exists(abs)) {
                sendTextFrame(FR_GET_INFO, "ERR not found " + abs);
                break;
            }
            File f = fs->open(abs, FILE_READ);
            if (!f) {
                sendTextFrame(FR_GET_INFO, "ERR open " + abs);
                break;
            }
            uint32_t total = f.size();
            f.close();
            sendTextFrame(FR_GET_INFO, "OK " + abs + " " + String(total));
        } break;

        case FR_GET_DATA: {
            // payload = path \0 offset(4 LE) len(2 LE)
            const char *path = (const char *)payload;
            size_t plen = strnlen(path, len);
            if (plen + 7 > len) {
                // Failures go out as FR_MSG, never as FR_GET_DATA_R: a data
                // frame is binary and the host has no way to tell it apart
                // from a text error on the same type.
                sendTextFrame(FR_MSG, "ERR header");
                break;
            }
            uint32_t offset = get32(payload + plen + 1);
            uint16_t want = get16(payload + plen + 5);
            if (want > USBDBG_CHUNK) want = USBDBG_CHUNK;

            String abs = resolve(String(path));
            FS *fs;
            if (!getFsStorage(fs) || !fs->exists(abs)) {
                sendTextFrame(FR_MSG, "ERR not found " + abs);
                break;
            }
            File f = fs->open(abs, FILE_READ);
            if (!f) {
                sendTextFrame(FR_MSG, "ERR open " + abs);
                break;
            }
            uint32_t total = f.size();
            if (offset >= total) {
                f.close();
                sendTextFrame(FR_GET_DONE, "EOF " + String(total) + " bytes");
                break;
            }
            if (!f.seek(offset)) {
                f.close();
                sendTextFrame(FR_MSG, "ERR seek " + String(offset));
                break;
            }
            static uint8_t getBuf[4 + USBDBG_CHUNK];
            size_t n = f.read(getBuf + 4, want);
            f.close();
            if (n == 0) {
                sendTextFrame(FR_MSG, "ERR read " + String(offset));
                break;
            }
            put32(getBuf, offset);
            sendFrame(FR_GET_DATA_R, getBuf, n + 4);
        } break;

        default: sendTextFrame(FR_MSG, "ERR unknown frame type " + String(type)); break;
    }
}

static void feedBinary(uint8_t b) {
    // Frame: A5 5A type len_lo len_hi payload... crc_lo crc_hi
    if (s_rxLen < 5) {
        if (s_rxLen == 0 && b != USBDBG_MAGIC0) return; // resync
        if (s_rxLen == 1 && b != USBDBG_MAGIC1) {
            // lost sync; restart with this byte
            s_rxLen = (b == USBDBG_MAGIC0) ? 1 : 0;
            return;
        }
        s_rxBuf[s_rxLen++] = b;
        if (s_rxLen == 5) {
            uint16_t len = get16(s_rxBuf + 3);
            if (len > USBDBG_MAX_PAYLOAD) {
                s_rxLen = 0;
                return;
            }
            s_rxWant = 5 + len + 2;
        }
        return;
    }

    s_rxBuf[s_rxLen++] = b;
    if (s_rxLen < s_rxWant) return;

    // complete frame - verify crc
    uint8_t type = s_rxBuf[2];
    uint16_t len = get16(s_rxBuf + 3);
    uint16_t wantCrc = get16(s_rxBuf + 5 + len);
    uint16_t gotCrc = crc16(s_rxBuf + 2, len + 3);
    s_rxLen = 0;
    s_rxWant = 0;
    if (wantCrc != gotCrc) {
        sendTextFrame(FR_MSG, "ERR bad crc");
        return;
    }
    handleFrame(type, s_rxBuf + 5, len);
}

bool pollSerial() {
    if (!enabled()) return false;
    if (!s_binary) return false; // text mode: the stock CLI owns the port

    // Drain a whole frame (plus whatever is queued behind it) per pass, so a
    // transfer is not limited to the tiny per-tick budget. Bounded so the
    // debug engine can never starve the rest of the firmware.
    int budget = 4096;
    while (serialDevice->available() && budget-- > 0) { feedBinary((uint8_t)serialDevice->read()); }
    return true; // consumed: the stock text path must not re-read the port
}

// ---------------------------------------------------------------------------
// Pump
// ---------------------------------------------------------------------------
void loop() {
    if (!enabled()) return;
    streamErrors();
    pollWatches();
    pumpQueue();
}

} // namespace UsbDebug
