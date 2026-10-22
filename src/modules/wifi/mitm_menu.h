#ifndef __MITM_MENU_H__
#define __MITM_MENU_H__

#include <Arduino.h>

#if !defined(LITE_VERSION)

/**
 * @file mitm_menu.h
 * @brief On-device UI and payload files for the net_spoof MITM engine.
 *
 * The engine itself (net_spoof.cpp) was written for the JavaScript bindings and
 * is unchanged: arpSpoof / dnsSpoof / httpInterceptor still drive it. This file
 * adds the two missing pieces so the same attacks can be used without a script:
 *
 *   - the `DNS Inject` and `HTML Intercept` entries of WiFi > Wifi Atks, built
 *     on the engine's public API only;
 *   - the payload files those menus load from the card.
 *
 * Payload files (on the mounted filesystem, SD first):
 *
 *   /BruceSpoof/*.mitm          ONE payload for the whole attack (see below)
 *   /BruceSpoof/dns.txt         one rule per line:  <domain> <ip>
 *                               `* <ip>` answers every query, `#` comments.
 *   /BruceSpoof/redirect.txt    one rule per line:  <pattern> <url>
 *   /BruceSpoof/html/*.html     an injected page (must fit one ethernet frame)
 *
 * The unified payload (MITM Attack page, `.mitm`, `.txt` or `.html`) is the
 * keypad-free way to describe a whole run. It is a plain-text list of commands,
 * one per line, `#` for comments - a thin, readable front end for the same
 * engine the JavaScript API exposes:
 *
 *   page <match> <file>         httpInterceptor.addInjection()   200 + HTML
 *   redirect <match> <url>      httpInterceptor.addRedirect()    302
 *   dns <name|*> <ip>           dnsSpoof.addRule() / startAll()
 *                               (<ip> may be `gateway` or `this`; the token is
 *                               resolved on each query, so the payload loads
 *                               even while the station is still down)
 *   target all|<ip>             arpSpoof.start() - overrides the page's mode
 *   fallback dns <ip> [secs]    if nothing was injected/redirected in <secs>
 *   fallback redirect <url> [secs]   ... try this instead (default 10 s)
 *
 * A `.html` payload is simply `page * <that file>`.
 *
 * Nothing here sets up WiFi on its own beyond asking the station to associate:
 * the engine needs a real association because the frames go out on the keyed
 * station link (see the note at the top of net_spoof.h).
 */

#include "net_spoof.h"

/// Directory holding every MITM payload.
#define MITM_PAYLOAD_DIR "/BruceSpoof"
#define MITM_DNS_PAYLOAD MITM_PAYLOAD_DIR "/dns.txt"
#define MITM_HTML_DIR MITM_PAYLOAD_DIR "/html"
#define MITM_REDIRECT_PAYLOAD MITM_PAYLOAD_DIR "/redirect.txt"
#define MITM_SCRIPT_PAYLOAD MITM_PAYLOAD_DIR "/mitm.txt"

/// How much of a DNS / redirect payload is read (rules are 8 x ~96 bytes).
#define MITM_DNS_MAX_BYTES 1024
#define MITM_REDIRECT_MAX_BYTES 2048
/// A unified payload is a few command lines; 4 KB is ~100 of them.
#define MITM_SCRIPT_MAX_BYTES 4096
/// How long a `fallback` line waits for an HTTP hit when it names no time.
#define MITM_FALLBACK_DEFAULT_MS 10000
/// An injected page shares one ethernet frame with the IP/TCP/HTTP headers.
#define MITM_HTML_MAX 1100
/// Shown on screen when a payload file cannot be opened.
#define MITM_PAYLOAD_HINT "Create it from the menu (Create example)."

/// Outcome of loading one payload file.
struct MitmPayloadResult {
    bool ok = false;
    int loaded = 0;    // rules accepted by the engine
    int bad = 0;       // lines rejected (syntax, bad IP, rule table full)
    int skipped = 0;   // blank lines and comments
    bool truncated = false; // file was longer than the size cap
    String path;
    String error;      // set when ok is false
};

// ---------------------------------------------------------------------------
// Payload files
// ---------------------------------------------------------------------------

