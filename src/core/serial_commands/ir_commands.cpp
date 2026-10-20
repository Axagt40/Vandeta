#include "ir_commands.h"
#include "core/sd_functions.h"
#include "helpers.h"
#include "modules/ir/custom_ir.h"
#include "modules/ir/ir_ac.h"
#include "modules/ir/ir_read.h"
#include "modules/ir/ir_universal.h"
#include <ArduinoJson.h>
#include <globals.h>

uint32_t irCallback(cmd *c) {
    serialDevice->println("Turning off IR LED");
    // The pin has to be claimed before the write: without this the console got
    // "IO 2 is not set as GPIO. Execute digitalMode(2, OUTPUT) first." and the
    // LED stayed on.
    pinMode(bruceConfigPins.irTx, OUTPUT);
    digitalWrite(bruceConfigPins.irTx, LED_OFF);
    return true;
}

uint32_t irRxCallback(cmd *c) {
    Command cmd(c);

    Argument arg = cmd.getArgument("raw");
    bool raw = arg.isSet();

    // `ir rx [timeout]` / `ir rx raw [timeout]` - the documented form. It used
    // to be rejected as an unknown argument and the window was hardcoded to
    // 10 s, so `ir rx 4` (as printed by `help`) could never work.
    int timeout = 10;
    Argument timeoutArg = cmd.getArgument("timeout");
    if (timeoutArg.isSet()) {
        long v = timeoutArg.getValue().toInt();
        if (v > 0) timeout = (int)min(v, 600L);
    }

    IrRead *i = NULL; // avoid calling the constructor here

    if (raw) {
        i = new IrRead(true, true); // true -> headless mode, true = raw mode
    } else {
        i = new IrRead(true); // true -> headless mode
    }

    serialDevice->println("Waiting for signal (" + String(timeout) + "s)...");
    String r = i->loop_headless(timeout);
    if (r.length() == 0) {
        delete i;
        return false;
    }

    serialDevice->println(r);
    delete i;
    return true;
}

uint32_t irTxCallback(cmd *c) {
    // usage: ir tx <protocol> <address without spaces> <command without spaces>
    // e.g. ir tx NEC 04000000 08000000

    Command cmd(c);

    Argument prtcArg = cmd.getArgument("protocol");
    Argument addrArg = cmd.getArgument("address");
    Argument cmdArg = cmd.getArgument("command");
    String protocol = prtcArg.getValue();
    String address = addrArg.getValue();
    String command = cmdArg.getValue();
    protocol.trim();
    address.trim();
    command.trim();

    if (address.length() != 8 || command.length() != 8) {
        serialDevice->println("Address and command must be 8 characters long");
        return false;
    }

    IRCode code;
    code.type = "parsed";
    code.protocol = protocol;
    code.address = address;
    code.command = command;

    sendIRCommand(&code);
    return true;
}

uint32_t irTxRawCallback(cmd *c) {
    // usage: ir tx_raw <frequency> <samples>

    Command cmd(c);

    Argument freqArg = cmd.getArgument("frequency");
    Argument samplesArg = cmd.getArgument("samples");
    String freqStr = freqArg.getValue();
    String samples = samplesArg.getValue();
    freqStr.trim();
    samples.trim();

    uint32_t frequency = freqStr.toInt();

    if (frequency == 0) {
        serialDevice->println("Invalid frequency: " + String(frequency));
        return false;
    }

    if (samples.length() == 0) {
        serialDevice->println("Missing data samples");
        return false;
    }

    IRCode code;
    code.type = "raw";
    code.frequency = frequency;
    code.data = samples;

    sendIRCommand(&code);
    return true;
}

