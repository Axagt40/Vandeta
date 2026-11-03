#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "script_folder_js.h"

#include "globals_js.h"
#include "helpers_js.h"
#include "interpreter.h"

#include "core/sd_functions.h"
#include <globals.h>

#include <FS.h>
#include <LittleFS.h>
#include <SD.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

extern "C" {
#include "mqjs_stdlib.h"
}

// The interpreter exports the running script's location as these two buffers
// (see interpreter.cpp); scriptFolder uses "the current directory" = the folder
// the running script was started from.
extern char *scriptDirpath;
extern char *scriptName;

// ---------------------------------------------------------------------------
// Limits and state
// ---------------------------------------------------------------------------

#define SF_MAX_ENTRIES 8    // scripts that can be tracked at once
#define SF_MAX_BG 3         // background jobs allowed to run simultaneously
#define SF_MAX_SHARED 16    // shared key/value pairs
#define SF_MIN_MEM 32768
#define SF_MAX_MEM (256 * 1024)
#define SF_DEFAULT_MEM (96 * 1024)

enum {
    SF_IDLE = 0,    // known but not loaded
    SF_LOADED = 1,  // source in memory
    SF_RUNNING = 2,
    SF_DONE = 3,
    SF_STOPPED = 4,
    SF_ERROR = 5,
    SF_KILLED = 6,
};

struct SfEntry {
    bool used;
    String name;    // name as the script asked for it
    String path;    // resolved absolute path
    String fsName;  // "sd" or "littlefs"
    uint32_t size;
    FS *fs;
    char *src;      // loaded source (owned; ps_malloc'd by readBigFile)
    size_t srcLen;

    TaskHandle_t task;
    JSContext *bgCtx;   // only valid while the job runs
    uint8_t *bgMem;
    size_t bgMemSize;
    volatile bool stopRequested;
    volatile int state;
    uint32_t runs;
    uint32_t startedAt;
    String lastError;
    String argStore[8]; // options.args, handed to the job as the global __args
    int argCount;
};

/// mquickjs has no JS_IsArray(), so go through the object class.
static bool sf_is_array(JSContext *ctx, JSValue val) {
    if (!JS_IsObject(ctx, val)) return false;
    return JS_GetClassID(ctx, val) == JS_CLASS_ARRAY;
}

static SfEntry s_entries[SF_MAX_ENTRIES];
static SemaphoreHandle_t s_mtx = NULL;
static String s_sharedKey[SF_MAX_SHARED];
static String s_sharedVal[SF_MAX_SHARED];
static int s_sharedCount = 0;

static void sf_lock() {
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
}
static void sf_unlock() {
    if (s_mtx) xSemaphoreGive(s_mtx);
}
static void sf_ensure_mutex() {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
}

static void sf_log_func(void *opaque, const void *buf, size_t buf_len) {
    fwrite(buf, 1, buf_len, stdout);
}

// ---------------------------------------------------------------------------
// Background context registry (consulted by globals_js.cpp)
// ---------------------------------------------------------------------------

bool scriptfolder_is_background_ctx(JSContext *ctx) {
    sf_ensure_mutex();
    sf_lock();
    bool found = false;
    for (int i = 0; i < SF_MAX_ENTRIES && !found; i++) {
        if (s_entries[i].used && s_entries[i].bgCtx == ctx) found = true;
    }
    sf_unlock();
    return found;
}

void scriptfolder_register_ctx(JSContext *ctx, bool background) {
    // The context is already stored in the entry before the task starts, so
    // there is nothing to do for the background case; the call exists so the
    // task can state its intent explicitly.
    (void)ctx;
    (void)background;
}

/**
 * @brief Should this context keep running?
 *
 * The interpreter-wide flag (interpreter_state) goes negative when the
 * foreground script ends, which must not stop a background job: those run on
 * their own context and keep going until they are stopped or closed.
 */
bool scriptfolder_ctx_should_run(JSContext *ctx) {
    sf_ensure_mutex();
    sf_lock();
    SfEntry *entry = NULL;
    for (int i = 0; i < SF_MAX_ENTRIES; i++) {
        if (s_entries[i].used && s_entries[i].bgCtx == ctx) {
            entry = &s_entries[i];
            break;
        }
    }
    bool stop = entry ? entry->stopRequested : false;
    sf_unlock();
    if (entry) return !stop;
    return interpreter_state >= 0;
}

