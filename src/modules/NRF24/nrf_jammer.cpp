#include "nrf_jammer.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "nrf_common.h"
#include <cstring>
#include <globals.h>

/*
    Jam types (Sel cycles through them):

      Sequential  - walk the mode's channel list in order, retune every loop
      FHSS        - same, in shuffled order
      Enchanted   - activity-targeted: listen (RPD) on the mode's channels,
                    then park a LOCKED carrier only on the ones that actually
                    carry energy, dwelling longer where more traffic was heard,
                    and re-scan every ENCH_SCAN_MS to follow the target.

    Why Enchanted exists: the two sweeps retune RF_CH on every loop pass, i.e.
    every few microseconds. The nRF24 PLL needs ~130 us to settle after a
    channel change, so the carrier never actually locks onto a frequency - it
    chirps across the band and only a fraction of the radiated energy lands
    where the target is listening. Parking a locked carrier for >=300 us on the
    channels that are in use puts the full PA output exactly on the target's
    frequency, which is what extends the usable range.

    The activity map is the nRF24's own RPD bit (received power > ~-64 dBm),
    sampled per channel. This is the same "scan then jam only what is active"
    idea used by the ESP32 port's activity-scan jammer source, with a
    per-channel dwell weighted by the hit count added on top.
*/

#define ENCH_SCAN_MS 500          // re-scan period
#define ENCH_SCAN_SWEEPS 2        // samples per channel, per scan
#define ENCH_RPD_SETTLE_US 250    // RX settle before the RPD bit is valid
#define ENCH_MAX_TARGETS 12       // busiest channels kept from one scan
#define ENCH_MIN_DWELL_US 300     // >= nRF24 PLL re-lock (~130 us) with margin
#define ENCH_MAX_DWELL_US 2400

struct NrfJamMode {
    const char *name;
    byte *channels;
    size_t count;
};

static uint8_t enchScore[125];   // decayed activity per nRF24 channel
static uint8_t enchHits[125];    // one scan's raw hits
static uint8_t enchTargets[ENCH_MAX_TARGETS];
static uint16_t enchDwell[ENCH_MAX_TARGETS];
static size_t enchTargetCount = 0;
static size_t enchRotate = 0;
static size_t enchFallback = 0;
static uint32_t enchLastScan = 0;
static size_t enchLastLoggedCount = (size_t)-1; // invalid on purpose: first scan logs
static uint8_t enchLastLoggedTop = 0xFF;

