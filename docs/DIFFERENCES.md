# How Vandeta differs from stock Bruce

Same codebase, same licence, one board. Stock Bruce is column two; what this
build does about it is column three.

| Area | Stock Bruce 1.16.1 | Vandeta |
|------|--------------------|---------|
| CC1101 and display | Both sit on one SPI bus; a screen write during a radio transaction corrupts it, so scans, sweeps and captures drop out on the T-Embed | A `TFT_eSPI` `busLock()`/`busUnlock()` pair plus `rf_spi_lock()` around every radio operation, sweeps parked between hops, adaptive noise floor |
| Host control | WebUI and Telnet over WiFi; no shell over the cable | USB console with a line editor, history, Tab completion: `ui`/`move`/`press`, `ui watch`, `file`, `log`, `errors`, `livepatch`, `liveboot`, `display dump` |
| Menus | A scrolling icon menu | A 3x3 launcher grid paged nine tiles at a time, each tile with its own icon and label, a `1/2` page counter under the grid, the selection on the gradient plate, and one status strip (clock, SD/GPS/WiFi/WebUI/BLE/WireGuard, charge, battery) that survives every repaint |
| Sub-GHz tools | Scan/Copy, Custom SubGhz, Spectrum, jammer, replay, pin and module config | Plus an RF Suite page (Frequency Analyzer, Field Meter, Packet Monitor, Bit Editor, Wardriver) and a bit editor that edits a pulse train as 0/1 - load or type it, set `TE` and the repeat count, flip one bit - and re-modulates it from the console |
| Infrared | TV-B-Gone, receiver, custom IR codes | Plus Universal Remote (2,858 codes from the Flipper libraries), IR Suite 1.1 (brand / remote / family sweeps, save as `.ir`, `.txt` payload runs, DLNA/Cast mirroring), hand-written RCA and NEC42 encoders, 12 brand AC remotes |
| IR analysis | TV-B-Gone, receiver, custom codes; whatever the demodulator hands over is saved as-is | Learn & normalize in the IR Suite: models a capture against one `te`, snaps the jitter onto that model, names the protocol when the decoder knows it and writes a smaller portable `.ir`; durations the model cannot explain stay literal |
| nRF24 | Jammer, 2.4 GHz spectrum | Plus pseudo-promiscuous raw capture (CRC off, auto-ack off), hop 0-125, RGB remote presets, multi-frequency detect, packet analysis, verbatim replay, JS bindings, PSRAM capture buffers |
| BLE | BLE Scan, Bad BLE, the vendor spams, Capture & Replay, BLE Keyboard (Cardputer/T-Deck only) | Plus a BLE Attacks page - Cadence, Pairing Storm, a 20 s BLE Flood, and Auth Capture saving `/BruceBLE/auth_<addr>.key` - and BLE Suite 3.1 (vulnerability scan, HID/DuckyScript, FastPair and HFP exploits, audio attacks, a sniffer and a model database) |
| Wardriver | Band-hop on/off | Three modes - Hop 400 ms, Fast hop 120 ms, Wideband parked with an 812.5 kHz filter and squelch - and a capture the receiver lost pulses through is deleted and counted, not saved as a good one |
| Packet monitor | One radio per screen | One dashboard fed by WiFi, BLE, nRF24 and Sub-GHz at once, with filters, per-radio counters and CSV/PCAP export |
| MITM | ARP/DNS spoofing, Evil Portal | Plus a whole attack from the keypad with ARP restore on stop, a live event feed, plaintext HTTP on any port, HTTPS on 443 with a self-signed endpoint, and suppression of QUIC, DoT, IPv6 and AAAA/SVCB/HTTPS |
| WiFi analysis | Attacks only, plus a raw sniffer to read them by hand | The Counter-Surv dashboard (deauth, evil-twin, KARMA, ARP and Remote ID events, BLE address rotation), EAPOL Harvest for 802.1X identities in `Target Atks`, and DHCP Starvation for a whole VLAN |
| Developer loop | Rebuild and reflash | `tune` (19 range-checked values), `patch` (three variant tables plus an A/B code overlay), `devflash`, `autotest`, one-line `battery`/`buttons`/`clock`/`uptime` reporters, and a per-build ELF, map, app-only image and build ID |
| Board support | 18 devices | One: the LilyGo T-Embed CC1101 Plus; the other boards' pin-outs stay in `boards/`, untested |
| Deliberately not | - | No new radios or protocols: the CC1101 and nRF24 cannot demodulate 802.11. No BLE relay proxy. No PEAP termination. No microphone on this board. No account, cloud or telemetry. |

The screenshots at the top of the [README](../README.md) are captured from the
board over USB; the site's
[documentation](https://axagt40.github.io/Vandeta/docs.html) describes each page
in the table above.