static int sf_bg_running() {
    int n = 0;
    for (int i = 0; i < SF_MAX_ENTRIES; i++) {
        if (s_entries[i].used && s_entries[i].state == SF_RUNNING) n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Path / entry helpers
// ---------------------------------------------------------------------------

/// The directory the running script came from, or the usual scripts folder.
static String sf_base_dir() {
    if (scriptDirpath != NULL && scriptDirpath[0] != '\0') return String(scriptDirpath);
    FS *fs = &LittleFS;
    String folder = getScriptsFolder(fs);
    if (folder.length()) return folder;
    return String("/scripts");
}

/// Turn a script name (or path) into an absolute path plus the filesystem.
static bool sf_resolve_path(const String &rawName, String &path, FS **outFs, String &fsTag, String &err) {
    String name = rawName;
    name.trim();
    if (name.length() == 0) {
        err = "script name is required";
        return false;
    }

    path = name;
    if (!path.startsWith("/")) path = sf_base_dir() + "/" + path;

    if (sdcardMounted && (SD.exists(path) || SD.exists(path + ".js"))) {
        if (!SD.exists(path)) path += ".js";
        *outFs = &SD;
        fsTag = "sd";
        return true;
    }
    if (LittleFS.exists(path) || LittleFS.exists(path + ".js")) {
        if (!LittleFS.exists(path)) path += ".js";
        *outFs = &LittleFS;
        fsTag = "littlefs";
        return true;
    }

    *outFs = sdcardMounted ? (FS *)&SD : (FS *)&LittleFS;
    fsTag = sdcardMounted ? "sd" : "littlefs";
    err = "not found: " + path;
    return false;
}

static int sf_find(const String &name) {
    for (int i = 0; i < SF_MAX_ENTRIES; i++) {
        if (!s_entries[i].used) continue;
        if (s_entries[i].name == name || s_entries[i].path == name) return i;
    }
    return -1;
}

/// Look up an entry by name/path; @p create adds a new one when it is missing.
static int sf_get_or_create(const String &name, bool create, String &err) {
    sf_ensure_mutex();
    sf_lock();
    int idx = sf_find(name);
    if (idx >= 0 || !create) {
        sf_unlock();
        if (idx < 0) err = "script is not loaded: " + name;
        return idx;
    }
    for (int i = 0; i < SF_MAX_ENTRIES; i++) {
        if (!s_entries[i].used) {
            s_entries[i] = SfEntry();
            s_entries[i].used = true;
            s_entries[i].name = name;
            s_entries[i].state = SF_IDLE;
            s_entries[i].fs = NULL;
            sf_unlock();
            return i;
        }
    }
    sf_unlock();
    String msg = "too many tracked scripts (max ";
    msg += SF_MAX_ENTRIES;
    msg += "); close() one first";
    err = msg;
    return -1;
}

/// Load the entry's source into memory (no-op when it is already loaded).
static bool sf_load_source(SfEntry &e, String &err) {
    if (e.src != NULL) return true;

    String path;
    FS *fs = NULL;
    String fsTag;
    if (e.path.length() == 0) {
        if (!sf_resolve_path(e.name, path, &fs, fsTag, err)) return false;

        e.path = path;
        e.fsName = fsTag;
    } else {
        fs = e.fs;
        if (fs == NULL) {
            if (sdcardMounted && SD.exists(e.path)) {
                fs = &SD;
                e.fsName = "sd";
            } else {
                fs = &LittleFS;
                e.fsName = "littlefs";
            }
        }
    }
    e.fs = fs;

    size_t len = 0;
    char *buf = readBigFile(fs, e.path, false, &len);
    if (buf == NULL) {
        err = "could not read " + e.path;
        return false;
    }
    e.src = buf;
    e.srcLen = len;
    e.size = (uint32_t)len;
    e.state = SF_LOADED;
    return true;
}

// ---------------------------------------------------------------------------
// Background job task
// ---------------------------------------------------------------------------

static void sf_set_args(JSContext *ctx, String *args, int count);

static void sf_setup_globals(JSContext *ctx, SfEntry &e) {
    JSGCRef global_ref;
    JSValue *global = JS_PushGCRef(ctx, &global_ref);
    *global = JS_GetGlobalObject(ctx);
    if (JS_IsException(*global)) {
        JS_PopGCRef(ctx, &global_ref);
        return;
    }
    JS_SetPropertyStr(ctx, *global, "__filepath", JS_NewString(ctx, e.path.c_str()));
    String dir = e.path;
    int slash = dir.lastIndexOf('/');
    if (slash > 0) dir = dir.substring(0, slash);
    JS_SetPropertyStr(ctx, *global, "__dirpath", JS_NewString(ctx, dir.c_str()));
    JS_SetPropertyStr(ctx, *global, "__script", JS_NewString(ctx, e.name.c_str()));
    JS_SetPropertyStr(ctx, *global, "BRUCE_VERSION", JS_NewString(ctx, BRUCE_VERSION));
    JS_SetPropertyStr(ctx, *global, "BRUCE_PRICOLOR", JS_NewInt32(ctx, bruceConfig.priColor));
    JS_SetPropertyStr(ctx, *global, "BRUCE_SECCOLOR", JS_NewInt32(ctx, bruceConfig.secColor));
    JS_SetPropertyStr(ctx, *global, "BRUCE_BGCOLOR", JS_NewInt32(ctx, bruceConfig.bgColor));
    JS_SetPropertyStr(ctx, *global, "HIGH", JS_NewInt32(ctx, HIGH));
    JS_SetPropertyStr(ctx, *global, "LOW", JS_NewInt32(ctx, LOW));
    JS_SetPropertyStr(ctx, *global, "INPUT", JS_NewInt32(ctx, INPUT));
    JS_SetPropertyStr(ctx, *global, "OUTPUT", JS_NewInt32(ctx, OUTPUT));
    JS_SetPropertyStr(ctx, *global, "PULLUP", JS_NewInt32(ctx, PULLUP));
    JS_SetPropertyStr(ctx, *global, "INPUT_PULLUP", JS_NewInt32(ctx, INPUT_PULLUP));
    JS_SetPropertyStr(ctx, *global, "PULLDOWN", JS_NewInt32(ctx, PULLDOWN));
    JS_SetPropertyStr(ctx, *global, "INPUT_PULLDOWN", JS_NewInt32(ctx, INPUT_PULLDOWN));
    JS_PopGCRef(ctx, &global_ref);
}

static int sf_interrupt_handler(JSContext *ctx, void *opaque) {
    SfEntry *e = (SfEntry *)opaque;
    if (e && e->stopRequested) return 1;
    return 0;
}

/// Read the pending exception into @p out and clear it. "Script exited" is a
/// clean stop, not an error.
static bool sf_capture_exception(JSContext *ctx, String &out) {
    JSValue ex = JS_GetException(ctx);
    if (JS_IsUndefined(ex) || JS_IsNull(ex)) return true;

    JSCStringBuf buf;
    const char *msg = JS_ToCString(ctx, ex, &buf);
    out = msg ? String(msg) : String("unknown error");
    return out.indexOf("Script exited") >= 0;
}

static void sf_job_task(void *arg) {
    sf_ensure_mutex();
    SfEntry *e = (SfEntry *)arg;
    uint8_t *mem = e->bgMem;
    size_t memSize = e->bgMemSize;

    JSContext *ctx = JS_NewContext(mem, memSize, &js_stdlib);
    if (ctx == NULL) {
        sf_lock();
        e->state = SF_ERROR;
        e->lastError = "could not create a JS context";
        e->task = NULL;
        sf_unlock();
        if (mem) free(mem);
        e->bgMem = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Published before any JavaScript runs: scriptfolder_ctx_should_run() and
    // scriptfolder_is_background_ctx() match on this pointer.
    sf_lock();
    e->bgCtx = ctx;
    sf_unlock();

    JS_SetLogFunc(ctx, sf_log_func);
    // The interpreter polls this handler from its bytecode loop, which is how
    // stop() aborts a runaway script without killing the task.
    JS_SetContextOpaque(ctx, e);
    JS_SetInterruptHandler(ctx, sf_interrupt_handler);

    js_timers_init(ctx);
    sf_setup_globals(ctx, *e);
    if (e->argCount > 0) sf_set_args(ctx, e->argStore, e->argCount);

    // JS_EVAL_REPL: an assignment to an undeclared name ("X = 1;" without var)
    // defines a global instead of raising a ReferenceError. mquickjs only does
    // that for REPL-style evaluation, and a script fragment is exactly that.
    JSValue val = JS_Eval(ctx, e->src, e->srcLen, e->name.c_str(), JS_EVAL_REPL);
    String errText;
    int finalState = SF_DONE;

    if (JS_IsException(val)) {
        bool cleanExit = sf_capture_exception(ctx, errText);
        finalState = cleanExit ? SF_STOPPED : SF_ERROR;
    } else {
        // Timers keep a background script alive (e.g. setInterval); run_timers()
        // returns as soon as there are none left or the job is stopped.
        run_timers(ctx);
        if (e->stopRequested) finalState = SF_STOPPED;
    }

    js_timers_deinit(ctx);
    JS_FreeContext(ctx);
    if (mem) free(mem);

    sf_lock();
    e->bgCtx = NULL;
    e->bgMem = NULL;
    e->bgMemSize = 0;
    e->task = NULL;
    e->state = finalState;
    if (finalState == SF_ERROR) e->lastError = errText;
    sf_unlock();

    Serial.printf(
        "[SF] background job '%s' finished (state=%d%s)\n",
        e->name.c_str(),
        finalState,
        finalState == SF_ERROR ? (String(", error: ") + errText).c_str() : ""
    );
    vTaskDelete(NULL);
}

/// Start @p e as a background job. @p memSize 0 picks the default.
static bool sf_start_background(SfEntry &e, size_t memSize, String &err) {
    if (memSize == 0) {
        size_t maxAlloc = psramFound() ? ESP.getMaxAllocPsram() : ESP.getMaxAllocHeap();
        memSize = SF_DEFAULT_MEM;
        if (maxAlloc > 0 && maxAlloc < memSize + 16384) memSize = maxAlloc / 2;
    }
    if (memSize < SF_MIN_MEM) memSize = SF_MIN_MEM;
    if (memSize > SF_MAX_MEM) memSize = SF_MAX_MEM;

    uint8_t *mem = psramFound() ? (uint8_t *)ps_malloc(memSize) : (uint8_t *)malloc(memSize);
    if (mem == NULL) {
        err = "could not allocate " + String((unsigned)memSize) + " bytes for the script heap";
        return false;
    }

    e.bgMem = mem;
    e.bgMemSize = memSize;
    e.stopRequested = false;
    e.state = SF_RUNNING;
    e.startedAt = millis();
    e.runs++;
    e.lastError = "";

    TaskHandle_t handle = NULL;
    BaseType_t rc = xTaskCreateUniversal(
        sf_job_task,
        "sfScript",
        8192,
        &e,
        1,
        &handle,
        ARDUINO_RUNNING_CORE
    );
    if (rc != pdPASS) {
        free(mem);
        e.bgMem = NULL;
        e.bgMemSize = 0;
        e.state = SF_ERROR;
        err = "could not start the background task";
        return false;
    }
    e.task = handle;
    // e.bgCtx is filled in by sf_job_task() as soon as the context exists.
    return true;
}

// ---------------------------------------------------------------------------
// scriptFolder.create
// ---------------------------------------------------------------------------

JSValue native_scriptFolderCreate(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.create(folderName: string)
    //   Creates a folder (nested paths like "attacks/wifi" are created level by
    //   level) inside the current script directory.
    // returns: { ok, folder, path, fs, created, error? }
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "scriptFolder.create(folderName:string)");
    }
    String name = js_tocstring_copy(ctx, argv[0]);
    name.trim();

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    JS_SetPropertyStr(ctx, *obj, "folder", JS_NewString(ctx, name.c_str()));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));

    if (name.length() == 0) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "folderName is required"));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    String base = sf_base_dir();
    // New folders go next to the running script, on whichever filesystem it
    // lives (SD when there is a card, LittleFS otherwise).
    bool baseOnSd = sdcardMounted && SD.exists(base);
    FS *fs = baseOnSd ? (FS *)&SD : (FS *)&LittleFS;

    String path = name.startsWith("/") ? name : (base + "/" + name);
    JS_SetPropertyStr(ctx, *obj, "path", JS_NewString(ctx, path.c_str()));
    JS_SetPropertyStr(ctx, *obj, "fs", JS_NewString(ctx, baseOnSd ? "sd" : "littlefs"));

    if (fs->exists(path)) {
        JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
        JS_SetPropertyStr(ctx, *obj, "created", JS_NewBool(false));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    // mkdir() is single level, so walk the path from the base downwards.
    String partial = "";
    int start = name.startsWith("/") ? 1 : 0;
    bool ok = true;
    int pos = start;
    while (pos <= (int)path.length()) {
        int slash = path.indexOf('/', pos);
        if (slash < 0) {
            partial = path;
        } else {
            partial = path.substring(0, slash);
        }
        if (partial.length() > 1 && !fs->exists(partial)) {
            if (!fs->mkdir(partial)) {
                ok = false;
                break;
            }
        }
        if (slash < 0) break;
        pos = slash + 1;
    }

    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(ok));
    JS_SetPropertyStr(ctx, *obj, "created", JS_NewBool(ok));
    if (!ok) {
        String msg = "mkdir failed: ";
        msg += path;
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, msg.c_str()));
    }
    return JS_PopGCRef(ctx, &obj_ref);
}

