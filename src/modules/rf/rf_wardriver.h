#ifndef __RF_WARDRIVER_H
#define __RF_WARDRIVER_H

// SubGHz Wardriver - continuous raw capture of whatever the CC1101 hears.
//
// There is no GPS on this board, so a "wardrive" record is the signal itself:
// every burst that is long enough to be a remote gets its own .sub file under
// /bruce/subghz/wardriver, and an identical replay of the same remote is
// counted but not written twice. The screen shows how many were saved, how
// many duplicates were dropped, and the last signal's statistics.
//
// Settings persist in bruceConfig (RF > Wardriver > Config): frequency, the
// duplicate window and the minimum pulse count that separates a remote from
// line noise.

#include <Arduino.h>

#define RF_WARDRIVER_DIR "/bruce/subghz/wardriver"

// Wardriver page: start a capture, browse/replay/identify what was captured,
// and the capture settings.
void rfWardriverMenu();
// `maxSeconds` > 0 stops the capture on its own (console use: `subghz wardriver
// capture 10`); 0 means run until SEL/BACK.
void rfWardriverRun(int maxSeconds = 0);
void rfWardriverConfigMenu();

// Synthetic check of the duplicate filter and the .sub writer (no radio needed).
// Returns a one-line report ending in PASS or FAIL.
String rfWardriverSelfTest();

// Console helpers: the same code the on-device browser uses.
String rfWardriverListFiles();              // one capture name per line
String rfWardriverIdentify(const String &path); // protocol name, "" if not recognised
bool rfWardriverReplay(const String &path);     // transmit the capture verbatim
String rfWardriverIdentifyAllFiles();           // "ok/total"

#endif
