#include "ir_ac.h"

#include "core/display.h"
#include "TV-B-Gone.h" // lock_ir_tx()/unlock_ir_tx()
#include "ir_utils.h"  // setup_ir_pin()
#include <IRremoteESP8266.h>
#include <globals.h>
#include <ir_Coolix.h>
#include <ir_Daikin.h>
#include <ir_Gree.h>
#include <ir_Hitachi.h>
#include <ir_Kelvinator.h>
#include <ir_Midea.h>
#include <ir_Mitsubishi.h>
#include <ir_Panasonic.h>
#include <ir_Samsung.h>
#include <ir_Tcl.h>
#include <ir_Toshiba.h>
#include <ir_Whirlpool.h>

// ---------------------------------------------------------------------------
// Brand table
//
// Every entry is a make whose protocol IRremoteESP8266 can encode in full. The
// order is rough market presence: the ones most people actually own first.
// ---------------------------------------------------------------------------

enum AcBrand {
    AC_DAIKIN = 0,
    AC_GREE,
    AC_MIDEA,
    AC_COOLIX, // the Coolix protocol: many no-name and rebadged units use it
    AC_PANASONIC,
    AC_SAMSUNG,
    AC_TOSHIBA,
    AC_TCL,
    AC_MITSUBISHI,
    AC_HITACHI,
    AC_KELVINATOR,
    AC_WHIRLPOOL,
    AC_BRAND_COUNT
};

static const char *AC_BRAND_NAMES[AC_BRAND_COUNT] = {
    "Daikin", "Gree", "Midea", "Coolix", "Panasonic", "Samsung",
    "Toshiba", "TCL",  "Mitsubishi", "Hitachi", "Kelvinator", "Whirlpool",
};

const char *irAcBrandName(int brand) {
    if (brand < 0 || brand >= AC_BRAND_COUNT) return "";
    return AC_BRAND_NAMES[brand];
}

int irAcBrandCount() { return AC_BRAND_COUNT; }

static const char *acModeName(uint8_t mode) {
    switch (mode) {
        case 0: return "COOL";
        case 1: return "HEAT";
        case 2: return "DRY";
        case 3: return "FAN";
        default: return "AUTO";
    }
}

static const char *acFanName(uint8_t fan) {
    switch (fan) {
        case 1: return "LOW";
        case 2: return "MED";
        case 3: return "HIGH";
        default: return "AUTO";
    }
}

static uint16_t acPin() { return bruceConfigPins.irTx; }

