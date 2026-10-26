#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "cast_js.h"

#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <esp_heap_caps.h>

#include "core/sd_functions.h"
#include "helpers_js.h"

// ---------------------------------------------------------------------------
// Small JS helpers (the ones in wifi_js.cpp are static to that file)
// ---------------------------------------------------------------------------
static String cast_js_str_arg(JSContext *ctx, JSValue *argv, int argc, int index) {
    if (argc <= index || !JS_IsString(ctx, argv[index])) return String("");
    return js_tocstring_copy(ctx, argv[index]);
}

static int cast_js_int_arg(JSContext *ctx, JSValue *argv, int argc, int index, int fallback) {
    int value = fallback;
    if (argc > index && JS_IsNumber(ctx, argv[index]) && !JS_ToInt32(ctx, &value, argv[index])) return value;
    return fallback;
}

static JSValue cast_js_string_array(JSContext *ctx, const std::vector<String> &lines) {
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

// ---------------------------------------------------------------------------
// URL handling: UPnP descriptions give the control URL relative to LOCATION
// ---------------------------------------------------------------------------
static String urlOrigin(const String &url) {
    int scheme = url.indexOf("://");
    if (scheme < 0) return "";
    int slash = url.indexOf('/', scheme + 3);
    return slash < 0 ? url : url.substring(0, slash);
}

static String absoluteUrl(const String &base, const String &rel) {
    String r = rel;
    r.trim();
    if (r.length() == 0) return "";
    if (r.startsWith("http://") || r.startsWith("https://")) return r;
    String origin = urlOrigin(base);
    if (origin.length() == 0) return "";
    if (r.charAt(0) == '/') return origin + r;
    // relative to the description's folder
    int slash = base.lastIndexOf('/');
    return (slash > 0 ? base.substring(0, slash + 1) : origin + "/") + r;
}

// ---------------------------------------------------------------------------
// SSDP: find every DLNA/UPnP MediaRenderer on the LAN
// ---------------------------------------------------------------------------
static void ssdpCollectLocations(uint32_t waitMs, std::vector<String> &locations) {
    WiFiUDP udp;
    if (!udp.begin(0)) return;

    const char *request = "M-SEARCH * HTTP/1.1\r\n"
                          "HOST: 239.255.255.250:1900\r\n"
                          "MAN: \"ssdp:discover\"\r\n"
                          "MX: 2\r\n"
                          "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                          "\r\n";
    udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
    udp.write((const uint8_t *)request, strlen(request));
    udp.endPacket();

    uint32_t deadline = millis() + waitMs;
    while (millis() < deadline) {
        int size = udp.parsePacket();
        if (size <= 0) {
            delay(20);
            continue;
        }
        char buf[512];
        int n = udp.read(buf, sizeof(buf) - 1);
        if (n <= 0) continue;
        buf[n] = 0;
        String response(buf);
        int at = response.indexOf("LOCATION:");
        if (at < 0) at = response.indexOf("Location:");
        if (at < 0) continue;
        int start = response.indexOf(':', at) + 1;
        int end = response.indexOf('\r', start);
        if (end < 0) end = response.indexOf('\n', start);
        if (end < 0) end = response.length();
        String location = response.substring(start, end);
        location.trim();
        if (location.length() == 0) continue;
        bool seen = false;
        for (size_t i = 0; i < locations.size(); i++) {
            if (locations[i] == location) {
                seen = true;
                break;
            }
        }
        if (!seen) locations.push_back(location);
    }
    udp.stop();
}

// Pull the friendly name and the AVTransport control URL out of a UPnP
// description. String search, not XML: the fields we need are single-line and
// every renderer emits them verbatim.
static bool ssdpDescribe(const String &location, String &name, String &control) {
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(2500);
    if (!http.begin(location)) return false;
    int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }
    String xml = http.getString();
    http.end();
    if (xml.length() == 0) return false;

    int fn = xml.indexOf("<friendlyName>");
    if (fn >= 0) {
        int fe = xml.indexOf("</friendlyName>", fn);
        if (fe > fn) {
            name = xml.substring(fn + 14, fe);
            name.trim();
        }
    }
    if (name.length() == 0) name = location;

    int svc = xml.indexOf("urn:schemas-upnp-org:service:AVTransport:1");
    if (svc < 0) return false;
    int cu = xml.indexOf("<controlURL>", svc);
    if (cu < 0) return false;
    int ce = xml.indexOf("</controlURL>", cu);
    if (ce < 0) return false;
    String rel = xml.substring(cu + 12, ce);
    control = absoluteUrl(location, rel);
    return control.length() > 0;
}

