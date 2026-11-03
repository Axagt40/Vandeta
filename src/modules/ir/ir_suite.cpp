#include "ir_suite.h"

#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "core/utils.h"
#include "ir_suite_script.h"
#include "modules/bjs_interpreter/helpers_js.h"
#include "modules/bjs_interpreter/interpreter.h"

#include <esp_heap_caps.h>

// The interpreter wants an owned, mutable buffer and frees it itself
// (run_bjs_script_headless(char *)), so the embedded copy is duplicated once
// into PSRAM. 50 KB would fit the internal heap today but not comfortably next
// to a running radio, and PSRAM is what every other large script buffer uses.
bool runIrSuite() {
    char *code = (char *)ps_malloc(IR_SUITE_JS_LEN + 1);
    if (code == nullptr) code = (char *)malloc(IR_SUITE_JS_LEN + 1);
    if (code == nullptr) {
        print_errorMessage("Not enough memory to start the IR Suite");
        return false;
    }
    memcpy(code, IR_SUITE_JS, IR_SUITE_JS_LEN);
    code[IR_SUITE_JS_LEN] = '\0';
    Serial.printf("IR Suite: starting %u bytes from the firmware image\n", (unsigned)IR_SUITE_JS_LEN);
    return run_bjs_script_headless(code);
}
#else
bool runIrSuite() { return false; }
#endif