uint32_t irTxFileCallback(cmd *c) {
    // example: ir tx_from_file LG_AKB72915206_power.ir false

    Command cmd(c);

    Argument filepathArg = cmd.getArgument("filepath");
    Argument hideDefaultUIArg = cmd.getArgument("hideDefaultUI");
    String filepath = filepathArg.getValue();
    String hideDefaultUI = hideDefaultUIArg.getValue();
    filepath.trim();

    if (filepath.indexOf(".ir") == -1) {
        serialDevice->println("Invalid file");
        return false;
    }

    if (!filepath.startsWith("/")) filepath = "/" + filepath;

    FS *fs;
    if (!getFsStorage(fs)) return false;

    if (!(*fs).exists(filepath)) {
        serialDevice->println("File does not exist");
        return false;
    }

    return txIrFile(fs, filepath, hideDefaultUI);
}

uint32_t irTxBufferCallback(cmd *c) {
#ifndef LITE_VERSION
    if (!(_setupPsramFs())) return false;

    char *txt = _readFileFromSerial();
    String tmpfilepath = "/tmpramfile"; // TODO: Change to use char *txt directly
    File f = PSRamFS.open(tmpfilepath, FILE_WRITE);
    if (!f) return false;

    f.write((const uint8_t *)txt, strlen(txt));
    f.close();
    free(txt);

    bool r = txIrFile(&PSRamFS, tmpfilepath);
    PSRamFS.remove(tmpfilepath);

    return r;
#else
    return false;
#endif
}

// --- universal remotes -----------------------------------------------------
// The same library + sweep the IR > Universal Remote menu uses, exposed on the
// console so a sweep can be measured instead of watched: `ir uni send tv Power 5`.

static const IrUniversalCategory *irUniFind(const String &name) {
    for (size_t i = 0; i < IR_UNIVERSAL_CATEGORY_COUNT; i++) {
        const IrUniversalCategory &c = IR_UNIVERSAL_CATEGORIES[i];
        if (name.equalsIgnoreCase(c.label) || name.equalsIgnoreCase(c.file)) return &c;
    }
    return nullptr;
}

uint32_t irUniversalListCallback(cmd *c) {
    FS *fs = nullptr;
    if (!getFsStorage(fs)) {
        serialDevice->println("no storage");
        return false;
    }
    serialDevice->println("universal libraries in " IR_UNIVERSAL_DIR ":");
    for (size_t i = 0; i < IR_UNIVERSAL_CATEGORY_COUNT; i++) {
        const IrUniversalCategory &cat = IR_UNIVERSAL_CATEGORIES[i];
        String path = irUniversalPath(cat.file);
        if (!irUniversalAvailable(fs, cat.file)) {
            serialDevice->println("  " + String(cat.label) + "\t" + cat.file + "\tmissing");
            continue;
        }
        std::vector<String> buttons = irUniversalButtons(fs, path);
        serialDevice->println(
            "  " + String(cat.label) + "\t" + cat.file + "\t" + String((int)buttons.size()) + " buttons"
        );
    }
    return true;
}

uint32_t irUniversalButtonsCallback(cmd *c) {
    Command cmd(c);
    String name = cmd.getArgument("category").getValue();
    const IrUniversalCategory *cat = irUniFind(name);
    if (cat == nullptr) {
        serialDevice->println("unknown category: " + name + " (tv, ac, fans, audio, projectors, leds)");
        return false;
    }
    FS *fs = nullptr;
    if (!getFsStorage(fs) || !irUniversalAvailable(fs, cat->file)) {
        serialDevice->println(String(cat->label) + " library missing");
        return false;
    }
    std::vector<String> buttons = irUniversalButtons(fs, irUniversalPath(cat->file));
    serialDevice->println(String(cat->label) + ": " + String((int)buttons.size()) + " buttons");
    for (const String &b : buttons) serialDevice->println("  " + b);
    return true;
}