// A reachable network is either a joined station link or our own AP (the TV is
// then a client of that AP, so discovery has to run there too).
static bool castNetworkUp() {
    if (WiFi.isConnected()) return true;
    wifi_mode_t mode = WiFi.getMode();
    return (mode & WIFI_MODE_AP) != 0;
}

static IPAddress castLocalIp() {
    if (WiFi.isConnected()) return WiFi.localIP();
    return WiFi.softAPIP();
}

JSValue native_wifiSsdp(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    std::vector<String> lines;
    if (!castNetworkUp()) return cast_js_string_array(ctx, lines);

    int waitMs = cast_js_int_arg(ctx, argv, argc, 0, 2500);
    if (waitMs < 300) waitMs = 300;
    if (waitMs > 8000) waitMs = 8000;

    std::vector<String> locations;
    ssdpCollectLocations((uint32_t)waitMs, locations);

    for (size_t i = 0; i < locations.size(); i++) {
        String name, control;
        if (!ssdpDescribe(locations[i], name, control)) continue;
        String ip = locations[i];
        int scheme = ip.indexOf("://");
        if (scheme >= 0) ip = ip.substring(scheme + 3);
        int slash = ip.indexOf('/');
        if (slash >= 0) ip = ip.substring(0, slash);
        int colon = ip.indexOf(':');
        if (colon >= 0) ip = ip.substring(0, colon);
        lines.push_back(ip + "|" + name + "|" + control);
    }
    return cast_js_string_array(ctx, lines);
}

// ---------------------------------------------------------------------------
// mDNS: find Chromecast receivers (_googlecast._tcp.local)
// ---------------------------------------------------------------------------
static void dnsPutName(std::vector<uint8_t> &out, const char *name) {
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63) break;
        out.push_back((uint8_t)len);
        for (size_t i = 0; i < len; i++) out.push_back((uint8_t)p[i]);
        if (!dot) break;
        p = dot + 1;
    }
    out.push_back(0);
}

// Read a (possibly compressed) DNS name; returns the number of bytes consumed.
static size_t dnsReadName(const uint8_t *data, size_t len, size_t at, String &out) {
    size_t consumed = 0;
    bool jumped = false;
    int guard = 0;
    while (at < len && guard++ < 64) {
        uint8_t code = data[at];
        if (code == 0) {
            if (!jumped) consumed++;
            break;
        }
        if ((code & 0xC0) == 0xC0) {
            if (at + 1 >= len) break;
            if (!jumped) consumed += 2;
            // only follow pointers inside the packet, never twice downwards
            at = (size_t)(((code & 0x3F) << 8) | data[at + 1]);
            jumped = true;
            continue;
        }
        if (at + 1 + code > len) break;
        if (out.length()) out += ".";
        for (uint8_t i = 0; i < code; i++) out += (char)data[at + 1 + i];
        at += 1 + code;
        if (!jumped) consumed += 1 + code;
    }
    return consumed;
}

