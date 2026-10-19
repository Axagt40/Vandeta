#include "debug_commands.h"

#include "core/radio_mem.h"
#include "core/sd_functions.h"
#include "core/usb_debug/usbdebug.h"
#include "core/display.h" // activeMenuOptions()
#include "core/utils.h"    // getOptionsJSON()
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <globals.h>

#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "modules/bjs_interpreter/interpreter.h"
#endif

/*********************************************************************
**  Developer Options CLI verbs (the renamed USB Debugging page)
**
**  These extend the stock Bruce serial CLI (src/core/serial_commands/)
**  rather than replacing it: file operations, JS execution, device
**  control and system queries keep using the existing commands, which the
**  debug layer only decorates (relative paths, history, batching).
*********************************************************************/

static bool requireDebug() {
    if (UsbDebug::enabled()) return true;
    serialDevice->println("Developer Options are OFF. Enable them in Config > System Config > Developer Options");
    serialDevice->println("(or 'usbdebug on' - the setting is persisted).");
    return false;
}

// Re-join the arguments from `from` on, so `queue js /x.js` keeps its path.
static String joinArgs(cmd *c, int from = 0) {
    Command cmd(c);
    String out;
    int n = cmd.countArgs();
    for (int i = from; i < n; i++) {
        if (out.length() > 0) out += " ";
        out += cmd.getArgument(i).getValue();
    }
    return out;
}

// ---------------------------------------------------------------------------
// usbdebug <on|off|status|dev on|dev off|proto text|proto binary|reset>
// ---------------------------------------------------------------------------
static uint32_t usbDebugCallback(cmd *c) {
    Command cmd(c);
    String action = cmd.getArgument(0).getValue();
    action.trim();
    action.toLowerCase();

    if (action == "on" || action == "1" || action == "enable") {
        bruceConfig.setUsbDebug(1);
        serialDevice->println("Developer Options: ON (persisted, active immediately)");
        serialDevice->println("verbs: " + String(UsbDebug::verbList()));
        return true;
    }
    if (action == "off" || action == "0" || action == "disable") {
        bruceConfig.setUsbDebug(0);
        serialDevice->println("Developer Options: OFF");
        return true;
    }
    if (action == "dev") {
        String v = cmd.getArgument(1).getValue();
        v.trim();
        v.toLowerCase();
        bool on = !(v == "off" || v == "0" || v == "false");
        bruceConfig.setUsbDebugDevMode(on ? 1 : 0);
        UsbDebug::applyDevMode(on);
        return true;
    }
    if (action == "proto") {
        String v = cmd.getArgument(1).getValue();
        v.trim();
        v.toLowerCase();
        if (v == "binary") {
            UsbDebug::setBinaryMode(true);
            return true;
        }
        if (v == "text") {
            UsbDebug::setBinaryMode(false);
            return true;
        }
        serialDevice->println("usage: usbdebug proto text|binary");
        return false;
    }
    if (action == "reset") {
        UsbDebug::clearQueue();
        UsbDebug::clearWatches();
        serialDevice->println("Debug session state cleared (queue + watches)");
        return true;
    }

    // status (default)
    serialDevice->println("Developer Options: " + String(UsbDebug::enabled() ? "ON" : "OFF"));
    serialDevice->println("  developer mode : " + String(UsbDebug::devMode() ? "ON" : "OFF"));
    serialDevice->println("  protocol       : " + String(UsbDebug::binaryMode() ? "binary" : "text"));
    serialDevice->println("  preferred proto: " + String(bruceConfig.usbDebugBinary ? "binary" : "text"));
    serialDevice->println("  queue          : " + String(UsbDebug::batchBusy() ? "running" : "idle"));
    serialDevice->println("  error stream   : " + String(UsbDebug::errorStream() ? "ON" : "OFF"));
    serialDevice->println("  cwd            : " + UsbDebug::cwd());
    serialDevice->println("  threads        : console=serialcmds  ui=main loop  interpreter=js task");
    return true;
}

// ---------------------------------------------------------------------------
// cd / pwd
// ---------------------------------------------------------------------------
static uint32_t cdCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String path = cmd.getArgument(0).getValue();
    path.trim();
    if (path.length() == 0) path = "/";
    if (UsbDebug::changeDir(path)) {
        serialDevice->println(UsbDebug::cwd());
        return true;
    }
    serialDevice->println("cd: no such directory: " + path);
    return false;
}

static uint32_t pwdCallback(cmd *c) {
    if (!requireDebug()) return false;
    serialDevice->println(UsbDebug::cwd());
    return true;
}

