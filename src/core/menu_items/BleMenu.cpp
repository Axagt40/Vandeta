#include "BleMenu.h"
#include "core/display.h"
#include "core/packet_view.h"
#include "core/utils.h"
#include "modules/badusb_ble/ducky_typer.h"
#include "modules/ble/ble_common.h"
#include "modules/ble/ble_detector.h"
#include "modules/ble/ble_ninebot.h"
#include "modules/ble/ble_capture.h"
#include "modules/ble/ble_spam.h"
#include "modules/ble/ble_attacks.h"
#if !defined(LITE_VERSION)
#include "modules/ble/BLE_Suite.h"
#else
#include "modules/ble/ble_sniffer.h"
#endif
#include <globals.h>

void BleMenu::optionsMenu() {
    options.clear();
#if !defined(LITE_VERSION)
    if (BLEConnected) {
        options.push_back({"Disconnect", [=]() {
                               BLEDevice::deinit();
                               BLEConnected = false;
                               delete hid_ble;
                               hid_ble = nullptr;
                           }});
    }
#endif
#if !defined(LITE_VERSION)
    // 13 flat rows mixed scanning, attacks, HID and device tools, and the two
    // attack pages ("BLE Suite" and "BLE Atks") read as unrelated buttons.
    // Four groups now; the session row above and Back are where they were, and
    // every tool keeps its own name inside its group.
    options.push_back({"Scan & Detect", [this]() { scanMenu(); }});
    options.push_back({"Attacks", [this]() { attacksMenu(); }});
    options.push_back({"HID & BadUSB", [this]() { hidMenu(); }});
    options.push_back({"Tools", [this]() { toolsMenu(); }});
#else
    options.push_back({"BLE Spam", [=]() { spamMenu(); }});
    options.push_back({"Capture & Replay", [=]() { bleCaptureMenu(); }});
    options.push_back({"Packet Monitor", [=]() { packetMonitor(PKT_SRC_BLE); }});
    options.push_back({"BLE Sniffer", [=]() { BLE_SnifferMenu(); }});
#endif
    addOptionToMainMenu();

    loopOptions(options, MENU_TYPE_SUBMENU, "Bluetooth", 0, false);
}

#if !defined(LITE_VERSION)
void BleMenu::scanMenu() {
    std::vector<Option> scan;
    scan.push_back({"BLE Scan", ble_scan});
    scan.push_back({"BLE Detector", []() { vandetaRadarMenu(); }});
    scan.push_back({"Packet Monitor", [=]() { packetMonitor(PKT_SRC_BLE); }});
    scan.push_back({"Back", [this]() { optionsMenu(); }});
    loopOptions(scan, MENU_TYPE_SUBMENU, "Scan & Detect");
}

void BleMenu::attacksMenu() {
    std::vector<Option> atks;
    atks.push_back({"BLE Suite", [=]() { BleSuiteMenu(); }});
    atks.push_back({"BLE Atks", [=]() { BleAttacksMenu(); }});
    atks.push_back({"BLE Spam", [=]() { spamMenu(); }});
    atks.push_back({"iBeacon", [=]() {
                        ibeacon("Bruce", "e4c159a0-8c82-11e6-bdf4-0800200c9a66", 0x004C);
                    }});
    atks.push_back({"Back", [this]() { optionsMenu(); }});
    loopOptions(atks, MENU_TYPE_SUBMENU, "BLE Attacks");
}

void BleMenu::hidMenu() {
    std::vector<Option> hid;
    hid.push_back({"Bad BLE", [=]() { ducky_setup(hid_ble, true); }});
    hid.push_back({"BLE Keyboard", [=]() { ducky_keyboard(hid_ble, true); }});
    hid.push_back({"Presenter mode", [=]() { PresenterMode(hid_ble, true); }});
    hid.push_back({"Media Cmds", [=]() { MediaCommands(hid_ble, true); }});
    hid.push_back({"Back", [this]() { optionsMenu(); }});
    loopOptions(hid, MENU_TYPE_SUBMENU, "HID & BadUSB");
}

void BleMenu::toolsMenu() {
    std::vector<Option> tools;
    tools.push_back({"Capture & Replay", [=]() { bleCaptureMenu(); }});
    tools.push_back({"Ninebot", [=]() { BLENinebot(); }});
    tools.push_back({"Back", [this]() { optionsMenu(); }});
    loopOptions(tools, MENU_TYPE_SUBMENU, "BLE Tools");
}
#endif

void BleMenu::drawIcon(float scale) {
    clearIconArea();

    int lineWidth = scale * 5;
    int iconW = scale * 36;
    int iconH = scale * 60;
    int radius = scale * 5;
    int deltaRadius = scale * 10;

    if (iconW % 2 != 0) iconW++;
    if (iconH % 4 != 0) iconH += 4 - (iconH % 4);

    tft.drawWideLine(
        iconCenterX,
        iconCenterY + iconH / 4,
        iconCenterX - iconW,
        iconCenterY - iconH / 4,
        lineWidth,
        bruceConfig.priColor,
        bruceConfig.priColor
    );
    tft.drawWideLine(
        iconCenterX,
        iconCenterY - iconH / 4,
        iconCenterX - iconW,
        iconCenterY + iconH / 4,
        lineWidth,
        bruceConfig.priColor,
        bruceConfig.priColor
    );
    tft.drawWideLine(
        iconCenterX,
        iconCenterY + iconH / 4,
        iconCenterX - iconW / 2,
        iconCenterY + iconH / 2,
        lineWidth,
        bruceConfig.priColor,
        bruceConfig.priColor
    );
    tft.drawWideLine(
        iconCenterX,
        iconCenterY - iconH / 4,
        iconCenterX - iconW / 2,
        iconCenterY - iconH / 2,
        lineWidth,
        bruceConfig.priColor,
        bruceConfig.priColor
    );

    tft.drawWideLine(
        iconCenterX - iconW / 2,
        iconCenterY - iconH / 2,
        iconCenterX - iconW / 2,
        iconCenterY + iconH / 2,
        lineWidth,
        bruceConfig.priColor,
        bruceConfig.priColor
    );

    tft.drawArc(
        iconCenterX,
        iconCenterY,
        2.5 * radius,
        2 * radius,
        210,
        330,
        bruceConfig.priColor,
        bruceConfig.bgColor
    );
    tft.drawArc(
        iconCenterX,
        iconCenterY,
        2.5 * radius + deltaRadius,
        2 * radius + deltaRadius,
        210,
        330,
        bruceConfig.priColor,
        bruceConfig.bgColor
    );
    tft.drawArc(
        iconCenterX,
        iconCenterY,
        2.5 * radius + 2 * deltaRadius,
        2 * radius + 2 * deltaRadius,
        210,
        330,
        bruceConfig.priColor,
        bruceConfig.bgColor
    );
}
