#ifndef __WIFI_MENU_H__
#define __WIFI_MENU_H__

#include <MenuItemInterface.h>

class WifiMenu : public MenuItemInterface {
public:
    WifiMenu() : MenuItemInterface("WiFi") {}

    void optionsMenu(void);
    void drawIcon(float scale);
    bool hasTheme() { return bruceConfig.theme.wifi; }
    const String& themePath() override { return bruceConfig.theme.paths.wifi; }

private:
    // One page per purpose. The flat WiFi list had grown to 22 rows - more
    // than three screens on the T-Embed - so the tools are grouped and the
    // front page stays a single glance. Every tool keeps its own name.
    void attacksMenu(void);
    void monitorMenu(void);
    void remoteMenu(void);
    void configMenu(void);
};

#endif