// ---------------------------------------------------------------------------
// scriptFolder.list
// ---------------------------------------------------------------------------

JSValue native_scriptFolderList(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.list(folderName?: string)
    //   Lists folders and .js/.bjs files. The default folder is the directory
    //   the running script was started from.
    // returns: { ok, path, fs, count, entries: [{ name, path, isDir, size }],
    //            error? }  (folders count as entries, so a folder that only
    //   contains sub-folders is not empty)
    String folder = sf_base_dir();
    if (argc > 0 && JS_IsString(ctx, argv[0]) && !JS_IsUndefined(argv[0])) {
        String arg = js_tocstring_copy(ctx, argv[0]);
        arg.trim();
        if (arg.length()) folder = arg.startsWith("/") ? arg : (sf_base_dir() + "/" + arg);
    }

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "path", JS_NewString(ctx, folder.c_str()));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));

    bool onSd = sdcardMounted && SD.exists(folder);
    FS *fs = onSd ? (FS *)&SD : (FS *)&LittleFS;
    JS_SetPropertyStr(ctx, *obj, "fs", JS_NewString(ctx, onSd ? "sd" : "littlefs"));

    File root = fs->open(folder);
    if (!root || !root.isDirectory()) {
        String msg = "not a directory: ";
        msg += folder;
        JSGCRef empty_ref;
        JSValue *empty = JS_PushGCRef(ctx, &empty_ref);
        *empty = JS_NewArray(ctx, 0);
        JS_SetPropertyStr(ctx, *obj, "entries", *empty);
        JS_SetPropertyStr(ctx, *obj, "count", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, msg.c_str()));
        JS_PopGCRef(ctx, &empty_ref);
        return JS_PopGCRef(ctx, &obj_ref);
    }

    // Collect first (the JS array must not be built inside the directory walk
    // because each JS_NewString can move the heap).
    String names[48];
    bool dirs[48];
    uint32_t sizes[48];
    int count = 0;
    while (count < 48) {
        bool isDir = false;
        String fullPath = root.getNextFileName(&isDir);
        if (fullPath.length() == 0) break;

        String base = fullPath.substring(fullPath.lastIndexOf('/') + 1);
        if (base.startsWith(".")) continue;

        if (!isDir) {
            int dot = base.lastIndexOf('.');
            String ext = dot >= 0 ? base.substring(dot + 1) : "";
            ext.toUpperCase();
            if (ext != "JS" && ext != "BJS") continue;
        }

        uint32_t size = 0;
        if (!isDir) {
            File f = fs->open(fullPath);
            if (f) {
                size = (uint32_t)f.size();
                f.close();
            }
        }
        names[count] = base;
        dirs[count] = isDir;
        sizes[count] = size;
        count++;
    }
    root.close();

    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, count);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_PopGCRef(ctx, &obj_ref);
    }
    for (int i = 0; i < count; i++) {
        JSGCRef entry_ref;
        JSValue *entry = JS_PushGCRef(ctx, &entry_ref);
        *entry = JS_NewObject(ctx);
        if (JS_IsException(*entry)) {
            JS_PopGCRef(ctx, &entry_ref);
            break;
        }
        JS_SetPropertyStr(ctx, *entry, "name", JS_NewString(ctx, names[i].c_str()));
        JS_SetPropertyStr(ctx, *entry, "path", JS_NewString(ctx, (folder + "/" + names[i]).c_str()));
        JS_SetPropertyStr(ctx, *entry, "isDir", JS_NewBool(dirs[i]));
        JS_SetPropertyStr(ctx, *entry, "size", JS_NewInt32(ctx, (int)sizes[i]));
        JS_SetPropertyUint32(ctx, *arr, (uint32_t)i, *entry);
        JS_PopGCRef(ctx, &entry_ref);
    }
    JS_SetPropertyStr(ctx, *obj, "entries", *arr);
    JS_SetPropertyStr(ctx, *obj, "count", JS_NewInt32(ctx, count));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_PopGCRef(ctx, &arr_ref);
    return JS_PopGCRef(ctx, &obj_ref);
}

