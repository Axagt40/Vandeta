#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include "config.h"
#include "configPins.h"
#include <NTPClient.h>
#include <globals.h>

void _setBrightness(uint8_t brightval) __attribute__((weak));

void setBrightness(uint8_t brightval, bool save = true);

void getBrightness();

int gsetRotation(bool set = false);

void setBrightnessMenu();

// Battery (Config > Battery): the power-saving mode and the charge limit.
// Both read bruceConfig and are safe to call on any board.
void applyBatterySaver();

void applyChargeLimit();

int chargeLimitPercent();

// Brightness the firmware will apply: bruceConfig.bright, capped while Battery
// Saver is on. Lets the `battery` console verb report the setting rather than
// the transient 0 the power-save timer writes when it blanks the panel.
int effectiveScreenBrightness();

// CPU clock the firmware should run at: `defaultMhz` normally, or the saver's
// lower clock while Battery Saver is on. Every place that restores the CPU
// clock on its way out (sleep wake, Developer Mode, the Wi-Fi cracker) uses
// this, so none of them can silently undo the saver.
int batterySaverClock(int defaultMhz);

void setChargeLimitMenu();

void setUIColor();

bool setCustomUIColorMenu();

void setCustomUIColorChoiceMenu(int colorType);

void setCustomUIColorSettingMenuR(int colorType);

void setCustomUIColorSettingMenuG(int colorType);

void setCustomUIColorSettingMenuB(int colorType);

void setCustomUIColorSettingMenu(
    int colorType, int rgb, std::function<uint16_t(uint16_t, int)> colorGenerator
);

void addEvilWifiMenu();

void removeEvilWifiMenu();

void setEvilEndpointCreds();

void setEvilEndpointSsid();

void setEvilAllowEndpointDisplay();

void setEvilAllowGetCreds();

void setEvilAllowSetSsid();

void setEvilPasswordMode();

void setEvilGatewayIp();

void setRFModuleMenu();

void setRFFreqMenu();

void setRFIDModuleMenu();

void addMifareKeyMenu();

void setSleepMode();

void setDimmerTimeMenu();

void setClock();

void runClockLoop(bool showMenuHint = false);

int gsetIrTxPin(bool set = false);

void setIrTxRepeats();

int gsetIrRxPin(bool set = false);

int gsetRfTxPin(bool set = false);

int gsetRfRxPin(bool set = false);

void setSoundConfig();

void setSoundVolume();

#ifdef HAS_RGB_LED
void setLedBlinkConfig();
#endif

void setWifiStartupConfig();

void setStartupApp();

void setGpsBaudrateMenu();

void setNetworkCredsMenu();

void setBadUSBBLEMenu();
void setBadUSBBLEKeyboardLayoutMenu();
void setBadUSBBLEKeyDelayMenu();
void setBadUSBBLEShowOutputMenu();

void setSPIPinsMenu(BruceConfigPins::SPIPins &value);

void setUARTPinsMenu(BruceConfigPins::UARTPins &value);

void setI2CPinsMenu(BruceConfigPins::I2CPins &value);

void setTheme();

void setMacAddressMenu();

#if !defined(LITE_VERSION)
void enableBLEAPI();

bool appStoreInstalled();

void installAppStoreJS();
#endif

#endif
