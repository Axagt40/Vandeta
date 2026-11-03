#ifndef __SERIAL_CMDS_H__
#define __SERIAL_CMDS_H__

#include <Arduino.h>

extern TaskHandle_t serialcmdsTaskHandle;

void startSerialCommandsHandlerTask(bool initQueues = false);

bool parseSerialCommand(const String &command, bool waitForResponse = true);

// A page that owns the USB port for its own protocol (the USB Ethernet bridge
// carries raw frames on it) suspends the console, so the CLI task cannot read
// those bytes as a command line. While suspended the task only runs the
// Developer Options housekeeping.
void setSerialConsoleSuspended(bool suspended);
bool serialConsoleSuspended();
#endif