JSValue native_wifiMdnsCast(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    std::vector<String> lines;
    if (!castNetworkUp()) return cast_js_string_array(ctx, lines);

    int waitMs = cast_js_int_arg(ctx, argv, argc, 0, 2500);
    if (waitMs < 300) waitMs = 300;
    if (waitMs > 8000) waitMs = 8000;

    WiFiUDP udp;
    if (!udp.begin(0)) return cast_js_string_array(ctx, lines);

    std::vector<uint8_t> query = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    dnsPutName(query, "_googlecast._tcp.local");
    query.push_back(0);
    query.push_back(12); // PTR
    query.push_back(0);
    query.push_back(1); // IN

    udp.beginPacket(IPAddress(224, 0, 0, 251), 5353);
    udp.write(query.data(), query.size());
    udp.endPacket();

    struct Hit {
        String ip;
        uint16_t port;
        String name;
    };
    std::vector<Hit> hits;
    uint16_t pendingPort = 8009;
    String pendingName;

    uint32_t deadline = millis() + waitMs;
    while (millis() < deadline) {
        int size = udp.parsePacket();
        if (size <= 0) {
            delay(20);
            continue;
        }
        uint8_t buf[900];
        int n = udp.read(buf, sizeof(buf));
        if (n < 12) continue;
        int questions = (buf[4] << 8) | buf[5];
        int answers = (buf[6] << 8) | buf[7];
        int authority = (buf[8] << 8) | buf[9];
        int additional = (buf[10] << 8) | buf[11];
        size_t at = 12;
        String scratch;
        for (int q = 0; q < questions && at < (size_t)n; q++) {
            at += dnsReadName(buf, n, at, scratch) + 4;
        }
        int total = answers + authority + additional;
        for (int r = 0; r < total && at + 10 < (size_t)n; r++) {
            String owner;
            at += dnsReadName(buf, n, at, owner);
            if (at + 10 > (size_t)n) break;
            uint16_t type = (buf[at] << 8) | buf[at + 1];
            uint16_t rdlen = (buf[at + 8] << 8) | buf[at + 9];
            at += 10;
            if (at + rdlen > (size_t)n) break;
            const uint8_t *rdata = buf + at;
            if (type == 12) { // PTR -> instance name
                String instance;
                dnsReadName(buf, n, at, instance);
                if (instance.length()) pendingName = instance;
            } else if (type == 33 && rdlen >= 6) { // SRV -> port + target
                pendingPort = (uint16_t)((rdata[4] << 8) | rdata[5]);
            } else if (type == 1 && rdlen == 4) { // A -> IPv4
                Hit hit;
                hit.ip = String(rdata[0]) + "." + String(rdata[1]) + "." + String(rdata[2]) + "." + String(rdata[3]);
                hit.port = pendingPort;
                hit.name = pendingName.length() ? pendingName : String("Chromecast");
                hits.push_back(hit);
            }
            at += rdlen;
        }
    }
    udp.stop();

    for (size_t i = 0; i < hits.size(); i++) {
        lines.push_back(hits[i].ip + "|" + String(hits[i].port) + "|" + hits[i].name);
    }
    return cast_js_string_array(ctx, lines);
}

// ---------------------------------------------------------------------------
// The image itself: a one-file HTTP server the TV can fetch from
// ---------------------------------------------------------------------------
static AsyncWebServer *castServer = nullptr;

// The image is read into RAM before the server starts. Streaming it from the
// SD card inside the AsyncTCP task shares the SPI bus with the display and the
// main loop, and a transfer stalls after the first few KB - the TV then gets a
// truncated JPEG and reports "Playback not available". A mirror image is small
// (a few hundred KB at most), so one buffer avoids the contention entirely.
static uint8_t *castFileData = nullptr;
static size_t castFileSize = 0;

static void castFileRelease() {
    if (castFileData != nullptr) free(castFileData);
    castFileData = nullptr;
    castFileSize = 0;
}

static void castServeStopInternal() {
    if (castServer == nullptr) return;
    castServer->end();
    delete castServer;
    castServer = nullptr;
    // castFileData is deliberately kept: a response already in flight still
    // reads from it. It is released when the next file is served.
}

static const char *mimeForPath(const String &path) {
    String ext = path.substring(path.lastIndexOf('.') + 1);
    ext.toLowerCase();
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "bmp") return "image/bmp";
    if (ext == "mp4") return "video/mp4";
    return "application/octet-stream";
}

