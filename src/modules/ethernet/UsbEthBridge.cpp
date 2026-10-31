/**
 * @file UsbEthBridge.cpp
 * @brief Ethernet over the USB-C port, with the PC cable acting as the PHY.
 *
 * The T-Embed CC1101 cannot talk to a USB-C hub's RJ45 port: the ESP32-S3 has
 * no Ethernet MAC (no RMII pins), the USB-C connector is wired to the chip's
 * USB *device* peripheral, and no ESP32 driver exists for the RTL8153/AX88179
 * bridges those hubs use. So the adapter stays on the PC, and this module makes
 * the board behave as if a cable were plugged into it: raw Ethernet frames go
 * over USB to a host helper that owns the real NIC.
 *
 * Keeping it at layer 2 (instead of tunnelling IP) is the point. ARP, DHCP and
 * the IP checksums stay in lwIP on the board, the frames leave the PC's NIC
 * with the board's own MAC, and the existing Ethernet tools - ARP scan, DHCP
 * starvation, MAC flooding - work against the real network unchanged.
 *
 * Wire format, both directions, no checksum (USB bulk transfers are already
 * protected, so the extra bytes would only cost throughput):
 *
 *      A5 5A 5A A5 <type:1> <len:2 little endian> <payload:len>
 *
 *   type 0x01  Ethernet frame. Board -> host: a frame to put on the wire.
 *              Host -> board: a frame received from the wire.
 *   type 0x02  Host -> board, 1 byte: does the PC's NIC have carrier.
 *   type 0x03  Host -> board: the helper is leaving, close the page.
 *
 * The host side is FREEBUFF/tools/eth_bridge.py.
 */
#if !defined(LITE_VERSION)
#include "UsbEthBridge.h"

#include <Arduino.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp32-hal-psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "core/serialcmds.h"
#include <globals.h> // EscPress
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include <stdlib.h> // atoi

