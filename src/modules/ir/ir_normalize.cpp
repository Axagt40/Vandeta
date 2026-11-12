#include "ir_normalize.h"

#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/scrollableTextArea.h"
#include "core/sd_functions.h"
#include "ir_utils.h"
#include "core/sound_theme.h"
#include <IRrecv.h>
#include <IRutils.h>
#include <globals.h>

namespace IrNorm {

namespace {

constexpr int kTeMin = 40;
constexpr int kTeMax = 5000;

String protocolName(const decode_results &r) {
    if (r.decode_type == decode_type_t::UNKNOWN) return "RAW";
    String s = typeToString(r.decode_type, r.repeat);
    if (s.length() == 0) return "RAW";
    s.replace(" ", "_");
    return s;
}

// Every IR encoding is built on one base unit: NEC's 560 us, RC5's 889 us, the
// AC protocols' 430 us. The shortest mark that repeats is that unit - the
// nominal ``te''. A histogram (50 us buckets) is used instead of a gcd because
// a single noisy sample would otherwise drag the whole model.
uint16_t estimateTe(const uint16_t *d, uint16_t n) {
    uint16_t hist[64] = {0}; // 50 us buckets, up to 3200 us
    for (uint16_t i = 0; i < n; i += 2) { // marks are even indices
        const uint16_t v = d[i];
        if (v < kTeMin || v > 3200) continue;
        hist[v / 50]++;
    }
    for (int b = 1; b < 64; b++)
        if (hist[b] >= 2) return (uint16_t)(b * 50 + 25);

    // Nothing repeated: the shortest valid mark is the best available unit.
    uint16_t shortest = 0;
    for (uint16_t i = 0; i < n; i += 2) {
        if (d[i] >= kTeMin && (shortest == 0 || d[i] < shortest)) shortest = d[i];
    }
    return shortest;
}

// Snap one duration to the nearest whole multiple of te. The residual decides
// whether the model really explains it: anything off by more than a quarter is
// kept literal, because silently rewriting an unexplained pulse is how a
// "normalised" capture stops working on the device it was learned from.
uint16_t snapTo(uint16_t v, uint16_t te, bool &explained) {
    if (te == 0) {
        explained = false;
        return v;
    }
    int k = (v + te / 2) / te;
    if (k < 1) k = 1;
    if (k > 255) k = 255;
    const uint32_t snapped = (uint32_t)k * te;
    const int diff = (int)v - (int)snapped;
    const int absDiff = diff < 0 ? -diff : diff;
    explained = (uint32_t)absDiff * 4 <= (uint32_t)v; // within 25%
    return (uint16_t)(snapped > 0xFFFF ? 0xFFFF : snapped);
}

} // namespace

String capture(int timeoutSec, bool save, String &err) {
    if (timeoutSec < 1) timeoutSec = 1;
    if (timeoutSec > 60) timeoutSec = 60;

    setup_ir_pin(bruceConfigPins.irRx, INPUT);
    IRrecv ir(bruceConfigPins.irRx, SAFE_STACK_BUFFER_SIZE / 2, 50);
    ir.enableIRIn();

    const uint32_t end = millis() + (uint32_t)timeoutSec * 1000UL;
    decode_results results;
    bool got = false;
    while ((int32_t)(millis() - end) < 0) {
        if (ir.decode(&results)) {
            got = true;
            break;
        }
        delay(10);
        yield();
    }
    if (!got) {
        ir.disableIRIn();
        err = "no IR signal in " + String(timeoutSec) + "s";
        return "";
    }

    // Same confirmation the IR Read page gives: a code landed, so the capture
    // is audible even when the user is looking at the remote, not the panel.
    soundPlay(CUE_READ);

    const uint16_t len = getCorrectedRawLength(&results);
    uint16_t *raw = resultToRawArray(&results);
    ir.disableIRIn();

    if (raw == nullptr || len == 0) {
        if (raw != nullptr) delete[] raw;
        err = "empty capture";
        return "";
    }

    const String proto = protocolName(results);
    const uint16_t te = estimateTe(raw, len);

    uint16_t *snapped = new uint16_t[len];
    if (snapped == nullptr) {
        delete[] raw;
        err = "out of memory";
        return "";
    }

    uint16_t explainedCount = 0;
    for (uint16_t i = 0; i < len; i++) {
        bool explained = false;
        snapped[i] = snapTo(raw[i], te, explained);
        if (explained) explainedCount++;
    }

    // A repeat frame starts after a gap many units long; counting those says how
    // many frames were learned without trusting any one protocol's frame length.
    uint16_t frames = 1;
    if (te > 0) {
        for (uint16_t i = 1; i < len; i += 2) {
            if (raw[i] > (uint16_t)(te * 10) && frames < 99) frames++;
        }
    }

    const uint32_t rawBytes = (uint32_t)len * sizeof(uint16_t);
    const uint32_t modelBytes = (uint32_t)len + 6; // one unit-multiple byte each + header
    const int fit = len ? (int)((uint32_t)explainedCount * 100 / len) : 0;

    String out = "irnorm: " + proto;
    if (results.decode_type != decode_type_t::UNKNOWN) out += " " + String(results.bits) + " bits";
    out += ", " + String(len) + " marks/spaces, " + String(frames) + " frame(s)\n";
    out += "  te " + String(te) + " us, model fit " + String(fit) + "% (" + String(len - explainedCount) +
           " durations kept literal)\n";
    out += "  compact form " + String(modelBytes) + " B vs " + String(rawBytes) + " B raw (" +
           String(rawBytes ? (uint32_t)modelBytes * 100 / rawBytes : 0) + "%)\n";
    // The receiver is a demodulator: it strips the carrier, so 38 kHz here is the
    // band this hardware answers to, not a measurement of the remote's carrier.
    out += "  carrier 38 kHz (receiver band, not measured)\n";

    if (save) {
        FS *fs = nullptr;
        if (setupSdCard()) fs = &SD;
        else if (checkLittleFsSize()) fs = &LittleFS;
        if (fs == nullptr) {
            delete[] snapped;
            delete[] raw;
            err = "no storage (SD card or LittleFS)";
            return "";
        }
        if (!fs->exists("/BruceIR")) fs->mkdir("/BruceIR");
        const String path = "/BruceIR/NORM_" + proto + "_" + String((uint32_t)(millis() % 1000)) + ".ir";

        String content;
        content.reserve((uint32_t)len * 6 + 320);
        content += "Filetype: Bruce IR File\n";
        content += "Version: 1\n";
        content += "#\n";
        content += "# normalized: te=" + String(te) + " us, " + proto + ", frames " + String(frames) + ", fit " +
                   String(fit) + "%\n";
        content += "name: Normalized " + proto + "\n";
        content += "type: raw\n";
        content += "frequency: 38000\n";
        content += "duty_cycle: 0.33\n";
        content += "data: ";
        for (uint16_t i = 0; i < len; i++) {
            content += String(snapped[i]);
            content += " ";
        }
        content += "\n#\n";

        File file = fs->open(path, FILE_WRITE);
        if (!file) {
            delete[] snapped;
            delete[] raw;
            err = "cannot write " + path;
            return "";
        }
        file.print(content);
        file.close();
        out += "  saved " + path + " on " + String(fs == &SD ? "SD" : "LittleFS") + "\n";
    }

    delete[] snapped;
    delete[] raw;
    return out;
}

void menu() {
    // One line so a serial session can tell this page apart from the suite menu
    // it was opened from (the JS suite reaches it through ir.normalizeMenu()).
    Serial.println("irnorm: page open");
    String v = num_keyboard("10", 2, "Listen (s):");
    if (v == "\x1B") {
        Serial.println("irnorm: cancelled");
        return;
    }
    int seconds = v.toInt();
    if (seconds < 1) seconds = 1;
    Serial.println("irnorm: listening " + String(seconds) + "s");

    String err;
    const String report = capture(seconds, true, err);
    // The page shows this in a text area, which never reaches the USB mirror.
    // Echo it once so a serial session can confirm the capture really ran (and
    // whether it heard something) without looking at the panel.
    Serial.println(report.length() ? report : ("irnorm: ERR: " + err));
    ScrollableTextArea area = ScrollableTextArea("IR Normalize");
    area.fromString(report.length() ? report : ("ERR: " + err));
    area.show();
    // ScrollableTextArea marks "leave the page" by setting returnToMenu. This
    // page now lives inside the IR Suite script, where that flag means "stop
    // the interpreter" - it must stay false so the suite menu comes back.
    returnToMenu = false;
}

} // namespace IrNorm