// ---------------------------------------------------------------------------
// queue / batch
// ---------------------------------------------------------------------------
static uint32_t queueCallback(cmd *c) {
    if (!requireDebug()) return false;
    String line = joinArgs(c);
    line.trim();
    if (line.length() == 0) {
        serialDevice->println("usage: queue <command> [args]");
        return false;
    }
    UsbDebug::queueCommand(line);
    serialDevice->println("queued: " + line);
    return true;
}

static uint32_t batchCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String action = cmd.getArgument(0).getValue();
    action.trim();
    action.toLowerCase();

    if (action == "on") {
        UsbDebug::setBatchMode(true);
        serialDevice->println("batch mode ON (queued commands drain one per tick)");
        return true;
    }
    if (action == "off") {
        UsbDebug::setBatchMode(false);
        UsbDebug::clearQueue();
        serialDevice->println("batch mode OFF, queue cleared");
        return true;
    }
    if (action == "clear") {
        UsbDebug::clearQueue();
        serialDevice->println("queue cleared");
        return true;
    }
    if (action == "run" || action == "start") {
        UsbDebug::setBatchMode(true);
        serialDevice->println("batch draining...");
        return true;
    }
    serialDevice->println("batch: " + String(UsbDebug::batchMode() ? "running" : "idle"));
    return true;
}

// ---------------------------------------------------------------------------
// history / complete
// ---------------------------------------------------------------------------
static uint32_t historyCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String arg = cmd.getArgument(0).getValue();
    arg.trim();
    if (arg == "clear") {
        serialDevice->println("History cleared (use 'settings usbDebug 0' to fully reset)");
        return true;
    }
    size_t limit = arg.length() ? (size_t)arg.toInt() : 0;
    UsbDebug::printHistory(limit);
    return true;
}

static uint32_t completeCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String prefix = cmd.getArgument(0).getValue();
    prefix.trim();
    String hit = UsbDebug::complete(prefix);
    if (hit.length() == 0) serialDevice->println("complete: no match for '" + prefix + "'");
    else serialDevice->println(hit);
    return true;
}

// ---------------------------------------------------------------------------
// hotreload / liveupdate
// ---------------------------------------------------------------------------
static uint32_t hotReloadCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String action = cmd.getArgument(0).getValue();
    String path = cmd.getArgument(1).getValue();
    action.trim();
    action.toLowerCase();
    path.trim();

    if (action == "on") {
        UsbDebug::setHotReload(true);
        serialDevice->println("hot reload ON");
        return true;
    }
    if (action == "off") {
        UsbDebug::setHotReload(false);
        serialDevice->println("hot reload OFF");
        return true;
    }
    if (action == "add") {
        if (path.length() == 0) {
            serialDevice->println("usage: hotreload add <path.js>");
            return false;
        }
        if (UsbDebug::addWatch(path)) {
            serialDevice->println("watching " + UsbDebug::resolve(path));
            return true;
        }
        serialDevice->println("cannot watch (not found, or 8 files already watched): " + path);
        return false;
    }
    if (action == "remove") {
        serialDevice->println(UsbDebug::removeWatch(path) ? "watch removed" : "not watched: " + path);
        return true;
    }
    if (action == "list" || action.length() == 0) {
        UsbDebug::listWatches();
        return true;
    }
    serialDevice->println("usage: hotreload on|off|add <path>|remove <path>|list");
    return false;
}

static uint32_t liveUpdateCallback(cmd *c) {
    if (!requireDebug()) return false;
    String what = joinArgs(c);
    what.trim();
    if (what.length() == 0) what = "status";
    serialDevice->println(UsbDebug::liveUpdate(what));
    return true;
}

// ---------------------------------------------------------------------------
// errors / log
// ---------------------------------------------------------------------------
static uint32_t errorsCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String action = cmd.getArgument(0).getValue();
    action.trim();
    action.toLowerCase();
    UsbDebug::printErrors(action == "clear");
    return true;
}

static uint32_t logCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String action = cmd.getArgument(0).getValue();
    action.trim();
    action.toLowerCase();
    if (action == "on") {
        UsbDebug::setErrorStream(true);
        return true;
    }
    if (action == "off") {
        UsbDebug::setErrorStream(false);
        return true;
    }
    // Simple, human/AI friendly forms: `log show` dumps the ring, `log clear`
    // empties it. `log on|off` keeps streaming ESP-IDF errors live.
    if (action == "show" || action == "dump" || action == "list") {
        UsbDebug::printErrors(false);
        return true;
    }
    if (action == "clear") {
        UsbDebug::printErrors(true);
        serialDevice->println("log: cleared");
        return true;
    }
    serialDevice->println("log: error stream " + String(UsbDebug::errorStream() ? "ON" : "OFF") + " (log on|off|show|clear)");
    return true;
}