// ---------------------------------------------------------------------------
// scriptFolder.load
// ---------------------------------------------------------------------------

JSValue native_scriptFolderLoad(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.load(scriptName: string)
    //   Reads the script from SD/LittleFS into memory and remembers it under
    //   `scriptName`. It is not executed until run() is called.
    // returns: { ok, name, path, fs, size, state, error? }
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "scriptFolder.load(scriptName:string)");
    }
    String name = js_tocstring_copy(ctx, argv[0]);
    name.trim();

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "name", JS_NewString(ctx, name.c_str()));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));

    String err;
    int idx = sf_get_or_create(name, true, err);
    if (idx < 0) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, err.c_str()));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    SfEntry &e = s_entries[idx];

    if (!sf_load_source(e, err)) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, err.c_str()));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "path", JS_NewString(ctx, e.path.c_str()));
    JS_SetPropertyStr(ctx, *obj, "fs", JS_NewString(ctx, e.fsName.c_str()));
    JS_SetPropertyStr(ctx, *obj, "size", JS_NewInt32(ctx, (int)e.srcLen));
    JS_SetPropertyStr(ctx, *obj, "state", JS_NewString(ctx, "loaded"));
    return JS_PopGCRef(ctx, &obj_ref);
}

// ---------------------------------------------------------------------------
// scriptFolder.run
// ---------------------------------------------------------------------------

