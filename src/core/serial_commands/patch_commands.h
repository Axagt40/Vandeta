#ifndef __PATCH_COMMANDS_H__
#define __PATCH_COMMANDS_H__

#include <SimpleCLI.h>

// Developer-only console surface for the task 1 variant tables (phase 2).
//
// `patch list` names every table with its current index and its variants;
// `patch <name> <index>` selects one. The selection is stored in the phase 1
// tunables table (TUNE_ENUM), so it stays one value with one bruce.conf key
// instead of a second mechanism beside it.
//
// Like `tune`, the whole group answers "ERR: developer mode off" and nothing
// else while Developer Mode is off, and util_commands.cpp leaves it out of
// `help`. Phase 3 extends this same command with the overlay verbs
// (apply|revert|clear|journal); the variant selection stays on the same entry.
void createPatchCommands(SimpleCLI *cli);

#endif
