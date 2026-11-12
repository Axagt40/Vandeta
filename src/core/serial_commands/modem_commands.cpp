#include "modem_commands.h"

#include "modules/others/audio_modem.h"
#include <globals.h>

static uint32_t modemCallback(cmd *c) {
    Command cmd(c);
    String mode = cmd.getArgument(0).getValue();
    mode.trim();
    if (mode == "send") {
        String text = cmd.getArgument(1).getValue();
        const int baud = cmd.getArgument(2).getValue().toInt();
        String ultra = cmd.getArgument(3).getValue();
        ultra.trim();
        String msg;
        const bool ok = AudioModem::send(text, baud > 0 ? baud : 1200, ultra == "ultrasonic" || ultra == "1", msg);
        serialDevice->println(ok ? "OK " + msg : "ERR: " + msg);
        return true;
    }
    if (mode == "recv") {
        int secs = cmd.getArgument(1).getValue().toInt();
        if (secs <= 0) secs = 10;
        String msg;
        const bool ok = AudioModem::receive(secs, msg);
        serialDevice->println(ok ? "OK " + msg : "ERR: " + msg);
        return true;
    }
    serialDevice->println("ERR: modem send <text> [baud] [ultrasonic] | modem recv <secs>");
    return true;
}

void createModemCommands(SimpleCLI *cli) {
    Command modem = cli->addCommand("modem", modemCallback);
    modem.addPosArg("mode", "");
    modem.addPosArg("a", "");
    modem.addPosArg("b", "");
    modem.addPosArg("c", "");
}