// ---------------------------------------------------------------------------
// proc / sys
// ---------------------------------------------------------------------------
static uint32_t procCallback(cmd *c) {
    if (!requireDebug()) return false;

    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *arr = (TaskStatus_t *)malloc(n * sizeof(TaskStatus_t));
    if (!arr) {
        serialDevice->println("proc: out of memory");
        return false;
    }
    uint32_t totalRuntime = 0;
    n = uxTaskGetSystemState(arr, n, &totalRuntime);

    serialDevice->println("Tasks: " + String((uint32_t)n));
    serialDevice->println("  name\tstate\tprio\thwm\tcore");
    for (UBaseType_t i = 0; i < n; i++) {
        String line = "  ";
        line += arr[i].pcTaskName;
        line += "\t";
        line += String((int)arr[i].eCurrentState);
        line += "\t";
        line += String((uint32_t)arr[i].uxCurrentPriority);
        line += "\t";
        line += String((uint32_t)arr[i].usStackHighWaterMark);
        line += "\t";
        line += String((int)arr[i].xCoreID);
        serialDevice->println(line);
    }
    free(arr);

    serialDevice->println(
        "interpreter_state=" + String((int)interpreter_state) +
        "  wizard/hypervisor: none (no RTOS process control exposed)"
    );
    serialDevice->println("batch running=" + String(UsbDebug::batchBusy() ? "yes" : "no"));
    return true;
}

static uint32_t sysCallback(cmd *c) {
    if (!requireDebug()) return false;
    serialDevice->println("chip        : " + String(ESP.getChipModel()) + " rev " + String(ESP.getChipRevision()));
    serialDevice->println("cores       : " + String(ESP.getChipCores()) + " @ " + String(getCpuFrequencyMhz()) + " MHz");
    serialDevice->println("flash       : " + String((uint32_t)ESP.getFlashChipSize()) + " bytes");
    serialDevice->println("sdk         : " + String(ESP.getSdkVersion()));
    serialDevice->println("heap free   : " + String((uint32_t)ESP.getFreeHeap()) + " / " + String((uint32_t)ESP.getHeapSize()));
    serialDevice->println(
        "internal    : " + String((uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) +
        " largest=" + String((uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL))
    );
    if (psramFound()) {
        serialDevice->println(
            "psram free  : " + String((uint32_t)ESP.getFreePsram()) + " / " + String((uint32_t)ESP.getPsramSize())
        );
    }
    serialDevice->println("reset reason: " + String((uint32_t)esp_reset_reason()));
    serialDevice->println("uptime      : " + String(millis() / 1000) + "s");
    serialDevice->println("device      : " + String(DEVICE_NAME));
    serialDevice->println("version     : " + String(BRUCE_VERSION));
    serialDevice->println("debug mode  : " + String(UsbDebug::enabled() ? "ON" : "OFF"));
    serialDevice->println("cwd         : " + UsbDebug::cwd());
    return true;
}

// ---------------------------------------------------------------------------
// mem - memory monitoring / reporting
// ---------------------------------------------------------------------------
// One place that answers "is there enough RAM for the next radio?" without a
// debugger. Internal DRAM (not PSRAM) is the scarce resource on this board.
// Wi-Fi is gated by the largest contiguous internal/DMA block alone; BLE needs
// that too, but the binding constraint is the TOTAL free internal DRAM (its
// controller/host/HCI buffers add up to ~56 KB measured, see radio_mem.h).
static uint32_t memCallback(cmd *c) {
    if (!requireDebug()) return false;

    size_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t psramFree = psramFound() ? ESP.getFreePsram() : 0;
    size_t psramLargest = psramFound() ? ESP.getMaxAllocPsram() : 0;

    serialDevice->println("heap free      : " + String((uint32_t)ESP.getFreeHeap()) + " / " + String((uint32_t)ESP.getHeapSize()));
    serialDevice->println("heap min ever  : " + String((uint32_t)ESP.getMinFreeHeap()));
    serialDevice->println("internal free  : " + String((uint32_t)internalFree) + " largest=" + String((uint32_t)internalLargest));
    serialDevice->println("dma free       : " + String((uint32_t)dmaFree) + " largest=" + String((uint32_t)dmaLargest));
    serialDevice->println("psram free     : " + String((uint32_t)psramFree) + " largest=" + String((uint32_t)psramLargest));
    serialDevice->println(
        "radio wifi ok  : " + String(radioHasMemForWifi() ? "yes" : "no") + " (needs " +
        String((uint32_t)RADIO_WIFI_MIN_DMA_BLOCK) + "B contiguous DMA)"
    );
    serialDevice->println(
        "radio ble ok   : " +
        String(
            (internalFree >= RADIO_BLE_MIN_INTERNAL_FREE && dmaLargest >= RADIO_BLE_MIN_DMA_BLOCK) ? "yes" : "no"
        ) +
        " (guard for a NEW bring-up: needs " + String((uint32_t)(RADIO_BLE_MIN_INTERNAL_FREE / 1024)) +
        "KB free internal + " + String((uint32_t)(RADIO_BLE_MIN_DMA_BLOCK / 1024)) +
        "KB contiguous DMA, releases WiFi if short; SKIPPED when the stack is already up, so 'no' here is normal "
        "while BLE is running)"
    );
    return true;
}

static uint32_t protoCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String v = cmd.getArgument(0).getValue();
    v.trim();
    v.toLowerCase();
    if (v == "binary") {
        UsbDebug::setBinaryMode(true);
        return true;
    }
    if (v == "text") {
        UsbDebug::setBinaryMode(false);
        return true;
    }
    serialDevice->println(
        "proto text   - line based CLI (default, always restored at boot)\n"
        "proto binary - framed A5 5A type len payload crc16"
    );
    return true;
}

