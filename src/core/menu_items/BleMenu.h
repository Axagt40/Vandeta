#ifndef __BLE_MENU_H__
#define __BLE_MENU_H__

#include <MenuItemInterface.h>

class BleMenu : public MenuItemInterface {
public:
    BleMenu() : MenuItemInterface("BLE") {}

    void optionsMenu(void);
    void drawIcon(float scale);
    bool hasTheme() { return bruceConfig.theme.ble; }
    const String& themePath() override { return bruceConfig.theme.paths.ble; }

private:
    // Grouped front page: 13 flat rows mixed scanning, attacks, HID and
    // device tools. The two attack pages (BLE Suite, BLE Atks) now sit
    // together under Attacks instead of reading as two unrelated buttons.
    void scanMenu(void);
    void attacksMenu(void);
    void hidMenu(void);
    void toolsMenu(void);
    void configMenu(void);
    void setBleNameMenu(void);
};

#endif
