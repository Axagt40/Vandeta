#if !defined(LITE_VERSION)

#ifndef USB_ETH_BRIDGE_H
#define USB_ETH_BRIDGE_H

#include <Arduino.h>
#include <esp_netif.h>

/**
 * Ethernet over the USB-C port, with the PC on the other end of the cable
 * acting as the PHY.
 *
 * Why this exists: the T-Embed CC1101 cannot drive an Ethernet port itself.
 * The ESP32-S3 has no MAC/PHY peripheral (no RMII), and its USB-C connector is
 * wired to the chip's USB device peripheral, so a USB-C hub's RJ45 port cannot
 * be enumerated or powered by the board. What *can* be done is to put a real
 * Ethernet device on the PC (including the RTL8153 inside a USB-C hub) and let
 * the board borrow it over the cable it is already plugged into.
 *
 * The link carries raw Ethernet frames, not IP: lwIP still does ARP, DHCP and
 * checksums, and the frames leave the PC's NIC with the board's own MAC. That
 * is what makes the Ethernet pages' layer-2 tools (ARP scan, DHCP starvation,
 * MAC flooding) work against the real network instead of only against a W5500
 * module on the QWIIC port.
 *
 * The host side is FREEBUFF/tools/eth_bridge.py; it needs root because it opens
 * a raw packet socket. Both ends speak what eth_bridge.py documents.
 *
 * Types: 0x01 Ethernet frame, 0x02 carrier state, 0x03 stop, 0x04 key press.
 * The key frame matters because the text console is suspended while the bridge
 * owns the port: it is what lets the PC drive the Ethernet page (and stop a
 * running ARP scan / DHCP starvation / MAC flooding) without touching the board.
 */
namespace UsbEthBridge {

/// Creates the interface and the two worker tasks. True when the interface is
/// up; the IP itself arrives later by DHCP.
bool start();

/// Tears the interface down and gives the USB console back to the CLI.
void stop();

bool running();

/// True once anything at all arrived from the PC helper: it is running and the
/// cable to the PC is alive, whatever the NIC's carrier says.
bool hostSeen();

/// Carrier state last reported by the PC helper (false until it says otherwise).
bool hostLinkUp();

/// True once the PC helper asked the board to leave the page (the helper sends
/// this when it is stopped, so Ctrl-C on the PC does not leave a dead page up).
bool stopRequested();
void clearStopRequest();

/// Dotted address of the interface, "0.0.0.0" until DHCP completes.
String ipString();

/// The lwIP interface, for the tools that take an esp_netif handle.
esp_netif_t *netif();

uint32_t rxFrames();
uint32_t txFrames();
/// Frames the board could not hand to the link (TX slots full).
uint32_t droppedFrames();

} // namespace UsbEthBridge

#endif // LITE_VERSION
#endif // USB_ETH_BRIDGE_H