bool irAcSend(int brand, const IrAcState &st) {
    lock_ir_tx();
    bool ok = true;

    switch (brand) {
        case AC_DAIKIN: {
            IRDaikinESP ac(acPin());
            ac.begin();
            ac.setPower(st.power);
            ac.setMode(st.mode == 0   ? kDaikinCool
                       : st.mode == 1 ? kDaikinHeat
                       : st.mode == 2 ? kDaikinDry
                       : st.mode == 3 ? kDaikinFan
                                      : kDaikinAuto);
            ac.setTemp(st.temp);
            ac.setFan(st.fan == 0   ? kDaikinFanAuto
                      : st.fan == 1 ? kDaikinFanMin
                      : st.fan == 2 ? kDaikinFanMed
                                    : kDaikinFanMax);
            ac.send();
            break;
        }
        case AC_GREE: {
            IRGreeAC ac(acPin());
            ac.begin();
            ac.setUseFahrenheit(false);
            ac.setPower(st.power);
            ac.setMode(st.mode == 0   ? kGreeCool
                       : st.mode == 1 ? kGreeHeat
                       : st.mode == 2 ? kGreeDry
                       : st.mode == 3 ? kGreeFan
                                      : kGreeAuto);
            ac.setTemp(st.temp, false);
            ac.setFan(st.fan == 0   ? kGreeFanAuto
                      : st.fan == 1 ? kGreeFanMin
                      : st.fan == 2 ? kGreeFanMed
                                    : kGreeFanMax);
            ac.send();
            break;
        }
        case AC_MIDEA: {
            IRMideaAC ac(acPin());
            ac.begin();
            ac.setPower(st.power);
            ac.setMode(st.mode == 0   ? kMideaACCool
                       : st.mode == 1 ? kMideaACHeat
                       : st.mode == 2 ? kMideaACDry
                       : st.mode == 3 ? kMideaACFan
                                      : kMideaACAuto);
            // Midea wants Celsius explicitly on some models.
            ac.setTemp(st.temp, true);
            ac.setFan(st.fan == 0   ? kMideaACFanAuto
                      : st.fan == 1 ? kMideaACFanLow
                      : st.fan == 2 ? kMideaACFanMed
                                    : kMideaACFanHigh);
            ac.send();
            break;
        }
        case AC_COOLIX: {
            IRCoolixAC ac(acPin());
            ac.begin();
            ac.setPower(st.power);
            ac.setMode(st.mode == 0   ? kCoolixCool
                       : st.mode == 1 ? kCoolixHeat
                       : st.mode == 2 ? kCoolixDry
                       : st.mode == 3 ? kCoolixFan
                                      : kCoolixAuto);
            ac.setTemp(constrain(st.temp, kCoolixTempMin, kCoolixTempMax));
            ac.setFan(st.fan == 0   ? kCoolixFanAuto
                      : st.fan == 1 ? kCoolixFanMed
                      : st.fan == 2 ? kCoolixFanMed
                                    : kCoolixFanMax);
            ac.send();
            break;
        }
        case AC_PANASONIC: {
            IRPanasonicAc ac(acPin());
            ac.begin();
            ac.setPower(st.power);
            ac.setMode(st.mode == 0   ? kPanasonicAcCool
                       : st.mode == 1 ? kPanasonicAcHeat
                       : st.mode == 2 ? kPanasonicAcDry
                       : st.mode == 3 ? kPanasonicAcFan
                                      : kPanasonicAcAuto);
            ac.setTemp(st.temp);
            ac.setFan(st.fan == 0   ? kPanasonicAcFanAuto
                      : st.fan == 1 ? kPanasonicAcFanLow
                      : st.fan == 2 ? kPanasonicAcFanMed
                                    : kPanasonicAcFanHigh);
            ac.send();
            break;
        }
        case AC_SAMSUNG: {
            IRSamsungAc ac(acPin());
            ac.begin();
            ac.setPower(st.power);
            ac.setMode(st.mode == 0   ? kSamsungAcCool
                       : st.mode == 1 ? kSamsungAcHeat
                       : st.mode == 2 ? kSamsungAcDry
                       : st.mode == 3 ? kSamsungAcFan
                                      : kSamsungAcAuto);
            ac.setTemp(st.temp);
            ac.setFan(st.fan == 0   ? kSamsungAcFanAuto
                      : st.fan == 1 ? kSamsungAcFanLow
                      : st.fan == 2 ? kSamsungAcFanMed
                                    : kSamsungAcFanHigh);
            ac.send();
            break;
        }
        case AC_TOSHIBA: {
            IRToshibaAC ac(acPin());
            ac.begin();
            ac.setPower(st.power);
            ac.setMode(st.mode == 0   ? kToshibaAcCool
                       : st.mode == 1 ? kToshibaAcHeat
                       : st.mode == 2 ? kToshibaAcDry
                       : st.mode == 3 ? kToshibaAcFan
                                      : kToshibaAcAuto);
            ac.setTemp(st.temp);
            ac.setFan(st.fan == 0   ? kToshibaAcFanAuto
                      : st.fan == 1 ? kToshibaAcFanMin
                      : st.fan == 2 ? kToshibaAcFanMed
                                    : kToshibaAcFanMax);
            ac.send();
            break;
        }
        case AC_TCL: {
            IRTcl112Ac ac(acPin());
            ac.begin();
            ac.setPower(st.power);
            ac.setMode(st.mode == 0   ? kTcl112AcCool
                       : st.mode == 1 ? kTcl112AcHeat
                       : st.mode == 2 ? kTcl112AcDry
                       : st.mode == 3 ? kTcl112AcFan
                                      : kTcl112AcAuto);
            ac.setTemp((float)st.temp);
            ac.setFan(st.fan == 0   ? kTcl112AcFanAuto
                      : st.fan == 1 ? kTcl112AcFanLow
                      : st.fan == 2 ? kTcl112AcFanMed
                                    : kTcl112AcFanHigh);
            ac.send();
            break;
        }
        case AC_MITSUBISHI: {
            IRMitsubishiAC ac(acPin());
            ac.begin();
            ac.setPower(st.power);
            ac.setMode(st.mode == 0   ? kMitsubishiAcCool
                       : st.mode == 1 ? kMitsubishiAcHeat
                       : st.mode == 2 ? kMitsubishiAcDry
                       : st.mode == 3 ? kMitsubishiAcFan
                                      : kMitsubishiAcAuto);
            ac.setTemp((float)st.temp);
            ac.setFan(st.fan == 0   ? kMitsubishiAcFanAuto
                      : st.fan == 1 ? 1
                      : st.fan == 2 ? 3
                                    : kMitsubishiAcFanMax);
            ac.send();
            break;
        }
        case AC_HITACHI: {
            IRHitachiAc ac(acPin());
            ac.begin();
            ac.setPower(st.power);
            ac.setMode(st.mode == 0   ? kHitachiAcCool
                       : st.mode == 1 ? kHitachiAcHeat
                       : st.mode == 2 ? kHitachiAcDry
                       : st.mode == 3 ? kHitachiAcFan
                                      : kHitachiAcAuto);
            ac.setTemp(st.temp);
            ac.setFan(st.fan == 0   ? kHitachiAcFanAuto
                      : st.fan == 1 ? kHitachiAcFanLow
                      : st.fan == 2 ? kHitachiAcFanMed
                                    : kHitachiAcFanHigh);
            ac.send();
            break;
        }
        case AC_KELVINATOR: {
            IRKelvinatorAC ac(acPin());
            ac.begin();
            ac.setPower(st.power);
            ac.setMode(st.mode == 0   ? kKelvinatorCool
                       : st.mode == 1 ? kKelvinatorHeat
                       : st.mode == 2 ? kKelvinatorDry
                       : st.mode == 3 ? kKelvinatorFan
                                      : kKelvinatorAuto);
            ac.setTemp(st.temp);
            ac.setFan(st.fan == 0   ? kKelvinatorFanAuto
                      : st.fan == 1 ? kKelvinatorFanMin
                      : st.fan == 2 ? 3
                                    : kKelvinatorFanMax);
            ac.send();
            break;
        }
        case AC_WHIRLPOOL: {
            IRWhirlpoolAc ac(acPin());
            ac.begin();
            ac.setPowerToggle(st.power);
            ac.setMode(st.mode == 0   ? kWhirlpoolAcCool
                       : st.mode == 1 ? kWhirlpoolAcHeat
                       : st.mode == 2 ? kWhirlpoolAcDry
                       : st.mode == 3 ? kWhirlpoolAcFan
                                      : kWhirlpoolAcAuto);
            ac.setTemp(st.temp);
            ac.setFan(st.fan == 0   ? kWhirlpoolAcFanAuto
                      : st.fan == 1 ? kWhirlpoolAcFanLow
                                    : kWhirlpoolAcFanHigh);
            // Whirlpool needs an explicit command; power change uses POWER,
            // everything else is a settings update (TEMP performs that).
            ac.setCommand(st.power ? kWhirlpoolAcCommandPower : kWhirlpoolAcCommandTemp);
            ac.send();
            break;
        }
        default: ok = false; break;
    }

    unlock_ir_tx();
    return ok;
}

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------

