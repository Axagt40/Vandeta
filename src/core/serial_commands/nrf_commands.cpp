#include "nrf_commands.h"
#include "helpers.h"
#include "modules/NRF24/nrf_capture.h"
#include "modules/NRF24/nrf_common.h"
#include <globals.h>

// Console driver for the NRF24 raw capture / replay / analysis engine so it can
// be exercised over the Developer Options console without the on-device menu.
//
//   nrfcap info
//   nrfcap init [channel]
//   nrfcap promisc <0|1>
//   nrfcap channel <n>
//   nrfcap rate <0|1|2>
//   nrfcap power <0..3>
//   nrfcap freqrange <startMhz> <endMhz>
//   nrfcap scan [dwellMs]
//   nrfcap capture <ms> [hop]
//   nrfcap multifreq [ms]
//   nrfcap list
//   nrfcap replay <hex> [channel] [repeat]
//   nrfcap save <name>
//   nrfcap load <name>
//   nrfcap analyze <hex>

static String nrfHex(const uint8_t *data, uint8_t len) {
    String hex;
    for (int i = 0; i < len; i++) {
        char h[3];
        snprintf(h, sizeof(h), "%02X", data[i]);
        hex += h;
    }
    return hex;
}

static int nrfParseHexLocal(const char *s, uint8_t *out, int maxLen) {
    int n = 0, hi = -1;
    for (const char *p = s; *p && n < maxLen; p++) {
        char c = *p;
        if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p++;
            continue;
        }
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;
        if (hi < 0) hi = v;
        else {
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return n;
}

static bool nrfEnsure(uint8_t channel) {
    if (nrfCaptureReady()) return true;
    NrfCaptureConfig cfg;
    cfg.startChannel = channel;
    cfg.endChannel = 125;
    return nrfCaptureInit(cfg);
}

uint32_t nrfCapCallback(cmd *c) {
    Command cmd(c);
    String mode = cmd.getArgument(0).getValue();
    mode.trim();
    mode.toLowerCase();
    String a = cmd.getArgument(1).getValue();
    String b = cmd.getArgument(2).getValue();
    String d = cmd.getArgument(3).getValue();
    a.trim();
    b.trim();
    d.trim();

    if (mode == "info") {
        if (!nrfEnsure(76)) {
            serialDevice->println("ERR NRF24 not connected");
            return false;
        }
        serialDevice->println(
            String("chip=") + (NRFradio.isChipConnected() ? "ok" : "absent") + " channel=" +
            String(nrfCaptureGetChannel()) + " freq=" + String(2400 + nrfCaptureGetChannel()) + " rate=" +
            String(nrfCaptureGetDataRate()) + " power=" + String(3) + " promisc=" +
            (nrfCaptureIsPromiscuous() ? "1" : "0") + " range=" + String(nrfCaptureStartFreq()) + "-" +
            String(nrfCaptureEndFreq()) + " packets=" + String(nrfCaptureCount()) + " folder=" +
            nrfCaptureFolder()
        );
        return true;
    }
    if (!nrfEnsure(76)) {
        serialDevice->println("ERR NRF24 not connected");
        return false;
    }

    if (mode == "init") {
        uint8_t ch = a.length() ? (uint8_t)a.toInt() : 76;
        NrfCaptureConfig cfg;
        cfg.startChannel = ch;
        cfg.endChannel = 125;
        serialDevice->println(String("init=") + (nrfCaptureInit(cfg) ? "ok" : "fail"));
        return true;
    }
    if (mode == "promisc") {
        bool on = a == "1" || a.equalsIgnoreCase("on") || a.equalsIgnoreCase("true");
        serialDevice->println(String("promisc=") + (nrfCaptureSetPromiscuous(on) ? "ok" : "fail"));
        return true;
    }
    if (mode == "channel") {
        nrfCaptureSetChannel((uint8_t)a.toInt());
        serialDevice->println(String("channel=") + String(nrfCaptureGetChannel()));
        return true;
    }
    if (mode == "rate") {
        nrfCaptureSetDataRate((uint8_t)a.toInt());
        serialDevice->println(String("rate=") + String(nrfCaptureGetDataRate()));
        return true;
    }
    if (mode == "power") {
        nrfCaptureSetPower((uint8_t)a.toInt());
        serialDevice->println(String("power=") + a);
        return true;
    }
    if (mode == "freqrange") {
        nrfCaptureSetFrequencyRange((uint16_t)a.toInt(), (uint16_t)b.toInt());
        serialDevice->println(
            String("range=") + String(nrfCaptureStartFreq()) + "-" + String(nrfCaptureEndFreq())
        );
        return true;
    }
    if (mode == "scan") {
        uint32_t dwell = a.length() ? a.toInt() : 2;
        static NrfChannelActivity act[126];
        int n = nrfScanChannelActivity(0, 125, dwell, act, 126);
        int busy = 0;
        for (int i = 0; i < n; i++) {
            if (act[i].level == 0) continue;
            busy++;
            serialDevice->println(String("ch=") + String(act[i].channel) + " freq=" + String(act[i].freqMhz) + " level=" + String(act[i].level));
        }
        serialDevice->println(String("busy=") + String(busy));
        return true;
    }
    if (mode == "capture") {
        uint32_t ms = a.length() ? a.toInt() : 3000;
        bool hop = b == "1" || b.equalsIgnoreCase("hop");
        int n = nrfCaptureRun(ms, hop);
        serialDevice->println(
            String("RESULT capture count=") + String(n) + " dropped=" + String(nrfCaptureDropped()) + " hop=" +
            (hop ? "1" : "0")
        );
        for (int i = 0; i < n && i < 16; i++) {
            NrfCapturedPacket p;
            if (!nrfCapturePacket(i, &p)) continue;
            serialDevice->println(
                String("pkt i=") + String(i) + " ch=" + String(p.channel) + " freq=" + String(p.freqMhz) +
                " len=" + String(p.len) + " ts=" + String(p.timestamp) + " data=" + nrfHex(p.data, p.len)
            );
        }
        return true;
    }
    if (mode == "multifreq") {
        uint32_t ms = a.length() ? a.toInt() : 4000;
        nrfCaptureRun(ms, true);
        uint8_t chans[8];
        int n = nrfFilterMultiFreqChannels(chans, 8);
        for (int i = 0; i < n; i++)
            serialDevice->println(String("signal ch=") + String(chans[i]) + " freq=" + String(2400 + chans[i]));
        serialDevice->println(
            String("RESULT multifreq signals=") + String(n) + " dualFreq=" + (n >= 2 ? "1" : "0")
        );
        return true;
    }
    if (mode == "list") {
        for (int i = 0; i < nrfCaptureCount(); i++) {
            NrfCapturedPacket p;
            if (!nrfCapturePacket(i, &p)) continue;
            serialDevice->println(
                String("pkt i=") + String(i) + " ch=" + String(p.channel) + " freq=" + String(p.freqMhz) +
                " len=" + String(p.len) + " data=" + nrfHex(p.data, p.len)
            );
        }
        serialDevice->println(String("count=") + String(nrfCaptureCount()));
        return true;
    }
    if (mode == "replay") {
        uint8_t bytes[NRF_CAP_MAX_PAYLOAD];
        int len = nrfParseHexLocal(a.c_str(), bytes, NRF_CAP_MAX_PAYLOAD);
        if (len <= 0) {
            serialDevice->println("ERR bad hex payload");
            return false;
        }
        int channel = b.length() ? b.toInt() : nrfCaptureGetChannel();
        int repeat = d.length() ? d.toInt() : 3;
        bool ok = nrfReplayRaw(bytes, (uint8_t)len, (uint8_t)channel, (uint8_t)repeat, nrfCaptureGetDataRate());
        serialDevice->println(String("RESULT replay ok=") + (ok ? "1" : "0") + " ch=" + String(channel) + " bytes=" + String(len));
        return true;
    }
    if (mode == "save") {
        if (nrfCaptureCount() == 0) {
            serialDevice->println("ERR nothing captured");
            return false;
        }
        static NrfCapturedPacket pkts[NRF_CAP_MAX_PACKETS];
        int got = 0;
        for (int i = 0; i < nrfCaptureCount() && got < NRF_CAP_MAX_PACKETS; i++)
            if (nrfCapturePacket(i, &pkts[got])) got++;
        bool ok = nrfSaveSignal(a, pkts, got, "nrfcap");
        serialDevice->println(String("RESULT save ok=") + (ok ? "1" : "0") + " packets=" + String(got) + " name=" + a);
        return true;
    }
    if (mode == "load") {
        static NrfCapturedPacket pkts[NRF_CAP_MAX_PACKETS];
        int n = nrfLoadSignal(a, pkts, NRF_CAP_MAX_PACKETS, nullptr);
        int kept = nrfCaptureLoad(pkts, n);
        uint8_t chans[4];
        int nc = nrfChooseReplayChannels(pkts, kept, chans, 4);
        serialDevice->println(String("RESULT load packets=") + String(kept) + " channels=" + String(nc));
        for (int i = 0; i < nc; i++) serialDevice->println(String("  ch=") + String(chans[i]) + " freq=" + String(2400 + chans[i]));
        return true;
    }
    if (mode == "analyze") {
        uint8_t bytes[NRF_CAP_MAX_PAYLOAD];
        int len = nrfParseHexLocal(a.c_str(), bytes, NRF_CAP_MAX_PAYLOAD);
        NrfSignalAnalysis an = nrfAnalyzeSignal(bytes, (uint8_t)len);
        serialDevice->println(
            String("RESULT analyze len=") + String(an.len) + " unique=" + String(an.uniqueBytes) +
            " transitions=" + String(an.bitTransitions) + " min=" + String(an.minByte) + " max=" +
            String(an.maxByte) + " repeats=" + (an.repeats ? "1" : "0") + " codeLen=" +
            String(an.repeatLength) + " kind=" + an.kind
        );
        return true;
    }
    serialDevice->println(
        "modes: info init promisc channel rate power freqrange scan capture multifreq list replay save load "
        "analyze"
    );
    return false;
}

void createNrfCommands(SimpleCLI *cli) {
    Command cmd = cli->addCommand("nrfcap,nrf", nrfCapCallback);
    cmd.addPosArg("mode");
    cmd.addPosArg("a", "");
    cmd.addPosArg("b", "");
    cmd.addPosArg("c", "");
}
