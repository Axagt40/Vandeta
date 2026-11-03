#ifndef __DEBUG_COMMANDS_H__
#define __DEBUG_COMMANDS_H__

#include <SimpleCLI.h>

// Registers the Developer Options verbs. They are always registered, but every
// callback refuses to run unless bruceConfig.usbDebug is set, so the console
// surface of a stock build is unchanged.
void createDebugCommands(SimpleCLI *cli);

// Polled from UsbDebug::loop() on every tick (about 10 ms). While `ui watch on`
// is active it emits the current screen whenever it changes, so a host sees the
// menu update live as keys are pressed (on the device or over the console).
// It is a no-op when watching is off or Developer Options are disabled.
void debugPollUiWatch();

// Stops the live screen watcher. Called by UsbDebug::releaseResources() when the
// Developer Options setting is turned off, so the watcher cannot stay armed
// while the mode is disabled.
void debugUiWatchOff();

#endif
