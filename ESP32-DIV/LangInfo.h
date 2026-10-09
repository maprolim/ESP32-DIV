#pragma once

// Centralized text for every per-feature "info" screen (BTN_RIGHT on a menu
// item opens one). One InfoText per feature, indexed the same way as that
// feature's existing item-name array (see each array's comment in
// ESP32-DIV.ino) and skipping any "Back to Main Menu" entry. Sizes are
// written as literals here on purpose: the matching NUM_SUBMENU_ITEMS/
// FEATURES constants live in ESP32-DIV.ino with internal linkage, so this
// translation unit can't reference them directly.
//
// Adding a feature's info text: add one more InfoText to the right array
// below, in the same order as its item-name array. Adding a language:
// extend Lang/LANG_COUNT in Lang.h, then add one more string to every
// InfoText{...} initializer here. See i18n/README.md for the full walkthrough.

#include "Lang.h"

extern const InfoText wifi_page0_info[12];
extern const InfoText bluetooth_page0_info[9];
extern const InfoText nrf_info[6];
extern const InfoText subghz_info[5];
extern const InfoText tools_info[4];
extern const InfoText rfid_info[8];
extern const InfoText gps_info[2];
extern const InfoText ir_info[4];
