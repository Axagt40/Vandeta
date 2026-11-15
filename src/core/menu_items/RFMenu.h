#ifndef __RF_MENU_H__
#define __RF_MENU_H__

#include <MenuItemInterface.h>

class RFMenu : public MenuItemInterface {
public:
    RFMenu() : MenuItemInterface("RF") {}

    void optionsMenu(void);
    void drawIcon(float scale);
    bool hasTheme() { return bruceConfig.theme.rf; }
    const String& themePath() override { return bruceConfig.theme.paths.rf; }

private:
    // The four spectrum renderers answer the same question, so the RF page
    // has one Spectrum row and this picker chooses the renderer.
    void spectrumMenu(void);
    void configMenu(void);
};

#endif
