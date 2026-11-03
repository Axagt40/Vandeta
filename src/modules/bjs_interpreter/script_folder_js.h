#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#ifndef __SCRIPT_FOLDER_JS_H__
#define __SCRIPT_FOLDER_JS_H__

#include "helpers_js.h"

extern "C" {
// scriptFolder.*
JSValue native_scriptFolderCreate(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_scriptFolderList(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_scriptFolderLoad(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_scriptFolderRun(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_scriptFolderClose(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_scriptFolderIsRunning(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_scriptFolderGetAllScripts(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_scriptFolderStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_scriptFolderKill(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
// Shared key/value store so background scripts (which have their own global
// scope) can still exchange state with each other and with the main script.
JSValue native_scriptFolderSetShared(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_scriptFolderGetShared(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue native_scriptFolderClearShared(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

/**
 * @brief True when @p ctx belongs to a background script job.
 *
 * Background jobs run on their own JSContext and outlive the main script, so
 * the interpreter-wide "should this script keep going" flag (interpreter_state,
 * which goes negative when the foreground script ends) must not stop them.
 * globals_js.cpp consults this from delay() and the timer pump.
 */
bool scriptfolder_is_background_ctx(JSContext *ctx);

/**
 * @brief Should @p ctx keep running?
 *
 * For the foreground interpreter this is just "interpreter_state >= 0". For a
 * background job it is "not stopped", so a job survives the main script
 * exiting. Used by delay() and the timer pump in globals_js.cpp.
 */
bool scriptfolder_ctx_should_run(JSContext *ctx);

/// Stop every background job and release all buffers (called on script exit).
void scriptfolder_cleanup();
}

#endif
#endif
