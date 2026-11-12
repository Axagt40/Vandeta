#ifndef __CORE_DEV_DEVFLASH_H__
#define __CORE_DEV_DEVFLASH_H__

// Developer-only over-the-wire firmware upload (task 1, phase 4).
//
// The image goes into the *inactive* OTA partition with the IDF's own OTA API,
// and the boot partition is switched only by `end` after the size and the crc32
// both match. An interrupted transfer therefore leaves the running image and the
// boot pointer untouched: the board stays bootable no matter where the host
// stopped, and `begin` on the same size starts a fresh session.
//
// Gated on Developer Mode at the command layer and again here, because the
// framed binary protocol can call straight into this module.

#include <Arduino.h>
#include <stdint.h>

namespace DevFlash {

// Open a session. `expectedId` (8 bytes) is the build ID the host believes the
// device is running; a mismatch is refused unless `force`, so a payload built
// against another tree cannot be pushed by mistake. Null skips the check.
bool begin(uint32_t imageSize, uint32_t imageCrc, const uint8_t *expectedId, bool force, String &msg);

// Append one chunk. `offset` must equal the bytes already written (the protocol
// is strictly sequential, which is what esp_ota_write needs).
bool writeChunk(uint32_t offset, const uint8_t *data, size_t len, String &msg);

// Check the total size and crc32, finish the OTA handle and point the bootloader
// at the new partition.
bool end(String &msg);

// Discard the session; the running image is unaffected.
void abort(String &msg);

bool active();
uint32_t expectedSize();
uint32_t written();
uint32_t nextOffset();

// crc32 (IEEE, zlib-compatible) reused by the text chunk path.
uint32_t crc32(const uint8_t *data, size_t len);

} // namespace DevFlash

#endif
