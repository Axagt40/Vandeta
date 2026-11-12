# Roadmap

**The current build is a pre-release: 1.4 pre.** 1.3 is the newest official
release, and the flasher keeps opening on it; the pre-release channel has to be
selected on purpose. Nothing on this page is dated, scheduled or promised.

Two roadmaps used to live in this repository - this one and Bruce's own
`2.0_road_path.md`. They are merged here: the seven Vandeta workstreams first,
then the upstream 2.0 items Vandeta inherits, with their real state.

## Vandeta workstreams

Seven of them. **None is started.**

| # | Workstream | Status | Why it is open |
|---|------------|--------|----------------|
| 1 | BLE MITM relay proxy | Not started | needs two simultaneous links and a second controllable peripheral |
| 2 | WPA2-Enterprise rogue AP (PEAP) | Not started | needs an 802.1X state machine the firmware does not have |
| 3 | Encrypted-DNS visibility (DoH/ECH) | Not started | the reset needs the TLS SNI; ECH and DoH-by-IP are invisible today |
| 4 | IPv6 resolver retirement (DHCPv6) | Not started | only the RA path is retired; the DHCPv6 resolver is just blocked |
| 5 | Single-target ARP resilience | Not started | a randomised MAC and a cold ARP cache can miss the takeover |
| 6 | SPI and UART in the JavaScript interpreter | Not started | upstream 2.0 item; I2C is the only bus exposed |
| 7 | RF RollJam | Not started | upstream 2.0 item; needs two captures and a timed replay |

### 1. BLE MITM relay proxy

Relay one BLE link through the board to a second peripheral, so a phone pairs
with the board while the board pairs with the real device. Blocked on two
simultaneous connections and a controllable peripheral to relay to; until then
the honest subset is the passive capture the BLE menu already does.

### 2. WPA2-Enterprise rogue AP (PEAP)

An 802.1X authenticator that terminates PEAP and MSCHAPv2. Today's EAPOL work is
passive: it reads the outer identity and the requested methods, and does not
tunnel or take a credential.

### 3. Encrypted-DNS visibility (DoH/ECH)

The MITM engine resets QUIC and blocks TCP/853, but DoH is only visible when the
TLS SNI is in the clear. ECH and DoH-by-IP would need either a resolver list
match or TLS inspection, and neither is in the build.

### 4. IPv6 resolver retirement (DHCPv6)

RA retirement (lifetime 0, RDNSS 0) is sent and IPv6 data is dropped, but a
resolver handed out over DHCPv6 is not retired - it is only blocked. Finishing
this means answering DHCPv6 ourselves.

### 5. Single-target ARP resilience

A single-host poison needs the victim's MAC, and a randomised MAC plus a cold
ARP cache makes the first takeover after a reboot miss. The direction is a retry
with an active probe instead of a one-shot pair.

### 6. SPI and UART in the JavaScript interpreter

Upstream's 2.0 path lists I2C, SPI and UART bus access for the interpreter; only
I2C exists. On this board SPI is shared with the CC1101 and the display, so the
binding would have to take the same lock the radio layer does.

### 7. RF RollJam

Capture a rolling-code remote's two frames and replay them in the window the
receiver expects. The sub-GHz layer already captures whole bursts and replays
verbatim, so this is a sequencing problem, not a radio one.

## Upstream 2.0 path (inherited)

Bruce's plan for 2.0 is to move the framework to Arduino-ESP32 v3 and make the
ESP32-C5 reachable. State below is upstream's, as of this merge; items marked
done are in Vandeta by virtue of the base being 1.16.1.

| Area | Item | Upstream state |
|------|------|----------------|
| Framework | Arduino-ESP32 v3, newer C++ | Done |
| Framework | ESP32-C5 support (5 GHz WiFi) | Done |
| Framework | Remove OpenHaystack | Done |
| Framework | Remove DPWO (turn it into JavaScript) | Done |
| Hardware | Common input handling in `#define` tables, not per device | Open |
| Hardware | Shared PPM and fuel-gauge functions | Open |
| Display | A TFT middle class (ArduinoGFX / Lovyan_GFX) | Done |
| RFID | PN532 emulation | Done |
| RFID | LoRa Pager RFID chip read/write/emulate | Done |
| RF | RollJam | Open (Vandeta workstream 7) |
| RF | Protocol encode/decode | Done |
| nRF24 | Mouse jacking | Done |
| nRF24 | Keyboard jacking | Done |
| BLE | Session hijacking | Open |
| Interpreter | I2C access | Done |
| Interpreter | SPI access | Open (Vandeta workstream 6) |
| Interpreter | UART access | Open (Vandeta workstream 6) |
| FM | Remove it and move it into JavaScript | Open |
