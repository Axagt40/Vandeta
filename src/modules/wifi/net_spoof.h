#ifndef __NET_SPOOF_H__
#define __NET_SPOOF_H__

#if !defined(LITE_VERSION)

/**
 * @file net_spoof.h
 * @brief Headless MITM engine for the Bruce JS interpreter.
 *
 * Backs the arpSpoof / dnsSpoof / httpInterceptor JavaScript bindings.
 *
 * Why this works on WPA2 (and why the raw-802.11 injectors do not):
 * every frame here is handed to the driver with esp_wifi_internal_tx() on the
 * *associated station* interface. The station link is already keyed, so the
 * hardware encrypts the frame with the pairwise key before it leaves the radio.
 * A frame built by wifi_raw_inject() is transmitted without a key and is
 * therefore dropped by WPA2 clients. That difference is the whole reason this
 * module talks through the netif instead of through esp_wifi_80211_tx().
 *
 * What it can and cannot do:
 *   - ARP spoofing puts this board between a target and its gateway in both
 *     directions (the classic two-way poison), so the target's traffic arrives
 *     at this board's MAC.
 *   - Because the frame arrives here, a layer-2 hook can read the target's DNS
 *     queries and HTTP requests and answer them from this board.
 *   - There is NO forwarding: the ESP32 cannot NAT/route, so a spoofed target
 *     loses its real internet path for as long as the poison is active. This is
 *     an interception MITM, not a transparent one. netspoof_arp_stop()
 *     (or dropping the interpreter) restores the real ARP mappings.
 */

#include <Arduino.h>
#include <IPAddress.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

#define NETSPOOF_MAX_TARGETS 32      // hosts remembered by a discovery scan
#define NETSPOOF_MAX_PAIRS 8         // simultaneous ARP spoof pairs
#define NETSPOOF_MAX_RULES 8         // DNS rules and HTTP rules each
#define NETSPOOF_MAX_CAPTURES 8      // ring buffer served by getInterceptedData()
#define NETSPOOF_CAPTURE_PAYLOAD 320 // bytes of the request kept per capture
#define NETSPOOF_QUEUE_DEPTH 6       // L2 hook -> service task queue
#define NETSPOOF_HOOK_COPY 640       // bytes copied out of a frame by the hook
#define NETSPOOF_FRAME_MAX 1514      // ethernet frame ceiling
#define NETSPOOF_POISON_INTERVAL_MS 250 // ARP re-poison period
#define NETSPOOF_SVC_TICK_MS 10      // service task tick

// ---------------------------------------------------------------------------
// Public data shapes
// ---------------------------------------------------------------------------

/// One host found by the ARP discovery scan.
struct NetSpoofTarget {
    IPAddress ip;
    uint8_t mac[6];
    String macStr;   // "AA:BB:CC:DD:EE:FF"
    bool gateway;    // this entry IS the default gateway
    bool spoofed;    // an active ARP pair covers this IP
    uint8_t pairedWith[6]; // gateway MAC when spoofed
};

/// One DNS or HTTP rule.
struct NetSpoofRule {
    String match;   // domain / URL pattern, lower-cased
    String value;   // answer IP, redirect URL or HTML body
    bool wildcard;  // true = matches everything (startAll('*', ...))
    uint32_t hits;
};

/// One captured (and possibly modified) request.
struct NetSpoofCapture {
    uint32_t ts;      // millis() when it was seen
    uint8_t kind;     // 0 = DNS query, 1 = HTTP request
    uint8_t action;   // 0 = observed only, 1 = answered/redirected, 2 = injected
    IPAddress clientIp;
    char clientMac[18];
    char method[8];   // "GET", "POST", ... (HTTP) or "DNS"
    char host[48];    // Host header, or the DNS question name
    char path[96];    // request path / query string
    char data[NETSPOOF_CAPTURE_PAYLOAD]; // request snippet or decoded query
};

// ---------------------------------------------------------------------------
// Preconditions
// ---------------------------------------------------------------------------

/**
 * @brief Check that the station is associated (all of this module needs it).
 * @param err filled with a human-readable reason when false is returned
 */
bool netspoof_wifi_ready(String &err);

/// True when the L2 hook is installed on the station netif.
bool netspoof_hook_installed();

// ---------------------------------------------------------------------------
// ARP spoofing
// ---------------------------------------------------------------------------

