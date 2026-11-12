Import("env")
import os

# The Arduino ESP32 framework injects -fno-lto into LINKFLAGS to prevent LTO
# at link time. Remove it so GCC can perform whole-program LTO optimization
# across all LTO IR objects (user code + framework Arduino source).
# Prebuilt ESP-IDF archives (libfreertos.a etc.) are regular objects and
# participate in LTO linking transparently.
flags = env.get("LINKFLAGS", [])
if "-fno-lto" in flags:
    env.Replace(LINKFLAGS=[f for f in flags if f != "-fno-lto"])

# Parallel LTO link, development environment only.
#
# It has to be appended to LINKFLAGS, from here:
#   * a bare -flto=4 in build_flags is classified by PlatformIO's ProcessFlags()
#     as a *compile* flag, so it never reaches the link command at all. That is
#     why the 2026-11-08 PLATFORMIO_BUILD_FLAGS=-flto=4 trick did nothing for
#     the link and instead changed every object's signature, silently throwing
#     away the whole cached object tree whenever it was toggled.
#   * build.py is registered as `post:build.py`, i.e. it runs after the link.
# This script runs at environment init - the same place the -fno-lto removal
# above proves a LINKFLAGS edit survives into the link command.
#
# LTO stays ON: only the number of parallel LTRANS jobs changes, so the image
# is byte-for-byte the release build and stays flashable. The shipping
# environment never gets the flag. VANDETA_LTO_JOBS=0 (or 1) disables it.
pioenv = env.subst("${PIOENV}")
if pioenv.endswith("-dev"):
    jobs = os.environ.get("VANDETA_LTO_JOBS", "4")
    if jobs not in ("", "0", "1"):
        env.Append(LINKFLAGS=[f"-flto={jobs}"])
