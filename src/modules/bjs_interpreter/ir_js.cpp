#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "ir_js.h"

#include "modules/ir/custom_ir.h"
#include "modules/ir/ir_read.h"
#include "modules/ir/ir_universal.h"

#include "core/sd_functions.h"

#include "helpers_js.h"

// Module registration is performed in `mqjs_stdlib.c`.

JSValue native_irTransmitFile(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "irTransmitFile(filename:string) required");
    FileParamsJS file = js_get_path_from_params(ctx, argv, true, true);
    bool hideDefaultUI = false;
    if (argc > 1 && JS_IsBool(argv[1])) hideDefaultUI = JS_ToBool(ctx, argv[1]);
    bool r = txIrFile(file.fs, file.path, hideDefaultUI);
    return JS_NewBool(r);
}

JSValue native_irTransmit(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "irTransmit(data:string,protocol?:string,bits?:int) required");
    JSCStringBuf sb;
    const char *data = JS_ToCString(ctx, argv[0], &sb);
    const char *protocol = "NEC";
    if (argc > 1 && JS_IsString(ctx, argv[1])) { protocol = JS_ToCString(ctx, argv[1], &sb); }
    int bits = 32;
    if (argc > 2 && JS_IsNumber(ctx, argv[2])) JS_ToInt32(ctx, &bits, argv[2]);
    bool r = sendDecodedCommand(protocol, data, bits);
    return JS_NewBool(r);
}

JSValue native_irRead(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: irRead();
    // usage: irRead(timeout_in_seconds : number);
    // returns a string of the generated ir file, empty string on timeout or other
    // errors
    // This function is registered twice in stdlib: normal and raw. The "raw"
    // variant will be a separate C function (native_irReadRaw) that passes raw=true.
    IrRead irRead = IrRead(true, 0);
    int timeout = 10;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &timeout, argv[0]);
    String result = irRead.loop_headless(timeout);
    return JS_NewString(ctx, result.c_str());
}

JSValue native_irReadRaw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    IrRead irRead = IrRead(true, 1);
    int timeout = 10;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &timeout, argv[0]);
    String result = irRead.loop_headless(timeout);
    return JS_NewString(ctx, result.c_str());
}

JSValue native_irTransmitRaw(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: ir.transmitRaw(frequency: int, rawData: string)
    //   frequency: carrier in kHz (38 = 38kHz)
    //   rawData:   space separated microsecond durations (mark/space pairs)
    // returns: true when the frame was sent, false on bad input
    if (argc < 2 || !JS_IsString(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "irTransmitRaw(frequency:int, rawData:string)");

    int frequency = 38;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &frequency, argv[0]);
    if (frequency <= 0) frequency = 38;

    JSCStringBuf rb;
    const char *rawData = JS_ToCString(ctx, argv[1], &rb);
    if (rawData == NULL || rawData[0] == '\0') return JS_NewBool(false);

    sendRawCommand((uint16_t)frequency, String(rawData));
    return JS_NewBool(true);
}

// ---------------------------------------------------------------------------
// Universal remote library
// ---------------------------------------------------------------------------
// The TV universal library holds one list of candidate codes per button. A
// brute-force script has to walk that list one code at a time, remember which
// code it just sent, and rebuild a whole remote from the code it identified.
// These bindings are that walk - no UI, no progress bar, no pacing (the script
// owns the timing).
//
// Every code is handed to JS as one pipe separated line, field order:
//   name|type|protocol|address|command|data|bits|frequency
// A pipe never appears in these fields (protocols and addresses are hex words,
// raw data is space separated integers), so split('|') is safe.

static String irCodeLine(const IrUniversalCode &c) {
    String s = c.name;
    s += '|';
    s += c.type;
    s += '|';
    s += c.protocol;
    s += '|';
    s += c.address;
    s += '|';
    s += c.command;
    s += '|';
    s += c.data;
    s += '|';
    s += String(c.bits);
    s += '|';
    s += String(c.frequency);
    return s;
}

