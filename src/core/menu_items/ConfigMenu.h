#ifndef __CONFIG_MENU_H__
#define __CONFIG_MENU_H__

#include <MenuItemInterface.h>

class ConfigMenu : public MenuItemInterface {
public:
    ConfigMenu() : MenuItemInterface("Config") {}

    void optionsMenu(void);
    void drawIcon(float scale);
    bool hasTheme() { return bruceConfig.theme.config; }
    const String &themePath() override { return bruceConfig.theme.paths.config; }

private:
    // Submenus
    void displayUIMenu(void);
    void ledMenu(void);
    void audioMenu(void);
    void systemMenu(void);
    void advancedMenu(void);
    void batteryMenu(void);
    void powerMenu(void);
    void pinsMenu(void);
    void usbDebugMenu(void);
    void tunablesMenu(void); // task 1 tunables, encoder mirror of `tune list`

    // Helper methods for complex operations
    void switchToUSBSerial(void);
    void switchToUARTSerial(void);
};

#endif
