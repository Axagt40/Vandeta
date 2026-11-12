#include "watch_commands.h"

#include "modules/wifi/countersurv.h"
#include "modules/wifi/eapol_ident.h"
#include <globals.h>

// Both verbs are ordinary user features (not developer-gated): a blue-team
// screen is the point of the task, so it must work on a stock build.
static uint32_t bluewatchCallback(cmd *c) {
    Command cmd(c);
    int seconds = cmd.getArgument(0).getValue().toInt();
    if (seconds <= 0) seconds = 20;
    if (seconds > 300) seconds = 300;
    serialDevice->println("bluewatch: capturing " + String(seconds) + "s on 1-13...");
    serialDevice->println(CounterSurv::run(seconds, false));
    return true;
}

static uint32_t wififpCallback(cmd *c) {
    Command cmd(c);
    int seconds = cmd.getArgument(0).getValue().toInt();
    if (seconds <= 0) seconds = 20;
    if (seconds > 300) seconds = 300;
    serialDevice->println("wififp: capturing probe requests for " + String(seconds) + "s...");
    serialDevice->println(CounterSurv::run(seconds, true));
    return true;
}

// Task 3, BLE half: `blecadence [secs]`. Same analysis the dashboard page
// shows - one payload, many rotating addresses, and how fast they rotate.
static uint32_t blecadenceCallback(cmd *c) {
    Command cmd(c);
    int seconds = cmd.getArgument(0).getValue().toInt();
    if (seconds <= 0) seconds = 20;
    if (seconds > 300) seconds = 300;
    serialDevice->println("blecadence: scanning " + String(seconds) + "s...");
    serialDevice->println(CounterSurv::bleCadence(seconds));
    return true;
}

// Task 5, 802.1X half: `eapolid [secs]` harvests EAPOL-Start frames and clear
// EAP Response/Identity fields (outer identity + requested method).
static uint32_t eapolidCallback(cmd *c) {
    Command cmd(c);
    int seconds = cmd.getArgument(0).getValue().toInt();
    if (seconds <= 0) seconds = 20;
    if (seconds > 120) seconds = 120;
    serialDevice->println("eapolid: listening " + String(seconds) + "s on 1-13...");
    serialDevice->println(EapolIdent::harvest(seconds));
    return true;
}

void createWatchCommands(SimpleCLI *cli) {
    Command bw = cli->addCommand("bluewatch", bluewatchCallback);
    bw.addPosArg("seconds", "20");
    Command fp = cli->addCommand("wififp", wififpCallback);
    fp.addPosArg("seconds", "20");
    Command bc = cli->addCommand("blecadence", blecadenceCallback);
    bc.addPosArg("seconds", "20");
    Command ei = cli->addCommand("eapolid", eapolidCallback);
    ei.addPosArg("seconds", "20");
}