namespace UsbEthBridge {
namespace {

// Four magic bytes, not two. A 2-byte magic turns up inside ordinary Ethernet
// payloads about once every 65536 byte positions, and when it does the parser
// locks onto a bogus length and eats every frame behind it (observed on the
// bench: one DHCP DISCOVER, then silence for the rest of the run).
constexpr uint8_t kMagic0 = 0xA5;
constexpr uint8_t kMagic1 = 0x5A;
constexpr uint8_t kMagic2 = 0x5A;
constexpr uint8_t kMagic3 = 0xA5;
constexpr uint8_t kTypeFrame = 0x01;
constexpr uint8_t kTypeLink = 0x02;
constexpr uint8_t kTypeStop = 0x03;
constexpr uint8_t kTypeKey = 0x04;

// Key codes carried by a kTypeKey frame (one byte payload).
constexpr uint8_t kKeyBack = 0;
constexpr uint8_t kKeyOk = 1;
constexpr uint8_t kKeyUp = 2;
constexpr uint8_t kKeyDown = 3;
constexpr uint8_t kKeyPrev = 4;
constexpr uint8_t kKeyNext = 5;

constexpr size_t kFrameMax = 1600; // 1500 MTU + 14 header, with slack
constexpr int kTxSlots = 3;        // frames in flight towards the host
constexpr size_t kRxRing = 4096;   // USBCDC RX queue holds one byte per item
constexpr size_t kChunk = 512;

// Parser states.
enum : uint8_t { ST_MAGIC0, ST_MAGIC1, ST_MAGIC2, ST_MAGIC3, ST_TYPE, ST_LEN_LO, ST_LEN_HI, ST_PAYLOAD };

struct Slot {
    uint16_t len;
    uint8_t data[kFrameMax];
};

Slot *s_slots = nullptr;
QueueHandle_t s_freeSlots = nullptr;
QueueHandle_t s_readySlots = nullptr;

uint8_t s_rxBuf[kFrameMax];
uint8_t s_chunk[kChunk];
size_t s_rxWant = 0;  // length announced by the header
size_t s_rxFill = 0;  // bytes stored so far
uint8_t s_state = ST_MAGIC0;
uint8_t s_type = 0;

esp_netif_t *s_netif = nullptr;
struct netif *s_lwipNetif = nullptr;
esp_netif_driver_base_t s_driverBase = {nullptr, nullptr};
TaskHandle_t s_rxTask = nullptr;
TaskHandle_t s_txTask = nullptr;

volatile bool s_running = false;
volatile bool s_hostLink = false;
volatile bool s_hostSeen = false;
volatile bool s_stopRequested = false;
volatile uint32_t s_rxCount = 0;
volatile uint32_t s_txCount = 0;
volatile uint32_t s_dropCount = 0;
volatile uint32_t s_badCount = 0;
size_t s_prevRxRing = 0;

/// Locally administered, unicast, derived from the chip's station MAC so the
/// DHCP lease is stable across sessions.
void makeMac(uint8_t mac[6]) {
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        mac[0] = 0x02;
        mac[1] = 0x00;
        mac[2] = 0x00;
        mac[3] = 0x57;
        mac[4] = 0x42;
        mac[5] = 0x01;
    }
    mac[0] = (uint8_t)((mac[0] | 0x02) & 0xFE);
}

esp_err_t transmitToLink(void *handler, void *buffer, size_t len) {
    (void)handler;
    if (!s_running || buffer == nullptr || len == 0 || len > kFrameMax) {
        s_dropCount++;
        return ESP_FAIL;
    }
    Slot *slot = nullptr;
    if (xQueueReceive(s_freeSlots, &slot, 0) != pdTRUE || slot == nullptr) {
        s_dropCount++;
        return ESP_FAIL;
    }
    memcpy(slot->data, buffer, len);
    slot->len = (uint16_t)len;
    if (xQueueSend(s_readySlots, &slot, 0) != pdTRUE) {
        xQueueSend(s_freeSlots, &slot, 0);
        s_dropCount++;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t postAttach(esp_netif_t *netif, esp_netif_iodriver_handle handler) {
    esp_netif_driver_base_t *base = (esp_netif_driver_base_t *)handler;
    if (base == nullptr) { return ESP_ERR_INVALID_ARG; }
    base->netif = netif;
    esp_netif_driver_ifconfig_t cfg = {};
    cfg.handle = base;
    cfg.transmit = transmitToLink;
    // No zero-copy and no custom RX buffer: esp_netif_receive() then copies,
    // which lets the parser reuse one static buffer.
    cfg.transmit_wrap = nullptr;
    cfg.driver_free_rx_buffer = nullptr;
    return esp_netif_set_driver_config(netif, &cfg);
}

/**
 * Hand a frame to lwIP directly.
 *
 * NOT esp_netif_receive(): that path wraps the buffer in an "esp_pbuf" whose
 * custom free callback goes through the driver's driver_free_rx_buffer, and
 * with no such callback installed the first free lands on a NULL function
 * pointer - "Guru Meditation ... InstrFetchProhibited, PC: 0x00000000" inside
 * esp_pbuf_free, triggered by the first DHCP reply (bench, 2026-10-30).
 * pbuf_alloc + tcpip_input is the plain lwIP ingress: it copies the frame and
 * nothing else owns it, so nothing can be freed twice or through a NULL hook.
 */
void resolveLwipNetif() {
    if (s_lwipNetif != nullptr || s_netif == nullptr) { return; }
    // This ESP-IDF only publishes the *name* of the underlying lwIP netif
    // (esp_netif_get_netif_impl is private), so the handle is looked up by it.
    //
    // NOT netif_find(): that calls LWIP_ASSERT_CORE_LOCKED(), and this runs from
    // the menu task, so the assert aborts the chip (bench, 2026-10-30:
    // "assert failed: netif_find ... netif.c:1768", right after the interface
    // came up). Walking netif_list needs no lock - it is what MACFlooding and
    // DHCPStarvation already do.
    char impl_name[8] = {0};
    if (esp_netif_get_netif_impl_name(s_netif, impl_name) != ESP_OK) { return; }
    const uint8_t num = (uint8_t)atoi(impl_name + 2);
    for (struct netif *n = netif_list; n != nullptr; n = n->next) {
        if (n->name[0] == impl_name[0] && n->name[1] == impl_name[1] && n->num == num) {
            s_lwipNetif = n;
            return;
        }
    }
}

void deliverToLwip(const uint8_t *frame, size_t len) {
    if (s_netif == nullptr) { return; }
    resolveLwipNetif();
    struct netif *n = s_lwipNetif;
    if (n == nullptr) {
        s_badCount++;
        return;
    }
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p == nullptr) {
        s_badCount++;
        return;
    }
    if (pbuf_take(p, frame, len) != ERR_OK || tcpip_input(p, n) != ERR_OK) {
        pbuf_free(p);
        s_badCount++;
        return;
    }
    s_rxCount++;
}

/// Remote keypad. The text console is suspended while the bridge owns the port,
/// so without this the page's submenu and any running tool inside it can only be
/// driven by the physical buttons. This does exactly what the debug console's
/// `press` command does: set the flag, then AnyKeyPress/SerialCmdPress so the
/// input loop takes it instead of waiting for hardware.
void injectKeyFrame(uint8_t key) {
    volatile bool *flag = nullptr;
    switch (key) {
        case kKeyBack: flag = &EscPress; break;
        case kKeyOk: flag = &SelPress; break;
        case kKeyUp: flag = &UpPress; break;
        case kKeyDown: flag = &DownPress; break;
        case kKeyPrev: flag = &PrevPress; break;
        case kKeyNext: flag = &NextPress; break;
        default: return;
    }
    *flag = true;
    AnyKeyPress = true;
    SerialCmdPress = true;
}

void completeFrame() {
    // Any frame at all proves the helper is alive on the other end of the cable.
    s_hostSeen = true;
    if (s_type == kTypeFrame) {
        if (s_rxFill > 0) { deliverToLwip(s_rxBuf, s_rxFill); }
    } else if (s_type == kTypeLink) {
        s_hostLink = (s_rxFill >= 1) && (s_rxBuf[0] != 0);
    } else if (s_type == kTypeKey) {
        if (s_rxFill >= 1) { injectKeyFrame(s_rxBuf[0]); }
    } else if (s_type == kTypeStop) {
        // The helper is stopping. Deliver BACK the way the serial `nav esc`
        // command does - EscPress alone is not enough, because a press is only
        // taken while AnyKeyPress/SerialCmdPress are clear - so an open menu
        // closes itself. The console is suspended while bridging, which would
        // otherwise leave the physical BACK button as the only way out.
        s_stopRequested = true;
        EscPress = true;
        AnyKeyPress = true;
        SerialCmdPress = true;
    }
    s_rxWant = 0;
    s_rxFill = 0;
}

void feedByte(uint8_t byte) {
    // One byte can be both a failed candidate and the start of the real magic,
    // so a mismatch in the magic states re-examines the same byte instead of
    // dropping it. Without that, junk like "... A5 5A" immediately before a
    // frame eats the frame's first magic byte and the whole frame is lost
    // (caught by the host-side parser test, 2026-10-30).
    for (;;) {
        switch (s_state) {
            case ST_MAGIC0:
                if (byte == kMagic0) { s_state = ST_MAGIC1; }
                return;
            case ST_MAGIC1:
                if (byte == kMagic1) {
                    s_state = ST_MAGIC2;
                    return;
                }
                s_state = ST_MAGIC0;
                break; // re-examine
            case ST_MAGIC2:
                if (byte == kMagic2) {
                    s_state = ST_MAGIC3;
                    return;
                }
                s_state = ST_MAGIC0;
                break; // re-examine
            case ST_MAGIC3:
                if (byte == kMagic3) {
                    s_state = ST_TYPE;
                    return;
                }
                s_state = ST_MAGIC0;
                break; // re-examine
            case ST_TYPE:
                s_type = byte;
                s_state = ST_LEN_LO;
                return;
            case ST_LEN_LO:
                s_rxWant = byte;
                s_state = ST_LEN_HI;
                return;
            case ST_LEN_HI:
                s_rxWant |= ((size_t)byte) << 8;
                // Per-type length sanity: a garbage header is rejected here
                // instead of being allowed to swallow the frames behind it.
                {
                    bool ok = (s_type == kTypeFrame)   ? (s_rxWant >= 14 && s_rxWant <= kFrameMax)
                              : (s_type == kTypeLink)  ? (s_rxWant == 1)
                              : (s_type == kTypeStop)  ? (s_rxWant == 0)
                              : (s_type == kTypeKey)   ? (s_rxWant == 1)
                                                       : false;
                    if (!ok) {
                        s_rxWant = 0;
                        s_badCount++;
                        s_state = ST_MAGIC0;
                        return;
                    }
                }
                if (s_rxWant == 0) {
                    // Only the stop frame is zero-length, and it still has to
                    // run through the dispatcher.
                    completeFrame();
                    s_state = ST_MAGIC0;
                } else {
                    s_rxFill = 0;
                    s_state = ST_PAYLOAD;
                }
                return;
            case ST_PAYLOAD:
                s_rxBuf[s_rxFill++] = byte;
                if (s_rxFill >= s_rxWant) {
                    completeFrame();
                    s_state = ST_MAGIC0;
                }
                return;
            default:
                s_state = ST_MAGIC0;
                return;
        }
    }
}

void rxTask(void *arg) {
    (void)arg;
    const TickType_t idle = pdMS_TO_TICKS(1);
    while (s_running) {
        int avail = Serial.available();
        if (avail <= 0) {
            vTaskDelay(idle);
            continue;
        }
        size_t want = (size_t)avail;
        if (want > kChunk) { want = kChunk; }
        int got = Serial.read(s_chunk, want);
        if (got <= 0) {
            vTaskDelay(idle);
            continue;
        }
        for (int i = 0; i < got; i++) { feedByte(s_chunk[i]); }
        // No delay here: at 12 Mbps a 1500-byte frame spans ~1 ms, and the
        // 4 KB RX queue is the only buffer in front of this loop.
    }
    s_rxTask = nullptr;
    vTaskDelete(nullptr);
}

void txTask(void *arg) {
    (void)arg;
    uint8_t hdr[7] = {kMagic0, kMagic1, kMagic2, kMagic3, kTypeFrame, 0, 0};
    Slot *slot = nullptr;
    while (s_running) {
        if (xQueueReceive(s_readySlots, &slot, pdMS_TO_TICKS(50)) != pdTRUE) { continue; }
        if (slot == nullptr) { continue; }
        uint16_t len = slot->len;
        hdr[5] = (uint8_t)(len & 0xFF);
        hdr[6] = (uint8_t)(len >> 8);
        Serial.write(hdr, sizeof(hdr));
        Serial.write(slot->data, len);
        s_txCount++;
        xQueueSend(s_freeSlots, &slot, portMAX_DELAY);
        slot = nullptr;
    }
    s_txTask = nullptr;
    vTaskDelete(nullptr);
}

void releaseSlots() {
    if (s_freeSlots) {
        vQueueDelete(s_freeSlots);
        s_freeSlots = nullptr;
    }
    if (s_readySlots) {
        vQueueDelete(s_readySlots);
        s_readySlots = nullptr;
    }
    if (s_slots) {
        free(s_slots);
        s_slots = nullptr;
    }
}

} // namespace

bool start() {
    if (s_running) { return true; }

    // The console has to stop reading the port before the first frame arrives,
    // and the RX queue has to hold more than a single 1500-byte frame.
    s_prevRxRing = Serial.setRxBufferSize(kRxRing);

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.setRxBufferSize(s_prevRxRing ? s_prevRxRing : 256);
        return false;
    }
    // Only created when nobody has one yet (WiFi, packet view and this module
    // all share the same default loop).
    esp_event_loop_create_default();

