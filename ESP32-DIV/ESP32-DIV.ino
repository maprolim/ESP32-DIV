#include <Arduino.h>
#include <PCF8574.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include "SettingsStore.h"
#include "LangInfo.h"
#include "Strings.h"
#include "Touchscreen.h"
#include "config.h"
#include "ducky.h"
#include "icon.h"
#include "ir.h"
#include "gps.h"
#include "rfid.h"
#include "shared.h"
#include "utils.h"
#include "hwdetect.h"

#if !BOARD_HAS_ESP32S3
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#endif

TFT_eSPI tft = TFT_eSPI();

PCF8574 pcf(PCF8574_I2C_ADDR);

void setBrightness(uint8_t value) {
  ledcWrite(PWM_CHANNEL, value);
}

bool feature_exit_requested = false;

const int NUM_MENU_ITEMS = 8;
const StrKey menu_items[NUM_MENU_ITEMS] = {
    STR_TILE_WIFI,
    STR_TILE_24GHZ,
    STR_TILE_MORE,
    STR_TILE_SETTINGS,
    STR_TILE_BLUETOOTH,
    STR_TILE_SUBGHZ,
    STR_TILE_TOOLS,
    STR_TILE_ABOUT};

const unsigned char *bitmap_icons[NUM_MENU_ITEMS] = {
    bitmap_icon_wifi,
    bitmap_icon_jammer,
    bitmap_icon_dialog,
    bitmap_icon_setting,
    bitmap_icon_spoofer,
    bitmap_icon_analyzer,
    bitmap_icon_stat,
    bitmap_icon_question};

int current_menu_index = 0;
bool is_main_menu = false;

// WiFi's 12 features as one flat list; the generic paged engine (below)
// slices it into pages of kPagedItemsPerPage on its own.
static constexpr int WIFI_FEATURE_COUNT = 12;
const StrKey wifi_items[WIFI_FEATURE_COUNT] = {
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
    STR_WIFI_CHANNEL_GRAPH};

// Info text (BTN_RIGHT on the WiFi menu) -- see wifi_page0_info in LangInfo.cpp;
// all 12 items have text (spans both paged-submenu pages).
static constexpr int WIFI_INFO_COUNT = 12;

// Bluetooth's 9 features as one flat list, same idea as WiFi above.
static constexpr int BT_FEATURE_COUNT = 9;
const StrKey bluetooth_items[BT_FEATURE_COUNT] = {
    STR_BT_BLE_JAMMER,
    STR_BT_BLE_SPOOFER,
    STR_BT_SOUR_APPLE,
    STR_BT_AIRTAG_SPOOFER,
    STR_BT_AIRTAG_SNIFFER,
    STR_BT_SNIFFER,
    STR_BT_BLE_SCANNER,
    STR_BT_BLE_RUBBER_DUCKY,
    STR_BT_SKIMMER_DETECT};

// Info text (BTN_RIGHT) -- see bluetooth_page0_info in LangInfo.cpp;
// all 9 items have text.
static constexpr int BT_INFO_COUNT = 9;

static FeatureUI::Button s_pagedFooterBtns[2];
static int s_pagedFooterFocus = -1;  // 0=back, 1=page btn, -1=none

const int nrf_NUM_SUBMENU_ITEMS = 7;
const StrKey nrf_submenu_items[nrf_NUM_SUBMENU_ITEMS] = {
    STR_NRF_SCANNER,
    STR_NRF_PROTO_KILL,
    STR_NRF_ESB_SNIFFER,
    STR_NRF_ESB_REPLAY,
    STR_NRF_MOUSEJACK_SCAN,
    STR_NRF_MOUSEJACK_INJECT,
    STR_BACK_TO_MAIN_MENU};

// Info text (BTN_RIGHT) -- see nrf_info in LangInfo.cpp, same order as
// nrf_submenu_items (without "Back").

const int subghz_NUM_SUBMENU_ITEMS = 6;
const StrKey subghz_submenu_items[subghz_NUM_SUBMENU_ITEMS] = {
    STR_SUBGHZ_REPLAY_ATTACK,
    STR_SUBGHZ_JAMMER,
    STR_SUBGHZ_DE_BRUIJN_BRUTE,
    STR_SUBGHZ_JAMMING_DETECTOR,
    STR_SUBGHZ_SAVED_PROFILE,
    STR_BACK_TO_MAIN_MENU};

// Info text (BTN_RIGHT) -- see subghz_info in LangInfo.cpp, same order as
// subghz_submenu_items (without "Back").

const int tools_NUM_SUBMENU_ITEMS = 5;
const StrKey tools_submenu_items[tools_NUM_SUBMENU_ITEMS] = {
    STR_TOOLS_SERIAL_MONITOR,
    STR_TOOLS_UPDATE_FIRMWARE,
    STR_TOOLS_TOUCH_CALIBRATE,
    STR_TOOLS_SD_FILE_MANAGER,
    STR_BACK_TO_MAIN_MENU};

// Info text (BTN_RIGHT) -- see tools_info in LangInfo.cpp, same order as
// tools_submenu_items (without "Back").

static constexpr uint8_t OTHER_LAYER_HOME = 0;
static constexpr uint8_t OTHER_LAYER_IR   = 1;
static constexpr uint8_t OTHER_LAYER_RFID = 2;
static constexpr uint8_t OTHER_LAYER_GPS  = 3;

const int other_NUM_SUBMENU_ITEMS = 4;
static constexpr int OTHER_GRID_COLS = 2;
const StrKey other_submenu_items[other_NUM_SUBMENU_ITEMS] = {
    STR_TILE_IR,
    STR_TILE_RFID,
    STR_TILE_GPS,
    STR_MORE_BACK};

const int rfid_NUM_SUBMENU_ITEMS = 9;
const StrKey rfid_submenu_items[rfid_NUM_SUBMENU_ITEMS] = {
    STR_RFID_CARD_READER,
    STR_RFID_CARD_CLONE,
    STR_RFID_ERASE,
    STR_RFID_DUMP,
    STR_RFID_DECODE_ACCESS,
    STR_RFID_JAM_READER,
    STR_RFID_TAG_DISRUPT,
    STR_RFID_DISRUPT_EMULATE,
    STR_BACK_TO_MAIN_MENU};

// Info text (BTN_RIGHT) -- see rfid_info in LangInfo.cpp, same order as
// rfid_submenu_items (without "Back").

const int gps_NUM_SUBMENU_ITEMS = 3;
const StrKey gps_submenu_items[gps_NUM_SUBMENU_ITEMS] = {
    STR_GPS_WARDRIVER,
    STR_GPS_SATELLITE_SCANNER,
    STR_BACK_TO_MAIN_MENU};

// Info text (BTN_RIGHT) -- see gps_info in LangInfo.cpp, same order as
// gps_submenu_items (without "Back").

const int ir_NUM_SUBMENU_ITEMS = 5;
const StrKey ir_submenu_items[ir_NUM_SUBMENU_ITEMS] = {
    STR_IR_RECORD,
    STR_IR_SAVED_PROFILE,
    STR_IR_UNIVERSAL_CONTROLLER,
    STR_IR_UNIVERSAL_CONTROLLER_AC,
    STR_BACK_TO_MAIN_MENU};

// Info text (BTN_RIGHT) -- see ir_info in LangInfo.cpp, same order as
// ir_submenu_items (without "Back").

const int about_NUM_SUBMENU_ITEMS = 1;
const StrKey about_submenu_items[about_NUM_SUBMENU_ITEMS] = {
    STR_BACK_TO_MAIN_MENU};

const int setting_NUM_SUBMENU_ITEMS = 1;
const StrKey setting_submenu_items[setting_NUM_SUBMENU_ITEMS] = {
    STR_BACK_TO_MAIN_MENU};

int current_submenu_index = 0;
bool in_sub_menu = false;
int last_submenu_index = -1;
bool submenu_initialized = false;
uint8_t other_layer = OTHER_LAYER_HOME;
int last_other_menu_index = -1;
bool other_menu_grid_initialized = false;
// Remembers which tile (IR=0 / RFID=1 / GPS=2) was selected on the "More"
// grid, so going back from IR/RFID/GPS restores that tile instead of
// always landing on the first one.
int other_home_selected_tile = 0;

const StrKey *active_submenu_items = nullptr;
int active_submenu_size = 0;

const unsigned char *wifi_icons[WIFI_FEATURE_COUNT] = {
    bitmap_icon_wifi,
    bitmap_icon_antenna,
    bitmap_icon_wifi_jammer,
    bitmap_icon_Skull_3,
    bitmap_icon_eye2,
    bitmap_icon_jammer,
    bitmap_icon_bash,
    bitmap_icon_eye_blind,
    bitmap_icon_key,
    bitmap_icon_list,
    bitmap_icon_devil,
    bitmap_icon_chart_dot
};

const unsigned char *bluetooth_icons[BT_FEATURE_COUNT] = {
    bitmap_icon_ble_jammer,
    bitmap_icon_spoofer,
    bitmap_icon_apple,
    bitmap_icon_tags,
    bitmap_icon_magnifying_glass,
    bitmap_icon_analyzer,
    bitmap_icon_graph,
    bitmap_icon_rubber_ducky,
    bitmap_icon_Wireless_4
};

const unsigned char *nrf_submenu_icons[nrf_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_scanner,
    bitmap_icon_kill,
    bitmap_icon_analyzer,
    bitmap_icon_follow,
    bitmap_icon_magnifying_glass,
    bitmap_icon_key,
    bitmap_icon_go_back
};

const unsigned char *subghz_submenu_icons[subghz_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_antenna,
    bitmap_icon_no_signal,
    bitmap_icon_graph_self_loop,
    bitmap_icon_Voice_Id,
    bitmap_icon_list,
    bitmap_icon_go_back
};

const unsigned char *tools_submenu_icons[tools_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_bash,
    bitmap_icon_follow,
    bitmap_icon_undo,
    bitmap_icon_sdcard,
    bitmap_icon_go_back
};

const unsigned char *other_submenu_icons[other_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_led,
    bitmap_icon_rfid_chip,
    bitmap_icon_satellite,
    bitmap_icon_go_back
};

const unsigned char *rfid_submenu_icons[rfid_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_magnifying_glass,
    bitmap_icon_follow,
    bitmap_icon_recycle,
    bitmap_icon_dot_matrix,
    bitmap_icon_key,
    bitmap_icon_kill,
    bitmap_icon_flash,
    bitmap_icon_devil,
    bitmap_icon_go_back
};

const unsigned char *gps_submenu_icons[gps_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_satellite,
    bitmap_icon_satellite_dish,
    bitmap_icon_go_back
};

const unsigned char *ir_submenu_icons[ir_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_led,
    bitmap_icon_list,
    bitmap_icon_remote_control,
    bitmap_icon_temp,
    bitmap_icon_go_back
};

const unsigned char *about_submenu_icons[about_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_go_back
};

const unsigned char *setting_submenu_icons[setting_NUM_SUBMENU_ITEMS] = {
    bitmap_icon_go_back
};

const unsigned char **active_submenu_icons = nullptr;

// ── Generic paged submenu engine ────────────────────────────────────────
// Every list-style submenu (anything except the "More" tile grid) is driven
// by the same engine: a flat items/icons array plus a total feature count,
// paginated kPagedItemsPerPage at a time with "Back to Main Menu" as an
// implicit footer button rather than a trailing array entry. A menu with
// <=9 features just gets one page and no arrows; growing past 9 gets paging
// for free, with no per-menu special-casing needed anywhere else.
static const int kPagedItemsPerPage = 9;
static int g_pagedPage = 0;  // shared cursor; reset whenever a submenu is (re)entered

static int pagedTotalCount() {
    switch (current_menu_index) {
        case 0: return WIFI_FEATURE_COUNT;
        case 1: return nrf_NUM_SUBMENU_ITEMS - 1;
        case 2:
            if (other_layer == OTHER_LAYER_IR)   return ir_NUM_SUBMENU_ITEMS - 1;
            if (other_layer == OTHER_LAYER_RFID) return rfid_NUM_SUBMENU_ITEMS - 1;
            if (other_layer == OTHER_LAYER_GPS)  return gps_NUM_SUBMENU_ITEMS - 1;
            return 0;  // HOME is the tile grid, not a paged list
        case 4: return BT_FEATURE_COUNT;
        case 5: return subghz_NUM_SUBMENU_ITEMS - 1;
        case 6: return tools_NUM_SUBMENU_ITEMS - 1;
        default: return 0;
    }
}

static const StrKey* pagedTotalItems() {
    switch (current_menu_index) {
        case 0: return wifi_items;
        case 1: return nrf_submenu_items;
        case 2:
            if (other_layer == OTHER_LAYER_IR)   return ir_submenu_items;
            if (other_layer == OTHER_LAYER_RFID) return rfid_submenu_items;
            if (other_layer == OTHER_LAYER_GPS)  return gps_submenu_items;
            return nullptr;
        case 4: return bluetooth_items;
        case 5: return subghz_submenu_items;
        case 6: return tools_submenu_items;
        default: return nullptr;
    }
}

static const unsigned char** pagedTotalIcons() {
    switch (current_menu_index) {
        case 0: return wifi_icons;
        case 1: return nrf_submenu_icons;
        case 2:
            if (other_layer == OTHER_LAYER_IR)   return ir_submenu_icons;
            if (other_layer == OTHER_LAYER_RFID) return rfid_submenu_icons;
            if (other_layer == OTHER_LAYER_GPS)  return gps_submenu_icons;
            return nullptr;
        case 4: return bluetooth_icons;
        case 5: return subghz_submenu_icons;
        case 6: return tools_submenu_icons;
        default: return nullptr;
    }
}

static int pagedPageCount() {
    const int n = (pagedTotalCount() + kPagedItemsPerPage - 1) / kPagedItemsPerPage;
    return n > 0 ? n : 1;
}

// Items visible on the CURRENT page (< kPagedItemsPerPage on the last page).
static int pagedFeatureCount() {
    const int total = pagedTotalCount();
    const int remain = total - g_pagedPage * kPagedItemsPerPage;
    if (remain <= 0) return 0;
    return remain < kPagedItemsPerPage ? remain : kPagedItemsPerPage;
}

// Bottom row: [icon | Main Menu] ........ [Next Page | icon] (only when >1 page)
static int pagedBackBtnIndex() {
    return pagedFeatureCount();
}

static int pagedPageBtnIndex() {
    return pagedFeatureCount() + 1;
}

static int pagedNavRowY() {
    return tft.height() - 30;
}

// Row pitch for each feature in a paged submenu's list (icon + "| Label").
// 9 * 28 = 252, well clear of pagedNavRowY() at height-30=290. Shared by the
// list draw, the selection highlight redraw, the touch hit-test, and the
// overflow-arrow placement — all must agree on this value or taps/highlights
// land on the wrong row.
static const int kPagedRowH = 28;

static bool pagedOnLastPage() {
    return g_pagedPage >= pagedPageCount() - 1;
}

// Label/icon flip direction depending on where we are: everywhere but the
// last page, the button moves forward ("Next"); on the last page there is
// no next page, so it moves backward instead ("Previous") -- both the text
// and the action (see the three g_pagedPage updates below) agree on this.
static const char* pagedPageBtnLabel() {
    return pagedOnLastPage() ? t(STR_PAGED_PREV_PAGE) : t(STR_PAGED_NEXT_PAGE);
}

static const unsigned char* pagedPageBtnIcon() {
    return pagedOnLastPage() ? bitmap_icon_navigate_left : bitmap_icon_navigate_right;
}

// The paged-submenu footer's "Main Menu" button actually just goes back one
// level inside the "More" tile's IR/RFID/GPS screens (to the More grid),
// not all the way to the main menu -- label it "Back" there instead, same
// wording as the More grid's own back tile (STR_MORE_BACK). Every other
// paged submenu (WiFi, Bluetooth, nRF24, SubGHz, Tools) really does go to
// the main menu, so it keeps STR_MAIN_MENU.
static StrKey pagedBackLabelKey() {
    if (current_menu_index == 2 && other_layer != OTHER_LAYER_HOME) {
        return STR_MORE_BACK;
    }
    return STR_MAIN_MENU;
}

// Advances to the next page, or back to the previous one when already on
// the last page -- matches the "Next"/"Previous" flip in pagedPageBtnLabel()
// above, so the button's text and its actual action always agree. The three
// call sites below (generic paged-submenu touch, WiFi, Bluetooth) used to
// each hardcode a forward-only +1, which is how a 2-page list ended up
// showing "Next Page" with nowhere left to go on its last page.
static void pagedAdvancePage() {
    if (pagedOnLastPage()) {
        g_pagedPage = (g_pagedPage - 1 + pagedPageCount()) % pagedPageCount();
    } else {
        g_pagedPage = (g_pagedPage + 1) % pagedPageCount();
    }
}