/// Copy a JS array of arguments (or a single string) into @p out.
static void sf_read_args(JSContext *ctx, JSValue val, String *out, int *count, int max) {
    *count = 0;
    if (JS_IsUndefined(val) || JS_IsNull(val)) return;

    if (JS_IsString(ctx, val)) {
        out[(*count)++] = js_tocstring_copy(ctx, val);
        return;
    }
    if (!JS_IsObject(ctx, val)) return;

    JSValue lenVal = JS_GetPropertyStr(ctx, val, "length");
    int len = 0;
    if (JS_IsNumber(ctx, lenVal)) JS_ToInt32(ctx, &len, lenVal);
    if (len <= 0) return;
    if (len > max) len = max;

    for (int i = 0; i < len; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, val, (uint32_t)i);
        if (JS_IsUndefined(item)) continue;
        out[(*count)++] = js_tocstring_copy(ctx, item);
    }
}

/// Set the global `__args` array in the given context.
static void sf_set_args(JSContext *ctx, String *args, int count) {
    JSGCRef global_ref;
    JSValue *global = JS_PushGCRef(ctx, &global_ref);
    *global = JS_GetGlobalObject(ctx);
    if (JS_IsException(*global)) {
        JS_PopGCRef(ctx, &global_ref);
        return;
    }
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, count);
    if (!JS_IsException(*arr)) {
        for (int i = 0; i < count; i++) {
            JS_SetPropertyUint32(ctx, *arr, (uint32_t)i, JS_NewString(ctx, args[i].c_str()));
        }
        JS_SetPropertyStr(ctx, *global, "__args", *arr);
        JS_SetPropertyStr(ctx, *global, "__argc", JS_NewInt32(ctx, count));
    }
    JS_PopGCRef(ctx, &arr_ref);
    JS_PopGCRef(ctx, &global_ref);
}

