#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "keyboard_js.h"

#include "core/mykeyboard.h"

#include "helpers_js.h"

// Enum for keyboard types
enum KeyboardType { KEYBOARD_NORMAL, KEYBOARD_HEX, KEYBOARD_NUM };

// Helper function that handles the common argument parsing logic for keyboard functions
static JSValue keyboard_wrapper(JSContext *ctx, JSValue *argv, int argc, KeyboardType type) {
    String result = "";

    if (argc == 0) {
        switch (type) {
            case KEYBOARD_NORMAL: result = keyboard(""); break;
            case KEYBOARD_HEX: result = hex_keyboard(""); break;
            case KEYBOARD_NUM: result = num_keyboard(""); break;
        }
    } else if (JS_IsNumber(ctx, argv[0])) {
        int max_size = 0;
        if (JS_ToInt32(ctx, &max_size, argv[0])) return JS_EXCEPTION;
        const char *msg = "";
        if (argc > 1 && JS_IsString(ctx, argv[1])) {
            JSCStringBuf buf;
            msg = JS_ToCString(ctx, argv[1], &buf);
        }
        bool mask_input = JS_IsBool(argv[argc - 1]) ? JS_ToBool(ctx, argv[argc - 1]) : false;
        switch (type) {
            case KEYBOARD_NORMAL: result = keyboard("", max_size, msg, mask_input); break;
            case KEYBOARD_HEX: result = hex_keyboard("", max_size, msg, mask_input); break;
            case KEYBOARD_NUM: result = num_keyboard("", max_size, msg, mask_input); break;
        }
    } else if (JS_IsString(ctx, argv[0])) {
        JSCStringBuf buf0;
        const char *title = JS_ToCString(ctx, argv[0], &buf0);
        if (argc == 1 || !JS_IsNumber(ctx, argv[1])) {
            switch (type) {
                case KEYBOARD_NORMAL: result = keyboard(title ? title : ""); break;
                case KEYBOARD_HEX: result = hex_keyboard(title ? title : ""); break;
                case KEYBOARD_NUM: result = num_keyboard(title ? title : ""); break;
            }
        } else {
            int max_size = 0;
            if (JS_ToInt32(ctx, &max_size, argv[1])) return JS_EXCEPTION;
            if (argc < 3 || !JS_IsString(ctx, argv[2])) {
                switch (type) {
                    case KEYBOARD_NORMAL: result = keyboard(title ? title : "", max_size); break;
                    case KEYBOARD_HEX: result = hex_keyboard(title ? title : "", max_size); break;
                    case KEYBOARD_NUM: result = num_keyboard(title ? title : "", max_size); break;
                }
            } else {
                JSCStringBuf buf2;
                const char *msg = JS_ToCString(ctx, argv[2], &buf2);
                bool mask_input = JS_IsBool(argv[argc - 1]) ? JS_ToBool(ctx, argv[argc - 1]) : false;
                switch (type) {
                    case KEYBOARD_NORMAL:
                        result = keyboard(title ? title : "", max_size, msg ? msg : "", mask_input);
                        break;
                    case KEYBOARD_HEX:
                        result = hex_keyboard(title ? title : "", max_size, msg ? msg : "", mask_input);
                        break;
                    case KEYBOARD_NUM:
                        result = num_keyboard(title ? title : "", max_size, msg ? msg : "", mask_input);
                        break;
                }
            }
        }
    } else {
        switch (type) {
            case KEYBOARD_NORMAL: result = keyboard(""); break;
            case KEYBOARD_HEX: result = hex_keyboard(""); break;
            case KEYBOARD_NUM: result = num_keyboard(""); break;
        }
    }
    if (result == "\x1B") result = "";
    return JS_NewString(ctx, result.c_str());
}

JSValue native_keyboard(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return keyboard_wrapper(ctx, argv, argc, KEYBOARD_NORMAL);
}

JSValue native_hex_keyboard(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return keyboard_wrapper(ctx, argv, argc, KEYBOARD_HEX);
}

JSValue native_num_keyboard(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return keyboard_wrapper(ctx, argv, argc, KEYBOARD_NUM);
}

JSValue native_getPrevPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return JS_NewBool(check(PrevPress));
}

JSValue native_getSelPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return JS_NewBool(check(SelPress));
}

JSValue native_getEscPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return JS_NewBool(check(EscPress));
}

JSValue native_getNextPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return JS_NewBool(check(NextPress));
}

// Rotary encoder detents collected since the last call, with the firmware's own
// sign convention (negative = next/clockwise). The JS IR Suite uses it to skip
// candidates during a sweep without transmitting while the wheel is moving.
extern int32_t drainRotarySteps();

JSValue native_getRotarySteps(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return JS_NewInt32(ctx, drainRotarySteps());
}

JSValue native_getAnyPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return JS_NewBool(check(AnyKeyPress));
}

JSValue native_setLongPress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // TODO: if backgroud app implemented, store in ctx and set if on foreground/background
    LongPress = (argc > 0) ? JS_ToBool(ctx, argv[0]) : false;
    return JS_UNDEFINED;
}

JSValue native_getKeysPressed(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // Every JS_NewString()/JS_SetPropertyUint32() below can allocate, so the
    // array and the string being appended both have to be GC roots. Re-reading
    // from the ref after the append also guarantees the append itself used a
    // current pointer. See native_wifiGetCapturedPackets() in wifi_js.cpp.
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, 0);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
#ifdef HAS_KEYBOARD
    keyStroke key = _getKeyPress();
    if (!key.pressed) return JS_PopGCRef(ctx, &arr_ref);
    uint32_t arrayIndex = 0;
    JSGCRef s_ref;
    JSValue *s = JS_PushGCRef(ctx, &s_ref);
    *s = JS_UNDEFINED;
    for (auto i : key.word) {
        char str[2] = {(char)i, '\0'};
        *s = JS_NewString(ctx, str);
        JS_SetPropertyUint32(ctx, *arr, arrayIndex++, *s);
    }
    if (key.del) {
        *s = JS_NewString(ctx, "Delete");
        JS_SetPropertyUint32(ctx, *arr, arrayIndex++, *s);
    }
    if (key.enter) {
        *s = JS_NewString(ctx, "Enter");
        JS_SetPropertyUint32(ctx, *arr, arrayIndex++, *s);
    }
    if (key.fn) {
        *s = JS_NewString(ctx, "Function");
        JS_SetPropertyUint32(ctx, *arr, arrayIndex++, *s);
    }
    for (auto i : key.modifier_keys) {
        if (i == 0x82) {
            *s = JS_NewString(ctx, "Alt");
            JS_SetPropertyUint32(ctx, *arr, arrayIndex++, *s);
        } else if (i == 0x2B) {
            *s = JS_NewString(ctx, "Tab");
            JS_SetPropertyUint32(ctx, *arr, arrayIndex++, *s);
        } else if (i == 0x00) {
            *s = JS_NewString(ctx, "Option");
            JS_SetPropertyUint32(ctx, *arr, arrayIndex++, *s);
        }
    }
    JS_PopGCRef(ctx, &s_ref);
#endif
    return JS_PopGCRef(ctx, &arr_ref);
}

#endif