static void layoutPagedFooterButtons() {
    const int y = pagedNavRowY();
    const bool multiPage = pagedPageCount() > 1;
    const int w0 = multiPage ? tft.width() / 2 : tft.width();
    s_pagedFooterBtns[0] = {
        0, (int16_t)y, (int16_t)w0, 28,
        t(pagedBackLabelKey()), FeatureUI::ButtonStyle::Secondary, false};
    if (multiPage) {
        s_pagedFooterBtns[1] = {
            (int16_t)w0, (int16_t)y, (int16_t)(tft.width() - w0), 28,
            pagedPageBtnLabel(), FeatureUI::ButtonStyle::Secondary, false};
    }
}

static void drawPagedFooterButtons() {
    layoutPagedFooterButtons();
    const int y = pagedNavRowY();
    const int rowH = 28;
    const int iconSize = 16;
    const bool multiPage = pagedPageCount() > 1;
    tft.fillRect(0, y, tft.width(), rowH, UI_BG);

    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextSize(1);
    // Font 2 is ~16px; center icon + text on the same midline within the row.
    const int textH = 16;
    const int iconY = y + (rowH - iconSize) / 2;
    const int textY = y + (rowH - textH) / 2;

    {
        const uint16_t color = (s_pagedFooterFocus == 0) ? UI_ICON : UI_TEXT;
        tft.setTextColor(color, UI_BG);
        tft.drawBitmap(10, iconY, bitmap_icon_go_back, iconSize, iconSize, color);
        tft.setCursor(30, textY);
        tft.print(t(pagedBackLabelKey()));
    }

    if (multiPage) {
        const uint16_t color = (s_pagedFooterFocus == 1) ? UI_ICON : UI_TEXT;
        const char* label = pagedPageBtnLabel();
        const int gap = 4;
        const int textW = tft.textWidth(label);
        // Right-aligned group: [label][gap][icon] — same vertical midline.
        const int iconX = tft.width() - 10 - iconSize;
        const int textX = iconX - gap - textW;
        tft.setTextColor(color, UI_BG);
        tft.setCursor(textX, textY);
        tft.print(label);
        tft.drawBitmap(iconX, iconY, pagedPageBtnIcon(), iconSize, iconSize, color);
    }
}

// Slices active_submenu_items/icons to the current menu's current page and
// resets the draw/selection state so the next displaySubmenu() does a full
// redraw. Call after changing current_menu_index, other_layer, or g_pagedPage.
static void pagedApplyPage() {
    active_submenu_items = pagedTotalItems() + g_pagedPage * kPagedItemsPerPage;
    active_submenu_icons = pagedTotalIcons() + g_pagedPage * kPagedItemsPerPage;
    // +1 = features + Back. The "Next Page" footer button is reachable only
    // by touch (see pagedSubmenuEdgeFlip) — left out of the cycle on purpose
    // so UP/DOWN never land on it.
    active_submenu_size = pagedFeatureCount() + 1;
    if (current_submenu_index >= active_submenu_size) {
        current_submenu_index = 0;
    }
    s_pagedFooterFocus = -1;
    last_submenu_index = -1;
    submenu_initialized = false;
}

// Shared by every paged submenu list. Pressing DOWN on the last feature of a
// page jumps straight to the next page's first feature (if any), and
// pressing UP on the first feature of a page jumps back to the previous
// page's last feature (if any) — same feel as scrolling through one
// continuous list. Returns true when it handled the move, so the caller
// skips its normal +/-1 step.
static bool pagedSubmenuEdgeFlip(bool goingDown) {
    const int pageCount = pagedPageCount();
    const int featureCount = pagedFeatureCount();

    if (goingDown && current_submenu_index == featureCount - 1 && g_pagedPage < pageCount - 1) {
        g_pagedPage++;
        pagedApplyPage();
        current_submenu_index = 0;
        return true;
    }

    if (!goingDown && current_submenu_index == 0 && g_pagedPage > 0) {
        g_pagedPage--;
        pagedApplyPage();
        current_submenu_index = pagedFeatureCount() - 1;  // last item of the page we just landed on
        return true;
    }

    return false;
}

// Shared touch targeting for any paged submenu list screen (everything
// except WiFi/Bluetooth, which have their own richer touch dispatch).
// Returns the index a tap landed on — 0..featureCount-1 for a feature row,
// pagedBackBtnIndex() for "Main Menu" — so the caller can set
// current_submenu_index and fall into its own per-index dispatch chain, same
// as it already does for the physical SELECT path. Returns -1 if the tap was
// fully handled here already (a page flip) or missed everything.
static int pagedSubmenuTouchHit(int x, int y) {
    layoutPagedFooterButtons();
    const int n = (pagedPageCount() > 1) ? 2 : 1;
    const int footerHit = FeatureUI::hit(s_pagedFooterBtns, n, x, y);
    if (footerHit == 0) {
        return pagedBackBtnIndex();
    }
    if (footerHit == 1) {
        pagedAdvancePage();
        current_submenu_index = 0;
        pagedApplyPage();
        displaySubmenu();
        delay(200);
        return -1;
    }
    const int featureCount = pagedFeatureCount();
    for (int i = 0; i < featureCount; i++) {
        const int yPos = 30 + i * kPagedRowH;
        if (x >= 10 && x <= 220 && y >= yPos && y <= yPos + kPagedRowH) {
            return i;
        }
    }
    return -1;
}

void updateActiveSubmenu() {
    g_pagedPage = 0;
    switch (current_menu_index) {
        case 0:
        case 1:
        case 4:
        case 5:
        case 6:
            current_submenu_index = 0;
            pagedApplyPage();
            break;
        case 2:
            if (other_layer == OTHER_LAYER_HOME) {
                active_submenu_items = other_submenu_items;
                active_submenu_size = other_NUM_SUBMENU_ITEMS;
                active_submenu_icons = other_submenu_icons;
            } else {
                current_submenu_index = 0;
                pagedApplyPage();
            }
            break;
        case 3:
            active_submenu_items = nullptr;
            active_submenu_size = 0;
            active_submenu_icons = nullptr;
            break;
        case 7:
            active_submenu_items = nullptr;
            active_submenu_size = 0;
            active_submenu_icons = nullptr;
            break;

        default:
            active_submenu_items = nullptr;
            active_submenu_size = 0;
            active_submenu_icons = nullptr;
            break;
    }
}