JSValue native_wifiServeFile(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "serveFile(path:string, port?:int)");
    if (!castNetworkUp()) return JS_NewString(ctx, "");

    FileParamsJS file = js_get_path_from_params(ctx, argv, true);
    if (!file.exist) return JS_NewString(ctx, "");

    int port = cast_js_int_arg(ctx, argv, argc, 1, 8765);
    if (port < 1024 || port > 65535) port = 8765;

    castServeStopInternal();

    FS *fs = file.fs;
    if (fs == nullptr) return JS_NewString(ctx, "");
    String path = file.path;
    String mime = mimeForPath(path);

    const size_t kMaxServeBytes = 4 * 1024 * 1024;
    File src = fs->open(path, FILE_READ);
    if (!src) return JS_NewString(ctx, "");
    size_t size = src.size();
    if (size == 0 || size > kMaxServeBytes) {
        src.close();
        return JS_NewString(ctx, "");
    }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        src.close();
        return JS_NewString(ctx, "");
    }
    size_t got = 0;
    while (got < size) {
        int n = src.read(buf + got, size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    src.close();
    if (got != size) {
        free(buf);
        return JS_NewString(ctx, "");
    }
    castFileRelease();
    castFileData = buf;
    castFileSize = got;

    castServer = new AsyncWebServer((uint16_t)port);
    if (castServer == nullptr) return JS_NewString(ctx, "");

    auto serve = [mime](AsyncWebServerRequest *request) {
        if (castFileData == nullptr || castFileSize == 0) {
            request->send(404, "text/plain", "no image");
            return;
        }
        AsyncWebServerResponse *response =
            request->beginResponse(200, mime, castFileData, castFileSize);
        response->addHeader("Access-Control-Allow-Origin", "*");
        response->addHeader("Cache-Control", "no-store");
        // DLNA renderers (Sony in particular) use these to decide whether the
        // object is playable. Without them the TV accepts the URI but refuses
        // the item with "Playback not available".
        String features = "DLNA.ORG_OP=01;DLNA.ORG_CI=0";
        if (mime == "image/jpeg") features = "DLNA.ORG_PN=JPEG_LRG;" + features;
        response->addHeader("contentFeatures.dlna.org", features);
        response->addHeader("transferMode.dlna.org", "Streaming");
        request->send(response);
    };
    castServer->on("/", HTTP_GET, serve);
    castServer->onNotFound(serve);
    castServer->begin();

    String url = "http://" + castLocalIp().toString() + ":" + String(port) + "/" +
                 path.substring(path.lastIndexOf('/') + 1);
    return JS_NewString(ctx, url.c_str());
}

JSValue native_wifiServeStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    castServeStopInternal();
    return JS_NewBool(true);
}

// ---------------------------------------------------------------------------
// The AP a TV can be put on when its own Wi-Fi password is unknown
// ---------------------------------------------------------------------------
JSValue native_wifiApStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(ctx, argv[0])) return JS_ThrowTypeError(ctx, "apStart(ssid, password?, channel?)");
    String ssid = cast_js_str_arg(ctx, argv, argc, 0);
    String password = cast_js_str_arg(ctx, argv, argc, 1);
    int channel = cast_js_int_arg(ctx, argv, argc, 2, 6);
    if (ssid.length() == 0) return JS_NewString(ctx, "");
    if (password.length() && password.length() < 8) password = ""; // WPA2 needs 8+

    // AP+STA: the station link (if any) stays up, so the image server is
    // reachable from both the TV on this AP and anything on the old network.
    WiFi.mode(WIFI_MODE_APSTA);
    IPAddress ip(192, 168, 4, 1);
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
    bool ok = password.length()
                  ? WiFi.softAP(ssid.c_str(), password.c_str(), (uint8_t)channel, 0, 4)
                  : WiFi.softAP(ssid.c_str(), NULL, (uint8_t)channel, 0, 4);
    if (!ok) return JS_NewString(ctx, "");
    return JS_NewString(ctx, WiFi.softAPIP().toString().c_str());
}

