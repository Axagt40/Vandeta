#include "ir_suite.h"

#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "core/dev/tunables.h"
#include "core/utils.h"
#include "ir_suite_script.h"
#include "modules/bjs_interpreter/helpers_js.h"
#include "modules/bjs_interpreter/interpreter.h"

#include <esp_heap_caps.h>

// Task 1: `ir.lock_replays` is how many of the codes just sent the suite keeps
// for the narrowing replay. The script is generated at build time with its own
// `var LOCK_CODES = 10;`, so the value is written into this private copy before
// the interpreter sees it: `tune set ir.lock_replays N` applies to the next IR
// Suite launch, no rebuild. The replacement never widens the literal it edits
// (max 50 is two digits, the generated literal is two), so no buffer growth.
static void applyLockReplays(char *code) {
    static const char key[] = "var LOCK_CODES = ";
    char *at = strstr(code, key);
    if (at == nullptr) return;
    char *num = at + sizeof(key) - 1;
    char *end = strchr(num, ';');
    if (end == nullptr) return;
    char buf[8];
    int n = snprintf(buf, sizeof(buf), "%d", (int)tuneGetByName("ir.lock_replays", nullptr));
    size_t span = (size_t)(end - num);
    if (n <= 0 || (size_t)n > span) return;
    memcpy(num, buf, (size_t)n);
    memset(num + n, ' ', span - (size_t)n); // pad to keep the line length
}

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
    applyLockReplays(code);
    Serial.printf("IR Suite: starting %u bytes from the firmware image\n", (unsigned)IR_SUITE_JS_LEN);
    const bool started = run_bjs_script_headless(code);
    // The interpreter runs in its own task and run_bjs_script_headless() leaves
    // returnToMenu set; the menu that launched the suite would read that as
    // "leave" and unwind all the way to Main Menu behind the script's back.
    // Clearing it here keeps the menu stack parked while the suite owns the
    // screen, which is what the IR > IR Suite row always meant.
    returnToMenu = false;
    return started;
}
#else
bool runIrSuite() { return false; }
#endif
