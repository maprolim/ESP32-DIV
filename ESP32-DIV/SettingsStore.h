#pragma once

#include <Arduino.h>
#include "shared.h"
#include "Lang.h"
#include "Strings.h"

#ifndef SETTINGS_PATH
#define SETTINGS_PATH "/config/settings.json"
#endif

struct AppSettings {

  uint8_t  brightness = BKL_LEVEL_MED;
  Theme    theme      = Theme::Dark;
  uint8_t  accentColor = 0;

  bool     neopixelEnabled = false;

  bool     autoWifiScan    = true;
  bool     autoBleScan     = true;

  uint8_t  infoLang = INFO_LANG_PT_BR;  // language shown on feature "info" screens

  uint16_t touchXMin = TOUCH_X_MIN;
  uint16_t touchXMax = TOUCH_X_MAX;
  uint16_t touchYMin = TOUCH_Y_MIN;
  uint16_t touchYMax = TOUCH_Y_MAX;

};

struct AccentOption {
  StrKey name;  // resolved through t() at draw time, see accentPresetName()
  uint16_t color565;
};

constexpr uint8_t ACCENT_PRESET_COUNT = 7;

AppSettings& settings();
uint8_t accentPresetClamp(uint8_t preset);
uint16_t accentColor565(uint8_t preset);
const char* accentPresetName(uint8_t preset);

const char* settingsBoardProfileId();
void settingsApplyBoardTouchDefaults();
bool settingsLoad();
bool settingsSave();