JSValue native_wifiApStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    WiFi.softAPdisconnect(true);
    if (!WiFi.isConnected()) WiFi.mode(WIFI_MODE_STA);
    return JS_NewBool(true);
}

// ---------------------------------------------------------------------------
// CASTV2: the protocol behind "Cast" on a Chromecast / Google TV / Android TV
// ---------------------------------------------------------------------------
// TLS on port 8009, then length-prefixed protobuf messages carrying JSON.
// Only the fields this sender needs are encoded/decoded by hand; pulling in a
// protobuf runtime for six fields would cost more flash than this whole module.
static void pbVarint(std::vector<uint8_t> &out, uint32_t value) {
    while (value >= 0x80) {
        out.push_back((uint8_t)(value | 0x80));
        value >>= 7;
    }
    out.push_back((uint8_t)value);
}

static void pbFieldString(std::vector<uint8_t> &out, uint8_t field, const String &value) {
    pbVarint(out, ((uint32_t)field << 3) | 2);
    pbVarint(out, (uint32_t)value.length());
    for (size_t i = 0; i < value.length(); i++) out.push_back((uint8_t)value.charAt(i));
}

static void pbFieldVarint(std::vector<uint8_t> &out, uint8_t field, uint32_t value) {
    pbVarint(out, ((uint32_t)field << 3) | 0);
    pbVarint(out, value);
}

static bool castSend(WiFiClientSecure &client, const String &destination, const String &ns, const String &json) {
    std::vector<uint8_t> msg;
    pbFieldVarint(msg, 1, 0);                    // protocolVersion = CASTV2_1_0
    pbFieldString(msg, 2, "sender-0");           // sourceId
    pbFieldString(msg, 3, destination);          // destinationId
    pbFieldString(msg, 4, ns);                   // namespace
    pbFieldVarint(msg, 5, 0);                    // payloadType = STRING
    pbFieldString(msg, 6, json);                 // payloadUtf8

    uint8_t header[4] = {
        (uint8_t)((msg.size() >> 24) & 0xFF),
        (uint8_t)((msg.size() >> 16) & 0xFF),
        (uint8_t)((msg.size() >> 8) & 0xFF),
        (uint8_t)(msg.size() & 0xFF),
    };
    if (client.write(header, 4) != 4) return false;
    if (client.write(msg.data(), msg.size()) != msg.size()) return false;
    client.flush();
    return true;
}

// Read one varint out of a protobuf buffer.
static bool pbReadVarint(const uint8_t *data, size_t len, size_t &at, uint32_t &value) {
    value = 0;
    int shift = 0;
    while (at < len && shift < 32) {
        uint8_t byte = data[at++];
        value |= (uint32_t)(byte & 0x7F) << shift;
        shift += 7;
        if (!(byte & 0x80)) return true;
    }
    return false;
}

// Field 6 (payloadUtf8) of a CastMessage, when it carries a string payload.
static String pbPayload(const uint8_t *data, size_t len) {
    size_t at = 0;
    String payload;
    uint32_t payloadType = 1;
    while (at < len) {
        uint32_t key = 0;
        if (!pbReadVarint(data, len, at, key)) break;
        uint8_t field = (uint8_t)(key >> 3);
        uint8_t wire = (uint8_t)(key & 7);
        if (wire == 0) {
            uint32_t value = 0;
            if (!pbReadVarint(data, len, at, value)) break;
            if (field == 5) payloadType = value;
        } else if (wire == 2) {
            uint32_t size = 0;
            if (!pbReadVarint(data, len, at, size)) break;
            if (at + size > len) break;
            if (field == 6) {
                payload.reserve(size);
                for (uint32_t i = 0; i < size; i++) payload += (char)data[at + i];
            }
            at += size;
        } else if (wire == 5) {
            at += 4;
        } else if (wire == 1) {
            at += 8;
        } else {
            break;
        }
    }
    return payloadType == 0 ? payload : String("");
}

