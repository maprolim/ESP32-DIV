#pragma once

#include <stdint.h>

// Central place for the device's language concept. Drives every localized
// string in scope today: tile names, feature names inside each tile's menu,
// and the per-feature "info" screens (WiFi/Bluetooth/.../RIGHT button). See
// Strings.h (tile/feature labels) and LangInfo.h (info paragraphs), both of
// which key off the same Lang value via t()/infoLangText().
//
// To add a new language later: add a slot to Lang/LANG_COUNT/LANG_NAMES
// below, then add one more string to every LocStr{...} initializer in
// Strings.cpp and LangInfo.cpp (the compiler will flag any list that is
// short). See i18n/README.md for the full walkthrough.

enum Lang : uint8_t {
  LANG_EN    = 0,
  LANG_PT_BR = 1,
  LANG_ES    = 2,
};

static constexpr int LANG_COUNT = 3;

// Short label for each language, used by the Settings UI's language picker.
static const char* const LANG_NAMES[LANG_COUNT] = {"EN", "PT-BR", "ES"};

// One translatable string, indexed by Lang. Aggregate-initialize with
// {enText, ptText, esText} in source order matching Lang above. Kept generic
// (not "InfoText") on purpose: both the short tile/feature labels (Strings.cpp)
// and the long info paragraphs (LangInfo.cpp) reuse this same shape.
struct LocStr {
  const char* text[LANG_COUNT];
};

// Legacy alias -- LangInfo.{h,cpp} predate Strings.cpp and were written
// against the name "InfoText"; kept so that file doesn't need a rename too.
using InfoText = LocStr;

// Returns t's string for `lang`, with two safety nets so a bad index or a
// hole in a translation table can never hand a null/garbage pointer to the
// display code (which would crash the ESP32 on tft.print()):
//   1. out-of-range or missing `lang` slot -> fall back to English.
//   2. even English missing (should never happen, arrays are sized to
//      LANG_COUNT at compile time) -> return a visible placeholder instead
//      of a null pointer.
inline const char* locText(const LocStr& t, uint8_t lang) {
  if (lang < LANG_COUNT && t.text[lang]) {
    return t.text[lang];
  }
  if (t.text[LANG_EN]) {
    return t.text[LANG_EN];
  }
  return "?";
}

// Legacy alias for the function name used throughout the existing info-screen
// code; same thing as locText().
inline const char* infoLangText(const LocStr& t, uint8_t lang) {
  return locText(t, lang);
}

// Legacy names, kept so SettingsStore/utils.cpp (which predate the
// tile/feature translation work) keep compiling unchanged.
static constexpr Lang INFO_LANG_EN    = LANG_EN;
static constexpr Lang INFO_LANG_PT_BR = LANG_PT_BR;
static constexpr int  INFO_LANG_COUNT = LANG_COUNT;
#define INFO_LANG_NAMES LANG_NAMES
