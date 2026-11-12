#ifndef __IR_NORMALIZE_H__
#define __IR_NORMALIZE_H__

#include <Arduino.h>

// Task 4: IR learn-and-normalize.
//
// `IrRead` saves whatever the demodulator handed over: a burst of marks and
// spaces with jitter, sometimes with the repeat frame glued on the end. This
// models the capture against one base unit (te), snaps every duration to it,
// names the protocol when the decoder recognised one, and writes a .ir that is
// both smaller and more portable than the raw dump. Durations that the model
// cannot explain are kept literal instead of being bent into shape.
namespace IrNorm {

// Capture one waveform (up to `timeoutSec`), normalise it and optionally save it
// under /BruceIR. Returns a human report, or an empty string with `err` set.
String capture(int timeoutSec, bool save, String &err);

// The panel wrapper: asks for the listen window, captures and shows the report.
void menu();

} // namespace IrNorm

#endif
