#include <Arduino.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_LG.h>
#include <IRutils.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <algorithm>
#include <vector>
#include "KeyboardUI.h"
#include "Touchscreen.h"
#include "icon.h"
#include "shared.h"
#include "utils.h"

static constexpr int kIrScreenH = 320;

static int irContentBottom() {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : kIrScreenH;
}

static void irClearBody(uint16_t color = FEATURE_BG) {
  if (featureHasTouchNavBar()) {
    featureClearContent(color);
  } else {
    tft.fillScreen(color);
  }
}

static void irClearContentFrom(int topY, uint16_t color = FEATURE_BG) {
  const int bottom = irContentBottom();
  if (bottom > topY) {
    tft.fillRect(0, topY, 240, bottom - topY, color);
  }
}

static void irRedrawNavChrome() {
  redrawTouchButtonBar();
  maintainTouchNavBar();
}

static void irSetCaptureNavLabels() {
  setTouchNavLabels("Rep-", "Save", "Exit", "Send", "Rep+");
}

static void irSetSavedNavLabels() {
  setTouchNavLabels("Delete", "Next", "Exit", "Prev", "TX");
}

static void irSetUniversalRemoteNavLabels() {
  setTouchNavLabels("Prev", "Browse", "Exit", "Reload", "Next");
}

static void irSetUniversalBrowseNavLabels() {
  setTouchNavLabels("Back", "Next", "Exit", "Prev", "Select");
}

// Navegacao por linha/coluna (dir: 0=cima,1=baixo,2=esq,3=dir). Pula desabilitados.
// Cima/baixo: vai para a linha mais proxima na vertical (coluna so desempata).
// Esq/dir: anda apenas dentro da mesma linha (|dy| <= altura do botao).
static int irNavSelect(const FeatureUI::Button* b, int n, int cur, int dir) {
  if (n <= 0) return cur;
  if (cur < 0 || cur >= n) cur = 0;
  const int cx = b[cur].x + b[cur].w / 2;
  const int cy = b[cur].y + b[cur].h / 2;
  const long rowTol = b[cur].h > 16 ? b[cur].h : 16;   // tolerancia de "mesma linha"
  int best = -1; long bestCost = 2147483647L;
  int bestAny = -1; long bestAnyCost = 2147483647L;    // fallback esq/dir (qualquer linha)
  for (int i = 0; i < n; i++) {
    if (i == cur) continue;
    if (b[i].w <= 0 || b[i].h <= 0 || b[i].disabled) continue;
    const int dx = (b[i].x + b[i].w / 2) - cx;
    const int dy = (b[i].y + b[i].h / 2) - cy;
    const long adx = dx < 0 ? -dx : dx;
    const long ady = dy < 0 ? -dy : dy;
    bool ok = false; long cost = 0;
    switch (dir) {
      case 0: ok = dy < -2; cost = ady * 1000 + adx; break; // cima: linha mais proxima
      case 1: ok = dy >  2; cost = ady * 1000 + adx; break; // baixo
      case 2: // esq: mesma linha; senao, mais proximo a esquerda
        if (dx < -2) {
          const long any = adx * 1000 + ady;
          if (any < bestAnyCost) { bestAnyCost = any; bestAny = i; }
          if (ady <= rowTol) { ok = true; cost = any; }
        }
        break;
      case 3: // dir: mesma linha; senao, mais proximo a direita
        if (dx > 2) {
          const long any = adx * 1000 + ady;
          if (any < bestAnyCost) { bestAnyCost = any; bestAny = i; }
          if (ady <= rowTol) { ok = true; cost = any; }
        }
        break;
    }
    if (!ok) continue;
    if (cost < bestCost) { bestCost = cost; best = i; }
  }
  if (best >= 0) return best;
  if ((dir == 2 || dir == 3) && bestAny >= 0) return bestAny;  // fallback
  return cur;
}

// Verdadeiro se nenhum botao (habilitado) tem centro a esquerda do atual.
static bool irIsLeftEdge(const FeatureUI::Button* b, int n, int cur) {
  if (cur < 0 || cur >= n) return true;
  const int cx = b[cur].x + b[cur].w / 2;
  for (int i = 0; i < n; i++) {
    if (i == cur) continue;
    if (b[i].w <= 0 || b[i].h <= 0 || b[i].disabled) continue;
    if ((b[i].x + b[i].w / 2) < cx - 2) return false;
  }
  return true;
}



namespace IRRemoteFeature {

static constexpr uint16_t kRecvPin = IR_RX_PIN;
static constexpr uint16_t kSendPin = IR_TX_PIN;
static constexpr uint16_t kKhz     = IR_DEFAULT_KHZ;

static constexpr int16_t kToolbarY = 20;
static constexpr int16_t kToolbarH = 16;
static constexpr int16_t kIconSize = 16;

static constexpr int kIrToolbarBottom = 36;
static constexpr int kIrToolbarGap = 8;
static constexpr int kIrBoxHeaderH = 15;
static constexpr int kIrStatusY = kIrToolbarBottom + kIrToolbarGap;
static constexpr int kIrStatusBoxH = 91;
static constexpr int kIrLogGap = 4;
static constexpr int kIrLogBoxH = 49;
static constexpr int kIrLogBoxTop = kIrStatusY + kIrStatusBoxH + kIrLogGap;
static constexpr int kIrLogStartY = kIrLogBoxTop + kIrBoxHeaderH;
static constexpr int kIrLogEndY = kIrLogBoxTop + kIrLogBoxH - 2;
static constexpr int kIrGraphTop = kIrLogBoxTop + kIrLogBoxH + 4;
static constexpr int kIrGraphMarginX = 6;
static constexpr int kIrLineHeight = 12;
static constexpr int kIrStatusLineCount = 6;
static constexpr int kIrStatusTextY = kIrStatusY + kIrBoxHeaderH;
static constexpr int kIrMaxLogLines = 48;
static constexpr uint16_t kIrPlotBg = 0x0842;
static constexpr uint16_t kIrGridColor = 0x2945;
static constexpr int kIrGridDivisions = 4;

struct IrPlotLayout {
  int graphTop = 0;
  int axisX = 0;
  int plotRight = 0;
  int plotTop = 0;
  int plotBottom = 0;
  int plotHeight = 0;
  int plotWidth = 0;
  bool valid = false;
};

static IrPlotLayout s_irPlot;
static bool s_irBoxesDrawn = false;
static bool s_irGraphChromeDrawn = false;
static String s_irLogBuffer[kIrMaxLogLines];
static uint16_t s_irLogColor[kIrMaxLogLines];
static int s_irLogIndex = 0;
static String s_irStatusLineText[kIrStatusLineCount];
static uint16_t s_irStatusLineColor[kIrStatusLineCount];
static bool s_irStatusStaticDrawn = false;
static uint32_t s_irLastStatusDrawMs = 0;

static constexpr uint16_t kMaxRawLen = 512;

static IRrecv s_recv(kRecvPin);
static IRsend s_send(kSendPin);
static decode_results s_results;

static bool s_hasCapture = false;

static uint16_t s_raw[kMaxRawLen];
static uint16_t s_rawLen = 0;

static decode_type_t s_decodeType = decode_type_t::UNKNOWN;
static uint64_t s_value = 0;
static uint16_t s_bits = 0;

static uint8_t s_repeat = 1;
static bool s_autoTx = false;
static uint32_t s_autoIntervalMs = 450;
static uint32_t s_lastAutoMs = 0;

static bool s_uiDrawn = false;
static bool s_contentDirty = true;
static uint32_t s_lastActionMs = 0;
static bool s_captureUiDirty = false;
static uint32_t s_lastCaptureUiMs = 0;
static uint32_t s_lastWaveHash = 0;
static uint32_t s_lastStableHash = 0;

static bool s_hasParsedKey = false;
static uint64_t s_keyAddr = 0;
static uint64_t s_keyCmd  = 0;
static String s_keyText   = "";

static uint8_t s_waveZoomIdx = 0;
static uint32_t s_wavePanUs = 0;
static constexpr uint32_t kWaveZoomUs[] = {0, 100000, 50000, 20000, 10000};
static constexpr uint8_t kWaveZoomCount = 5;

static uint8_t  s_scope[240]{};
static uint16_t s_scopePos = 0;
static uint32_t s_lastScopeSampleUs = 0;
static uint32_t s_lastScopeDrawMs = 0;

static uint32_t waveformWindowUs(uint32_t totalUs) {
  const uint32_t w = kWaveZoomUs[s_waveZoomIdx % kWaveZoomCount];
  if (w == 0) return totalUs;
  return (totalUs < w) ? totalUs : w;
}

static const char* zoomLabel() {
  switch (s_waveZoomIdx % kWaveZoomCount) {
    case 0: return "FIT";
    case 1: return "100ms";
    case 2: return "50ms";
    case 3: return "20ms";
    default: return "10ms";
  }
}

static uint32_t irWaveTotalUs() {
  uint32_t total = 0;
  for (uint16_t i = 0; i < s_rawLen; i++) {
    total += s_raw[i];
  }
  return total;
}

static void irWaveClampPan() {
  const uint32_t total = irWaveTotalUs();
  const uint32_t window = waveformWindowUs(total);
  if (window == 0 || window >= total) {
    s_wavePanUs = 0;
    return;
  }
  const uint32_t maxPan = total - window;
  if (s_wavePanUs > maxPan) {
    s_wavePanUs = maxPan;
  }
}

static String irGraphTitle() {
  if (!s_hasCapture) {
    return "Live IR Scope";
  }
  String title = "Waveform " + String(zoomLabel());
  if (s_waveZoomIdx != 0 && s_wavePanUs > 0) {
    title += " @+" + String(s_wavePanUs / 1000) + "ms";
  }
  return title;
}

static void irDrawGraphChrome();
static void redrawWaveformPlotOnly();
static void irPrint(const String& text, uint16_t color, bool extraSpace);
static void irEnsurePlotLayout();
static void redrawGraphTitleOnly(bool captured);

static void irGraphRefreshWaveform() {
  if (!s_irPlot.valid) {
    irEnsurePlotLayout();
  }
  redrawGraphTitleOnly(true);
  redrawWaveformPlotOnly();
}

static void irWaveZoomIn() {
  if (!s_hasCapture) {
    return;
  }
  if (s_waveZoomIdx + 1 < kWaveZoomCount) {
    s_waveZoomIdx++;
  }
  irWaveClampPan();
  irGraphRefreshWaveform();
}

static void irWaveZoomOut() {
  if (!s_hasCapture) {
    return;
  }
  if (s_waveZoomIdx > 0) {
    s_waveZoomIdx--;
  }
  if (s_waveZoomIdx == 0) {
    s_wavePanUs = 0;
  }
  irWaveClampPan();
  irGraphRefreshWaveform();
}

static void irWaveCycleZoom() {
  if (!s_hasCapture) {
    return;
  }
  s_waveZoomIdx = (uint8_t)((s_waveZoomIdx + 1) % kWaveZoomCount);
  if (s_waveZoomIdx == 0) {
    s_wavePanUs = 0;
  }
  irWaveClampPan();
  irGraphRefreshWaveform();
}

static void irWavePanBy(int32_t deltaUs) {
  if (!s_hasCapture || s_waveZoomIdx == 0) {
    return;
  }
  const uint32_t total = irWaveTotalUs();
  const uint32_t window = waveformWindowUs(total);
  if (window == 0 || window >= total) {
    return;
  }
  const uint32_t maxPan = total - window;
  const int64_t next = (int64_t)s_wavePanUs + deltaUs;
  if (next <= 0) {
    s_wavePanUs = 0;
  } else if ((uint32_t)next >= maxPan) {
    s_wavePanUs = maxPan;
  } else {
    s_wavePanUs = (uint32_t)next;
  }
  irGraphRefreshWaveform();
}

static void irHandleGraphTouch(int x, int y) {
  if (!s_hasCapture || !s_irPlot.valid) {
    return;
  }
  if (x < s_irPlot.axisX || x > s_irPlot.plotRight ||
      y < s_irPlot.plotTop || y > s_irPlot.plotBottom) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_lastActionMs < 220) {
    return;
  }

  const int plotW = s_irPlot.plotRight - s_irPlot.axisX;
  const int relX = x - s_irPlot.axisX;
  const uint32_t total = irWaveTotalUs();
  const uint32_t window = waveformWindowUs(total);
  const uint32_t panStep = (window / 4 > 1000U) ? (window / 4) : 1000U;

  if (relX < plotW / 3) {
    irWavePanBy(-(int32_t)panStep);
    irPrint("[*] Graph pan -", UI_DIM_TEXT, false);
  } else if (relX > (plotW * 2) / 3) {
    irWavePanBy((int32_t)panStep);
    irPrint("[*] Graph pan +", UI_DIM_TEXT, false);
  } else {
    irWaveCycleZoom();
    irPrint(String("[*] Zoom ") + zoomLabel(), UI_DIM_TEXT, false);
  }
  s_lastActionMs = now;
}

static void irEnsurePlotLayout() {
  const int screenW = tft.width();
  s_irPlot.graphTop = kIrGraphTop;
  s_irPlot.axisX = kIrGraphMarginX;
  s_irPlot.plotRight = screenW - kIrGraphMarginX;
  s_irPlot.plotTop = s_irPlot.graphTop + 12;
  s_irPlot.plotBottom = irContentBottom() - 13;
  s_irPlot.plotHeight = s_irPlot.plotBottom - s_irPlot.plotTop;
  s_irPlot.plotWidth = s_irPlot.plotRight - s_irPlot.axisX;
  s_irPlot.valid = s_irPlot.plotWidth >= 32 && s_irPlot.plotHeight >= 10;
}