    s_slots = (Slot *)(psramFound() ? ps_malloc(sizeof(Slot) * kTxSlots) : malloc(sizeof(Slot) * kTxSlots));
    if (s_slots == nullptr) { return false; }
    s_freeSlots = xQueueCreate(kTxSlots, sizeof(Slot *));
    s_readySlots = xQueueCreate(kTxSlots, sizeof(Slot *));
    if (s_freeSlots == nullptr || s_readySlots == nullptr) {
        releaseSlots();
        return false;
    }
    for (int i = 0; i < kTxSlots; i++) {
        Slot *slot = &s_slots[i];
        xQueueSend(s_freeSlots, &slot, 0);
    }

    // Only the first entry into the page creates the interface; later entries
    // reuse the one stop() left down.
    if (s_netif == nullptr) {
        s_driverBase.post_attach = postAttach;
        s_driverBase.netif = nullptr;

        esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
        s_netif = esp_netif_new(&netif_cfg);
        if (s_netif == nullptr) {
            releaseSlots();
            Serial.setRxBufferSize(s_prevRxRing ? s_prevRxRing : 256);
            return false;
        }
        if (esp_netif_attach(s_netif, &s_driverBase) != ESP_OK) {
            esp_netif_destroy(s_netif);
            s_netif = nullptr;
            releaseSlots();
            Serial.setRxBufferSize(s_prevRxRing ? s_prevRxRing : 256);
            return false;
        }

        uint8_t mac[6];
        makeMac(mac);
        esp_netif_set_mac(s_netif, mac);
    }

