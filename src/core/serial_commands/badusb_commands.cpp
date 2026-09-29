#include "badusb_commands.h"
#include "core/sd_functions.h"
#include "helpers.h"
#include "modules/badusb_ble/ducky_typer.h"

uint32_t badusbFileCallback(cmd *c) {
#ifndef LITE_VERSION
    // badusb run_from_file HelloWorld.txt

    Command cmd(c);

    Argument arg = cmd.getArgument("filepath");
    String filepath = arg.getValue();
    filepath.trim();

    if (filepath.indexOf(".txt") == -1) {
        serialDevice->println("Invalid filename");
        return false;
    }
    if (!filepath.startsWith("/")) filepath = "/" + filepath;

    FS *fs;
    if (!getFsStorage(fs)) return false;

    if (!(*fs).exists(filepath)) {
        serialDevice->println("File does not exist");
        return false;
    }

#ifdef USB_as_HID
    ducky_startKb(hid_usb, false);
    key_input(*fs, filepath, hid_usb);
    delete hid_usb;
    hid_usb = nullptr;

    // TODO: need to reinit serial when finished
    // Kb.end();
    // USB.~ESPUSB(); // Explicit call to destructor
    // serialDevice->begin(115200);

    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

uint32_t badusbBufferCallback(cmd *c) {
#ifndef LITE_VERSION
    if (!(_setupPsramFs())) return false;

    char *txt = _readFileFromSerial();
    String tmpfilepath = "/tmpramfile"; // TODO: Change to use char *txt directly
    File f = PSRamFS.open(tmpfilepath, FILE_WRITE);
    if (!f) return false;

    f.write((const uint8_t *)txt, strlen(txt));
    f.close();
    free(txt);

#ifdef USB_as_HID
    ducky_startKb(hid_usb, false);
    key_input(PSRamFS, tmpfilepath, hid_usb);
    delete hid_usb;
    hid_usb = nullptr;

    PSRamFS.remove(tmpfilepath);
    return true;
#else
    PSRamFS.remove(tmpfilepath);
    return false;
#endif
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// BadUSB configuration over the console. These mirror the on-device
// Others > BadUSB & HID > BadUSB Config menu so a headless session (USB
// Debugging mode) can set and verify the whole configuration.
// ---------------------------------------------------------------------------

static const char *badusbHidTypeNameCli(int t) {
    switch (t) {
        case 1: return "keyboard+mouse";
        case 2: return "mouse";
        default: return "keyboard";
    }
}

uint32_t badusbConfigCallback(cmd *c) {
#ifndef LITE_VERSION
    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%04X:%04X", bruceConfig.badUSBBLEVid, bruceConfig.badUSBBLEPid);
    serialDevice->println(String("layout=") + badusbLayoutName(bruceConfig.badUSBBLEKeyboardLayout) +
                          " (" + String(bruceConfig.badUSBBLEKeyboardLayout) + ")");
    serialDevice->println(String("usb_id=") + idbuf);
    serialDevice->println(String("manufacturer=") + bruceConfig.badUSBBLEManufacturer);
    serialDevice->println(String("product=") + bruceConfig.badUSBBLEProduct);
    serialDevice->println(
        String("serial=") + (bruceConfig.badUSBBLESerial.length() ? bruceConfig.badUSBBLESerial : String("(default)"))
    );
    serialDevice->println(String("hid=") + badusbHidTypeNameCli(bruceConfig.badUSBBLEHidType));
    serialDevice->println(String("key_delay=") + String(bruceConfig.badUSBBLEKeyDelay));
    serialDevice->println(String("string_delay=") + String(bruceConfig.badUSBBLEStringDelay));
    serialDevice->println(String("layout_file=") + bruceConfig.badUSBBLECustomLayoutFile);
    serialDevice->println(String("payload_dir=") + bruceConfig.badUSBBLEPayloadDir);
    serialDevice->println(
        String("default_payload=") +
        (bruceConfig.badUSBBLEDefaultPayload.length() ? bruceConfig.badUSBBLEDefaultPayload : String("(none)"))
    );
    serialDevice->println(String("show_output=") + (bruceConfig.badUSBBLEShowOutput ? "1" : "0"));
    return true;
#else
    return false;
#endif
}

uint32_t badusbSetCallback(cmd *c) {
#ifndef LITE_VERSION
    Command cmd(c);
    String field = cmd.getArgument("field").getValue();
    // The value is rebuilt from up to four trailing tokens so multi-word strings
    // ("Acme Wireless Keyboard", "Bruce BadUSB") can be set without quoting.
    String value = "";
    const char *valueArgs[] = {"value", "value2", "value3", "value4"};
    for (int i = 0; i < 4; i++) {
        String part = cmd.getArgument(valueArgs[i]).getValue();
        part.trim();
        if (!part.length()) continue;
        if (value.length()) value += " ";
        value += part;
    }
    field.trim();
    field.toLowerCase();

    if (field == "vid") {
        bruceConfig.setBadUSBBLEVid((uint16_t)strtol(value.c_str(), nullptr, 16));
    } else if (field == "pid") {
        bruceConfig.setBadUSBBLEPid((uint16_t)strtol(value.c_str(), nullptr, 16));
    } else if (field == "manufacturer") {
        bruceConfig.setBadUSBBLEManufacturer(value);
    } else if (field == "product") {
        bruceConfig.setBadUSBBLEProduct(value);
    } else if (field == "serial") {
        bruceConfig.setBadUSBBLESerial(value);
    } else if (field == "hid") {
        value.toLowerCase();
        if (value == "keyboard" || value == "kb")
            bruceConfig.setBadUSBBLEHidType(0);
        else if (value == "composite" || value == "kbm" || value == "keyboard+mouse")
            bruceConfig.setBadUSBBLEHidType(1);
        else if (value == "mouse")
            bruceConfig.setBadUSBBLEHidType(2);
        else {
            serialDevice->println("expected keyboard|composite|mouse");
            return false;
        }
    } else if (field == "layout") {
        int idx = -1;
        if (value.length() && isdigit((unsigned char)value[0])) {
            idx = value.toInt();
        } else {
            for (int i = 0; i < badusbLayoutCount(); i++) {
                if (value.equalsIgnoreCase(badusbLayoutName(i))) {
                    idx = i;
                    break;
                }
            }
        }
        if (idx < 0 || idx >= badusbLayoutCount()) {
            serialDevice->println("unknown layout (use index 0-15 or a name)");
            return false;
        }
        bruceConfig.setBadUSBBLEKeyboardLayout(idx);
    } else if (field == "keydelay") {
        bruceConfig.setBadUSBBLEKeyDelay((uint16_t)value.toInt());
    } else if (field == "stringdelay") {
        bruceConfig.setBadUSBBLEStringDelay((uint16_t)value.toInt());
    } else if (field == "payloaddir") {
        bruceConfig.setBadUSBBLEPayloadDir(value);
    } else if (field == "defaultpayload") {
        bruceConfig.setBadUSBBLEDefaultPayload(value);
    } else if (field == "layoutfile") {
        bruceConfig.setBadUSBBLECustomLayoutFile(value);
    } else if (field == "showoutput") {
        bruceConfig.setBadUSBBLEShowOutput(value == "1" || value.equalsIgnoreCase("on") || value.equalsIgnoreCase("true"));
    } else {
        serialDevice->println(
            "fields: vid pid manufacturer product serial hid layout keydelay stringdelay "
            "payloaddir defaultpayload layoutfile showoutput"
        );
        return false;
    }
    serialDevice->println("ok");
    return true;
#else
    return false;
#endif
}

uint32_t badusbLayoutImportCallback(cmd *c) {
#ifndef LITE_VERSION
    Command cmd(c);
    String path = cmd.getArgument("filepath").getValue();
    path.trim();
    if (path.length() == 0 || !path.startsWith("/")) path = "/" + path;
    if (!badusbLoadCustomLayoutFile(path)) {
        serialDevice->println("failed to parse layout file");
        return false;
    }
    bruceConfig.setBadUSBBLECustomLayoutFile(path);
    bruceConfig.setBadUSBBLEKeyboardLayout(BADUSB_LAYOUT_CUSTOM_INDEX);
    serialDevice->println("ok custom layout imported and selected");
    return true;
#else
    return false;
#endif
}

uint32_t badusbLayoutExportCallback(cmd *c) {
#ifndef LITE_VERSION
    if (!badusbSaveCustomLayoutFile(bruceConfig.badUSBBLECustomLayoutFile)) {
        serialDevice->println("failed to write layout file");
        return false;
    }
    serialDevice->println(String("ok ") + bruceConfig.badUSBBLECustomLayoutFile);
    return true;
#else
    return false;
#endif
}

uint32_t badusbLayoutCopyCallback(cmd *c) {
#ifndef LITE_VERSION
    Command cmd(c);
    int idx = cmd.getArgument("index").getValue().toInt();
    if (!badusbSeedCustomLayoutFrom(idx)) {
        serialDevice->println("invalid source layout (0-14)");
        return false;
    }
    bruceConfig.setBadUSBBLEKeyboardLayout(BADUSB_LAYOUT_CUSTOM_INDEX);
    serialDevice->println("ok custom layout seeded, select index 15 to use it");
    return true;
#else
    return false;
#endif
}

uint32_t badusbMouseCallback(cmd *c) {
#ifndef LITE_VERSION
#if defined(USB_as_HID)
    Command cmd(c);
    int dx = cmd.getArgument("dx").getValue().toInt();
    int dy = cmd.getArgument("dy").getValue().toInt();
    int wheel = cmd.getArgument("wheel").getValue().toInt();
    if (dx < -127) dx = -127;
    if (dx > 127) dx = 127;
    if (dy < -127) dy = -127;
    if (dy > 127) dy = 127;
    badusbMouseMove((int8_t)dx, (int8_t)dy, (int8_t)wheel);
    serialDevice->println("ok");
    return true;
#else
    serialDevice->println("USB HID not available on this board");
    return false;
#endif
#else
    return false;
#endif
}

uint32_t badusbClickCallback(cmd *c) {
#ifndef LITE_VERSION
#if defined(USB_as_HID)
    Command cmd(c);
    String button = cmd.getArgument("button").getValue();
    button.trim();
    button.toLowerCase();
    uint8_t b = 0x01; // left
    if (button == "right") b = 0x02;
    else if (button == "middle") b = 0x04;
    badusbMouseClick(b);
    serialDevice->println("ok");
    return true;
#else
    serialDevice->println("USB HID not available on this board");
    return false;
#endif
#else
    return false;
#endif
}

void createBadUsbCommands(SimpleCLI *cli) {
#ifndef LITE_VERSION
    Command badusbCmd = cli->addCompositeCmd("bu,badusb");

    Command fileCmd = badusbCmd.addCommand("run_from_file", badusbFileCallback);
    fileCmd.addPosArg("filepath");

    Command bufferCmd = badusbCmd.addCommand("run_from_buffer", badusbBufferCallback);

    badusbCmd.addCommand("config", badusbConfigCallback);

    Command setCmd = badusbCmd.addCommand("set", badusbSetCallback);
    setCmd.addPosArg("field");
    setCmd.addPosArg("value");
    setCmd.addPosArg("value2", "");
    setCmd.addPosArg("value3", "");
    setCmd.addPosArg("value4", "");

    Command importCmd = badusbCmd.addCommand("layout-import", badusbLayoutImportCallback);
    importCmd.addPosArg("filepath");

    badusbCmd.addCommand("layout-export", badusbLayoutExportCallback);

    Command copyCmd = badusbCmd.addCommand("layout-copy", badusbLayoutCopyCallback);
    copyCmd.addPosArg("index");

    Command mouseCmd = badusbCmd.addCommand("mouse", badusbMouseCallback);
    mouseCmd.addPosArg("dx");
    mouseCmd.addPosArg("dy");
    mouseCmd.addPosArg("wheel");

    Command clickCmd = badusbCmd.addCommand("click", badusbClickCallback);
    clickCmd.addPosArg("button");
#endif
}