// ---------------------------------------------------------------------------
// ui - real-time device control: drive the UI and read back where you are
// ---------------------------------------------------------------------------
// This replaces the old blind loop of `nav` + `option`, whose feedback was
// unreliable: `nav` dumped the options immediately after injecting a keypress,
// i.e. before the main loop had processed it, so the readback was the screen you
// were LEAVING (which is why a host "had to restart to see where it was").
//
// `ui` injects the key, waits for the UI to settle, then prints where you
// actually are: the menu title, every option with the highlighted row marked, and
// a machine-readable `UISTATE <json>` line. Every verb lives in this Developer
// Options layer, so it is inert while the setting is off, and none of it needs a
// reboot between commands - the port is opened once and keys are pulsed live.
// Read the menu that is actually on screen, not the global `options` vector: many
// menus (Config, WiFi, QR code, BLE, ...) build a LOCAL vector that shadows the
// global, and loopOptions publishes whichever one it is rendering via
// activeMenuOptions(). Reading the global here was why `ui`/`move`/`press`
// reported a stale or empty screen.
static int uiActiveIndex() {
    std::vector<Option> &opts = activeMenuOptions();
    int i = 0;
    for (auto &opt : opts) {
        if (opt.hovered) return i;
        i++;
    }
    return -1;
}

static void printUiState() {
    std::vector<Option> &opts = activeMenuOptions();
    int active = uiActiveIndex();
    int i = 0;
    const char *kind = menuOptionType == 0 ? "main" : (menuOptionType == 1 ? "sub" : "regular");
    serialDevice->println(
        "[UI] " + menuOptionLabel + " [" + String(kind) + "] " + String((uint32_t)opts.size()) +
        " option(s), active=" + String(active)
    );
    i = 0;
    for (auto &opt : opts) {
        serialDevice->println(String("  ") + (opt.hovered ? ">" : " ") + String(i) + " - " + opt.label);
        i++;
    }
    serialDevice->println("UISTATE " + getOptionsJSON());
}

// Wait for the screen to actually change (or the timeout to run out), then dump.
// A keypress is handled by the main loop, not by this serial task, so reading the
// state immediately after injecting it returns the OLD menu - the bug this fixes.
static void settleAndDump(uint32_t ms = 900) {
    String beforeLabel = menuOptionLabel;
    size_t beforeCount = activeMenuOptions().size();
    uint8_t beforeType = menuOptionType;
    int beforeActive = uiActiveIndex();
    uint32_t start = millis();
    while (millis() - start < ms) {
        if (menuOptionLabel != beforeLabel || activeMenuOptions().size() != beforeCount ||
            menuOptionType != beforeType || uiActiveIndex() != beforeActive)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(80)); // let the repaint finish
    printUiState();
}

// One command must be exactly ONE keypress. The main loop consumes the flag in
// check() (clearing it), so wait for that instead of pulsing for a fixed window -
// pulsing made a single `ui down` scroll the menu two or three rows. The timeout
// only exists so a UI busy inside a blocking screen cannot hang the console, and
// the flag is cleared afterwards so no phantom press is left behind.
static void injectKey(volatile bool *flag) {
    *flag = true;
    AnyKeyPress = true;
    SerialCmdPress = true;
    uint32_t start = millis();
    while (*flag && millis() - start < 250) vTaskDelay(pdMS_TO_TICKS(5));
    *flag = false;
}

// --- live screen watcher ---------------------------------------------------
// `ui watch on` makes the firmware push the screen to the console whenever it
// changes, so a host gets instant feedback while the menus are driven by the
// device's own buttons or by `ui` - no polling, no reboot. Driven from
// UsbDebug::loop() (the 10 ms serial pump), so it needs no task of its own.
static bool s_uiWatch = false;
static uint32_t s_uiWatchTick = 0;
static String s_uiWatchLabel;
static uint8_t s_uiWatchType = 0xFF;
static size_t s_uiWatchCount = (size_t)-1;
static int s_uiWatchActive = -2;

