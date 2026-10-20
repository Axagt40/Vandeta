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

Vandeta is custom firmware for one board, the **LilyGo T-Embed CC1101**, built
on **[Bruce](https://github.com/BruceDevices/firmware) 1.16.1** — the
open-source ESP32 offensive-security firmware. It keeps Bruce's architecture,
menus and tools as its foundation and adds the tooling listed below on top of
them. Upstream credit and the AGPL-3.0 `LICENSE` are untouched, and the whole
upstream feature list still applies here (it is reproduced further down).

> **All credit for the firmware belongs to the Bruce project and its
> contributors.** Vandeta would not exist without it.

---

## :sparkles: Why this exists

Vandeta's goal is to be the **only firmware you need for the T-Embed CC1101**:
everything Bruce does, plus a full **USB console** that can drive and inspect the
device from a PC — screens, menus, files, radios, memory and JavaScript — with no
WiFi, no reflash and no extra hardware.

---

## :computer: Vandeta feature list

Everything below is in Vandeta and **not** in stock Bruce. Bruce's own menus,
apps, radios and script ecosystem are unchanged and still present — this is the
extra layer on top of them.

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
* The error ring collects ESP-IDF, Arduino `log_x()` and plain `printf()` /
  NimBLE lines in one place.
* Console commands answer immediately — none waits on a modal banner — and
  turning Developer Mode off releases the console's queues, watches and RX
  buffer.

</details>

<details>
<summary><b>Sub-GHz / CC1101</b></summary>

* **Working CC1101 RF.** The radio shares the display's SPI bus, so Vandeta
  serializes every radio transaction against the display (`rf_spi_lock()`),
  parks sweeps between hops and uses an adaptive noise floor. Scans, reads,
  spectrum, the analyzer and the wardriver all run reliably.
* **Frequency Analyzer** — Flipper-style: coarse hop over the three CC1101
  bands, 20 kHz fine pass, big held frequency readout, last three hits, RSSI
  meter, and a log page with sort (`OK`) and scroll.
* **SubGHz Wardriver** — whole-burst capture (256 pulses, streamed straight to
  the card), duplicate window, 433/315/868/915 band hop, short-pulse noise
  rejection, a captures browser, verbatim replay and one-pass
  `Identify & save` to a `_dec.sub`.
* `subghz rx <seconds>` listens for a timed window; `subghz wardriver
  capture|list|identify|identifyall|replay|status|selftest` drives the wardriver
  from the console.
* Replay follows the signal's own length (up to 30 s), and an oversized capture
  is refused with a clear message.

</details>

<details>
<summary><b>nRF24 (2.4 GHz)</b></summary>

* **Raw Capture** (`NRF24 > Raw Capture`) receives in the
  **pseudo-promiscuous** configuration — CRC off, auto-ack off, the shortest
  address width and degenerate noise addresses on all six pipes — so a 2.4 GHz
  remote can be captured by address-agnostic listening.
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
* Capture buffers are allocated from PSRAM, keeping ≈36 KB of internal DRAM
  free for WiFi and BLE.

</details>

<details>
<summary><b>Packet Monitor</b></summary>

* One dashboard fed by **WiFi, BLE, nRF24 and Sub-GHz**, opened from each of
  those menus with its own radio preselected.
* Filters (radio, type, MAC-OUI, channel, text, only-new), counters per radio
  and per type, packets/sec, busiest channel, CSV export and PCAP export of the
  captured WiFi frames, pause, change source and clear.
* All four taps capture live on this board: WiFi, BLE, nRF24 and Sub-GHz.

</details>

<details>
<summary><b>BLE</b></summary>

* **BLE Detector** — live table of advertisers with Apple Continuity types
  decoded, distance estimate, paging, hold and clear.
* **Capture & Replay** with a live packet counter and Back-to-stop.
* JS GATT client (connect, services, characteristics, read/write/notify) and the
  capture/replay bindings, plus headless advertisement spam.
* BLE bring-up, teardown and the memory guard keep internal DRAM available, and
  a WiFi feature frees BLE memory first — so BLE tools start cleanly even after
  another radio has been used.

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
* `ir uni list|buttons|send`, `ir ac list|send`, `ir rx <seconds>`; GPI02 is
  claimed before use and the LED is switched off afterwards.

</details>

<details>
<summary><b>WiFi</b></summary>

* Every added WiFi attack lives on the **`Wifi Atks`** page, and every target
  attack on the **`Target Atks`** page — one page each.
* ARP spoofing, DNS spoofing and HTTP interception/modification that reach
  WPA2/WPA3 clients (the frames go out on the encrypted station link).
* Evil-twin captive portal that refuses with a readable error when DMA memory is
  short.
* Headless, deadline-bounded attacks so scripts can run them; `moreatk` verb.

</details>

<details>
<summary><b>BadUSB / HID</b></summary>

* `BadUSB Settings` menu: USB footprint (VID, PID, manufacturer, product,
  serial), keyboard layout (extra layouts and custom import/export), HID device
  type, attack timing, payload manager, show output.
* Payloads keep typing past characters the active layout cannot express.
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
* Bindings are pinned against the interpreter's moving GC, so scripts can build
  containers safely.

</details>

<details>
<summary><b>Files, storage and settings</b></summary>

* `md5`, `crc32`, `cat` and `storage read` stream in 512-byte chunks — any file
  size, with a flat 512-byte buffer.
* `file read|write|list|delete|stat` with paths resolved against the console's
  working directory.
* Persisted settings in `bruce.conf`: the Developer Mode switch, the protocol
  preference, the whole BadUSB footprint and the Wardriver frequency, duplicate
  window, minimum pulse count and band hop — all validated on load.
* Stored names follow the menus (`JS Interpreter` → `Apps` keeps a hidden entry
  hidden).

</details>

<details>
<summary><b>Menus and navigation</b></summary>

* Breadcrumb path (`WiFi > Wifi Atks > Target Atks`), `n/total` counter and a
  scrollbar on long lists, plus an encoder hint bar (`TURN` / `SEL` / `BACK`).
* A child menu works on its own copy of the option list, so it cannot disturb
  the page it was opened from.
* Back returns to the parent menu.
* Capture screens show live progress and `Press BACK to stop`; only Back aborts,
  and presses queued during a capture are dropped before the result screen.
* The script interpreter is listed as **Apps** with an app-drawer icon, and the
  BadUSB page as **BadUSB Settings**.
* `ui` / `nav` report the menu that is on screen.

</details>

<details>
<summary><b>Board support and memory</b></summary>

* The CC1101 and the display share one SPI bus; a `TFT_eSPI` bus lock
  (`busLock()` / `busUnlock()`) lets the RF layer take the bus for a whole radio
  operation.
* nRF24 capture buffers are allocated from PSRAM, keeping ≈36 KB of internal
  DRAM free for the radios.
* The WiFi tap brings the radio up directly and releases the driver when it
  closes.
* Starting a WiFi feature first frees BLE, the web UI and stale captures, and
  the BLE guard checks the full ~56 KB a bring-up needs.

</details>

<details>
<summary><b>Diagnostics and reliability</b></summary>

* Console output formats `%d` / `%s` correctly.
* Screen streaming survives a rapid `display stop` / `start`, and `display dump`
  copies the log on the heap so a long dump is safe.
* BLE scans are armed asynchronously and stopped cleanly.
* `i2c scan` prints one device per line, `proc` shows `any` for no-affinity
  tasks, and the CC1101 driver prints no internal chatter.
* Large files hash and print without wedging the console.

</details>

<details>
<summary><b>Build and release</b></summary>

* Default `platformio.ini` env is `lilygo-t-embed-cc1101`; `build.py` merges
  bootloader + partitions + app into `vandeta.bin` for a single flash at `0x0`.
* The universal IR libraries ship in `sd_files/infrared/Universal/` and on the
  card under `/infrared/Universal/`.
* Firmware internals keep Bruce's names — `/BruceJS`, the `bruce` verbs, the
  BruceNet AP — with AGPL-3.0 and upstream credit unchanged.

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
  <https://github.com/pr3y/Bruce>). Vandeta is built on Bruce 1.16.1; the
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
  - [x] Extra Nordic layout, plus import/export/copy of custom layout files
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
