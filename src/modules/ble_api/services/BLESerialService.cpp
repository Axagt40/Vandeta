#if !defined(LITE_VERSION)
#include "BLESerialService.h"
#include "modules/ble/ble_common.h" // bleNotifyRetry
#include <NimBLEDevice.h>

BLESerialService::BLESerialService() : BruceBLEService() {}

BLESerialService::~BLESerialService() {}

static bool newValue = false;

class BLESerialCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
        newValue = true;
    }
};

void BLESerialService::setup(NimBLEServer *pServer) {
    pService = pServer->createService("4371ec0b-3d43-49f9-b731-7c72a4a7bb91");

    serial_char = pService->createCharacteristic(
        "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9", // Battery Level
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE
    );

    callbacks = new BLESerialCallbacks();
    serial_char->setCallbacks(callbacks);

    pService->start();
    pServer->getAdvertising()->addServiceUUID(pService->getUUID());
}

void BLESerialService::end() { delete callbacks; }

int BLESerialService::available() {
    if (!newValue) return 0;
    newValue = false;

    return serial_char->getValue().size();
}

size_t BLESerialService::println(const String &s) {
    String toSend = s + "\r\n";
    bleNotifyRetry(serial_char, reinterpret_cast<const uint8_t *>(toSend.c_str()), toSend.length());
    vTaskDelay(pdMS_TO_TICKS(10)); // Add some delay to ensure data is read by the client
    return toSend.length();
}

size_t BLESerialService::print(const String &s) {
    bleNotifyRetry(serial_char, reinterpret_cast<const uint8_t *>(s.c_str()), s.length());
    vTaskDelay(pdMS_TO_TICKS(10));
    return s.length();
}

size_t BLESerialService::println(size_t n) {
    String s = String(n);
    return println(s);
}

void BLESerialService::vprintf(const char *fmt, va_list args) {
    // Three defects lived here, all of them invisible until the BLE serial was
    // the active output (`ble serial`):
    //  * vsnprintf() consumes its va_list, so the sprintf() on the next line
    //    formatted from a used-up list - undefined behaviour, garbage text;
    //  * that sprintf() had no bound, so anything longer than BUFFER_SIZE
    //    (a menu dump, a log line with a path) overflowed a stack buffer;
    //  * the length handed to the notification was vsnprintf()'s answer - the
    //    UNTRUNCATED size - so a long line made the radio read past the end of
    //    the string that was actually written.
    // Format once, into the buffer, and send exactly what was written. This is
    // the same shape as Serial's own vprintf().
    va_list copy;
    va_copy(copy, args);
    char str[BUFFER_SIZE];
    int n = vsnprintf(str, sizeof(str), fmt, copy);
    va_end(copy);
    if (n < 0) return;
    size_t len = (size_t)n < sizeof(str) ? (size_t)n : sizeof(str) - 1;
    if (len == 0) return;

    bleNotifyRetry(serial_char, reinterpret_cast<const uint8_t *>(str), len);
    vTaskDelay(pdMS_TO_TICKS(10));
}

String BLESerialService::readStringUntil(char terminator) {
    Serial.println("readStringUntil");
    String result = "";
    std::string value = serial_char->getValue();
    for (char c : value) {
        result += c;
        if (c == terminator) break;
    }
    return result;
}

size_t BLESerialService::println(const uint32_t n) {
    String s = String(n);
    return println(s);
}

size_t BLESerialService::print(const int n, int format) {
    String s = String(n, format);
    return print(s);
}

size_t BLESerialService::println(const int n, int format) {
    String s = String(n, format);
    return println(s);
}

size_t BLESerialService::println() { return println(""); }

size_t BLESerialService::write(uint8_t *str, size_t size) {
    bleNotifyRetry(serial_char, str, size);
    vTaskDelay(pdMS_TO_TICKS(10));
    return size;
}

int BLESerialService::read() {
    if (!available()) return -1;

    std::string value = serial_char->getValue();
    if (value.empty()) return -1;

    char firstChar = value[0];
    // Remove the first character from the buffer
    if (value.length() > 1) {
        serial_char->setValue(value.substr(1));
    } else {
        serial_char->setValue("");
    }

    return (int)firstChar;
}

void BLESerialService::setMTU(uint16_t mtu) { this->mtu = mtu; }

#endif
