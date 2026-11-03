#ifndef __SCRIPTS_MENU_H__
#define __SCRIPTS_MENU_H__

#include <MenuItemInterface.h>

class ScriptsMenu : public MenuItemInterface {
public:
    // Main menu label. The module behind it is still the JavaScript interpreter
    // (scripts, App Store, Load...), so only the displayed name changed.
    ScriptsMenu() : MenuItemInterface("Apps") {}

    void optionsMenu();
    void drawIcon(float scale);
    bool hasTheme() { return bruceConfig.theme.interpreter; }
    const String& themePath() override { return bruceConfig.theme.paths.interpreter; }
};

#endif
