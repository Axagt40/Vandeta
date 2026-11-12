#ifndef __MODEM_COMMANDS_H__
#define __MODEM_COMMANDS_H__

#include "core/serial_commands/cli.h"

// Task 4: `modem send <text> [baud] [ultrasonic]` / `modem recv <secs>`.
void createModemCommands(SimpleCLI *cli);

#endif