static bool touchButtonInputEnabled = false;
static bool touchButtonCueDrawn = false;
static bool s_touchNavLabelsConfigured = false;
static bool s_touchNavHeld[5] = {false, false, false, false, false};
#if HAS_PCF8574_BUTTONS
static bool s_pcfButtonLastState[8] = {true, true, true, true, true, true, true, true};
#endif
static FeatureUI::Button s_touchNavBtns[5];
static const char* s_touchNavLabels[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
static constexpr int16_t TOUCH_NAV_BAR_H = (FeatureUI::FOOTER_H * 4) / 5;  // 20% shorter than footer

static int touchNavPinForIndex(int idx) {
  switch (idx) {
    case 0: return BTN_LEFT;
    case 1: return BTN_DOWN;
    case 2: return BTN_SELECT;
    case 3: return BTN_UP;
    case 4: return BTN_RIGHT;
    default: return -1;
  }
}

void setTouchButtonInputEnabled(bool enabled) {
  if (touchButtonInputEnabled != enabled) {
    touchButtonCueDrawn = false;
    if (!enabled) {
      for (int i = 0; i < 5; ++i) {
        s_touchNavLabels[i] = nullptr;
      }
      s_touchNavLabelsConfigured = false;
      for (int i = 0; i < 5; ++i) {
        s_touchNavHeld[i] = false;
      }
    }
  }
  touchButtonInputEnabled = enabled;
#if TOUCH_BUTTON_CUE_ENABLED
  if (enabled && feature_active) {
    drawTouchNavBar();
    touchButtonCueDrawn = true;
  }
#endif
}

bool featureHasTouchNavBar() {
#if TOUCH_BUTTON_CUE_ENABLED
  return touchButtonInputEnabled && feature_active;
#else
  return false;
#endif
}

void setTouchNavLabels(const char* left, const char* down, const char* center,
                       const char* up, const char* right) {
  s_touchNavLabels[0] = left;
  s_touchNavLabels[1] = down;
  s_touchNavLabels[2] = center;
  s_touchNavLabels[3] = up;
  s_touchNavLabels[4] = right;
  s_touchNavLabelsConfigured = true;
  invalidateTouchButtonCue();
}

void invalidateTouchButtonCue() {
  touchButtonCueDrawn = false;
}

void resetTouchNavHeldState() {
  for (int i = 0; i < 5; ++i) {
    s_touchNavHeld[i] = false;
  }
}

void redrawTouchButtonBar() {
  invalidateTouchButtonCue();
  drawTouchButtonCue();
}

static void layoutTouchNavBtns() {
  const int barY = tft.height() - TOUCH_NAV_BAR_H;
  const int barH = TOUCH_NAV_BAR_H;
  const int totalW = tft.width();
  const int cellW = totalW / 5;

  for (int i = 0; i < 5; ++i) {
    const int x = i * cellW;
    const int w = (i == 4) ? (totalW - x) : cellW;
    s_touchNavBtns[i] = {
      (int16_t)x, (int16_t)barY, (int16_t)w, (int16_t)barH,
      nullptr, FeatureUI::ButtonStyle::Secondary, false};
  }
}

static String fitTouchNavLabel(const char* label, int maxWidth) {
  if (!label || !label[0]) {
    return String();
  }
  String out = label;
  if (tft.textWidth(out) <= maxWidth) {
    return out;
  }
  while (out.length() > 1 && tft.textWidth(out + "...") > maxWidth) {
    out.remove(out.length() - 1);
  }
  return out + "...";
}

static void drawTouchNavBar() {
  static const unsigned char* kIcons[5] = {
    bitmap_icon_LEFT,
    bitmap_icon_DOWN,
    bitmap_icon_go_back,
    bitmap_icon_UP,
    bitmap_icon_RIGHT,
  };
  constexpr int kIconSize = 16;

  const int barY = tft.height() - TOUCH_NAV_BAR_H;
  const int barH = TOUCH_NAV_BAR_H;
  const int barW = tft.width();

  layoutTouchNavBtns();

  tft.fillRect(0, barY, barW, barH, UI_FG);
  tft.drawFastHLine(0, barY, barW, UI_LINE);

  for (int i = 0; i < 5; ++i) {
    const auto& b = s_touchNavBtns[i];
    if (i > 0) {
      tft.drawFastVLine(b.x, barY + 3, barH - 6, UI_LINE);
    }

    if (s_touchNavLabels[i] && s_touchNavLabels[i][0]) {
      tft.setTextDatum(MC_DATUM);
      const uint16_t txtColor = (i == 2) ? UI_ICON : UI_TEXT;
      tft.setTextColor(txtColor, UI_FG);
      const String fit = fitTouchNavLabel(s_touchNavLabels[i], b.w - 8);
      tft.drawString(fit, b.x + b.w / 2, b.y + b.h / 2, 1);
    } else {
      const int ix = b.x + (b.w - kIconSize) / 2;
      const int iy = b.y + (b.h - kIconSize) / 2;
      const bool inactiveSlot = s_touchNavLabelsConfigured && !s_touchNavLabels[i];
      const unsigned char* icon = inactiveSlot ? bitmap_icon_dots : kIcons[i];
      const uint16_t iconColor = inactiveSlot ? LIGHT_GRAY : ((i == 2) ? UI_ICON : UI_TEXT);
      tft.drawBitmap(ix, iy, icon, kIconSize, kIconSize, iconColor);
    }
  }

  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(UI_TEXT, FEATURE_BG);
}

void maintainTouchNavBar() {
#if TOUCH_BUTTON_CUE_ENABLED
  if (!touchButtonInputEnabled || !feature_active || touchButtonCueDrawn) {
    return;
  }
  drawTouchNavBar();
  touchButtonCueDrawn = true;
#endif
}

void featureClearContent(uint16_t color) {
  const int bottom = touchNavContentBottomY();
  if (bottom > 0) {
    tft.fillRect(0, 0, tft.width(), bottom, color);
  } else {
    tft.fillScreen(color);
  }
}

int16_t touchNavReservedHeight() {
#if TOUCH_BUTTON_CUE_ENABLED
  if (touchButtonInputEnabled && feature_active) {
    return TOUCH_NAV_BAR_H;
  }
#endif
  return 0;
}

int16_t touchNavContentBottomY() {
  return (int16_t)(tft.height() - touchNavReservedHeight());
}

void drawTouchButtonCue() {
#if TOUCH_BUTTON_CUE_ENABLED
  if (!touchButtonInputEnabled || !feature_active) {
    return;
  }
  drawTouchNavBar();
  touchButtonCueDrawn = true;
#endif
}

static int touchNavIndexForPin(int buttonPin) {
  for (int i = 0; i < 5; ++i) {
    if (touchNavPinForIndex(i) == buttonPin) {
      return i;
    }
  }
  return -1;
}

static bool isTouchNavSlotDown(int idx) {
  if (idx < 0 || !feature_active || !touchButtonInputEnabled) {
    return false;
  }

  int x = 0;
  int y = 0;
  if (!readTouchXYDismiss(x, y)) {
    return false;
  }

  layoutTouchNavBtns();

  const int stripTop = tft.height() - TOUCH_NAV_BAR_H;
  if (y < stripTop) {
    return false;
  }

  return FeatureUI::hit(s_touchNavBtns, 5, x, y) == idx;
}

// ---------------------------------------------------------------------------
// Debounce CENTRAL dos botoes fisicos (PCF8574).
// O estado "pressionado" so muda depois de permanecer estavel por
// BTN_DEBOUNCE_MS. Isso elimina de vez o "apertei 1x e o aparelho contou 2x".
// Toda feature deve usar isButtonPressed / isPhysicalButtonPressed /
// isButtonPressedEdge (que passam por aqui) e NAO ler o PCF cru.
// ---------------------------------------------------------------------------
// Debounce ASSIMETRICO (robusto e responsivo):
//  - APERTO: aceito na hora, com 1 leitura (responsivo; nao depende da taxa de
//    polling; botoes nunca "somem").
//  - SOLTA: so e aceita depois de permanecer solta por BTN_RELEASE_MS continuos.
//    Como o PCF8574 e lido por I2C disputado com outro core, uma leitura espuria
//    isolada ("solta" momentanea no meio do toque) e descartada -> acaba o
//    "apertei 1x e contou 2x" (que era um falso solta+aperta durante o hold).
static constexpr uint32_t BTN_RELEASE_MS = 40;
static bool     s_physStable[8]    = {false,false,false,false,false,false,false,false};
static bool     s_physRelPending[8]= {false,false,false,false,false,false,false,false};
static uint32_t s_physRelStart[8]  = {0,0,0,0,0,0,0,0};
static bool     s_physEdgeLast[8]  = {false,false,false,false,false,false,false,false};

// Contadores de diagnostico (lidos na tela do Settings): quantas descidas de
// borda RAW (leitura crua) e STB (estado debounced) por botao.
uint16_t g_btnRawDown[8] = {0,0,0,0,0,0,0,0};
uint16_t g_btnStbDown[8] = {0,0,0,0,0,0,0,0};

static bool physButtonDebounced(int buttonPin) {
#if HAS_PCF8574_BUTTONS
  if (getPcf8574Address() != 0) {
    const int idx = buttonPin & 7;
    const bool rawPressed = !pcf.digitalRead(buttonPin);   // ativo em nivel baixo
    const uint32_t now = millis();
    static bool s_rawPrev[8] = {false,false,false,false,false,false,false,false};
    if (rawPressed != s_rawPrev[idx]) { s_rawPrev[idx] = rawPressed; if (rawPressed) g_btnRawDown[idx]++; }
    const bool before = s_physStable[idx];
    if (rawPressed) {
      s_physRelPending[idx] = false;       // qualquer leitura de aperto cancela a solta
      s_physStable[idx] = true;            // aperto aceito imediatamente
    } else if (s_physStable[idx]) {
      if (!s_physRelPending[idx]) {
        s_physRelPending[idx] = true;      // candidata a solta: comeca a contar
        s_physRelStart[idx] = now;
      } else if ((uint32_t)(now - s_physRelStart[idx]) >= BTN_RELEASE_MS) {
        s_physStable[idx] = false;         // solta confirmada (persistiu) -> ignora glitch
        s_physRelPending[idx] = false;
      }
    } else {
      s_physRelPending[idx] = false;
    }
    if (s_physStable[idx] != before && s_physStable[idx]) g_btnStbDown[idx]++;
    return s_physStable[idx];
  }
#endif
  return false;
}

bool isPhysicalButtonPressed(int buttonPin) {
  return physButtonDebounced(buttonPin);
}

bool isTouchNavButtonPressed(int buttonPin) {
  const int idx = touchNavIndexForPin(buttonPin);
  if (idx < 0) {
    return false;
  }
  return isTouchNavSlotDown(idx);
}

bool isButtonPressed(int buttonPin) {
  if (isPhysicalButtonPressed(buttonPin)) {
    return true;
  }
  return isTouchNavButtonPressed(buttonPin);
}

bool isTouchNavButtonPressedEdge(int buttonPin) {
  if (!feature_active || !touchButtonInputEnabled) {
    return false;
  }

  const int navIdx = touchNavIndexForPin(buttonPin);
  if (navIdx < 0) {
    return false;
  }

  const bool down = isTouchNavSlotDown(navIdx);
  const bool edge = down && !s_touchNavHeld[navIdx];
  s_touchNavHeld[navIdx] = down;
  return edge;
}

bool isButtonPressedEdge(int buttonPin) {
#if HAS_PCF8574_BUTTONS
  if (getPcf8574Address() != 0) {
    const int idx = buttonPin & 7;
    const bool stable = physButtonDebounced(buttonPin);       // estado ja debounced
    const bool edge = stable && !s_physEdgeLast[idx];         // borda de pressionar
    s_physEdgeLast[idx] = stable;
    if (edge) {
      return true;
    }
  }
#endif

  return isTouchNavButtonPressedEdge(buttonPin);
}

// Consome a soltura REAL do botao (fisico ou touch-nav) antes de deixar a
// acao seguir adiante. Um toque humano pode durar mais que os ~200ms usados
// antes como "debounce" aqui pelos handlers de menu, o que fazia o MESMO
// toque ainda estar "pressionado" na proxima vez que o handler era chamado
// e ser contado como uma 2a (ou 3a) acao -- ex.: cursor do menu andando
// varias posicoes, ou o aperto que ABRIU um submenu sendo relido como a
// primeira opcao dele. O debounce central (physButtonDebounced) ja confirma
// corretamente 1 borda por toque (medido no aparelho); o bug era aqui, na
// camada de menu. Identico ao padrao que ja era usado so pro "<" (voltar).
static const uint32_t BTN_ACTION_RELEASE_MS = 60;
void waitButtonReleased(int buttonPin) {
    uint32_t t = millis();
    while ((uint32_t)(millis() - t) < BTN_ACTION_RELEASE_MS) {
        if (isButtonPressed(buttonPin)) t = millis();
        delay(5);
    }
}

bool featureExitButtonPressed() {
  return isPhysicalButtonPressed(BTN_SELECT) || isTouchNavButtonPressed(BTN_SELECT);
}

static void showFeatureUnavailable(const char* featureName, const char* requirement) {
  feature_active = false;
  feature_exit_requested = false;
  showNotification(featureName, requirement);
  delay(250);
}

static void runBleDuckyFeature() {
#if FEATURE_BLE_DUCKY
  current_submenu_index = 5;
  in_sub_menu = true;
  feature_active = true;
  feature_exit_requested = false;
  Ducky::enter();
  while (current_submenu_index == 5 && !feature_exit_requested) {
      current_submenu_index = 5;
      in_sub_menu = true;
      Ducky::loop();
  }

  Ducky::exit();
  if (feature_exit_requested) {
      in_sub_menu = true;
      is_main_menu = false;
      submenu_initialized = false;
      feature_active = false;
      feature_exit_requested = false;
      displaySubmenu();
      delay(200);
  }
#else
  showFeatureUnavailable("BLE Rubber Ducky", "This feature requires ESP32-S3.");
#endif
}

float currentBatteryVoltage = readBatteryVoltage();
unsigned long last_interaction_time = 0;

int last_menu_index = -1;
bool menu_initialized = false;

const int COLUMN_WIDTH = 120;
const int X_OFFSET_LEFT = 10;
const int X_OFFSET_RIGHT = X_OFFSET_LEFT + COLUMN_WIDTH;
const int Y_START = 30;
const int Y_SPACING = 75;

void displayOtherMenuGrid();
void displayPagedSubmenu();

// Last submenu item ("Back to Main Menu") is pinned to the bottom of the screen.
// Used by the touch-tap loops in the simpler paged submenus (nRF24, SubGHz,
// Tools, Other/IR/RFID/GPS) to find each row's on-screen Y — must match the
// kPagedRowH pitch displayPagedSubmenu() actually draws at, and the "Back to
// Main Menu" pin position matches the footer bar's own Y (pagedNavRowY()),
// since Back is drawn there now instead of inline. Correct for the common,
// single-page case; a menu that grows past kPagedItemsPerPage would still
// get Next/Prev via the physical UP/DOWN edge-flip, just not via a touch tap
// on the footer's "Next Page" half (not reachable through this loop).
static int submenuItemY(int index) {
    if (active_submenu_size > 0 && index == active_submenu_size - 1) {
        return tft.height() - 30;
    }
    return 30 + index * kPagedRowH;
}

// A PT-BR/ES translation routinely runs longer than its English source
// ("Settings" -> "Configuracoes"), which can overflow a 100px tile or a
// list row's tap zone. Returns `s` unchanged when it already fits inside
// maxWidth (measured with whatever font is currently loaded); otherwise
// returns a truncated copy with a trailing "..." that does fit. Uses a
// small rotating set of static buffers so it's safe to call more than once
// in the same expression (e.g. textWidth(fitText(...)) then print(fitText(...))).
static const char* fitText(const char* s, int maxWidth) {
    if (!s || tft.textWidth(s) <= maxWidth) return s;

    static char buf[4][40];
    static int slot = 0;
    char* out = buf[slot];
    slot = (slot + 1) % 4;

    // Reserve room for the trailing "..." (3 bytes) + NUL up front, so the
    // strcat() below can never overflow `out` regardless of how much the
    // while loop below ends up shortening `len` by.
    const size_t maxLen = sizeof(buf[0]) - 1 - 3;
    size_t len = strlen(s);
    if (len > maxLen) len = maxLen;
    memcpy(out, s, len);
    out[len] = '\0';

    while (len > 1) {
        String withEllipsis = String(out) + "...";
        if ((int)tft.textWidth(withEllipsis) <= maxWidth) break;
        len--;
        out[len] = '\0';
    }
    strcat(out, "...");
    return out;
}

void displaySubmenu() {
    setTouchButtonInputEnabled(false);

    if (current_menu_index == 2 && other_layer == OTHER_LAYER_HOME) {
        displayOtherMenuGrid();
        return;
    }

    // Every list-style submenu (WiFi, Bluetooth, nRF24, SubGHz, Tools, and
    // the Other menu's IR/RFID/GPS layers) goes through the same generic
    // paged renderer. The Other menu's HOME layer (tile grid, handled above)
    // and Settings/About (custom screens, never reach this function) are the
    // only exceptions.
    if (current_menu_index == 0 || current_menu_index == 1 || current_menu_index == 4 ||
        current_menu_index == 5 || current_menu_index == 6 ||
        (current_menu_index == 2 && other_layer != OTHER_LAYER_HOME)) {
        displayPagedSubmenu();
        return;
    }

    menu_initialized = false;
    last_menu_index = -1;

    tft.setTextFont(2);
    tft.setTextSize(1);

    if (!submenu_initialized) {
        tft.fillScreen(UI_BG);

        for (int i = 0; i < active_submenu_size; i++) {
            const int yPos = submenuItemY(i);
            const bool isBack = (i == active_submenu_size - 1);

            tft.setTextColor(UI_TEXT, UI_BG);
            tft.drawBitmap(10, yPos, active_submenu_icons[i], 16, 16, UI_TEXT);
            tft.setCursor(30, yPos);
            if (!isBack) {
                tft.print("| ");
            }
            tft.print(fitText(t(active_submenu_items[i]), 186));
        }

        submenu_initialized = true;
        last_submenu_index = -1;
    }

    if (last_submenu_index != current_submenu_index) {
        if (last_submenu_index >= 0) {
            const int prev_yPos = submenuItemY(last_submenu_index);
            const bool prevBack = (last_submenu_index == active_submenu_size - 1);

            tft.fillRect(0, prev_yPos, tft.width(), 28, UI_BG);
            tft.setTextColor(UI_TEXT, UI_BG);
            tft.drawBitmap(10, prev_yPos, active_submenu_icons[last_submenu_index], 16, 16, UI_TEXT);
            tft.setCursor(30, prev_yPos);
            if (!prevBack) {
                tft.print("| ");
            }
            tft.print(fitText(t(active_submenu_items[last_submenu_index]), 186));
        }

        const int new_yPos = submenuItemY(current_submenu_index);
        const bool newBack = (current_submenu_index == active_submenu_size - 1);

        tft.fillRect(0, new_yPos, tft.width(), 28, UI_BG);
        tft.setTextColor(UI_ICON, UI_BG);
        tft.drawBitmap(10, new_yPos, active_submenu_icons[current_submenu_index], 16, 16, UI_ICON);
        tft.setCursor(30, new_yPos);
        if (!newBack) {
            tft.print("| ");
        }
        tft.print(fitText(t(active_submenu_items[current_submenu_index]), 186));

        last_submenu_index = current_submenu_index;
    }

    drawStatusBar(currentBatteryVoltage, true);
}

// Tiny filled chevron hinting that the list continues onto the other page.
// Purely informational — whether it's needed depends only on which of the
// two pages is showing, never on the current selection, so it's drawn once
// per full redraw rather than tracked like the selection highlight.
//
// Sits inline in the right margin of the row it belongs to (the last item's
// row for the down arrow, the first item's row for the up arrow) instead of
// on its own line below/above the list. Two earlier versions tried to fit it
// into the vertical gap between the list and the footer/status bar instead,
// but that gap is real screen space someone may reasonably expect an item to
// use, which is exactly what was being complained about — reusing an
// existing row's right margin (past the item touch hitbox, which stops at
// x=220) claims no additional vertical space at all, regardless of screen
// height or row pitch.
static const int kPagedArrowHalfW = 4;
static const int kPagedArrowHalfH = 3;
static const int kPagedArrowX = 226;  // right margin, clear of the x<=220 item tap zone

// cy = vertical center of the 16px icon/text row this arrow sits beside.
static void drawPagedOverflowArrow(bool pointingDown, int cy) {
    const int cx = kPagedArrowX;
    tft.setTextColor(UI_TEXT, UI_BG);
    if (pointingDown) {
        tft.fillTriangle(cx - kPagedArrowHalfW, cy - kPagedArrowHalfH,
                          cx + kPagedArrowHalfW, cy - kPagedArrowHalfH,
                          cx, cy + kPagedArrowHalfH, UI_TEXT);
    } else {
        tft.fillTriangle(cx - kPagedArrowHalfW, cy + kPagedArrowHalfH,
                          cx + kPagedArrowHalfW, cy + kPagedArrowHalfH,
                          cx, cy - kPagedArrowHalfH, UI_TEXT);
    }
}

void displayPagedSubmenu() {
    menu_initialized = false;
    last_menu_index = -1;

    const int featureCount = pagedFeatureCount();
    tft.setTextFont(2);
    tft.setTextSize(1);

    if (!submenu_initialized) {
        tft.fillScreen(UI_BG);
        for (int i = 0; i < featureCount; i++) {
            const int yPos = 30 + i * kPagedRowH;
            tft.setTextColor(UI_TEXT, UI_BG);
            tft.drawBitmap(10, yPos, active_submenu_icons[i], 16, 16, UI_TEXT);
            tft.setCursor(30, yPos);
            tft.print("| ");
            tft.print(fitText(t(active_submenu_items[i]), 186));
        }

        // Show a down arrow on this page's last item when there's a page after
        // it, and an up arrow on the first item when there's a page before it
        // — works for any number of pages, not just 2. Each arrow rides inline
        // on the row it belongs to (see drawPagedOverflowArrow) rather than
        // claiming a line of its own.
        const int pageCount = pagedPageCount();
        if (g_pagedPage < pageCount - 1) {
            const int lastRowY = 30 + (featureCount - 1) * kPagedRowH;
            drawPagedOverflowArrow(/*pointingDown=*/true, lastRowY + 8);
        }
        if (g_pagedPage > 0) {
            drawPagedOverflowArrow(/*pointingDown=*/false, 30 + 8);
        }

        drawPagedFooterButtons();
        submenu_initialized = true;
        last_submenu_index = -1;
        s_pagedFooterFocus = -1;
    }

    if (last_submenu_index != current_submenu_index) {
        if (last_submenu_index >= 0 && last_submenu_index < featureCount) {
            const int prev_yPos = 30 + last_submenu_index * kPagedRowH;
            tft.setTextColor(UI_TEXT, UI_BG);
            tft.drawBitmap(10, prev_yPos, active_submenu_icons[last_submenu_index], 16, 16, UI_TEXT);
            tft.setCursor(30, prev_yPos);
            tft.print("| ");
            tft.print(fitText(t(active_submenu_items[last_submenu_index]), 186));
        }

        if (current_submenu_index >= 0 && current_submenu_index < featureCount) {
            const int new_yPos = 30 + current_submenu_index * kPagedRowH;
            tft.setTextColor(UI_ICON, UI_BG);
            tft.drawBitmap(10, new_yPos, active_submenu_icons[current_submenu_index], 16, 16, UI_ICON);
            tft.setCursor(30, new_yPos);
            tft.print("| ");
            tft.print(fitText(t(active_submenu_items[current_submenu_index]), 186));
            s_pagedFooterFocus = -1;
        } else if (current_submenu_index == pagedBackBtnIndex()) {
            s_pagedFooterFocus = 0;
        } else if (current_submenu_index == pagedPageBtnIndex()) {
            s_pagedFooterFocus = 1;
        } else {
            s_pagedFooterFocus = -1;
        }

        drawPagedFooterButtons();
        last_submenu_index = current_submenu_index;
    }

    drawStatusBar(currentBatteryVoltage, true);
}

void displayOtherMenuGrid() {
    applyThemeToPalette(settings().theme);

    submenu_initialized = false;
    last_submenu_index = -1;
    menu_initialized = false;
    last_menu_index = -1;

    tft.setTextFont(2);

    if (!other_menu_grid_initialized) {
        tft.fillScreen(UI_BG);

        for (int i = 0; i < other_NUM_SUBMENU_ITEMS; i++) {
            int column = i % OTHER_GRID_COLS;
            int row = i / OTHER_GRID_COLS;
            int x_position = (column == 0) ? X_OFFSET_LEFT : X_OFFSET_RIGHT;
            int y_position = Y_START + row * Y_SPACING;

            tft.fillRoundRect(x_position, y_position, 100, 60, 5, UI_FG);
            tft.drawRoundRect(x_position, y_position, 100, 60, 5, UI_LINE);
            tft.drawBitmap(x_position + 42, y_position + 10, other_submenu_icons[i], 16, 16, UI_ICON);

            tft.setTextColor(UI_TEXT, UI_FG);
            const char* label = fitText(t(other_submenu_items[i]), 92);
            int textWidth = tft.textWidth(label);
            int textX = x_position + (100 - textWidth) / 2;
            int textY = y_position + 30;
            tft.setCursor(textX, textY);
            tft.print(label);
        }

        other_menu_grid_initialized = true;
        last_other_menu_index = -1;
    }

    if (last_other_menu_index != current_submenu_index) {
        for (int i = 0; i < other_NUM_SUBMENU_ITEMS; i++) {
            int column = i % OTHER_GRID_COLS;
            int row = i / OTHER_GRID_COLS;
            int x_position = (column == 0) ? X_OFFSET_LEFT : X_OFFSET_RIGHT;
            int y_position = Y_START + row * Y_SPACING;

            if (i == last_other_menu_index) {
                tft.fillRoundRect(x_position, y_position, 100, 60, 5, UI_FG);
                tft.drawRoundRect(x_position, y_position, 100, 60, 5, UI_LINE);
                tft.setTextColor(UI_TEXT, UI_FG);
                tft.drawBitmap(x_position + 42, y_position + 10,
                               other_submenu_icons[last_other_menu_index], 16, 16, UI_ICON);
                const char* label = fitText(t(other_submenu_items[last_other_menu_index]), 92);
                int textWidth = tft.textWidth(label);
                int textX = x_position + (100 - textWidth) / 2;
                int textY = y_position + 30;
                tft.setCursor(textX, textY);
                tft.print(label);
            }
        }

        int column = current_submenu_index % OTHER_GRID_COLS;
        int row = current_submenu_index / OTHER_GRID_COLS;
        int x_position = (column == 0) ? X_OFFSET_LEFT : X_OFFSET_RIGHT;
        int y_position = Y_START + row * Y_SPACING;

        tft.fillRoundRect(x_position, y_position, 100, 60, 5, UI_FG);
        tft.drawRoundRect(x_position, y_position, 100, 60, 5, UI_ICON);

        tft.setTextColor(UI_ICON, UI_FG);
        tft.drawBitmap(x_position + 42, y_position + 10, other_submenu_icons[current_submenu_index],
                       16, 16, SELECTED_ICON_COLOR);
        const char* label = fitText(t(other_submenu_items[current_submenu_index]), 92);
        int textWidth = tft.textWidth(label);
        int textX = x_position + (100 - textWidth) / 2;
        int textY = y_position + 30;
        tft.setCursor(textX, textY);
        tft.print(label);

        last_other_menu_index = current_submenu_index;
    }

    drawStatusBar(currentBatteryVoltage, true);
}

/** Main menu "Other" tile (index 2): triple preview icons (LED / satellite / dots). */
static constexpr int MAIN_MENU_OTHER_IDX = 2;
static constexpr int MAIN_MENU_OTHER_ICON_GAP = 4;

static void drawMainMenuOtherTripleIcons(int x_position, int y_position, uint16_t iconColor) {
    const int tripleW = 16 * 3 + MAIN_MENU_OTHER_ICON_GAP * 2;
    int ix = x_position + (100 - tripleW) / 2;
    const int iy = y_position + 10;
    tft.drawBitmap(ix, iy, bitmap_icon_led, 16, 16, iconColor);
    tft.drawBitmap(ix + 16 + MAIN_MENU_OTHER_ICON_GAP, iy, bitmap_icon_satellite, 16, 16, iconColor);
    tft.drawBitmap(ix + 32 + MAIN_MENU_OTHER_ICON_GAP * 2, iy, bitmap_icon_down_dots, 16, 16, iconColor);
}

void displayMenu() {

  setTouchButtonInputEnabled(false);
  applyThemeToPalette(settings().theme);

const uint16_t icon_colors[NUM_MENU_ITEMS] = {
  UI_ICON,
  UI_ICON,
  UI_ICON,
  UI_ICON,
  UI_ICON,
  UI_ICON,
  UI_ICON,
  UI_ICON
};

    submenu_initialized = false;
    last_submenu_index = -1;
    other_menu_grid_initialized = false;
    last_other_menu_index = -1;
    tft.setTextFont(2);

    if (!menu_initialized) {
        tft.fillScreen(UI_BG);

        for (int i = 0; i < NUM_MENU_ITEMS; i++) {
            int column = i / 4;
            int row = i % 4;
            int x_position = (column == 0) ? X_OFFSET_LEFT : X_OFFSET_RIGHT;
            int y_position = Y_START + row * Y_SPACING;

            tft.fillRoundRect(x_position, y_position, 100, 60, 5, UI_FG);
            tft.drawRoundRect(x_position, y_position, 100, 60, 5, UI_LINE);
            if (i == MAIN_MENU_OTHER_IDX) {
                drawMainMenuOtherTripleIcons(x_position, y_position, icon_colors[i]);
            } else {
                tft.drawBitmap(x_position + 42, y_position + 10, bitmap_icons[i], 16, 16, icon_colors[i]);
            }

            tft.setTextColor(UI_TEXT, UI_FG);
            const char* label = fitText(t(menu_items[i]), 92);
            int textWidth = tft.textWidth(label);
            int textX = x_position + (100 - textWidth) / 2;
            int textY = y_position + 30;
            tft.setCursor(textX, textY);
            tft.print(label);
        }
        menu_initialized = true;
        last_menu_index = -1;
    }

    if (last_menu_index != current_menu_index) {
        for (int i = 0; i < NUM_MENU_ITEMS; i++) {
            int column = i / 4;
            int row = i % 4;
            int x_position = (column == 0) ? X_OFFSET_LEFT : X_OFFSET_RIGHT;
            int y_position = Y_START + row * Y_SPACING;

            if (i == last_menu_index) {
                tft.fillRoundRect(x_position, y_position, 100, 60, 5, UI_FG);
                tft.drawRoundRect(x_position, y_position, 100, 60, 5, UI_LINE);
                tft.setTextColor(UI_TEXT, UI_FG);
                if (last_menu_index == MAIN_MENU_OTHER_IDX) {
                    drawMainMenuOtherTripleIcons(x_position, y_position, icon_colors[last_menu_index]);
                } else {
                    tft.drawBitmap(x_position + 42, y_position + 10, bitmap_icons[last_menu_index], 16, 16, icon_colors[last_menu_index]);
                }
                const char* label = fitText(t(menu_items[last_menu_index]), 92);
                int textWidth = tft.textWidth(label);
                int textX = x_position + (100 - textWidth) / 2;
                int textY = y_position + 30;
                tft.setCursor(textX, textY);
                tft.print(label);
            }
        }

        int column = current_menu_index / 4;
        int row = current_menu_index % 4;
        int x_position = (column == 0) ? X_OFFSET_LEFT : X_OFFSET_RIGHT;
        int y_position = Y_START + row * Y_SPACING;

        tft.fillRoundRect(x_position, y_position, 100, 60, 5, UI_FG);
        tft.drawRoundRect(x_position, y_position, 100, 60, 5, UI_ICON);

        tft.setTextColor(UI_ICON, UI_FG);
        if (current_menu_index == MAIN_MENU_OTHER_IDX) {
            drawMainMenuOtherTripleIcons(x_position, y_position, SELECTED_ICON_COLOR);
        } else {
            tft.drawBitmap(x_position + 42, y_position + 10, bitmap_icons[current_menu_index], 16, 16, SELECTED_ICON_COLOR);
        }
        const char* label = fitText(t(menu_items[current_menu_index]), 92);
        int textWidth = tft.textWidth(label);
        int textX = x_position + (100 - textWidth) / 2;
        int textY = y_position + 30;
        tft.setCursor(textX, textY);
        tft.print(label);

        last_menu_index = current_menu_index;
    }
    drawStatusBar(currentBatteryVoltage, true);
}

// Paragrafo com quebra de linha manual (igual ao printWrappedText, mas devolve
// o Y final e para de desenhar se passar de maxY, pra nao invadir area vizinha).
static int drawWrappedParagraph(int x, int y, int maxWidth, int maxY, const char* text) {
    String msg = text ? String(text) : String("");
    msg.trim();
    const int lineH = 13;
    while (msg.length() > 0 && y <= maxY) {
        int lineEnd = msg.length();
        while (lineEnd > 0 && tft.textWidth(msg.substring(0, lineEnd)) > maxWidth) {
            lineEnd--;
        }
        if (lineEnd <= 0) {
            break;
        }
        if (lineEnd < msg.length()) {
            int lastSpace = msg.substring(0, lineEnd).lastIndexOf(' ');
            if (lastSpace > 0) {
                lineEnd = lastSpace;
            }
        }
        tft.setCursor(x, y);
        tft.print(msg.substring(0, lineEnd));
        msg = msg.substring(lineEnd);
        msg.trim();
        y += lineH;
    }
    return y;
}

// Full-screen info for the selected menu item when the user presses
// BTN_RIGHT, in the language chosen under Settings > Language. BTN_LEFT goes
// back to the submenu it was opened from.
static void drawFeatureInfoScreen(const char* title, const InfoText& info) {
    tft.fillScreen(UI_BG);
    currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true);

    const uint8_t lang = settings().infoLang;
    const int xPad = 14;
    const int maxWidth = tft.width() - 2 * xPad;
    const int maxY = tft.height() - 12;
    int y = 32;

    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(UI_ICON, UI_BG);
    tft.setCursor(xPad, y);
    tft.print(title);
    y += 20;

    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(UI_DIM_TEXT, UI_BG);
    tft.setCursor(xPad, y);
    tft.print("< voltar");
    y += 12;

    tft.drawFastHLine(xPad - 2, y, tft.width() - 2 * (xPad - 2), UI_LINE);
    y += 8;

    tft.setTextColor(UI_ICON, UI_BG);
    tft.setCursor(xPad, y);
    tft.print(INFO_LANG_NAMES[lang < INFO_LANG_COUNT ? lang : INFO_LANG_EN]);
    tft.print(":");
    y += 13;
    tft.setTextColor(UI_TEXT, UI_BG);
    drawWrappedParagraph(xPad, y, maxWidth, maxY, infoLangText(info, lang));
}