static String irFitStatusText(const String& text) {
  const int maxWidth = tft.width() - 16;
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

static void irScrollLog() {
  for (int i = 0; i < kIrMaxLogLines - 1; i++) {
    s_irLogBuffer[i] = s_irLogBuffer[i + 1];
    s_irLogColor[i] = s_irLogColor[i + 1];
  }
}

static int irLogVisibleLines() {
  const int h = kIrLogEndY - kIrLogStartY;
  if (h <= 0 || kIrLineHeight <= 0) {
    return 1;
  }
  return h / kIrLineHeight;
}

static void irRedrawActivityLog() {
  const int visible = irLogVisibleLines();
  tft.fillRect(8, kIrLogStartY, tft.width() - 16, kIrLogEndY - kIrLogStartY, TFT_BLACK);
  if (visible <= 0 || s_irLogIndex <= 0) {
    return;
  }

  const int start = (s_irLogIndex > visible) ? (s_irLogIndex - visible) : 0;
  for (int row = 0; row < visible; row++) {
    const int bufIndex = start + row;
    if (bufIndex >= s_irLogIndex) {
      break;
    }
    if (s_irLogBuffer[bufIndex].length() == 0) {
      continue;
    }
    const int yPos = kIrLogStartY + row * kIrLineHeight;
    tft.setTextSize(1);
    tft.setTextColor(s_irLogColor[bufIndex], TFT_BLACK);
    tft.setCursor(8, yPos);
    tft.print(s_irLogBuffer[bufIndex]);
  }
}

static void irPrint(const String& text, uint16_t color, bool extraSpace = false) {
  if (s_irLogIndex >= kIrMaxLogLines) {
    irScrollLog();
    s_irLogIndex = kIrMaxLogLines - 1;
  }

  s_irLogBuffer[s_irLogIndex] = text;
  s_irLogColor[s_irLogIndex] = color;
  s_irLogIndex++;

  if (extraSpace && s_irLogIndex < kIrMaxLogLines) {
    s_irLogBuffer[s_irLogIndex] = "";
    s_irLogColor[s_irLogIndex] = TFT_WHITE;
    s_irLogIndex++;
  }

  irRedrawActivityLog();
}

static void irDrawStatusLine(int line, const String& text, uint16_t color) {
  const int y = kIrStatusTextY + line * kIrLineHeight;
  const String fitted = irFitStatusText(text);
  tft.fillRect(8, y, tft.width() - 16, kIrLineHeight, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(8, y);
  tft.print(fitted);
}

static void irDrawStatusLineIfChanged(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kIrStatusLineCount) {
    return;
  }
  if (s_irStatusLineText[line] == text && s_irStatusLineColor[line] == color) {
    return;
  }
  s_irStatusLineText[line] = text;
  s_irStatusLineColor[line] = color;
  irDrawStatusLine(line, text, color);
}

static void irDrawTextBoxes() {
  tft.fillRect(0, kIrStatusY - 2, tft.width(), kIrGraphTop - kIrStatusY + 2, TFT_BLACK);
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawRoundRect(4, kIrStatusY, tft.width() - 8, kIrStatusBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kIrLogBoxTop, tft.width() - 8, kIrLogBoxH, 3, UI_LINE);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.drawString("Signal Status", 8, kIrStatusY + 3);
  tft.drawString("Activity", 8, kIrLogBoxTop + 3);
  s_irBoxesDrawn = true;
}

static void irDrawStaticStatusLines() {
  if (s_irStatusStaticDrawn) {
    return;
  }
  irDrawStatusLineIfChanged(5, "Carrier: " + String(kKhz) + " kHz IR", UI_DIM_TEXT);
  s_irStatusStaticDrawn = true;
}

static void irUpdateGraphHintLine() {
  if (s_hasCapture) {
    irDrawStatusLineIfChanged(5, "Graph: +/- zoom  tap L/R pan", UI_DIM_TEXT);
  } else {
    s_irStatusLineText[5] = "";
    s_irStatusLineColor[5] = 0;
    irDrawStatusLineIfChanged(5, "Carrier: " + String(kKhz) + " kHz IR", UI_DIM_TEXT);
  }
}

static void irUpdateStatusPanel(bool force = false) {
  const uint32_t now = millis();
  if (!force && now - s_irLastStatusDrawMs < 120) {
    return;
  }
  s_irLastStatusDrawMs = now;

  irDrawStaticStatusLines();
  irDrawStatusLineIfChanged(4, "Repeat: " + String((unsigned)s_repeat) +
                            "   Auto: " + String(s_autoTx ? "ON" : "OFF"),
                            s_autoTx ? UI_WARN : UI_DIM_TEXT);

  if (!s_hasCapture) {
    irDrawStatusLineIfChanged(0, "State: Listening", UI_WARN);
    irDrawStatusLineIfChanged(1, "Type: --", UI_DIM_TEXT);
    irDrawStatusLineIfChanged(2, "Key: --", UI_DIM_TEXT);
    irDrawStatusLineIfChanged(3, "Raw: --", UI_DIM_TEXT);
    irUpdateGraphHintLine();
    return;
  }

  irDrawStatusLineIfChanged(0, "State: Captured", UI_OK);
  irDrawStatusLineIfChanged(1, "Type: " + String(typeToString(s_decodeType)), UI_TEXT);
  String keyLine = s_keyText;
  if (keyLine.length() > 28) {
    keyLine = keyLine.substring(0, 28);
  }
  irDrawStatusLineIfChanged(2, "Key: " + keyLine, UI_TEXT);
  irDrawStatusLineIfChanged(3, "Raw: " + String((unsigned)s_rawLen) +
                            " pulses  Bits: " + String((unsigned)s_bits), UI_TEXT);
  irUpdateGraphHintLine();
}

static void irDrawGraphGrid() {
  for (int g = 1; g < kIrGridDivisions; g++) {
    const int gy = s_irPlot.plotTop + ((s_irPlot.plotHeight * g) + (kIrGridDivisions / 2)) / kIrGridDivisions;
    tft.drawFastHLine(s_irPlot.axisX + 1, gy, s_irPlot.plotWidth - 2, kIrGridColor);
  }
  for (int v = 1; v < kIrGridDivisions; v++) {
    const int vx = s_irPlot.axisX + ((s_irPlot.plotWidth * v) + (kIrGridDivisions / 2)) / kIrGridDivisions;
    tft.drawFastVLine(vx, s_irPlot.plotTop + 1, s_irPlot.plotHeight - 2, kIrGridColor);
  }
}

static void irDrawGraphChrome() {
  irEnsurePlotLayout();
  if (!s_irPlot.valid) {
    return;
  }

  const int screenW = tft.width();
  const int graphBottom = irContentBottom() - 2;
  tft.fillRect(0, kIrGraphTop, screenW, graphBottom - kIrGraphTop + 2, TFT_BLACK);
  tft.fillRect(s_irPlot.axisX, s_irPlot.plotTop, s_irPlot.plotWidth, s_irPlot.plotHeight, kIrPlotBg);
  irDrawGraphGrid();
  tft.drawRect(s_irPlot.axisX, s_irPlot.plotTop, s_irPlot.plotWidth, s_irPlot.plotHeight, UI_LINE);

  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  if (s_hasCapture) {
    const String title = irGraphTitle();
    tft.drawString(title, (screenW - tft.textWidth(title)) / 2, s_irPlot.graphTop + 2);
  } else {
    tft.drawString("Live IR Scope", (screenW - 66) / 2, s_irPlot.graphTop + 2);
  }

  s_irGraphChromeDrawn = true;
}

static void irResetUiState() {
  s_irBoxesDrawn = false;
  s_irGraphChromeDrawn = false;
  s_irPlot.valid = false;
  s_irStatusStaticDrawn = false;
  s_irLastStatusDrawMs = 0;
  s_waveZoomIdx = 0;
  s_wavePanUs = 0;
  s_irLogIndex = 0;
  for (int i = 0; i < kIrMaxLogLines; i++) {
    s_irLogBuffer[i] = "";
    s_irLogColor[i] = 0;
  }
  for (int i = 0; i < kIrStatusLineCount; i++) {
    s_irStatusLineText[i] = "";
    s_irStatusLineColor[i] = 0;
  }
}

static void redrawGraphTitleOnly(bool captured) {
  (void)captured;
  if (s_irGraphChromeDrawn) {
    const int screenW = tft.width();
    tft.fillRect(0, s_irPlot.graphTop, screenW, 12, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    if (s_hasCapture) {
      const String title = irGraphTitle();
      tft.drawString(title, (screenW - tft.textWidth(title)) / 2, s_irPlot.graphTop + 2);
    } else {
      tft.drawString("Live IR Scope", (screenW - 66) / 2, s_irPlot.graphTop + 2);
    }
  }
}

static void redrawScopePlotOnly() {
  if (!s_irGraphChromeDrawn) {
    return;
  }
  if (!s_irPlot.valid) {
    irEnsurePlotLayout();
  }
  if (!s_irPlot.valid) {
    return;
  }

  const int px0 = s_irPlot.axisX + 1;
  const int py0 = s_irPlot.plotTop + 1;
  const int pw  = s_irPlot.plotWidth - 2;
  const int ph  = s_irPlot.plotHeight - 2;

  tft.startWrite();
  tft.fillRect(px0, py0, pw, ph, kIrPlotBg);

  const int highY = py0 + 8;
  const int lowY  = py0 + ph - 9;

  for (int x = 0; x < pw && x < 240; x++) {
    uint16_t idx = (uint16_t)((s_scopePos + x) % 240);
    bool levelHigh = (s_scope[idx] != 0);
    int y = levelHigh ? highY : lowY;
    tft.drawPixel(px0 + x, y, levelHigh ? UI_WARN : UI_OK);
    if (x > 0) {
      uint16_t pidx = (uint16_t)((s_scopePos + x - 1) % 240);
      bool prevHigh = (s_scope[pidx] != 0);
      int py = prevHigh ? highY : lowY;
      if (py != y) {
        tft.drawFastVLine(px0 + x, highY, (lowY - highY + 1), kIrGridColor);
      }
    }
  }
  tft.endWrite();
}

static void redrawWaveformPlotOnly() {
  if (!s_irGraphChromeDrawn) {
    return;
  }
  if (!s_irPlot.valid) {
    irEnsurePlotLayout();
  }
  if (!s_irPlot.valid) {
    return;
  }

  const int px0 = s_irPlot.axisX + 1;
  const int py0 = s_irPlot.plotTop + 1;
  const int pw  = s_irPlot.plotWidth - 2;
  const int ph  = s_irPlot.plotHeight - 2;

  const uint16_t colGridMajor = UI_LINE;
  const uint16_t colGridMinor = kIrGridColor;
  const uint16_t colMark      = UI_OK;
  const uint16_t colSpace     = UI_WARN;
  const uint16_t colText      = UI_DIM_TEXT;

  tft.startWrite();
  tft.fillRect(px0, py0, pw, ph, kIrPlotBg);

  const int labelY = py0 + ph - 10;
  const int highY = py0 + 10;
  const int lowY  = py0 + ph - 18;

  const uint32_t totalUs = irWaveTotalUs();
  if (totalUs == 0 || s_rawLen == 0) {
    tft.endWrite();
    return;
  }

  const uint32_t windowUs = waveformWindowUs(totalUs);
  if (windowUs == 0) {
    tft.endWrite();
    return;
  }

  const uint32_t panUs = s_wavePanUs;
  const uint32_t viewEnd = panUs + windowUs;

  auto tickPxFor = [&](uint32_t tickUs) -> int {
    if (tickUs == 0) return 9999;
    return (int)((tickUs * (uint32_t)pw) / windowUs);
  };

  uint32_t tickUs = 2000;
  if (windowUs > 80000) tickUs = 10000;
  else if (windowUs > 30000) tickUs = 5000;
  else if (windowUs > 15000) tickUs = 5000;

  while (tickPxFor(tickUs) < 10 && tickUs < 50000) tickUs *= 2;

  uint32_t majorEvery = 1;
  while (tickPxFor((uint32_t)(tickUs * majorEvery)) < 36 && majorEvery < 16) majorEvery *= 2;

  tft.setTextFont(1);
  tft.setTextColor(colText, kIrPlotBg);
  int lastLabelRight = -10000;

  for (uint32_t t = 0, n = 0; t <= windowUs; t += tickUs, n++) {
    int x = px0 + (int)((t * (uint32_t)pw) / windowUs);
    if (x < px0 || x >= (px0 + pw)) continue;

    const bool isMajor = (n % majorEvery) == 0;
    tft.drawFastVLine(x, py0, ph, (t == 0) ? colGridMajor : (isMajor ? colGridMajor : colGridMinor));

    if (!isMajor) continue;

    uint32_t ms = (t + panUs) / 1000;
    char buf[10];
    snprintf(buf, sizeof(buf), "%lums", (unsigned long)ms);
    int tw = tft.textWidth(buf, 1);
    int lx = x - (tw / 2);
    if (lx < px0) lx = px0;
    if (lx + tw > (px0 + pw)) lx = (px0 + pw) - tw;

    if (lx <= lastLabelRight) continue;
    tft.setCursor(lx, labelY);
    tft.print(buf);
    lastLabelRight = lx + tw + 2;
  }

  bool isMark = true;
  uint32_t tUs = 0;

  for (uint16_t i = 0; i < s_rawLen; i++) {
    const uint32_t segStart = tUs;
    const uint32_t segEnd = tUs + s_raw[i];
    tUs = segEnd;

    if (segEnd <= panUs) {
      isMark = !isMark;
      continue;
    }
    if (segStart >= viewEnd) {
      break;
    }

    const uint32_t clipStart = (segStart < panUs) ? panUs : segStart;
    const uint32_t clipEnd = (segEnd > viewEnd) ? viewEnd : segEnd;

    int x1 = px0 + (int)(((clipStart - panUs) * (uint32_t)pw) / windowUs);
    int x2 = px0 + (int)(((clipEnd - panUs) * (uint32_t)pw) / windowUs);
    if (x1 < px0) x1 = px0;
    if (x2 > px0 + pw) x2 = px0 + pw;

    const int y = isMark ? highY : lowY;
    const uint16_t c = isMark ? colMark : colSpace;

    if (x2 > x1) {
      tft.drawFastHLine(x1, y, x2 - x1, c);
      tft.drawFastHLine(x1, y + (isMark ? 1 : -1), x2 - x1, c);
    } else {
      tft.drawPixel(x1, y, c);
    }

    if (x2 != x1) {
      tft.drawFastVLine(x2, highY, (lowY - highY + 1), colGridMinor);
    }

    isMark = !isMark;
  }
  tft.endWrite();
}

static void redrawCapturedDetailsOnly() {
  irUpdateStatusPanel(true);
  redrawGraphTitleOnly(true);
  redrawWaveformPlotOnly();
}

static const char* kRandomNames[] = {
  "TV", "AC", "Lamp", "Fan", "Receiver", "Projector", "Power", "Mute", "VolUp", "VolDn"
};

static uint32_t hashWaveform() {

  uint32_t h = 2166136261u;
  auto mix = [&](uint32_t v) {
    h ^= v;
    h *= 16777619u;
  };
  mix((uint32_t)s_rawLen);
  mix((uint32_t)s_decodeType);
  mix((uint32_t)s_bits);
  mix((uint32_t)s_value);
  mix((uint32_t)(s_value >> 32));
  const uint16_t n = (s_rawLen < 32) ? s_rawLen : 32;
  for (uint16_t i = 0; i < n; i++) mix((uint32_t)s_raw[i]);
  return h;
}

static void tryReplay();
static void saveCapture();
static void runUI();
static void irRestoreChrome();

static void irRestoreChrome() {
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);

  s_uiDrawn = false;
  s_irBoxesDrawn = false;
  s_irGraphChromeDrawn = false;
  s_irStatusStaticDrawn = false;
  for (int i = 0; i < kIrStatusLineCount; i++) {
    s_irStatusLineText[i] = "";
    s_irStatusLineColor[i] = 0;
  }

  runUI();
  irDrawTextBoxes();
  irEnsurePlotLayout();
  irDrawGraphChrome();
  irUpdateStatusPanel(true);

  irRedrawActivityLog();

  if (s_hasCapture) {
    redrawWaveformPlotOnly();
  } else {
    redrawScopePlotOnly();
  }

  irRedrawNavChrome();
  tft.drawFastHLine(0, kToolbarY, 240, UI_LINE);
  tft.drawFastHLine(0, kToolbarY + kToolbarH, 240, UI_LINE);
}

static String getUserInputName() {
  OnScreenKeyboardConfig cfg;
  cfg.titleLine1      = "[!] Name this IR capture";
  cfg.titleLine2      = "(max 15 chars, ^ caps, # sym)";
  osKeyboardUseStandardLayout(cfg);
  cfg.maxLen          = 15;
  cfg.shuffleNames    = kRandomNames;
  cfg.shuffleCount    = (uint8_t)(sizeof(kRandomNames) / sizeof(kRandomNames[0]));
  cfg.buttonsY        = 195;
  cfg.backLabel       = "Back";
  cfg.middleLabel     = "Shuffle";
  cfg.okLabel         = "OK";
  cfg.enableShuffle   = true;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg   = "Name cannot be empty!";

  OnScreenKeyboardResult r = showOnScreenKeyboard(cfg, "");
  irRestoreChrome();
  if (!r.accepted) {
    return "";
  }
  return r.text;
}

namespace {
  static constexpr const char* IR_DIR = "/ir";
  static constexpr const char* IR_FILE_PREFIX = "/ir/ir_";
  static constexpr uint32_t IR_MAGIC = 0x31525249;

  struct __attribute__((packed)) IrCaptureHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t khz;
    uint16_t rawLen;
    uint8_t  decodeType;
    uint8_t  reserved;
    uint16_t bits;
    uint64_t value;
    char     name[16];
  };

  static bool irEnsureDir(const char* dirPath) {
    if (SD.exists(dirPath)) return true;
    if (SD.mkdir(dirPath)) return true;
    if (dirPath && dirPath[0] == '/') return SD.mkdir(dirPath + 1);
    return false;
  }

  static bool makeNextIrPath(String& outPath) {
    char buf[32];
    for (uint16_t i = 0; i < 10000; i++) {
      snprintf(buf, sizeof(buf), "%s%04u.bin", IR_FILE_PREFIX, (unsigned)i);
      if (!SD.exists(buf)) { outPath = String(buf); return true; }
    }
    return false;
  }
}

static void updateDisplay() {
  if (!s_irBoxesDrawn) {
    irDrawTextBoxes();
    irRedrawActivityLog();
  }
  irUpdateStatusPanel(true);

  if (!s_irGraphChromeDrawn) {
    irDrawGraphChrome();
    if (!s_hasCapture) {
      redrawScopePlotOnly();
    } else {
      redrawWaveformPlotOnly();
    }
  }
}

static void handleTouchNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    s_autoTx = false;
    if (s_repeat > 1) s_repeat--;
    s_contentDirty = true;
    irUpdateStatusPanel(true);
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    s_autoTx = false;
    if (s_repeat < 10) s_repeat++;
    s_contentDirty = true;
    irUpdateStatusPanel(true);
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    s_autoTx = false;
    if (s_hasCapture) tryReplay();
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    if (s_hasCapture) saveCapture();
  }
}

static void runUI() {
  static constexpr int kIconBackX = 10;
  static constexpr int kIconZoomOutX = 130;
  static constexpr int kIconZoomInX = 170;
  static constexpr int kIconAutoX = 210;
  static int iconY = kToolbarY;

  if (!s_uiDrawn) {
    tft.fillRect(0, kToolbarY, 160, kToolbarH, DARK_GRAY);
    tft.setTextColor(UI_TEXT, DARK_GRAY);
    tft.setCursor(35, kToolbarY + 4);
    tft.print("IR Record");

    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.fillRect(160, kToolbarY, 80, kToolbarH, DARK_GRAY);

    tft.drawBitmap(kIconBackX, iconY, bitmap_icon_go_back, kIconSize, kIconSize, UI_ICON);
    if (s_hasCapture) {
      tft.drawBitmap(kIconZoomOutX, iconY, bitmap_icon_sort_down_minus, kIconSize, kIconSize, UI_ICON);
      tft.drawBitmap(kIconZoomInX, iconY, bitmap_icon_sort_up_plus, kIconSize, kIconSize, UI_ICON);
    }
    tft.drawBitmap(kIconAutoX, iconY, bitmap_icon_random, kIconSize, kIconSize, UI_ICON);

    tft.drawFastHLine(0, kToolbarY + kToolbarH, 240, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      switch (activeIcon) {
        case 0:
          irWaveZoomOut();
          irPrint("[*] Zoom " + String(zoomLabel()), UI_DIM_TEXT, false);
          break;
        case 1:
          irWaveZoomIn();
          irPrint("[*] Zoom " + String(zoomLabel()), UI_DIM_TEXT, false);
          break;
        case 2:
          if (s_hasCapture) {
            s_autoTx = !s_autoTx;
            s_lastAutoMs = 0;
            s_contentDirty = true;
            irPrint(String("[*] Auto ") + (s_autoTx ? "ON" : "OFF"), s_autoTx ? UI_WARN : UI_DIM_TEXT, false);
            irUpdateStatusPanel(true);
          } else {
            irPrint("[!] No capture for auto", UI_DIM_TEXT, false);
          }
          break;
        default:
          break;
      }
      if (activeIcon == 0) {
        tft.drawBitmap(kIconZoomOutX, iconY, bitmap_icon_sort_down_minus, kIconSize, kIconSize, UI_ICON);
      } else if (activeIcon == 1) {
        tft.drawBitmap(kIconZoomInX, iconY, bitmap_icon_sort_up_plus, kIconSize, kIconSize, UI_ICON);
      } else if (activeIcon == 2) {
        tft.drawBitmap(kIconAutoX, iconY, bitmap_icon_random, kIconSize, kIconSize, UI_ICON);
      }
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = millis();
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > kToolbarY && y < (kToolbarY + kToolbarH)) {
        if (x > kIconBackX && x < (kIconBackX + kIconSize)) {
          feature_exit_requested = true;
        } else if (s_hasCapture && x > kIconZoomOutX && x < (kIconZoomOutX + kIconSize) && animationState == 0) {
          tft.drawBitmap(kIconZoomOutX, iconY, bitmap_icon_sort_down_minus, kIconSize, kIconSize, TFT_BLACK);
          animationState = 1;
          activeIcon = 0;
          lastAnimationTime = millis();
        } else if (s_hasCapture && x > kIconZoomInX && x < (kIconZoomInX + kIconSize) && animationState == 0) {
          tft.drawBitmap(kIconZoomInX, iconY, bitmap_icon_sort_up_plus, kIconSize, kIconSize, TFT_BLACK);
          animationState = 1;
          activeIcon = 1;
          lastAnimationTime = millis();
        } else if (x > kIconAutoX && x < (kIconAutoX + kIconSize) && animationState == 0) {
          tft.drawBitmap(kIconAutoX, iconY, bitmap_icon_random, kIconSize, kIconSize, TFT_BLACK);
          animationState = 1;
          activeIcon = 2;
          lastAnimationTime = millis();
        }
      }
    }
    lastTouchCheck = millis();
  }
}

