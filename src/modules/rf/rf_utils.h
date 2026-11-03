#ifndef __RF_UTILS_H__
#define __RF_UTILS_H__

#include "protocols/rf_keeloq.h" // KeeLoq cipher/keystore + bitAt/g5/KEELOQ_* macros
#include "structs.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
// ESP-IDF 5.5 based framework determines the channels autommatically
// you do not have the hability to choose the channel
// `memSymbols` is the RX buffer size in RMT symbols (2 durations each). The
// default matches the driver's smallest block; callers that must not lose a
// pulse mid-burst can ask for more, and get NULL if the hardware cannot give it.
rmt_channel_handle_t setup_rf_rx(int memSymbols = 64);

#define RMT_MAX_PULSES 10000 // Maximum number of pulses to record
#define RMT_CLK_DIV 80       /*!< RMT counter clock divider */
#define RMT_1US_TICKS (80000000 / RMT_CLK_DIV / 1000000)
#define RMT_1MS_TICKS (RMT_1US_TICKS * 1000)
#define SIGNAL_STRENGTH_THRESHOLD 1500 // Adjust this threshold as needed

extern const float subghz_frequency_list[57];
extern const char *subghz_frequency_ranges[];
extern const int range_limits[4][2];
extern bool rmtInstalled;
// Number of live RMT RX sessions (scan, record, spectrum, listen, wardriver).
// GDO0 is the same pin the transmitter drives, so a transmit that starts while
// one of these is armed re-plans the pin under the running RMT channel - on this
// chip that shows up as `Guru Meditation: Cache error / MMU entry fault` and a
// reboot, not as a clean error. sendRfCommand() refuses in that case.
extern volatile int rfRxSessions;
bool rfRadioReceiving();

bool initRfModule(String mode = "", float frequency = 0);
void deinitRfModule();
void initCC1101once(SPIClass *SSPI);

// Serializes CC1101 SPI traffic against the display's SPI transactions.
//
// On most boards the CC1101 is wired to the same pins as the TFT, so
// acquireSPIBus() hands the RF layer the display's own SPIClass instance. The
// SmartRC driver opens/closes a transaction around every register access
// (SpiStart()/SpiEnd()), i.e. it calls beginTransaction()/endTransaction() on a
// shared instance. If a console command (serial command task) does that while
// the render task is in the middle of a draw, endTransaction() releases an SPI
// parameter mutex that belongs to the OTHER task - FreeRTOS asserts
// (xTaskPriorityDisinherit) and the board reboots. That is exactly what
// `subghz scan` used to do. Every RF entry point wraps its CC1101 access in
// these two helpers: the outermost lock also claims the display's transaction
// (tft.startWrite()), so RF and rendering can never interleave. Nested calls are
// cheap (recursive), so callers may lock coarsely.
void rf_spi_lock();
void rf_spi_unlock();

void setMHZ(float frequency);
// getRssi() is one SPI burst as well; go through this wrapper rather than
// calling ELECHOUSE_cc1101.getRssi() directly from a non-render task.
int rfGetRssi();
// setRxBW() is a register write on the same shared bus. It has to go through
// this wrapper for the same reason as rfGetRssi(): a page running in the UI task
// and a console command in the serial task both touch the CC1101, and an
// unlocked write opens an SPI transaction on the display's bus, which the other
// task then closes - FreeRTOS asserts in xTaskPriorityDisinherit and reboots.
void rfSetRxBW(float bandwidth);
// True while any RMT RX session is armed (see rfRxSessions).
bool rfRadioReceiving();
int find_pulse_index(const std::vector<int> &indexed_durations, int duration);
uint64_t crc64_ecma(const std::vector<int> &data);

void addToRecentCodes(struct RfCodes rfcode);
struct RfCodes selectRecentRfMenu();
bool setMHZMenu();
void rf_range_selection(float currentFrequency = 0.0);

uint64_t reverse_bits(uint64_t num, uint8_t bits);

String rf_subghz_header(float frequencyMHz);

#endif