JSValue native_scriptFolderRun(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.run(scriptName: string, options?: {
    //            background?: bool,   // run in its own JS context + task (default false)
    //            wait?: bool,         // with background: block until it finishes
    //            timeoutMs?: int,     // with wait: give up after this long (0 = wait)
    //            memory?: int,        // background heap size (32768..262144)
    //            args?: string[]      // passed to the script as the global __args
    //        })
    //
    //   Foreground (default) evaluates the script in THIS context, so it shares
    //   globals with the caller: that is how split scripts call each other.
    //   background:true gives the script its own context (own globals, keeps
    //   running after the caller returns) and its own FreeRTOS task.
    //
    // returns: { ok, name, mode, state, background, runs, error? }
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "scriptFolder.run(scriptName:string, options?:object)");
    }
    String name = js_tocstring_copy(ctx, argv[0]);
    name.trim();

    bool background = false, wait = false;
    int timeoutMs = 0, memory = 0;
    String args[8];
    int argCount = 0;

    if (argc > 1 && JS_IsObject(ctx, argv[1]) && !sf_is_array(ctx, argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "background");
        if (!JS_IsUndefined(v)) background = JS_ToBool(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "wait");
        if (!JS_IsUndefined(v)) wait = JS_ToBool(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "timeoutMs");
        if (JS_IsNumber(ctx, v)) JS_ToInt32(ctx, &timeoutMs, v);
        v = JS_GetPropertyStr(ctx, argv[1], "memory");
        if (JS_IsNumber(ctx, v)) JS_ToInt32(ctx, &memory, v);
        v = JS_GetPropertyStr(ctx, argv[1], "args");
        sf_read_args(ctx, v, args, &argCount, 8);
    } else if (argc > 1 && sf_is_array(ctx, argv[1])) {
        sf_read_args(ctx, argv[1], args, &argCount, 8);
    }
    if (timeoutMs < 0) timeoutMs = 0;

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "name", JS_NewString(ctx, name.c_str()));
    JS_SetPropertyStr(ctx, *obj, "mode", JS_NewString(ctx, background ? "background" : "foreground"));
    JS_SetPropertyStr(ctx, *obj, "background", JS_NewBool(background));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));

    String err;
    int idx = sf_get_or_create(name, true, err);
    if (idx < 0) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, err.c_str()));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    SfEntry &e = s_entries[idx];
    if (!sf_load_source(e, err)) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, err.c_str()));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    if (!background) {
        // Foreground: nested evaluation in the caller's context, so the script
        // shares this global scope (that is how split scripts see each other).
        // The outcome is reported instead of propagated: a broken part-file
        // gives the caller a usable error rather than the crash screen.
        //
        // JS_EVAL_REPL makes "X = 1;" define a global (see the background
        // note); JS_EVAL_RETVAL keeps the script's last value so run() can hand
        // it back as `result`.
        sf_set_args(ctx, args, argCount);
        JSValue val = JS_Eval(
            ctx, e.src, e.srcLen, e.name.c_str(), JS_EVAL_REPL | JS_EVAL_RETVAL
        );
        String errText;
        bool ok = true;
        JSGCRef val_ref;
        bool pinned = false;
        if (JS_IsException(val)) {
            bool cleanExit = sf_capture_exception(ctx, errText);
            if (!cleanExit) {
                ok = false;
                e.state = SF_ERROR;
                e.lastError = errText;
                Serial.printf("[SF] '%s' failed: %s\n", e.name.c_str(), errText.c_str());
            } else {
                e.state = SF_STOPPED;
            }
        } else {
            e.state = SF_DONE;
            // Pinned: everything below allocates and can move the heap.
            JSValue *pv = JS_PushGCRef(ctx, &val_ref);
            *pv = val;
            pinned = true;
        }
        e.runs++;
        JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(ok));
        JS_SetPropertyStr(ctx, *obj, "state", JS_NewString(ctx, ok ? "done" : "error"));
        JS_SetPropertyStr(ctx, *obj, "runs", JS_NewInt32(ctx, (int)e.runs));
        JS_SetPropertyStr(ctx, *obj, "path", JS_NewString(ctx, e.path.c_str()));
        if (!ok) JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, errText.c_str()));
        if (pinned) {
            if (!JS_IsUndefined(val_ref.val)) JS_SetPropertyStr(ctx, *obj, "result", val_ref.val);
            JS_PopGCRef(ctx, &val_ref);
        }
        return JS_PopGCRef(ctx, &obj_ref);
    }

    if (e.state == SF_RUNNING) {
        String msg = "already running: ";
        msg += name;
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, msg.c_str()));
        JS_SetPropertyStr(ctx, *obj, "state", JS_NewString(ctx, "running"));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    sf_ensure_mutex();
    sf_lock();
    int running = sf_bg_running();
    sf_unlock();
    if (running >= SF_MAX_BG) {
        String msg = "too many background scripts (max ";
        msg += SF_MAX_BG;
        msg += ")";
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, msg.c_str()));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    for (int i = 0; i < argCount && i < 8; i++) e.argStore[i] = args[i];
    e.argCount = argCount;
    if (!sf_start_background(e, (size_t)memory, err)) {
        e.argCount = 0;
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, err.c_str()));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    if (wait) {
        uint32_t start = millis();
        while (e.state == SF_RUNNING) {
            if (timeoutMs > 0 && (int)(millis() - start) > timeoutMs) break;
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }

    const char *stateName = "running";
    if (e.state == SF_DONE) stateName = "done";
    else if (e.state == SF_STOPPED) stateName = "stopped";
    else if (e.state == SF_ERROR) stateName = "error";
    else if (e.state == SF_KILLED) stateName = "killed";

    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(e.state != SF_ERROR));
    JS_SetPropertyStr(ctx, *obj, "state", JS_NewString(ctx, stateName));
    JS_SetPropertyStr(ctx, *obj, "runs", JS_NewInt32(ctx, (int)e.runs));
    JS_SetPropertyStr(ctx, *obj, "path", JS_NewString(ctx, e.path.c_str()));
    if (e.state == SF_ERROR && e.lastError.length()) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, e.lastError.c_str()));
    }
    return JS_PopGCRef(ctx, &obj_ref);
}

// ---------------------------------------------------------------------------
// scriptFolder.stop / kill / close / isRunning / getAllScripts
// ---------------------------------------------------------------------------