static void tryReplay() {
  if (!s_hasCapture || s_rawLen == 0) return;

  s_recv.disableIRIn();
  irPrint("[!] Sending...", UI_TEXT, false);
  irDrawStatusLineIfChanged(0, "State: Sending", UI_WARN);

  for (uint8_t i = 0; i < s_repeat; i++) {
    s_send.sendRaw(s_raw, s_rawLen, kKhz);
    delay(35);
  }

  if (!s_autoTx) {
    irPrint("[+] Send done", UI_WARN, false);
  }

  delay(50);
  s_recv.enableIRIn();
  s_lastActionMs = millis();
  irUpdateStatusPanel(true);
}

static void saveCapture() {
  if (!s_hasCapture || s_rawLen == 0) {
    showNotification("IR", "No capture to save yet.");
    return;
  }
  if (!isSDCardAvailable()) {
    showNotification("IR", "SD not available.");
    return;
  }
  if (!irEnsureDir(IR_DIR)) {
    showNotification("IR", "Cannot create /ir");
    return;
  }

  String name = getUserInputName();
  if (name.length() == 0) {
    return;
  }

  irPrint("[!] Saving...", UI_TEXT, false);

  String path;
  if (!makeNextIrPath(path)) {
    showNotification("IR", "Too many files in /ir");
    return;
  }

  IrCaptureHeader h{};
  h.magic = IR_MAGIC;
  h.version = 1;
  h.khz = kKhz;
  h.rawLen = s_rawLen;
  h.decodeType = (uint8_t)s_decodeType;
  h.reserved = 0;
  h.bits = s_bits;
  h.value = s_value;
  memset(h.name, 0, sizeof(h.name));
  strncpy(h.name, name.c_str(), sizeof(h.name) - 1);

  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    showNotification("IR", "Open file failed");
    return;
  }
  bool ok = true;
  ok = ok && (f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h));
  ok = ok && (f.write((const uint8_t*)s_raw, (size_t)s_rawLen * sizeof(uint16_t)) == (size_t)s_rawLen * sizeof(uint16_t));
  f.close();

  if (!ok) {
    showNotification("IR", "Write failed");
    return;
  }

  irPrint("[+] Saved " + name, UI_OK, false);
}

static bool extractHexAfterLabel(const String& s, const char* label, uint64_t& out) {
  int li = s.indexOf(label);
  if (li < 0) return false;
  int ox = s.indexOf("0x", li);
  if (ox < 0) return false;
  int i = ox + 2;
  uint64_t v = 0;
  bool any = false;
  while (i < (int)s.length()) {
    char c = s[i];
    uint8_t d;
    if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint8_t)(10 + c - 'a');
    else if (c >= 'A' && c <= 'F') d = (uint8_t)(10 + c - 'A');
    else break;
    any = true;
    v = (v << 4) | d;
    i++;
  }
  if (!any) return false;
  out = v;
  return true;
}

static uint32_t stableHash() {

  if (!s_hasParsedKey) return hashWaveform();
  uint32_t h = 2166136261u;
  auto mix = [&](uint32_t v) { h ^= v; h *= 16777619u; };
  mix((uint32_t)s_decodeType);
  mix((uint32_t)s_bits);
  mix((uint32_t)s_keyAddr);
  mix((uint32_t)(s_keyAddr >> 32));
  mix((uint32_t)s_keyCmd);
  mix((uint32_t)(s_keyCmd >> 32));
  return h;
}

static void pollCapture() {
  if (s_recv.decode(&s_results)) {

    const uint16_t rawlen = s_results.rawlen;
    const uint16_t want = (rawlen > 1) ? (rawlen - 1) : 0;

    if (want == 0 || want > kMaxRawLen) {

      s_recv.resume();
      return;
    }

    const bool hadCapture = s_hasCapture;

    const bool allOnes =
        (s_results.value == 0xFFFFFFFFULL) ||
        (s_results.value == 0xFFFFULL) ||
        (s_results.value == 0xFFFFFFFFFFFFFFFFULL);
    const bool shortFrame = (want > 0 && want < 12);
    const bool looksLikeRepeat =
        hadCapture && ((shortFrame) || (allOnes && s_results.decode_type == s_decodeType));
    if (looksLikeRepeat) {
      s_recv.resume();
      return;
    }

    for (uint16_t i = 1; i < rawlen; i++) {

      s_raw[i - 1] = s_results.rawbuf[i] * kRawTick;
    }
    s_rawLen = want;

    s_decodeType = s_results.decode_type;
    s_value = s_results.value;
    s_bits = s_results.bits;

    s_hasParsedKey = false;
    s_keyAddr = 0;
    s_keyCmd = 0;
    s_keyText = "";
    {
      String human = resultToHumanReadableBasic(&s_results);
      uint64_t addr = 0, cmd = 0;
      const bool hasAddr = extractHexAfterLabel(human, "Address", addr) || extractHexAfterLabel(human, "Addr", addr);
      const bool hasCmd  = extractHexAfterLabel(human, "Command", cmd)  || extractHexAfterLabel(human, "Cmd", cmd);
      if (hasAddr && hasCmd) {
        s_hasParsedKey = true;
        s_keyAddr = addr;
        s_keyCmd  = cmd;
        s_keyText = String(typeToString(s_decodeType)) + " A:0x" + uint64ToString(addr, 16) + " C:0x" + uint64ToString(cmd, 16);
      } else {

        s_keyText = String(typeToString(s_decodeType)) + " V:0x" + uint64ToString(s_value, 16);
      }
    }

    s_hasCapture = true;

    const uint32_t newHash = stableHash();

    if (!hadCapture) {
      s_contentDirty = true;
      s_irGraphChromeDrawn = false;
      s_uiDrawn = false;
      s_waveZoomIdx = 0;
      s_wavePanUs = 0;
      irPrint("[+] Signal captured", UI_OK, false);
      s_lastWaveHash = newHash;
      s_lastStableHash = newHash;
    } else {
      if (newHash != s_lastStableHash) {
        s_lastStableHash = newHash;
        const uint32_t now = millis();
        if (s_lastCaptureUiMs == 0 || (now - s_lastCaptureUiMs) >= 150) {
          s_captureUiDirty = true;
          s_lastCaptureUiMs = now;
        }
      }
    }

    s_recv.resume();
  }
}

void setup() {
  setTouchButtonInputEnabled(true);
  irSetCaptureNavLabels();
  s_repeat = 1;
  s_autoTx = false;
  s_lastAutoMs = 0;
  s_uiDrawn = false;
  s_contentDirty = true;
  s_lastActionMs = 0;

  pinMode(kRecvPin, INPUT_PULLUP);
  pinMode(kSendPin, OUTPUT);
  s_send.begin();
  s_recv.enableIRIn();

  s_hasCapture = false;
  s_rawLen = 0;
  s_decodeType = decode_type_t::UNKNOWN;
  s_value = 0;
  s_bits = 0;

  irResetUiState();
  irClearBody(TFT_BLACK);
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  runUI();
  irDrawTextBoxes();
  irEnsurePlotLayout();
  irDrawGraphChrome();
  irUpdateStatusPanel(true);
  irPrint("[+] IR Record ready", UI_WARN, false);
  irPrint("[*] Point remote and press", UI_DIM_TEXT, false);
  irRedrawNavChrome();
  s_contentDirty = false;
}

void loop() {

  if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  runUI();
  if (s_uiDrawn) {
    tft.drawFastHLine(0, kToolbarY, 240, UI_LINE);
    tft.drawFastHLine(0, kToolbarY + kToolbarH, 240, UI_LINE);
  }
  handleTouchNavButtons();

  pollCapture();

  if (s_hasCapture) {
    int x, y;
    if (readTouchXY(x, y)) {
      if (!s_irPlot.valid) {
        irEnsurePlotLayout();
      }
      irHandleGraphTouch(x, y);
    }
  }

  if (s_captureUiDirty && s_hasCapture) {
    redrawCapturedDetailsOnly();
    s_captureUiDirty = false;
  }

  if (!s_hasCapture) {
    uint32_t nowUs = micros();
    if (s_lastScopeSampleUs == 0) s_lastScopeSampleUs = nowUs;

    uint8_t steps = 0;
    while ((uint32_t)(nowUs - s_lastScopeSampleUs) >= 1000 && steps < 8) {
      s_lastScopeSampleUs += 1000;

      s_scope[s_scopePos] = (uint8_t)(digitalRead(kRecvPin) ? 1 : 0);
      s_scopePos = (uint16_t)((s_scopePos + 1) % 240);
      steps++;
    }

    uint32_t nowMs = millis();
    if (s_lastScopeDrawMs == 0) s_lastScopeDrawMs = nowMs;
    if ((uint32_t)(nowMs - s_lastScopeDrawMs) >= 80) {

      redrawScopePlotOnly();
      s_lastScopeDrawMs = nowMs;
    }
  }

  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 200;
  static bool prevLeft=false, prevRight=false, prevUp=false, prevDown=false;
  const bool leftPressed  = isPhysicalButtonPressed(BTN_LEFT);
  const bool rightPressed = isPhysicalButtonPressed(BTN_RIGHT);
  const bool upPressed    = isPhysicalButtonPressed(BTN_UP);
  const bool downPressed  = isPhysicalButtonPressed(BTN_DOWN);

  if (rightPressed && !prevRight && millis() - lastDebounceTime > debounceDelay) {
    s_autoTx = false;
    if (s_repeat < 10) s_repeat++;
    s_contentDirty = true;
    irUpdateStatusPanel(true);
    lastDebounceTime = millis();
  }
  if (leftPressed && !prevLeft && millis() - lastDebounceTime > debounceDelay) {
    s_autoTx = false;
    if (s_repeat > 1) s_repeat--;
    s_contentDirty = true;
    irUpdateStatusPanel(true);
    lastDebounceTime = millis();
  }
  if (upPressed && !prevUp && s_hasCapture && millis() - lastDebounceTime > debounceDelay) {
    s_autoTx = false;
    tryReplay();
    lastDebounceTime = millis();
  }
  if (downPressed && !prevDown && s_hasCapture && millis() - lastDebounceTime > debounceDelay) {
    saveCapture();
    lastDebounceTime = millis();
  }

  prevLeft = leftPressed;
  prevRight = rightPressed;
  prevUp = upPressed;
  prevDown = downPressed;

  if (s_autoTx && s_hasCapture) {
    uint32_t now = millis();
    if (s_lastAutoMs == 0 || (now - s_lastAutoMs) >= s_autoIntervalMs) {
      tryReplay();
      s_lastAutoMs = now;
    }
  }

  if (s_contentDirty) {
    currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, false);
    runUI();
    updateDisplay();
    s_contentDirty = false;
  }

  delay(10);
}

}

namespace IRSavedProfile {

static constexpr const char* IR_DIR = "/ir";
static constexpr uint32_t IR_MAGIC = 0x31525249;

struct __attribute__((packed)) IrCaptureHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t khz;
  uint16_t rawLen;
  uint8_t  decodeType;
  uint8_t  reserved;
  uint16_t bits;
  uint64_t value;
  char     name[16];
};

static bool uiDrawn = false;
static int yshift = 16;

static constexpr uint8_t ITEMS_PER_PAGE = 7;
static constexpr int LIST_X = 10;
static constexpr int LIST_W = 220;
static constexpr int HEADER_Y = 50;
static constexpr int HEADER_H = 14;
static constexpr int LIST_Y = HEADER_Y + HEADER_H + 2;
static constexpr int ROW_H  = 18;
static constexpr int UI_GAP_Y = 6;
static constexpr int DETAIL_LINE_H = 14;
static constexpr int DETAIL_LINE_GAP = 3;
static constexpr int DETAIL_LINE_STEP = DETAIL_LINE_H + DETAIL_LINE_GAP;
static constexpr int DETAIL_LINES = 4;
static constexpr int DETAIL_CONTENT_H = DETAIL_LINE_STEP * (DETAIL_LINES - 1) + DETAIL_LINE_H;
static constexpr int DETAIL_LABEL_X = LIST_X;
static constexpr int DETAIL_VALUE_X = 50;
static constexpr int DETAIL_COL2_LABEL_X = 130;
static constexpr int DETAIL_COL2_VALUE_X = 165;

static int profileBottomY() {
  return irContentBottom();
}

static int listBottomY() {
  return LIST_Y + (ITEMS_PER_PAGE * ROW_H);
}

static int detailsY() {
  const int areaTop = listBottomY();
  const int areaBottom = profileBottomY();
  const int areaH = areaBottom - areaTop;
  if (areaH <= DETAIL_CONTENT_H) {
    return areaTop + UI_GAP_Y;
  }
  return areaTop + (areaH - DETAIL_CONTENT_H) / 2;
}

static std::vector<String> irFiles;
static uint16_t irTotal = 0;
static uint16_t currentIndex = 0;
static String sdLastErr = "";

static String selectedPath = "";
static IrCaptureHeader selectedHeader{};
static bool selectedValid = false;

static uint16_t cachedPageStart = 0xFFFF;
static IrCaptureHeader cachedPage[ITEMS_PER_PAGE]{};
static bool cachedOk[ITEMS_PER_PAGE]{};
static bool cacheDirty = true;

static bool deleteArmed = false;
static uint32_t deleteArmUntilMs = 0;

static constexpr uint16_t kMaxRawLen2 = 512;
static uint16_t txRaw[kMaxRawLen2]{};

static IRsend irsend(IR_TX_PIN);
static void transmitProfile(uint16_t idx);
static void deleteProfile(uint16_t idx);

static bool endsWith(const String& s, const char* suf) {
  int sl = s.length();
  int tl = (int)strlen(suf);
  if (tl > sl) return false;
  return s.substring(sl - tl) == suf;
}

static String baseName(const String& path) {
  int slash = path.lastIndexOf('/');
  return (slash >= 0) ? path.substring(slash + 1) : path;
}

static bool readHeader(const String& path, IrCaptureHeader& out, String* errOut = nullptr) {
  File f = SD.open(path, FILE_READ);
  if (!f) { if (errOut) *errOut = "Open failed"; return false; }
  if (f.read((uint8_t*)&out, sizeof(out)) != (int)sizeof(out)) { f.close(); if (errOut) *errOut="Read hdr failed"; return false; }
  f.close();
  if (out.magic != IR_MAGIC || out.version != 1) { if (errOut) *errOut="Bad file"; return false; }
  out.name[15] = '\0';
  if (out.rawLen == 0 || out.rawLen > kMaxRawLen2) { if (errOut) *errOut="Bad rawLen"; return false; }
  if (out.khz == 0 || out.khz > 100) { if (errOut) *errOut="Bad khz"; return false; }
  return true;
}

static bool readRaw(const String& path, const IrCaptureHeader& h, uint16_t* outRaw, String* errOut = nullptr) {
  File f = SD.open(path, FILE_READ);
  if (!f) { if (errOut) *errOut = "Open failed"; return false; }

  if (!f.seek(sizeof(IrCaptureHeader))) { f.close(); if (errOut) *errOut="Seek failed"; return false; }
  const size_t want = (size_t)h.rawLen * sizeof(uint16_t);
  if (f.read((uint8_t*)outRaw, want) != (int)want) { f.close(); if (errOut) *errOut="Read raw failed"; return false; }
  f.close();
  return true;
}

static uint16_t pageStartForIndex(uint16_t idx) {
  return (uint16_t)((idx / ITEMS_PER_PAGE) * ITEMS_PER_PAGE);
}

static void refreshSdIndex(bool keepSelection = true) {
  String oldSel = keepSelection && irTotal > 0 ? irFiles[currentIndex] : String("");

  irFiles.clear();
  irTotal = 0;
  sdLastErr = "";

  if (!isSDCardAvailable()) {
    sdLastErr = "SD not available";
    currentIndex = 0;
    selectedValid = false;
    cacheDirty = true;
    return;
  }

  if (!SD.exists(IR_DIR)) {
    sdLastErr = "No /ir";
    currentIndex = 0;
    selectedValid = false;
    cacheDirty = true;
    return;
  }

  File d = SD.open(IR_DIR);
  if (!d) {
    sdLastErr = "Open /ir failed";
    currentIndex = 0;
    selectedValid = false;
    cacheDirty = true;
    return;
  }

  for (;;) {
    File f = d.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String name = String(f.name());

      if (endsWith(name, ".bin") && (name.startsWith("ir_") || name.startsWith("/ir/ir_") || name.startsWith(String(IR_DIR) + "/ir_"))) {
        String full = name.startsWith("/") ? name : (String(IR_DIR) + "/" + name);
        irFiles.push_back(full);
      }
    }
    f.close();
  }
  d.close();

  if (irFiles.empty()) {
    sdLastErr = "No IR captures";
    currentIndex = 0;
    selectedValid = false;
    cacheDirty = true;
    return;
  }

  std::sort(irFiles.begin(), irFiles.end(), [](const String& a, const String& b) {
    return baseName(a) < baseName(b);
  });

  irTotal = (uint16_t)irFiles.size();

  if (keepSelection && oldSel.length()) {
    auto it = std::find(irFiles.begin(), irFiles.end(), oldSel);
    if (it != irFiles.end()) currentIndex = (uint16_t)std::distance(irFiles.begin(), it);
  }
  if (currentIndex >= irTotal) currentIndex = (uint16_t)(irTotal - 1);

  selectedValid = false;
  cacheDirty = true;
}