static void irAcControl(int brand) {
    IrAcState st;
    const char *name = irAcBrandName(brand);
    setup_ir_pin(bruceConfigPins.irTx, OUTPUT);
    init_ir_tx_mutex(); // shared with TV-B-Gone / Universal Remote sweeps

    while (true) {
        // Local vector: this page is opened from irAcMenu(), which is still
        // inside its own loopOptions(). Replacing the shared global `options`
        // there made the parent redraw with a stale index (the AC page looked
        // blank / jumped back).
        std::vector<Option> localOptions = {
            {String("Power: ") + (st.power ? "ON" : "OFF"),
             [&]() {
                 st.power = !st.power;
                 irAcSend(brand, st);
             }                                      },
            {String("Mode: ") + acModeName(st.mode),
             [&]() {
                 st.mode = (st.mode + 1) % 5;
                 irAcSend(brand, st);
             }                                      },
            {String("Temp: ") + String(st.temp) + " C  (-)",
             [&]() {
                 if (st.temp > 16) st.temp--;
                 irAcSend(brand, st);
             }                                      },
            {String("Temp: ") + String(st.temp) + " C  (+)",
             [&]() {
                 if (st.temp < 30) st.temp++;
                 irAcSend(brand, st);
             }                                      },
            {String("Fan: ") + acFanName(st.fan),
             [&]() {
                 st.fan = (st.fan + 1) % 4;
                 irAcSend(brand, st);
             }                                      },
            {"Send state",
             [&]() { irAcSend(brand, st); }          },
            {"Back", []() {}},
        };

        String title = String(name) + "  " + acModeName(st.mode) + " " + String(st.temp) + "C";
        int sel = loopOptions(localOptions, MENU_TYPE_SUBMENU, title.c_str());
        if (sel == -1 || sel == (int)localOptions.size() - 1) return;
    }
}

void irAcMenu() {
    while (true) {
        std::vector<Option> localOptions;
        for (int i = 0; i < irAcBrandCount(); i++) {
            int idx = i;
            localOptions.push_back({irAcBrandName(idx), [idx]() { irAcControl(idx); }});
        }
        localOptions.push_back({"Back", []() {}});

        int sel = loopOptions(localOptions, MENU_TYPE_SUBMENU, "AC Remotes");
        if (sel == -1 || sel == (int)localOptions.size() - 1) return;
    }
}
