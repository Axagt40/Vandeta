#pragma once

// IR > IR Suite: the Vandeta IR Suite 1.1 (the brute force for every remote type
// the universal libraries cover - TV, AC, Fans, Audio, Projectors, LEDs - by
// brand, by remote or by family, plus the saved-remote pad, payload runner and
// TV mirroring) started from the IR menu.
// The script is embedded in the firmware image (ir_suite_script.h, generated
// from src/modules/ir/ir_suite_src/Vandeta_IR_Suite.js), so no SD card copy is
// needed. The libraries it walks (/infrared/...) still come from the card or
// from LittleFS, whichever has them.
//
// Returns false when the interpreter is not part of this build or the copy
// could not be allocated; the caller then just stays on the menu.
bool runIrSuite();
