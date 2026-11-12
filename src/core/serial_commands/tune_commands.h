#ifndef __TUNE_COMMANDS_H__
#define __TUNE_COMMANDS_H__

#include <SimpleCLI.h>

// Developer-only console surface for the task 1 tunables table.
//
// The commands are always registered (so the parser stays one list), but every
// callback answers "ERR: developer mode off" and nothing else while Developer
// Mode is off, and util_commands.cpp leaves the whole group out of `help` in
// that state. The VALUES are never gated: they live in bruce.conf and apply to
// a stock build too.
void createTuneCommands(SimpleCLI *cli);

#endif
