#include "EthernetMenu.h"
#if !defined(LITE_VERSION)
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/settings.h"
#include "core/utils.h"
#include "modules/ethernet/ARPScanner.h"
#include "modules/ethernet/DHCPStarvation.h"
#include "modules/ethernet/EthernetHelper.h"
#include "modules/ethernet/MACFlooding.h"
#include "modules/ethernet/UsbEthBridge.h"
#include <globals.h>

/// One line of state for the USB bridge page. The board has no carrier of its
/// own: the PC helper reports whether the real NIC is up.
static void draw_usb_eth_status(const String &title, const String &hint) {
    drawMainBorderWithTitle(title);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    // Two different things: the PC helper answering at all, and that PC's NIC
    // having a cable in it. Without a cable the helper still runs, so the page
    // must not say "no PC" in that case.
    tft.drawString(String("PC: ") + (UsbEthBridge::hostSeen() ? "helper ok" : "waiting"), 10, 40);
    tft.drawString(String("NIC: ") + (UsbEthBridge::hostLinkUp() ? "carrier up" : "no carrier"), 10, 56);
    tft.drawString(String("IP: ") + UsbEthBridge::ipString(), 10, 72);
    tft.drawString(
        String("RX ") + String(UsbEthBridge::rxFrames()) + "  TX " + String(UsbEthBridge::txFrames()), 10, 88
    );
    tft.drawString(String("Dropped: ") + String(UsbEthBridge::droppedFrames()), 10, 104);
    if (hint.length()) tft.drawString(hint, 10, 122);
    tft.drawString("BACK to leave", 10, tftHeight - 20);
}

/// Brings the bridge up and waits, with a deadline, for the PC helper and a
/// DHCP lease. Returns false when the interface could not be created at all;
/// a missing helper or a LAN without DHCP only shows up as "no IP".
bool EthernetMenu::start_usb_ethernet() {
    if (UsbEthBridge::running()) return true;
    if (!UsbEthBridge::start()) return false;

    uint32_t start = millis();
    uint32_t lastPaint = 0;
    while (millis() - start < 20000) {
        if (check(EscPress) || UsbEthBridge::stopRequested()) break;
        if (UsbEthBridge::ipString() != "0.0.0.0") break;
        if (millis() - lastPaint >= 500) {
            lastPaint = millis();
            draw_usb_eth_status(
                "USB Ethernet",
                !UsbEthBridge::hostSeen() ? "Run eth_bridge.py --share on PC"
                : !UsbEthBridge::hostLinkUp() ? "Cable in the PC's LAN port"
                                              : "Asking for a lease..."
            );
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    // "No lease" almost always means the helper bridged a NIC with no DHCP on
    // it (or the hub's LAN cabled back into this PC), so name the fix.
    draw_usb_eth_status(
        "USB Ethernet", UsbEthBridge::ipString() == "0.0.0.0" ? "No lease - try --share" : "Ready"
    );
    return true;
}

void EthernetMenu::start_ethernet() {
    eth = new EthernetHelper();
    if (!eth->setup()) {
        displayError("W5500 not found");
        delete eth;
        eth = nullptr;
        return;
    }
    // Bounded: a W5500 with no DHCP server used to hang this page forever.
    uint32_t start = millis();
    while (!eth->is_connected() && millis() - start < 15000) { delay(100); }
    if (!eth->is_connected()) displayError("No DHCP lease", true);
}

void EthernetMenu::usb_ethernet_menu() {
    if (!start_usb_ethernet()) {
        displayError("USB bridge failed", true);
        return;
    }

    options = {
        {"Scan Hosts",
         []() {
             run_arp_scanner(UsbEthBridge::netif());
         }},
        {"DHCP Starvation",
         []() {
             DHCPStarvation();
         }},
        {"MAC Flooding",
         []() {
             MACFlooding();
         }},
        {"Live Status",
         []() {
             uint32_t start = millis();
             while (millis() - start < 120000) {
                 if (check(EscPress) || check(SelPress) || UsbEthBridge::stopRequested()) break;
                 draw_usb_eth_status("USB Ethernet", "");
                 vTaskDelay(pdMS_TO_TICKS(500));
             }
         }},
    };
    addOptionToMainMenu();

    delay(200);
    loopOptions(options, MENU_TYPE_SUBMENU, "USB Ethernet");
    UsbEthBridge::stop();
}

void EthernetMenu::optionsMenu() {
    options = {
        {"USB Ethernet (PC LAN)",
         [this]() {
             usb_ethernet_menu();
         }                                              },
        // These three drive the W5500 on the QWIIC pins. Without that module
        // they can only answer "W5500 not found", so the label says so up front
        // instead of looking like the board's own Ethernet is broken.
        {"Scan Hosts (W5500)",
         [this]() {
             start_ethernet();
             if (eth != nullptr) {
                 run_arp_scanner();
                 eth->stop();
             } else {
                    displayError("W5500 not found");
             }
         }                        },
        {"DHCP Starvation (W5500)",
         [this]() {
             start_ethernet();
             if (eth != nullptr) {
                 DHCPStarvation();
                 eth->stop();
             } else {
                    displayError("W5500 not found");
             }
         }                        },
        {"MAC Flooding (W5500)",    [this]() {
             start_ethernet();
             if (eth != nullptr) {
                 MACFlooding();
                 eth->stop();
             } else {
                    displayError("W5500 not found");
             }
         }}
    };
    addOptionToMainMenu();

    delay(200);

    loopOptions(options, MENU_TYPE_SUBMENU, "Ethernet");
}

void EthernetMenu::drawIcon(float scale) {
    clearIconArea();

    // Dimensions
    int cx = iconCenterX;
    int cy = iconCenterY + (4 * scale); // Centered and slightly lower
    int w = 48 * scale;
    int h = 40 * scale;
    if (w % 2 != 0) w++;
    if (h % 2 != 0) h++;

    int x1 = cx - w / 2;
    int x2 = cx + w / 2;
    int y1 = cy - h / 2;
    int y2 = cy + h / 2;

    // Thickness, Width & Height
    int t = 3 * scale;
    if (t < 2) t = 2;

    int nw = 20 * scale;
    if (nw % 2 != 0) nw++;
    int nh = 12 * scale;


    // Borders
    tft.fillRect(x1, y1, w, t, bruceConfig.priColor);

    tft.fillRect(x1, y1, t, h, bruceConfig.priColor);
    tft.fillRect(x2 - t, y1, t, h, bruceConfig.priColor);

    // Pieces
    int sideW = (w - nw) / 2 + t;
    tft.fillRect(x1, y2 - t, sideW, t, bruceConfig.priColor);
    tft.fillRect(x2 - sideW, y2 - t, sideW, t, bruceConfig.priColor);
    tft.fillRect(cx - nw / 2, y2 - nh - t, t, nh + t, bruceConfig.priColor);
    tft.fillRect(cx + nw / 2 - t, y2 - nh - t, t, nh + t, bruceConfig.priColor);
    tft.fillRect(cx - nw / 2, y2 - nh - t, nw, t, bruceConfig.priColor);

    // 8 Pins
    int pinW = 2 * scale;
    if (pinW < 1) pinW = 1;
    int pinH = (h / 2) - t;

    int step = (28 * scale) / 7;
    if (step < 2) step = 2;
    int totalSpread = step * 7;
    int firstPinX = cx - (totalSpread / 2);

    for (int i = 0; i < 8; i++) {
        int px = firstPinX + (i * step) - (pinW / 2);
        tft.fillRect(px, y1 + t, pinW, pinH, bruceConfig.priColor);
    }
}
#endif