// Cheap by design: four scalar comparisons per tick, no allocation, and the
// (relatively expensive) screen dump only when something actually changed. Four
// fields are enough to notice navigation - a different menu (title/type), a
// changed row count, or a moved highlight.
void debugUiWatchOff() { s_uiWatch = false; }

void debugPollUiWatch() {
    // Hard gate: never emit while the setting is off, even if the watcher was
    // left armed by a console that has since been disabled.
    if (!UsbDebug::enabled()) {
        s_uiWatch = false;
        return;
    }
    if (!s_uiWatch) return;
    uint32_t now = millis();
    if (now - s_uiWatchTick < 120) return; // ~8 checks/second at most
    s_uiWatchTick = now;
    size_t count = activeMenuOptions().size();
    int active = uiActiveIndex();
    if (menuOptionLabel == s_uiWatchLabel && menuOptionType == s_uiWatchType && count == s_uiWatchCount &&
        active == s_uiWatchActive)
        return;
    s_uiWatchLabel = menuOptionLabel;
    s_uiWatchType = menuOptionType;
    s_uiWatchCount = count;
    s_uiWatchActive = active;
    printUiState();
}

static uint32_t uiCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String action = cmd.getArgument(0).getValue();
    String value = cmd.getArgument(1).getValue();
    action.trim();
    action.toLowerCase();
    value.trim();

    // `ui key <name>` is the explicit form of the plain shortcuts.
    if (action == "key" && value.length() > 0) {
        action = value;
        action.toLowerCase();
        value = "";
    }

    if (action == "watch") {
        value.toLowerCase();
        if (value == "off" || value == "0" || value == "false") {
            s_uiWatch = false;
            serialDevice->println("[UIW] watcher off");
            return true;
        }
        if (value == "on" || value == "1" || value == "true") {
            s_uiWatch = true;
            s_uiWatchTick = 0;
            s_uiWatchCount = (size_t)-1; // force one immediate dump
            debugPollUiWatch();
            serialDevice->println("[UIW] watcher on - every screen change is printed");
            return true;
        }
        serialDevice->println(String("[UIW] watcher ") + (s_uiWatch ? "ON" : "OFF") + " (ui watch on|off)");
        printUiState();
        return true;
    }

    if (action.length() == 0 || action == "dump" || action == "where" || action == "show") {
        printUiState();
        return true;
    }
    if (action == "help") {
        serialDevice->println(
            "ui                     - show the current screen (menu + options + active index)\n"
            "ui up|down|left|right  - move the highlight, then show the new screen\n"
            "ui select|enter|ok     - press SELECT, then show the new screen\n"
            "ui back|esc            - press BACK, then show the new screen\n"
            "ui prev|next           - encoder-style left|right, then show the screen\n"
            "ui nextpage|prevpage   - page up/down, then show the new screen\n"
            "ui key <name>          - explicit form of the shortcuts above\n"
            "ui select <n>          - pick row n directly, then show the new screen\n"
            "ui watch on|off        - stream the screen every time it changes (live)"
        );
        return true;
    }

    // Direct row selection: `option <n>` without the stale label.
    if (action == "select" && value.length() > 0) {
        int opt = value.toInt();
        if (opt < 0 || opt >= (int)activeMenuOptions().size()) {
            serialDevice->println("ui: no option " + value + " on this screen");
            printUiState();
            return false;
        }
        forceMenuOption = opt;
        settleAndDump();
        return true;
    }

    volatile bool *flag = nullptr;
    if (action == "up") flag = &UpPress;
    else if (action == "down") flag = &DownPress;
    else if (action == "left" || action == "prev") flag = &PrevPress;
    else if (action == "right" || action == "next") flag = &NextPress;
    else if (action == "select" || action == "sel" || action == "enter" || action == "ok") flag = &SelPress;
    else if (action == "back" || action == "esc") flag = &EscPress;
    else if (action == "nextpage") flag = &NextPagePress;
    else if (action == "prevpage") flag = &PrevPagePress;

    if (flag == nullptr) {
        serialDevice->println("ui: unknown action '" + action + "' - try 'ui help'");
        return false;
    }

    injectKey(flag);
    settleAndDump();
    return true;
}

// ---------------------------------------------------------------------------
// move <direction> / press <key>
// ---------------------------------------------------------------------------
// The simple, human-and-AI friendly face of `ui`. `move up` changes the
// highlight, `press ok` commits it, and each command prints where the UI is now
// (menu title + every option with the highlighted row marked) so a serial
// terminal always sees immediate feedback instead of a stale screen.
static volatile bool *uiKeyFlagFor(const String &name) {
    if (name == "up") return &UpPress;
    if (name == "down") return &DownPress;
    if (name == "left" || name == "prev") return &PrevPress;
    if (name == "right" || name == "next") return &NextPress;
    if (name == "nextpage" || name == "pagedown") return &NextPagePress;
    if (name == "prevpage" || name == "pageup") return &PrevPagePress;
    if (name == "ok" || name == "select" || name == "sel" || name == "enter" || name == "confirm")
        return &SelPress;
    if (name == "back" || name == "esc" || name == "cancel") return &EscPress;
    return nullptr;
}

