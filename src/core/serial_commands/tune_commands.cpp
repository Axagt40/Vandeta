#include "tune_commands.h"

#include "core/dev/tunables.h"
#include <globals.h>

// Task 1, phase 1: the `tune` verbs.
//
// One command with a `mode` argument, exactly like `mitm` - this CLI matches
// whole command words, so registering "tune/list" and friends only produced
// "Unknown argument at command 'tune/list'". One line of output per call, never
// a modal banner, because these are typed in the middle of a debugging session.
//
// Gating (read twice): the verbs refuse with "ERR: developer mode off" and the
// group is absent from `help` when the switch is off. The VALUES are not gated -
// a tuning saved with developer mode on still applies after it is turned off,
// which is the only way a tuning is worth having.
//
// The switch is bruceConfig.usbDebug and nothing else: bruceConfig.devMode is the
// legacy alias validateConfig() re-syncs at boot, so reading it here made the
// verbs appear immediately after `usbdebug off` on a board that had once
// persisted devMode=1.

static bool tuneAllowed() { return bruceConfig.usbDebug != 0; }

static void tuneRefuse() { serialDevice->println("ERR: developer mode off"); }

// "live" / "pending reboot" / "stored only" - the flags, spelled out, so a reader
// can tell an entry that changes behaviour from one that only stores a number.
static String tuneFlagText(const tune_entry *e) {
    String flags = (e->flags & TUNE_REBOOT) ? "pending reboot" : "live";
    if (e->flags & TUNE_BACKED) flags += ", own conf key";
    if (e->flags & TUNE_PENDING) flags += ", stored only (no consumer yet)";
    return flags;
}

// A variant entry is still one index; without its name the line is unreadable
// ("nrf.capture_variant = 2" says nothing), so the selected variant is always
// shown and `tune list` adds the whole set.
static String tuneVariantText(const tune_entry *e, bool verbose) {
    if (!(e->flags & TUNE_ENUM)) return "";
    const char *cur = tuneVariantName(e);
    if (!verbose) return String(" (") + (cur ? cur : "?") + ")";
    String names;
    for (int i = 0; i < tuneVariantCount(e); i++) {
        if (i) names += "|";
        names += tuneVariantNameAt(e, i);
    }
    return ", variants: " + names;
}

static String tuneLine(const tune_entry *e, bool verbose) {
    String line = String(e->name) + " = " + String(tuneGet(e)) + tuneVariantText(e, false);
    if (verbose) {
        line += " (" + String(tuneTypeName(e->type)) + ") [" + String(e->min) + ".." + String(e->max) +
                "] def " + String(e->def) + " " + tuneFlagText(e) + tuneVariantText(e, true);
    }
    return line;
}

static int tuneStoredCount() {
    int n = 0;
    for (int i = 0; i < tuneCount(); i++) {
        const tune_entry *e = tuneAt(i);
        if (e != nullptr && !(e->flags & TUNE_BACKED)) n++;
    }
    return n;
}

static bool tuneUnknown(const String &name) {
    serialDevice->println("ERR: unknown tunable '" + name + "' (try 'tune list')");
    return false;
}

uint32_t tuneCallback(cmd *c) {
    if (!tuneAllowed()) {
        tuneRefuse();
        return true;
    }

    Command cmd(c);
    // The parser hands the first word to `mode`; keep the rest separate so
    // "tune set rf.park_ms 300" reads name=rf.park_ms value=300.
    String arg1 = cmd.getArgument("a").getValue();
    String arg2 = cmd.getArgument("b").getValue();
    String word = cmd.getArgument("mode").getValue();
    word.trim();
    arg1.trim();
    arg2.trim();

    if (word.length() == 0 || word == "list") {
        for (int i = 0; i < tuneCount(); i++) {
            const tune_entry *e = tuneAt(i);
            if (e != nullptr) serialDevice->println(tuneLine(e, true));
        }
        return true;
    }

    if (word == "get") {
        const tune_entry *e = tuneFind(arg1.c_str());
        if (e == nullptr) return tuneUnknown(arg1);
        serialDevice->println(tuneLine(e, false));
        return true;
    }

    if (word == "set") {
        tune_entry *e = (tune_entry *)tuneFind(arg1.c_str());
        if (e == nullptr) return tuneUnknown(arg1);
        if (arg2.length() == 0) {
            serialDevice->println("ERR: tune set <name> <value>");
            return false;
        }
        bool clamped = false;
        int32_t requested = arg2.toInt();
        int32_t applied = tuneSet(e, requested, &clamped);
        String line = tuneLine(e, false);
        if (clamped) line += " (clamped from " + String(requested) + ")";
        if (e->flags & TUNE_REBOOT) line += " (pending reboot)";
        serialDevice->println(line);
        return true;
    }

    if (word == "reset") {
        if (arg1.length() == 0) {
            tuneResetAll();
            serialDevice->println("tuning reset to defaults (" + String(tuneCount()) + " entries)");
            return true;
        }
        tune_entry *e = (tune_entry *)tuneFind(arg1.c_str());
        if (e == nullptr) return tuneUnknown(arg1);
        tuneReset(e);
        serialDevice->println(tuneLine(e, false) + " (default)");
        return true;
    }

    if (word == "save") {
        bruceConfig.saveFile();
        // Backed entries were written by saveFile() under their own keys already;
        // saying so beats implying they were skipped by mistake.
        serialDevice->println("tuning saved to bruce.conf (" + String(tuneStoredCount()) + " entries)");
        return true;
    }

    if (word == "load") {
        bruceConfig.fromFile(true); // the same validated path boot uses
        serialDevice->println("tuning reloaded from bruce.conf (" + String(tuneStoredCount()) + " entries)");
        return true;
    }

    serialDevice->println("ERR: unknown tune mode '" + word + "' (list|get|set|reset|save|load)");
    return false;
}

void createTuneCommands(SimpleCLI *cli) {
    Command tune = cli->addCommand("tune", tuneCallback);
    tune.addPosArg("mode", "");
    tune.addPosArg("a", "");
    tune.addPosArg("b", "");
}
