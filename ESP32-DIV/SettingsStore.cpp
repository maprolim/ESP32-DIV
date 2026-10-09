#include <ArduinoJson.h>
#include <SD.h>
#include "SettingsStore.h"
#include "utils.h"


static AppSettings g_settings;
AppSettings& settings() { return g_settings; }

static const AccentOption kAccentPresets[] = {
  {STR_ACCENT_ORANGE, 0xFBE4},
  {STR_ACCENT_GREEN,  0xB721},
  {STR_ACCENT_RED,    0xF800},
  {STR_ACCENT_CYAN,   0x07FF},
  {STR_ACCENT_PURPLE, 0xF81F},
  {STR_ACCENT_YELLOW, 0xFFE0},
  {STR_ACCENT_WHITE,  0xFFFF},
};

uint8_t accentPresetClamp(uint8_t preset) {
  if (preset >= ACCENT_PRESET_COUNT) return 0;
  return preset;
}

uint16_t accentColor565(uint8_t preset) {
  return kAccentPresets[accentPresetClamp(preset)].color565;
}

const char* accentPresetName(uint8_t preset) {
  return t(kAccentPresets[accentPresetClamp(preset)].name);
}

const char* settingsBoardProfileId() {
  return TOUCH_PROFILE_ID;
}

void settingsApplyBoardTouchDefaults() {
  auto& s = g_settings;
  s.touchXMin = TOUCH_X_MIN;
  s.touchXMax = TOUCH_X_MAX;
  s.touchYMin = TOUCH_Y_MIN;
  s.touchYMax = TOUCH_Y_MAX;
}

static bool settingsTouchSavedForBoard(const StaticJsonDocument<512>& doc) {
  JsonObjectConst touch = doc["touch"];
  if (touch.isNull()) {
    return false;
  }
  if (!touch["xMin"].is<uint16_t>() || !touch["xMax"].is<uint16_t>() ||
      !touch["yMin"].is<uint16_t>() || !touch["yMax"].is<uint16_t>()) {
    return false;
  }
  const char* savedBoard = doc["board"] | "";
  if (savedBoard[0] == '\0') {
    return true;
  }
  return strcmp(savedBoard, TOUCH_PROFILE_ID) == 0;
}

static bool sd_mounted = false;
static bool mountSD() {

  if (sd_mounted) {
    if (SD.cardType() != CARD_NONE) return true;
    sd_mounted = false;
  }

  sd_mounted = isSDCardAvailable();
  return sd_mounted;
}

static bool ensureDir(const char* dirPath) {
  if (!mountSD()) return false;
  if (!SD.exists(dirPath)) {
    if (SD.mkdir(dirPath)) return true;

    if (dirPath && dirPath[0] == '/') {
      return SD.mkdir(dirPath + 1);
    }
    return false;
  }
  return true;
}

bool settingsLoad() {
  settingsApplyBoardTouchDefaults();
  sdRetryMount();
  if (!mountSD()) return false;
  if (!SD.exists(SETTINGS_PATH)) return true;

  File f = SD.open(SETTINGS_PATH, FILE_READ);
  if (!f) return false;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  auto& s = g_settings;
  s.brightness      = doc["brightness"]      | s.brightness;
  s.theme           = (Theme)(uint8_t)(doc["theme"] | (uint8_t)s.theme);
  s.accentColor     = accentPresetClamp(doc["accentColor"] | s.accentColor);
  s.neopixelEnabled = doc["neopixelEnabled"] | s.neopixelEnabled;

  s.autoWifiScan    = doc["autoWifiScan"]    | s.autoWifiScan;
  s.autoBleScan     = doc["autoBleScan"]     | s.autoBleScan;
  {
    uint8_t lang = doc["infoLang"] | s.infoLang;
    s.infoLang = (lang < INFO_LANG_COUNT) ? lang : INFO_LANG_PT_BR;
  }

  if (s.autoWifiScan != s.autoBleScan) {
    bool en = (s.autoWifiScan || s.autoBleScan);
    s.autoWifiScan = en;
    s.autoBleScan  = en;
  }

  if (settingsTouchSavedForBoard(doc)) {
    JsonObjectConst touch = doc["touch"];
    s.touchXMin = touch["xMin"] | s.touchXMin;
    s.touchXMax = touch["xMax"] | s.touchXMax;
    s.touchYMin = touch["yMin"] | s.touchYMin;
    s.touchYMax = touch["yMax"] | s.touchYMax;
  } else {
    settingsApplyBoardTouchDefaults();
  }

  return true;
}

bool settingsSave() {
  sdRetryMount();

  if (!ensureDir("/config")) {
    sd_mounted = false;
    if (!ensureDir("/config")) return false;
  }

  File f = SD.open(SETTINGS_PATH, FILE_WRITE);
  if (!f) {
    sd_mounted = false;
    if (!mountSD()) return false;
    f = SD.open(SETTINGS_PATH, FILE_WRITE);
    if (!f) return false;
  }

  auto& s = g_settings;
  StaticJsonDocument<512> doc;
  doc["board"]           = TOUCH_PROFILE_ID;
  doc["brightness"]      = s.brightness;
  doc["theme"]           = (uint8_t)s.theme;
  doc["accentColor"]     = s.accentColor;
  doc["neopixelEnabled"] = s.neopixelEnabled;

  doc["autoWifiScan"]    = s.autoWifiScan;
  doc["autoBleScan"]     = s.autoBleScan;
  doc["infoLang"]        = s.infoLang;

  JsonObject t = doc.createNestedObject("touch");
  t["xMin"] = s.touchXMin;
  t["xMax"] = s.touchXMax;
  t["yMin"] = s.touchYMin;
  t["yMax"] = s.touchYMax;

  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}
