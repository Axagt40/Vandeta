#include "core/ui_gradient.h"
#include "core/display.h"
#include <globals.h>
#include <math.h>

namespace {

// Same endpoints as bootCrownRows() in src/main.cpp: the splash crown runs from
// rgb(255, 20, 147) on the left rim to rgb(128, 0, 255) on the right one.
const int RAMP_FROM_R = 255, RAMP_FROM_G = 20, RAMP_FROM_B = 147;
const int RAMP_TO_R = 128, RAMP_TO_G = 0, RAMP_TO_B = 255;

// One ramp step per five screen columns. 320 / 5 = 64 steps, which keeps a full
// width element at 64 draw calls or fewer - about the same cost as a flat fill,
// and no more packets than the tft_logger ring holds. The 5 px bands are below
// the eye's limit against the 565 colour steps on this 320 px panel.
const int STEP = 5;

// 320 px panel / 5, with room for wider orientations.
const int MAX_BANDS = 96;
uint16_t bands[MAX_BANDS];
int bandWidth = 0; // screen width the bands were built for

// C integer division (truncates toward zero), which is what the splash uses.
inline int truncDiv(int a, int b) {
    int q = abs(a) / abs(b);
    return ((a < 0) == (b < 0)) ? q : -q;
}

// tft.color565() by hand: building the ramp must not touch the display or
// allocate anything.
inline uint16_t pack565(int r, int g, int b) {
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void buildBands(int w) {
    if (w <= 0) {
        bandWidth = 0;
        return;
    }
    int n = (w + STEP - 1) / STEP;
    if (n > MAX_BANDS) n = MAX_BANDS;
    for (int i = 0; i < n; i++) {
        const int x = i * STEP;
        const int t = (w > 1) ? x * 255 / (w - 1) : 0;
        bands[i] = pack565(
            RAMP_FROM_R + truncDiv((RAMP_TO_R - RAMP_FROM_R) * t, 255),
            RAMP_FROM_G + truncDiv((RAMP_TO_G - RAMP_FROM_G) * t, 255),
            RAMP_FROM_B + truncDiv((RAMP_TO_B - RAMP_FROM_B) * t, 255)
        );
    }
    bandWidth = w;
}

// Rebuilt only when the panel width changes (rotation), never per frame.
inline void ensureBands() {
    if (bandWidth != tftWidth) buildBands(tftWidth);
}

// Number of columns from `dx` that share the colour at `dx`, capped at `left`.
int runLength(int screenX, int dx, int left) {
    const uint16_t colour = uiGradientColorAt(screenX + dx);
    int run = 1;
    while (run < left && uiGradientColorAt(screenX + dx + run) == colour) run++;
    return run;
}

// Vertical extent of a rounded rect at column x. top/bottom are relative to the
// rect's own top edge; false when the column falls outside the shape.
bool columnSpan(int x, int w, int h, int r, int &top, int &bottom) {
    if (x < 0 || x >= w || w <= 0 || h <= 0) return false;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    top = 0;
    bottom = h - 1;
    const int edge = (x < w - 1 - x) ? x : w - 1 - x; // distance to the nearer side
    if (edge >= r) return true;
    // Circle of radius r centred at (r, r) (and mirrored at the bottom).
    const int dy = r - edge;
    int cut = r - (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
    if (cut < 0) cut = 0;
    top = cut;
    bottom = h - 1 - cut;
    return bottom >= top;
}

} // namespace

bool uiGradientOn() {
    if (!bruceConfig.uiGradient) return false;
    return tftWidth > 0;
}

uint16_t uiGradientColorAt(int x) {
    if (!uiGradientOn()) return bruceConfig.priColor;
    ensureBands();
    if (bandWidth <= 0) return bruceConfig.priColor;
    if (x < 0) x = 0;
    if (x >= bandWidth) x = bandWidth - 1;
    int i = x / STEP;
    const int n = (bandWidth + STEP - 1) / STEP;
    if (i >= n) i = n - 1;
    return bands[i];
}

void uiGradientReset() { bandWidth = 0; }

void uiGradientFillRect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!uiGradientOn()) {
        tft.fillRect(x, y, w, h, bruceConfig.priColor);
        return;
    }
    for (int dx = 0; dx < w;) {
        const int run = runLength(x, dx, w - dx);
        tft.fillRect(x + dx, y, run, h, uiGradientColorAt(x + dx));
        dx += run;
    }
}

void uiGradientFillRoundRect(int x, int y, int w, int h, int r) {
    if (w <= 0 || h <= 0) return;
    if (!uiGradientOn()) {
        tft.fillRoundRect(x, y, w, h, r, bruceConfig.priColor);
        return;
    }
    for (int dx = 0; dx < w;) {
        const int run = runLength(x, dx, w - dx);
        int top, bottom;
        // The span of the middle column of the run: inside a 5 px band the
        // rounded corner cannot differ by more than a pixel, so one fillRect
        // per run is visually identical to per-column lines.
        if (!columnSpan(dx + run / 2, w, h, r, top, bottom)) {
            dx += run;
            continue;
        }
        tft.fillRect(x + dx, y + top, run, bottom - top + 1, uiGradientColorAt(x + dx));
        dx += run;
    }
}

void uiGradientHLine(int x, int y, int w) {
    if (w <= 0) return;
    if (!uiGradientOn()) {
        tft.drawFastHLine(x, y, w, bruceConfig.priColor);
        return;
    }
    for (int dx = 0; dx < w;) {
        const int run = runLength(x, dx, w - dx);
        tft.drawFastHLine(x + dx, y, run, uiGradientColorAt(x + dx));
        dx += run;
    }
}

void uiGradientVLine(int x, int y, int h) {
    if (h <= 0) return;
    if (!uiGradientOn()) {
        tft.drawFastVLine(x, y, h, bruceConfig.priColor);
        return;
    }
    tft.drawFastVLine(x, y, h, uiGradientColorAt(x));
}

void uiGradientDrawRect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!uiGradientOn()) {
        tft.drawRect(x, y, w, h, bruceConfig.priColor);
        return;
    }
    uiGradientHLine(x, y, w);
    uiGradientHLine(x, y + h - 1, w);
    uiGradientVLine(x, y, h);
    uiGradientVLine(x + w - 1, y, h);
}

void uiGradientDrawRoundRect(int x, int y, int w, int h, int r) {
    if (w <= 0 || h <= 0) return;
    if (!uiGradientOn()) {
        tft.drawRoundRect(x, y, w, h, r, bruceConfig.priColor);
        return;
    }
    // The two horizontal edges, as runs of one colour per band.
    for (int dx = 0; dx < w;) {
        const int run = runLength(x, dx, w - dx);
        int top, bottom;
        if (columnSpan(dx + run / 2, w, h, r, top, bottom)) {
            const uint16_t colour = uiGradientColorAt(x + dx);
            tft.drawFastHLine(x + dx, y + top, run, colour);
            tft.drawFastHLine(x + dx, y + bottom, run, colour);
        }
        dx += run;
    }
    // The straight left/right edges (the only columns whose whole span is the
    // outline; the corner columns above already got their two pixels).
    int top, bottom;
    if (columnSpan(0, w, h, r, top, bottom))
        tft.drawFastVLine(x, y + top, bottom - top + 1, uiGradientColorAt(x));
    if (columnSpan(w - 1, w, h, r, top, bottom))
        tft.drawFastVLine(x + w - 1, y + top, bottom - top + 1, uiGradientColorAt(x + w - 1));
}
