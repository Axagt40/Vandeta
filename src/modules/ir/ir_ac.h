#ifndef __IR_AC_H
#define __IR_AC_H

#include <Arduino.h>

// Brand AC remotes for the Universal Remote > AC page.
//
// The .ir libraries on the card hold fixed button codes from one captured
// remote (Electrolux in ac.ir), so the AC page could only ever switch *that*
// unit on and off. An air conditioner is not a button remote though: every
// transmission carries the whole state (power, mode, temperature, fan), which
// is exactly what IRremoteESP8266's per-brand AC classes build. This page drives
// those classes, so the popular makes below get a real AC remote - set 22 C on
// cool and only that frame goes out.
//
// The file libraries stay available from the same page ("Library codes").

struct IrAcState {
    bool power = true;
    uint8_t mode = 0;  // 0 cool, 1 heat, 2 dry, 3 fan, 4 auto
    uint8_t temp = 24; // celsius
    uint8_t fan = 0;   // 0 auto, 1 low, 2 medium, 3 high
};

// One transmission with the given state. Returns false for a brand this build
// cannot encode.
bool irAcSend(int brand, const IrAcState &state);

// Brand list, index -> name ("Daikin", "Gree", ...). -1 for an unknown index.
const char *irAcBrandName(int brand);
int irAcBrandCount();

// The on-device page: brand -> live state screen.
void irAcMenu();

#endif