static bool loadSelected(String* errOut = nullptr) {
  if (irTotal == 0) { selectedValid = false; return false; }
  selectedPath = irFiles[currentIndex];
  IrCaptureHeader h{};
  if (!readHeader(selectedPath, h, errOut)) { selectedValid = false; return false; }
  selectedHeader = h;
  selectedValid = true;
  return true;
}

static void ensurePageCache() {
  if (irTotal == 0) return;
  uint16_t start = pageStartForIndex(currentIndex);
  if (!cacheDirty && cachedPageStart == start) return;
  cachedPageStart = start;
  for (uint8_t i = 0; i < ITEMS_PER_PAGE; i++) {
    cachedOk[i] = false;
    uint16_t idx = (uint16_t)(start + i);
    if (idx >= irTotal) continue;
    String err;
    cachedOk[i] = readHeader(irFiles[idx], cachedPage[i], &err);
    if (!cachedOk[i]) memset(&cachedPage[i], 0, sizeof(IrCaptureHeader));
  }
  cacheDirty = false;
}

static void drawHeaderLine() {
  tft.fillRect(LIST_X, HEADER_Y, LIST_W, HEADER_H, FEATURE_BG);
  tft.setCursor(LIST_X, HEADER_Y);
  tft.setTextColor(UI_ICON, FEATURE_BG);
  tft.printf("Profile %d/%d", (int)currentIndex + 1, (int)irTotal);
}

static void drawRow(uint16_t pageStart, uint8_t row) {
  uint16_t idx = (uint16_t)(pageStart + row);
  if (idx >= irTotal) return;
  bool isSel = (idx == currentIndex);
  int y = LIST_Y + (row * ROW_H);

  uint16_t bg = isSel ? UI_FG : FEATURE_BG;
  uint16_t fg = isSel ? UI_ICON : UI_TEXT;
  tft.fillRect(LIST_X, y, LIST_W, ROW_H - 1, bg);
  tft.setTextColor(fg, bg);
  tft.setCursor(LIST_X, y + 4);
  tft.printf("%2d. ", (int)idx + 1);

  if (cachedOk[row]) {
    char nameBuf[17];
    memcpy(nameBuf, cachedPage[row].name, 16);
    nameBuf[16] = '\0';
    String nm = String(nameBuf);
    if (nm.length() == 0) nm = baseName(irFiles[idx]);
    if (nm.length() > 10) nm = nm.substring(0, 10);
    tft.print(nm);

    decode_type_t dt = (decode_type_t)cachedPage[row].decodeType;
    String ty = String(typeToString(dt));
    if (ty.length() > 6) ty = ty.substring(0, 6);
    int tw = tft.textWidth(ty, 1);
    tft.setCursor(LIST_X + LIST_W - 4 - tw, y + 4);
    tft.print(ty);
  } else {
    tft.print("<?>");
  }
}

static void drawListPage(uint16_t pageStart) {
  ensurePageCache();
  tft.fillRect(LIST_X, LIST_Y, LIST_W, (ITEMS_PER_PAGE * ROW_H), FEATURE_BG);
  for (uint8_t row = 0; row < ITEMS_PER_PAGE; row++) {
    if ((uint16_t)(pageStart + row) >= irTotal) break;
    drawRow(pageStart, row);
  }
}

static void drawDetails() {
  const int dy = detailsY();
  const int gapTop = listBottomY();
  const int gapH = profileBottomY() - gapTop;
  if (gapH > 0) {
    tft.fillRect(LIST_X, gapTop, LIST_W, gapH, FEATURE_BG);
  }
  tft.drawFastHLine(LIST_X, listBottomY(), LIST_W, UI_LINE);

  String err;
  if (!selectedValid) loadSelected(&err);
  tft.setTextColor(UI_TEXT, FEATURE_BG);
  if (!selectedValid) {
    tft.setCursor(DETAIL_LABEL_X, dy);
    tft.print("Read failed:");
    tft.setCursor(DETAIL_VALUE_X, dy);
    tft.print(err);
    return;
  }

  decode_type_t dt = (decode_type_t)selectedHeader.decodeType;
  tft.setCursor(DETAIL_LABEL_X, dy);
  tft.print("Type:");
  tft.setCursor(DETAIL_VALUE_X, dy);
  tft.print(String(typeToString(dt)));
  tft.setCursor(DETAIL_COL2_LABEL_X, dy);
  tft.print("kHz:");
  tft.setCursor(DETAIL_COL2_VALUE_X, dy);
  tft.printf("%ukHz", (unsigned)selectedHeader.khz);

  tft.setCursor(DETAIL_LABEL_X, dy + DETAIL_LINE_STEP);
  tft.print("Bits:");
  tft.setCursor(DETAIL_VALUE_X, dy + DETAIL_LINE_STEP);
  tft.print((unsigned)selectedHeader.bits);
  tft.setCursor(DETAIL_COL2_LABEL_X, dy + DETAIL_LINE_STEP);
  tft.print("Raw:");
  tft.setCursor(DETAIL_COL2_VALUE_X, dy + DETAIL_LINE_STEP);
  tft.print((unsigned)selectedHeader.rawLen);

  tft.setCursor(DETAIL_LABEL_X, dy + (DETAIL_LINE_STEP * 2));
  tft.print("Val:");
  tft.setCursor(DETAIL_VALUE_X, dy + (DETAIL_LINE_STEP * 2));
  tft.print("0x");
  tft.print(uint64ToString(selectedHeader.value, 16));

  tft.setTextColor(UI_DIM_TEXT, FEATURE_BG);
  tft.setCursor(DETAIL_LABEL_X, dy + (DETAIL_LINE_STEP * 3));
  tft.print("SRC:");
  tft.setCursor(DETAIL_VALUE_X, dy + (DETAIL_LINE_STEP * 3));
  tft.print(baseName(selectedPath));

  if (deleteArmed && (int32_t)(millis() - deleteArmUntilMs) < 0) {
    int hintY = dy + (DETAIL_LINE_STEP * 4);
    if (hintY >= profileBottomY() - 12) hintY = profileBottomY() - 12;
    tft.setCursor(DETAIL_LABEL_X, hintY);
    tft.setTextColor(UI_WARN, FEATURE_BG);
    tft.print("Press Delete again to confirm");
  }
}

static void updateSelectionUI(uint16_t oldIndex, bool forceListRedraw = false) {
  if (irTotal == 0) return;
  uint16_t oldPage = pageStartForIndex(oldIndex);
  uint16_t newPage = pageStartForIndex(currentIndex);

  tft.startWrite();
  drawHeaderLine();
  if (forceListRedraw || oldPage != newPage) {
    drawListPage(newPage);
  } else {

    uint16_t pageStart = newPage;
    uint8_t oldRow = (uint8_t)(oldIndex - pageStart);
    uint8_t newRow = (uint8_t)(currentIndex - pageStart);
    ensurePageCache();
    if (oldRow < ITEMS_PER_PAGE) drawRow(pageStart, oldRow);
    if (newRow < ITEMS_PER_PAGE) drawRow(pageStart, newRow);
  }
  drawDetails();
  tft.endWrite();
}

static void selectNext() {
  if (irTotal == 0) return;
  uint16_t oldIdx = currentIndex;
  currentIndex = (uint16_t)((currentIndex + 1) % irTotal);
  selectedValid = false;
  cacheDirty = true;
  deleteArmed = false;
  updateSelectionUI(oldIdx, false);
}

static void selectPrev() {
  if (irTotal == 0) return;
  uint16_t oldIdx = currentIndex;
  currentIndex = (uint16_t)((currentIndex + irTotal - 1) % irTotal);
  selectedValid = false;
  cacheDirty = true;
  deleteArmed = false;
  updateSelectionUI(oldIdx, false);
}

static void handleTouchNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }

  if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
    if (irTotal > 0) deleteProfile(currentIndex);
  }
  if (isTouchNavButtonPressedEdge(BTN_DOWN)) {
    selectNext();
  }
  if (isTouchNavButtonPressedEdge(BTN_UP)) {
    selectPrev();
  }
  if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
    if (irTotal > 0) transmitProfile(currentIndex);
  }
}

static void updateDisplay() {
  irClearBody(FEATURE_BG);
  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  uiDrawn = false;

  drawHeaderLine();
  if (irTotal == 0) {
    tft.setCursor(LIST_X, HEADER_Y + DETAIL_LINE_H);
    tft.setTextColor(UI_TEXT, FEATURE_BG);
    tft.print(sdLastErr.length() ? sdLastErr : "No profiles on SD.");
    return;
  }
  drawListPage(pageStartForIndex(currentIndex));
  drawDetails();
}

static void transmitProfile(uint16_t idx) {
  if (irTotal == 0 || idx >= irTotal) return;

  String path = irFiles[idx];
  String err;
  IrCaptureHeader h{};
  if (!readHeader(path, h, &err)) {
    showNotification("IR", "Read failed");
    selectedValid = false;
    cacheDirty = true;
    updateDisplay();
    return;
  }
  if (!readRaw(path, h, txRaw, &err)) {
    showNotification("IR", "Raw read failed");
    updateDisplay();
    return;
  }

  irsend.begin();
  irClearContentFrom(40, FEATURE_BG);
  tft.setCursor(10, 44);
  tft.setTextColor(UI_TEXT, FEATURE_BG);
  tft.print("Sending...");

  irsend.sendRaw(txRaw, h.rawLen, h.khz);
  delay(250);

  irClearContentFrom(40, FEATURE_BG);
  tft.setCursor(10, 44);
  tft.print("Done!");
  delay(300);

  updateDisplay();
}

static void deleteProfile(uint16_t idx) {
  if (irTotal == 0 || idx >= irTotal) return;

  uint32_t now = millis();
  if (!deleteArmed || (int32_t)(now - deleteArmUntilMs) >= 0) {
    deleteArmed = true;
    deleteArmUntilMs = now + 3000;
    drawDetails();
    return;
  }

  deleteArmed = false;

  String path = irFiles[idx];
  if (!SD.remove(path)) {
    irClearContentFrom(40, FEATURE_BG);
    tft.setCursor(10, 30 + yshift);
    tft.setTextColor(UI_WARN, FEATURE_BG);
    tft.print("Delete FAILED");
    tft.setCursor(10, 45 + yshift);
    tft.setTextColor(UI_TEXT, FEATURE_BG);
    tft.print(baseName(path));
    delay(1200);
    updateDisplay();
    return;
  }

  refreshSdIndex(false);
  if (irTotal == 0) currentIndex = 0;
  else if (currentIndex >= irTotal) currentIndex = (uint16_t)(irTotal - 1);
  selectedValid = false;
  cacheDirty = true;
  updateDisplay();
}

static void runUI() {

  static const int ICON_NUM = 4;
  static int iconX[ICON_NUM] = {130, 170, 210, 10};
  static int iconY = 20;

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_antenna,
    bitmap_icon_recycle,
    bitmap_icon_undo,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {

    tft.fillRect(0, 20, 240, 16, UI_FG);
    tft.drawLine(0, 20, 240, 20, UI_LINE);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i]) tft.drawBitmap(iconX[i], iconY, icons[i], 16, 16, UI_ICON);
    }
    tft.drawLine(0, 20 + 16, 240, 20 + 16, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], 16, 16, UI_ICON);
      animationState = 2;

      switch (activeIcon) {
        case 0:
          if (irTotal > 0) transmitProfile(currentIndex);
          break;
        case 1:
          if (irTotal > 0) deleteProfile(currentIndex);
          break;
        case 2:
          refreshSdIndex(true);
          selectedValid = false;
          cacheDirty = true;
          deleteArmed = false;
          updateDisplay();
          break;
        case 3:
          feature_exit_requested = true;
          break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = millis();
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {

      if (y >= LIST_Y && y < (LIST_Y + (ITEMS_PER_PAGE * ROW_H)) && x >= LIST_X && x < (LIST_X + LIST_W)) {
        uint8_t row = (uint8_t)((y - LIST_Y) / ROW_H);
        uint16_t oldIdx = currentIndex;
        uint16_t start = pageStartForIndex(currentIndex);
        uint16_t idx = (uint16_t)(start + row);
        if (idx < irTotal) {
          currentIndex = idx;
          selectedValid = false;
          cacheDirty = true;
          deleteArmed = false;
          updateSelectionUI(oldIdx, false);
        }
      }

      if (y > 20 && y < (20 + 16)) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + 16) {
            if (icons[i] && animationState == 0) {
              if (i == 3) {
                feature_exit_requested = true;
              } else {
                tft.drawBitmap(iconX[i], iconY, icons[i], 16, 16, FEATURE_BG);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = millis();
              }
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = millis();
  }
}

void setup() {
  setTouchButtonInputEnabled(true);
  irSetSavedNavLabels();
  irsend.begin();
  irClearBody(FEATURE_BG);
  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  uiDrawn = false;

  refreshSdIndex(false);
  cacheDirty = true;
  deleteArmed = false;
  updateDisplay();
  irRedrawNavChrome();
}

void loop() {
  if (feature_active && (feature_exit_requested || featureExitButtonPressed())) {
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  runUI();
  if (uiDrawn) {
    tft.drawFastHLine(0, 20, 240, UI_LINE);
    tft.drawFastHLine(0, 36, 240, UI_LINE);
  }
  handleTouchNavButtons();

  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 200;
  static bool prevUp = false;
  static bool prevDown = false;
  static bool prevRight = false;
  static bool prevLeft = false;

  bool prevPressed    = isPhysicalButtonPressed(BTN_UP);
  bool nextPressed    = isPhysicalButtonPressed(BTN_DOWN);
  bool txPressed      = isPhysicalButtonPressed(BTN_RIGHT);
  bool deletePressed  = isPhysicalButtonPressed(BTN_LEFT);

  if (irTotal > 0) {

    if (nextPressed && !prevDown && millis() - lastDebounceTime > debounceDelay) {
      selectNext();
      lastDebounceTime = millis();
    }

    if (prevPressed && !prevUp && millis() - lastDebounceTime > debounceDelay) {
      selectPrev();
      lastDebounceTime = millis();
    }

    if (txPressed && !prevRight && millis() - lastDebounceTime > debounceDelay) {
      transmitProfile(currentIndex);
      lastDebounceTime = millis();
    }

    if (deletePressed && !prevLeft && millis() - lastDebounceTime > debounceDelay) {
      deleteProfile(currentIndex);
      lastDebounceTime = millis();
    }
  } else {
    tft.setCursor(10, 50 + yshift);
    tft.setTextColor(UI_TEXT, FEATURE_BG);
    tft.print(sdLastErr.length() ? sdLastErr : "No profiles on SD.");
  }

  prevUp = prevPressed;
  prevDown = nextPressed;
  prevRight = txPressed;
  prevLeft = deletePressed;
}

}

namespace IRUniversalController {

static constexpr const char* PROFILES_PATH = "/ir_profiles.json";
static constexpr const char* PROFILES_DIR  = "/ir_profiles";

static IRsend s_send(IR_TX_PIN);

static constexpr uint16_t kMaxProfiles = 500;

static constexpr int16_t kToolbarY = 20;
static constexpr int16_t kToolbarH = 16;
static constexpr int16_t kIconSize = 16;
static constexpr int kToolbarBottom = kToolbarY + kToolbarH;
static constexpr int kBodyTop = kToolbarBottom + 1;

static constexpr int kPadX = 10;
static constexpr int kListW = 220;
static constexpr int kDetailLineH = 14;
static constexpr int kDetailLineGap = 3;
static constexpr int kDetailLineStep = kDetailLineH + kDetailLineGap;
static constexpr int kDetailValueX = 50;
static constexpr int kDetailCol2LabelX = 138;
static constexpr int kDetailCol2ValueX = 173;

static constexpr int kInfoBoxX = 4;
static constexpr int kInfoBoxW = 232;
static constexpr int kInfoBoxRight = kInfoBoxX + kInfoBoxW;
static constexpr int kInfoBoxHeaderH = 15;
static constexpr int kInfoLines = 3;
static constexpr int kInfoContentH = kDetailLineStep * (kInfoLines - 1) + kDetailLineH;
static constexpr int kInfoBoxY = kBodyTop + 6;
static constexpr int kInfoBoxH = kInfoBoxHeaderH + kInfoContentH + 8;
static constexpr int kInfoTextY = kInfoBoxY + kInfoBoxHeaderH + 2;
static constexpr int kRemoteKeysTop = kInfoBoxY + kInfoBoxH + 10;

static constexpr int kBrowseHeaderY = kToolbarBottom + 8;
static constexpr int kBrowseHeaderH = 14;
static constexpr int kListY = kBrowseHeaderY + kBrowseHeaderH + 2;

static constexpr int kIconBackX = 10;
static constexpr int kIconBrowseX = 170;
static constexpr int kIconReloadX = 210;

enum KeyId : uint8_t {
  Power = 0,
  Mute,
  VolUp,
  VolDn,
  ChUp,
  ChDn,
  Up,
  Down,
  Left,
  Right,
  Ok,
  Back,
  Home,
  Guide,
  KeyCount
};

static const char* keyLabel(KeyId k) {
  switch (k) {
    case Power: return "PWR";
    case Mute:  return "MUTE";
    case VolUp: return "VOL+";
    case VolDn: return "VOL-";
    case ChUp:  return "CH+";
    case ChDn:  return "CH-";
    case Up:    return "UP";
    case Down:  return "DOWN";
    case Left:  return "LEFT";
    case Right: return "RIGHT";
    case Ok:    return "OK";
    case Back:  return "BACK";
    case Home:  return "HOME";
    case Guide: return "GUIDE";
    default:    return "?";
  }
}

struct Profile {
  String name;
  String category;
  String brand;
  String model;
  bool fromSd = false;
  decode_type_t proto = decode_type_t::UNKNOWN;
  uint16_t bits = 0;
  // Keep as 64-bit so we can support protocols like RC6 (36-bit), Panasonic (48-bit),
  // Pioneer (64-bit), etc. Avoid ArduinoJson's 64-bit requirement by only parsing
  // 64-bit values from strings (not Variant::as<uint64_t>).
  uint64_t code[KeyCount]{};
  bool has[KeyCount]{};
};

static std::vector<Profile> s_profiles;
static int s_profileIdx = 0;
static bool s_uiDrawn = false;
static String s_lastErr = "";
static bool s_loadedFromSd = false;

static FeatureUI::Button s_keyBtns[KeyCount];


static const unsigned char* keyIcon(KeyId k) {
  switch (k) {
    case Power: return bitmap_icon_power;
    case Mute:  return nullptr;  // desenhado como texto "MUTE"
    case VolUp: return bitmap_icon_sort_up_plus;
    case VolDn: return bitmap_icon_sort_down_minus;
    case ChUp:  return bitmap_icon_UP;
    case ChDn:  return bitmap_icon_DOWN;
    case Up:    return bitmap_icon_UP;
    case Down:  return bitmap_icon_DOWN;
    case Left:  return bitmap_icon_LEFT;
    case Right: return bitmap_icon_RIGHT;
    case Ok:    return bitmap_icon_start;
    case Back:  return bitmap_icon_go_back;
    case Home:  return bitmap_icon_house;
    case Guide: return bitmap_icon_list;
    default:    return nullptr;
  }
}

static void drawKeyButton(KeyId k, bool pressed = false) {
  const FeatureUI::Button& b = s_keyBtns[(int)k];
  if (b.w <= 0 || b.h <= 0) return;

  const bool disabled = b.disabled;
  const uint16_t fill = disabled ? UI_BG : (pressed ? UI_FG : UI_BG);
  const uint16_t edge = disabled ? UI_LINE : (pressed ? UI_ICON : UI_LINE);

  tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, fill);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, edge);

  const unsigned char* ico = keyIcon(k);
  if (ico) {
    const int iconSize = 16;
    const int ix = b.x + (b.w - iconSize) / 2;
    const int iy = b.y + (b.h - iconSize) / 2;
    tft.drawBitmap(ix, iy, ico, iconSize, iconSize, disabled ? UI_DIM_TEXT : UI_ICON);
  } else {
    // Sem ícone: desenha o rótulo como texto (ex.: MUTE)
    const char* lbl = keyLabel(k);
    tft.setTextFont(1);
    tft.setTextSize(1);
    const int tw = tft.textWidth(lbl, 1);
    const uint16_t tc = disabled ? UI_DIM_TEXT : UI_ICON;  // mesma cor dos icones
    tft.setTextColor(tc, fill);
    tft.setCursor(b.x + (b.w - tw) / 2, b.y + (b.h - 8) / 2);
    tft.print(lbl);
  }
}