static uint32_t moveCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String dir = cmd.getArgument(0).getValue();
    dir.trim();
    dir.toLowerCase();
    volatile bool *flag = uiKeyFlagFor(dir);
    // `move` must not be used to commit; that is `press`.
    if (flag == &SelPress || flag == &EscPress) flag = nullptr;
    if (flag == nullptr) {
        serialDevice->println("move: usage move up|down|left|right|prev|next|nextpage|prevpage");
        return false;
    }
    injectKey(flag);
    settleAndDump();
    return true;
}

static uint32_t pressCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String key = cmd.getArgument(0).getValue();
    key.trim();
    key.toLowerCase();
    if (key.length() == 0) key = "ok"; // `press` on its own means OK
    volatile bool *flag = uiKeyFlagFor(key);
    if (flag == nullptr) {
        serialDevice->println("press: usage press ok|back|up|down|left|right");
        return false;
    }
    injectKey(flag);
    settleAndDump();
    return true;
}

// ---------------------------------------------------------------------------
// file <read|write|list|delete|stat> ...
// ---------------------------------------------------------------------------
// A small, predictable file API for a serial terminal: paths resolve against the
// debug working directory, relative names are accepted, and every operation says
// what it did. It wraps the same filesystem helpers the stock commands use.
static uint32_t fileCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String action = cmd.getArgument(0).getValue();
    String path = cmd.getArgument(1).getValue();
    action.trim();
    action.toLowerCase();
    path.trim();

    if (action == "read" || action == "cat") {
        if (path.length() == 0) {
            serialDevice->println("usage: file read <filename>");
            return false;
        }
        FS *fs = nullptr;
        if (!getFsStorage(fs)) {
            serialDevice->println("file: no filesystem available");
            return false;
        }
        path = UsbDebug::resolve(path);
        if (!fs->exists(path)) {
            serialDevice->println("file: not found: " + path);
            return false;
        }
        serialDevice->println(readSmallFile(*fs, path));
        return true;
    }

    if (action == "write") {
        if (path.length() == 0) {
            serialDevice->println("usage: file write <filename> <data>");
            return false;
        }
        // Re-join everything after the path, so the data may contain spaces.
        String data;
        int n = cmd.countArgs();
        for (int i = 2; i < n; i++) {
            if (data.length()) data += " ";
            data += cmd.getArgument(i).getValue();
        }
        FS *fs = nullptr;
        if (!getFsStorage(fs)) {
            serialDevice->println("file: no filesystem available");
            return false;
        }
        path = UsbDebug::resolve(path);
        File f = fs->open(path, FILE_WRITE, true);
        if (!f) {
            serialDevice->println("file: cannot open for writing: " + path);
            return false;
        }
        size_t written = f.write((const uint8_t *)data.c_str(), data.length());
        f.close();
        serialDevice->println("file: wrote " + String((uint32_t)written) + " bytes to " + path);
        return true;
    }

    if (action == "list" || action == "ls") {
        FS *fs = nullptr;
        if (!getFsStorage(fs)) {
            serialDevice->println("file: no filesystem available");
            return false;
        }
        if (path.length() == 0) path = UsbDebug::cwd();
        path = UsbDebug::resolve(path);
        if (!fs->exists(path)) {
            serialDevice->println("file: not found: " + path);
            return false;
        }
        File dir = fs->open(path);
        if (!dir) {
            serialDevice->println("file: cannot open: " + path);
            return false;
        }
        int count = 0;
        while (true) {
            bool isDir = false;
            String full = dir.getNextFileName(&isDir);
            if (full.length() == 0) break;
            String name = full.substring(full.lastIndexOf('/') + 1);
            serialDevice->println(String("  ") + name + (isDir ? "/" : "") );
            count++;
        }
        dir.close();
        serialDevice->println("file: " + String(count) + " entries in " + path);
        return true;
    }

    if (action == "delete" || action == "remove" || action == "rm") {
        if (path.length() == 0) {
            serialDevice->println("usage: file delete <filename>");
            return false;
        }
        FS *fs = nullptr;
        if (!getFsStorage(fs)) return false;
        path = UsbDebug::resolve(path);
        if (!fs->exists(path)) {
            serialDevice->println("file: not found: " + path);
            return false;
        }
        serialDevice->println(fs->remove(path) ? "file: deleted " + path : "file: delete failed: " + path);
        return true;
    }

    if (action == "stat") {
        if (path.length() == 0) {
            serialDevice->println("usage: file stat <filename>");
            return false;
        }
        FS *fs = nullptr;
        if (!getFsStorage(fs)) return false;
        path = UsbDebug::resolve(path);
        File f = fs->open(path);
        if (!f) {
            serialDevice->println("file: not found: " + path);
            return false;
        }
        serialDevice->println(
            "file: " + path + " size=" + String((uint32_t)f.size()) + (f.isDirectory() ? " dir" : "")
        );
        f.close();
        return true;
    }

    serialDevice->println("file: usage file read|write|list|delete|stat <filename> [data]");
    return false;
}

