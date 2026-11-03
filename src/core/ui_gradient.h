#ifndef __UI_GRADIENT_H__
#define __UI_GRADIENT_H__

#include <Arduino.h>

// The splash crown's pink -> purple ramp, available to the rest of the UI.
//
// The colour for every screen column is computed ONCE into a small table and
// then looked up, so drawing a gradient element costs the same number of pixels
// as the flat-colour version: no per-frame interpolation, no per-pixel maths.
// Every helper falls back to `bruceConfig.priColor` when the user turned the
// gradient off (Config > Display & UI > UI Color > Gradient), so call sites do
// not need their own if/else.
//
// All helpers go through the TFT wrapper (drawFastVLine/drawPixel), which means
// they are logged by tft_logger and show up in the USB screen mirror.

// False when the user disabled the gradient or the screen has no width yet.
bool uiGradientOn();

// Cached colour for a screen column. x is clamped to the panel.
uint16_t uiGradientColorAt(int x);

// Drop the cached ramp (rotation/orientation change, or a new panel size).
// The ramp itself does not depend on the theme, so a colour change does not
// need this; it is here so a future ramp can be reset cheaply.
void uiGradientReset();

// Filled shapes. `r` is the corner radius, same meaning as fillRoundRect().
void uiGradientFillRect(int x, int y, int w, int h);
void uiGradientFillRoundRect(int x, int y, int w, int h, int r);

// 1 px wide shapes.
void uiGradientHLine(int x, int y, int w);
void uiGradientVLine(int x, int y, int h);
void uiGradientDrawRect(int x, int y, int w, int h);
void uiGradientDrawRoundRect(int x, int y, int w, int h, int r);

#endif