static JSValue irStringArray(JSContext *ctx, const std::vector<String> &lines) {
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, 0);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (size_t i = 0; i < lines.size(); i++) {
        JS_SetPropertyUint32(ctx, *arr, (uint32_t)i, JS_NewString(ctx, lines[i].c_str()));
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

// ir.universalList() -> ["TV|tv.ir|/infrared/Universal/tv.ir", ...]
JSValue native_irUniversalList(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;

    std::vector<String> lines;
    FS *fs = nullptr;
    if (!getFsStorage(fs) || fs == nullptr) return irStringArray(ctx, lines);

    for (size_t i = 0; i < IR_UNIVERSAL_CATEGORY_COUNT; i++) {
        const IrUniversalCategory &c = IR_UNIVERSAL_CATEGORIES[i];
        if (!irUniversalAvailable(fs, c.file)) continue;
        lines.push_back(String(c.label) + "|" + String(c.file) + "|" + irUniversalPath(c.file));
    }
    return irStringArray(ctx, lines);
}

// The first argument of every path-taking IR binding is either a path string or
// `{ fs, path }`. The script resolves a library on the card or in LittleFS and
// passes the object, so a plain path no longer forces the SD-first lookup (which
// kept reading a slow card even when the same file was already in flash).
static bool irPathArg(JSContext *ctx, JSValue v) {
    return JS_IsString(ctx, v) || JS_IsObject(ctx, v);
}

// ir.universalButtons(path) -> ["Power|113", "Vol_up|97", ...]
JSValue native_irUniversalButtons(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    std::vector<String> lines;
    if (argc < 1 || !irPathArg(ctx, argv[0])) return irStringArray(ctx, lines);

    FileParamsJS file = js_get_path_from_params(ctx, argv, true);
    if (!file.exist) return irStringArray(ctx, lines);

    std::vector<IrUniversalButton> buttons = irUniversalButtonList(file.fs, file.path);
    lines.reserve(buttons.size());
    for (const IrUniversalButton &b : buttons) lines.push_back(b.name + "|" + String(b.count));
    return irStringArray(ctx, lines);
}

// ir.universalCode(path, name, index) -> the pipe separated code, "" when the
// index runs past the end of that button's list.
JSValue native_irUniversalCode(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 3 || !irPathArg(ctx, argv[0]) || !JS_IsString(ctx, argv[1]) || !JS_IsNumber(ctx, argv[2]))
        return JS_ThrowTypeError(ctx, "irUniversalCode(path:string|{fs,path}, name:string, index:int)");

    int index = 0;
    JS_ToInt32(ctx, &index, argv[2]);

    String name = js_tocstring_copy(ctx, argv[1]);
    FileParamsJS file = js_get_path_from_params(ctx, argv, true);
    if (!file.exist) return JS_NewString(ctx, "");

    IrUniversalCode code;
    if (!irUniversalCodeAt(file.fs, file.path, name, index, code)) return JS_NewString(ctx, "");
    return JS_NewString(ctx, irCodeLine(code).c_str());
}

// ir.universalSend(path, name, index) -> true when that one candidate went out.
JSValue native_irUniversalSend(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 3 || !irPathArg(ctx, argv[0]) || !JS_IsString(ctx, argv[1]) || !JS_IsNumber(ctx, argv[2]))
        return JS_ThrowTypeError(ctx, "irUniversalSend(path:string|{fs,path}, name:string, index:int)");

    int index = 0;
    JS_ToInt32(ctx, &index, argv[2]);

    String name = js_tocstring_copy(ctx, argv[1]);
    FileParamsJS file = js_get_path_from_params(ctx, argv, true);
    if (!file.exist) return JS_NewBool(false);

    return JS_NewBool(irUniversalSendAt(file.fs, file.path, name, index));
}

// ir.universalFamily(path, protocol, address) -> every button the library holds
// for that exact remote (same protocol and address field).
JSValue native_irUniversalFamily(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    std::vector<String> lines;
    if (argc < 3 || !irPathArg(ctx, argv[0]) || !JS_IsString(ctx, argv[1]))
        return irStringArray(ctx, lines);

    String protocol = js_tocstring_copy(ctx, argv[1]);
    String address = argc > 2 && JS_IsString(ctx, argv[2]) ? js_tocstring_copy(ctx, argv[2]) : String("");

    FileParamsJS file = js_get_path_from_params(ctx, argv, true);
    if (!file.exist) return irStringArray(ctx, lines);

    for (const IrUniversalCode &c : irUniversalFamily(file.fs, file.path, protocol, address)) {
        lines.push_back(irCodeLine(c));
    }
    return irStringArray(ctx, lines);
}

// ir.transmitButton(path, name) -> send one named button from an IR file
// (a recorded remote or one saved by the Vandeta IR Suite - both are plain .ir files).
JSValue native_irTransmitButton(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2 || !irPathArg(ctx, argv[0]) || !JS_IsString(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "irTransmitButton(path:string|{fs,path}, name:string)");

    String name = js_tocstring_copy(ctx, argv[1]);
    FileParamsJS file = js_get_path_from_params(ctx, argv, true);
    if (!file.exist) return JS_NewBool(false);

    return JS_NewBool(sendIrFileButton(file.fs, file.path, name, true));
}

#endif