/**
 * @brief Parse /BruceSpoof/dns.txt style payloads into DNS rules.
 *
 * Replaces the current rule set unless @p clearFirst is false. A malformed line
 * is counted and skipped; it never aborts the load. `*` as the domain makes the
 * rule a wildcard, so it should be the last line of the file.
 */
MitmPayloadResult mitmPayloadLoadDns(const String &path = MITM_DNS_PAYLOAD, bool clearFirst = true);

/// Parse /BruceSpoof/redirect.txt style payloads into HTTP 302 rules.
MitmPayloadResult mitmPayloadLoadRedirects(
    const String &path = MITM_REDIRECT_PAYLOAD, bool clearFirst = true
);

/**
 * @brief Read one HTML file and install it as the injection rule.
 *
 * @param match host/substring to match, `*` (or an empty string) for every
 *              plain-HTTP request
 */
MitmPayloadResult mitmPayloadLoadHtml(const String &path, const String &match);

/// Read at most @p maxBytes of a text payload; the result carries the reason.
String mitmPayloadRead(const String &path, size_t maxBytes, MitmPayloadResult &res);

// ---------------------------------------------------------------------------
// Unified payload (the MITM Attack page)
// ---------------------------------------------------------------------------

/// Outcome of loading one `.mitm` / `.txt` / `.html` payload.
struct MitmScriptResult {
    bool ok = false;
    int pages = 0;      // page rules armed (page)
    int redirects = 0;  // redirect rules armed (redirect)
    int dns = 0;        // DNS rules armed (dns)
    int bad = 0;        // lines rejected
    int skipped = 0;    // comments and blank lines
    bool truncated = false;
    bool fallbackSet = false;
    bool fallbackDns = false;        // false = redirect fallback
    String fallbackValue;            // IP (dns) or URL (redirect)
    uint32_t fallbackAfterMs = MITM_FALLBACK_DEFAULT_MS;
    String target;                   // "all", an IPv4 address, or empty
    String path;
    String error;
};

/**
 * @brief Load a unified payload into the engine.
 *
 * Clears every DNS/HTTP rule first, then arms what the file describes, so one
 * payload is one attack (loading a second payload replaces the first). A path
 * ending in `.html` is treated as `page * <path>`.
 */
MitmScriptResult mitmPayloadLoadScript(const String &path);

/**
 * @brief Write the example payload for @p path.
 *
 * @param overwrite false keeps an existing file (the menu never clobbers rules)
 */
bool mitmPayloadWriteExample(const String &path, bool overwrite = false, String *error = nullptr);

/// Write dns.txt, redirect.txt and one HTML page, creating /BruceSpoof first.
int mitmPayloadWriteAllExamples(bool overwrite = false);

/// Create /BruceSpoof and /BruceSpoof/html when they are missing.
bool mitmEnsurePayloadDirs();

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

/// WiFi > Wifi Atks > MITM Attack: network, targets, one payload, one Start.
void mitmAttackMenu();

// ---------------------------------------------------------------------------
// Headless access (the `mitm` console verb, and how the page is tested)
// ---------------------------------------------------------------------------

/// Set the target mode: "all" or one IPv4 address. False when unparsable.
bool mitmAttackSetTarget(const String &spec);

/// Payload path currently selected on the page (empty when none).
String mitmAttackPayloadPath();

/// Select the payload the page (and a later `mitm run` with no path) uses.
void mitmAttackSetPayload(const String &path);

/// "All devices", the single address, or "Single (none picked)".
String mitmAttackTargetLabel();

/**
 * @brief Arm @p path, run it for @p ms, stop and restore ARP.
 *
 * The same code path Start uses on the page, without the screen or the key
 * watch. @p summary gets the counters of the run, @p error the reason it could
 * not start.
 */
bool mitmAttackRunHeadless(const String &path, uint32_t ms, String &summary, String &error);

/**
 * @brief True when the station is associated, i.e. when the engine can run.
 *
 * Deliberately a check and nothing more: these pages open while the station is
 * down (so they can be inspected and their payloads edited), and the rows that
 * need the link report it themselves.
 */
bool mitmEnsureStation();

#endif // !LITE_VERSION

#endif // __MITM_MENU_H__
