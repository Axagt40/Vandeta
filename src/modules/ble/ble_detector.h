#ifndef __BLE_DETECTOR_H
#define __BLE_DETECTOR_H

// BLE Detector - the Momentum-style device radar, drawn with the Vandeta UI.
//
// A live table of every advertiser in range: name (when the advertisement
// carries one), address, RSSI, estimated distance and how many advertisements
// were seen. Devices are ordered strongest first and stay listed for a while
// after they stop advertising, so a phone that walks past is still readable.
//
// Names beyond Apple (Tile, Samsung SmartTag, Chipolo, Fast Pair / Find My,
// ASHA hearing aids) come from the vendor table in the .cpp, keyed on the
// advertised 16-bit service UUID, company id or name. A recognised tracker can
// be opened with SEL -> Track: big live RSSI, since-listed min/max, a proximity
// bar in the theme ramp, and a rising-signal cue.
//
// Adverts that are non-connectable and never stop get a marker and a footer
// count ("beacon-like"), which is a pattern worth a second look, not proof of
// anything.
//
// This is a receive-only scanner: it never connects and never writes.

/// Open the live radar table (no class filter: everything in range).
void bleDetectorRun();

/**
 * @brief BLE Detector menu (Bluetooth > BLE Detector): pick the device class
 * the table should show.
 *
 * Rows: Everything, Trackers & tags, Phones & audio, Apple, Drones, Beacons,
 * and Back. The classes all drive the same scan engine (the class only filters
 * rows), so switching never restarts the radio.
 */
void vandetaRadarMenu();

#endif