static JSValue sf_stop_or_kill(
    JSContext *ctx, int argc, JSValue *argv, bool force, const char *usage
) {
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_ThrowTypeError(ctx, "%s", usage);
    String name = js_tocstring_copy(ctx, argv[0]);
    name.trim();

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "name", JS_NewString(ctx, name.c_str()));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));

    String err;
    int idx = sf_get_or_create(name, false, err);
    if (idx < 0) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, err.c_str()));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    SfEntry &e = s_entries[idx];
    bool wasRunning = (e.state == SF_RUNNING);

    if (!wasRunning) {
        JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
        JS_SetPropertyStr(ctx, *obj, "wasRunning", JS_NewBool(false));
        JS_SetPropertyStr(ctx, *obj, "state", JS_NewString(ctx, "not running"));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    if (!force) {
        // Graceful: the interrupt handler aborts the script at its next
        // bytecode boundary. A script blocked in a native call stops only when
        // that call returns, so the state may stay "running" for a moment.
        e.stopRequested = true;
        JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
        JS_SetPropertyStr(ctx, *obj, "wasRunning", JS_NewBool(true));
        JS_SetPropertyStr(ctx, *obj, "state", JS_NewString(ctx, "stopping"));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    TaskHandle_t task = e.task;
    e.stopRequested = true;
    if (task == xTaskGetCurrentTaskHandle()) {
        // A background script killing itself. Deleting the running task here
        // would tear the interpreter down mid-call, so downgrade it to a stop:
        // the job task notices the flag at its next bytecode boundary and
        // unwinds its own context.
        JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
        JS_SetPropertyStr(ctx, *obj, "wasRunning", JS_NewBool(true));
        JS_SetPropertyStr(ctx, *obj, "state", JS_NewString(ctx, "stopping"));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    if (task) {
        vTaskDelete(task);
        sf_lock();
        e.task = NULL;
        if (e.bgCtx) {
            JS_FreeContext(e.bgCtx);
            e.bgCtx = NULL;
        }
        if (e.bgMem) {
            free(e.bgMem);
            e.bgMem = NULL;
        }
        e.bgMemSize = 0;
        e.state = SF_KILLED;
        sf_unlock();
    } else {
        e.state = SF_KILLED;
    }

    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "wasRunning", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "state", JS_NewString(ctx, "killed"));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_scriptFolderStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.stop(scriptName: string)
    //   Asks a background script to stop. It is cooperative: the engine aborts
    //   the script at its next bytecode boundary, so a script can finish what
    //   it is doing (an infinite `while(true){...}` loop or a long delay()
    //   stops as soon as it next runs JavaScript).
    // returns: { ok, name, wasRunning, state, error? }
    return sf_stop_or_kill(ctx, argc, argv, false, "scriptFolder.stop(scriptName:string)");
}

JSValue native_scriptFolderKill(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.kill(scriptName: string)
    //   Forceful: deletes the script's task immediately and frees its JS heap.
    //   The script gets no chance to clean up (use stop() when you can).
    // returns: { ok, name, wasRunning, state, error? }
    return sf_stop_or_kill(ctx, argc, argv, true, "scriptFolder.kill(scriptName:string)");
}

JSValue native_scriptFolderClose(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.close(scriptName: string)
    //   Stops the script if needed and releases its source buffer and entry.
    // returns: { ok, name, wasRunning, released, error? }
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "scriptFolder.close(scriptName:string)");
    }
    String name = js_tocstring_copy(ctx, argv[0]);
    name.trim();

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "name", JS_NewString(ctx, name.c_str()));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));

    String err;
    int idx = sf_get_or_create(name, false, err);
    if (idx < 0) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, err.c_str()));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    SfEntry &e = s_entries[idx];
    bool wasRunning = (e.state == SF_RUNNING);

    if (wasRunning) {
        TaskHandle_t task = e.task;
        e.stopRequested = true;
        // Same guard as stop()/kill(): a job closing itself must not delete the
        // task it is running on.
        if (task && task != xTaskGetCurrentTaskHandle()) vTaskDelete(task);
    }

    sf_lock();
    if (e.bgCtx) {
        JS_FreeContext(e.bgCtx);
        e.bgCtx = NULL;
    }
    if (e.bgMem) {
        free(e.bgMem);
        e.bgMem = NULL;
        e.bgMemSize = 0;
    }
    if (e.src) {
        free(e.src);
        e.src = NULL;
    }
    e.srcLen = 0;
    e.task = NULL;
    e.state = SF_IDLE;
    e.used = false;
    sf_unlock();

    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "wasRunning", JS_NewBool(wasRunning));
    JS_SetPropertyStr(ctx, *obj, "released", JS_NewBool(true));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_scriptFolderIsRunning(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.isRunning(scriptName: string) -> boolean
    if (argc < 1 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "scriptFolder.isRunning(scriptName:string)");
    }
    String name = js_tocstring_copy(ctx, argv[0]);
    name.trim();

    sf_ensure_mutex();
    sf_lock();
    int idx = sf_find(name);
    bool running = (idx >= 0 && s_entries[idx].state == SF_RUNNING);
    sf_unlock();
    return JS_NewBool(running);
}

JSValue native_scriptFolderGetAllScripts(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.getAllScripts()
    //   Every script this context knows about, with its state and counters.
    // returns: { ok, count, running, scripts: [{ name, path, size, state,
    //            running, runs, error? }] }
    static const char *kStateNames[] = {"idle", "loaded", "running", "done", "stopped", "error", "killed"};

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    sf_ensure_mutex();
    sf_lock();
    int count = 0, running = 0;
    for (int i = 0; i < SF_MAX_ENTRIES; i++) {
        if (!s_entries[i].used) continue;
        count++;
        if (s_entries[i].state == SF_RUNNING) running++;
    }

    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, count);
    if (JS_IsException(*arr)) {
        sf_unlock();
        JS_PopGCRef(ctx, &arr_ref);
        return JS_PopGCRef(ctx, &obj_ref);
    }

    int out = 0;
    for (int i = 0; i < SF_MAX_ENTRIES; i++) {
        if (!s_entries[i].used) continue;
        SfEntry &e = s_entries[i];

        JSGCRef entry_ref;
        JSValue *entry = JS_PushGCRef(ctx, &entry_ref);
        *entry = JS_NewObject(ctx);
        if (JS_IsException(*entry)) {
            JS_PopGCRef(ctx, &entry_ref);
            break;
        }
        int st = e.state;
        if (st < 0 || st > SF_KILLED) st = SF_IDLE;
        JS_SetPropertyStr(ctx, *entry, "name", JS_NewString(ctx, e.name.c_str()));
        JS_SetPropertyStr(ctx, *entry, "path", JS_NewString(ctx, e.path.c_str()));
        JS_SetPropertyStr(ctx, *entry, "size", JS_NewInt32(ctx, (int)e.srcLen));
        JS_SetPropertyStr(ctx, *entry, "state", JS_NewString(ctx, kStateNames[st]));
        JS_SetPropertyStr(ctx, *entry, "running", JS_NewBool(st == SF_RUNNING));
        JS_SetPropertyStr(ctx, *entry, "background", JS_NewBool(e.bgMem != NULL || e.bgCtx != NULL));
        JS_SetPropertyStr(ctx, *entry, "runs", JS_NewInt32(ctx, (int)e.runs));
        if (e.lastError.length()) {
            JS_SetPropertyStr(ctx, *entry, "error", JS_NewString(ctx, e.lastError.c_str()));
        }
        JS_SetPropertyUint32(ctx, *arr, (uint32_t)out++, *entry);
        JS_PopGCRef(ctx, &entry_ref);
    }
    sf_unlock();

    JS_SetPropertyStr(ctx, *obj, "scripts", *arr);
    JS_SetPropertyStr(ctx, *obj, "count", JS_NewInt32(ctx, out));
    JS_SetPropertyStr(ctx, *obj, "running", JS_NewInt32(ctx, running));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_PopGCRef(ctx, &arr_ref);
    return JS_PopGCRef(ctx, &obj_ref);
}