// ---------------------------------------------------------------------------
// livepatch <apply|reload|list|status>
// ---------------------------------------------------------------------------
// This board has a single factory app partition, so a running firmware image
// cannot be patched from RAM. What CAN be changed live is the JavaScript layer,
// which is what scripts and modules are. `livepatch apply` writes a module under
// /BruceJS/livepatch/<module>.js and reloads it immediately, `reload` re-runs an
// already stored module, `list` shows them and `status` reports the last one.
#define LIVEPATCH_DIR "/BruceJS/livepatch"
static String s_livepatchLast;

static String livepatchPath(const String &module) {
    String name = module;
    if (!name.endsWith(".js")) name += ".js";
    return String(LIVEPATCH_DIR) + "/" + name;
}

static uint32_t livepatchCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String action = cmd.getArgument(0).getValue();
    String module = cmd.getArgument(1).getValue();
    action.trim();
    action.toLowerCase();
    module.trim();

    if (action == "apply") {
        if (module.length() == 0) {
            serialDevice->println("usage: livepatch apply <module> <code>");
            return false;
        }
        // Re-join the code so it may contain spaces.
        String code;
        int n = cmd.countArgs();
        for (int i = 2; i < n; i++) {
            if (code.length()) code += " ";
            code += cmd.getArgument(i).getValue();
        }
        FS *fs = nullptr;
        if (!getFsStorage(fs)) {
            serialDevice->println("livepatch: no filesystem available");
            return false;
        }
        if (!fs->exists(LIVEPATCH_DIR)) fs->mkdir(LIVEPATCH_DIR);
        String path = livepatchPath(module);
        File f = fs->open(path, FILE_WRITE, true);
        if (!f) {
            serialDevice->println("livepatch: cannot write " + path);
            return false;
        }
        size_t written = f.write((const uint8_t *)code.c_str(), code.length());
        f.close();
        s_livepatchLast = module;
        serialDevice->println("livepatch: applied '" + module + "' (" + String((uint32_t)written) + " bytes)");
#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
        run_bjs_script_headless(*fs, path);
        serialDevice->println("livepatch: '" + module + "' reloaded");
#endif
        return true;
    }

    if (action == "reload") {
        if (module.length() == 0) module = s_livepatchLast;
        if (module.length() == 0) {
            serialDevice->println("usage: livepatch reload <module>");
            return false;
        }
        FS *fs = nullptr;
        if (!getFsStorage(fs)) return false;
        String path = livepatchPath(module);
        if (!fs->exists(path)) {
            serialDevice->println("livepatch: no such module '" + module + "' (" + path + ")");
            return false;
        }
        s_livepatchLast = module;
#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
        run_bjs_script_headless(*fs, path);
        serialDevice->println("livepatch: '" + module + "' reloaded");
#else
        serialDevice->println("livepatch: interpreter unavailable");
#endif
        return true;
    }

    if (action == "list") {
        FS *fs = nullptr;
        if (!getFsStorage(fs)) return false;
        if (!fs->exists(LIVEPATCH_DIR)) {
            serialDevice->println("livepatch: no patches (run 'livepatch apply <module> <code>')");
            return true;
        }
        File dir = fs->open(LIVEPATCH_DIR);
        int count = 0;
        while (true) {
            bool isDir = false;
            String full = dir.getNextFileName(&isDir);
            if (full.length() == 0) break;
            serialDevice->println("  " + full.substring(full.lastIndexOf('/') + 1));
            count++;
        }
        dir.close();
        serialDevice->println("livepatch: " + String(count) + " patch(es)");
        return true;
    }

    // status (default)
    serialDevice->println("livepatch: dir=" + String(LIVEPATCH_DIR));
    serialDevice->println(
        "  last module : " + (s_livepatchLast.length() ? s_livepatchLast : String("(none this session)"))
    );
    serialDevice->println("  interpreter : " + String(interpreter_state > 0 ? "running" : "idle"));
    serialDevice->println("  note        : live firmware patching is impossible on this board (single factory");
    serialDevice->println("                partition); livepatch changes the JavaScript layer only.");
    return true;
}

