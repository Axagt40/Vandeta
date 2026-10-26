#ifndef __USB_DEBUG_H__
#define __USB_DEBUG_H__

#include <Arduino.h>
#include <FS.h>

/*********************************************************************
**  UsbDebug - the "Developer Options" mode, previously "USB Debugging"
**
**  Enabled by bruceConfig.usbDebug and persisted in /bruce.conf, so the
**  mode survives reboots. When it is on, the USB-CDC console additionally
**  accepts the debug verb set (cd/pwd, batch queue, history, autocomplete,
**  hot reload, live update, error stream, process list) and can be switched
**  to a framed binary protocol for fast file transfer.
**
**  The mode is deliberately built on top of what stock Bruce already has:
**  the SimpleCLI command table (src/core/serial_commands/) and the
**  interpreter's `js` command are reused rather than re-implemented.
*********************************************************************/
namespace UsbDebug {

// --- lifecycle ---------------------------------------------------------
// enabled() reports the persisted master toggle.
bool enabled();
// devMode() reports the "speed over stability" sub-toggle.
bool devMode();
// begin() is called once from setup(), after the config is loaded. It
// installs the log hook and prints the boot banner when the mode is on.
void begin();
// applyEnabled() is called when the persisted setting is changed at runtime.
// Turning it ON resizes the USB-CDC RX queue so the framed protocol can fit a
// whole frame; turning it OFF calls releaseResources().
void applyEnabled(bool on);
// releaseResources() gives back everything the mode allocated or switched on
// (error stream, batch queue, hot-reload watches, UI watcher, binary protocol
// and the enlarged USB-CDC RX queue), so a device with Developer Options OFF is
// as light as a stock build.
void releaseResources();
// loop() is pumped from the serial command task. It drains the batch queue
// and polls the hot-reload watches. Safe to call when the mode is off.
void loop();

// --- filesystem working directory -------------------------------------
String cwd();
bool changeDir(const String &path);
// resolve() maps a path that may be relative against the current directory.
String resolve(const String &path);

// --- command history / autocomplete -----------------------------------
void remember(const String &line);
// expandHistory() turns "!!" and "!<n>" into the recalled command.
String expandHistory(const String &line);
void printHistory(size_t limit);
// complete() returns the candidate command names for a prefix, one per line.
String complete(const String &prefix);
// verbList() returns the debug-only verbs, space separated.
const char *verbList();

// --- command queuing / batch execution --------------------------------
void queueCommand(const String &line);
void clearQueue();
void setBatchMode(bool on);
bool batchMode();
bool batchBusy();

// --- hot reload (JavaScript) ------------------------------------------
bool addWatch(const String &path);
// Same, on an explicit filesystem: JS paths can land on LittleFS while the card
// is mounted, and getFsStorage() only ever returns the card.
bool addWatch(FS *fs, const String &path);
bool removeWatch(const String &path);
void clearWatches();
void listWatches();
void setHotReload(bool on);
bool hotReload();

// --- live update (no reflash) -----------------------------------------
// what = "config" | "assets" | "script:<path>" | "status"
String liveUpdate(const String &what);

// --- error / log ring -------------------------------------------------
void pushError(const String &msg);
void printErrors(bool clear);
void setErrorStream(bool on);
bool errorStream();

// --- serial pump ------------------------------------------------------
// pollSerial() gives the debug engine the first look at the console. It
// returns true when it consumed the input itself (binary mode), so the
// normal text CLI must not also read the port.
bool pollSerial();
// pollText() is the text-mode line editor. It consumes the bytes available on
// the console and returns true once a whole line is ready, handling backspace,
// Ctrl-C/U/L, TAB completion and Up/Down recall of earlier commands. Call it
// instead of Stream::readStringUntil() so a command is executed the moment
// Enter arrives and the arrow keys work like they do on a Flipper Zero's
// terminal. Inert in binary mode.
bool pollText(String &line);

// --- binary protocol --------------------------------------------------
bool binaryMode();
void setBinaryMode(bool on);
// sendFrame() writes one framed message (magic, type, len, payload, crc16).
void sendFrame(uint8_t type, const uint8_t *payload, size_t len);
void sendTextFrame(uint8_t type, const String &text);
// sendText() writes a plain text line when in text mode, otherwise it is
// wrapped in an OUT frame so a binary host still sees status messages.
void sendText(const String &s);

// --- developer mode ---------------------------------------------------
// applyDevMode() raises the CPU clock and keeps the panel awake, trading
// stability for speed while the mode is on.
void applyDevMode(bool on);

} // namespace UsbDebug

#endif
