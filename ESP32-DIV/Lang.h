#pragma once

#include <stdint.h>

// Central place for the device's language concept. Today this only controls
// which language the per-feature "info" screens (WiFi/Bluetooth/.../RIGHT
// button) show. The enum/helpers here are written so a future "whole UI"
// translation (menu labels, messages, etc.) can reuse the same InfoLang
// value and InfoText pattern instead of inventing a second mechanism --
// that is explicitly out of scope for now, this just keeps the door open.
//
// To add a new language later: add a slot to InfoLang/INFO_LANG_COUNT/
// INFO_LANG_NAMES below, then add one more string to every InfoText{...}
// initializer in LangInfo.cpp (compiler will flag any list that is short).

enum InfoLang : uint8_t {
  INFO_LANG_EN    = 0,
  INFO_LANG_PT_BR = 1,
};

static constexpr int INFO_LANG_COUNT = 2;

// Short label for each language, used by the Settings UI.
static const char* const INFO_LANG_NAMES[INFO_LANG_COUNT] = {"EN", "PT-BR"};

// One translatable string, indexed by InfoLang. Aggregate-initialize with
// {enText, ptText} in source order matching InfoLang above.
struct InfoText {
  const char* text[INFO_LANG_COUNT];
};

// Returns t's string for `lang`, falling back to English if `lang` is out of
// range or that slot is null (keeps things safe if INFO_LANG_COUNT grows
// before every table is filled in).
inline const char* infoLangText(const InfoText& t, uint8_t lang) {
  if (lang >= INFO_LANG_COUNT || !t.text[lang]) {
    return t.text[INFO_LANG_EN];
  }
  return t.text[lang];
}