static String castReadMessage(WiFiClientSecure &client, uint32_t timeoutMs) {
    uint32_t deadline = millis() + timeoutMs;
    uint8_t header[4];
    size_t got = 0;
    while (got < 4 && millis() < deadline) {
        if (!client.available()) {
            delay(10);
            continue;
        }
        int n = client.read(header + got, 4 - got);
        if (n > 0) got += (size_t)n;
    }
    if (got < 4) return "";
    uint32_t length = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) | header[3];
    if (length == 0 || length > 16384) return "";
    uint8_t *buffer = (uint8_t *)malloc(length);
    if (buffer == nullptr) return "";
    got = 0;
    while (got < length && millis() < deadline) {
        if (!client.available()) {
            delay(10);
            continue;
        }
        int n = client.read(buffer + got, length - got);
        if (n > 0) got += (size_t)n;
    }
    String payload;
    if (got == length) payload = pbPayload(buffer, length);
    free(buffer);
    return payload;
}

static String castPushImage(const String &ip, const String &url, const String &contentType) {
    WiFiClientSecure client;
    // A Chromecast presents a Google-signed certificate for a device on the LAN;
    // pinning that chain would need the CA bundle in flash for no security gain.
    client.setInsecure();
    client.setTimeout(6);
    if (!client.connect(ip.c_str(), 8009)) return "connect failed";

    castSend(client, "receiver-0", "urn:x-cast:com.google.cast.tp.connection", "{\"type\":\"CONNECT\"}");
    castSend(
        client,
        "receiver-0",
        "urn:x-cast:com.google.cast.receiver",
        "{\"type\":\"LAUNCH\",\"appId\":\"CC1AD845\",\"requestId\":1}"
    );

    String transport;
    uint32_t deadline = millis() + 9000;
    while (transport.length() == 0 && millis() < deadline) {
        String json = castReadMessage(client, 2000);
        if (json.length() == 0) continue;
        if (json.indexOf("LAUNCH_ERROR") >= 0) {
            client.stop();
            return "launch refused";
        }
        int at = json.indexOf("\"transportId\":\"");
        if (at >= 0) {
            int end = json.indexOf('"', at + 15);
            if (end > at) transport = json.substring(at + 15, end);
        }
    }
    if (transport.length() == 0) {
        client.stop();
        return "no transport (receiver busy?)";
    }

    castSend(client, transport, "urn:x-cast:com.google.cast.tp.connection", "{\"type\":\"CONNECT\"}");
    String load = String("{\"type\":\"LOAD\",\"requestId\":2,\"media\":{\"contentId\":\"") + url + "\",\"contentType\":\"" +
                  contentType + "\",\"streamType\":\"BUFFERED\",\"metadata\":{\"type\":0,\"metadataType\":0}}}";
    if (!castSend(client, transport, "urn:x-cast:com.google.cast.media", load)) {
        client.stop();
        return "load not sent";
    }

    deadline = millis() + 9000;
    while (millis() < deadline) {
        String json = castReadMessage(client, 2000);
        if (json.length() == 0) continue;
        if (json.indexOf("LOAD_FAILED") >= 0) {
            client.stop();
            return "load failed";
        }
        if (json.indexOf("MEDIA_STATUS") >= 0 && json.indexOf("\"playerState\"") >= 0) {
            client.stop();
            return "ok";
        }
    }
    client.stop();
    return "sent";
}

JSValue native_wifiCastImage(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsString(ctx, argv[0]) || !JS_IsString(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "castImage(ip:string, url:string, contentType?:string)");
    if (!castNetworkUp()) return JS_NewString(ctx, "no wifi");

    String ip = cast_js_str_arg(ctx, argv, argc, 0);
    String url = cast_js_str_arg(ctx, argv, argc, 1);
    String type = cast_js_str_arg(ctx, argv, argc, 2);
    if (type.length() == 0) type = "image/jpeg";
    if (ip.length() == 0 || url.length() == 0) return JS_NewString(ctx, "bad arguments");

    String result = castPushImage(ip, url, type);
    return JS_NewString(ctx, result.c_str());
}

#endif
