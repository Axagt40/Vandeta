#ifndef __DUCKY_TYPER_H
#define __DUCKY_TYPER_H
#if !defined(LITE_VERSION)
#include <Arduino.h>
#include <SD.h>
#include <USB.h>
#include <globals.h>

#ifdef USB_as_HID
#include <USBHIDKeyboard.h>
#else
#include <CH9329_Keyboard.h>
#endif
#include <BleKeyboard.h>

extern HIDInterface *hid_usb;
extern HIDInterface *hid_ble;
extern int activeBLEInstances;

struct DuckyCommand;
struct DuckyCommandLookup;
struct DuckyCombination;

// Start badUSB or badBLE ducky runner
void ducky_setup(HIDInterface *&hid, bool ble = false);

// Setup the keyboard for badUSB or badBLE
// functionId: 0=Keyboard, 1=Media, 2=BadUSB, 3=Presenter
void ducky_startKb(HIDInterface *&hid, bool ble, int functionId = 0);

// Parses a file to run in the badUSB
void key_input(FS fs, const String &bad_script, HIDInterface *hid);

// Sends a simple command through USB
void key_input_from_string(const String &text);

// Use device as a keyboard (USB or BLE)
void ducky_keyboard(HIDInterface *&hid, bool ble = false);

// Send media commands through BLE or USB HID
void MediaCommands(HIDInterface *hid, bool ble = false);

DuckyCommandLookup *findDuckyCommand(const char *cmd);
DuckyCombination *findDuckyCombination(const char *cmd);

void sendAltChar(HIDInterface *hid, uint8_t charCode);
void sendAltString(HIDInterface *hid, const String &text);

void printHeaderBadUSBBLE(const String &bad_script);
void printStatusBadUSBBLE(const String &status);
void printTFTBadUSBBLE(const String &text, uint16_t color = TFT_WHITE, bool newline = false);

void printDecimalTime(uint32_t milliseconds);

bool waitForButtonPress();
bool handlePauseResume();

// Presenter mode - press button to advance slides
void PresenterMode(HIDInterface *&hid, bool ble = true);

// Shared cleanup for ducky_typer BLE functions - cleans a specific instance
void cleanupDuckyBLE(HIDInterface *&hid);

// ---------------------------------------------------------------------------
// BadUSB keyboard layout management (backs the BadUSB Config menu and the
// badusb.setLayout console/JS helpers).
//   indices 0..14  on-flash layouts (0 US, 1 Danish, 2 UK, 3 FR, 4 DE, 5 HU,
//                  6 IT, 7 US-alt, 8 PT-BR, 9 PT, 10 SI, 11 ES, 12 SV,
//                  13 TR, 14 Finnish)
//   index 15       user supplied custom layout held in RAM
// ---------------------------------------------------------------------------
#define BADUSB_LAYOUT_CUSTOM_INDEX 15
#define BADUSB_LAYOUT_COUNT_TOTAL 16

/// Human readable name for a layout index (never returns NULL).
const char *badusbLayoutName(int idx);
/// Number of selectable layout slots (16).
int badusbLayoutCount();
/// Byte table for a layout index; handles the custom layout and clamps.
const uint8_t *badusbResolveLayout(int idx);
/// Copy an on-flash layout into the custom buffer ("create from existing").
bool badusbSeedCustomLayoutFrom(int idx);
/// Import a custom layout from a text file of 128 hex bytes.
bool badusbLoadCustomLayoutFile(const String &path);
/// Export the current custom layout to a text file.
bool badusbSaveCustomLayoutFile(const String &path);
/// Run a payload file through the HID/BLE ducky engine (true when it ran).
bool badusbRunPayload(const String &filepath);

#if defined(USB_as_HID)
/// Apply the configured USB VID/PID/manufacturer/product/serial before USB.begin().
void badusbApplyUsbFootprint();
/// Create the optional mouse HID endpoint for composite/mouse device types.
void badusbStartHidExtraDevices();
void badusbMouseMove(int8_t x, int8_t y, int8_t wheel);
void badusbMouseClick(uint8_t button);
#endif

// Double cleanup with cooling delay
void safeCleanupDuckyBLE(HIDInterface *&hid);

#endif
#endif
