#include "wifi5g.h"

#include <Arduino.h>

#include "config.h"
#include "shared.h"
#include "utils.h"

namespace Wifi5g {

static const int kMaxNets = 40;

struct NetEntry {
  char ssid[33];
  int channel;
  int rssi;
};

static NetEntry s_nets[kMaxNets];
static int s_netCount = 0;
static bool s_inBlock = false;
static bool s_dirty = false;
static uint32_t s_lastScan = 0;
static String s_line = "";

static void drawList() {
  tft.fillRect(0, 26, tft.width(), tft.height() - 26, TFT_BLACK);
  tft.setTextSize(1);
  int y = 30;
  if (s_netCount == 0) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(6, y);
    tft.print("Aguardando scan do C5...");
    s_dirty = false;
    return;
  }
  for (int i = 0; i < s_netCount && y < tft.height() - 8; i++) {
    bool is5g = s_nets[i].channel >= 36;
    tft.setTextColor(is5g ? TFT_GREEN : TFT_WHITE, TFT_BLACK);
    String ssid = String(s_nets[i].ssid);
    if (ssid.length() > 18) ssid = ssid.substring(0, 18);
    String line = ssid + "  CH" + String(s_nets[i].channel) + "  " + String(s_nets[i].rssi) + "dBm";
    tft.setCursor(6, y);
    tft.print(line);
    y += 12;
  }
  s_dirty = false;
}

void wifi5gSetup() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(6, 4);
  tft.print("Wi-Fi 5G");
  tft.drawFastHLine(0, 24, tft.width(), TFT_WHITE);
  s_netCount = 0;
  s_inBlock = false;
  s_lastScan = 0;
  Serial1.setRxBufferSize(4096);            // buffer maior (antes do begin)
  Serial1.begin(115200, SERIAL_8N1, 5, 6);  // UART1: RX=GPIO5, TX=GPIO6 (C5)
  Serial1.println("scan");
  drawList();
}

void wifi5gLoop() {
  if (isButtonPressed(BTN_LEFT)) {
    waitButtonReleased(BTN_LEFT);
    feature_exit_requested = true;
    return;
  }

  while (Serial1.available()) {
    char c = (char)Serial1.read();
    Serial.write(c);  // eco (debug): devolve no TX UART0 para eu ver na COM5
    if (c == '\n') {
      String line = s_line;
      s_line = "";
      line.trim();
      if (line.length() == 0) continue;

      if (line.startsWith("=== SCAN")) {
        s_netCount = 0;
        s_inBlock = true;
        continue;
      }
      if (line.startsWith("=== FIM")) {
        s_inBlock = false;
        s_dirty = true;
        continue;
      }
      if (!s_inBlock) continue;

      int p1 = line.indexOf('|');
      if (p1 <= 0) continue;
      int p2 = line.indexOf('|', p1 + 1);
      if (p2 < 0) continue;
      int p3 = line.indexOf('|', p2 + 1);
      if (p3 < 0) continue;

      if (s_netCount >= kMaxNets) continue;
      NetEntry& n = s_nets[s_netCount];
      line.substring(0, p1).toCharArray(n.ssid, sizeof(n.ssid));
      String ch = line.substring(p1 + 1, p2);
      String rssi = line.substring(p2 + 1, p3);
      n.channel = ch.startsWith("CH") ? ch.substring(2).toInt() : 0;
      n.rssi = rssi.startsWith("RSSI") ? rssi.substring(4).toInt() : 0;
      s_netCount++;
      s_dirty = true;
    } else {
      s_line += c;
    }
  }

  if (s_dirty) drawList();

  updateStatusBar();
  if (featureHasTouchNavBar()) maintainTouchNavBar();

  if (millis() - s_lastScan > 8000) {
    s_lastScan = millis();
    Serial1.println("scan");
  }
}

}  // namespace Wifi5g