// ---------------------------------------------------------------------------
// liveboot <enable|disable|status>
// ---------------------------------------------------------------------------
// Reuses Bruce's existing startup-app mechanism rather than adding a second boot
// path: enabling liveboot selects the "JS Interpreter" startup app and points it
// at the script, so the same code that already runs app-at-boot runs this.
static uint32_t livebootCallback(cmd *c) {
    if (!requireDebug()) return false;
    Command cmd(c);
    String action = cmd.getArgument(0).getValue();
    String script = cmd.getArgument(1).getValue();
    action.trim();
    action.toLowerCase();
    script.trim();

    if (action == "enable" || action == "on") {
        if (script.length() == 0) {
            // Reuse the configured script if there is one, so `liveboot enable`
            // alone re-arms the previous selection.
            script = bruceConfig.startupAppJSInterpreterFile;
        }
        if (script.length() == 0) {
            serialDevice->println("usage: liveboot enable <script.js>");
            return false;
        }
        if (!script.startsWith("/")) script = "/" + script;
        bruceConfig.setStartupAppJSInterpreterFile(script);
        bruceConfig.setStartupApp("JS Interpreter");
        serialDevice->println("liveboot: enabled - '" + script + "' will run at boot");
        return true;
    }

    if (action == "disable" || action == "off") {
        bruceConfig.setStartupApp("");
        serialDevice->println("liveboot: disabled");
        return true;
    }

    // status (default)
    bool enabled = bruceConfig.startupApp == "JS Interpreter" && bruceConfig.startupAppJSInterpreterFile.length() > 0;
    serialDevice->println("liveboot : " + String(enabled ? "ENABLED" : "disabled"));
    serialDevice->println("  startup app : " + (bruceConfig.startupApp.length() ? bruceConfig.startupApp : String("(none)")));
    serialDevice->println(
        "  script      : " +
        (bruceConfig.startupAppJSInterpreterFile.length() ? bruceConfig.startupAppJSInterpreterFile : String("(none)"))
    );
    return true;
}

void createDebugCommands(SimpleCLI *cli) {
    Command usbDebug = cli->addCommand("usbdebug,usb_dbg,dbg", usbDebugCallback);
    usbDebug.addPosArg("action", "status");
    usbDebug.addPosArg("value", "");

    Command cd = cli->addCommand("cd", cdCallback);
    cd.addPosArg("path", "/");

    cli->addCommand("pwd", pwdCallback);

    cli->addBoundlessCmd("queue,q", queueCallback);

    Command batch = cli->addCommand("batch", batchCallback);
    batch.addPosArg("action", "status");

    Command history = cli->addCommand("history,hist", historyCallback);
    history.addPosArg("limit", "");

    Command complete = cli->addCommand("complete,comp", completeCallback);
    complete.addPosArg("prefix", "");

    Command hot = cli->addCommand("hotreload,hot", hotReloadCallback);
    hot.addPosArg("action", "list");
    hot.addPosArg("path", "");

    cli->addBoundlessCmd("liveupdate,live", liveUpdateCallback);

    Command errors = cli->addCommand("errors,errs", errorsCallback);
    errors.addPosArg("action", "");

    Command log = cli->addCommand("log", logCallback);
    log.addPosArg("action", "");

    cli->addCommand("proc,tasks,ps", procCallback);
    cli->addCommand("sys,devinfo", sysCallback);
    cli->addCommand("mem,meminfo,ram", memCallback);

    Command proto = cli->addCommand("proto", protoCallback);
    proto.addPosArg("mode", "");

    Command ui = cli->addCommand("ui,screen,where,uiinfo", uiCallback);
    ui.addPosArg("action", "dump");
    ui.addPosArg("value", "");

    // Simple, human/AI friendly command set (short verbs, one clear action each).
    Command move = cli->addCommand("move", moveCallback);
    move.addPosArg("direction", "");

    Command press = cli->addCommand("press", pressCallback);
    press.addPosArg("key", "ok");

    Command file = cli->addCommand("file", fileCallback);
    file.addPosArg("action", "");
    file.addPosArg("path", "");
    file.addPosArg("data", "");
    file.addPosArg("data2", "");
    file.addPosArg("data3", "");
    file.addPosArg("data4", "");

    Command livepatch = cli->addCommand("livepatch,lpatch", livepatchCallback);
    livepatch.addPosArg("action", "status");
    livepatch.addPosArg("module", "");
    livepatch.addPosArg("code", "");
    livepatch.addPosArg("code2", "");
    livepatch.addPosArg("code3", "");
    livepatch.addPosArg("code4", "");

    Command liveboot = cli->addCommand("liveboot,lboot", livebootCallback);
    liveboot.addPosArg("action", "status");
    liveboot.addPosArg("script", "");
}