static String shortStatusLine(const String& s, int maxLen = 32) {
  if ((int)s.length() <= maxLen) return s;
  return s.substring(0, maxLen);
}

static String truncateWithEllipsis(const String& s, int maxPx) {
  if (maxPx <= 0 || s.length() == 0) return s;
  if ((int)tft.textWidth(s, 1) <= maxPx) return s;
  const int ellW = tft.textWidth("...", 1);
  String out = s;
  while (out.length() > 0 && (int)tft.textWidth(out, 1) + ellW > maxPx) {
    out.remove(out.length() - 1);
  }
  return out + "...";
}

static String normalizeToken(const String& in) {
  String s;
  s.reserve(in.length());
  for (int i = 0; i < (int)in.length(); i++) {
    char c = in[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) s += c;
  }
  return s;
}

static bool keyFromToken(const String& tok, KeyId& out) {
  String k = normalizeToken(tok);
  if (k == "POWER" || k == "PWR") { out = Power; return true; }
  if (k == "MUTE") { out = Mute; return true; }
  if (k == "VOLUP" || k == "VOLUMEUP" || k == "VOLPLUS" || k == "VUP" || k == "VOLP") { out = VolUp; return true; }
  if (k == "VOLDN" || k == "VOLDOWN" || k == "VOLUMEDOWN" || k == "VOLMINUS" || k == "VDN" || k == "VOLM") { out = VolDn; return true; }
  if (k == "CHUP" || k == "CHANUP" || k == "CHANNELUP" || k == "CHPLUS") { out = ChUp; return true; }
  if (k == "CHDN" || k == "CHDOWN" || k == "CHANDOWN" || k == "CHANNELDOWN" || k == "CHMINUS") { out = ChDn; return true; }
  if (k == "UP") { out = Up; return true; }
  if (k == "DOWN" || k == "DN") { out = Down; return true; }
  if (k == "LEFT") { out = Left; return true; }
  if (k == "RIGHT") { out = Right; return true; }
  if (k == "OK" || k == "ENTER" || k == "SELECT") { out = Ok; return true; }
  if (k == "BACK" || k == "RETURN" || k == "EXIT") { out = Back; return true; }
  if (k == "HOME" || k == "SMARTHUB" || k == "SMART") { out = Home; return true; }
  if (k == "GUIDE" || k == "EPG") { out = Guide; return true; }
  return false;
}

static bool protoFromToken(const String& tok, decode_type_t& out) {
  String p = normalizeToken(tok);
  if (p == "NEC") { out = decode_type_t::NEC; return true; }
  if (p == "NECLIKE") { out = decode_type_t::NEC_LIKE; return true; }
  if (p == "SONY" || p == "SIRC") { out = decode_type_t::SONY; return true; }
  if (p == "SAMSUNG" || p == "SAMSUNG32") { out = decode_type_t::SAMSUNG; return true; }
  if (p == "SAMSUNG36") { out = decode_type_t::SAMSUNG36; return true; }
  if (p == "LG") { out = decode_type_t::LG; return true; }
  if (p == "LG2") { out = decode_type_t::LG2; return true; }
  if (p == "JVC") { out = decode_type_t::JVC; return true; }
  if (p == "DENON") { out = decode_type_t::DENON; return true; }
  if (p == "PANASONIC" || p == "KASEIKYO") { out = decode_type_t::PANASONIC; return true; }
  if (p == "RC5" || p == "RC5X") { out = decode_type_t::RC5; return true; }
  if (p == "RC6") { out = decode_type_t::RC6; return true; }
  if (p == "PIONEER") { out = decode_type_t::PIONEER; return true; }
  if (p == "DISH") { out = decode_type_t::DISH; return true; }
  if (p == "GICABLE") { out = decode_type_t::GICABLE; return true; }
  if (p == "EPSON") { out = decode_type_t::EPSON; return true; }

  return false;
}

static String inferCategoryFromName(const String& name) {
  String n = name;
  n.toUpperCase();
  if (n.indexOf("TV") >= 0) return "TV";
  if (n.indexOf("AVR") >= 0 || n.indexOf("RECEIVER") >= 0 || n.indexOf("SOUNDBAR") >= 0) return "AVR";
  if (n.indexOf("PROJECTOR") >= 0) return "PROJECTOR";
  if (n.indexOf("XBOX") >= 0 || n.indexOf("PLAYSTATION") >= 0) return "GAME";
  if (n.indexOf("AC") >= 0 || n.indexOf("AIR") >= 0) return "AC";
  if (n.indexOf("CABLE") >= 0 || n.indexOf("DISH") >= 0 || n.indexOf("STB") >= 0) return "STB";
  return "OTHER";
}

static String inferBrandFromName(const String& name) {
  int par = name.indexOf('(');
  String s = (par > 0) ? name.substring(0, par) : name;
  s.trim();
  int sp = s.indexOf(' ');
  if (sp > 0) s = s.substring(0, sp);
  s.trim();
  if (!s.length()) return "UNKNOWN";
  s.toUpperCase();
  return s;
}

static void loadBuiltinProfiles() {
  s_profiles.clear();

  {
    Profile p{};
    p.name = "Samsung TV (common)";
    p.proto = decode_type_t::SAMSUNG;
    p.bits = 32;
    p.code[Power] = 0xE0E040BFu; p.has[Power] = true;
    p.code[Mute]  = 0xE0E0F00Fu; p.has[Mute]  = true;
    p.code[VolUp] = 0xE0E0E01Fu; p.has[VolUp] = true;
    p.code[VolDn] = 0xE0E0D02Fu; p.has[VolDn] = true;
    p.code[ChUp]  = 0xE0E048B7u; p.has[ChUp]  = true;
    p.code[ChDn]  = 0xE0E008F7u; p.has[ChDn]  = true;
    p.code[Up]    = 0xE0E006F9u; p.has[Up]    = true;
    p.code[Down]  = 0xE0E08679u; p.has[Down]  = true;
    p.code[Left]  = 0xE0E0A659u; p.has[Left]  = true;
    p.code[Right] = 0xE0E046B9u; p.has[Right] = true;
    p.code[Ok]    = 0xE0E016E9u; p.has[Ok]    = true;
    p.code[Back]  = 0xE0E01AE5u; p.has[Back]  = true;
    s_profiles.push_back(p);
  }

  {
    Profile p{};
    p.name = "LG TV (common)";
    p.proto = decode_type_t::NEC;
    p.bits = 32;
    p.code[Power] = 0x20DF10EFu; p.has[Power] = true;
    p.code[Mute]  = 0x20DF906Fu; p.has[Mute]  = true;
    p.code[VolUp] = 0x20DF40BFu; p.has[VolUp] = true;
    p.code[VolDn] = 0x20DFC03Fu; p.has[VolDn] = true;
    p.code[ChUp]  = 0x20DF00FFu; p.has[ChUp]  = true;
    p.code[ChDn]  = 0x20DF807Fu; p.has[ChDn]  = true;
    p.code[Up]    = 0x20DF02FDu; p.has[Up]    = true;
    p.code[Down]  = 0x20DF827Du; p.has[Down]  = true;
    p.code[Left]  = 0x20DFE01Fu; p.has[Left]  = true;
    p.code[Right] = 0x20DF609Fu; p.has[Right] = true;
    p.code[Ok]    = 0x20DF22DDu; p.has[Ok]    = true;
    p.code[Back]  = 0x20DF14EBu; p.has[Back]  = true;
    s_profiles.push_back(p);
  }

  {
    Profile p{};
    p.name = "Sony TV (common)";
    p.proto = decode_type_t::SONY;
    p.bits = 12;
    p.code[Power] = 0x0A90u; p.has[Power] = true;
    p.code[Mute]  = 0x0290u; p.has[Mute]  = true;
    p.code[VolUp] = 0x0490u; p.has[VolUp] = true;
    p.code[VolDn] = 0x0C90u; p.has[VolDn] = true;
    p.code[ChUp]  = 0x0090u; p.has[ChUp]  = true;
    p.code[ChDn]  = 0x0890u; p.has[ChDn]  = true;
    s_profiles.push_back(p);
  }

  {
    Profile p{};
    p.name = "Xbox 360";
    p.proto = decode_type_t::RC6;
    p.bits = 36;
    p.code[Power] = 0x0C800F740Cu; p.has[Power] = true;  
    s_profiles.push_back(p);
  }

  {
    Profile p{};
    p.name = "Epson Projector";
    p.proto = decode_type_t::EPSON;
    p.bits = 32;
    p.code[Power] = 0xC1AA09F6u; p.has[Power] = true;
    s_profiles.push_back(p);
  }

  {
    Profile p{};
    p.name = "JVC VCR";
    p.proto = decode_type_t::JVC;
    p.bits = 16;
    p.code[Power] = 0xC2B8u; p.has[Power] = true; 
    s_profiles.push_back(p);
  }

  {
    Profile p{};
    p.name = "Denon AVR";
    p.proto = decode_type_t::DENON;
    p.bits = 15;
    p.code[Power] = 0x2278u; p.has[Power] = true;
    s_profiles.push_back(p);
  }

  {
    Profile p{};
    p.name = "Denon AVR";
    p.proto = decode_type_t::DENON;
    p.bits = 48;
    p.code[Power] = 0x2A4C028D6CE3ULL; p.has[Power] = true;
    s_profiles.push_back(p);
  }

  {
    Profile p{};
    p.name = "Panasonic";
    p.proto = decode_type_t::PANASONIC;
    p.bits = 48;
    p.code[Power] = 0x40040190ED7CULL; p.has[Power] = true;
    s_profiles.push_back(p);
  }

  {
    Profile p{};
    p.name = "Pioneer";
    p.proto = decode_type_t::PIONEER;
    p.bits = 64;
    p.code[Power] = 0x55FF00AAAA00FF55ULL; p.has[Power] = true;
    s_profiles.push_back(p);
  }

  {
    Profile p{};
    p.name = "DISH";
    p.proto = decode_type_t::DISH;
    p.bits = 16;
    p.code[Power] = 0x9C00u; p.has[Power] = true;
    s_profiles.push_back(p);
  }

  {
    Profile p{};
    p.name = "GI Cable";
    p.proto = decode_type_t::GICABLE;
    p.bits = 16;
    p.code[Power] = 0x8807u; p.has[Power] = true;
    s_profiles.push_back(p);
  }

  for (auto& p : s_profiles) {
    if (!p.category.length()) p.category = inferCategoryFromName(p.name);
    if (!p.brand.length())    p.brand    = inferBrandFromName(p.name);
  }

  s_lastErr = "Built-in profiles";
}

static bool endsWith(const String& s, const char* suf) {
  int sl = s.length();
  int tl = (int)strlen(suf);
  if (tl > sl) return false;
  return s.substring(sl - tl) == suf;
}

static bool parseProfilesJson(File& f, size_t docCapacity, String* errOut = nullptr) {

  if (docCapacity < 512) docCapacity = 512;
  DynamicJsonDocument doc(docCapacity);
  DeserializationError err = deserializeJson(doc, f);
  if (err) { if (errOut) *errOut = "JSON parse failed"; return false; }

  JsonArray arr = doc["profiles"].as<JsonArray>();
  if (!arr.isNull()) {
    for (JsonVariant v : arr) {
      JsonObject o = v.as<JsonObject>();
      if (o.isNull()) continue;

      const char* nm = o["name"] | "";
      const char* pr = o["protocol"] | "";
      const char* cat = o["category"] | "";
      const char* br  = o["brand"] | "";
      decode_type_t proto = decode_type_t::UNKNOWN;
      if (!protoFromToken(String(pr), proto)) continue;

      Profile p{};
      p.name = String(nm);
      if (!p.name.length()) p.name = String(pr);
      p.category = String(cat);
      p.brand = String(br);
      if (!p.category.length()) p.category = inferCategoryFromName(p.name);
      if (!p.brand.length()) p.brand = inferBrandFromName(p.name);
      p.proto = proto;
      p.bits = (uint16_t)(o["bits"] | ((proto == decode_type_t::SONY) ? 12 : 32));
      p.model = String((const char*)(o["model"] | ""));
      p.fromSd = true;

      JsonObject codes = o["codes"].as<JsonObject>();
      if (!codes.isNull()) {
        for (JsonPair kv : codes) {
          String keyName = String(kv.key().c_str());
          KeyId kid;
          if (!keyFromToken(keyName, kid)) continue;

          const char* valStr = kv.value().as<const char*>();
          uint64_t code = 0;
          bool ok = false;
          if (valStr && strlen(valStr)) {
            String s(valStr);
            s.trim();
            if (s.startsWith("0x") || s.startsWith("0X")) {
              code = strtoull(s.c_str() + 2, nullptr, 16);
              ok = true;
            } else {
              code = strtoull(s.c_str(), nullptr, 10);
              ok = true;
            }
          } else if (kv.value().is<uint32_t>()) {
            code = (uint64_t)kv.value().as<uint32_t>();
            ok = true;
          } else if (kv.value().is<uint16_t>()) {
            code = (uint64_t)kv.value().as<uint16_t>();
            ok = true;
          } else if (kv.value().is<uint8_t>()) {
            code = (uint64_t)kv.value().as<uint8_t>();
            ok = true;
          }
          if (!ok) continue;

          p.code[kid] = code;
          p.has[kid] = true;
        }
      }

      s_profiles.push_back(p);
      if ((int)s_profiles.size() >= (int)kMaxProfiles) break;
    }
    return !s_profiles.empty();
  }

  JsonObject o = doc.as<JsonObject>();
  if (!o.isNull() && (o.containsKey("name") || o.containsKey("protocol"))) {
    const char* nm = o["name"] | "";
    const char* pr = o["protocol"] | "";
    const char* cat = o["category"] | "";
    const char* br  = o["brand"] | "";
    decode_type_t proto = decode_type_t::UNKNOWN;
    if (!protoFromToken(String(pr), proto)) { if (errOut) *errOut = "Bad protocol"; return false; }

    Profile p{};
    p.name = String(nm);
    if (!p.name.length()) p.name = String(pr);
    p.category = String(cat);
    p.brand = String(br);
    if (!p.category.length()) p.category = inferCategoryFromName(p.name);
    if (!p.brand.length()) p.brand = inferBrandFromName(p.name);
    p.proto = proto;
    p.bits = (uint16_t)(o["bits"] | ((proto == decode_type_t::SONY) ? 12 : 32));
    p.model = String((const char*)(o["model"] | ""));
    p.fromSd = true;

    JsonObject codes = o["codes"].as<JsonObject>();
    if (!codes.isNull()) {
      for (JsonPair kv : codes) {
        String keyName = String(kv.key().c_str());
        KeyId kid;
        if (!keyFromToken(keyName, kid)) continue;
        const char* valStr = kv.value().as<const char*>();
        if (!valStr || !strlen(valStr)) continue;
        String s(valStr);
        s.trim();
        uint64_t code = 0;
        if (s.startsWith("0x") || s.startsWith("0X")) code = strtoull(s.c_str() + 2, nullptr, 16);
        else code = strtoull(s.c_str(), nullptr, 10);
        p.code[kid] = code;
        p.has[kid] = true;
      }
    }
    s_profiles.push_back(p);
    return true;
  }

  if (errOut) *errOut = "No profiles";
  return false;
}

static int s_sdCount = 0;

