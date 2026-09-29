#ifndef __DEBUG_COMMANDS_H__
#define __DEBUG_COMMANDS_H__

#include <SimpleCLI.h>

// Registers the Developer Options verbs. They are always registered, but every
// callback refuses to run unless bruceConfig.usbDebug is set, so the console
// surface of a stock build is unchanged.
void createDebugCommands(SimpleCLI *cli);

#endif
