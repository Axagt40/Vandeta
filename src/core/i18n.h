#ifndef __VANDETA_I18N_H__
#define __VANDETA_I18N_H__

#include <Arduino.h>

// Firmware language. 0 is English and is also the fallback for every string
// that has no translation, so a language pack can be partial without ever
// leaving a blank label on the panel.
enum Language : uint8_t { LANG_EN = 0, LANG_ES, LANG_FR, LANG_DE, LANG_PT, LANG_COUNT };

int languageCount();
/// Name of a language, as shown in Config > System Config > Language.
const char *languageName(int lang);
/// Translate a UI label. Unknown strings come back unchanged (English).
String i18n(const String &text);
String i18n(const char *text);

#endif