// Acrescenta (NAO limpa) os perfis do SD ao vetor atual. Marca fromSd=true no parser.
static bool loadProfilesFromSd(String* errOut = nullptr) {
  s_sdCount = 0;
  if (!isSDCardAvailable()) { if (errOut) *errOut = "SD nao montado"; return false; }

  if (SD.exists(PROFILES_PATH)) {
    File f = SD.open(PROFILES_PATH, FILE_READ);
    if (f) {
      int before = (int)s_profiles.size();
      String perr;
      parseProfilesJson(f, 8192, &perr);
      f.close();
      s_sdCount += (int)s_profiles.size() - before;
    }
  }

  if (SD.exists(PROFILES_DIR)) {
    File d = SD.open(PROFILES_DIR);
    if (d) {
      for (;;) {
        File f = d.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
          String name = String(f.name());
          if (endsWith(name, ".json")) {
            int before = (int)s_profiles.size();
            String perr;
            parseProfilesJson(f, 2048, &perr);
            s_sdCount += (int)s_profiles.size() - before;
          }
        }
        f.close();
        if ((int)s_profiles.size() >= (int)kMaxProfiles) break;
      }
      d.close();
    }
  }

  if (s_sdCount == 0) { if (errOut) *errOut = "Nenhum perfil no SD"; return false; }
  if (errOut) *errOut = String(s_sdCount) + " do SD";
  return true;
}

// Carrega SEMPRE os embutidos e, por cima, acrescenta os do SD (cada um com sua tag).
static void refreshProfiles() {
  s_profiles.clear();
  loadBuiltinProfiles();            // fromSd = false

  String sderr;
  s_loadedFromSd = loadProfilesFromSd(&sderr);
  s_lastErr = s_loadedFromSd ? sderr : (String("SD: ") + sderr);

  if (s_profileIdx < 0 || s_profileIdx >= (int)s_profiles.size()) s_profileIdx = 0;
}

// ============================================================================
// Universal Controller - fluxo de 3 telas: Lista -> (Detalhes | Controle)
// ============================================================================

static constexpr const char* RECENT_PATH = "/ir_profiles/recent.txt";

enum class Screen : uint8_t { List, Details, Control };
static Screen s_screen = Screen::List;

static std::vector<int> s_order;           // todos os perfis, ordenados
static std::vector<String> s_recentNames;  // persistido no SD, mais recente primeiro (<=3)
static std::vector<int> s_recent;          // indices resolvidos dos recentes

struct ListRow { int profileIdx; bool header; const char* label; };
static std::vector<ListRow> s_rows;
static int s_sel = 0;         // indice em s_rows (nunca um header)
static int s_top = 0;         // primeira linha visivel
static int s_selProfile = 0;  // perfil escolhido para Detalhes
static int s_navSel = 0;      // botao do controle selecionado (tela Control)

static constexpr int LIST_ROW_H = 20;
static int listTopY()  { return kBodyTop + 6; }
static int listBottom(){ return irContentBottom() - 4; }
static int listRows()  {
  int r = (listBottom() - listTopY()) / LIST_ROW_H;
  return r < 1 ? 1 : r;
}

// ---------- recentes (persistencia no SD) ----------
static void loadRecentNames() {
  s_recentNames.clear();
  if (!isSDCardAvailable()) return;
  File f = SD.open(RECENT_PATH, FILE_READ);
  if (!f) return;
  while (f.available() && (int)s_recentNames.size() < 3) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) s_recentNames.push_back(line);
  }
  f.close();
}

static void saveRecentNames() {
  if (!isSDCardAvailable()) return;
  if (!SD.exists(PROFILES_DIR)) SD.mkdir(PROFILES_DIR);
  if (SD.exists(RECENT_PATH)) SD.remove(RECENT_PATH);
  File f = SD.open(RECENT_PATH, FILE_WRITE);
  if (!f) return;
  for (auto& n : s_recentNames) f.println(n);
  f.close();
}

static void pushRecent(const String& name) {
  if (!name.length()) return;
  for (int i = 0; i < (int)s_recentNames.size(); i++) {
    if (s_recentNames[i] == name) { s_recentNames.erase(s_recentNames.begin() + i); break; }
  }
  s_recentNames.insert(s_recentNames.begin(), name);
  while ((int)s_recentNames.size() > 3) s_recentNames.pop_back();
  saveRecentNames();
}

// ---------- ordenacao / montagem de linhas ----------
static void rebuildOrder() {
  s_order.clear();
  for (int i = 0; i < (int)s_profiles.size(); i++) s_order.push_back(i);
  std::sort(s_order.begin(), s_order.end(), [](int a, int b) {
    const Profile& pa = s_profiles[a];
    const Profile& pb = s_profiles[b];
    auto up = [](String s) { s.toUpperCase(); return s; };
    String x, y;
    x = up(pa.brand);    y = up(pb.brand);    if (x != y) return x < y;   // fabricante
    x = up(pa.category); y = up(pb.category); if (x != y) return x < y;   // tipo
    x = up(pa.model);    y = up(pb.model);    if (x != y) return x < y;   // modelo
    x = up(pa.name);     y = up(pb.name);     return x < y;               // nome
  });
}

static void rebuildRecent() {
  s_recent.clear();
  for (auto& n : s_recentNames) {
    for (int i = 0; i < (int)s_profiles.size(); i++) {
      if (s_profiles[i].name == n) { s_recent.push_back(i); break; }
    }
    if ((int)s_recent.size() >= 3) break;
  }
}

static int firstSelectable() {
  for (int i = 0; i < (int)s_rows.size(); i++) if (!s_rows[i].header) return i;
  return 0;
}

static void rebuildRows() {
  s_rows.clear();
  if (!s_recent.empty()) {
    s_rows.push_back({ -1, true, "Recentes" });
    for (int idx : s_recent) s_rows.push_back({ idx, false, nullptr });
    s_rows.push_back({ -1, true, "Todos" });
  }
  for (int idx : s_order) s_rows.push_back({ idx, false, nullptr });
  if (s_sel < 0 || s_sel >= (int)s_rows.size() || s_rows[s_sel].header) s_sel = firstSelectable();
}

static void moveSel(int dir) {
  int n = (int)s_rows.size();
  if (n == 0) return;
  int i = s_sel + dir;
  while (i >= 0 && i < n && s_rows[i].header) i += dir;
  if (i < 0 || i >= n) return;   // mantem se sairia da lista
  s_sel = i;
}

static void ensureVisible() {
  int rows = listRows();
  if (s_sel < s_top) s_top = s_sel;
  if (s_sel >= s_top + rows) s_top = s_sel - rows + 1;
  if (s_top < 0) s_top = 0;
}

// ---------- desenho ----------
static void drawToolbar() {
  tft.fillRect(0, kToolbarY, 240, kToolbarH, UI_FG);
  tft.drawBitmap(kIconBackX, kToolbarY, bitmap_icon_go_back, kIconSize, kIconSize, UI_ICON);
  if (s_screen != Screen::Details) {
    tft.drawBitmap(kIconReloadX, kToolbarY, bitmap_icon_undo, kIconSize, kIconSize, UI_ICON);
  }
  tft.drawFastHLine(0, kToolbarBottom, 240, UI_LINE);
}

static void drawList() {
  tft.fillRect(0, kBodyTop, 240, irContentBottom() - kBodyTop, FEATURE_BG);
  tft.setTextFont(1);
  tft.setTextSize(1);

  if (s_rows.empty()) {
    tft.setTextColor(UI_WARN, FEATURE_BG);
    tft.setCursor(kPadX, listTopY() + 4);
    tft.print(s_lastErr.length() ? s_lastErr : "Sem controles");
    return;
  }

  ensureVisible();
  const int rows = listRows();
  const int baseY = listTopY();
  const int innerW = 240 - 2 * kPadX;

  for (int r = 0; r < rows; r++) {
    const int ri = s_top + r;
    if (ri >= (int)s_rows.size()) break;
    const ListRow& row = s_rows[ri];
    const int ry = baseY + r * LIST_ROW_H;

    if (row.header) {
      tft.setTextColor(UI_ICON, FEATURE_BG);
      tft.setCursor(kPadX, ry + 6);
      tft.print(row.label);
      tft.drawFastHLine(kPadX, ry + LIST_ROW_H - 2, innerW, UI_LINE);
      continue;
    }

    const Profile& p = s_profiles[row.profileIdx];
    const bool sel = (ri == s_sel);
    const uint16_t bg = sel ? UI_FG : FEATURE_BG;
    const uint16_t fg = sel ? UI_ICON : UI_TEXT;
    tft.fillRect(kPadX, ry, innerW, LIST_ROW_H - 1, bg);

    const char* tag = p.fromSd ? "SD" : "BI";
    const int tagW = tft.textWidth(tag, 1);
    tft.setTextColor(p.fromSd ? UI_OK : UI_DIM_TEXT, bg);
    tft.setCursor(kPadX + innerW - tagW - 2, ry + 6);
    tft.print(tag);

    tft.setTextColor(fg, bg);
    tft.setCursor(kPadX + 4, ry + 6);
    tft.print(truncateWithEllipsis(p.name, innerW - tagW - 14));
  }
}

static void drawDetails() {
  tft.fillRect(0, kBodyTop, 240, irContentBottom() - kBodyTop, FEATURE_BG);
  if (s_selProfile < 0 || s_selProfile >= (int)s_profiles.size()) return;
  const Profile& p = s_profiles[s_selProfile];

  tft.setTextFont(1);
  tft.setTextSize(1);
  int y = kBodyTop + 8;
  const int step = 18;
  const int valX = kPadX + 72;

  auto line = [&](const char* label, const String& val, uint16_t vc) {
    tft.setTextColor(UI_DIM_TEXT, FEATURE_BG);
    tft.setCursor(kPadX, y);
    tft.print(label);
    tft.setTextColor(vc, FEATURE_BG);
    tft.setCursor(valX, y);
    tft.print(truncateWithEllipsis(val.length() ? val : String("-"), 240 - valX - kPadX));
    y += step;
  };

  int nk = 0;
  for (int i = 0; i < (int)KeyCount; i++) if (p.has[i]) nk++;

  line("Nome:",      p.name, UI_TEXT);
  line("Modelo:",    p.model, UI_TEXT);
  line("Marca:",     p.brand, UI_TEXT);
  line("Tipo:",      p.category, UI_TEXT);
  line("Protocolo:", String(typeToString(p.proto)), UI_TEXT);
  line("Bits:",      String((unsigned)p.bits), UI_TEXT);
  line("Fonte:",     p.fromSd ? String("SD card") : String("Built-in"), p.fromSd ? UI_OK : UI_DIM_TEXT);
  line("Teclas:",    String(nk) + "/" + String((int)KeyCount), UI_TEXT);
}

static void layoutKeyButtons() {
  const int top = kBodyTop + 20;
  const int bottom = irContentBottom() - 6;
  const int gap = 6;
  const int areaX = kInfoBoxX;
  const int areaW = kInfoBoxW;

  // Linha de cima: PWR | HOME | BACK
  const int topBtnH = 32;
  const int topBtnW = (areaW - 2 * gap) / 3;
  const int yTop = top;
  const int x0 = areaX;
  const int x1 = x0 + topBtnW + gap;
  const int x2 = areaX + areaW - topBtnW;
  s_keyBtns[(int)Power] = { (int16_t)x0,(int16_t)yTop,(int16_t)topBtnW,(int16_t)topBtnH,keyLabel(Power),FeatureUI::ButtonStyle::Secondary,true };
  s_keyBtns[(int)Home]  = { (int16_t)x1,(int16_t)yTop,(int16_t)topBtnW,(int16_t)topBtnH,keyLabel(Home), FeatureUI::ButtonStyle::Secondary,true };
  s_keyBtns[(int)Back]  = { (int16_t)x2,(int16_t)yTop,(int16_t)topBtnW,(int16_t)topBtnH,keyLabel(Back), FeatureUI::ButtonStyle::Secondary,true };

  // Regiao do meio: rockers (3 segmentos) + d-pad
  const int yMid = yTop + topBtnH + 10;
  const int midH = bottom - yMid;
  const int rockerW = 52;
  int rockerH = midH;
  if (rockerH > 132) rockerH = 132;
  const int segGap = 4;
  const int segH = (rockerH - 2 * segGap) / 3;
  const int ry = yMid + (midH - rockerH) / 2;

  const int volX = areaX;
  const int chX  = areaX + areaW - rockerW;

  // Volume: VOL+ / MUTE / VOL-
  s_keyBtns[(int)VolUp] = { (int16_t)volX,(int16_t)ry,                      (int16_t)rockerW,(int16_t)segH,keyLabel(VolUp),FeatureUI::ButtonStyle::Secondary,true };
  s_keyBtns[(int)Mute]  = { (int16_t)volX,(int16_t)(ry + segH + segGap),    (int16_t)rockerW,(int16_t)segH,keyLabel(Mute), FeatureUI::ButtonStyle::Secondary,true };
  s_keyBtns[(int)VolDn] = { (int16_t)volX,(int16_t)(ry + 2*(segH + segGap)),(int16_t)rockerW,(int16_t)segH,keyLabel(VolDn),FeatureUI::ButtonStyle::Secondary,true };

  // Canal: CH+ / GUIDE / CH-
  s_keyBtns[(int)ChUp]  = { (int16_t)chX,(int16_t)ry,                       (int16_t)rockerW,(int16_t)segH,keyLabel(ChUp), FeatureUI::ButtonStyle::Secondary,true };
  s_keyBtns[(int)Guide] = { (int16_t)chX,(int16_t)(ry + segH + segGap),     (int16_t)rockerW,(int16_t)segH,keyLabel(Guide),FeatureUI::ButtonStyle::Secondary,true };
  s_keyBtns[(int)ChDn]  = { (int16_t)chX,(int16_t)(ry + 2*(segH + segGap)), (int16_t)rockerW,(int16_t)segH,keyLabel(ChDn), FeatureUI::ButtonStyle::Secondary,true };

  // D-pad central
  const int dpadSize = 96;
  const int dpadX = (240 - dpadSize) / 2;
  const int dpadY = yMid + (midH - dpadSize) / 2;
  const int unit = 30;
  const int okSize = 40;
  const int okX = dpadX + (dpadSize - okSize) / 2;
  const int okY = dpadY + (dpadSize - okSize) / 2;
  s_keyBtns[(int)Ok]    = { (int16_t)okX,(int16_t)okY,(int16_t)okSize,(int16_t)okSize,keyLabel(Ok),FeatureUI::ButtonStyle::Secondary,true };
  s_keyBtns[(int)Up]    = { (int16_t)(dpadX + (dpadSize - unit)/2),(int16_t)dpadY,(int16_t)unit,(int16_t)unit,keyLabel(Up),FeatureUI::ButtonStyle::Secondary,true };
  s_keyBtns[(int)Down]  = { (int16_t)(dpadX + (dpadSize - unit)/2),(int16_t)(dpadY + dpadSize - unit),(int16_t)unit,(int16_t)unit,keyLabel(Down),FeatureUI::ButtonStyle::Secondary,true };
  s_keyBtns[(int)Left]  = { (int16_t)dpadX,(int16_t)(dpadY + (dpadSize - unit)/2),(int16_t)unit,(int16_t)unit,keyLabel(Left),FeatureUI::ButtonStyle::Secondary,true };
  s_keyBtns[(int)Right] = { (int16_t)(dpadX + dpadSize - unit),(int16_t)(dpadY + (dpadSize - unit)/2),(int16_t)unit,(int16_t)unit,keyLabel(Right),FeatureUI::ButtonStyle::Secondary,true };
}

static void drawControl() {
  tft.fillRect(0, kBodyTop, 240, irContentBottom() - kBodyTop, FEATURE_BG);
  if (s_profileIdx < 0 || s_profileIdx >= (int)s_profiles.size()) return;
  const Profile& p = s_profiles[s_profileIdx];

  // Titulo (nome do controle)
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(UI_ICON, FEATURE_BG);
  tft.setCursor(kPadX, kBodyTop + 5);
  tft.print(truncateWithEllipsis(p.name, 240 - 2 * kPadX));

  layoutKeyButtons();
  for (int i = 0; i < (int)KeyCount; i++) s_keyBtns[i].disabled = !p.has[i];
  for (int i = 0; i < (int)KeyCount; i++) drawKeyButton((KeyId)i, i == s_navSel);
}

static void drawFooter() {
  if (s_screen == Screen::List)
    setTouchNavLabels("Voltar", "Baixo", "Load", "Cima", "Detalhes");
  else if (s_screen == Screen::Details)
    setTouchNavLabels("Voltar", "", "", "", "");
  else
    setTouchNavLabels("Voltar", "", "OK", "", "");
  irRedrawNavChrome();
}

static void drawAll() {
  irClearBody(FEATURE_BG);
  drawStatusBar(readBatteryVoltage(), true);
  drawToolbar();
  if (s_screen == Screen::List)         drawList();
  else if (s_screen == Screen::Details) drawDetails();
  else                                  drawControl();
  drawFooter();
  s_uiDrawn = true;
}

// ---------- acoes ----------
static void sendKey(KeyId k) {
  if (s_profiles.empty()) return;
  if (s_profileIdx < 0 || s_profileIdx >= (int)s_profiles.size()) return;
  const Profile& p = s_profiles[s_profileIdx];
  if (!p.has[k]) { showNotification("IR", "Tecla nao disponivel neste controle"); return; }
  s_send.begin();
  uint16_t repeat = 0;
  if (p.proto == decode_type_t::SONY || p.proto == decode_type_t::SONY_38K) repeat = 2;
  if (!s_send.send(p.proto, p.code[k], p.bits, repeat))
    showNotification("IR", "Protocolo nao habilitado no build");
}

static void doReload() {
  refreshProfiles();
  rebuildOrder();
  rebuildRecent();
  rebuildRows();
  s_top = 0;
  s_uiDrawn = false;
}

static void goList() {
  s_screen = Screen::List;
  s_uiDrawn = false;
}

static void openDetails() {
  if (s_rows.empty()) return;
  const ListRow& row = s_rows[s_sel];
  if (row.header || row.profileIdx < 0) return;
  s_selProfile = row.profileIdx;
  s_screen = Screen::Details;
  s_uiDrawn = false;
}

static void loadSelected() {
  if (s_rows.empty()) return;
  const ListRow& row = s_rows[s_sel];
  if (row.header || row.profileIdx < 0) return;
  s_profileIdx = row.profileIdx;
  pushRecent(s_profiles[s_profileIdx].name);
  rebuildRecent();
  rebuildRows();
  s_navSel = 0;
  for (int i = 0; i < (int)KeyCount; i++)
    if (s_profiles[s_profileIdx].has[i]) { s_navSel = i; break; }
  s_screen = Screen::Control;
  s_uiDrawn = false;
}

