#pragma once

#include <Arduino.h>
#include <FS.h>
#include <vector>

/***************************************************************************************
** Universal IR remotes - the Flipper/Momentum model, drawn with the Bruce UI.
**
** A "universal remote" is a list of candidate codes per button. Picking the
** button sends every code that carries that name, one after another, until the
** device reacts (or the list runs out). That is exactly what Flipper's universal
** remote scenes do: the library files (tv.ir, ac.ir, audio.ir, projectors.ir,
** fans.ir, leds.ir) hold all the candidates, and the app walks them.
**
** Here the libraries live on the card under /infrared/Universal/ and are read
** a record at a time, so a 250 KB library never has to be in RAM.
***************************************************************************************/

#define IR_UNIVERSAL_DIR "/infrared/Universal"

struct IrUniversalCategory {
    const char *label; // what the menu shows
    const char *file;  // file name inside IR_UNIVERSAL_DIR
};

// The six categories the Flipper/Momentum port ships.
extern const IrUniversalCategory IR_UNIVERSAL_CATEGORIES[];
extern const size_t IR_UNIVERSAL_CATEGORY_COUNT;

// Full path of a category file, e.g. "/infrared/Universal/tv.ir".
String irUniversalPath(const char *file);

// True when the library for that category is on the card.
bool irUniversalAvailable(FS *fs, const char *file);

// One row of the button menu: the name, and how many candidate codes carry it.
struct IrUniversalButton {
    String name;
    uint16_t count = 0;
};

// Unique button names in file order (this is the menu the device shows) with the
// number of candidate codes each one has. One 512-byte blockwise pass over the
// file - it replaced a per-line scan that spent ~3.4 s on the 257 KB fans.ir.
std::vector<IrUniversalButton> irUniversalButtonList(FS *fs, const String &path);

// Names only, for callers that do not show a progress bar.
std::vector<String> irUniversalButtons(FS *fs, const String &path);

// Send every code named `button` (at most `maxSends`, 0 = all). Returns how many
// were transmitted; protocols this firmware cannot encode are counted as
// skipped and reported once on the console.
// `totalHint` is the candidate count from irUniversalButtonList(): with it the
// sweep sends its first code immediately instead of pre-scanning the whole file
// just to size the progress bar.
int irUniversalSend(
    FS *fs, const String &path, const String &button, int maxSends = 0, bool showUI = false,
    volatile bool *stop = nullptr, int totalHint = 0
);

// One code from a library, for callers that walk the candidates themselves
// (the Vandeta IR Suite script): every field the Flipper record carries, plus its position
// among the codes that share the same name.
struct IrUniversalCode {
    String name;
    String type;   // "parsed" | "raw"
    String protocol;
    String address;
    String command;
    String data;   // raw timings, or the `value:`/`state:` of a parsed code
    uint16_t bits = 32;
    uint32_t frequency = 0;
    uint16_t index = 0;
};

// Read the `index`-th code named `button` (0-based). false when the library runs
// out. Reads record by record, so asking for code 300 costs one pass, not an
// index of the whole file in RAM.
bool irUniversalCodeAt(FS *fs, const String &path, const String &button, int index, IrUniversalCode &out);

// Transmit exactly that one candidate. false when it does not exist or the
// protocol cannot be encoded by this firmware.
bool irUniversalSendAt(FS *fs, const String &path, const String &button, int index);

// Every code in the library that belongs to the same remote: matching protocol
// AND the same address field. That pair is what identifies one device, so this
// is how an identified power code is turned back into a usable remote (Power,
// Vol_up, Mute ... whatever the library holds for that address).
std::vector<IrUniversalCode> irUniversalFamily(
    FS *fs, const String &path, const String &protocol, const String &address, int max = 64
);

// The on-device menu: category -> button -> transmit.
void irUniversalMenu();

// Diagnostic: the microsecond frame the hand-written RCA / NEC42 encoders will
// transmit, space separated (empty for protocols handled by the shared sender).
String irUniversalRawFrame(const String &protocol, uint32_t address, uint32_t command);
