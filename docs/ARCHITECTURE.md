# Architecture

One board, one image. This file is the map: where code lives, what runs on which
task, and the invariant that decides most bugs on the T-Embed CC1101 Plus.

## Module map

```
src/
├── main.cpp               setup(), the main loop task, the menu dispatch
├── core/
│   ├── display.{h,cpp}    TFT, the menu renderer, loopOptions()
│   ├── menu_items/        one file per top-level menu (WiFi, BLE, RF, ...)
│   ├── packet_view.cpp    the Packet Monitor dashboard and its four taps
│   ├── serial_commands/   the USB console: cli.cpp dispatches to one file per area
│   ├── dev/               tunables table and the A/B patch overlay
│   ├── tftLogger/         the screen stream the mirror and the Bruce App read
│   ├── usb_debug/         console plumbing and the error ring
│   └── wifi/              STA/AP bring-up shared by every WiFi feature
└── modules/
    ├── rf/                CC1101: scan, spectrum, analyzer, wardriver, bit editor
    ├── NRF24/             raw capture, jamming, census
    ├── ble/               detector, capture/replay, attacks, GATT client
    ├── ble_api/           the BLE services that expose the device
    ├── ir/                universal remote, IR Suite, brand AC, RCA/NEC42
    ├── wifi/              attacks, MITM engine, portal, interception
    ├── rfid/              PN532, 125 kHz driver, MIFARE, SRIX
    ├── bjs_interpreter/   the JavaScript bindings
    └── ...                gps, lora, fm, ethernet, badusb_ble, others
```

## Tasks and queues

`usbdebug status` prints the live truth; the layout below is what it reports:

| Task | Owns |
|------|------|
| main loop | the UI: menus, drawing, key handling |
| `serialcmds` | the console: reads a line, dispatches, answers |
| js task | the interpreter, including background scripts |
| `async_serial` (`tftLogger`) | the screen stream, so a busy screen never blocks the loop |
| `usbEthernet` | the USB Ethernet bridge's RX path |
| sound cue task | the short beeps, so a cue never stalls a redraw |

Queues exist where a producer must not block the consumer: the console's command
queue, the tftLogger write queue, the BLE capture queue and the packet monitor's
tap ring. A new feature should never write to the display from another task - it
posts to the loop instead.

## The invariant: one SPI bus

The CC1101 and the display share SPI on this board. Any radio transaction that
overlaps a screen write comes back corrupted, which is why stock Bruce's scans,
sweeps and captures drop out here.

`src/modules/rf/rf_utils.cpp` holds `rf_spi_lock()` / `rf_spi_unlock()`: a
recursive mutex plus `TFT_eSPI::busLock()`, taken around a **whole** radio
operation, not per register write. Any code that touches the CC1101 must hold it
for the duration; sweeps park between hops rather than holding it for the whole
scan. If you add a radio path and it "works but sometimes returns noise", you
are missing the lock.

The nRF24 is a separate radio on the same bus: it takes the same lock, and it
is not usable at the same time as the CC1101.

## Memory guards

Internal DRAM is the scarce resource (the radios need DMA-capable memory), so
every radio bring-up is guarded:

- Each tap (WiFi, BLE, nRF24, sub-GHz) checks the internal DRAM and DMA it needs
  before starting, releases the other radio first, and fails with a readable
  message instead of panicking.
- nRF24 capture buffers are allocated from **PSRAM**, which keeps roughly 36 KB
  of internal DRAM for WiFi and BLE.
- Starting a WiFi feature frees BLE, the web UI and stale captures first.
- `mem` is the check: internal free and DMA free before and after your change.

## Where a new feature hooks in

| You want to add | Touch |
|-----------------|-------|
| A menu row | the matching `src/core/menu_items/*.cpp`, plus its module |
| A console verb | `src/core/serial_commands/cli.cpp` (the dispatch table) and one `*_commands.cpp` |
| A tuning value | `src/core/dev/tunables.cpp` (one table, `TUNE_LIVE`/`TUNE_REBOOT`) |
| A script binding | `src/modules/bjs_interpreter/` |
| A packet source | `src/core/packet_view.cpp` (a new `PKT_SRC_*` tap) |
| A screen | `src/core/display.cpp` conventions: breadcrumb, `n/total`, encoder hint |

## Conventions worth keeping

- Menus publish their local option list so `ui` and `ui watch` can read the
  screen that is actually rendered (`activeOptions` in `display.cpp`).
- Settings persist in `bruce.conf` and are validated on load; a hand-edited or
  truncated config must never break a boot.
- Long operations are chunked (512-byte file reads, streamed captures), so a
  large file cannot exhaust the heap.
- Firmware internals keep Bruce's names (`/BruceJS`, the `bruce` verbs, the
  BruceNet AP) to keep the upstream diff readable.
