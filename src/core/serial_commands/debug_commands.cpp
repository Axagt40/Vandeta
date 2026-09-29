#include "debug_commands.h"

#include "core/radio_mem.h"
#include "core/usb_debug/usbdebug.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <globals.h>

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
    serialDevice->println("error stream: " + String(UsbDebug::errorStream() ? "ON" : "OFF"));
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
    serialDevice->println("version     : Bruce v" + String(BRUCE_VERSION));
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
        " (needs " + String((uint32_t)(RADIO_BLE_MIN_INTERNAL_FREE / 1024)) + "KB free internal + " +
        String((uint32_t)(RADIO_BLE_MIN_DMA_BLOCK / 1024)) + "KB contiguous DMA, no WiFi free attempt)"
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
}