uint32_t irUniversalSendCallback(cmd *c) {
    Command cmd(c);
    String name = cmd.getArgument("category").getValue();
    String button = cmd.getArgument("name").getValue();
    int maxSends = cmd.getArgument("max").getValue().toInt();

    const IrUniversalCategory *cat = irUniFind(name);
    if (cat == nullptr) {
        serialDevice->println("unknown category: " + name);
        return false;
    }
    FS *fs = nullptr;
    if (!getFsStorage(fs) || !irUniversalAvailable(fs, cat->file)) {
        serialDevice->println(String(cat->label) + " library missing");
        return false;
    }
    int sent = irUniversalSend(fs, irUniversalPath(cat->file), button, maxSends, false, nullptr);
    if (sent == 0) {
        serialDevice->println("no code named '" + button + "' in " + cat->file);
        return false;
    }
    return true;
}

uint32_t irUniversalEncodeCallback(cmd *c) {
    Command cmd(c);
    String protocol = cmd.getArgument("protocol").getValue();
    uint32_t address = strtoul(cmd.getArgument("address").getValue().c_str(), nullptr, 16);
    uint32_t command = strtoul(cmd.getArgument("code").getValue().c_str(), nullptr, 16);

    String frame = irUniversalRawFrame(protocol, address, command);
    if (frame.length() == 0) {
        serialDevice->println("no hand-written encoder for " + protocol + " (RCA, NEC42 only)");
        return false;
    }
    serialDevice->println(frame);
    return true;
}

// `ir ac list` / `ir ac send <brand> <mode> <temp> <fan>` - drives the same
// per-brand AC encoders the Universal Remote > AC > Brand remotes page uses, so
// every make can be exercised (and its frame measured) without a remote.
uint32_t irAcListCallback(cmd *c) {
    for (int i = 0; i < irAcBrandCount(); i++) {
        serialDevice->println(String(i) + "\t" + irAcBrandName(i));
    }
    return true;
}

uint32_t irAcSendCallback(cmd *c) {
    Command cmd(c);
    String brandArg = cmd.getArgument("brand").getValue();
    String modeArg = cmd.getArgument("mode").getValue();
    String tempArg = cmd.getArgument("temp").getValue();
    String fanArg = cmd.getArgument("fan").getValue();
    brandArg.trim();
    modeArg.trim();
    tempArg.trim();
    fanArg.trim();

    int brand = -1;
    if (brandArg.length() && isdigit((unsigned char)brandArg[0])) brand = brandArg.toInt();
    else {
        for (int i = 0; i < irAcBrandCount(); i++) {
            if (brandArg.equalsIgnoreCase(irAcBrandName(i))) {
                brand = i;
                break;
            }
        }
    }
    if (brand < 0 || brand >= irAcBrandCount()) {
        serialDevice->println("unknown brand (ir ac list)");
        return false;
    }

    IrAcState st;
    if (modeArg.equalsIgnoreCase("cool")) st.mode = 0;
    else if (modeArg.equalsIgnoreCase("heat")) st.mode = 1;
    else if (modeArg.equalsIgnoreCase("dry")) st.mode = 2;
    else if (modeArg.equalsIgnoreCase("fan")) st.mode = 3;
    else if (modeArg.equalsIgnoreCase("auto")) st.mode = 4;
    else if (modeArg.length()) st.mode = (uint8_t)modeArg.toInt();

    if (tempArg.length()) st.temp = (uint8_t)constrain(tempArg.toInt(), 16, 30);

    if (fanArg.equalsIgnoreCase("auto")) st.fan = 0;
    else if (fanArg.equalsIgnoreCase("low")) st.fan = 1;
    else if (fanArg.equalsIgnoreCase("med")) st.fan = 2;
    else if (fanArg.equalsIgnoreCase("high")) st.fan = 3;
    else if (fanArg.length()) st.fan = (uint8_t)constrain(fanArg.toInt(), 0, 3);

    if (!irAcSend(brand, st)) {
        serialDevice->println("encode failed");
        return false;
    }
    serialDevice->println(
        String("sent ") + irAcBrandName(brand) + " mode=" + String(st.mode) + " temp=" + String(st.temp) +
        " fan=" + String(st.fan)
    );
    return true;
}