    s_rxWant = 0;
    s_rxFill = 0;
    s_state = ST_MAGIC0;
    s_type = 0;
    s_rxCount = 0;
    s_txCount = 0;
    s_dropCount = 0;
    s_badCount = 0;
    s_hostLink = false;
    s_hostSeen = false;
    s_stopRequested = false;

    setSerialConsoleSuspended(true);
    s_running = true;

    xTaskCreate(rxTask, "usbeth_rx", 4096, nullptr, 5, &s_rxTask);
    xTaskCreate(txTask, "usbeth_tx", 3072, nullptr, 4, &s_txTask);

    // action_start brings the netif up (DHCP attaches to an up netif);
    // action_connected marks the link up, which is what the lwIP side wants
    // before it will send a DISCOVER. The PC helper's carrier report is only
    // shown, never enforced: a cable-less NIC should fail at DHCP, not here.
    esp_netif_action_start(s_netif, nullptr, 0, nullptr);
    esp_netif_action_connected(s_netif, nullptr, 0, nullptr);
    resolveLwipNetif();
    // Starts the DHCP client. ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED is the
    // normal answer when the inherent config already armed it.
    esp_netif_dhcpc_start(s_netif);
    return true;
}

void stop() {
    if (!s_running) { return; }
    s_running = false;

    // Wait for the workers to leave on their own; never vTaskDelete() them.
    // Deleting a task while it sits inside tcpip_input() can leave lwIP's core
    // locked, and then esp_netif_destroy() below blocks forever - which also
    // blocks the menu task, so the console never comes back and the board looks
    // dead (bench, 2026-10-30: helper stopped, board silent, no reboot).
    for (int i = 0; i < 200 && (s_rxTask != nullptr || s_txTask != nullptr); i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (s_rxTask != nullptr || s_txTask != nullptr) {
        // Still inside a syscall. Leave the interface alone rather than tear it
        // down under a running task; the console comes back either way.
        Serial.println("[USBETH] worker still busy, interface left up");
        setSerialConsoleSuspended(false);
        s_running = false;
        return;
    }

    // The interface itself is kept for the life of the boot and just taken
    // down. esp_netif_destroy() has to talk to the lwIP core, and one failed
    // round trip there wedges the menu task (see above); an interface that is
    // down costs nothing, and re-entering the page reuses it.
    if (s_netif != nullptr) {
        esp_netif_dhcpc_stop(s_netif);
        esp_netif_action_disconnected(s_netif, nullptr, 0, nullptr);
        esp_netif_action_stop(s_netif, nullptr, 0, nullptr);
    }
    releaseSlots();
    setSerialConsoleSuspended(false);
    Serial.setRxBufferSize(s_prevRxRing ? s_prevRxRing : 256);
    // Drop whatever the host sent while the interface was going down, so the
    // CLI does not start parsing a half frame as a command.
    while (Serial.available() > 0) { Serial.read(); }
}

bool running() { return s_running; }

bool hostSeen() { return s_hostSeen; }

bool hostLinkUp() { return s_hostLink; }

bool stopRequested() { return s_stopRequested; }

void clearStopRequest() { s_stopRequested = false; }

String ipString() {
    if (s_netif == nullptr) { return String("0.0.0.0"); }
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK) { return String("0.0.0.0"); }
    return IPAddress(info.ip.addr).toString();
}

esp_netif_t *netif() { return s_netif; }

uint32_t rxFrames() { return s_rxCount; }

uint32_t txFrames() { return s_txCount; }

uint32_t droppedFrames() { return s_dropCount + s_badCount; }

} // namespace UsbEthBridge
#endif
