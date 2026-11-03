#ifndef __PACKET_VIEW_H__
#define __PACKET_VIEW_H__

// Task 17 - real-time packet monitoring dashboard.
//
// PHASE 1 (demo shell): the dashboard, its menu and the Filters / Counters /
// Export screens with their titles, columns, labels and navigation.
// PHASE 2 (this build): the four radio taps are wired in and the table is live.
//   - WiFi : promiscuous capture, all 802.11 frames, channel hopping.
//   - BLE  : passive advertisement scan (reuses modules/ble/ble_capture).
//   - NRF24: pseudo-promiscuous raw packets (reuses modules/NRF24/nrf_capture).
//   - SubGHz: CC1101 pulse/edge detection on GDO0.
//
// The dashboard shows RADIO METADATA ONLY - source, channel, transmitter
// address, frame type/subtype, length and RSSI, plus an SSID/name label where
// the radio advertises one openly. It deliberately does not decrypt, reassemble
// or extract payload contents.
enum PacketSource { PKT_SRC_WIFI = 0, PKT_SRC_BLE, PKT_SRC_NRF24, PKT_SRC_SUBGHZ };

void packetMonitor(PacketSource source);

#endif
