#pragma once

// Translatable keys for every tile name and every feature name inside a
// tile's menu (i18n scope: tiles + feature names + info paragraphs -- info
// paragraphs live in LangInfo.h/.cpp instead, see the comment there for why
// they're kept in a separate file). See i18n/README.md for how to add a
// language or a new string.
//
// Pattern: each UI array in ESP32-DIV.ino that used to hold `const char*`
// literals now holds `StrKey` values instead (same order, same size), and
// every place that used to print/measure that literal directly now calls
// t(key) to resolve it in the current language. t() is cheap (array index +
// a couple of null checks), so it is fine to call it every redraw.
//
// All translations are kept strictly within Basic ASCII (32-127) -- the
// screens that render these strings use TFT_eSPI's stock bitmap fonts
// (Font1/Font2), which don't have accented glyphs. See LangInfo.h for the
// same constraint on the info-paragraph text.

#include "Lang.h"

enum StrKey : uint16_t {
  // Main menu tiles (menu_items in ESP32-DIV.ino)
  STR_TILE_WIFI = 0,
  STR_TILE_24GHZ,
  STR_TILE_MORE,
  STR_TILE_SETTINGS,
  STR_TILE_BLUETOOTH,
  STR_TILE_SUBGHZ,
  STR_TILE_TOOLS,
  STR_TILE_ABOUT,

  // "More" tile's own grid (other_submenu_items)
  STR_TILE_IR,
  STR_TILE_RFID,
  STR_TILE_GPS,
  STR_MAIN_MENU,            // also reused by the paged-submenu footer button

  // Shared trailing nav entry of every paged submenu list
  STR_BACK_TO_MAIN_MENU,

  // Settings tile's own row labels (AppSettingsUI::items in utils.cpp)
  STR_SETTINGS_BRIGHTNESS,
  STR_SETTINGS_THEME,
  STR_SETTINGS_ACCENT,
  STR_SETTINGS_NEOPIXEL,
  STR_SETTINGS_AUTO_SCAN,
  STR_SETTINGS_LANGUAGE,

  // WiFi features (wifi_items, WIFI_FEATURE_COUNT)
  STR_WIFI_PACKET_MONITOR,
  STR_WIFI_BEACON_SPAMMER,
  STR_WIFI_24GHZ_DEAUTHER,
  STR_WIFI_PROBE_REQUEST_FLOOD,
  STR_WIFI_DEAUTH_DETECTOR,
  STR_WIFI_24GHZ_SCANNER,
  STR_WIFI_CAPTIVE_PORTAL,
  STR_WIFI_HIDDEN_SSID_REVEALER,
  STR_WIFI_WPS_SCANNER,
  STR_WIFI_ARP_SCANNER,
  STR_WIFI_KARMA_ATTACK,
  STR_WIFI_CHANNEL_GRAPH,

  // Bluetooth features (bluetooth_items, BT_FEATURE_COUNT)
  STR_BT_BLE_JAMMER,
  STR_BT_BLE_SPOOFER,
  STR_BT_SOUR_APPLE,
  STR_BT_AIRTAG_SPOOFER,
  STR_BT_AIRTAG_SNIFFER,
  STR_BT_SNIFFER,
  STR_BT_BLE_SCANNER,
  STR_BT_BLE_RUBBER_DUCKY,
  STR_BT_SKIMMER_DETECT,

  // nRF24 features (nrf_submenu_items, excl. trailing Back)
  STR_NRF_SCANNER,
  STR_NRF_PROTO_KILL,
  STR_NRF_ESB_SNIFFER,
  STR_NRF_ESB_REPLAY,
  STR_NRF_MOUSEJACK_SCAN,
  STR_NRF_MOUSEJACK_INJECT,

  // SubGHz features (subghz_submenu_items, excl. trailing Back)
  STR_SUBGHZ_REPLAY_ATTACK,
  STR_SUBGHZ_JAMMER,
  STR_SUBGHZ_DE_BRUIJN_BRUTE,
  STR_SUBGHZ_JAMMING_DETECTOR,
  STR_SUBGHZ_SAVED_PROFILE,

  // Tools features (tools_submenu_items, excl. trailing Back)
  STR_TOOLS_SERIAL_MONITOR,
  STR_TOOLS_UPDATE_FIRMWARE,
  STR_TOOLS_TOUCH_CALIBRATE,
  STR_TOOLS_SD_FILE_MANAGER,

  // RFID features (rfid_submenu_items, excl. trailing Back)
  STR_RFID_CARD_READER,
  STR_RFID_CARD_CLONE,
  STR_RFID_ERASE,
  STR_RFID_DUMP,
  STR_RFID_DECODE_ACCESS,
  STR_RFID_JAM_READER,
  STR_RFID_TAG_DISRUPT,
  STR_RFID_DISRUPT_EMULATE,

  // GPS features (gps_submenu_items, excl. trailing Back)
  STR_GPS_WARDRIVER,
  STR_GPS_SATELLITE_SCANNER,

  // IR features (ir_submenu_items, excl. trailing Back)
  STR_IR_RECORD,
  STR_IR_SAVED_PROFILE,
  STR_IR_UNIVERSAL_CONTROLLER,
  STR_IR_UNIVERSAL_CONTROLLER_AC,

  STR_KEY_COUNT
};

extern const LocStr STRINGS[STR_KEY_COUNT];

// Resolves `key` in the currently selected language (settings().infoLang),
// with the same two-level fallback as locText(): missing translation for the
// current language -> English; out-of-range key -> a visible "?" instead of
// a null/garbage pointer reaching tft.print().
const char* t(StrKey key);
