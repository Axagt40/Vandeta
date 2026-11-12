# Building Vandeta

Everything here produces `vandeta.bin`, the single merged image the releases and
the browser flasher install. It is one board and one environment; there is no
board matrix to configure.

## Toolchain

| Component | Version |
|-----------|---------|
| PlatformIO Core | 6.1.19 or newer |
| Platform | `pioarduino/platform-espressif32` 55.03.39 |
| Arduino-ESP32 | 3.3.9 |
| Toolchain | `toolchain-xtensa-esp-elf` 14.2.0 |
| Python | 3.x, with the pioarduino dependencies |

The first build resolves `lib_deps` over the network; after that everything is
cached under `.pio/`.

## Build

```sh
git clone https://github.com/Axagt40/Vandeta.git
cd Vandeta
pio run -e lilygo-t-embed-cc1101
```

Two images come out:

| File | What it is |
|------|------------|
| `.pio/build/lilygo-t-embed-cc1101/firmware.bin` | the application only |
| `vandeta.bin` in the project root | bootloader + partition table + app, merged |

`build.py` runs `esptool merge-bin` as a post-action, which is what produces the
merged file in the root. Nothing else needs to be done by hand.

`BRUCE_VERSION` is a `build_flags` define, so changing it invalidates the cache
and the next build is a full 15-25 minute one.

## Flash

The merged image is written at **`0x0`** - never another offset:

```sh
sha256sum -c vandeta.bin.sha256
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 115200 \
    write_flash 0x0 vandeta.bin
```

Keep **115200**. The board's USB-Serial-JTAG link sets the real speed; a write at
`921600` stopped part-way during testing. An app-only image can be pushed over
USB with `devflash` from the console, but a release is always the merged file at
`0x0`.

The board appears as `303a:1001` (console mode) and needs `303a:0002` left alone
(USB Mass Storage mode). On Linux, no `/dev/ttyACM0` with `303a:1001` visible
means `sudo modprobe cdc_acm`.

## Where the site is built from

The flasher and the documentation are plain static files in `site/`, published
by `.github/workflows/pages.yml`. They are not generated; edit them directly.

## Version string

`version.txt` carries the version, and the compiler sees it as `BRUCE_VERSION`
through `platformio.ini` and `build_id.py`. A pre-release is spelled
`1.4-pre` in the tag and `1.4 pre` in prose.