static void ctrlGo(int ni) {
  if (ni < 0 || ni == s_navSel) return;
  const int o = s_navSel;
  s_navSel = ni;
  drawKeyButton((KeyId)o, false);
  drawKeyButton((KeyId)s_navSel, true);
}

// Adjacencias fixas do controle da TV (dir: 0=cima,1=baixo,2=esq,3=dir).
// Retorna a tecla alvo, ou -1 para usar a navegacao generica.
static int tvNavTarget(KeyId k, int dir) {
  switch (k) {
    case VolUp: if (dir == 1) return Mute;  if (dir == 3) return Left;  break;
    case Mute:  if (dir == 1) return VolDn; if (dir == 0) return VolUp; if (dir == 3) return Left;  break;
    case VolDn: if (dir == 0) return Mute;  if (dir == 3) return Left;  break;
    case ChUp:  if (dir == 1) return Guide; if (dir == 2) return Right; break;
    case Guide: if (dir == 1) return ChDn;  if (dir == 0) return ChUp;  if (dir == 2) return Right; break;
    case ChDn:  if (dir == 0) return Guide; if (dir == 2) return Right; break;
    case Up:    if (dir == 0) return Home; if (dir == 1) return Ok;   if (dir == 2) return Left; if (dir == 3) return Right; break;
    case Down:  if (dir == 0) return Ok;   if (dir == 1) return Down; if (dir == 2) return Left; if (dir == 3) return Right; break;
    default: break;
  }
  return -1;
}

static void ctrlNav(int dir) {
  int t = tvNavTarget((KeyId)s_navSel, dir);
  if (t < 0 || s_keyBtns[t].disabled) t = irNavSelect(s_keyBtns, (int)KeyCount, s_navSel, dir);
  ctrlGo(t);
}

static void goList();
static void ctrlLeft() {
  int t = tvNavTarget((KeyId)s_navSel, 2);
  if (t >= 0 && !s_keyBtns[t].disabled) { ctrlGo(t); return; }
  const int ni = irNavSelect(s_keyBtns, (int)KeyCount, s_navSel, 2);
  if (ni != s_navSel) { ctrlGo(ni); return; }
  if (irIsLeftEdge(s_keyBtns, (int)KeyCount, s_navSel)) goList();  // so volta na coluna mais a esq
}

static void ctrlActivate() {
  if (s_navSel < 0 || s_navSel >= (int)KeyCount) return;
  if (s_keyBtns[s_navSel].disabled) return;
  sendKey((KeyId)s_navSel);
}

static bool handleToolbarTouch(int x, int y) {
  if (y <= kToolbarY || y >= kToolbarBottom) return false;
  if (x > kIconBackX && x < kIconBackX + kIconSize) {
    if (s_screen == Screen::List) feature_exit_requested = true;
    else goList();
    return true;
  }
  if (s_screen != Screen::Details && x > kIconReloadX && x < kIconReloadX + kIconSize) {
    doReload();
    return true;
  }
  return false;
}

void setup() {
  setTouchButtonInputEnabled(true);
  s_send.begin();
  s_screen = Screen::List;
  s_profileIdx = 0;
  s_uiDrawn = false;
  loadRecentNames();
  refreshProfiles();
  rebuildOrder();
  rebuildRecent();
  rebuildRows();
  s_sel = firstSelectable();
  s_top = 0;
  drawAll();
}

void loop() {
  if (feature_active && feature_exit_requested) {
    return;
  }

  if (!s_uiDrawn) drawAll();
  maintainTouchNavBar();

  if (s_screen == Screen::List) {
    if (isButtonPressedEdge(BTN_UP))     { moveSel(-1); drawList(); }
    if (isButtonPressedEdge(BTN_DOWN))   { moveSel(+1); drawList(); }
    if (isButtonPressedEdge(BTN_LEFT))   { feature_exit_requested = true; }   // volta/sai
    if (isButtonPressedEdge(BTN_RIGHT))  { openDetails(); }
    if (isButtonPressedEdge(BTN_SELECT)) { loadSelected(); }                  // meio = Load
  } else if (s_screen == Screen::Details) {
    if (isButtonPressedEdge(BTN_LEFT))   { goList(); }
  } else { // Control: setas movem (espacial); esq no canto = Voltar; meio aciona
    const bool nUp = isButtonPressedEdge(BTN_UP);
    const bool nDn = isButtonPressedEdge(BTN_DOWN);
    const bool nRt = isButtonPressedEdge(BTN_RIGHT);
    const bool nLf = isButtonPressedEdge(BTN_LEFT);
    const bool nOk = isButtonPressedEdge(BTN_SELECT);
    static uint32_t lastNavMs = 0;
    const uint32_t nowNav = millis();
    if ((uint32_t)(nowNav - lastNavMs) > 160) {   // anti-bounce (1 acao por toque)
      if      (nUp) { ctrlNav(0);     lastNavMs = nowNav; }
      else if (nDn) { ctrlNav(1);     lastNavMs = nowNav; }
      else if (nRt) { ctrlNav(3);     lastNavMs = nowNav; }
      else if (nLf) { ctrlLeft();     lastNavMs = nowNav; }
      else if (nOk) { ctrlActivate(); lastNavMs = nowNav; }
    }
  }

  // Toque na tela
  static uint32_t lastTouchMs = 0;
  static bool touchWas = false;
  const uint32_t now = millis();
  const bool touchNow = isTouchDownDismiss();
  constexpr uint32_t kTouchDebounceMs = 280;
  if (touchNow && !touchWas) {
    int x, y;
    touchWas = true;
    if (!readTouchXY(x, y)) return;
    if ((uint32_t)(now - lastTouchMs) < kTouchDebounceMs) return;
    lastTouchMs = now;

    if (handleToolbarTouch(x, y)) return;

    if (s_screen == Screen::List) {
      const int rows = listRows();
      const int ty = listTopY();
      if (y >= ty && y < ty + rows * LIST_ROW_H) {
        const int ri = s_top + (y - ty) / LIST_ROW_H;
        if (ri >= 0 && ri < (int)s_rows.size() && !s_rows[ri].header) {
          if (ri == s_sel) loadSelected();           // tocar de novo na selecionada = Load
          else { s_sel = ri; drawList(); }
        }
      }
    } else if (s_screen == Screen::Control) {
      const int k = FeatureUI::hit(s_keyBtns, (int)KeyCount, x, y);
      if (k >= 0 && !s_keyBtns[k].disabled) {
        if (s_navSel != k) { drawKeyButton((KeyId)s_navSel, false); s_navSel = k; }
        drawKeyButton((KeyId)k, true);
        sendKey((KeyId)k);
      }
    }
  }
  if (!touchNow) touchWas = false;

  delay(10);
}

}  // namespace IRUniversalController


// ============================================================================
// Universal Controller A/C - mesmo fluxo (Lista -> Detalhes | Controle),
// mas state-based. Por enquanto so LG (via IRLgAc); estrutura pronta p/ SD.
// ============================================================================
namespace IRUniversalAC {

static constexpr const char* AC_DIR    = "/ir_ac";
static constexpr const char* AC_RECENT = "/ir_ac/recent.txt";

static constexpr int16_t kToolbarY = 20;
static constexpr int16_t kToolbarH = 16;
static constexpr int16_t kIconSize = 16;
static constexpr int kToolbarBottom = kToolbarY + kToolbarH;  // 36
static constexpr int kBodyTop = kToolbarBottom + 1;           // 37
static constexpr int kPadX = 10;
static constexpr int kIconBackX = 10;
static constexpr int kIconReloadX = 210;
static constexpr int LIST_ROW_H = 20;

static IRLgAc s_lg(IR_TX_PIN);
static IRsend s_rawSend(IR_TX_PIN);   // p/ comandos diretos (swing por posicao, luz)
static int s_swingIdx = -1;           // -1 = nenhuma posicao enviada ainda

static uint32_t parseHex32(const char* s) {
  if (!s) return 0;
  String t(s); t.trim();
  if (t.startsWith("0x") || t.startsWith("0X")) return (uint32_t)strtoul(t.c_str() + 2, nullptr, 16);
  return (uint32_t)strtoul(t.c_str(), nullptr, 16);
}
static decode_type_t protoFromStr(const String& s, decode_type_t def) {
  String u = s; u.toUpperCase();
  if (u == "LG")  return decode_type_t::LG;
  if (u == "LG2") return decode_type_t::LG2;
  return def;
}

// ---------- modelos LG ----------
static const lg_ac_remote_model_t kLgModels[] = {
  AKB75215403, AKB74955603, AKB73757604, GE6711AR2853M, LG6711A20083V
};
static constexpr int kLgModelCount = sizeof(kLgModels) / sizeof(kLgModels[0]);

static const char* lgModelName(lg_ac_remote_model_t m) {
  switch (m) {
    case AKB75215403:   return "AKB75215403";
    case AKB74955603:   return "AKB74955603";
    case AKB73757604:   return "AKB73757604";
    case GE6711AR2853M: return "GE6711AR2853M";
    case LG6711A20083V: return "LG6711A20083V";
    default:            return "AKB75215403";
  }
}

static lg_ac_remote_model_t lgModelFromString(const String& in) {
  String s = in; s.toUpperCase();
  if (s.indexOf("GE6711") >= 0)   return GE6711AR2853M;
  if (s.indexOf("20083") >= 0)    return LG6711A20083V;
  if (s.indexOf("74955603") >= 0) return AKB74955603;
  if (s.indexOf("73757604") >= 0) return AKB73757604;
  if (s.indexOf("75215403") >= 0) return AKB75215403;
  return AKB75215403;  // default (inclui o AKB75215424 do usuario)
}

static const char* lgProtoName(lg_ac_remote_model_t m) {
  return (m == GE6711AR2853M || m == LG6711A20083V) ? "LG" : "LG2";
}

// ---------- perfis ----------
struct AcProfile {
  String name;
  String brand;
  String model;
  bool fromSd = false;
  lg_ac_remote_model_t lgModel = AKB75215403;
  std::vector<uint32_t> swingCodes;               // posicoes da aba, em ordem
  decode_type_t swingProto = decode_type_t::LG2;  // protocolo p/ enviar swing
  uint32_t lightCode = 0x88C00A6;                 // toggle da luz do display
  decode_type_t lightProto = decode_type_t::LG2;
};

// Preenche codigos padrao (da biblioteca) de swing/luz; o SD pode sobrescrever.
static void setAcDefaults(AcProfile& p) {
  static const uint32_t def[] = {
    kLgAcSwingVLowest, kLgAcSwingVLow, kLgAcSwingVMiddle,
    kLgAcSwingVUpperMiddle, kLgAcSwingVHigh, kLgAcSwingVHighest,
    kLgAcSwingVSwing, kLgAcSwingVOff
  };
  p.swingCodes.assign(def, def + (sizeof(def) / sizeof(def[0])));
  p.swingProto = decode_type_t::LG2;
  p.lightCode = kLgAcLightToggle;
  p.lightProto = decode_type_t::LG2;
}
static std::vector<AcProfile> s_profs;
static int s_profIdx = 0;
static int s_sdCount = 0;
static bool s_loadedSd = false;
static String s_lastErr;

static void loadBuiltinAc() {
  auto add = [&](const char* n, const char* model) {
    AcProfile p;
    p.name = n; p.brand = "LG"; p.model = model; p.fromSd = false;
    p.lgModel = lgModelFromString(model);
    setAcDefaults(p);
    s_profs.push_back(p);
  };
  add("LG Dual Inverter",        "AKB75215403");
  add("LG (AKB75215403)",        "AKB75215403");
  add("LG (AKB74955603)",        "AKB74955603");
  add("LG (AKB73757604)",        "AKB73757604");
  add("LG (GE6711AR2853M)",      "GE6711AR2853M");
  add("LG (LG6711A20083V)",      "LG6711A20083V");
}

static bool loadAcFromSd() {
  s_sdCount = 0;
  if (!isSDCardAvailable()) return false;
  if (!SD.exists(AC_DIR)) return false;
  File d = SD.open(AC_DIR);
  if (!d) return false;
  for (;;) {
    File f = d.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String name = String(f.name());
      if (name.endsWith(".json")) {
        DynamicJsonDocument doc(2048);
        if (!deserializeJson(doc, f)) {
          JsonObject o = doc.as<JsonObject>();
          String brand = String((const char*)(o["brand"] | ""));
          brand.toUpperCase();
          if (brand.length() == 0 || brand == "LG") {  // so LG por enquanto
            AcProfile p;
            p.name  = String((const char*)(o["name"]  | ""));
            p.brand = "LG";
            p.model = String((const char*)(o["model"] | "AKB75215403"));
            p.fromSd = true;
            p.lgModel = lgModelFromString(p.model);
            if (!p.name.length()) p.name = String("LG ") + p.model;
            setAcDefaults(p);
            p.swingProto = protoFromStr(String((const char*)(o["swing_proto"] | "")), p.swingProto);
            p.lightProto = protoFromStr(String((const char*)(o["light_proto"] | "")), p.lightProto);
            if (o.containsKey("light")) { uint32_t lc = parseHex32(o["light"] | ""); if (lc) p.lightCode = lc; }
            JsonArray sw = o["swing"].as<JsonArray>();
            if (!sw.isNull()) {
              std::vector<uint32_t> codes;
              for (JsonVariant v : sw) { uint32_t c = parseHex32(v.as<const char*>()); if (c) codes.push_back(c); }
              if (!codes.empty()) p.swingCodes = codes;
            }
            s_profs.push_back(p);
            s_sdCount++;
          }
        }
      }
    }
    f.close();
    if ((int)s_profs.size() >= 200) break;
  }
  d.close();
  return s_sdCount > 0;
}

static void refreshAc() {
  s_profs.clear();
  loadBuiltinAc();
  s_loadedSd = loadAcFromSd();
  s_lastErr = s_loadedSd ? (String(s_sdCount) + " do SD") : String("SD: nenhum");
  if (s_profIdx < 0 || s_profIdx >= (int)s_profs.size()) s_profIdx = 0;
}

// ---------- estado do A/C ----------
struct AcState {
  bool power = false;
  uint8_t mode = kLgAcCool;
  uint8_t temp = 22;
  uint8_t fan = kLgAcFanAuto;
  bool swingV = false;
  bool light = true;
};
static AcState s_st;

static const uint8_t kFanCycle[] = { kLgAcFanAuto, kLgAcFanLow, kLgAcFanMedium, kLgAcFanMax };
static constexpr int kFanCycleCount = sizeof(kFanCycle) / sizeof(kFanCycle[0]);

static const char* fanName(uint8_t f) {
  switch (f) {
    case kLgAcFanAuto:   return "Auto";
    case kLgAcFanLow:    return "Low";
    case kLgAcFanMedium: return "Med";
    case kLgAcFanMax:    return "Max";
    case kLgAcFanLowest: return "Min";
    case kLgAcFanHigh:   return "High";
    default:             return "?";
  }
}
static const char* modeName(uint8_t m) {
  switch (m) {
    case kLgAcCool: return "Cool";
    case kLgAcDry:  return "Dry";
    case kLgAcFan:  return "Fan";
    case kLgAcAuto: return "Auto";
    case kLgAcHeat: return "Heat";
    default:        return "?";
  }
}

static void acApplyAndSend() {
  if (s_profs.empty()) return;
  const AcProfile& p = s_profs[s_profIdx];
  s_lg.setModel(p.lgModel);
  s_lg.setPower(s_st.power);
  s_lg.setMode(s_st.mode);
  s_lg.setTemp(s_st.temp);
  s_lg.setFan(s_st.fan);
  // Swing e luz sao enviados por comando direto (ver acButton), nao aqui.
  s_lg.send();
}

// ---------- botoes do controle ----------
enum AcBtn { B_PWR, B_MODE, B_TEMPUP, B_TEMPDN, B_SWING, B_SPEED, B_LIGHT, B_COUNT };
static FeatureUI::Button s_btn[B_COUNT];

static void acButton(int b) {
  switch (b) {
    case B_PWR:    s_st.power = !s_st.power; break;
    case B_MODE:   s_st.power = true; s_st.mode = (uint8_t)((s_st.mode + 1) % 5); break;
    case B_TEMPUP: s_st.power = true; if (s_st.temp < kLgAcMaxTemp) s_st.temp++; break;
    case B_TEMPDN: s_st.power = true; if (s_st.temp > kLgAcMinTemp) s_st.temp--; break;
    case B_SWING: {
      s_st.power = true;
      const AcProfile& p = s_profs[s_profIdx];
      if (p.swingCodes.empty()) return;
      s_swingIdx = (s_swingIdx + 1) % (int)p.swingCodes.size();
      s_rawSend.send(p.swingProto, p.swingCodes[s_swingIdx], kLgBits);
      return;  // comando direto, nao reenvia o estado completo
    }
    case B_SPEED: {
      s_st.power = true;
      int i = 0; for (; i < kFanCycleCount; i++) if (kFanCycle[i] == s_st.fan) break;
      if (i >= kFanCycleCount) i = -1;
      s_st.fan = kFanCycle[(i + 1) % kFanCycleCount];
      break;
    }
    case B_LIGHT: {
      const AcProfile& p = s_profs[s_profIdx];
      if (p.lightCode) s_rawSend.send(p.lightProto, p.lightCode, kLgBits);
      return;  // toggle direto da luz do display
    }
  }
  acApplyAndSend();
}

// ---------- telas: estado de lista ----------
enum class AScreen : uint8_t { List, Details, Control };
static AScreen s_scr = AScreen::List;

static std::vector<int> s_order;
static std::vector<String> s_recentNames;
static std::vector<int> s_recent;
struct Row { int idx; bool header; const char* label; };
static std::vector<Row> s_rows;
static int s_sel = 0, s_top = 0, s_selProf = 0;
static int s_navSel = 0;   // botao do controle selecionado (tela Control)
static bool s_drawn = false;

