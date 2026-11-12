#ifndef __CORE_DEV_BUILD_INFO_H__
#define __CORE_DEV_BUILD_INFO_H__

// Build identity (task 1, phase 4).
//
// BRUCE_VERSION used to live in build_flags, so every version bump changed the
// compile flags and invalidated the whole object cache - a 15-25 minute rebuild
// for one string. The version and the build ID are now generated into
// `vnd_build_info.h` by build_id.py and included by exactly one translation
// unit (`build_info.cpp`). Everything else calls the functions below, so a
// version bump only recompiles one file.
//
// This header is hand-written and never changes, so including it anywhere costs
// nothing in rebuild terms.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// "vandeta 1.4-pre", from version.txt at build time.
const char *vndVersion();

// "<git-sha8>[-dirty]-<YYYYMMDD-HHMMSS>", e.g. "1a2b3c4d-dirty-20261111-153000".
const char *vndBuildId();

// The same ID folded to the 8 bytes a VNDP payload header carries. Stable for
// one image, so `patch apply` and `devflash begin` can both refuse a blob that
// was built for a different tree.
void vndBuildIdBytes(uint8_t out[8]);

#ifdef __cplusplus
}
#endif

#endif
