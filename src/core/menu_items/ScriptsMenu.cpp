#include "ScriptsMenu.h"
#include "core/display.h"
#include "core/settings.h"
#include "core/utils.h"
#include "modules/bjs_interpreter/interpreter.h" // for JavaScript interpreter
#include <algorithm>                             // for std::sort

void ScriptsMenu::optionsMenu() {
#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
    if (interpreter_state >= 0) {
        interpreter_state = 1;
        returnToMenu = true;
        return;
    }

    options = getScriptsOptionsList("", false);

    options.push_back({"Load...", run_bjs_script});
    addOptionToMainMenu();

    loopOptions(options, MENU_TYPE_SUBMENU, "Scripts");
#endif
}

void ScriptsMenu::drawIcon(float scale) {
    clearIconArea();

    // App drawer: a rounded card holding a 3x3 grid of tiles, so the entry reads
    // as "applications" while keeping the same line-art look (primary colour on
    // background) as the other main menu icons. Purely a visual change - the
    // entry still opens the JavaScript interpreter / App Store page.
    int iconW = scale * 56;
    int iconH = scale * 56;

    if (iconW % 2 != 0) iconW++;
    if (iconH % 2 != 0) iconH++;

    int x = iconCenterX - iconW / 2;
    int y = iconCenterY - iconH / 2;
    int inner = scale * 9;
    int corner = scale * 10;

    tft.drawRoundRect(x, y, iconW, iconH, corner, bruceConfig.priColor);

    int cell = (iconW - 2 * inner) / 3;
    int tile = cell * 2 / 3;
    if (tile < 3) tile = 3;
    int tileRadius = tile / 3;
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int tx = x + inner + col * cell + (cell - tile) / 2;
            int ty = y + inner + row * cell + (cell - tile) / 2;
            tft.fillRoundRect(tx, ty, tile, tile, tileRadius, bruceConfig.priColor);
        }
    }
}