/**
 * @brief ARP-scan the local subnet and fill the target list.
 *
 * Sends one ARP request per host address and reads the LwIP ARP table, the same
 * method Bruce's ARPScanner / NetCut use. Blocks for the scan duration
 * (bounded by @p timeoutMs).
 *
 * @return number of hosts found (0 is a valid answer, not necessarily an error)
 */
int netspoof_scan_targets(uint32_t timeoutMs, String &err);

int netspoof_target_count();
const NetSpoofTarget *netspoof_target(int idx);
void netspoof_clear_targets();

/// Current default gateway and its MAC (BSSID fallback when the ARP table is empty).
bool netspoof_gateway_info(IPAddress &gw, uint8_t mac[6]);

/// Resolve one IPv4 address to a MAC with an ARP request + table poll.
bool netspoof_resolve_mac(const IPAddress &ip, uint8_t mac[6], uint32_t timeoutMs);

/// This board's station MAC.
void netspoof_my_mac(uint8_t mac[6]);

/**
 * @brief Start (or add) a two-way ARP spoof between @p target and @p gateway.
 *
 * Poisons both ends so target->gateway and gateway->target traffic comes here.
 * The pairing is re-poisoned every NETSPOOF_POISON_INTERVAL_MS by the service
 * task until netspoof_arp_stop(), so it survives while the script does other
 * work. Returns false with @p err set when the station is down, the addresses
 * do not resolve or the pair table is full.
 */
bool netspoof_arp_start(const IPAddress &target, const IPAddress &gateway, String &err);

/**
 * @brief Restore every real ARP mapping and stop poisoning.
 * Sends the correct gateway MAC to each target and the correct target MAC to
 * the gateway (reply + broadcast request, RFC 826) before dropping the pairs.
 */
bool netspoof_arp_stop(String &err);

/// Restore without touching the error string (used on interpreter exit).
void netspoof_arp_restore_all();

bool netspoof_arp_active();
int netspoof_arp_pair_count();
uint32_t netspoof_arp_frames();
uint32_t netspoof_arp_uptime_ms();
bool netspoof_arp_pair_info(
    int idx, IPAddress &target, IPAddress &gateway, char *targetMac, char *gatewayMac
);
/// True when @p ip is covered by an active pair.
bool netspoof_arp_pair_index(const IPAddress &ip, int *idx);

// ---------------------------------------------------------------------------
// DNS spoofing (needs an active ARP pair to see the queries)
// ---------------------------------------------------------------------------

bool netspoof_dns_start(String &err);
bool netspoof_dns_stop(String &err);
bool netspoof_dns_active();
uint32_t netspoof_dns_queries();
uint32_t netspoof_dns_answers();

/**
 * @brief Add a DNS rule.
 * @param domain name to match, or "*" for every query
 * @param ip dotted quad returned for a match
 * @param wildcard true = match every query
 */
bool netspoof_dns_add_rule(const String &domain, const String &ip, bool wildcard, String &err);
void netspoof_dns_clear_rules();
int netspoof_dns_rule_count();
const NetSpoofRule *netspoof_dns_rule(int idx);
/// The IP a query name resolves to, or false when no rule matches.
bool netspoof_dns_lookup(const String &name, IPAddress &ip);

// ---------------------------------------------------------------------------
// HTTP interception (needs an active ARP pair to see the requests)
// ---------------------------------------------------------------------------

bool netspoof_http_start(String &err);
bool netspoof_http_stop(String &err);
bool netspoof_http_active();
uint32_t netspoof_http_requests();
uint32_t netspoof_http_redirected();
uint32_t netspoof_http_injected();

bool netspoof_http_add_redirect(const String &pattern, const String &url, bool wildcard, String &err);
bool netspoof_http_add_injection(const String &pattern, const String &html, bool wildcard, String &err);
void netspoof_http_clear_rules();
int netspoof_http_redirect_count();
int netspoof_http_inject_count();
const NetSpoofRule *netspoof_http_redirect_rule(int idx);
const NetSpoofRule *netspoof_http_inject_rule(int idx);

// ---------------------------------------------------------------------------
// Captured data
// ---------------------------------------------------------------------------

int netspoof_capture_count();
/// @param idx 0 = newest capture
const NetSpoofCapture *netspoof_capture(int idx);
void netspoof_capture_clear();

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/// Stop interception, restore ARP and release the service task/queue.
void netspoof_cleanup();

#endif // !LITE_VERSION
#endif // __NET_SPOOF_H__