void createIrAcCommands(Command *irCmd) {
    Command ac = irCmd->addCompositeCmd("ac");
    ac.addCommand("list", irAcListCallback);
    Command send = ac.addCommand("send", irAcSendCallback);
    send.addPosArg("brand");
    send.addPosArg("mode", "cool");
    send.addPosArg("temp", "24");
    send.addPosArg("fan", "auto");
}

void createIrUniversalCommands(Command *irCmd) {
    Command uni = irCmd->addCompositeCmd("uni");
    uni.addCommand("list", irUniversalListCallback);
    Command encode = uni.addCommand("encode", irUniversalEncodeCallback);
    encode.addPosArg("protocol");
    encode.addPosArg("address");
    encode.addPosArg("code");
    Command buttons = uni.addCommand("buttons", irUniversalButtonsCallback);
    buttons.addPosArg("category");
    Command send = uni.addCommand("send", irUniversalSendCallback);
    send.addPosArg("category");
    send.addPosArg("name");
    send.addPosArg("max", "0");
}

uint32_t irSendCallback(cmd *c) {
    // tasmota json command  https://tasmota.github.io/docs/Tasmota-IR/#sending-ir-commands
    // e.g. IRSend {\"Protocol\":\"NEC\",\"Bits\":32,\"Data\":\"0x20DF10EF\"}
    // TODO: decode "data" into "address+command" and use existing "send*Command" funcs

    Command cmd(c);

    Argument args = cmd.getArgument(0);
    String args_str = args.getValue();
    args_str.trim();
    // serialDevice->println(command);

    JsonDocument jsonDoc;
    if (deserializeJson(jsonDoc, args_str)) {
        serialDevice->println("Failed to parse json");
        serialDevice->println(args_str);
        return false;
    }

    JsonObject args_json = jsonDoc.as<JsonObject>(); // root

    uint16_t bits = 32;         // defaults to 32 bits
    String protocolStr = "nec"; // defaults to NEC protocol
    String dataStr = "";

    if (args_json["Data"].isNull()) {
        serialDevice->println("json missing data field");
        return false;
    } else {
        dataStr = args_json["Data"].as<String>();
    }

    if (!args_json["Protocol"].isNull()) protocolStr = args_json["Protocol"].as<String>();

    if (!args_json["Bits"].isNull()) bits = args_json["Bits"].as<int>();

    return sendDecodedCommand(protocolStr, dataStr, bits);
}

void createIrRxCommand(Command *irCmd) {
    Command cmd = irCmd->addCommand("rx", irRxCallback);
    cmd.addFlagArg("raw");
    cmd.addPosArg("timeout", "10");
}

void createIrTxCommand(Command *irCmd) {
    Command cmd = irCmd->addCommand("tx", irTxCallback);
    cmd.addPositionalArgument("protocol");
    cmd.addPositionalArgument("address");
    cmd.addPositionalArgument("command");
}

void createIrTxRawCommand(Command *irCmd) {
    Command cmd = irCmd->addCommand("tx_raw", irTxRawCallback);
    cmd.addPositionalArgument("frequency");
    cmd.addPositionalArgument("samples");
}

void createIrTxFileCommand(Command *irCmd) {
    Command cmd = irCmd->addCommand("tx_from_file", irTxFileCallback);
    cmd.addPositionalArgument("filepath");
    cmd.addPositionalArgument("hideDefaultUI", "false");
}

void createIrTxBufferCommand(Command *irCmd) {
    Command cmd = irCmd->addCommand("tx_from_buffer", irTxBufferCallback);
}

void createIrCommands(SimpleCLI *cli) {
    Command cmd = cli->addCompositeCmd("ir", irCallback);

    createIrRxCommand(&cmd);
    createIrTxCommand(&cmd);
    createIrTxRawCommand(&cmd);
    createIrTxFileCommand(&cmd);
    createIrTxBufferCommand(&cmd);
    createIrUniversalCommands(&cmd);
    createIrAcCommands(&cmd);

    cli->addSingleArgCmd("IRSend", irSendCallback);
}
