#ifndef __BLE_DETECTOR_H
#define __BLE_DETECTOR_H

// BLE Detector - the Momentum-style device radar, drawn with the Vandeta UI.
//
// A live table of every advertiser in range: name (when the advertisement
// carries one), address, RSSI, estimated distance and how many advertisements
// were seen. Devices are ordered strongest first and stay listed for a while
// after they stop advertising, so a phone that walks past is still readable.
//
// This is a receive-only scanner: it never connects and never writes.

/// Open the live detector screen. Returns when the user backs out.
void bleDetectorRun();

#endif