static int listTopY()   { return kBodyTop + 6; }
static int listBottom() { return irContentBottom() - 4; }
static int listRows()   { int r = (listBottom() - listTopY()) / LIST_ROW_H; return r < 1 ? 1 : r; }

static String acTrunc(const String& s, int maxPx) {
  if (maxPx <= 0 || !s.length()) return s;
  if ((int)tft.textWidth(s, 1) <= maxPx) return s;
  const int ell = tft.textWidth("...", 1);
  String o = s;
  while (o.length() && (int)tft.textWidth(o, 1) + ell > maxPx) o.remove(o.length() - 1);
  return o + "...";
}

// ---------- recentes ----------
static void loadRecentNames() {
  s_recentNames.clear();
  if (!isSDCardAvailable()) return;
  File f = SD.open(AC_RECENT, FILE_READ);
  if (!f) return;
  while (f.available() && (int)s_recentNames.size() < 3) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) s_recentNames.push_back(line);
  }
  f.close();
}
static void saveRecentNames() {
  if (!isSDCardAvailable()) return;
  if (!SD.exists(AC_DIR)) SD.mkdir(AC_DIR);
  if (SD.exists(AC_RECENT)) SD.remove(AC_RECENT);
  File f = SD.open(AC_RECENT, FILE_WRITE);
  if (!f) return;
  for (auto& n : s_recentNames) f.println(n);
  f.close();
}
static void pushRecent(const String& name) {
  if (!name.length()) return;
  for (int i = 0; i < (int)s_recentNames.size(); i++)
    if (s_recentNames[i] == name) { s_recentNames.erase(s_recentNames.begin() + i); break; }
  s_recentNames.insert(s_recentNames.begin(), name);
  while ((int)s_recentNames.size() > 3) s_recentNames.pop_back();
  saveRecentNames();
}

static void rebuildOrder() {
  s_order.clear();
  for (int i = 0; i < (int)s_profs.size(); i++) s_order.push_back(i);
  std::sort(s_order.begin(), s_order.end(), [](int a, int b) {
    const AcProfile& pa = s_profs[a];
    const AcProfile& pb = s_profs[b];
    auto up = [](String s) { s.toUpperCase(); return s; };
    String x, y;
    x = up(pa.brand); y = up(pb.brand); if (x != y) return x < y;
    x = up(pa.model); y = up(pb.model); if (x != y) return x < y;
    x = up(pa.name);  y = up(pb.name);  return x < y;
  });
}
static void rebuildRecent() {
  s_recent.clear();
  for (auto& n : s_recentNames) {
    for (int i = 0; i < (int)s_profs.size(); i++)
      if (s_profs[i].name == n) { s_recent.push_back(i); break; }
    if ((int)s_recent.size() >= 3) break;
  }
}
static int firstSelectable() {
  for (int i = 0; i < (int)s_rows.size(); i++) if (!s_rows[i].header) return i;
  return 0;
}
static void rebuildRows() {
  s_rows.clear();
  if (!s_recent.empty()) {
    s_rows.push_back({ -1, true, "Recentes" });
    for (int idx : s_recent) s_rows.push_back({ idx, false, nullptr });
    s_rows.push_back({ -1, true, "Todos" });
  }
  for (int idx : s_order) s_rows.push_back({ idx, false, nullptr });
  if (s_sel < 0 || s_sel >= (int)s_rows.size() || s_rows[s_sel].header) s_sel = firstSelectable();
}
static void moveSel(int dir) {
  int n = (int)s_rows.size();
  if (n == 0) return;
  int i = s_sel + dir;
  while (i >= 0 && i < n && s_rows[i].header) i += dir;
  if (i < 0 || i >= n) return;
  s_sel = i;
}
static void ensureVisible() {
  int rows = listRows();
  if (s_sel < s_top) s_top = s_sel;
  if (s_sel >= s_top + rows) s_top = s_sel - rows + 1;
  if (s_top < 0) s_top = 0;
}

// ---------- desenho ----------
static void drawToolbar() {
  tft.fillRect(0, kToolbarY, 240, kToolbarH, UI_FG);
  tft.drawBitmap(kIconBackX, kToolbarY, bitmap_icon_go_back, kIconSize, kIconSize, UI_ICON);
  if (s_scr != AScreen::Details)
    tft.drawBitmap(kIconReloadX, kToolbarY, bitmap_icon_undo, kIconSize, kIconSize, UI_ICON);
  tft.drawFastHLine(0, kToolbarBottom, 240, UI_LINE);
}

static void drawList() {
  tft.fillRect(0, kBodyTop, 240, irContentBottom() - kBodyTop, FEATURE_BG);
  tft.setTextFont(1); tft.setTextSize(1);
  if (s_rows.empty()) {
    tft.setTextColor(UI_WARN, FEATURE_BG);
    tft.setCursor(kPadX, listTopY() + 4);
    tft.print("Sem controles A/C");
    return;
  }
  ensureVisible();
  const int rows = listRows();
  const int baseY = listTopY();
  const int innerW = 240 - 2 * kPadX;
  for (int r = 0; r < rows; r++) {
    const int ri = s_top + r;
    if (ri >= (int)s_rows.size()) break;
    const Row& row = s_rows[ri];
    const int ry = baseY + r * LIST_ROW_H;
    if (row.header) {
      tft.setTextColor(UI_ICON, FEATURE_BG);
      tft.setCursor(kPadX, ry + 6);
      tft.print(row.label);
      tft.drawFastHLine(kPadX, ry + LIST_ROW_H - 2, innerW, UI_LINE);
      continue;
    }
    const AcProfile& p = s_profs[row.idx];
    const bool sel = (ri == s_sel);
    const uint16_t bg = sel ? UI_FG : FEATURE_BG;
    const uint16_t fg = sel ? UI_ICON : UI_TEXT;
    tft.fillRect(kPadX, ry, innerW, LIST_ROW_H - 1, bg);
    const char* tag = p.fromSd ? "SD" : "BI";
    const int tagW = tft.textWidth(tag, 1);
    tft.setTextColor(p.fromSd ? UI_OK : UI_DIM_TEXT, bg);
    tft.setCursor(kPadX + innerW - tagW - 2, ry + 6);
    tft.print(tag);
    tft.setTextColor(fg, bg);
    tft.setCursor(kPadX + 4, ry + 6);
    tft.print(acTrunc(p.name, innerW - tagW - 14));
  }
}

static void drawDetails() {
  tft.fillRect(0, kBodyTop, 240, irContentBottom() - kBodyTop, FEATURE_BG);
  if (s_selProf < 0 || s_selProf >= (int)s_profs.size()) return;
  const AcProfile& p = s_profs[s_selProf];
  tft.setTextFont(1); tft.setTextSize(1);
  int y = kBodyTop + 8;
  const int step = 18;
  const int valX = kPadX + 80;
  auto line = [&](const char* label, const String& val, uint16_t vc) {
    tft.setTextColor(UI_DIM_TEXT, FEATURE_BG);
    tft.setCursor(kPadX, y); tft.print(label);
    tft.setTextColor(vc, FEATURE_BG);
    tft.setCursor(valX, y);
    tft.print(acTrunc(val.length() ? val : String("-"), 240 - valX - kPadX));
    y += step;
  };
  line("Nome:",     p.name, UI_TEXT);
  line("Marca:",    p.brand, UI_TEXT);
  line("Modelo:",   String(lgModelName(p.lgModel)), UI_OK);
  line("Protocolo:",String(lgProtoName(p.lgModel)), UI_TEXT);
  line("Temp:",     String(kLgAcMinTemp) + "-" + String(kLgAcMaxTemp) + "C", UI_TEXT);
  line("Modos:",    "Cool/Dry/Fan/Auto/Heat", UI_TEXT);
  line("Fonte:",    p.fromSd ? String("SD card") : String("Built-in"), p.fromSd ? UI_OK : UI_DIM_TEXT);
}

// Desenha um botao do controle AC; selecionado = Primary (vermelho, como o PWR).
static void acDrawBtn(int i, bool sel) {
  const FeatureUI::Button& b = s_btn[i];
  FeatureUI::drawButtonRect(b.x, b.y, b.w, b.h, b.label,
                            sel ? FeatureUI::ButtonStyle::Primary : FeatureUI::ButtonStyle::Secondary,
                            false, b.disabled, 2);
}

static void acLayout() {
  const int top = kBodyTop + 24;
  const int bottom = irContentBottom() - 6;
  const int unit = (bottom - top) / 5;
  const int bh = unit - 6;
  const int sideW = 80, centerW = 96;
  const int xL = 8, xR = 240 - 8 - sideW, xC = (240 - centerW) / 2;
  int y = top;
  s_btn[B_PWR]    = { (int16_t)xL,(int16_t)y,(int16_t)sideW,(int16_t)bh,"PWR", FeatureUI::ButtonStyle::Primary,  false };
  s_btn[B_MODE]   = { (int16_t)xR,(int16_t)y,(int16_t)sideW,(int16_t)bh,"MODE",FeatureUI::ButtonStyle::Secondary,false };
  y += unit;
  s_btn[B_TEMPUP] = { (int16_t)xC,(int16_t)y,(int16_t)centerW,(int16_t)bh,"TEMP +",FeatureUI::ButtonStyle::Secondary,false };
  y += unit;
  s_btn[B_TEMPDN] = { (int16_t)xC,(int16_t)y,(int16_t)centerW,(int16_t)bh,"TEMP -",FeatureUI::ButtonStyle::Secondary,false };
  y += unit;
  s_btn[B_SWING]  = { (int16_t)xL,(int16_t)y,(int16_t)sideW,(int16_t)bh,"SWING",FeatureUI::ButtonStyle::Secondary,false };
  s_btn[B_SPEED]  = { (int16_t)xR,(int16_t)y,(int16_t)sideW,(int16_t)bh,"SPEED",FeatureUI::ButtonStyle::Secondary,false };
  y += unit;
  s_btn[B_LIGHT]  = { (int16_t)xC,(int16_t)y,(int16_t)centerW,(int16_t)bh,"LIGHT",FeatureUI::ButtonStyle::Secondary,false };
}

static void drawControl() {
  tft.fillRect(0, kBodyTop, 240, irContentBottom() - kBodyTop, FEATURE_BG);
  if (s_profIdx < 0 || s_profIdx >= (int)s_profs.size()) return;
  const AcProfile& p = s_profs[s_profIdx];
  tft.setTextFont(1); tft.setTextSize(1);
  tft.setTextColor(UI_ICON, FEATURE_BG);
  tft.setCursor(kPadX, kBodyTop + 3);
  tft.print(acTrunc(p.name, 240 - 2 * kPadX));
  String stline = s_st.power
      ? (String(modeName(s_st.mode)) + "  " + String(s_st.temp) + "C  Fan:" + fanName(s_st.fan)
         + (s_swingIdx >= 0 ? (String("  Aba#") + String(s_swingIdx + 1)) : String("")))
      : String("Desligado");
  tft.setTextColor(s_st.power ? UI_OK : UI_DIM_TEXT, FEATURE_BG);
  tft.setCursor(kPadX, kBodyTop + 13);
  tft.print(acTrunc(stline, 240 - 2 * kPadX));
  acLayout();
  for (int i = 0; i < B_COUNT; i++) acDrawBtn(i, i == s_navSel);
}

static void drawFooter() {
  if (s_scr == AScreen::List)
    setTouchNavLabels("Voltar", "Baixo", "Load", "Cima", "Detalhes");
  else if (s_scr == AScreen::Details)
    setTouchNavLabels("Voltar", "", "", "", "");
  else
    setTouchNavLabels("Voltar", "", "OK", "", "");
  irRedrawNavChrome();
}

static void drawAll() {
  irClearBody(FEATURE_BG);
  drawStatusBar(readBatteryVoltage(), true);
  drawToolbar();
  if (s_scr == AScreen::List)         drawList();
  else if (s_scr == AScreen::Details) drawDetails();
  else                                drawControl();
  drawFooter();
  s_drawn = true;
}

// ---------- acoes ----------
static void doReload() {
  refreshAc();
  rebuildOrder();
  rebuildRecent();
  rebuildRows();
  s_top = 0;
  s_drawn = false;
}
static void goList()  { s_scr = AScreen::List; s_drawn = false; }
static void openDetails() {
  if (s_rows.empty()) return;
  const Row& row = s_rows[s_sel];
  if (row.header || row.idx < 0) return;
  s_selProf = row.idx;
  s_scr = AScreen::Details;
  s_drawn = false;
}
static void loadSelected() {
  if (s_rows.empty()) return;
  const Row& row = s_rows[s_sel];
  if (row.header || row.idx < 0) return;
  s_profIdx = row.idx;
  pushRecent(s_profs[s_profIdx].name);
  rebuildRecent(); rebuildRows();
  // reinicia estado ao abrir um controle
  s_st = AcState();
  s_swingIdx = -1;
  s_lg.stateReset();
  s_lg.setModel(s_profs[s_profIdx].lgModel);
  s_navSel = 0;   // PWR
  s_scr = AScreen::Control;
  s_drawn = false;
}

static void ctrlGo(int ni) {
  if (ni < 0 || ni == s_navSel) return;
  const int o = s_navSel;
  s_navSel = ni;
  acDrawBtn(o, false);
  acDrawBtn(s_navSel, true);
}

static void ctrlNav(int dir) {
  ctrlGo(irNavSelect(s_btn, (int)B_COUNT, s_navSel, dir));
}

static void goList();
static void ctrlLeft() {
  const int ni = irNavSelect(s_btn, (int)B_COUNT, s_navSel, 2);
  if (ni != s_navSel) { ctrlGo(ni); return; }
  if (irIsLeftEdge(s_btn, (int)B_COUNT, s_navSel)) goList();  // so volta na coluna mais a esq
}

static void ctrlActivate() {
  if (s_navSel < 0 || s_navSel >= (int)B_COUNT) return;
  acDrawBtn(s_navSel, true);
  acButton(s_navSel);
  delay(90);
  drawControl();   // atualiza linha de estado + mantem selecao
}

static bool handleToolbarTouch(int x, int y) {
  if (y <= kToolbarY || y >= kToolbarBottom) return false;
  if (x > kIconBackX && x < kIconBackX + kIconSize) {
    if (s_scr == AScreen::List) feature_exit_requested = true;
    else goList();
    return true;
  }
  if (s_scr != AScreen::Details && x > kIconReloadX && x < kIconReloadX + kIconSize) {
    doReload();
    return true;
  }
  return false;
}

void setup() {
  setTouchButtonInputEnabled(true);
  s_lg.begin();
  s_rawSend.begin();
  s_scr = AScreen::List;
  s_profIdx = 0;
  s_drawn = false;
  loadRecentNames();
  refreshAc();
  rebuildOrder();
  rebuildRecent();
  rebuildRows();
  s_sel = firstSelectable();
  s_top = 0;
  drawAll();
}

void loop() {
  if (feature_active && feature_exit_requested) {
    return;
  }
  if (!s_drawn) drawAll();
  maintainTouchNavBar();

  if (s_scr == AScreen::List) {
    if (isButtonPressedEdge(BTN_UP))     { moveSel(-1); drawList(); }
    if (isButtonPressedEdge(BTN_DOWN))   { moveSel(+1); drawList(); }
    if (isButtonPressedEdge(BTN_LEFT))   { feature_exit_requested = true; }   // volta/sai
    if (isButtonPressedEdge(BTN_RIGHT))  { openDetails(); }
    if (isButtonPressedEdge(BTN_SELECT)) { loadSelected(); }                  // meio = Load
  } else if (s_scr == AScreen::Details) {
    if (isButtonPressedEdge(BTN_LEFT))   { goList(); }
  } else { // Control: setas movem (espacial); esq no canto = Voltar; meio aciona
    const bool nUp = isButtonPressedEdge(BTN_UP);
    const bool nDn = isButtonPressedEdge(BTN_DOWN);
    const bool nRt = isButtonPressedEdge(BTN_RIGHT);
    const bool nLf = isButtonPressedEdge(BTN_LEFT);
    const bool nOk = isButtonPressedEdge(BTN_SELECT);
    static uint32_t lastNavMs = 0;
    const uint32_t nowNav = millis();
    if ((uint32_t)(nowNav - lastNavMs) > 160) {   // anti-bounce (1 acao por toque)
      if      (nUp) { ctrlNav(0);     lastNavMs = nowNav; }
      else if (nDn) { ctrlNav(1);     lastNavMs = nowNav; }
      else if (nRt) { ctrlNav(3);     lastNavMs = nowNav; }
      else if (nLf) { ctrlLeft();     lastNavMs = nowNav; }
      else if (nOk) { ctrlActivate(); lastNavMs = nowNav; }
    }
  }

  static uint32_t lastTouchMs = 0;
  static bool touchWas = false;
  const uint32_t now = millis();
  const bool touchNow = isTouchDownDismiss();
  constexpr uint32_t kTouchDebounceMs = 280;
  if (touchNow && !touchWas) {
    int x, y;
    touchWas = true;
    if (!readTouchXY(x, y)) return;
    if ((uint32_t)(now - lastTouchMs) < kTouchDebounceMs) return;
    lastTouchMs = now;
    if (handleToolbarTouch(x, y)) return;

    if (s_scr == AScreen::List) {
      const int rows = listRows();
      const int ty = listTopY();
      if (y >= ty && y < ty + rows * LIST_ROW_H) {
        const int ri = s_top + (y - ty) / LIST_ROW_H;
        if (ri >= 0 && ri < (int)s_rows.size() && !s_rows[ri].header) {
          if (ri == s_sel) loadSelected();
          else { s_sel = ri; drawList(); }
        }
      }
    } else if (s_scr == AScreen::Control) {
      const int k = FeatureUI::hit(s_btn, (int)B_COUNT, x, y);
      if (k >= 0) {
        s_navSel = k;
        acDrawBtn(k, true);
        acButton(k);
        delay(90);
        drawControl();   // atualiza linha de estado + botoes
      }
    }
  }
  if (!touchNow) touchWas = false;

  delay(10);
}

}  // namespace IRUniversalAC
