#include "massStorage.h"
#if defined(SOC_USB_OTG_SUPPORTED)
#include "core/bus_HAL.h" // acquireSPIBus(), for the fast MSC re-mount
#include "core/display.h"
#include <USB.h>
bool MassStorage::shouldStop = false;
int32_t MassStorage::status = -1;

MassStorage::MassStorage() { setup(); }

MassStorage::~MassStorage() {
    msc.end();
    USB.~ESPUSB();

    // Hack to make USB back to flash mode
    USB.enableDFU();
}

void MassStorage::setup() {
    displayMessage("Mounting...");

    setShouldStop(false);

    if (!setupSdCard()) {
        displayError("SD card not found.");
        delay(1000);
        return;
    }

    beginUsb();

    // 50 ms is enough for the host to start enumeration; the old 500 ms was a
    // full half second of "Mounting..." on every entry.
    delay(50);
    return loop();
}

void MassStorage::loop() {
    int32_t prev_status = -1;
    while (!check(EscPress) && !shouldStop) {
        if (prev_status != status) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            switch (status) {
                case ARDUINO_USB_STARTED_EVENT: drawUSBStickIcon(true); break; // host opened the card
                case ARDUINO_USB_STOPPED_EVENT: drawUSBStickIcon(false); break;
                case ARDUINO_USB_SUSPEND_EVENT: MassStorage::displayMessage("USB suspend"); break;
                case ARDUINO_USB_RESUME_EVENT: MassStorage::displayMessage("USB resume"); break;
                default: break;
            }
            prev_status = status;
        } else vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

// The rest of the firmware mounts the card at 4 MHz for compatibility, which is
// roughly 500 KB/s. That is fine for a log file and far too slow for a host
// walking a 30 GB exFAT directory tree over MSC - the "almost a minute before
// I can browse the files" report. Re-mount at the fastest rate the card takes
// before handing it to USB, and fall back to the old clock (and then to the
// existing mount) so a card that cannot take the higher rate still works.
static void raiseSdClockForUsb() {
#ifdef USE_SD_MMC
    // SDMMC already runs at BOARD_MAX_SDMMC_FREQ.
#else
    if (bruceConfigPins.SDCARD_bus.sck < 0) return;
    SPIClass *bus = acquireSPIBus(
        bruceConfigPins.SDCARD_bus.sck, bruceConfigPins.SDCARD_bus.miso, bruceConfigPins.SDCARD_bus.mosi
    );
    if (bus == nullptr) bus = &sdcardSPI;
    const int8_t cs = (int8_t)bruceConfigPins.SDCARD_bus.cs;
    static const uint32_t rates[] = {40000000UL, 20000000UL, 10000000UL};
    for (uint8_t i = 0; i < 3; i++) {
        if (SD.begin(cs, *bus, rates[i], "/sd", 8)) {
            Serial.printf("Mass Storage: SD clock %lu Hz\n", (unsigned long)rates[i]);
            return;
        }
    }
    Serial.println("Mass Storage: fast remount failed, retrying at 4 MHz");
    if (SD.begin(cs, *bus, 4000000UL, "/sd", 8)) Serial.println("Mass Storage: back at 4 MHz");
    else Serial.println("Mass Storage: remount failed, using the existing mount");
#endif
}

void MassStorage::beginUsb() {
    raiseSdClockForUsb();
    setupUsbCallback();
    setupUsbEvent();
    drawUSBStickIcon(false);
    USB.begin();
}

void MassStorage::setupUsbCallback() {
    uint32_t secSize = SD.sectorSize();
    uint32_t numSectors = SD.numSectors();

    msc.vendorID("ESP32");
    msc.productID("VANDETA");
    msc.productRevision("1.0");

    msc.onRead(usbReadCallback);
    msc.onWrite(usbWriteCallback);
    msc.onStartStop(usbStartStopCallback);

    msc.mediaPresent(true);
    msc.begin(numSectors, secSize);
}

void MassStorage::setupUsbEvent() {
    USB.onEvent([](void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
        if (event_base == ARDUINO_USB_EVENTS) { status = event_id; }
    });
}

void MassStorage::displayMessage(String message) {
    drawMainBorderWithTitle("Mass Storage");
    padprintln("");
    padprintln(message);
}

// MSC passes an offset inside the first sector. Both callbacks used to ignore
// it and always started at the sector boundary, so every unaligned transfer
// returned (or wrote) the wrong bytes - a host that does that while updating a
// FAT or a directory entry leaves the filesystem inconsistent, which is the
// "sometimes the UI is broken after mass storage" report. Unaligned writes are
// read-modify-write, aligned ones go straight through.
static uint8_t s_sector[512];

int32_t usbWriteCallback(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    const uint32_t secSize = SD.sectorSize();
    if (secSize == 0 || secSize > sizeof(s_sector) || offset >= secSize) return -1;

    uint64_t freeSpace = SD.totalBytes() - SD.usedBytes();
    if (bufsize > freeSpace) return -1; // no space available

    uint32_t done = 0;
    if (offset != 0) {
        uint32_t head = bufsize;
        if (head > secSize - offset) head = secSize - offset;
        if (!SD.readRAW(s_sector, lba)) return -1;
        memcpy(s_sector + offset, buffer, head);
        if (!SD.writeRAW(s_sector, lba)) return -1;
        done = head;
        lba++;
    }

    uint32_t whole = (bufsize - done) / secSize;
    for (uint32_t x = 0; x < whole; x++) {
        if (!SD.writeRAW(buffer + done + x * secSize, lba + x)) return -1;
    }
    done += whole * secSize;
    if (done < bufsize) {
        if (!SD.readRAW(s_sector, lba + whole)) return -1;
        memcpy(s_sector, buffer + done, bufsize - done);
        if (!SD.writeRAW(s_sector, lba + whole)) return -1;
    }
    return bufsize;
}

int32_t usbReadCallback(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    const uint32_t secSize = SD.sectorSize();
    if (secSize == 0 || secSize > sizeof(s_sector) || offset >= secSize) return -1;

    uint8_t *out = reinterpret_cast<uint8_t *>(buffer);
    uint32_t done = 0;
    if (offset == 0) {
        uint32_t whole = bufsize / secSize;
        for (uint32_t x = 0; x < whole; x++) {
            if (!SD.readRAW(out + x * secSize, lba + x)) return -1;
        }
        done = whole * secSize;
        lba += whole;
    }
    if (done < bufsize) {
        uint32_t need = bufsize - done;
        if (need > secSize - offset) return -1;
        if (!SD.readRAW(s_sector, lba)) return -1;
        memcpy(out + done, s_sector + offset, need);
    }
    return bufsize;
}

bool usbStartStopCallback(uint8_t power_condition, bool start, bool load_eject) {
    if (!start && load_eject) {
        MassStorage::setShouldStop(true);
        return false;
    }

    return true;
}

void drawUSBStickIcon(bool plugged) {
    // This used to draw the body only on the first call of the boot (a static
    // flag): leaving Mass Storage and entering it again left a blank screen
    // with just the LED, which is the "waiting without the USB stick" report.
    MassStorage::displayMessage(plugged ? "Connected - open the drive on your PC" : "Waiting for USB host...");

    float scale;
    if (bruceConfigPins.rotation & 0b01) scale = float((float)tftHeight / (float)135);
    else scale = float((float)tftWidth / (float)240);

    int iconW = scale * 120;
    int iconH = scale * 40;

    if (iconW % 2 != 0) iconW++;
    if (iconH % 2 != 0) iconH++;

    int radius = 5;

    int bodyW = 2 * iconW / 3;
    int bodyH = iconH;
    int bodyX = tftWidth / 2 - iconW / 2;
    int bodyY = tftHeight / 2;

    int portW = iconW - bodyW;
    int portH = 0.8 * bodyH;
    int portX = bodyX + bodyW;
    int portY = bodyY + (bodyH - portH) / 2;

    int portDetailW = portW / 2;
    int portDetailH = portH / 4;
    int portDetailX = portX + (portW - portDetailW) / 2;
    int portDetailY1 = portY + 0.8 * portDetailH;
    int portDetailY2 = portY + portH - 1.8 * portDetailH;

    int ledW = 0.1 * bodyH;
    int ledH = 0.6 * bodyH;
    int ledX = bodyX + 2 * ledW;
    int ledY = bodyY + (iconH - ledH) / 2;

    // Body
    tft.fillRoundRect(bodyX, bodyY, bodyW, bodyH, radius, TFT_DARKCYAN);
    // Port USB
    tft.fillRoundRect(portX, portY, portW, portH, radius, TFT_LIGHTGREY);
    // Small square on port
    tft.fillRoundRect(portDetailX, portDetailY1, portDetailW, portDetailH, radius, TFT_DARKGREY);
    tft.fillRoundRect(portDetailX, portDetailY2, portDetailW, portDetailH, radius, TFT_DARKGREY);
    // Led
    tft.fillRoundRect(ledX, ledY, ledW, ledH, radius, plugged ? TFT_GREEN : TFT_RED);
    tft.setTextColor(TFT_LIGHTGREY, bruceConfig.bgColor);
    tft.drawCentreString("ESC: eject", tftWidth / 2, tftHeight - 16, 1);
}

#endif // SOC_USB_OTG_SUPPORTED
