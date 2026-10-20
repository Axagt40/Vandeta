# Vandeta

**The only firmware you need for the LilyGo T-Embed CC1101.**

[![Release](https://img.shields.io/github/v/release/Axagt40/Vandeta?label=release&color=blue)](https://github.com/Axagt40/Vandeta/releases/latest)
[![Board](https://img.shields.io/badge/board-LilyGo%20T--Embed%20CC1101-orange)](https://lilygo.cc/products/t-embed-cc1101)
[![Base](https://img.shields.io/badge/base-Bruce%201.16.1-blueviolet)](https://github.com/BruceDevices/firmware)
[![Licence](https://img.shields.io/badge/licence-AGPL--3.0-green)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/dhPXgVerqD)

> **[Download Vandeta 1.1](https://github.com/Axagt40/Vandeta/releases/latest)** — one merged image, flash it at `0x0`
> · **[What's new in 1.1](https://github.com/Axagt40/Vandeta/releases/tag/vandeta-1.1)**
> · **[Discord](https://discord.gg/dhPXgVerqD)**

Vandeta is a **reskin of [Bruce](https://github.com/BruceDevices/firmware)** — the
open-source ESP32 offensive-security firmware — built on **Bruce 1.16.1** and
tuned for one board: the **LilyGo T-Embed CC1101**. The code underneath is
Bruce's code: same architecture, same menus, same tools, same licence.
Upstream credit and the AGPL-3.0 `LICENSE` are untouched, and the whole
upstream feature list applies here too (it is reproduced further down).

> **All credit for the firmware belongs to the Bruce project and its
> contributors.** Vandeta changes branding and adds the tooling listed below; it
> is not a fork that hides where it comes from.

---

## :sparkles: Why this exists

Bruce already runs on this board, but out of the box you have to be at the
device to use most of it: the screen is small, the encoder is fiddly, and the
interesting parts (capture, replay, scripted attacks, radio analysis) are hard to
watch or automate from a laptop.

Vandeta's goal is to be the **only firmware you need for the T-Embed CC1101**:
everything Bruce does, plus a full **USB console** that can drive and inspect the
device from a PC — screens, menus, files, radios, memory and JavaScript — with no
WiFi, no reflash and no extra hardware.

---

## :zap: What it adds on top of Bruce

* **Developer Options (USB console).** A persisted toggle that turns the USB
  serial link into a real control surface: `ui` / `move` / `press` drive the menus
  and print the screen you land on, `file` / `storage` / `md5` / `crc32` work on
  the card, `mem` / `proc` / `sys` report the board, `errors` / `log show` dump
  the error ring, `livepatch` reloads the JavaScript layer without a reflash and
  `hotreload` re-runs a script when it changes on the card. It also speaks a
  framed binary protocol for reliable transfers over the same cable.
* **Packet Monitor.** One dashboard with a real capture ring fed by WiFi, BLE,
  nRF24 and Sub-GHz, with filters, counters, CSV export and PCAP export.
* **Frequency Analyzer.** A Flipper Zero style analyser for the CC1101 —
  coarse hop, 20 kHz fine pass, held readout, hit log — drawn in the theme
  colours.
* **nRF24 raw capture.** `NRF24 > Raw Capture` listens in the pseudo-promiscuous
  configuration (CRC off, auto-ack off, shortest address width, noise addresses
  on every pipe), so a 2.4 GHz remote with an address the chip does not know can
  be captured at all: 3-second captures or a 126-channel hop, RGB-remote presets,
  channel scan, multi-frequency detection, packet analysis, save/load, raw
  replay — and the same engine from `nrfcap` and `nrf24.*` in JavaScript.
* **Universal IR remotes.** TV, AC, fans, audio, projectors and LED libraries
  from the Flipper IR database — 2,858 codes on the card. Pick a category, pick a
  button, and it sends the candidates in turn until the device reacts, exactly
  like the Flipper/Momentum flow, in Vandeta's own menus. The AC page also has
  **brand remotes** for twelve makes (Daikin, Gree, Midea, Panasonic, Samsung,
  Toshiba, TCL, Mitsubishi, Hitachi, Kelvinator, Whirlpool, Coolix): set power,
  mode, 16-30 °C and fan on screen and exactly that state frame is sent.
* **SubGHz Wardriver.** Continuous capture of whatever the CC1101 hears: whole
  bursts (never cut at a buffer seam), silent-noise rejection, a duplicate
  window, optional 433/315/868/915 hopping, and one `.sub` file per signal under
  `/bruce/subghz/wardriver`. Afterwards each capture can be replayed verbatim or
  identified once into a decoded KeeLoq/OOK file.
* **BLE Detector.** A live table of every advertiser in range — name, address,
  RSSI and estimated distance, strongest first, with Apple's Continuity
  types (AirDrop, AirPods, Watch, Find My, ...) decoded instead of shown as
  `<?>`. Receive-only. It is the newest screen here and may not work properly
  yet: distances are an estimate and rotating addresses can duplicate rows.
* **More radio work from JavaScript.** Raw nRF24 capture (including the five
  documented 2.4 GHz LED-controller protocols, channel-lock capture and a repeat
  finder), BLE advertisement capture/replay and a GATT client, plus
  `wifi.captureStart()`/`getCapturedPackets()` and packet decoding.
* **Attacks that work on protected networks.** ARP spoofing, DNS spoofing and
  HTTP interception/modification against WPA2 clients, with a captive-portal
  (evil-twin) engine and a headless, deadline-bounded WiFi attack set.
* **BadUSB, scriptable.** Layouts, VID/PID, manufacturer/product strings and
  payloads are settable from the console and from scripts, and the BLE
  keyboard's `Apple/BLE Patch` makes it appear to iOS and macOS.
* **A wider JavaScript API** (WiFi inject/capture/portal, BLE, nRF24, IR, Sub-GHz,
  display, storage, dialog, keyboard) with split scripts and background jobs.

Everything else — the menus, the apps, the IR/RFID/NFC tooling, the upstream
script ecosystem — is Bruce, unchanged.

---

## :vs: How Vandeta differs from Bruce

Vandeta is not a rewrite. It is Bruce 1.16.1 with a new face, a handful of fixes
and extra tools on top; every menu, app, script and file format Bruce has is
still here, and no C identifier, file name or console verb was renamed.

| | Bruce | Vandeta |
|---|---|---|
| Branding | Bruce | Vandeta boot screen and crown logo, `vandeta 1.1` version, web-UI titles, USB product `VANDETA` |
| Control from a PC | stock serial CLI | **Developer Options**: `ui` / `ui watch` / `move` / `press`, `file`, `mem` / `proc` / `sys` / `errors`, `livepatch`, `liveboot`, framed binary transfers |
| **Sub-GHz / CC1101 RF** | stock driver sharing the display's SPI, so scans can hang or reset | **fixed** — every CC1101 access is serialized against the display; scans, reads, spectrum and the wardriver run reliably |
| Radio tools | Bruce's scanner and spectrum | + Frequency Analyzer, SubGHz Wardriver, Packet Monitor (WiFi/BLE/nRF24/Sub-GHz), BLE Detector, BLE Capture & Replay |
| nRF24 | spectrum, MouseJack, jammer | + **Raw Capture**: pseudo-promiscuous listen, 126-channel hop, RGB presets, channel scan, multi-frequency detect, packet analysis, save/load, replay |
| IR | TV-B-Gone, custom codes | + Universal Remote: 2,858 library codes, RCA/NEC42 encoders and 12 brand AC remotes |
| WiFi | Bruce's attack set | + ARP/DNS/HTTP interception on WPA2, hardened evil-twin portal, headless attacks, one flat attack menu |
| BadUSB / HID | basic payloads | + `BadUSB Settings` (footprint, layouts incl. Finnish and custom, HID type, timing, payloads), `Apple/BLE Patch` |
| JavaScript | Bruce's interpreter | + script folders, background jobs, ARP/DNS/HTTP spoofing, WiFi inject/capture/portal, BLE GATT, nRF24 raw capture |
| Menus and UI | title only | + breadcrumb path, `n/total` counter, scrollbar, encoder hint bar, live capture screens, nested-menu guard |

### The RF fix, specifically

On the T-Embed CC1101 the CC1101 sits on the **same SPI bus as the display**.
Bruce's driver opens and closes an SPI transaction around every register access,
so when a radio read lands while the screen is mid-draw, `endTransaction()`
releases an SPI mutex owned by the *other* task: FreeRTOS asserts
(`xTaskPriorityDisinherit`) and the board reboots, or the screen stops updating.
That is why stock Bruce's Sub-GHz screens can hang or reset: the radio and the
board are fine, the driver is not.

Vandeta serializes every CC1101 access against the display with one recursive
lock (`rf_spi_lock()` in `src/modules/rf/rf_utils.cpp`) that holds the display's
bus for the whole radio operation, parks sweeps between hops and uses an
adaptive noise floor instead of a fixed threshold. The result: **Scan/Copy,
Read, Spectrum, Frequency Analyzer, Wardriver, the Sub-GHz Packet Monitor and
the `subghz` console verbs all work reliably on this board.**

### The full list of differences

Everything below is in this build and is not in stock Bruce (or is a fix to it).

<details>
<summary><b>USB console — Developer Options</b></summary>

* One `Developer Mode` switch (`Config > System Config > Developer Options`)
  arms the console and the speed profile together.
* `ui`, `ui watch`, `move up|down|left|right|nextpage`, `press ok|back` drive the
  menus and print the screen you land on — including the pages that build a
  local option list (Config, System Config, WiFi, Bluetooth, Files, Others).
* `file read|write|list|delete|stat`, `log show|clear|on|off`, `errors`,
  `help <command>`.
* `usbdebug status|proto|on|off`, hot reload and live update rows in the menu,
  and a framed binary protocol for reliable transfers over the same cable.
* `livepatch apply <module> <code>` reloads the JavaScript layer without a
  reflash; `liveboot enable|disable|status` runs a script at boot.
* `display ring on|off` and `display dump` inspect what a screen actually drew.
* The error ring captures ESP-IDF, Arduino `log_x()` and plain `printf()` /
  NimBLE lines, not just one of the three.
* No console command can be blocked by a modal banner, and turning Developer
  Mode off releases the console's queues, watches and RX buffer.

</details>

<details>
<summary><b>Sub-GHz / CC1101</b></summary>

* Shared-SPI serialization for every radio transaction (the RF fix above),
  sweep parking between hops and an adaptive noise floor.
* **Frequency Analyzer** — Flipper-style: coarse hop over the three CC1101
  bands, 20 kHz fine pass, big held frequency readout, last three hits, RSSI
  meter, and a log page with sort (`OK`) and scroll.
* **SubGHz Wardriver** — whole-burst capture (256 pulses, streamed straight to
  the card, never cut at a buffer seam), duplicate window, 433/315/868/915
  band hop, short-pulse noise rejection, a captures browser, verbatim replay
  and one-pass `Identify & save` to a `_dec.sub`.
* `subghz rx <timeout>` honours its argument (it used to parse the timeout as a
  frequency); `subghz wardriver capture|list|identify|identifyall|replay|status|selftest`.
* Replay waits for the train's own duration (capped at 30 s) instead of a fixed
  2 s, and a capture bigger than the replay buffer is refused instead of
  crashing.

</details>

<details>
<summary><b>nRF24 (2.4 GHz)</b></summary>

* **Raw Capture** page (`NRF24 > Raw Capture`): receive in the
  **pseudo-promiscuous** configuration — CRC off, auto-ack off, the shortest
  address width and degenerate noise addresses on all six pipes — so a remote
  with an unknown address can actually be captured (the stock 3-byte fixed
  address only ever saw remotes whose address it already knew).
* Capture 3 s on the current channel or **hop 0-125**, **RGB remote capture**
  with presets (MiLight RGBW, CCT, RGB+CCT, RGB, FUT020), set capture channel,
  frequency range, promiscuous on/off and data rate (1 Mbps / 2 Mbps /
  250 kbps).
* **Channel scan** (126 channels with an activity level), **Multi-freq detect**
  (dual-frequency remotes), **Frequency analysis**, capture list, save/load
  profiles, **Replay capture** and **Analyze packet** (length, unique bytes,
  bit transitions, byte range, repetition, code length, kind).
* Live progress screen with `Press BACK to stop`; only Back aborts.
* Console: `nrfcap` / `nrf`
  `info|init|promisc|channel|rate|power|freqrange|scan|capture|capch|lock|rgb|multifreq|profile|profiles|repeat|list|replay|save|load|analyze`.
* JavaScript: `nrf24.captureRaw`, `replayRaw`, channel scan, multi-frequency
  detection, signal analysis, save/load and the raw receive/send bindings.
* Capture buffers come from PSRAM: the seven 5 KB copies that used to sit in
  `.bss` cost the board the internal RAM WiFi and BLE need.

</details>

<details>
<summary><b>Packet Monitor</b></summary>

* One dashboard fed by **WiFi, BLE, nRF24 and Sub-GHz**, opened from each of
  those menus with its own radio preselected.
* Filters (radio, type, MAC-OUI, channel, text, only-new), counters per radio
  and per type, packets/sec, busiest channel, CSV export and PCAP export of the
  captured WiFi frames, pause, change source and clear.
* The WiFi and BLE taps actually capture on this board (they answered “No WiFi
  radio” / “No BLE radio” before).

</details>

<details>
<summary><b>BLE</b></summary>

* **BLE Detector** — live table of advertisers with Apple Continuity types
  decoded, distance estimate, paging, hold and clear.
* **Capture & Replay** with a live packet counter and Back-to-stop.
* JS GATT client (connect, services, characteristics, read/write/notify) and the
  capture/replay bindings, plus headless advertisement spam.
* A failed BLE bring-up no longer holds ~34 KB of internal RAM for the rest of
  the boot, the memory guard tests the real requirement, `stopBLEStack()` tears
  the controller down on every path, and WiFi hands BLE memory back — so a BLE
  feature started after another radio is no longer refused with “Low RAM”.

</details>

<details>
<summary><b>Infrared</b></summary>

* **Universal Remote** — the Flipper IR database's TV / AC / fans / audio /
  projectors / LEDs libraries (2,858 codes, shipped on the card under
  `/infrared/Universal/`), sweep-per-button with pause and cancel.
* **Brand AC remotes** for twelve makes, sending a full state frame (power,
  mode, 16–30 °C, fan).
* Hand-written encoders for RCA and NEC42, protocols the ESP32 IR library lacks.
* Button lists build in one pass, so a category opens immediately and the
  sweep's progress bar is sized before the first code is sent.
* TV-B-Gone and the universal sweep share one pause/cancel policy.
* `ir uni list|buttons|send`, `ir ac list|send`, `ir rx <timeout>`; GPI02 is
  claimed before use and the LED is switched off afterwards.

</details>

<details>
<summary><b>WiFi</b></summary>

* One flat `Wifi Atks` menu and one flat `Target Atks` menu — the duplicate
  nested pages and duplicate Beacon Flood entry are gone.
* ARP spoofing, DNS spoofing and HTTP interception/modification that reach
  WPA2/WPA3 clients (the frames go out on the encrypted station link).
* Evil-twin captive portal hardened: it refuses with a readable error instead of
  crashing when DMA memory is short.
* Headless, deadline-bounded attacks so scripts can run them; `moreatk` verb.

</details>

<details>
<summary><b>BadUSB / HID</b></summary>

* `BadUSB Settings` menu: USB footprint (VID, PID, manufacturer, product,
  serial), keyboard layout (including Finnish and custom layout import/export),
  HID device type, attack timing, payload manager, show output.
* A character a layout cannot type no longer abandons the rest of the payload.
* `Apple/BLE Patch` and `BadBLE Name` in the same menu, so the BLE keyboard is
  listed by iOS and macOS and pairs Just Works.
* `badusb config|set|layout-import|layout-export|layout-copy|mouse|click`.

</details>

<details>
<summary><b>JavaScript API</b></summary>

* `arpSpoof`, `dnsSpoof` and `httpInterceptor` for the MITM engines.
* `wifi.injectPacket`, `sendRaw80211`, `captureStart`, `captureStop`,
  `getCapturedPackets`, `packetInfo`, `injectDns`, `injectHttp`,
  `injectHttpRedirect`, `injectHtml`, `portal`, `setPromiscuous`.
* BLE GATT client plus advertisement capture/replay and headless spam.
* `nrf24.captureRaw`, `replayRaw`, channel scan, multi-frequency detect, signal
  analysis, save/load, `receive`/`send` and the radio settings.
* `scriptFolder.create|list|load|run|close|isRunning|getAllScripts|stop|kill`,
  with `setShared`/`getShared` for passing state into a background job.
* `ir.transmitRaw`, `subghz.scan`, `storage.exists|size|copy`,
  `serial.available|read`.
* Bindings hardened against the interpreter's moving GC, so building containers
  in a script can no longer corrupt the heap.

</details>

<details>
<summary><b>Files, storage and settings</b></summary>

* `md5`, `crc32`, `cat` and `storage read` stream in 512-byte chunks — any file
  size, no 3 KB ceiling, no wedged console.
* `file read|write|list|delete|stat` with paths resolved against the console's
  working directory.
* New persisted settings in `bruce.conf`: the Developer Mode switch, the
  protocol preference, the whole BadUSB footprint and the Wardriver frequency,
  duplicate window, minimum pulse count and band hop — all validated on load.
* Renames migrate stored values (`JS Interpreter` → `Apps` stays hidden if it
  was hidden before).

</details>

<details>
<summary><b>Menus and navigation</b></summary>

* Breadcrumb path (`WiFi > Wifi Atks > Target Atks`), `n/total` counter and a
  scrollbar on long lists, plus an encoder hint bar (`TURN` / `SEL` / `BACK`).
* A child menu no longer overwrites its parent's rows — the “USB Footprint does
  nothing / Bad BLE does not open / the page flashes straight back” class of
  bugs.
* Back returns to the parent menu instead of redrawing the page you left.
* Capture screens show live progress and `Press BACK to stop`; only Back aborts,
  and presses queued during the capture are dropped before the result screen.
* `JS Interpreter` is now **Apps** with an app-drawer icon, and `BadUSB Config`
  is now **BadUSB Settings**.
* `ui` / `nav` readback reports the menu that is actually on screen, not the one
  you left.

</details>

<details>
<summary><b>Board support and memory</b></summary>

* The CC1101 and the display share one SPI bus; a `TFT_eSPI` bus lock
  (`busLock()` / `busUnlock()`) lets the RF layer take the bus for a whole radio
  operation.
* nRF24 capture buffers live in PSRAM instead of `.bss` (≈36 KB of internal DRAM
  returned), and every console/JS path shares one allocator.
* WiFi brings its radio up through the raw IDF calls and releases the driver on
  exit, so leaving a tap no longer strands ~60 KB.
* `ensureMemForWifi()` releases BLE, the web UI and stale captures before a WiFi
  feature gives up, and `radioHasMemForBle()` checks the real ~56 KB
  requirement instead of one contiguous block.

</details>

<details>
<summary><b>Fixes that apply everywhere</b></summary>

* `USBSerial::vprintf()` no longer prints garbage for `%d` / `%s`.
* The TFT screen-stream task no longer races a fast `display stop` / `start`
  into a reboot, and `display dump` no longer overflows the serial task's stack.
* NimBLE's asynchronous scan start is handled, so BLE capture and the dashboard
  do not cancel themselves.
* `i2c scan` takes its argument and prints one device per line; `proc` shows
  `any` for no-affinity tasks; the CC1101 driver no longer prints `setMHZ` /
  `SetTx` / `SetRx` chatter on the console.
* A read-only console command can no longer wedge the console on a blocking
  banner.

</details>

<details>
<summary><b>Branding and build</b></summary>

* Boot screen reads **Vandeta** with the crown drawn in your UI colour; the
  version string is `vandeta 1.1` on the splash, in `info` and in the web UI.
* USB product id `VANDETA`; web-UI titles are Vandeta's.
* Default `platformio.ini` env is `lilygo-t-embed-cc1101`; `build.py` merges
  bootloader + partitions + app into `vandeta.bin` for a single flash at `0x0`.
* The universal IR libraries ship in `sd_files/infrared/Universal/` and on the
  card under `/infrared/Universal/`.
* No C identifier, file name or console verb was renamed — `/BruceJS`, the
  `bruce` verbs, the BruceNet AP and the upstream menus stay as they are — and
  AGPL-3.0 plus upstream credit are unchanged.

</details>

---

## :electric_plug: Hardware

| | |
|---|---|
| Board | **LilyGo T-Embed CC1101** ("T-Embed C1011 plus") |
| MCU | ESP32-S3, 16 MB flash, 8 MB PSRAM |
| Display | 320x170 landscape, encoder navigation |
| Radios | CC1101 Sub-GHz, nRF24 socket, WiFi, BLE |
| Notes | Runs on battery; the CC1101 and nRF24 are not simultaneous |

---

## :building_construction: Installing

The image is **pre-merged** (bootloader + partition table + app), so it is one
`write_flash` at offset **`0x0`**. Download it from the
[latest release](https://github.com/Axagt40/Vandeta/releases/latest) —
`vandeta.bin` — together with `vandeta.bin.sha256`.

```sh
# 1. verify the download
sha256sum -c vandeta.bin.sha256

# 2. flash it (board in console mode: lsusb shows 303a:1001)
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
    write_flash 0x0 vandeta.bin
```

* **Wrong offset bricks the boot** — the merged image always goes to `0x0`.
* Windows: replace the port with `COM5` (or whatever Device Manager shows).
* No `/dev/ttyACM0` on Linux, but `lsusb` shows `303a:1001`? The `cdc_acm`
  driver is not loaded: `sudo modprobe cdc_acm`.
* `303a:0002` instead of `303a:1001` means the board is in USB Mass Storage mode.
  Leave it on the device before flashing.
* Clean install (wipes saved settings): `esptool.py ... erase_flash` first.
* Flashing this image does **not** touch the SD card.

The boot screen is drawn in your UI colour (Settings → Display & UI → UI Colour),
so it matches the rest of the theme.

---

## :keyboard: Using it from a PC

Turn on **Config → System Config → Developer Options**, then talk to the board
over the same USB cable:

```sh
# what is on the screen right now
ui
# press a key and read the screen you land on
ui down
ui select
# card and system
ls /                   storage free sd        mem        sys
file read /bruce.conf  md5 /bruce.conf         errors     proc
# script layer, live
js run /BruceJS/yourscript.js
livepatch apply hello console.log('hi')
```

The console boots in plain text and only switches modes when asked, so a
mis-typed command can never leave the board in an unusable state.

---

## :hammer_and_wrench: Building from source

PlatformIO Core 6.1.19+ and python3; the first build needs network access to
resolve `lib_deps` (Arduino-ESP32 3.3.9, pioarduino platform 55.03.39,
toolchain-xtensa-esp-elf 14.2.0), cached under `.pio/` afterwards.

```sh
pio run -e lilygo-t-embed-cc1101
```

This produces `vandeta-lilygo-t-embed-cc1101.bin` and a merged copy named
`vandeta.bin` in the project root (`build.py` runs `esptool merge-bin` as a
post-action). `BRUCE_VERSION` is a `build_flags` define, so changing it
invalidates the build cache — expect a 15-25 minute rebuild.

---

## :speech_balloon: Community

Questions, builds and feedback: **Discord** — <https://discord.gg/dhPXgVerqD>

---

## :clap: Credits and licence

* **Bruce** — <https://github.com/BruceDevices/firmware> (upstream:
  <https://github.com/pr3y/Bruce>). Vandeta is a reskin of Bruce 1.16.1; the
  firmware is Bruce's work and the whole upstream feature list, device support
  and documentation further down this file are its authors'.
* Licence: **AGPL-3.0** (`LICENSE`), third-party notices in `THIRD_PARTY.md`.
  Both are unchanged from upstream and both apply to this build.

---

## :bookmark_tabs: About the upstream project

Everything below this line is the upstream Bruce project — feature list,
supported devices, credits and license — kept intact on purpose.

---

---

# :shark: Bruce

Bruce is a versatile ESP32 firmware packed with offensive-security tools, built to make Red Team operations fast and portable.

It also supports [M5Stack](https://shop.m5stack.com), [LILYGO](https://lilygo.cc) , [RockBase IoT](https://www.rockbaseiot.com) and [Elecrow](https://www.elecrow.com) products, and works great with the Cardputer, Sticks, M5Cores, T-Decks and T-Embeds.

## :zap: Get Our Official DevKit!

### RF REAPER

**RF REAPER** is our custom PCB devkit, purpose-built for Bruce!

Every major feature works natively, right out of the box. Sub-GHz, NFC/RFID, IR, 2.4GHz(NRF), GPS-ready, and a microSD, all driven by a beefy ESP32-S3 (16MB Flash / 8MB PSRAM). Tons of GPIOs via the AW9523 expander plus Flipper Zero & iButton header compatibility mean you can hack, mod, and build on it endlessly. Want a specific function? Ask us with an issue, we'll check it.

👉 **[Buy the RF REAPER and official boards](https://shop.bruce.computer)**

**Check our fully open-source hardware too:** https://bruce.computer/boards

More custom devkit boards coming soon! Stay across our communities!

## :building_construction: How to install

### The easiest way to install Bruce is using our official Web Flasher!

### Check out: https://bruce.computer/flasher

Alternatively, you can download the latest binary from releases or actions and flash locally using esptool.py

```sh
esptool.py --port /dev/ttyACM0 write_flash 0x00000 Bruce-<device>.bin
```

**For m5stack devices**

If you already use M5Launcher to manage your m5stack device, you can install it with OTA

Or you can burn it directly from the [m5burner tool](https://docs.m5stack.com/en/download), just search for 'Bruce' (My official builds will be uploaded by "owner" and have photos.) on the device category you want to and click on burn

## :keyboard: Discord Server

Contact us in our [Discord Server](https://discord.gg/WJ9XF9czVT)!

## :bookmark_tabs: Wiki

For more information on each function supported by Bruce, [read our wiki here](https://wiki.bruce.computer/).
Also, [read our FAQ](https://wiki.bruce.computer/faq/)

## :computer: List of Features

<details>
  <summary><h2>WiFi</h2></summary>

- [x] Connect to WiFi
- [x] WiFi AP
- [x] Disconnect WiFi
- [x] [WiFi Atks](https://wiki.bruce.computer/features/wifi/#wifi-atks)
  - [x] [Beacon Spam](https://wiki.bruce.computer/features/wifi/#beacon-spam)
  - [x] [Target Atk](https://wiki.bruce.computer/features/wifi/#target-atks)
    - [x] Information
    - [x] Target Deauth
    - [x] EvilPortal + Deauth
  - [x] Deauth Flood (More than one target)
  - [x] Additional attacks (in the flat **Wifi Atks** menu)
    - [x] EAPOL Logoff
    - [x] Channel Switch
    - [x] Bad Message
    - [x] Auth Flood
    - [x] Probe Request Flood
    - [x] Authentication Attack
    - [x] EAPOL Start Attack
    - [x] Disassociation Attack
- [x] [Wardriving](https://wiki.bruce.computer/features/gps/#wardriving)
- [x] [TelNet](https://wiki.bruce.computer/features/wifi/#telnet)
- [x] [SSH](https://wiki.bruce.computer/features/wifi/#ssh)
- [x] [RAW Sniffer](https://wiki.bruce.computer/features/wifi/#raw-sniffer)
- [x] [TCP Client](https://wiki.bruce.computer/features/wifi/#client-tcp)
- [x] [TCP Listener](https://wiki.bruce.computer/features/wifi/#listen-tcp)
- [x] [Evil Portal](https://wiki.bruce.computer/features/wifi/#evil-portal)
- [x] [Scan Hosts](https://wiki.bruce.computer/features/wifi/#scan-hosts) (with TCP Port scanning)
- [x] [Responder](https://wiki.bruce.computer/features/wifi/#responder)
- [x] [Arp Spoofing](https://wiki.bruce.computer/features/wifi/#arp-spoofing)
- [x] [Arp Poisoning](https://wiki.bruce.computer/features/wifi/#arp-poisoning)
- [x] [Wireguard Tunneling](https://wiki.bruce.computer/features/wifi/#wireguard-tunneling)
- [x] Brucegotchi
  - [x] Pwnagotchi friend
  - [x] Pwngrid spam faces & names
    - [x] [Optional] DoScreen a very long name and face
    - [x] [Optional] Flood uniq peer identifiers

</details>

<details>
  <summary><h2>BLE</h2></summary>

- [x] [BLE Scan](https://wiki.bruce.computer/features/ble/#ble-scan)
- [x] Bad BLE - Run Ducky scripts, similar to [BadUsb](https://wiki.bruce.computer/features/ble/#badble)
- [x] BLE Keyboard - Cardputer and T-Deck Only
- [x] iOS Spam
- [x] Windows Spam
- [x] Samsung Spam
- [x] Capture & Replay (record, filter, analyse, save/load and replay raw advertisements, with per-vendor remote types)
- [x] Android Spam
- [x] Spam All
</details>

<details>
  <summary><h2>RF</h2></summary>

- [x] Scan/Copy
- [x] [Custom SubGhz](https://wiki.bruce.computer/features/rf/#replay-payloads-like-flipper)
- [x] Spectrum
- [x] Jammer Full (sends a full squared wave into output)
- [x] Jammer Intermittent (sends PWM signal into output)
- [x] Config
  - [x] RF TX Pin
  - [x] RF RX Pin
  - [x] RF Module
    - [x] RF433 T/R M5Stack
    - [x] [CC1101 (Sub-Ghz)](https://wiki.bruce.computer/features/rf/#cc1101)
  - [x] RF Frequency
- [x] Replay
</details>

<details>
  <summary><h2>RFID</h2></summary>

- [x] Read tag
- [x] Read 125kHz
- [x] Clone tag
- [x] Write NDEF records
- [x] Amiibolink
- [x] Chameleon
- [x] Write data
- [x] Erase data
- [x] Save file
- [x] Load file
- [x] Config
  - [x] [RFID Module](https://wiki.bruce.computer/features/rfid/#supported-modules)
    - [x] PN532
    - [x] PN532Killer
- [ ] Emulate tag
</details>

<details>
  <summary><h2>IR</h2></summary>

- [x] TV-B-Gone
- [x] IR Receiver
- [x] [Custom IR (NEC, NECext, SIRC, SIRC15, SIRC20, Samsung32, RC5, RC5X, RC6)](https://wiki.bruce.computer/features/ir/#replay-payloads-like-flipper)
- [x] Config - [X] Ir TX Pin - [X] Ir RX Pin
</details>

<details>
  <summary><h2>FM</h2></summary>

- [x] [Broadcast standard](https://wiki.bruce.computer/features/fm/#broadcast-standard)
- [x] [Broadcast reserved](https://wiki.bruce.computer/features/fm/#broadcast-standard)
- [x] [Broadcast stop](https://wiki.bruce.computer/features/fm/#broadcast-stop)
- [ ] [FM Spectrum](https://wiki.bruce.computer/features/fm/#fm-spectrum)
- [ ] [Hijack Traffic Announcements](https://wiki.bruce.computer/features/fm/#hijack-ta)
- [ ] [Config](https://wiki.bruce.computer/features/fm/#bookmark_tabs-config)
</details>

<details>
  <summary><h2>NRF24</h2></summary>

- [x] [NRF24 Jammer](https://wiki.bruce.computer/features/nrf24/)
- [x] 2.4G Spectrum
- [x] Raw Capture (pseudo-promiscuous listen, channel scan, multi-frequency detect, analyse, save/load, replay)
- [ ] Mousejack
</details>

<details>
  <summary><h2>Apps</h2></summary>

- [x] [JavaScript Interpreter](https://wiki.bruce.computer/features/js-interpreter/) [Credits to justinknight93](https://github.com/justinknight93/Doolittle)
</details>

<details>
  <summary><h2>Others</h2></summary>

- [x] Mic Spectrum
- [x] [QRCodes](https://wiki.bruce.computer/features/others/#qrcodes)
  - [x] Custom
  - [x] PIX (Brazil bank transfer system)
- [x] [SD Card Mngr](https://github.com/pr3y/Bruce/wiki/Others#sd-card-mngr)
  - [x] View image (jpg)
  - [x] File Info
  - [x] [Wigle Upload](https://wiki.bruce.computer/features/gps/#how-to-use-wigle)
  - [x] Play Audio
  - [x] View File
- [x] LittleFS Mngr
- [x] [WebUI](https://wiki.bruce.computer/controlling-device/webui/)
  - [x] Server Structure
  - [x] Html
  - [x] SDCard Mngr
  - [x] Spiffs Mngr
- [x] Megalodon
- [x] [BADUsb (New features, LittleFS and SDCard)](https://wiki.bruce.computer/features/others/#badusb)
  - [x] BadUSB config menu: USB footprint (VID/PID/manufacturer/product/serial), keyboard layout, HID device type, attack speed and timing, payload folder
  - [x] Finland (fi-FI) layout, plus import/export/copy of custom layout files
- [x] USB Keyboard - Cardputer and T-Deck Only
- [x] [iButton](https://wiki.bruce.computer/features/others/#ibutton)
- [x] LED Control
</details>

<details>
  <summary><h2>Clock</h2></summary>

- [x] RTC Support
- [x] NTP time adjust
- [x] Manual adjust
</details>

<details>
  <summary><h2>Connect (ESPNOW)</h2></summary>

- [x] Send File
- [x] Receive File
- [x] Send Commands
- [x] Receive Commands
</details>

<details>
  <summary><h2>Config</h2></summary>

- [x] Brightness
- [x] Dim Time
- [x] Orientation
- [x] UI Color
- [x] Boot Sound on/off
- [x] Clock
- [x] Sleep
- [x] Restart
</details>

## Specific functions per Device, the ones not mentioned here are available to all.

| Device                                                                                                                                                                                      | CC1101 | NRF24 | FM Radio |        PN532         | Mic  | BadUSB | RGB Led | Speaker | Fuel Gauge | LITE_VERSION |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :----: | :---: | :------: | :------------------: | :--: | :----: | :-----: | :-----: | :--------: | :----------: |
| [M5Stack Cardputer](https://shop.m5stack.com/products/m5stack-cardputer-kit-w-m5stamps) (and ADV)                                                                                           |  :ok:  | :ok:  |   :ok:   |         :ok:         | :ok: |  :ok:  |  :ok:   | NS4168  |    :x:     |     :x:      |
| [M5Stack M5StickC PLUS2](https://shop.m5stack.com/products/m5stickc-plus2-esp32-mini-iot-development-kit)                                                                                   |  :ok:  | :ok:  |   :ok:   |         :ok:         | :ok: | :ok:¹  |   :x:   |  Tone   |    :x:     |     :x:      |
| [M5Stack M5StickC PLUS](https://shop.m5stack.com/products/m5stickc-plus-esp32-pico-mini-iot-development-kit)                                                                                |  :ok:  | :ok:  |   :ok:   |         :ok:         | :ok: | :ok:¹  |   :x:   |  Tone   |    :x:     |     :x:²     |
| [M5Stack M5Core BASIC](https://shop.m5stack.com/products/basic-core-iot-development-kit)                                                                                                    |  :ok:  | :ok:  |   :ok:   |         :ok:         | :ok: | :ok:¹  |   :x:   |  Tone   |    :x:     |     :x:      |
| [M5Stack M5Core2](https://shop.m5stack.com/products/m5stack-core2-esp32-iot-development-kit-v1-1)                                                                                           |  :ok:  | :ok:  |   :ok:   |         :ok:         | :ok: | :ok:¹  |   :x:   |   :x:   |    :x:     |     :x:      |
| [M5Stack M5CoreS3](https://shop.m5stack.com/products/m5stack-cores3-esp32s3-lotdevelopment-kit)/[SE](https://shop.m5stack.com/products/m5stack-cores3-se-iot-controller-w-o-battery-bottom) |  :ok:  | :ok:  |   :ok:   |         :ok:         | :x:  |  :ok:  |   :x:   |   :x:   |    :x:     |     :x:      |
| [JCZN CYD&#x2011;2432S028](https://www.aliexpress.us/item/3256804774970998.html)                                                                                                            |  :ok:  | :ok:  |   :ok:   |         :ok:         | :x:  | :ok:¹  |   :x:   |   :x:   |    :x:     |     :x:²     |
| [Lilygo T&#x2011;Embed CC1101](https://lilygo.cc/products/t-embed-cc1101)                                                                                                                   |  :ok:  | :ok:  |   :ok:   |         :ok:         | :ok: |  :ok:  |  :ok:   |  :ok:   |    :ok:    |     :x:      |
| [Lilygo T&#x2011;Embed](https://lilygo.cc/products/t-embed)                                                                                                                                 |  :ok:  | :ok:  |   :ok:   |         :ok:         | :ok: |  :ok:  |  :ok:   |  :ok:   |    :x:     |     :x:      |
| [Lilygo T-Display-S3](https://lilygo.cc/products/t-display-s3)                                                                                                                              |  :ok:  | :ok:  |   :x:    |         :x:          | :x:  |  :ok:  |   :x:   |   :x:   |    :x:     |     :x:      |
| [Lilygo T&#x2011;Deck](https://lilygo.cc/products/t-deck) ([and pro](https://lilygo.cc/products/t-deck-plus-1))                                                                             |  :ok:  |  :x:  |   :x:    |         :x:          | :x:  |  :ok:  |   :x:   |   :x:   |    :x:     |     :x:      |
| [Lilygo T-Watch-S3](https://lilygo.cc/products/t-watch-s3)                                                                                                                                  |  :x:   |  :x:  |   :x:    |         :x:          | :x:  |  :ok:  |   :x:   |   :x:   |    :x:     |     :x:      |
| [Lilygo T-LoRa Pager](https://lilygo.cc/products/t-lora-pager)                                                                                                                              |  :x:   |  :x:  |   :x:    |         :x:          | :x:  |  :ok:  |   :x:   |   :x:   |    :x:     |     :x:      |
| [Smoochiee V2](https://www.pcbway.com/project/shareproject/Bruce_PCB_Smoochiee_d6a0284b.html)                                                                                               |  :ok:  | :ok:  |   :x:    |         :ok:         | :x:  |  :ok:  |   :x:   |   :x:   |    :x:     |     :x:      |
| [ESP32-C5](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c5/esp32-c5-devkitc-1/user_guide.html)                                                                           |  :ok:  | :ok:  |   :x:    |         :ok:         | :x:  |  :x:   |   :x:   |   :x:   |    :x:     |     :x:      |
| [Bruce RF Reaper](https://www.elecrow.com/bruce-pcb-rf-reaper.html)                                                                                                                         |  :ok:  | :ok:  |   :x:    | :ok: but w/ ST25R3916 | :x:  |  :ok:  |  :ok:   |   :x:   |    :ok:    |     :x:      |
| [Elecrow 24B](https://www.elecrow.com/2-4inch-esp32-miner-lcd-display-2pcs-cryptocurrency-solo-miner-with-1000kh-s-hashrate.html)                                                            |  :ok:  | :ok:  |   :ok:   |         :ok:         | :x:  | :ok:¹  |   :x:   |   :x:   |    :x:     |     :x:²     |
| [Elecrow 3.5"](https://www.elecrow.com/esp-terminal-with-esp32-3-5-inch-parallel-480x320-tft-capacitive-touch-display-rgb-by-chip-ili9488.html)                                                                                                                                                        |  :ok:  | :ok:  |   :ok:   |         :ok:         | :x:  | :ok:¹  |   :x:   |   :x:   |    :x:     |     :x:²     |
| [NM-CYD-C5 + RF HAT](https://https://rockbase.shop/products/nm-cyd-c5-colorful)                                                                                                                         |  :ok:  | :ok:  |   :x:    | :ok: | :x:  |  :ok:  |  :ok:   |   :x:   |    :ok:    |     :x:      |
² CYD have a LITE_VERSION version for Launcher Compatibility
¹ Core, CYD and StickCs Bad-USB: [here](https://wiki.bruce.computer/features/others/#badusb)

_LITE_VERSION_: TelNet, SSH, WireGuard, ScanHosts, RawSniffer, Brucegotchi, BLEBacon, BLEScan and Interpreter are NOT available for M5Launcher Compatibility

## :sparkles: Why and how does it look?

Bruce stems from a keen observation within the community focused on devices like Flipper Zero. While these devices offered a glimpse into the world of offensive security, there was a palpable sense that something more could be achieved without being that overpriced, particularly with the robust and modular hardware ecosystem provided by ESP32 Devices, Lilygo and M5Stack products.

![Bruce Main Menu](./media/pictures/pic1.png)
![Bruce on M5Core](./media/pictures/core.png)
![Bruce on Stick](./media/pictures/stick.png)
![Bruce on CYD](./media/pictures/cyd.png)
![Bruce on CYD with NM-RF-HAT](./media/pictures/bruce-cyd.png)

Other media can be [found here](./media/).

## :clap: Acknowledgements

- [@bmorcelli](https://github.com/bmorcelli) for new core and a bunch of new features, also porting to many devices!
- [@IncursioHack](https://github.com/IncursioHack) for adding RF and RFID modules features.
- [@Luidiblu](https://github.com/Luidiblu) for logo and UI design assistance.
- [@eadmaster](https://github.com/eadmaster) for adding a lot of features.
- [@rennancockles](https://github.com/rennancockles) for a lot of RFID code, refactoring and others features.
- [@7h30th3r0n3](https://github.com/7h30th3r0n3) refactoring and a lot of help with WiFi attacks.
- [@Tawank](https://github.com/Tawank) refactoring interpreter among many other things
- @pablonymous RF functions to read RAW Data
- [Smoochiee](https://github.com/smoochiee) for Bruce PCB design.
- TH3_KR4K3N for Stick cplus extender PCB design.
- Everyone who contributed in some way to the project, thanks :heart:

Bruce also stands on the shoulders of other great open-source firmware projects,
which inspired features and code across the project:

- [ESP32Marauder](https://github.com/justcallmekoko/ESP32Marauder) by [@justcallmekoko](https://github.com/justcallmekoko) — WiFi/Bluetooth offensive toolkit.
- [Launcher](https://github.com/bmorcelli/Launcher) by [@bmorcelli](https://github.com/bmorcelli) — the multi-app launcher/bootloader for the devices.
- [Evil-M5Project](https://github.com/7h30th3r0n3/Evil-M5Project) by [@7h30th3r0n3](https://github.com/7h30th3r0n3) — WiFi attack suite for M5Stack.
- [M5Stick-Nemo](https://github.com/n0xa/m5stick-nemo) by [@n0xa](https://github.com/n0xa) — multi-tool firmware for M5Stick devices.

Bruce builds on many free-software libraries, and parts of the RF and NFC/RFID
modules are derived from other projects. See [THIRD_PARTY.md](./THIRD_PARTY.md)
for third-party attribution and copyleft-compliance details.

## :construction: Disclaimer

Bruce is a tool for cyber offensive and red team operations, distributed under the terms of the Affero General Public License (AGPL). It is intended for legal and authorized security testing purposes only. Use of this software for any malicious or unauthorized activities is strictly prohibited. By downloading, installing, or using Bruce, you agree to comply with all applicable laws and regulations. This software is provided free of charge, and we do not accept payments for copies or modifications. The developers of Bruce assume no liability for any misuse of the software. Use at your own risk.
