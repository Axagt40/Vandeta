#include "core/dev/devflash.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <string.h>

#include "core/dev/build_info.h"
#include "core/usb_debug/usbdebug.h"

// esp_ota_write() is the IDF's sanctioned writer: it suspends the cache around
// the spi_flash_write itself, so there is no flash-resident code in the write
// path to move to IRAM. A hand-rolled writer would have to reimplement that.
namespace DevFlash {

static const esp_partition_t *s_part = nullptr;
static esp_ota_handle_t s_handle = 0;
static bool s_active = false;
static uint32_t s_size = 0;
static uint32_t s_written = 0;
static uint32_t s_crcWant = 0;
static uint32_t s_crc = 0;

// Nibble table: two lookups per byte instead of eight bit steps. A whole image
// is checksummed twice (device and host), so the cheap version is worth it.
static uint32_t s_tab[16];
static bool s_tabReady = false;

static void crcInit() {
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t c = i;
        for (int k = 0; k < 4; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_tab[i] = c;
    }
    s_tabReady = true;
}

static uint32_t crcUpdate(uint32_t state, const uint8_t *d, size_t n) {
    if (!s_tabReady) crcInit();
    uint32_t c = state ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c = s_tab[(c ^ d[i]) & 0x0F] ^ (c >> 4);
        c = s_tab[(c ^ (d[i] >> 4)) & 0x0F] ^ (c >> 4);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t crc32(const uint8_t *data, size_t len) { return crcUpdate(0, data, len); }

bool begin(uint32_t imageSize, uint32_t imageCrc, const uint8_t *expectedId, bool force, String &msg) {
    if (!UsbDebug::enabled()) {
        msg = "ERR: developer mode off";
        return false;
    }
    if (s_active) {
        msg = "ERR: transfer already open at " + String(s_written) + "/" + String(s_size) + " (abort first)";
        return false;
    }
    if (expectedId != nullptr && !force) {
        uint8_t mine[8];
        vndBuildIdBytes(mine);
        if (memcmp(mine, expectedId, 8) != 0) {
            msg = "ERR: build ID mismatch (this image is " + String(vndBuildId()) + ")";
            return false;
        }
    }
    if (imageSize == 0) {
        msg = "ERR: image size is 0";
        return false;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (part == nullptr) {
        msg = "ERR: no inactive OTA partition";
        return false;
    }
    if (imageSize > part->size) {
        msg = "ERR: image " + String(imageSize) + " B does not fit " + String(part->label) + " (" +
              String((uint32_t)part->size) + " B)";
        return false;
    }
    const esp_err_t e = esp_ota_begin(part, imageSize, &s_handle);
    if (e != ESP_OK) {
        msg = "ERR: esp_ota_begin: " + String(esp_err_to_name(e));
        return false;
    }
    s_part = part;
    s_size = imageSize;
    s_written = 0;
    s_crcWant = imageCrc;
    s_crc = 0;
    s_active = true;
    msg = "OK begin " + String(imageSize) + " B -> " + String(part->label) + " @0x" + String(part->address, HEX);
    return true;
}

bool writeChunk(uint32_t offset, const uint8_t *data, size_t len, String &msg) {
    if (!s_active) {
        msg = "ERR: no transfer open (devflash begin <size> <crc>)";
        return false;
    }
    if (offset != s_written) {
        msg = "ERR: out of order chunk at " + String(offset) + ", expected " + String(s_written);
        return false;
    }
    if (len == 0) {
        msg = "ERR: empty chunk";
        return false;
    }
    if ((uint64_t)s_written + len > s_size) {
        msg = "ERR: chunk runs past the declared image size";
        return false;
    }
    const esp_err_t e = esp_ota_write(s_handle, data, len);
    if (e != ESP_OK) {
        msg = "ERR: esp_ota_write: " + String(esp_err_to_name(e));
        return false;
    }
    s_crc = crcUpdate(s_crc, data, len);
    s_written += len;
    msg = "OK " + String(s_written) + "/" + String(s_size);
    return true;
}

bool end(String &msg) {
    if (!s_active) {
        msg = "ERR: no transfer open";
        return false;
    }
    if (s_written != s_size) {
        msg = "ERR: short image, have " + String(s_written) + " of " + String(s_size) + " B";
        return false;
    }
    // Compare before esp_ota_end(): a crc failure must not mark the image valid.
    if (s_crc != s_crcWant) {
        char got[9], want[9];
        snprintf(got, sizeof(got), "%08X", s_crc);
        snprintf(want, sizeof(want), "%08X", s_crcWant);
        msg = String("ERR: crc32 mismatch got ") + got + " want " + want;
        esp_ota_abort(s_handle);
        s_active = false;
        s_handle = 0;
        return false;
    }
    esp_err_t e = esp_ota_end(s_handle);
    s_active = false;
    s_handle = 0;
    if (e != ESP_OK) {
        msg = "ERR: esp_ota_end: " + String(esp_err_to_name(e));
        return false;
    }
    e = esp_ota_set_boot_partition(s_part);
    if (e != ESP_OK) {
        msg = "ERR: esp_ota_set_boot: " + String(esp_err_to_name(e));
        return false;
    }
    msg = "OK image verified (" + String(s_size) + " B, crc32 ok), boot partition set - reboot to run";
    return true;
}

void abort(String &msg) {
    if (!s_active) {
        msg = "OK nothing to abort";
        return;
    }
    esp_ota_abort(s_handle);
    s_handle = 0;
    s_active = false;
    s_size = 0;
    s_written = 0;
    msg = "OK transfer aborted, running image untouched";
}

bool active() { return s_active; }
uint32_t expectedSize() { return s_size; }
uint32_t written() { return s_written; }
uint32_t nextOffset() { return s_written; }

} // namespace DevFlash
