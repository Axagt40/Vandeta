#ifndef __DEVFLASH_COMMANDS_H__
#define __DEVFLASH_COMMANDS_H__

#include "core/serial_commands/cli.h"

// Developer-only firmware upload (task 1, phase 4): devflash begin|chunk|end|abort.
void createDevFlashCommands(SimpleCLI *cli);

#endif