// ---------------------------------------------------------------------------
// Shared key/value store (extras; they make split scripts possible when the
// pieces run in separate background contexts)
// ---------------------------------------------------------------------------

JSValue native_scriptFolderSetShared(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.setShared(key: string, value: string|number|boolean)
    // returns: { ok, key, value, keys, error? }
    if (argc < 2 || !JS_IsString(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "scriptFolder.setShared(key:string, value:any)");
    }
    String key = js_tocstring_copy(ctx, argv[0]);
    key.trim();
    String value = js_tocstring_copy(ctx, argv[1]);

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "key", JS_NewString(ctx, key.c_str()));
    JS_SetPropertyStr(ctx, *obj, "value", JS_NewString(ctx, value.c_str()));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));

    if (key.length() == 0) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "key is required"));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    sf_ensure_mutex();
    sf_lock();
    for (int i = 0; i < s_sharedCount; i++) {
        if (s_sharedKey[i] == key) {
            s_sharedVal[i] = value;
            sf_unlock();
            JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
            JS_SetPropertyStr(ctx, *obj, "keys", JS_NewInt32(ctx, s_sharedCount));
            return JS_PopGCRef(ctx, &obj_ref);
        }
    }
    if (s_sharedCount < SF_MAX_SHARED) {
        s_sharedKey[s_sharedCount] = key;
        s_sharedVal[s_sharedCount] = value;
        s_sharedCount++;
        sf_unlock();
        JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
        JS_SetPropertyStr(ctx, *obj, "keys", JS_NewInt32(ctx, s_sharedCount));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    sf_unlock();
    String msg = "shared store full (max ";
    msg += SF_MAX_SHARED;
    msg += ")";
    JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, msg.c_str()));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_scriptFolderGetShared(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.getShared(key?: string)
    //   With a key: the value, or "" when the key is not set.
    //   Without a key: { keys: [ { key, value } ] } for the whole store.
    if (argc >= 1 && JS_IsString(ctx, argv[0]) && !JS_IsUndefined(argv[0])) {
        String key = js_tocstring_copy(ctx, argv[0]);
        key.trim();
        sf_ensure_mutex();
        sf_lock();
        String value;
        for (int i = 0; i < s_sharedCount; i++) {
            if (s_sharedKey[i] == key) {
                value = s_sharedVal[i];
                break;
            }
        }
        sf_unlock();
        return JS_NewString(ctx, value.c_str());
    }

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    sf_ensure_mutex();
    sf_lock();
    int count = s_sharedCount;
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, count);
    if (!JS_IsException(*arr)) {
        for (int i = 0; i < count; i++) {
            JSGCRef entry_ref;
            JSValue *entry = JS_PushGCRef(ctx, &entry_ref);
            *entry = JS_NewObject(ctx);
            if (JS_IsException(*entry)) {
                JS_PopGCRef(ctx, &entry_ref);
                break;
            }
            JS_SetPropertyStr(ctx, *entry, "key", JS_NewString(ctx, s_sharedKey[i].c_str()));
            JS_SetPropertyStr(ctx, *entry, "value", JS_NewString(ctx, s_sharedVal[i].c_str()));
            JS_SetPropertyUint32(ctx, *arr, (uint32_t)i, *entry);
            JS_PopGCRef(ctx, &entry_ref);
        }
        JS_SetPropertyStr(ctx, *obj, "keys", *arr);
    }
    sf_unlock();
    JS_SetPropertyStr(ctx, *obj, "count", JS_NewInt32(ctx, count));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_PopGCRef(ctx, &arr_ref);
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_scriptFolderClearShared(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: scriptFolder.clearShared()
    // returns: { ok, cleared }
    sf_ensure_mutex();
    sf_lock();
    int before = s_sharedCount;
    s_sharedCount = 0;
    sf_unlock();

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "cleared", JS_NewInt32(ctx, before));
    return JS_PopGCRef(ctx, &obj_ref);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void scriptfolder_cleanup() {
    sf_ensure_mutex();

    // 1. Ask every job to stop.
    sf_lock();
    for (int i = 0; i < SF_MAX_ENTRIES; i++) {
        if (s_entries[i].used && s_entries[i].state == SF_RUNNING) s_entries[i].stopRequested = true;
    }
    sf_unlock();

    // 2. Give them a moment to unwind on their own.
    for (int round = 0; round < 40; round++) {
        sf_lock();
        int running = sf_bg_running();
        sf_unlock();
        if (running == 0) break;
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    // 3. Kill whatever is left, then release every buffer.
    for (int i = 0; i < SF_MAX_ENTRIES; i++) {
        if (!s_entries[i].used) continue;
        sf_lock();
        TaskHandle_t task = s_entries[i].task;
        if (task) vTaskDelete(task);
        s_entries[i].task = NULL;
        if (s_entries[i].bgCtx) {
            JS_FreeContext(s_entries[i].bgCtx);
            s_entries[i].bgCtx = NULL;
        }
        if (s_entries[i].bgMem) {
            free(s_entries[i].bgMem);
            s_entries[i].bgMem = NULL;
        }
        if (s_entries[i].src) {
            free(s_entries[i].src);
            s_entries[i].src = NULL;
        }
        s_entries[i].used = false;
        sf_unlock();
    }

    s_sharedCount = 0;
}

#endif