static void showFeatureInfoScreen(const char* title, const InfoText& info) {
    drawFeatureInfoScreen(title, info);
    while (true) {
        if (isButtonPressed(BTN_LEFT)) {
            waitButtonReleased(BTN_LEFT);
            break;
        }
        delay(10);
    }
}

void handleWiFiSubmenuButtons() {
    if (isButtonPressed(BTN_LEFT)) {   // "<" fisico volta ao menu principal
        waitButtonReleased(BTN_LEFT);  // espera soltar de verdade (evita reler o mesmo toque)
        in_sub_menu = false;
        feature_active = false;
        feature_exit_requested = false;
        g_pagedPage = 0;
        displayMenu();
        handleButtons();
        is_main_menu = false;
        return;
    }

    if (isButtonPressed(BTN_UP)) {
        if (!pagedSubmenuEdgeFlip(/*goingDown=*/false)) {
            current_submenu_index = (current_submenu_index - 1 + active_submenu_size) % active_submenu_size;
        }
        last_interaction_time = millis();
        displaySubmenu();
        waitButtonReleased(BTN_UP);
    }

    if (isButtonPressed(BTN_DOWN)) {
        if (!pagedSubmenuEdgeFlip(/*goingDown=*/true)) {
            current_submenu_index = (current_submenu_index + 1) % active_submenu_size;
        }
        last_interaction_time = millis();
        displaySubmenu();
        waitButtonReleased(BTN_DOWN);
    }

    // RIGHT on the selected item: opens a full-screen info page (all 12
    // WiFi features have text in wifi_page0_info -- WIFI_INFO_COUNT). WiFi
    // spans 2 paged-submenu pages, so the array index has to account for
    // which page we're on; current_submenu_index alone is only the index
    // *within* the current page. Also guard against the footer's
    // "Main Menu"/"Next Page" buttons, which share the index range just
    // past the real features (pagedFeatureCount()..).
    if (isButtonPressed(BTN_RIGHT)) {
        waitButtonReleased(BTN_RIGHT);
        const int wifiGlobalIndex = g_pagedPage * kPagedItemsPerPage + current_submenu_index;
        if (current_submenu_index < pagedFeatureCount() && wifiGlobalIndex < WIFI_INFO_COUNT) {
            showFeatureInfoScreen(t(wifi_items[wifiGlobalIndex]),
                                  wifi_page0_info[wifiGlobalIndex]);   // blocks until BTN_LEFT is released
            // The info screen used the whole display; force a full submenu
            // redraw (otherwise displaySubmenu() only does its usual
            // incremental update and leaves info-screen pixels on screen
            // until the next LEFT).
            submenu_initialized = false;
            last_submenu_index = -1;
            displaySubmenu();
        }
        return;
    }

    if (isButtonPressed(BTN_SELECT)) {
        last_interaction_time = millis();
        waitButtonReleased(BTN_SELECT);

        // The "Next/Prev Page" footer slot is touch-only (see
        // pagedSubmenuEdgeFlip) — UP/DOWN can no longer land current_submenu_index
        // on it, so there is no SELECT branch for it here anymore.

        // Footer: Back to Main Menu
        if (current_submenu_index == pagedBackBtnIndex()) {
            in_sub_menu = false;
            feature_active = false;
            feature_exit_requested = false;
            g_pagedPage = 0;
            displayMenu();
            handleButtons();
            is_main_menu = false;
            return;
        }

        if (g_pagedPage == 0 && current_submenu_index == 0) {
            current_submenu_index = 0;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            PacketMonitor::ptmSetup();
            // Saida e so do ptmLoop() (fisico LEFT / touch "Exit"), que seta
            // feature_exit_requested. NAO ter um checkpoint de saida aqui
            // tambem em cima do SELECT -- SELECT ficou livre nesta tela, e um
            // checkpoint redundante aqui fazia SELECT sair por engano.
            while (current_submenu_index == 0 && !feature_exit_requested) {
                current_submenu_index = 0;
                in_sub_menu = true;
                PacketMonitor::ptmLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (g_pagedPage == 0 && current_submenu_index == 1) {
            current_submenu_index = 1;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            BeaconSpammer::beaconSpamSetup();
            // Saida e so do beaconSpamLoop() (fisico LEFT / touch "Exit"), que
            // seta feature_exit_requested. NAO ter um checkpoint de saida aqui
            // tambem em cima do SELECT -- SELECT agora e o start/stop do spam,
            // e um checkpoint redundante aqui fazia SELECT sair em vez de
            // iniciar/parar o spam.
            while (current_submenu_index == 1 && !feature_exit_requested) {
                current_submenu_index = 1;
                in_sub_menu = true;
                BeaconSpammer::beaconSpamLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (g_pagedPage == 0 && current_submenu_index == 2) {
            current_submenu_index = 2;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            Deauther::deautherSetup();
            // Saida e so do deautherLoop() (fisico LEFT na lista de scan),
            // que seta feature_exit_requested. NAO ter um checkpoint de saida
            // aqui tambem em cima do SELECT -- SELECT agora e "View"/start-stop.
            while (current_submenu_index == 2 && !feature_exit_requested) {
                current_submenu_index = 2;
                in_sub_menu = true;
                Deauther::deautherLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (g_pagedPage == 0 && current_submenu_index == 3) {
            current_submenu_index = 3;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            ProbeRequestFlood::probeRequestFloodSetup();
            // Saida e so do probeRequestFloodLoop() (fisico LEFT na lista de
            // scan), que seta feature_exit_requested. NAO ter um checkpoint
            // de saida aqui tambem em cima do SELECT -- SELECT agora e
            // "View"/start-stop.
            while (current_submenu_index == 3 && !feature_exit_requested) {
                current_submenu_index = 3;
                in_sub_menu = true;
                ProbeRequestFlood::probeRequestFloodLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (g_pagedPage == 0 && current_submenu_index == 4) {
            current_submenu_index = 4;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            DeauthDetect::deauthdetectSetup();
            while (current_submenu_index == 4 && !feature_exit_requested) {
                current_submenu_index = 4;
                in_sub_menu = true;
                DeauthDetect::deauthdetectLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (g_pagedPage == 0 && current_submenu_index == 5) {
            current_submenu_index = 5;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            WifiScan::wifiscanSetup();
            while (current_submenu_index == 5 && !feature_exit_requested) {
                current_submenu_index = 5;
                in_sub_menu = true;
                WifiScan::wifiscanLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }
        if (g_pagedPage == 0 && current_submenu_index == 6) {
            current_submenu_index = 6;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            CaptivePortal::cportalSetup();
            while (current_submenu_index == 6 && !feature_exit_requested) {
                current_submenu_index = 6;
                in_sub_menu = true;
                CaptivePortal::cportalLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }
        if (g_pagedPage == 0 && current_submenu_index == 7) {
            current_submenu_index = 7;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            HiddenSsidReveal::hiddenSsidSetup();
            while (current_submenu_index == 7 && !feature_exit_requested) {
                current_submenu_index = 7;
                in_sub_menu = true;
                HiddenSsidReveal::hiddenSsidLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }
        // WPS Scanner moved from page1 slot 0 to page0 slot 8 so 9 items fit
        // on page 0 (was 8); page1's 3 remaining items (ARP Scanner, Karma
        // Attack, Channel Graph) were renumbered down by one slot to follow.
        if (g_pagedPage == 0 && current_submenu_index == 8) {
            current_submenu_index = 8;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            WpsScanner::wpsScannerSetup();
            while (current_submenu_index == 8 && !feature_exit_requested) {
                current_submenu_index = 8;
                in_sub_menu = true;
                WpsScanner::wpsScannerLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }
        if (g_pagedPage == 1 && current_submenu_index == 0) {
            current_submenu_index = 0;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            ArpScanner::arpScannerSetup();
            while (g_pagedPage == 1 && current_submenu_index == 0 && !feature_exit_requested) {
                current_submenu_index = 0;
                in_sub_menu = true;
                ArpScanner::arpScannerLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }
        if (g_pagedPage == 1 && current_submenu_index == 1) {
            current_submenu_index = 1;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            KarmaAttack::karmaSetup();
            while (g_pagedPage == 1 && current_submenu_index == 1 && !feature_exit_requested) {
                current_submenu_index = 1;
                in_sub_menu = true;
                KarmaAttack::karmaLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }
        if (g_pagedPage == 1 && current_submenu_index == 2) {
            current_submenu_index = 2;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            ChannelGraph::channelGraphSetup();
            while (g_pagedPage == 1 && current_submenu_index == 2 && !feature_exit_requested) {
                current_submenu_index = 2;
                in_sub_menu = true;
                ChannelGraph::channelGraphLoop();
                // LEFT exits here (not SELECT/featureExitButtonPressed): SELECT is
                // Rescan for this feature.
                if (isButtonPressed(BTN_LEFT)) {
                    in_sub_menu = true;
                    is_main_menu = false;
                    submenu_initialized = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displaySubmenu();
                    delay(200);
                    while (isButtonPressed(BTN_LEFT)) {
                    }
                    break;
                }
            }
            if (feature_exit_requested) {
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }
    }

    if (!feature_active) {
        int x, y;
        if (!readTouchXY(x, y)) { return; }
        delay(10);

        layoutPagedFooterButtons();
        const int footerHit = FeatureUI::hit(s_pagedFooterBtns, (pagedPageCount() > 1) ? 2 : 1, x, y);
        if (footerHit == 0) {
            // Left: Main Menu
            current_submenu_index = pagedBackBtnIndex();
            last_interaction_time = millis();
            displaySubmenu();
            delay(120);
            in_sub_menu = false;
            feature_active = false;
            feature_exit_requested = false;
            g_pagedPage = 0;
            displayMenu();
            handleButtons();
            is_main_menu = false;
            return;
        }
        if (footerHit == 1) {
            // Right: Next / Prev page
            current_submenu_index = pagedPageBtnIndex();
            last_interaction_time = millis();
            displaySubmenu();
            delay(120);
            pagedAdvancePage();
            current_submenu_index = 0;
            pagedApplyPage();
            displaySubmenu();
            delay(200);
            return;
        }

        const int featureCount = pagedFeatureCount();
        for (int i = 0; i < featureCount; i++) {
            int yPos = 30 + i * kPagedRowH;

            int button_x1 = 10;
            int button_y1 = yPos;
            int button_x2 = 220;
            int button_y2 = yPos + kPagedRowH;

            if (x >= button_x1 && x <= button_x2 && y >= button_y1 && y <= button_y2) {
                current_submenu_index = i;
                last_interaction_time = millis();
                displaySubmenu();
                delay(200);

                if (g_pagedPage == 0 && current_submenu_index == 0) {
                    current_submenu_index = 0;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    PacketMonitor::ptmSetup();
                    // Saida e so do ptmLoop() (fisico LEFT / touch "Exit"), que seta
                    // feature_exit_requested. NAO ter um checkpoint de saida aqui
                    // tambem em cima do SELECT -- SELECT ficou livre nesta tela, e um
                    // checkpoint redundante aqui fazia SELECT sair por engano.
                    while (current_submenu_index == 0 && !feature_exit_requested) {
                        current_submenu_index = 0;
                        in_sub_menu = true;
                        PacketMonitor::ptmLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 1) {
                    current_submenu_index = 1;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    BeaconSpammer::beaconSpamSetup();
                    // Saida e so do beaconSpamLoop() (fisico LEFT / touch "Exit"), que
                    // seta feature_exit_requested. NAO ter um checkpoint de saida aqui
                    // tambem em cima do SELECT -- SELECT agora e o start/stop do spam,
                    // e um checkpoint redundante aqui fazia SELECT sair em vez de
                    // iniciar/parar o spam.
                    while (current_submenu_index == 1 && !feature_exit_requested) {
                        current_submenu_index = 1;
                        in_sub_menu = true;
                        BeaconSpammer::beaconSpamLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 2) {
                    current_submenu_index = 2;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    Deauther::deautherSetup();
                    // Saida e so do deautherLoop() (fisico LEFT na lista de
                    // scan), que seta feature_exit_requested. NAO ter um
                    // checkpoint de saida aqui tambem em cima do SELECT --
                    // SELECT agora e "View"/start-stop.
                    while (current_submenu_index == 2 && !feature_exit_requested) {
                        current_submenu_index = 2;
                        in_sub_menu = true;
                        Deauther::deautherLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 3) {
                    current_submenu_index = 3;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    ProbeRequestFlood::probeRequestFloodSetup();
                    // Saida e so do probeRequestFloodLoop() (fisico LEFT na
                    // lista de scan), que seta feature_exit_requested. NAO
                    // ter um checkpoint de saida aqui tambem em cima do
                    // SELECT -- SELECT agora e "View"/start-stop.
                    while (current_submenu_index == 3 && !feature_exit_requested) {
                        current_submenu_index = 3;
                        in_sub_menu = true;
                        ProbeRequestFlood::probeRequestFloodLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 4) {
                    current_submenu_index = 4;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    DeauthDetect::deauthdetectSetup();
                    while (current_submenu_index == 4 && !feature_exit_requested) {
                        current_submenu_index = 4;
                        in_sub_menu = true;
                        DeauthDetect::deauthdetectLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 5) {
                    current_submenu_index = 5;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    WifiScan::wifiscanSetup();
                    while (current_submenu_index == 5 && !feature_exit_requested) {
                        current_submenu_index = 5;
                        in_sub_menu = true;
                        WifiScan::wifiscanLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 6) {
                    current_submenu_index = 6;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    CaptivePortal::cportalSetup();
                    while (current_submenu_index == 6 && !feature_exit_requested) {
                        current_submenu_index = 6;
                        in_sub_menu = true;
                        CaptivePortal::cportalLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 7) {
                    current_submenu_index = 7;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    HiddenSsidReveal::hiddenSsidSetup();
                    while (current_submenu_index == 7 && !feature_exit_requested) {
                        current_submenu_index = 7;
                        in_sub_menu = true;
                        HiddenSsidReveal::hiddenSsidLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 8) {
                    // See the matching comment in the physical-button dispatch above:
                    // WPS Scanner moved here from page1 slot 0 so 9 items fit on page 0.
                    current_submenu_index = 8;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    WpsScanner::wpsScannerSetup();
                    while (current_submenu_index == 8 && !feature_exit_requested) {
                        current_submenu_index = 8;
                        in_sub_menu = true;
                        WpsScanner::wpsScannerLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 1 && current_submenu_index == 0) {
                    current_submenu_index = 0;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    ArpScanner::arpScannerSetup();
                    while (g_pagedPage == 1 && current_submenu_index == 0 && !feature_exit_requested) {
                        current_submenu_index = 0;
                        in_sub_menu = true;
                        ArpScanner::arpScannerLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 1 && current_submenu_index == 1) {
                    current_submenu_index = 1;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    KarmaAttack::karmaSetup();
                    while (g_pagedPage == 1 && current_submenu_index == 1 && !feature_exit_requested) {
                        current_submenu_index = 1;
                        in_sub_menu = true;
                        KarmaAttack::karmaLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // evita reler o mesmo toque como "voltar" de novo no menu principal
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 1 && current_submenu_index == 2) {
                    current_submenu_index = 2;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    ChannelGraph::channelGraphSetup();
                    while (g_pagedPage == 1 && current_submenu_index == 2 && !feature_exit_requested) {
                        current_submenu_index = 2;
                        in_sub_menu = true;
                        ChannelGraph::channelGraphLoop();
                        // LEFT exits here (not SELECT/featureExitButtonPressed): SELECT is
                        // Rescan for this feature.
                        if (isButtonPressed(BTN_LEFT)) {
                            in_sub_menu = true;
                            is_main_menu = false;
                            submenu_initialized = false;
                            feature_active = false;
                            feature_exit_requested = false;
                            displaySubmenu();
                            delay(200);
                            while (isButtonPressed(BTN_LEFT)) {
                            }
                            break;
                        }
                    }
                    if (feature_exit_requested) {
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                }
                break;
            }
        }
    }
}

void handleBluetoothSubmenuButtons() {
    if (isButtonPressed(BTN_LEFT)) {   // "<" fisico volta ao menu principal
        waitButtonReleased(BTN_LEFT);  // espera soltar de verdade (evita reler o mesmo toque)
        in_sub_menu = false;
        feature_active = false;
        feature_exit_requested = false;
        g_pagedPage = 0;
        displayMenu();
        handleButtons();
        is_main_menu = false;
        return;
    }

    if (isButtonPressed(BTN_UP)) {
        if (!pagedSubmenuEdgeFlip(/*goingDown=*/false)) {
            current_submenu_index = (current_submenu_index - 1 + active_submenu_size) % active_submenu_size;
        }
        last_interaction_time = millis();
        displaySubmenu();
        waitButtonReleased(BTN_UP);
    }

    if (isButtonPressed(BTN_DOWN)) {
        if (!pagedSubmenuEdgeFlip(/*goingDown=*/true)) {
            current_submenu_index = (current_submenu_index + 1) % active_submenu_size;
        }
        last_interaction_time = millis();
        displaySubmenu();
        waitButtonReleased(BTN_DOWN);
    }

    // RIGHT on the selected item: opens a full-screen info page. All 9
    // Bluetooth features have text (BT_INFO_COUNT); they all fit on a
    // single paged-submenu page (9 == kPagedItemsPerPage), so no
    // page-relative index math is needed here, unlike the WiFi menu.
    if (isButtonPressed(BTN_RIGHT)) {
        waitButtonReleased(BTN_RIGHT);
        if (g_pagedPage == 0 && current_submenu_index < BT_INFO_COUNT) {
            showFeatureInfoScreen(t(bluetooth_items[current_submenu_index]),
                                  bluetooth_page0_info[current_submenu_index]);
            submenu_initialized = false;
            last_submenu_index = -1;
            displaySubmenu();
        }
        return;
    }

    if (isButtonPressed(BTN_SELECT)) {
        last_interaction_time = millis();
        waitButtonReleased(BTN_SELECT);

        // The "Next/Prev Page" footer slot is touch-only (see
        // pagedSubmenuEdgeFlip) — UP/DOWN can no longer land current_submenu_index
        // on it, so there is no SELECT branch for it here anymore.

        if (current_submenu_index == pagedBackBtnIndex()) {
            in_sub_menu = false;
            feature_active = false;
            feature_exit_requested = false;
            g_pagedPage = 0;
            displayMenu();
            handleButtons();
            is_main_menu = false;
            return;
        }

        if (g_pagedPage == 0 && current_submenu_index == 0) {
            current_submenu_index = 0;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            BleJammer::blejamSetup();
            while (g_pagedPage == 0 && current_submenu_index == 0 && !feature_exit_requested) {
                current_submenu_index = 0;
                in_sub_menu = true;
                BleJammer::blejamLoop();
            }
            BleJammer::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (g_pagedPage == 0 && current_submenu_index == 1) {
            current_submenu_index = 1;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            BleSpoofer::spooferSetup();
            while (g_pagedPage == 0 && current_submenu_index == 1 && !feature_exit_requested) {
                current_submenu_index = 1;
                in_sub_menu = true;
                BleSpoofer::spooferLoop();
            }
            BleSpoofer::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (g_pagedPage == 0 && current_submenu_index == 2) {
            current_submenu_index = 2;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            SourApple::sourappleSetup();
            while (g_pagedPage == 0 && current_submenu_index == 2 && !feature_exit_requested) {
                current_submenu_index = 2;
                in_sub_menu = true;
                SourApple::sourappleLoop();
            }
            SourApple::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (g_pagedPage == 0 && current_submenu_index == 3) {
            current_submenu_index = 3;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            AirTagSpoofer::airTagSetup();
            while (g_pagedPage == 0 && current_submenu_index == 3 && !feature_exit_requested) {
                current_submenu_index = 3;
                in_sub_menu = true;
                AirTagSpoofer::airTagLoop();
            }
            AirTagSpoofer::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (g_pagedPage == 0 && current_submenu_index == 4) {
            current_submenu_index = 4;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            AirTagSniffer::airTagSnifferSetup();
            while (g_pagedPage == 0 && current_submenu_index == 4 && !feature_exit_requested) {
                current_submenu_index = 4;
                in_sub_menu = true;
                AirTagSniffer::airTagSnifferLoop();
            }
            AirTagSniffer::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (g_pagedPage == 0 && current_submenu_index == 5) {
            current_submenu_index = 5;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            BleSniffer::blesnifferSetup();
            while (g_pagedPage == 0 && current_submenu_index == 5 && !feature_exit_requested) {
                current_submenu_index = 5;
                in_sub_menu = true;
                BleSniffer::blesnifferLoop();
            }
            BleSniffer::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (g_pagedPage == 0 && current_submenu_index == 6) {
            current_submenu_index = 6;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            BleScan::bleScanSetup();
            while (g_pagedPage == 0 && current_submenu_index == 6 && !feature_exit_requested) {
                current_submenu_index = 6;
                in_sub_menu = true;
                BleScan::bleScanLoop();
            }
            BleScan::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }
        if (g_pagedPage == 0 && current_submenu_index == 7) {
            runBleDuckyFeature();
        }

        // Skimmer Detect: was page1/slot0 back when BT was split 8+1; now all
        // 9 BT features fit on one page, so this is slot 8 of page 0.
        if (g_pagedPage == 0 && current_submenu_index == 8) {
            current_submenu_index = 8;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            BleSkimmer::bleSkimmerSetup();
            while (current_submenu_index == 8 && !feature_exit_requested) {
                current_submenu_index = 8;
                in_sub_menu = true;
                BleSkimmer::bleSkimmerLoop();
            }
            BleSkimmer::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }
    }

    if (!feature_active) {
        int x, y;
        if (!readTouchXY(x, y)) { return; }
        delay(10);

        layoutPagedFooterButtons();
        const int footerHit = FeatureUI::hit(s_pagedFooterBtns, (pagedPageCount() > 1) ? 2 : 1, x, y);
        if (footerHit == 0) {
            current_submenu_index = pagedBackBtnIndex();
            last_interaction_time = millis();
            displaySubmenu();
            delay(120);
            in_sub_menu = false;
            feature_active = false;
            feature_exit_requested = false;
            g_pagedPage = 0;
            displayMenu();
            handleButtons();
            is_main_menu = false;
            return;
        }
        if (footerHit == 1) {
            current_submenu_index = pagedPageBtnIndex();
            last_interaction_time = millis();
            displaySubmenu();
            delay(120);
            pagedAdvancePage();
            current_submenu_index = 0;
            pagedApplyPage();
            displaySubmenu();
            delay(200);
            return;
        }

        const int featureCount = pagedFeatureCount();
        for (int i = 0; i < featureCount; i++) {
            int yPos = 30 + i * kPagedRowH;

            int button_x1 = 10;
            int button_y1 = yPos;
            int button_x2 = 220;
            int button_y2 = yPos + kPagedRowH;

            if (x >= button_x1 && x <= button_x2 && y >= button_y1 && y <= button_y2) {
                current_submenu_index = i;
                last_interaction_time = millis();
                displaySubmenu();
                delay(200);

                if (g_pagedPage == 0 && current_submenu_index == 0) {
                    current_submenu_index = 0;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    BleJammer::blejamSetup();
                    while (g_pagedPage == 0 && current_submenu_index == 0 && !feature_exit_requested) {
                        current_submenu_index = 0;
                        in_sub_menu = true;
                        BleJammer::blejamLoop();
                    }
                    BleJammer::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 1) {
                    current_submenu_index = 1;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    BleSpoofer::spooferSetup();
                    while (g_pagedPage == 0 && current_submenu_index == 1 && !feature_exit_requested) {
                        current_submenu_index = 1;
                        in_sub_menu = true;
                        BleSpoofer::spooferLoop();
                    }
                    BleSpoofer::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 2) {
                    current_submenu_index = 2;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    SourApple::sourappleSetup();
                    while (g_pagedPage == 0 && current_submenu_index == 2 && !feature_exit_requested) {
                        current_submenu_index = 2;
                        in_sub_menu = true;
                        SourApple::sourappleLoop();
                    }
                    SourApple::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 3) {
                    current_submenu_index = 3;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    AirTagSpoofer::airTagSetup();
                    while (g_pagedPage == 0 && current_submenu_index == 3 && !feature_exit_requested) {
                        current_submenu_index = 3;
                        in_sub_menu = true;
                        AirTagSpoofer::airTagLoop();
                    }
                    AirTagSpoofer::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 4) {
                    current_submenu_index = 4;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    AirTagSniffer::airTagSnifferSetup();
                    while (g_pagedPage == 0 && current_submenu_index == 4 && !feature_exit_requested) {
                        current_submenu_index = 4;
                        in_sub_menu = true;
                        AirTagSniffer::airTagSnifferLoop();
                    }
                    AirTagSniffer::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 5) {
                    current_submenu_index = 5;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    BleSniffer::blesnifferSetup();
                    while (g_pagedPage == 0 && current_submenu_index == 5 && !feature_exit_requested) {
                        current_submenu_index = 5;
                        in_sub_menu = true;
                        BleSniffer::blesnifferLoop();
                    }
                    BleSniffer::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 6) {
                    current_submenu_index = 6;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    BleScan::bleScanSetup();
                    while (g_pagedPage == 0 && current_submenu_index == 6 && !feature_exit_requested) {
                        current_submenu_index = 6;
                        in_sub_menu = true;
                        BleScan::bleScanLoop();
                    }
                    BleScan::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (g_pagedPage == 0 && current_submenu_index == 7) {
                    runBleDuckyFeature();
                } else if (g_pagedPage == 0 && current_submenu_index == 8) {
                    current_submenu_index = 8;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    BleSkimmer::bleSkimmerSetup();
                    while (current_submenu_index == 8 && !feature_exit_requested) {
                        current_submenu_index = 8;
                        in_sub_menu = true;
                        BleSkimmer::bleSkimmerLoop();
                    }
                    BleSkimmer::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                }
                break;
            }
        }
    }
}
void handleNRFSubmenuButtons() {
    if (isButtonPressed(BTN_LEFT)) {   // "<" fisico volta ao menu principal
        waitButtonReleased(BTN_LEFT);  // espera soltar de verdade (evita reler o mesmo toque)
        in_sub_menu = false;
        feature_active = false;
        feature_exit_requested = false;
        displayMenu();
        handleButtons();
        is_main_menu = false;
        return;
    }

    if (isButtonPressed(BTN_UP)) {
        if (!pagedSubmenuEdgeFlip(/*goingDown=*/false)) {
            current_submenu_index = (current_submenu_index - 1 + active_submenu_size) % active_submenu_size;
        }
        last_interaction_time = millis();
        displaySubmenu();
        waitButtonReleased(BTN_UP);
    }

    if (isButtonPressed(BTN_DOWN)) {
        if (!pagedSubmenuEdgeFlip(/*goingDown=*/true)) {
            current_submenu_index = (current_submenu_index + 1) % active_submenu_size;
        }
        last_interaction_time = millis();
        displaySubmenu();
        waitButtonReleased(BTN_DOWN);
    }

    // RIGHT no item selecionado: abre uma tela cheia com a info (EN + PT-BR)
    // (nao mostra no ultimo item, "Back to Main Menu").
    if (isButtonPressed(BTN_RIGHT)) {
        waitButtonReleased(BTN_RIGHT);
        if (current_submenu_index < nrf_NUM_SUBMENU_ITEMS - 1) {
            showFeatureInfoScreen(t(nrf_submenu_items[current_submenu_index]),
                                  nrf_info[current_submenu_index]);
            submenu_initialized = false;
            last_submenu_index = -1;
            displaySubmenu();
        }
        return;
    }

    if (isButtonPressed(BTN_SELECT)) {
        last_interaction_time = millis();
        waitButtonReleased(BTN_SELECT);

        if (current_submenu_index == 6) {
            in_sub_menu = false;
            feature_active = false;
            feature_exit_requested = false;
            displayMenu();
            handleButtons();
            is_main_menu = false;
        }

        if (current_submenu_index == 0) {
            current_submenu_index = 0;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            Scanner::scannerSetup();
            while (current_submenu_index == 0 && !feature_exit_requested) {
                current_submenu_index = 0;
                in_sub_menu = true;
                Scanner::scannerLoop();
            }
            Scanner::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (current_submenu_index == 1) {
            current_submenu_index = 1;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            ProtoKill::prokillSetup();
            while (current_submenu_index == 1 && !feature_exit_requested) {
                current_submenu_index = 1;
                in_sub_menu = true;
                ProtoKill::prokillLoop();
            }
            ProtoKill::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (current_submenu_index == 2) {
            current_submenu_index = 2;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            EsbSniffer::esbSnifferSetup();
            while (current_submenu_index == 2 && !feature_exit_requested) {
                current_submenu_index = 2;
                in_sub_menu = true;
                EsbSniffer::esbSnifferLoop();
            }
            EsbSniffer::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (current_submenu_index == 3) {
            current_submenu_index = 3;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            EsbReplay::esbReplaySetup();
            while (current_submenu_index == 3 && !feature_exit_requested) {
                current_submenu_index = 3;
                in_sub_menu = true;
                EsbReplay::esbReplayLoop();
            }
            EsbReplay::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (current_submenu_index == 4) {
            current_submenu_index = 4;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            MouseJack::mouseJackSetup();
            while (current_submenu_index == 4 && !feature_exit_requested) {
                current_submenu_index = 4;
                in_sub_menu = true;
                MouseJack::mouseJackLoop();
            }
            MouseJack::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (current_submenu_index == 5) {
            current_submenu_index = 5;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            MouseJackInject::mouseJackInjectSetup();
            while (current_submenu_index == 5 && !feature_exit_requested) {
                current_submenu_index = 5;
                in_sub_menu = true;
                MouseJackInject::mouseJackInjectLoop();
            }
            MouseJackInject::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }
    }

    if (!feature_active) {
        int x, y;
        if (!readTouchXY(x, y)) { return; }
        delay(10);
        for (int i = 0; i < active_submenu_size; i++) {
            int yPos = submenuItemY(i);

            int button_x1 = 10;
            int button_y1 = yPos;
            int button_x2 = 220;
            int button_y2 = yPos + 28;

            if (x >= button_x1 && x <= button_x2 && y >= button_y1 && y <= button_y2) {
                current_submenu_index = i;
                last_interaction_time = millis();
                displaySubmenu();
                delay(200);

                if (current_submenu_index == 6) {
                    in_sub_menu = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displayMenu();
                    handleButtons();
                    is_main_menu = false;
                } else if (current_submenu_index == 0) {
                    current_submenu_index = 0;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    Scanner::scannerSetup();
                    while (current_submenu_index == 0 && !feature_exit_requested) {
                        current_submenu_index = 0;
                        in_sub_menu = true;
                        Scanner::scannerLoop();
                    }
                    Scanner::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (current_submenu_index == 1) {
                    current_submenu_index = 1;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    ProtoKill::prokillSetup();
                    while (current_submenu_index == 1 && !feature_exit_requested) {
                        current_submenu_index = 1;
                        in_sub_menu = true;
                        ProtoKill::prokillLoop();
                    }
                    ProtoKill::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (current_submenu_index == 2) {
                    current_submenu_index = 2;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    EsbSniffer::esbSnifferSetup();
                    while (current_submenu_index == 2 && !feature_exit_requested) {
                        current_submenu_index = 2;
                        in_sub_menu = true;
                        EsbSniffer::esbSnifferLoop();
                    }
                    EsbSniffer::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (current_submenu_index == 3) {
                    current_submenu_index = 3;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    EsbReplay::esbReplaySetup();
                    while (current_submenu_index == 3 && !feature_exit_requested) {
                        current_submenu_index = 3;
                        in_sub_menu = true;
                        EsbReplay::esbReplayLoop();
                    }
                    EsbReplay::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (current_submenu_index == 4) {
                    current_submenu_index = 4;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    MouseJack::mouseJackSetup();
                    while (current_submenu_index == 4 && !feature_exit_requested) {
                        current_submenu_index = 4;
                        in_sub_menu = true;
                        MouseJack::mouseJackLoop();
                    }
                    MouseJack::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (current_submenu_index == 5) {
                    current_submenu_index = 5;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    MouseJackInject::mouseJackInjectSetup();
                    while (current_submenu_index == 5 && !feature_exit_requested) {
                        current_submenu_index = 5;
                        in_sub_menu = true;
                        MouseJackInject::mouseJackInjectLoop();
                    }
                    MouseJackInject::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                }
                break;
            }
        }
    }
}
void handleSubGHzSubmenuButtons() {
    if (isButtonPressed(BTN_LEFT)) {   // "<" fisico volta ao menu principal
        waitButtonReleased(BTN_LEFT);  // espera soltar de verdade (evita reler o mesmo toque)
        in_sub_menu = false;
        feature_active = false;
        feature_exit_requested = false;
        displayMenu();
        handleButtons();
        is_main_menu = false;
        return;
    }

    if (isButtonPressed(BTN_UP)) {
        if (!pagedSubmenuEdgeFlip(/*goingDown=*/false)) {
            current_submenu_index = (current_submenu_index - 1 + active_submenu_size) % active_submenu_size;
        }
        last_interaction_time = millis();
        displaySubmenu();
        waitButtonReleased(BTN_UP);
    }

    if (isButtonPressed(BTN_DOWN)) {
        if (!pagedSubmenuEdgeFlip(/*goingDown=*/true)) {
            current_submenu_index = (current_submenu_index + 1) % active_submenu_size;
        }
        last_interaction_time = millis();
        displaySubmenu();
        waitButtonReleased(BTN_DOWN);
    }

    // RIGHT no item selecionado: abre uma tela cheia com a info (EN + PT-BR)
    // (nao mostra no ultimo item, "Back to Main Menu").
    if (isButtonPressed(BTN_RIGHT)) {
        waitButtonReleased(BTN_RIGHT);
        if (current_submenu_index < subghz_NUM_SUBMENU_ITEMS - 1) {
            showFeatureInfoScreen(t(subghz_submenu_items[current_submenu_index]),
                                  subghz_info[current_submenu_index]);
            submenu_initialized = false;
            last_submenu_index = -1;
            displaySubmenu();
        }
        return;
    }

    if (isButtonPressed(BTN_SELECT)) {
        last_interaction_time = millis();
        waitButtonReleased(BTN_SELECT);

        if (current_submenu_index == 5) {
            in_sub_menu = false;
            feature_active = false;
            feature_exit_requested = false;
            displayMenu();
            handleButtons();
            is_main_menu = false;
        }

        if (current_submenu_index == 0) {
            current_submenu_index = 0;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            replayat::ReplayAttackSetup();
            while (current_submenu_index == 0 && !feature_exit_requested) {
                current_submenu_index = 0;
                in_sub_menu = true;
                replayat::ReplayAttackLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (current_submenu_index == 1) {
            current_submenu_index = 1;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            subjammer::subjammerSetup();
            while (current_submenu_index == 1 && !feature_exit_requested) {
                current_submenu_index = 1;
                in_sub_menu = true;
                subjammer::subjammerLoop();
            }
            subjammer::exit();
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (current_submenu_index == 2) {
            current_submenu_index = 2;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            SubBrute::subBruteSetup();
            while (current_submenu_index == 2 && !feature_exit_requested) {
                current_submenu_index = 2;
                in_sub_menu = true;
                SubBrute::subBruteLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (current_submenu_index == 3) {
            current_submenu_index = 3;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            jammingdetector::Setup();
            while (current_submenu_index == 3 && !feature_exit_requested) {
                current_submenu_index = 3;
                in_sub_menu = true;
                jammingdetector::Loop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }

        if (current_submenu_index == 4) {
            current_submenu_index = 4;
            in_sub_menu = true;
            feature_active = true;
            feature_exit_requested = false;
            SavedProfile::saveSetup();
            while (current_submenu_index == 4 && !feature_exit_requested) {
                current_submenu_index = 4;
                in_sub_menu = true;
                SavedProfile::saveLoop();
            }
            if (feature_exit_requested) {
                waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                in_sub_menu = true;
                is_main_menu = false;
                submenu_initialized = false;
                feature_active = false;
                feature_exit_requested = false;
                displaySubmenu();
                delay(200);
            }
        }
    }

    if (!feature_active) {
        int x, y;
        if (!readTouchXY(x, y)) { return; }
        delay(10);
        for (int i = 0; i < active_submenu_size; i++) {
            int yPos = submenuItemY(i);

            int button_x1 = 10;
            int button_y1 = yPos;
            int button_x2 = 220;
            int button_y2 = yPos + 28;

            if (x >= button_x1 && x <= button_x2 && y >= button_y1 && y <= button_y2) {
                current_submenu_index = i;
                last_interaction_time = millis();
                displaySubmenu();
                delay(200);

                if (current_submenu_index == 5) {
                    in_sub_menu = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displayMenu();
                    handleButtons();
                    is_main_menu = false;
                } else if (current_submenu_index == 0) {
                    current_submenu_index = 0;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    replayat::ReplayAttackSetup();
                    while (current_submenu_index == 0 && !feature_exit_requested) {
                        current_submenu_index = 0;
                        in_sub_menu = true;
                        replayat::ReplayAttackLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (current_submenu_index == 1) {
                    current_submenu_index = 1;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    subjammer::subjammerSetup();
                    while (current_submenu_index == 1 && !feature_exit_requested) {
                        current_submenu_index = 1;
                        in_sub_menu = true;
                        subjammer::subjammerLoop();
                    }
                    subjammer::exit();
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (current_submenu_index == 2) {
                    current_submenu_index = 2;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    SubBrute::subBruteSetup();
                    while (current_submenu_index == 2 && !feature_exit_requested) {
                        current_submenu_index = 2;
                        in_sub_menu = true;
                        SubBrute::subBruteLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (current_submenu_index == 3) {
                    current_submenu_index = 3;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    jammingdetector::Setup();
                    while (current_submenu_index == 3 && !feature_exit_requested) {
                        current_submenu_index = 3;
                        in_sub_menu = true;
                        jammingdetector::Loop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                } else if (current_submenu_index == 4) {
                    current_submenu_index = 4;
                    in_sub_menu = true;
                    feature_active = true;
                    feature_exit_requested = false;
                    SavedProfile::saveSetup();
                    while (current_submenu_index == 4 && !feature_exit_requested) {
                        current_submenu_index = 4;
                        in_sub_menu = true;
                        SavedProfile::saveLoop();
                    }
                    if (feature_exit_requested) {
                        waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                        in_sub_menu = true;
                        is_main_menu = false;
                        submenu_initialized = false;
                        feature_active = false;
                        feature_exit_requested = false;
                        displaySubmenu();
                        delay(200);
                    }
                }
                break;
            }
        }
    }
}

constexpr int TOOLS_IDX_TERMINAL = 0;
constexpr int TOOLS_IDX_UPDATE   = 1;
constexpr int TOOLS_IDX_TOUCH    = 2;
constexpr int TOOLS_IDX_SD_FILES = 3;
constexpr int TOOLS_IDX_SETTINGS = -1;
constexpr int TOOLS_IDX_BACK     = 4;

static void runToolsFeatureExitCleanup() {
    in_sub_menu = true;
    is_main_menu = false;
    submenu_initialized = false;
    feature_active = false;
    feature_exit_requested = false;
    setTouchButtonInputEnabled(false);
    setTouchNavLabels(nullptr, nullptr, nullptr, nullptr, nullptr);
    resetTouchNavHeldState();
    displaySubmenu();
    delay(200);
    // Remapped layout: LEFT exits now (was SELECT) -- wait for its release
    // so the submenu above doesn't re-read the same press as "back" again.
    while (isButtonPressed(BTN_LEFT)) {
    }
}

static void runToolsFeature(int idx, void (*setupFn)(), void (*loopFn)()) {
    const bool useTouchNav = (idx != TOOLS_IDX_TOUCH);
    current_submenu_index = idx;
    in_sub_menu = true;
    feature_active = true;
    feature_exit_requested = false;
    if (useTouchNav) {
        setTouchButtonInputEnabled(true);
    }
    setupFn();
    while (current_submenu_index == idx && !feature_exit_requested) {
        current_submenu_index = idx;
        in_sub_menu = true;
        loopFn();
        if (feature_exit_requested) {
            waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
            break;
        }
        // Remapped layout: LEFT exits now (was SELECT) -- Touch Calibrate
        // doesn't use the touch nav bar (it needs raw touch input), so it
        // falls back to a direct physical-button check here.
        if (!useTouchNav && isButtonPressed(BTN_LEFT)) {
            break;
        }
    }
    runToolsFeatureExitCleanup();
}

static void launchToolsFeature(int idx) {
    switch (idx) {
        case TOOLS_IDX_TERMINAL:
            runToolsFeature(idx, Terminal::terminalSetup, Terminal::terminalLoop);
            break;
        case TOOLS_IDX_UPDATE:
            runToolsFeature(idx, FirmwareUpdate::updateSetup, FirmwareUpdate::updateLoop);
            break;
        case TOOLS_IDX_TOUCH:
            runToolsFeature(idx, TouchCalib::setup, TouchCalib::loop);
            break;
        case TOOLS_IDX_SD_FILES:
            runToolsFeature(idx, SdFileManager::setup, SdFileManager::loop);
            break;
        default:
            break;
    }
}
void handleToolsSubmenuButtons() {
    if (isButtonPressed(BTN_LEFT)) {   // "<" fisico volta ao menu principal
        waitButtonReleased(BTN_LEFT);  // espera soltar de verdade (evita reler o mesmo toque)
        in_sub_menu = false;
        feature_active = false;
        feature_exit_requested = false;
        displayMenu();
        handleButtons();
        is_main_menu = false;
        return;
    }

    if (isButtonPressed(BTN_UP)) {
        if (!pagedSubmenuEdgeFlip(/*goingDown=*/false)) {
            current_submenu_index = (current_submenu_index - 1 + active_submenu_size) % active_submenu_size;
        }
        last_interaction_time = millis();
        displaySubmenu();
        waitButtonReleased(BTN_UP);
    }

    if (isButtonPressed(BTN_DOWN)) {
        if (!pagedSubmenuEdgeFlip(/*goingDown=*/true)) {
            current_submenu_index = (current_submenu_index + 1) % active_submenu_size;
        }
        last_interaction_time = millis();
        displaySubmenu();
        waitButtonReleased(BTN_DOWN);
    }

    // RIGHT no item selecionado: abre uma tela cheia com a info (EN + PT-BR)
    // (nao mostra no ultimo item, "Back to Main Menu").
    if (isButtonPressed(BTN_RIGHT)) {
        waitButtonReleased(BTN_RIGHT);
        if (current_submenu_index < tools_NUM_SUBMENU_ITEMS - 1) {
            showFeatureInfoScreen(t(tools_submenu_items[current_submenu_index]),
                                  tools_info[current_submenu_index]);
            submenu_initialized = false;
            last_submenu_index = -1;
            displaySubmenu();
        }
        return;
    }

    if (isButtonPressed(BTN_SELECT)) {
        last_interaction_time = millis();
        waitButtonReleased(BTN_SELECT);

        if (current_submenu_index == TOOLS_IDX_BACK) {
            in_sub_menu = false;
            feature_active = false;
            feature_exit_requested = false;
            displayMenu();
            handleButtons();
            is_main_menu = false;
            return;
        }

        launchToolsFeature(current_submenu_index);
        return;
    }

    if (!feature_active) {
        int x, y;
        if (!readTouchXY(x, y)) {
            return;
        }

        for (int i = 0; i < active_submenu_size; i++) {
            int yPos = submenuItemY(i);

            int button_x1 = 10;
            int button_y1 = yPos;
            int button_x2 = 220;
            int button_y2 = yPos + 28;

            if (x >= button_x1 && x <= button_x2 && y >= button_y1 && y <= button_y2) {
                current_submenu_index = i;
                last_interaction_time = millis();
                displaySubmenu();
                delay(200);

                if (current_submenu_index == TOOLS_IDX_BACK) {
                    in_sub_menu = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displayMenu();
                    handleButtons();
                    is_main_menu = false;
                } else {
                    launchToolsFeature(current_submenu_index);
                }
                break;
            }
        }
    }
}

static void otherDismissPlaceholder() {
    delay(25);
    while (isButtonPressed(BTN_SELECT) || isButtonPressed(BTN_LEFT)) {
        delay(5);
    }
    while (!isButtonPressed(BTN_SELECT) && !isButtonPressed(BTN_LEFT)) {
        int x = 0, y = 0;
        if (!readTouchXYDismiss(x, y) && !readTouchXY(x, y)) {
            delay(12);
            continue;
        }
        if (isNotificationVisible()) {
            NotificationAction a = notificationHandleTouch(x, y);
            if (a != NotificationAction::None) {
                break;
            }
            hideNotification();
        }
        break;
    }
    if (in_sub_menu) {
        submenu_initialized = false;
        if (current_menu_index == 2 && other_layer == OTHER_LAYER_HOME) {
            other_menu_grid_initialized = false;
            last_other_menu_index = -1;
        }
        displaySubmenu();
    }
}

static void otherRfidReturnGuard() {
    delay(120);
    for (int i = 0; i < 120; i++) {
        if (!isButtonPressed(BTN_SELECT) && !isButtonPressed(BTN_LEFT) &&
            !isButtonPressed(BTN_RIGHT) && !isButtonPressed(BTN_UP) &&
            !isButtonPressed(BTN_DOWN) && !isTouchDownDismiss()) {
            break;
        }
        delay(5);
    }
    delay(120);
}

static void otherRfidPlaceholderAction(int idx) {
    feature_active = true;
    if (!RfidNfc::begin()) {
        showNotification("RFID/NFC", "PN532 not found. Check SPI wiring/pins.");
        otherDismissPlaceholder();
        feature_active = false;
        return;
    }
    feature_exit_requested = false;
    setTouchButtonInputEnabled(true);
    for (;;) {
        RfidNfc::clearSessionRetry();
        switch (idx) {
            case 0:
                RfidNfc::sessionCardReader();
                break;
            case 1:
                RfidNfc::sessionClone();
                break;
            case 2:
                RfidNfc::sessionErase();
                break;
            case 3:
                RfidNfc::sessionDump();
                break;
            case 4:
                RfidNfc::sessionDecodeAccess();
                break;
            case 5:
                RfidNfc::sessionJamReader();
                break;
            case 6:
                RfidNfc::sessionTagDisrupt();
                break;
            case 7:
                RfidNfc::sessionDisruptEmulate();
                break;
            default:
                feature_active = false;
                restoreSdAfterSharedSpi();
                return;
        }
        if (feature_exit_requested || !RfidNfc::consumeSessionRetry()) {
            break;
        }
        feature_exit_requested = false;
    }
    restoreSdAfterSharedSpi();
    otherRfidReturnGuard();
    submenu_initialized = false;
    displaySubmenu();
    feature_active = false;
}

static void otherGpsPlaceholderAction(int idx) {
    feature_active = true;
    if (idx == 0) {
        feature_exit_requested = false;
        setTouchButtonInputEnabled(true);
        for (;;) {
            GpsWardriver::clearSessionRetry();
            GpsWardriver::session();
            if (feature_exit_requested || !GpsWardriver::consumeSessionRetry()) {
                break;
            }
            feature_exit_requested = false;
        }
        setTouchButtonInputEnabled(false);
    } else {
        switch (idx) {
            case 1:
                GpsSatelliteScanner::session();
                break;
            default:
                feature_active = false;
                return;
        }
    }
    otherRfidReturnGuard();
    submenu_initialized = false;
    displaySubmenu();
    feature_active = false;
}

void handleOtherSubmenuButtons() {
    if (other_layer == OTHER_LAYER_HOME) {
        const int og_rows =
            (other_NUM_SUBMENU_ITEMS + OTHER_GRID_COLS - 1) / OTHER_GRID_COLS;

        if (isButtonPressed(BTN_UP)) {
            int row = current_submenu_index / OTHER_GRID_COLS;
            if (row > 0) {
                current_submenu_index -= OTHER_GRID_COLS;
            } else {
                current_submenu_index += OTHER_GRID_COLS * (og_rows - 1);
            }
            last_interaction_time = millis();
            displaySubmenu();
            waitButtonReleased(BTN_UP);
        }

        if (isButtonPressed(BTN_DOWN)) {
            int row = current_submenu_index / OTHER_GRID_COLS;
            if (row < og_rows - 1) {
                current_submenu_index += OTHER_GRID_COLS;
            } else {
                current_submenu_index -= OTHER_GRID_COLS * (og_rows - 1);
            }
            last_interaction_time = millis();
            displaySubmenu();
            waitButtonReleased(BTN_DOWN);
        }

        if (isButtonPressed(BTN_LEFT)) {
            int col = current_submenu_index % OTHER_GRID_COLS;
            if (col > 0) {
                current_submenu_index--;
            } else {
                current_submenu_index++;
            }
            last_interaction_time = millis();
            displaySubmenu();
            waitButtonReleased(BTN_LEFT);
        }

        if (isButtonPressed(BTN_RIGHT)) {
            int col = current_submenu_index % OTHER_GRID_COLS;
            if (col < OTHER_GRID_COLS - 1) {
                current_submenu_index++;
            } else {
                current_submenu_index--;
            }
            last_interaction_time = millis();
            displaySubmenu();
            waitButtonReleased(BTN_RIGHT);
        }
    } else {
        if (isButtonPressed(BTN_UP)) {
            if (!pagedSubmenuEdgeFlip(/*goingDown=*/false)) {
                current_submenu_index =
                    (current_submenu_index - 1 + active_submenu_size) % active_submenu_size;
            }
            last_interaction_time = millis();
            displaySubmenu();
            waitButtonReleased(BTN_UP);
        }

        if (isButtonPressed(BTN_DOWN)) {
            if (!pagedSubmenuEdgeFlip(/*goingDown=*/true)) {
                current_submenu_index = (current_submenu_index + 1) % active_submenu_size;
            }
            last_interaction_time = millis();
            displaySubmenu();
            waitButtonReleased(BTN_DOWN);
        }

        // RIGHT on the selected item: opens a full-screen info page, in the
        // language chosen under Settings > Language (not shown on each
        // list's last item, "Back to Main Menu").
        if (isButtonPressed(BTN_RIGHT)) {
            waitButtonReleased(BTN_RIGHT);
            const char* infoTitle = nullptr;
            const InfoText* infoRef = nullptr;
            if (other_layer == OTHER_LAYER_IR && current_submenu_index < ir_NUM_SUBMENU_ITEMS - 1) {
                infoTitle = t(ir_submenu_items[current_submenu_index]);
                infoRef = &ir_info[current_submenu_index];
            } else if (other_layer == OTHER_LAYER_RFID && current_submenu_index < rfid_NUM_SUBMENU_ITEMS - 1) {
                infoTitle = t(rfid_submenu_items[current_submenu_index]);
                infoRef = &rfid_info[current_submenu_index];
            } else if (other_layer == OTHER_LAYER_GPS && current_submenu_index < gps_NUM_SUBMENU_ITEMS - 1) {
                infoTitle = t(gps_submenu_items[current_submenu_index]);
                infoRef = &gps_info[current_submenu_index];
            }
            if (infoTitle && infoRef) {
                showFeatureInfoScreen(infoTitle, *infoRef);
                submenu_initialized = false;
                last_submenu_index = -1;
                displaySubmenu();
            }
            return;
        }

        // Botao fisico "<" nos submenus (IR, RFID/NFC, GPS) volta para "More".
        if (isButtonPressed(BTN_LEFT)) {
            waitButtonReleased(BTN_LEFT);  // espera soltar de verdade (evita reler o mesmo toque)
            other_layer = OTHER_LAYER_HOME;
            other_menu_grid_initialized = false;
            last_other_menu_index = -1;
            current_submenu_index = other_home_selected_tile;
            feature_active = false;
            feature_exit_requested = false;
            updateActiveSubmenu();
            submenu_initialized = false;
            last_interaction_time = millis();
            displaySubmenu();
            is_main_menu = false;
            delay(200);
            return;
        }
    }

    if (isButtonPressed(BTN_SELECT)) {
        last_interaction_time = millis();
        waitButtonReleased(BTN_SELECT);

        if (other_layer == OTHER_LAYER_HOME) {
            if (current_submenu_index == other_NUM_SUBMENU_ITEMS - 1) {
                in_sub_menu = false;
                feature_active = false;
                feature_exit_requested = false;
                displayMenu();
                handleButtons();
                is_main_menu = false;
            } else if (current_submenu_index == 0) {
                other_home_selected_tile = 0;
                other_layer = OTHER_LAYER_IR;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = 0;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
            } else if (current_submenu_index == 1) {
                other_home_selected_tile = 1;
                other_layer = OTHER_LAYER_RFID;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = 0;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
            } else if (current_submenu_index == 2) {
                other_home_selected_tile = 2;
                other_layer = OTHER_LAYER_GPS;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = 0;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
            }
        } else if (other_layer == OTHER_LAYER_IR) {
            if (current_submenu_index == ir_NUM_SUBMENU_ITEMS - 1) {
                other_layer = OTHER_LAYER_HOME;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = other_home_selected_tile;
                feature_active = false;
                feature_exit_requested = false;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
                is_main_menu = false;
            } else if (current_submenu_index == 0) {
                current_submenu_index = 0;
                in_sub_menu = true;
                feature_active = true;
                feature_exit_requested = false;
                IRRemoteFeature::setup();
                while (current_submenu_index == 0 && !feature_exit_requested) {
                    current_submenu_index = 0;
                    in_sub_menu = true;
                    IRRemoteFeature::loop();
                }
                if (feature_exit_requested) {
                    waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                    in_sub_menu = true;
                    is_main_menu = false;
                    submenu_initialized = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displaySubmenu();
                    delay(200);
                }
            } else if (current_submenu_index == 1) {
                current_submenu_index = 1;
                in_sub_menu = true;
                feature_active = true;
                feature_exit_requested = false;
                IRSavedProfile::setup();
                while (current_submenu_index == 1 && !feature_exit_requested) {
                    current_submenu_index = 1;
                    in_sub_menu = true;
                    IRSavedProfile::loop();
                }
                if (feature_exit_requested) {
                    waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                    in_sub_menu = true;
                    is_main_menu = false;
                    submenu_initialized = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displaySubmenu();
                    delay(200);
                }
            } else if (current_submenu_index == 2) {
                current_submenu_index = 2;
                in_sub_menu = true;
                feature_active = true;
                feature_exit_requested = false;
                IRUniversalController::setup();
                while (current_submenu_index == 2 && !feature_exit_requested) {
                    current_submenu_index = 2;
                    in_sub_menu = true;
                    IRUniversalController::loop();
                }
                if (feature_exit_requested) {
                    waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                    in_sub_menu = true;
                    is_main_menu = false;
                    submenu_initialized = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displaySubmenu();
                    delay(200);
                }
            } else if (current_submenu_index == 3) {
                current_submenu_index = 3;
                in_sub_menu = true;
                feature_active = true;
                feature_exit_requested = false;
                IRUniversalAC::setup();
                while (current_submenu_index == 3 && !feature_exit_requested) {
                    current_submenu_index = 3;
                    in_sub_menu = true;
                    IRUniversalAC::loop();
                }
                if (feature_exit_requested) {
                    waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                    in_sub_menu = true;
                    is_main_menu = false;
                    submenu_initialized = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displaySubmenu();
                    delay(200);
                }
            }
        } else if (other_layer == OTHER_LAYER_RFID) {
            if (current_submenu_index == rfid_NUM_SUBMENU_ITEMS - 1) {
                other_layer = OTHER_LAYER_HOME;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = other_home_selected_tile;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
                is_main_menu = false;
            } else {
                otherRfidPlaceholderAction(current_submenu_index);
            }
        } else if (other_layer == OTHER_LAYER_GPS) {
            if (current_submenu_index == gps_NUM_SUBMENU_ITEMS - 1) {
                other_layer = OTHER_LAYER_HOME;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = other_home_selected_tile;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
                is_main_menu = false;
            } else {
                otherGpsPlaceholderAction(current_submenu_index);
            }
        }
    }

    if (!feature_active) {
        int x, y;
        if (!readTouchXY(x, y)) { return; }
        delay(10);

        int touched_slot = -1;
        if (other_layer == OTHER_LAYER_HOME) {
            for (int i = 0; i < other_NUM_SUBMENU_ITEMS; i++) {
                int column = i % OTHER_GRID_COLS;
                int row = i / OTHER_GRID_COLS;
                int x_position = (column == 0) ? X_OFFSET_LEFT : X_OFFSET_RIGHT;
                int y_position = Y_START + row * Y_SPACING;
                int button_x1 = x_position;
                int button_y1 = y_position;
                int button_x2 = x_position + 100;
                int button_y2 = y_position + 60;
                if (x >= button_x1 && x <= button_x2 && y >= button_y1 && y <= button_y2) {
                    touched_slot = i;
                    break;
                }
            }
        } else {
            for (int i = 0; i < active_submenu_size; i++) {
                int yPos = submenuItemY(i);

                int button_x1 = 10;
                int button_y1 = yPos;
                int button_x2 = 220;
                int button_y2 = yPos + 28;

                if (x >= button_x1 && x <= button_x2 && y >= button_y1 && y <= button_y2) {
                    touched_slot = i;
                    break;
                }
            }
        }

        if (touched_slot < 0) {
            return;
        }

        current_submenu_index = touched_slot;
        last_interaction_time = millis();
        displaySubmenu();
        delay(200);

        if (other_layer == OTHER_LAYER_HOME) {
            if (current_submenu_index == other_NUM_SUBMENU_ITEMS - 1) {
                in_sub_menu = false;
                feature_active = false;
                feature_exit_requested = false;
                displayMenu();
                handleButtons();
                is_main_menu = false;
            } else if (current_submenu_index == 0) {
                other_home_selected_tile = 0;
                other_layer = OTHER_LAYER_IR;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = 0;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
            } else if (current_submenu_index == 1) {
                other_home_selected_tile = 1;
                other_layer = OTHER_LAYER_RFID;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = 0;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
            } else if (current_submenu_index == 2) {
                other_home_selected_tile = 2;
                other_layer = OTHER_LAYER_GPS;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = 0;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
            }
        } else if (other_layer == OTHER_LAYER_IR) {
            if (current_submenu_index == ir_NUM_SUBMENU_ITEMS - 1) {
                other_layer = OTHER_LAYER_HOME;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = other_home_selected_tile;
                feature_active = false;
                feature_exit_requested = false;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
                is_main_menu = false;
            } else if (current_submenu_index == 0) {
                current_submenu_index = 0;
                in_sub_menu = true;
                feature_active = true;
                feature_exit_requested = false;
                IRRemoteFeature::setup();
                while (current_submenu_index == 0 && !feature_exit_requested) {
                    current_submenu_index = 0;
                    in_sub_menu = true;
                    IRRemoteFeature::loop();
                }
                if (feature_exit_requested) {
                    waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                    in_sub_menu = true;
                    is_main_menu = false;
                    submenu_initialized = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displaySubmenu();
                    delay(200);
                }
            } else if (current_submenu_index == 1) {
                current_submenu_index = 1;
                in_sub_menu = true;
                feature_active = true;
                feature_exit_requested = false;
                IRSavedProfile::setup();
                while (current_submenu_index == 1 && !feature_exit_requested) {
                    current_submenu_index = 1;
                    in_sub_menu = true;
                    IRSavedProfile::loop();
                }
                if (feature_exit_requested) {
                    waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                    in_sub_menu = true;
                    is_main_menu = false;
                    submenu_initialized = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displaySubmenu();
                    delay(200);
                }
            } else if (current_submenu_index == 2) {
                current_submenu_index = 2;
                in_sub_menu = true;
                feature_active = true;
                feature_exit_requested = false;
                IRUniversalController::setup();
                while (current_submenu_index == 2 && !feature_exit_requested) {
                    current_submenu_index = 2;
                    in_sub_menu = true;
                    IRUniversalController::loop();
                }
                if (feature_exit_requested) {
                    waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                    in_sub_menu = true;
                    is_main_menu = false;
                    submenu_initialized = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displaySubmenu();
                    delay(200);
                }
            } else if (current_submenu_index == 3) {
                current_submenu_index = 3;
                in_sub_menu = true;
                feature_active = true;
                feature_exit_requested = false;
                IRUniversalAC::setup();
                while (current_submenu_index == 3 && !feature_exit_requested) {
                    current_submenu_index = 3;
                    in_sub_menu = true;
                    IRUniversalAC::loop();
                }
                if (feature_exit_requested) {
                    waitButtonReleased(BTN_LEFT);  // avoid re-reading the same LEFT press as "back" again one level up
                    in_sub_menu = true;
                    is_main_menu = false;
                    submenu_initialized = false;
                    feature_active = false;
                    feature_exit_requested = false;
                    displaySubmenu();
                    delay(200);
                }
            }
        } else if (other_layer == OTHER_LAYER_RFID) {
            if (current_submenu_index == rfid_NUM_SUBMENU_ITEMS - 1) {
                other_layer = OTHER_LAYER_HOME;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = other_home_selected_tile;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
                is_main_menu = false;
            } else {
                otherRfidPlaceholderAction(current_submenu_index);
            }
        } else if (other_layer == OTHER_LAYER_GPS) {
            if (current_submenu_index == gps_NUM_SUBMENU_ITEMS - 1) {
                other_layer = OTHER_LAYER_HOME;
                other_menu_grid_initialized = false;
                last_other_menu_index = -1;
                current_submenu_index = other_home_selected_tile;
                updateActiveSubmenu();
                submenu_initialized = false;
                displaySubmenu();
                is_main_menu = false;
            } else {
                otherGpsPlaceholderAction(current_submenu_index);
            }
        }
    }
}

void handleAboutPage() {
  feature_active = true;
  feature_exit_requested = false;

  tft.fillScreen(UI_BG);
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  // Whole content block (title through hardware rows) shifts up by this
  // amount to keep the last hardware row clear of the fixed footer at y=300.
  const int yOff = -18;

  tft.setTextFont(2);
  tft.setTextColor(UI_ICON, UI_BG);
  tft.setCursor(16, 40 + yOff);
  tftPrintObf(OBF_PN, sizeof(OBF_PN));

  tft.setTextFont(1);
  tft.setTextColor(UI_DIM_TEXT, UI_BG);
  tft.setCursor(16, 60 + yOff);
  tft.print(t(STR_ABOUT_BY));
  tftPrintObf(OBF_DN, sizeof(OBF_DN));
  tft.print(" - ");
  tft.print(ESP32DIV_VERSION);

  tft.drawFastHLine(12, 78 + yOff, 216, UI_LINE);

  const int xLabel = 16;
  const int xValue = 80;
  int y = 96 + yOff;
  const int step = 22;

  tft.setTextColor(UI_DIM_TEXT, UI_BG);
  tft.setCursor(xLabel, y);
  tft.print(t(STR_ABOUT_BOARD));
  tft.setTextColor(UI_TEXT, UI_BG);
  tft.setCursor(xValue, y);
  tft.print(ESP32DIV_BOARD_NAME);
  y += step;

  tft.setTextColor(UI_DIM_TEXT, UI_BG);
  tft.setCursor(xLabel, y);
  tft.print("Mail");
  tft.setTextColor(UI_TEXT, UI_BG);
  tft.setCursor(xValue, y);
  tftPrintObf(OBF_EM, sizeof(OBF_EM));
  y += step;

  tft.setTextColor(UI_DIM_TEXT, UI_BG);
  tft.setCursor(xLabel, y);
  tft.print("GitHub");
  tft.setTextColor(UI_TEXT, UI_BG);
  tft.setCursor(xValue, y);
  tftPrintObf(OBF_GH, sizeof(OBF_GH));
  y += step;

  tft.setTextColor(UI_DIM_TEXT, UI_BG);
  tft.setCursor(xLabel, y);
  tft.print("Web");
  tft.setTextColor(UI_TEXT, UI_BG);
  tft.setCursor(xValue, y);
  tftPrintObf(OBF_WB, sizeof(OBF_WB));

  // ---- HARDWARE section (grouped by status, from boot-time detection) ----
  // Slot modules fall into Installed (detected) or Supported (absent); the
  // SoC/board items are fixed. The ':' column position is computed from the
  // actual widest translated label (not a hardcoded char count) because
  // "Unsupported" isn't the longest in every language -- PT-BR's "Nao
  // suportado" runs longer, for instance.
  {
    String installed, supported;
    auto add = [](String& s, const char* name) {
      if (s.length()) s += ", ";
      s += name;
    };
    g_hwPresence.cc1101 ? add(installed, "Sub-GHz")  : add(supported, "Sub-GHz");
    g_hwPresence.nrf24  ? add(installed, "2.4GHz")   : add(supported, "2.4GHz");
    g_hwPresence.gps    ? add(installed, "GPS")      : add(supported, "GPS");
    g_hwPresence.pn532  ? add(installed, "NFC/RFID") : add(supported, "NFC/RFID");
    if (!installed.length()) installed = "-";
    if (!supported.length()) supported = "-";

    tft.drawFastHLine(12, 186 + yOff, 216, UI_LINE);

    tft.setTextColor(UI_ICON, UI_BG);
    tft.setCursor(16, 194 + yOff);
    tft.print("HARDWARE");

    const char* lblBuiltin    = t(STR_ABOUT_BUILTIN);
    const char* lblInstalled  = t(STR_ABOUT_INSTALLED);
    const char* lblSupported  = t(STR_ABOUT_SUPPORTED);
    const char* lblUnsupported = t(STR_ABOUT_UNSUPPORTED);

    const int hwLabelX = 16;
    int widest = tft.textWidth(lblBuiltin);
    widest = max(widest, (int)tft.textWidth(lblInstalled));
    widest = max(widest, (int)tft.textWidth(lblSupported));
    widest = max(widest, (int)tft.textWidth(lblUnsupported));
    const int hwColonX = hwLabelX + widest + 2;
    const int hwValueX = hwColonX + 8;
    int hy = 214 + yOff;
    const int hstep = 18;  // 2px tighter than the original 20, to help fit a wrapped line

    // Hanging indent: when a value is too wide for the line (e.g. PT-BR's
    // "Nao suportado" leaves less room than "Unsupported" did, which used to
    // push "SD" off the end of the Built-in row), continuation lines start
    // at hwValueX -- right after the ":" -- instead of TFT_eSPI's default
    // auto-wrap, which would wrap to x=0 and crowd the next row.
    const int valueMaxW = (tft.width() - 12) - hwValueX;
    auto row = [&](const char* label, const String& value) {
      tft.setTextColor(UI_DIM_TEXT, UI_BG);
      tft.setCursor(hwLabelX, hy);
      tft.print(label);
      tft.setCursor(hwColonX, hy);
      tft.print(":");
      tft.setTextColor(UI_TEXT, UI_BG);

      String remaining = value;
      remaining.trim();
      do {
        int fitLen = remaining.length();
        while (fitLen > 0 && tft.textWidth(remaining.substring(0, fitLen)) > valueMaxW) {
          fitLen--;
        }
        if (fitLen <= 0) fitLen = min((int)remaining.length(), 1);  // avoid looping forever on a single over-wide char
        if (fitLen < (int)remaining.length()) {
          int lastSpace = remaining.substring(0, fitLen).lastIndexOf(' ');
          if (lastSpace > 0) fitLen = lastSpace;
        }
        tft.setCursor(hwValueX, hy);
        tft.print(remaining.substring(0, fitLen));
        remaining = remaining.substring(fitLen);
        remaining.trim();
        hy += hstep;
      } while (remaining.length() > 0);
    };

    row(lblBuiltin,     "WiFi 2.4GHz, BLE, IR, SD");
    row(lblInstalled,   installed);
    row(lblSupported,   supported);
    row(lblUnsupported, "WiFi 5GHz");
  }

  tft.setTextColor(UI_DIM_TEXT, UI_BG);
  tft.setCursor(16, 300);
  tft.print(t(STR_ABOUT_TAP_TO_GO_BACK));

  while (!feature_exit_requested) {
    if (isButtonPressed(BTN_SELECT) || isButtonPressed(BTN_LEFT)) {
      last_interaction_time = millis();
      feature_exit_requested = true;
      { uint32_t t = millis(); while ((uint32_t)(millis() - t) < BTN_ACTION_RELEASE_MS) { if (isButtonPressed(BTN_SELECT) || isButtonPressed(BTN_LEFT)) t = millis(); delay(5); } }
      break;
    }

    int x, ty;
    if (readTouchXY(x, ty)) {
      last_interaction_time = millis();
      feature_exit_requested = true;
      delay(200);
      break;
    }

    delay(20);
  }

  feature_active = false;
  feature_exit_requested = false;
  in_sub_menu = false;
  submenu_initialized = false;

  menu_initialized = false;
  last_menu_index = -1;
  is_main_menu = false;
  displayMenu();
}
void handleSettingsSubmenuButtons() {

  feature_active = true;
  feature_exit_requested = false;

  AppSettingsUI::setup();
  while (!feature_exit_requested) {
    AppSettingsUI::loop();
  }

  feature_active = false;
  feature_exit_requested = false;

  in_sub_menu = false;
  submenu_initialized = false;

  menu_initialized = false;
  last_menu_index = -1;
  is_main_menu = false;
  displayMenu();
}

void handleButtons() {
    if (in_sub_menu) {
        switch (current_menu_index) {

            case 0: handleWiFiSubmenuButtons(); break;
            case 1: handleNRFSubmenuButtons(); break;
            case 2: handleOtherSubmenuButtons(); break;
            case 3: /* Settings: full-screen AppSettings, not list submenu */ break;
            case 4: handleBluetoothSubmenuButtons(); break;
            case 5: handleSubGHzSubmenuButtons(); break;
            case 6: handleToolsSubmenuButtons(); break;
            default: break;
        }
    } else {

        if (isButtonPressed(BTN_UP) && !is_main_menu) {
            current_menu_index--;
            if (current_menu_index < 0) {
                current_menu_index = NUM_MENU_ITEMS - 1;
            }
            last_interaction_time = millis();
            displayMenu();
            waitButtonReleased(BTN_UP);
        }

        if (isButtonPressed(BTN_DOWN) && !is_main_menu) {
            current_menu_index++;
            if (current_menu_index >= NUM_MENU_ITEMS) {
                current_menu_index = 0;
            }
            last_interaction_time = millis();
            displayMenu();
            waitButtonReleased(BTN_DOWN);
        }

        // Grade de 2 colunas x 4 linhas (col = indice/4, linha = indice%4).
        // LEFT/RIGHT andam na ordem de "leitura": mesma linha troca de coluna,
        // na volta da coluna direita passa pra proxima linha (e vice-versa).
        if (isButtonPressed(BTN_LEFT) && !is_main_menu) {
            int row = current_menu_index % 4;
            int col = current_menu_index / 4;
            if (col == 1) {
                current_menu_index = row;                    // mesma linha, coluna esquerda
            } else {
                current_menu_index = ((row - 1 + 4) % 4) + 4; // linha anterior, coluna direita
            }
            last_interaction_time = millis();
            displayMenu();
            waitButtonReleased(BTN_LEFT);
        }

        if (isButtonPressed(BTN_RIGHT) && !is_main_menu) {
            int row = current_menu_index % 4;
            int col = current_menu_index / 4;
            if (col == 0) {
                current_menu_index = row + 4;       // mesma linha, coluna direita
            } else {
                current_menu_index = (row + 1) % 4; // proxima linha, coluna esquerda
            }
            last_interaction_time = millis();
            displayMenu();
            waitButtonReleased(BTN_RIGHT);
        }

        if (isButtonPressed(BTN_SELECT)) {
            last_interaction_time = millis();
            waitButtonReleased(BTN_SELECT);

            if (current_menu_index == 3) {
                handleSettingsSubmenuButtons();
            } else if (current_menu_index == 7) {
                handleAboutPage();
            } else {
                updateActiveSubmenu();

                if (active_submenu_items && active_submenu_size > 0) {
                    current_submenu_index = 0;
                    if (current_menu_index == 2) {
                        other_layer = OTHER_LAYER_HOME;
                        other_menu_grid_initialized = false;
                        last_other_menu_index = -1;
                    }
                    in_sub_menu = true;
                    submenu_initialized = false;
                    displaySubmenu();
                }

                if (is_main_menu) {
                    is_main_menu = false;
                    displayMenu();
                } else {
                    is_main_menu = true;
                }
            }
        }

        static unsigned long lastTouchTime = 0;
        const unsigned long touchFeedbackDelay = 100;

        if (!feature_active && (millis() - lastTouchTime >= touchFeedbackDelay)) {
            int x, y;
            if (!readTouchXY(x, y)) { return; }
            delay(10);
        for (int i = 0; i < NUM_MENU_ITEMS; i++) {
                int column = i / 4;
                int row = i % 4;
                int x_position = (column == 0) ? X_OFFSET_LEFT : X_OFFSET_RIGHT;
                int y_position = Y_START + row * Y_SPACING;

                int button_x1 = x_position;
                int button_y1 = y_position;
                int button_x2 = x_position + 100;
                int button_y2 = y_position + 60;

                if (x >= button_x1 && x <= button_x2 && y >= button_y1 && y <= button_y2) {
                    current_menu_index = i;
                    last_interaction_time = millis();
                    displayMenu();

                    unsigned long startTime = millis();
                    while (isTouchDownDismiss() && (millis() - startTime < touchFeedbackDelay)) {
                        delay(10);
                    }

                    if (isTouchDownDismiss()) {

                        if (current_menu_index == 3) {
                            handleSettingsSubmenuButtons();
                        } else if (current_menu_index == 7) {
                            handleAboutPage();
                        } else {
                            updateActiveSubmenu();

                            if (active_submenu_items && active_submenu_size > 0) {
                                current_submenu_index = 0;
                                if (current_menu_index == 2) {
                                    other_layer = OTHER_LAYER_HOME;
                                    other_menu_grid_initialized = false;
                                    last_other_menu_index = -1;
                                }
                                in_sub_menu = true;
                                submenu_initialized = false;
                                displaySubmenu();
                            } else {
                                if (is_main_menu) {
                                    is_main_menu = false;
                                    displayMenu();
                                } else {
                                    is_main_menu = true;
                                }
                            }
                        }
                    }
                    delay(200);
                    break;
                }
            }
        }
    }
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("[boot] start");

#if !BOARD_HAS_ESP32S3
  // Weak USB / backlight load can brownout classic ESP32 during intro.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

  tft.init();
  tft.setRotation(TFT_ROTATION);

  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(BACKLIGHT_PIN, PWM_CHANNEL);
  setBrightness(80);

  applyThemeToPalette(settings().theme);

  tft.fillScreen(TFT_BLACK);

  loading(100, UI_ICON, 0, 0, 2, true);

  tft.fillScreen(TFT_BLACK);
  displayLogo(TFT_WHITE, 500);

  initSDCard();

#if BOARD_HAS_ESP32S3
  settingsLoad();
#else
  // Avoid SD mount via settingsLoad on v1 (same crash as step 3).
  settingsApplyBoardTouchDefaults();
  Serial.println("[boot] settings defaults (v1, SD deferred)");
#endif
  applyThemeToPalette(settings().theme);
  setBrightness(settings().brightness);

#if HAS_PCF8574_BUTTONS
  if (!initPcf8574Buttons()) {
    Serial.println("PCF8574 buttons unavailable");
  }
#else
  Serial.println("PCF8574 buttons disabled for this board");
#endif

#if BOARD_HAS_ESP32S3
  ensureBleStackReady();
#else
  // Classic ESP32: defer NimBLE; also skip boot-time WiFi scan task (heap/WDT).
  Serial.println("[boot] BLE/WiFi-bg deferred (v1)");
#endif

#if FEATURE_BLE_DUCKY
  Ducky::setup();
#endif

  // Probe optional radios/modules once, before the scanners start and before
  // the touchscreen setup (several share GPIO5 / the SPI buses). For the About
  // screen's HARDWARE section.
  hwDetectAll();

#if BOARD_HAS_ESP32S3
  WifiScan::startBackgroundScanner();
  BleScan::startBackgroundScanner();
  startStatusBarTask();
#else
  // Keep boot lightweight on ESP32 — status bar updates from loop() instead.
#endif

  menu_initialized = false;
  currentBatteryVoltage = readBatteryVoltage();
  displayMenu();
  drawStatusBar(currentBatteryVoltage, false);

  setupTouchscreen();

  last_interaction_time = millis();
  Serial.println("[boot] ready");
}

void loop() {
  applyThemeToPalette(settings().theme);
  handleButtons();
  updateStatusBar();
}
