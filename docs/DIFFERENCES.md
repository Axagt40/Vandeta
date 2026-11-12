# How Vandeta differs from stock Bruce

Same codebase, same licence, one board. Stock Bruce is column two; what this
build does about it is column three.

| Area | Stock Bruce 1.16.1 | Vandeta |
|------|--------------------|---------|
| CC1101 and display | Both sit on one SPI bus; a screen write during a radio transaction corrupts it, so scans, sweeps and captures drop out on the T-Embed | A `TFT_eSPI` `busLock()`/`busUnlock()` pair plus `rf_spi_lock()` around every radio operation, sweeps parked between hops, adaptive noise floor |
| Host control | WebUI and Telnet over WiFi; no shell over the cable | USB console with a line editor, history, Tab completion: `ui`/`move`/`press`, `ui watch`, `file`, `log`, `errors`, `livepatch`, `liveboot`, `display dump` |
| Infrared | TV-B-Gone, receiver, custom IR codes | Plus Universal Remote (2,858 codes from the Flipper libraries), IR Suite 1.1 (brand / remote / family sweeps, save as `.ir`, `.txt` payload runs, DLNA/Cast mirroring), hand-written RCA and NEC42 encoders, 12 brand AC remotes |
| nRF24 | Jammer, 2.4 GHz spectrum | Plus pseudo-promiscuous raw capture (CRC off, auto-ack off), hop 0-125, RGB remote presets, multi-frequency detect, packet analysis, verbatim replay, JS bindings, PSRAM capture buffers |
| Packet monitor | One radio per screen | One dashboard fed by WiFi, BLE, nRF24 and Sub-GHz at once, with filters, per-radio counters and CSV/PCAP export |
| MITM | ARP/DNS spoofing, Evil Portal | Plus a whole attack from the keypad with ARP restore on stop, a live event feed, plaintext HTTP on any port, HTTPS on 443 with a self-signed endpoint, and suppression of QUIC, DoT, IPv6 and AAAA/SVCB/HTTPS |
| Developer loop | Rebuild and reflash | `tune` (15 range-checked values), `patch` (three variant tables plus an A/B code overlay), `devflash`, and a per-build ELF, map, app-only image and build ID |
| Board support | 18 devices | One: the LilyGo T-Embed CC1101 Plus |
| Deliberately not | - | No new radios or protocols: the CC1101 and nRF24 cannot demodulate 802.11. No BLE relay proxy. No PEAP termination. No microphone on this board. No account, cloud or telemetry. |

Screenshots of these screens are the slots at the top of the
[README](../README.md); until they are filled, the site's
[documentation](https://axagt40.github.io/Vandeta/docs.html) is the reference.