static void shuffleChannels(uint8_t *arr, size_t count) {
    for (size_t i = count - 1; i > 0; i--) {
        size_t j = esp_random() % (i + 1);
        uint8_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

static void enchReset() {
    memset(enchScore, 0, sizeof(enchScore));
    memset(enchHits, 0, sizeof(enchHits));
    enchTargetCount = 0;
    enchRotate = 0;
    enchFallback = 0;
    enchLastScan = 0;
}

static String enchTargetSummary() {
    if (enchTargetCount == 0) return String("listening...");
    return String(enchTargetCount) + " ch, top CH " + String(enchTargets[0]);
}

// Leave the carrier, sample RPD on every channel of the mode, rebuild the
// ranked target list and bring the carrier back up on the busiest channel.
static void enchScan(const NrfJamMode &m) {
    memset(enchHits, 0, sizeof(enchHits));

    NRFradio.stopConstCarrier(); // per datasheet: CW is only reliable off via PWR_UP=0
    NRFradio.startListening();   // powerUp (Tpd2stby) + PRIM_RX + CE high
    // A second sample per channel is cheap on the short lists and doubles the
    // chance of catching a slow advertiser; on the wide lists (Full, BT) one
    // sweep is enough and keeps the scan gap (~31 ms per 500 ms) small.
    const uint8_t sweeps = (m.count > 64) ? 1 : ENCH_SCAN_SWEEPS;
    for (uint8_t sweep = 0; sweep < sweeps; sweep++) {
        for (size_t i = 0; i < m.count; i++) {
            uint8_t ch = m.channels[i];
            NRFradio.setChannel(ch);
            delayMicroseconds(ENCH_RPD_SETTLE_US);
            if (NRFradio.testRPD()) enchHits[ch]++;
        }
    }
    NRFradio.stopListening();

    // Decay the old map so a target that stops transmitting drops out, then
    // add this scan. 40 points per hit vs 7/8 decay: a channel seen on both
    // sweeps climbs to the top in ~2 scans, a silent one is gone in ~8.
    for (uint8_t ch = 0; ch < 125; ch++) {
        uint16_t v = (uint16_t)((enchScore[ch] * 7) / 8);
        if (enchHits[ch]) v += (uint16_t)enchHits[ch] * 40;
        enchScore[ch] = v > 255 ? 255 : (uint8_t)v;
    }

    // Rank the mode's channels by score (selection sort, no allocation).
    uint8_t cand[125];
    size_t candCount = 0;
    for (size_t i = 0; i < m.count; i++) {
        if (enchScore[m.channels[i]] > 0) cand[candCount++] = m.channels[i];
    }
    enchTargetCount = 0;
    while (enchTargetCount < ENCH_MAX_TARGETS && candCount > 0) {
        size_t best = 0;
        for (size_t i = 1; i < candCount; i++) {
            if (enchScore[cand[i]] > enchScore[cand[best]]) best = i;
        }
        uint8_t ch = cand[best];
        enchTargets[enchTargetCount] = ch;
        uint16_t dwell = ENCH_MIN_DWELL_US + (uint16_t)enchScore[ch] * 8;
        if (dwell > ENCH_MAX_DWELL_US) dwell = ENCH_MAX_DWELL_US;
        enchDwell[enchTargetCount] = dwell;
        enchTargetCount++;
        cand[best] = cand[--candCount];
    }
    enchRotate = 0;

    // One line per scan, and only when the picture changed, so the console
    // stays readable while the page is open (Developer Options ON only). This
    // is the only way to see what the scan actually heard without a radio
    // receiver pointed at the target.
    if (bruceConfig.usbDebug) {
        uint8_t top = enchTargetCount ? enchTargets[0] : 0;
        if (enchTargetCount != enchLastLoggedCount || top != enchLastLoggedTop) {
            enchLastLoggedCount = enchTargetCount;
            enchLastLoggedTop = top;
            Serial.printf(
                "[nrfjam] ench: %u targets, top CH %u (score %u)\n",
                (unsigned)enchTargetCount,
                (unsigned)top,
                (unsigned)(enchTargetCount ? enchScore[top] : 0)
            );
        }
    }

    uint8_t first = enchTargetCount ? enchTargets[0] : (m.count ? m.channels[0] : 50);
    NRFradio.startConstCarrier(RF24_PA_MAX, first);
}

// One step of the Enchanted engine: re-scan when due, then hold the carrier on
// the next target for its weighted dwell. If nothing was heard, fall back to a
// short dwell sweep of the whole list so a quiet-but-present target is covered.
static void enchantedTick(const NrfJamMode &m) {
    if ((uint32_t)(millis() - enchLastScan) >= ENCH_SCAN_MS) {
        enchScan(m);
        enchLastScan = millis();
    }

    if (enchTargetCount == 0) {
        if (enchFallback >= m.count) enchFallback = 0;
        NRFradio.setChannel(m.channels[enchFallback++]);
        delayMicroseconds(ENCH_MIN_DWELL_US);
    } else {
        size_t i = enchRotate % enchTargetCount;
        NRFradio.setChannel(enchTargets[i]);
        delayMicroseconds(enchDwell[i]);
        enchRotate++;
    }
}

void nrf_jammer() {
    int OnX = 0;
    uint8_t jamType = 0; // 0: Sequential, 1: FHSS, 2: Enchanted
    NRF24_MODE mode = nrf_setMode();
    int NRFOnline = 1;

    static const char *typeNames[] = {"Sequential", "FHSS      ", "Enchanted "};

    byte Test_channels[] = {50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 2,  4,  6,  8,
                            10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48};

    byte wifi_channels[] = {2, 7, 12, 17, 22, 27, 32, 37, 42, 47, 52, 57, 62, 67, 72, 77};

    byte ble_channels[] = {2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                           22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41};

    byte ble_adv_priority[] = {37, 38, 39, 1, 2, 3, 25, 26, 27, 79, 80, 81};

    byte bluetooth_channels[] = {2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
                                 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
                                 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
                                 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65,
                                 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80};
    byte usb_channels[] = {32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70};
    byte video_channels[] = {60, 62, 64, 66,  68,  70,  72,  74,  76,  78,  80,  82,  84,  86,  88,  90, 92,
                             94, 96, 98, 100, 102, 104, 106, 108, 110, 112, 114, 116, 118, 120, 122, 124};
    byte rc_channels[] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31, 33, 35, 37, 39};

    byte full_channels[] = {1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,
                            17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,
                            33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,
                            49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,
                            65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,
                            81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,
                            97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112,
                            113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124};
    byte zigbee_channels[] = {
        4,  5,  6,  // ch11
        9,  10, 11, // ch12
        14, 15, 16, // ch13
        19, 20, 21, // ch14
        24, 25, 26, // ch15
        29, 30, 31, // ch16
        34, 35, 36, // ch17
        39, 40, 41, // ch18
        44, 45, 46, // ch19
        49, 50, 51, // ch20
        54, 55, 56, // ch21
        59, 60, 61, // ch22
        64, 65, 66, // ch23
        69, 70, 71, // ch24
        74, 75, 76, // ch25
        79, 80, 81  // ch26
    };

    NrfJamMode modes[] = {
        {"Test        ", Test_channels,      sizeof(Test_channels) / sizeof(Test_channels[0])          },
        {"WiFi        ", wifi_channels,      sizeof(wifi_channels) / sizeof(wifi_channels[0])          },
        {"BLEch       ", ble_channels,       sizeof(ble_channels) / sizeof(ble_channels[0])            },
        {"BLE Adv Pri ", ble_adv_priority,   sizeof(ble_adv_priority) / sizeof(ble_adv_priority[0])    },
        {"Bluetooth   ", bluetooth_channels, sizeof(bluetooth_channels) / sizeof(bluetooth_channels[0])},
        {"USB         ", usb_channels,       sizeof(usb_channels) / sizeof(usb_channels[0])            },
        {"Video Stream", video_channels,     sizeof(video_channels) / sizeof(video_channels[0])        },
        {"RC          ", rc_channels,        sizeof(rc_channels) / sizeof(rc_channels[0])              },
        {"Zigbee      ", zigbee_channels,    sizeof(zigbee_channels) / sizeof(zigbee_channels[0])      },
        {"Full        ", full_channels,      sizeof(full_channels) / sizeof(full_channels[0])          }
    };

    if (nrf_start(mode)) {

        int modeIndex = 0;
        int hopIndex = 0;
        bool redraw = true;
        bool need_reshuffle = true;
        uint8_t shuffled_idx[128];
        int targetY = -1;
        uint32_t lastTargetDraw = 0;

        enchReset();

        if (CHECK_NRF_SPI(mode)) {
            NRFradio.setPALevel(RF24_PA_MAX);
            NRFradio.startConstCarrier(RF24_PA_MAX, 50);
            NRFradio.setAddressWidth(5);
            NRFradio.setPayloadSize(2);
            if (!NRFradio.setDataRate(RF24_2MBPS)) {
                Serial.println("Failed to set data rate to 2Mbps, trying 1Mbps");
                if (!NRFradio.setDataRate(RF24_1MBPS)) {
                    Serial.println("Failed to set data rate to 1Mbps, trying 250kbps");
                    if (!NRFradio.setDataRate(RF24_250KBPS)) {
                        Serial.println("Failed to set data rate to 250kbps, giving up");
                    }
                }
            }
        }

        drawMainBorder();

        NRFSerial.println("RADIOS");
        vTaskDelay(50 / portTICK_PERIOD_MS);

        while (true) {

            if ((CHECK_NRF_UART(mode)) || (CHECK_NRF_BOTH(mode))) {
                if (OnX == 0) {
                    NRFSerial.println("RADIOS");
                    vTaskDelay(50 / portTICK_PERIOD_MS);
                }

                if (NRFSerial.available()) {
                    String incomingNRFs = NRFSerial.readStringUntil('\n');
                    incomingNRFs.trim();
                    if (incomingNRFs.length() == 1 && isDigit(incomingNRFs.charAt(0))) {
                        OnX = 1;
                        NRFOnline = (incomingNRFs.toInt());
                        if (CHECK_NRF_BOTH(mode)) { NRFOnline = (incomingNRFs.toInt()) + 1; }
                        redraw = true;
                    }
                }
            }

            if (redraw) {
                drawMainBorderWithTitle("NRF JAMMER", false);
                printSubtitle("NRF function Jammer");
                padprintln("STATUS : " + String(NRFOnline) + " ACTIVE");
                String _modeName = String(modes[modeIndex].name) + "            ";
                _modeName = _modeName.substring(0, 13);
                padprintln("MODE : " + _modeName);
                padprintln("TYPE : " + String(typeNames[jamType]));
                // Remember the y of the target line so Enchanted can refresh it
                // in place (one line per second, not a full-page redraw).
                targetY = tft.getCursorY();
                tft.fillRect(0, targetY, tftWidth, tft.fontHeight(FP) + 2, bruceConfig.bgColor);
                padprintln("TGT  : " + (jamType == 2 ? enchTargetSummary() : String("sweep")));
                padprintln("");
                padprintln("> Switch Mode: Next/Prev");
                padprintln("> Type: Sel");
                padprintln("> Exit: Esc");

                tft.drawRoundRect(5, 5, tftWidth - 10, tftHeight - 10, 5, bruceConfig.priColor);
                if ((CHECK_NRF_UART(mode)) || (CHECK_NRF_BOTH(mode))) {
                    String Mode = modes[modeIndex].name;
                    Mode.replace(" ", "");
                    NRFSerial.println(Mode);
                }
                redraw = false;
                lastTargetDraw = millis();
            }

            if (jamType == 2) {
                // Enchanted runs its own scan/park loop; no per-iteration retune.
                enchantedTick(modes[modeIndex]);

                if (targetY >= 0 && (millis() - lastTargetDraw) >= 1000) {
                    String t = "TGT  : " + enchTargetSummary();
                    tft.fillRect(0, targetY, tftWidth, tft.fontHeight(FP) + 2, bruceConfig.bgColor);
                    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
                    tft.setTextSize(FP);
                    tft.setCursor(BORDER_PAD_X, targetY);
                    tft.print(t);
                    lastTargetDraw = millis();
                }
            } else {
                hopIndex++;
                if (hopIndex >= (int)modes[modeIndex].count) {
                    hopIndex = 0;
                    need_reshuffle = true;
                }
                uint8_t ch_idx;
                if (jamType == 1) {
                    if (need_reshuffle) {
                        size_t cnt = modes[modeIndex].count;
                        for (size_t i = 0; i < cnt; i++) shuffled_idx[i] = i;
                        shuffleChannels(shuffled_idx, cnt);
                        need_reshuffle = false;
                    }
                    ch_idx = shuffled_idx[hopIndex];
                } else {
                    ch_idx = hopIndex;
                }
                if (CHECK_NRF_SPI(mode)) { NRFradio.setChannel(modes[modeIndex].channels[ch_idx]); }
            }

            if (check(NextPress)) {
                modeIndex++;
                if (modeIndex >= (int)(sizeof(modes) / sizeof(modes[0]))) modeIndex = 0;
                hopIndex = 0;
                need_reshuffle = true;
                enchReset();
                redraw = true;
            }
            if (check(PrevPress)) {
                modeIndex--;
                if (modeIndex < 0) modeIndex = (sizeof(modes) / sizeof(modes[0])) - 1;
                hopIndex = 0;
                need_reshuffle = true;
                enchReset();
                redraw = true;
            }
            if (check(EscPress)) break;
            if (check(SelPress)) {
                jamType++;
                if (jamType > 2) jamType = 0;
                hopIndex = 0;
                need_reshuffle = true;
                enchReset();
                redraw = true;
            }
        }

        if (CHECK_NRF_SPI(mode)) NRFradio.stopConstCarrier();
        if ((CHECK_NRF_UART(mode)) || (CHECK_NRF_BOTH(mode))) { NRFSerial.println("OFF"); }

    } else {
        displayError("NRF24 not found", true);
    }
}
