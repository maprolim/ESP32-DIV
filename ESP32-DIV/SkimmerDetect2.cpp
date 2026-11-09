// ============================================================================
//  Skimmer Detect 2 — detector avancado de skimmers Bluetooth (firmware ESP32)
// ----------------------------------------------------------------------------
//  Varredura HIBRIDA (dual-mode):
//    * BLE ......... NimBLE-Arduino (economia de RAM / sem esgotar heap).
//    * Classico .... esp_bt (Bluedroid GAP Inquiry). So existe no ESP32
//                    original; no ESP32-S3 (ESP32-DIV V2) e compilado fora e o
//                    scanner roda 100% BLE.
//
//  Motor de heuristica em 3 camadas (score 0..100):
//    1) OUI/MAC  — 3 primeiros bytes contra tabela de fabricantes de modulos
//       seriais baratos (Cambridge Silicon Radio, TI, Espressif, Nordic...).
//    2) Classe + Nome — CoD "Uncategorized"/bruto OU nome de fabrica
//       (HC-05, HC-06, JDY-xx, BT-05...) OU string generica hex/aleatoria.
//    3) Servicos/UUID — SPP (0x1101) no Classico OU UUIDs UART customizados no
//       BLE (0xFFE0, 0xFFF0, Nordic UART, HM-10...). Gatilho critico.
//
//  UI nao-bloqueante: yield()/vTaskDelay() nos loops; LEFT sai, SELECT liga/
//  desliga, UP/DOWN navegam, RIGHT limpa. "Contador Geiger" suavizado por EMA
//  (Media Movel Exponencial) para localizar o alvo fisicamente.
// ============================================================================

#include "SkimmerDetect2.h"

#include "BleCompat.h"
#include "Touchscreen.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "icon.h"
#include "shared.h"
#include "utils.h"

// ----------------------------------------------------------------------------
// Constantes de tela e de layout
// ----------------------------------------------------------------------------
static constexpr int SK2_W = 240;
static constexpr int SK2_H = 320;

static constexpr int SK2_TITLE_Y = 21;   // linha de titulo (abaixo da status bar)
static constexpr int SK2_SUB_Y   = 38;   // linha de fase + contagem
static constexpr int SK2_HDR_H   = 56;   // altura da area de cabecalho
static constexpr int SK2_BOX_X   = 6;    // margem lateral da caixa do painel principal
static constexpr int SK2_BOX_PAD = 8;    // recuo interno do texto dentro da caixa
static constexpr int SK2_LIST_ROWS = 3;   // suspeitos visiveis na lista
static constexpr int SK2_LIST_ROW_H = 28; // 2 linhas por suspeito: nome+score / MAC+dBm
static constexpr int SK2_LIST_H = SK2_LIST_ROWS * SK2_LIST_ROW_H;

static constexpr uint8_t  SK2_MAX_HITS   = 24;
static constexpr uint8_t  SK2_NAME_LEN   = 24;
static constexpr uint8_t  SK2_LABEL_LEN  = 16;

static constexpr float    SK2_EMA_ALPHA  = 0.25f;  // suavizacao do RSSI

// Pontuacao das camadas (somadas e limitadas a 100). Os pesos de OUI e de nome
// NAO sao fixos: vem das tabelas (SK2_OUIS / SK2_SIGS), herdando a ideia de
// "nivel de severidade" usada pela feature "Skimmer Detect" original.
static constexpr uint8_t  PTS_RANDOM_NAME = 15;
static constexpr uint8_t  PTS_COD_UNCAT   = 20;
static constexpr uint8_t  PTS_UUID_UART   = 35;

// Limiares de classificacao.
static constexpr uint8_t  SCORE_SUSPECT   = 40;   // entra na lista de suspeitos
static constexpr uint8_t  SCORE_ALERT     = 60;   // dispara a tela de alerta

// Duracoes de fase e de alerta.
static constexpr uint32_t SK2_PHASE_MS    = 2600; // cada fase BLE/BT
static constexpr uint32_t SK2_ALERT_MS    = 2500; // overlay de alerta
static constexpr uint32_t SK2_UI_MS       = 50;   // taxa de redesenho da UI
static constexpr uint32_t SK2_BTN_MS      = 120;  // debounce local de botoes

// ----------------------------------------------------------------------------
// Tabela OUI (Filter 1). Formato: {b0,b1,b2, fabricante, pontos}.
// Sao os 3 primeiros bytes do MAC (Organizationally Unique Identifier) de
// fabricantes comuns de modulos seriais baratos usados em skimmers.
// ----------------------------------------------------------------------------
struct Sk2Oui {
  uint8_t b0, b1, b2;
  const char* vendor;
  uint8_t pts;
};

static const Sk2Oui SK2_OUIS[] = {
  // Cambridge Silicon Radio (CSR) — classicos HC-05/HC-06 e clones.
  {0x00,0x02,0x5B,"CSR",8}, {0x00,0x02,0x72,"CSR",8}, {0x00,0x0A,0x12,"CSR",8},
  {0x00,0x0A,0x62,"CSR",8}, {0x00,0x0A,0x94,"CSR",8}, {0x00,0x0B,0x5F,"CSR",8},
  {0x00,0x0B,0xCE,"CSR",8}, {0x00,0x0F,0x36,"CSR",8}, {0x00,0x10,0x60,"CSR",8},
  {0x00,0x12,0x60,"CSR",8}, {0x00,0x18,0x8B,"CSR",8}, {0x00,0x1B,0x63,"CSR",8},
  {0x00,0x1C,0x48,"CSR",8}, {0x00,0x1D,0xFD,"CSR",8}, {0x00,0x21,0xFE,"CSR",8},
  {0x00,0x25,0x13,"CSR",8}, {0x00,0x26,0x7E,"CSR",8},
  // Texas Instruments (CC2540/CC2541 usados em varios modulos BT/BLE).
  {0x00,0x0C,0x26,"TI",6}, {0x00,0x12,0x4B,"TI",6}, {0x00,0x17,0xE3,"TI",6},
  {0x00,0x18,0x30,"TI",6}, {0x00,0x1E,0xC0,"TI",6}, {0x5C,0x31,0x3A,"TI",6},
  // Espressif (ESP32/ESP8266 como modulos de dados baratos).
  {0x24,0x0A,0xC4,"Espressif",7}, {0x24,0x6F,0x28,"Espressif",7},
  {0x24,0xB2,0xDE,"Espressif",7}, {0x30,0xAE,0xA4,"Espressif",7},
  {0x34,0x85,0x18,"Espressif",7}, {0x3C,0x71,0xBF,"Espressif",7},
  {0x48,0x27,0xE2,"Espressif",7}, {0x4C,0xEB,0xD6,"Espressif",7},
  {0x54,0x32,0x04,"Espressif",7}, {0x60,0x01,0x94,"Espressif",7},
  {0x68,0xC6,0x3A,"Espressif",7}, {0x78,0xE3,0x6D,"Espressif",7},
  {0x7C,0x9E,0xBD,"Espressif",7}, {0x7C,0xDF,0xA1,"Espressif",7},
  {0x80,0x7D,0x3A,"Espressif",7}, {0x84,0xF3,0xEB,"Espressif",7},
  {0x8C,0xAA,0xB5,"Espressif",7}, {0x9C,0x9C,0x1F,"Espressif",7},
  {0xA4,0xCF,0x12,"Espressif",7}, {0xB4,0xE6,0x2D,"Espressif",7},
  {0xC4,0x4F,0x33,"Espressif",7}, {0xCC,0x50,0xE3,"Espressif",7},
  {0xD8,0xA0,0x1D,"Espressif",7}, {0xEC,0x62,0x60,"Espressif",7},
  {0xFC,0xF5,0xC4,"Espressif",7},
  // Nordic Semiconductor (nRF51/nRF52 — BT05/JDY/BT04 e derivados).
  {0x00,0x07,0x80,"Nordic",5}, {0x00,0x25,0xDF,"Nordic",5},
  {0x88,0x6B,0x0F,"Nordic",5}, {0x8C,0xAB,0x8E,"Nordic",5},
  {0x9C,0xB6,0xD0,"Nordic",5}, {0xCC,0xE8,0xAC,"Nordic",5},
  {0xD8,0x9E,0x3F,"Nordic",5}, {0xE8,0x9F,0x6D,"Nordic",5},
  {0xF4,0xCE,0x23,"Nordic",5},
  // Telink (TLSR82xx, modulos BT/LE genericos).
  {0x00,0x15,0x83,"Telink",5}, {0xA4,0xC1,0x38,"Telink",5},
};

// ----------------------------------------------------------------------------
// Assinaturas de nome de fabrica (Filter 2). O needle ja esta normalizado
// (A-Z0-9) e e procurado como substring no nome normalizado do dispositivo.
//
// Cada assinatura carrega o proprio peso (pts), herdado dos niveis de ameaca
// da feature original: HC-0x / FREE2MOVE sao os modulos seriais "crus" mais
// graves; HM-1x / AT-09 sao BLE populares que tambem aparecem em produtos
// legitimos, entao pesam menos. Assim "HC-05" pontua mais que "HM-10".
// ----------------------------------------------------------------------------
struct Sk2Sig {
  const char* needle;
  const char* label;
  uint8_t pts;
};

static const Sk2Sig SK2_SIGS[] = {
  // Severidade alta (modulos seriais crus, tipicos de skimmer).
  {"FREE2MOVE","FREE2MOVE",38}, {"BTHC05","BT-HC05",38}, {"BTHC06","BT-HC06",38},
  {"HC03","HC-03",38}, {"HC04","HC-04",38}, {"HC05","HC-05",38},
  {"HC06","HC-06",38}, {"HC08","HC-08",38},
  // Severidade media.
  {"MLTBT05","MLT-BT05",32}, {"BT04A","BT04-A",32}, {"BTSPP","BT-SPP",32},
  {"CC41A","CC41-A",32}, {"SPPCA","SPP-CA",32}, {"LINVOR","LINVOR",32},
  {"BT04","BT-04",32}, {"BT05","BT-05",32}, {"BT06","BT-06",32},
  {"BT08","BT-08",32}, {"CC41","CC41",32},
  {"JDY08","JDY-08",32}, {"JDY10","JDY-10",32}, {"JDY16","JDY-16",32},
  {"JDY23","JDY-23",32}, {"JDY31","JDY-31",32},
  // Severidade menor (BLE popular, presente tambem em aparelhos legitimos).
  {"HM10","HM-10",26}, {"HM11","HM-11",26}, {"HM19","HM-19",26},
  {"AT09","AT-09",26},
};

// UUIDs UART customizados comuns (Filter 3, BLE) — em minusculo.
static const char* const SK2_UART_UUID16[] = {
  "0xffe0","0xffe1","0xffe2","0xffe3","0xfff0","0xfff5","0xfee9","0x1101","0xabf0",
};

// UUIDs 128-bit UART (substrings, minusculo).
static const char* const SK2_UART_UUID128_SUB[] = {
  "6e400001-b5a3-f393-e0a9-e50e24dcca9e",  // Nordic UART Service (NUS)
  "0000ffe0-0000-1000-8000-00805f9b34fb",  // HM-10 / MLT-BT05 / BT05
  "0000fff0-0000-1000-8000-00805f9b34fb",  // JDY-31 / JDY-08
  "0000ffe1-0000-1000-8000-00805f9b34fb",  // UART TX/RX variants
  "0000ff00-0000-1000-8000-00805f9b34fb",
};

// ----------------------------------------------------------------------------
// Estado
// ----------------------------------------------------------------------------
namespace BleSkimmer2 {

struct Suspect {
  uint8_t mac[6];
  bool    isBle;
  int8_t  rssiRaw;      // ultima leitura crua
  float   rssiEma;      // suavizada (EMA)
  bool    emaInit;      // primeira amostra ja aplicada?
  uint8_t score;        // 0..100
  uint16_t hits;
  char    name[SK2_NAME_LEN];
  char    label[SK2_LABEL_LEN];  // motivo (OUI:CSR, HC-05, SPP...)
  uint32_t lastSeenMs;
  bool    dirty;
};

enum class Phase : uint8_t { Ble, Bt };

static Suspect s_hits[SK2_MAX_HITS];
static int s_hitCount = 0;
static int s_selected = 0;      // indice do suspeito em foco (Geiger)

static bool  s_scanning = false;
static Phase s_phase = Phase::Ble;
static uint32_t s_phaseStartMs = 0;
static uint32_t s_alertUntilMs = 0;

static BLEScan* s_scan = nullptr;

static bool s_listDirty = true;
static bool s_hasUpdates = false;
static bool s_needFullDraw = true;
static uint32_t s_lastUiMs = 0;
static uint32_t s_lastBtnMs = 0;
static int s_drawnCount = -1;

// Estado do painel principal (evita repintar a caixa/textos a cada quadro, que
// era a causa do "piscar"): -1 = nada desenhado, 0 = modo scan, 1 = modo Geiger.
static int  s_mainMode = -1;
static int  s_geigerSel = -1;          // qual suspeito esta desenhado no Geiger
static char s_lastSpinChar = '\0';     // ultimo caractere do indicador de scan
static int  s_lastMeterFilled = -1;    // ultimo numero de segmentos da barra
static int  s_lastEmaShown = -9999;    // ultimo RSSI (EMA) mostrado em texto
static uint32_t s_lastStatusMs = 0;    // throttle do updateStatusBar()
static uint32_t s_lastListRssiMs = 0;  // throttle do dBm ao vivo na lista
static const char* s_hintText = nullptr;// aviso temporario na area da lista
static uint32_t s_hintUntilMs = 0;
static bool s_hintWasActive = false;
static bool s_alertShown = false;      // overlay de alerta ja desenhado?

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

#if !BOARD_HAS_ESP32S3
static bool s_btUp = false;
#endif
static bool s_bleUp = false;

// ----------------------------------------------------------------------------
// Pequenos utilitarios
// ----------------------------------------------------------------------------
static int sk2ContentBottom() {
  return featureHasTouchNavBar() ? (int)touchNavContentBottomY() : SK2_H;
}

static int sk2MainTop() { return SK2_HDR_H + 4; }

static int sk2ListTop() { return sk2ContentBottom() - SK2_LIST_H; }

static int sk2MainBottom() { return sk2ListTop() - 4; }

static void sk2ClearBody() {
  if (featureHasTouchNavBar()) {
    featureClearContent(FEATURE_BG);
  } else {
    tft.fillScreen(FEATURE_BG);
  }
}

static void sk2Fit(char* out, size_t outSz, const char* s, int maxPx) {
  if (!out || outSz == 0) return;
  out[0] = '\0';
  if (!s || !s[0]) return;
  tft.setTextFont(1);
  tft.setTextSize(1);
  if (tft.textWidth(s) <= maxPx) {
    strncpy(out, s, outSz - 1);
    out[outSz - 1] = '\0';
    return;
  }
  size_t len = strlen(s);
  if (len > outSz - 4) len = outSz - 4;
  memcpy(out, s, len);
  out[len] = '\0';
  while (len > 1 && tft.textWidth(out) + tft.textWidth("...") > maxPx) {
    out[--len] = '\0';
  }
  strcat(out, "...");
}

static void sk2Lower(char* s) {
  for (; s && *s; ++s) {
    if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
  }
}

static void sk2Normalize(const char* in, char* out, size_t outSz) {
  size_t j = 0;
  for (size_t i = 0; in && in[i] && j + 1 < outSz; i++) {
    char c = in[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[j++] = c;
  }
  out[j] = '\0';
}

static bool sk2IsHexString(const char* norm) {
  const size_t n = strlen(norm);
  if (n < 8) return false;              // curto demais para ser "string aleatoria"
  for (size_t i = 0; i < n; i++) {
    const char c = norm[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return true;                          // ex.: "0018E4ABCDEF" (MAC como nome)
}

// ----------------------------------------------------------------------------
// Filter 1 — OUI/MAC
// ----------------------------------------------------------------------------
static const char* sk2OuiLookup(const uint8_t mac[6], uint8_t* ptsOut) {
  for (size_t i = 0; i < sizeof(SK2_OUIS) / sizeof(SK2_OUIS[0]); i++) {
    const Sk2Oui& o = SK2_OUIS[i];
    if (mac[0] == o.b0 && mac[1] == o.b1 && mac[2] == o.b2) {
      if (ptsOut) *ptsOut = o.pts;
      return o.vendor;
    }
  }
  return nullptr;
}

// ----------------------------------------------------------------------------
// Filter 2 — Classe do Dispositivo (CoD) + Nome
// ----------------------------------------------------------------------------
static bool sk2CodUncategorized(uint32_t cod, bool codValid) {
  if (!codValid) return false;
  // CoD: 11 bits service | 5 bits major | 6 bits minor | 2 bits format.
  const uint32_t major = (cod >> 8) & 0x1Fu;
  // "Uncategorized" (31) ou "Miscellaneous" (0) => classe bruta/nao especificada.
  return (major == 0x1Fu || major == 0x00u);
}

static bool sk2NameSignature(const char* rawName, char* labelOut, size_t labelSz,
                             uint8_t* ptsOut) {
  char norm[40];
  sk2Normalize(rawName, norm, sizeof(norm));
  if (!norm[0]) return false;

  // Familia JDY-xx inteira (qualquer JDY-0x/1x/2x/3x).
  if (strncmp(norm, "JDY", 3) == 0 && strlen(norm) >= 3) {
    strncpy(labelOut, "JDY-*", labelSz - 1);
    labelOut[labelSz - 1] = '\0';
    if (ptsOut) *ptsOut = 32;
    return true;
  }
  for (size_t i = 0; i < sizeof(SK2_SIGS) / sizeof(SK2_SIGS[0]); i++) {
    if (strstr(norm, SK2_SIGS[i].needle) != nullptr) {
      strncpy(labelOut, SK2_SIGS[i].label, labelSz - 1);
      labelOut[labelSz - 1] = '\0';
      if (ptsOut) *ptsOut = SK2_SIGS[i].pts;
      return true;
    }
  }
  return false;
}

// ----------------------------------------------------------------------------
// Filter 3 — Servicos/UUID
// ----------------------------------------------------------------------------
static bool sk2UuidStrIsUart(const char* uuidStr) {
  if (!uuidStr || !uuidStr[0]) return false;
  char u[48];
  strncpy(u, uuidStr, sizeof(u) - 1);
  u[sizeof(u) - 1] = '\0';
  sk2Lower(u);

  for (size_t i = 0; i < sizeof(SK2_UART_UUID16) / sizeof(SK2_UART_UUID16[0]); i++) {
    if (strcmp(u, SK2_UART_UUID16[i]) == 0) return true;
  }
  for (size_t i = 0; i < sizeof(SK2_UART_UUID128_SUB) / sizeof(SK2_UART_UUID128_SUB[0]); i++) {
    if (strstr(u, SK2_UART_UUID128_SUB[i]) != nullptr) return true;
  }
  return false;
}

// Verifica se o dispositivo BLE anuncia algum UUID UART (SPP/UART custom).
static bool sk2BleHasUartUuid(BLEAdvertisedDevice* d) {
  if (!d) return false;
  const uint8_t n = d->getServiceUUIDCount();
  for (uint8_t i = 0; i < n; i++) {
    if (sk2UuidStrIsUart(d->getServiceUUID(i).toString().c_str())) return true;
  }
  // Fallback: varre o payload bruto em busca de UUIDs UART/SPP (16-bit LE).
  // getPayload() retorna uint8_t* (nao std::string) nesta versao do NimBLE.
  const uint8_t* p = d->getPayload();
  const size_t len = d->getPayloadLength();
  if (p && len > 0) {
    for (size_t i = 0; i + 1 < len; i++) {
      const uint16_t u = (uint16_t)(p[i] | (p[i + 1] << 8));  // little-endian
      switch (u) {
        case 0x1101:  // SPP (Serial Port Profile)
        case 0xFFE0: case 0xFFE1: case 0xFFE2: case 0xFFE3:
        case 0xFFF0: case 0xFFF5: case 0xFEE9: case 0xABF0:
          return true;
        default:
          break;
      }
    }
  }
  return false;
}

// Verifica se o EIR Classico contem o UUID 16-bit 0x1101 (SPP).
static bool sk2EirHasSpp(const uint8_t* eir, int len) {
  if (!eir || len <= 0) return false;
  int i = 0;
  while (i < len) {
    const uint8_t fieldLen = eir[i];
    if (fieldLen == 0 || i + 1 + fieldLen > len) break;
    const uint8_t type = eir[i + 1];
    const uint8_t* data = eir + i + 2;
    const int dataLen = fieldLen - 1;
    // Listas de UUID 16-bit (0x02/0x03) e 32-bit (0x04/0x05).
    if (type == 0x02 || type == 0x03) {
      for (int k = 0; k + 1 < dataLen; k += 2) {
        // little-endian: SPP 0x1101 => 0x01 0x11
        if (data[k] == 0x01 && data[k + 1] == 0x11) return true;
      }
    }
    i += 1 + fieldLen;
  }
  return false;
}

// ----------------------------------------------------------------------------
// Analise heuristica completa (3 camadas) -> score + rotulo
// ----------------------------------------------------------------------------
struct Sk2Analysis {
  uint8_t score;
  char label[SK2_LABEL_LEN];
};

static void sk2Analyze(const uint8_t mac[6], bool isBle, const char* name,
                       uint32_t cod, bool codValid, bool uartUuid,
                       Sk2Analysis& out) {
  out.score = 0;
  out.label[0] = '\0';

  auto addLabel = [&](const char* s) {
    if (out.label[0]) strcat(out.label, " ");
    strncat(out.label, s, SK2_LABEL_LEN - strlen(out.label) - 1);
  };

  // Filter 1 — OUI (peso por fabricante: CSR 8, Espressif 7, TI 6, Nordic 5...).
  uint8_t ouiPts = 0;
  const char* vendor = sk2OuiLookup(mac, &ouiPts);
  if (vendor) {
    out.score += (uint8_t)(ouiPts * 4);   // 20..32 pts
    addLabel(vendor);
  }

  // Filter 2 — nome de fabrica (peso por assinatura) / string generica.
  char sigLabel[SK2_LABEL_LEN];
  uint8_t sigPts = 0;
  if (sk2NameSignature(name, sigLabel, sizeof(sigLabel), &sigPts)) {
    out.score += sigPts;
    addLabel(sigLabel);
  } else {
    char norm[40];
    sk2Normalize(name, norm, sizeof(norm));
    if (sk2IsHexString(norm)) {
      out.score += PTS_RANDOM_NAME;
      addLabel("hex");
    }
  }

  // Filter 2 — Classe do Dispositivo "Uncategorized"/bruta (Classico).
  if (sk2CodUncategorized(cod, codValid)) {
    out.score += PTS_COD_UNCAT;
    addLabel("COD");
  }

  // Filter 3 — SPP / UUID UART. Gatilho critico.
  if (uartUuid) {
    out.score += PTS_UUID_UART;
    addLabel(isBle ? "UART" : "SPP");
  }

  if (out.score > 100) out.score = 100;
  if (!out.label[0]) {
    strncpy(out.label, isBle ? "BLE" : "BT", SK2_LABEL_LEN - 1);
    out.label[SK2_LABEL_LEN - 1] = '\0';
  }
}

// ----------------------------------------------------------------------------
// EMA (Media Movel Exponencial) — suaviza o RSSI para o grafico nao piscar.
// ----------------------------------------------------------------------------
static float sk2Ema(float sample, float ema, bool& init) {
  if (!init) {
    init = true;
    return sample;
  }
  return SK2_EMA_ALPHA * sample + (1.0f - SK2_EMA_ALPHA) * ema;
}

// ----------------------------------------------------------------------------
// Gerenciamento da lista de suspeitos (thread-safe via portMUX)
// ----------------------------------------------------------------------------
static int sk2FindHit(const uint8_t mac[6]) {
  for (int i = 0; i < s_hitCount; i++) {
    if (memcmp(s_hits[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

static bool sk2Upsert(const uint8_t mac[6], bool isBle, int8_t rssi,
                      uint8_t score, const char* label, const char* name) {
  const uint32_t now = millis();
  int idx = sk2FindHit(mac);

  if (idx >= 0) {
    Suspect& h = s_hits[idx];
    h.rssiRaw = rssi;
    h.rssiEma = sk2Ema((float)rssi, h.rssiEma, h.emaInit);
    h.hits++;
    h.lastSeenMs = now;
    bool changed = false;
    if (score > h.score) { h.score = score; changed = true; }
    if (label && label[0] && strcmp(h.label, label) != 0) {
      strncpy(h.label, label, SK2_LABEL_LEN - 1);
      h.label[SK2_LABEL_LEN - 1] = '\0';
      changed = true;
    }
    if (name && name[0] && strcmp(h.name, name) != 0) {
      strncpy(h.name, name, SK2_NAME_LEN - 1);
      h.name[SK2_NAME_LEN - 1] = '\0';
      changed = true;
    }
    h.dirty = h.dirty || changed;
    // Se a pontuacao subiu e ficou acima do foco atual, foca nele.
    if (h.score > s_hits[s_selected].score && idx != s_selected) s_selected = idx;
    return false;
  }

  if (s_hitCount >= SK2_MAX_HITS) {
    // Eviccao do mais fraco (menor score; desempate por RSSI mais distante).
    int weakest = 0;
    for (int i = 1; i < s_hitCount; i++) {
      if (s_hits[i].score < s_hits[weakest].score) weakest = i;
      else if (s_hits[i].score == s_hits[weakest].score &&
               s_hits[i].rssiRaw < s_hits[weakest].rssiRaw) weakest = i;
    }
    if (score < s_hits[weakest].score) return false;
    memmove(&s_hits[weakest], &s_hits[weakest + 1],
            (size_t)(s_hitCount - weakest - 1) * sizeof(Suspect));
    s_hitCount--;
    if (s_selected >= s_hitCount) s_selected = s_hitCount > 0 ? s_hitCount - 1 : 0;
  }

  Suspect& h = s_hits[s_hitCount];
  memset(&h, 0, sizeof(h));
  memcpy(h.mac, mac, 6);
  h.isBle = isBle;
  h.rssiRaw = rssi;
  h.rssiEma = (float)rssi;
  h.emaInit = true;
  h.score = score;
  h.hits = 1;
  h.lastSeenMs = now;
  if (label) { strncpy(h.label, label, SK2_LABEL_LEN - 1); h.label[SK2_LABEL_LEN - 1] = '\0'; }
  if (name)  { strncpy(h.name, name, SK2_NAME_LEN - 1);   h.name[SK2_NAME_LEN - 1] = '\0'; }
  h.dirty = true;
  s_hitCount++;

  // Foca automaticamente o suspeito de maior score (o alvo do Geiger).
  if (s_hitCount == 1) {
    s_selected = 0;
  } else if (score > s_hits[s_selected].score) {
    s_selected = s_hitCount - 1;
  }
  return true;   // suspeito novo (primeira vez que este MAC aparece)
}

static void sk2ExtractName(BLEAdvertisedDevice* d, char* out, size_t outSz) {
  out[0] = '\0';
  if (!d || outSz < 2) return;
  std::string n = d->getName();
  if (n.empty()) n = d->getPayloadByType(0x09);  // Complete Local Name
  if (n.empty()) n = d->getPayloadByType(0x08);  // Shortened Local Name
  if (n.empty()) return;
  strncpy(out, n.c_str(), outSz - 1);
  out[outSz - 1] = '\0';
  for (char* p = out; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c < 32 || c > 126) *p = '?';
  }
  // Remove espacos/'?' sobrando no fim (nomes costumam vir com padding). Isso
  // faz a comparacao e a exibicao ficarem limpas e estaveis, sem o nome
  // "piscar" entre duas variantes a cada anuncio — igual a feature original.
  for (int i = (int)strlen(out) - 1; i >= 0; --i) {
    if (out[i] == ' ' || out[i] == '?') out[i] = '\0';
    else break;
  }
}

// ----------------------------------------------------------------------------
// Callback BLE (roda na task NimBLE) — mantem o trabalho curto e protege a
// lista com seccao critica.
// ----------------------------------------------------------------------------
class Sk2BleCallbacks : public BLEAdvertisedDeviceCallbacks {
 public:
  void onResult(BLEAdvertisedDevice* d) override {
    if (!s_scanning || !d) return;

    const std::string addrStr = d->getAddress().toString();
    unsigned int b0=0,b1=0,b2=0,b3=0,b4=0,b5=0;
    if (sscanf(addrStr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &b0,&b1,&b2,&b3,&b4,&b5) != 6) return;
    const uint8_t mac[6] = {(uint8_t)b0,(uint8_t)b1,(uint8_t)b2,
                            (uint8_t)b3,(uint8_t)b4,(uint8_t)b5};

    char name[SK2_NAME_LEN];
    sk2ExtractName(d, name, sizeof(name));

    Sk2Analysis a;
    sk2Analyze(mac, /*isBle=*/true, name, /*cod=*/0, /*codValid=*/false,
               sk2BleHasUartUuid(d), a);
    if (a.score < SCORE_SUSPECT) return;

    portENTER_CRITICAL(&s_mux);
    const bool isNew = sk2Upsert(mac, true, (int8_t)d->getRSSI(), a.score, a.label, name);
    s_hasUpdates = true;
    // Alerta so na PRIMEIRA vez que o suspeito aparece; senao o overlay se
    // repetiria a cada anuncio e nunca deixaria ver o radar Geiger.
    if (isNew && a.score >= SCORE_ALERT) s_alertUntilMs = millis() + SK2_ALERT_MS;
    portEXIT_CRITICAL(&s_mux);
  }
};

static Sk2BleCallbacks s_bleCallbacks;

// ----------------------------------------------------------------------------
// Callback GAP Classico (roda na task Bluedroid) — apenas em ESP32 (V1).
// ----------------------------------------------------------------------------
#if !BOARD_HAS_ESP32S3
static void sk2BtGapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
  if (event != ESP_BT_GAP_DISC_RES_EVT || !param || !s_scanning) return;

  const esp_bd_addr_t& bda = param->disc_res.bda;
  const uint8_t mac[6] = {bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]};

  int8_t rssi = 0;
  uint32_t cod = 0;
  bool codValid = false;
  bool hasSpp = false;
  char name[SK2_NAME_LEN] = {0};

  for (int i = 0; i < param->disc_res.num_prop; i++) {
    const esp_bt_gap_dev_prop_t& p = param->disc_res.prop[i];
    switch (p.type) {
      case ESP_BT_GAP_DEV_PROP_BDNAME:
        strncpy(name, (const char*)p.val, SK2_NAME_LEN - 1);
        name[SK2_NAME_LEN - 1] = '\0';
        break;
      case ESP_BT_GAP_DEV_PROP_COD:
        if (p.len >= (int)sizeof(uint32_t)) {
          cod = *(const uint32_t*)p.val;
          codValid = true;
        }
        break;
      case ESP_BT_GAP_DEV_PROP_RSSI:
        rssi = *(const int8_t*)p.val;
        break;
      case ESP_BT_GAP_DEV_PROP_EIR:
        hasSpp = sk2EirHasSpp((const uint8_t*)p.val, p.len);
        break;
      default:
        break;
    }
  }

  Sk2Analysis a;
  sk2Analyze(mac, /*isBle=*/false, name, cod, codValid, hasSpp, a);
  if (a.score < SCORE_SUSPECT) return;

  portENTER_CRITICAL(&s_mux);
  const bool isNew = sk2Upsert(mac, false, rssi, a.score, a.label, name);
  s_hasUpdates = true;
  if (isNew && a.score >= SCORE_ALERT) s_alertUntilMs = millis() + SK2_ALERT_MS;
  portEXIT_CRITICAL(&s_mux);
}
#endif

// ----------------------------------------------------------------------------
// Gerenciamento do radio (BLE via NimBLE; BT Classico via esp_bt)
// ----------------------------------------------------------------------------
static void sk2BleTeardown() {
  if (s_scan) {
    s_scan->stop();
    s_scan->setAdvertisedDeviceCallbacks(nullptr);
    s_scan = nullptr;
  }
#if !BOARD_HAS_ESP32S3
  // No ESP32 original o NimBLE precisa ser derrubado para devolver o
  // controlador ao modo Classico (handoff). No S3 isso NAO e necessario e
  // manter o stack de pe evita quebrar ensureBleStackReady()/outros recursos.
  if (s_bleUp) {
    NimBLEDevice::deinit(true);
    s_bleUp = false;
  }
#endif
}

static bool sk2BleEnsure() {
  if (s_bleUp) return true;
#if !BOARD_HAS_ESP32S3
  // Se o Classico estiver de pe, libera antes de subir o BLE.
  if (s_btUp) {
    esp_bt_gap_cancel_discovery();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_deinit();
    s_btUp = false;
  }
  // Devolve RAM do controlador Classico (seguro antes do init). O init do
  // NimBLE e idempotente (guarda interna `initialized`), entao nao ha risco
  // de dupla inicializacao.
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  if (!NimBLEDevice::getInitialized()) {
    NimBLEDevice::init(ESP32DIV_NAME);
  }
#else
  // S3: caminho padrao (com guarda de heap e liberacao de RAM Classico).
  if (!ensureBleStackReady()) return false;
#endif
  s_bleUp = true;
  return true;
}

#if !BOARD_HAS_ESP32S3
static bool sk2BtEnsure() {
  if (s_btUp) return true;

  // 1) derruba o NimBLE para devolver a RAM/controlador ao modo Classico.
  sk2BleTeardown();

  // 2) inicializa o controlador em modo Classico puro.
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  cfg.mode = ESP_BT_MODE_CLASSIC_BT;
  if (esp_bt_controller_init(&cfg) != ESP_OK) return false;
  if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) return false;
  if (esp_bluedroid_init() != ESP_OK) return false;
  if (esp_bluedroid_enable() != ESP_OK) return false;
  if (esp_bt_gap_register_callback(sk2BtGapCallback) != ESP_OK) return false;
  s_btUp = true;
  return true;
}

static void sk2BtStartDiscovery() {
  if (!s_btUp) return;
  // inq_len em unidades de ~1.28s: 2 => ~2.56s (casa com SK2_PHASE_MS).
  esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 2, 0);
}

static void sk2BtStopDiscovery() {
  if (!s_btUp) return;
  esp_bt_gap_cancel_discovery();
}
#endif

static void sk2StartBleScan() {
  if (!sk2BleEnsure()) return;
  if (!s_scan) {
    s_scan = BLEDevice::getScan();
  }
  s_scan->stop();
  s_scan->setAdvertisedDeviceCallbacks(&s_bleCallbacks, true);
  s_scan->setActiveScan(true);
  s_scan->setInterval(100);
  s_scan->setWindow(80);
  s_scan->setDuplicateFilter(false);
  s_scanning = true;
  s_scan->start(0, nullptr, false);  // varredura continua em background
}

static void sk2StopScan() {
  if (s_scan) {
    s_scan->stop();
    s_scan->setAdvertisedDeviceCallbacks(nullptr);
  }
#if !BOARD_HAS_ESP32S3
  sk2BtStopDiscovery();
#endif
  s_scanning = false;
}

// Maquina de estados da varredura hibrida: alterna BLE <-> BT Classico.
static void sk2RunScanMachine() {
  if (!s_scanning) return;
  const uint32_t now = millis();
  const uint32_t elapsed = now - s_phaseStartMs;

  if (s_phase == Phase::Ble) {
    // Re-arma o scan BLE caso o NimBLE tenha parado sozinho.
    if (s_scan && !s_scan->isScanning()) {
      s_scan->start(0, nullptr, false);
    }
    if (elapsed >= SK2_PHASE_MS) {
#if !BOARD_HAS_ESP32S3
      // Troca para o Classico (somente ESP32).
      if (s_scan) { s_scan->stop(); s_scan->setAdvertisedDeviceCallbacks(nullptr); }
      if (sk2BtEnsure()) {
        sk2BtStartDiscovery();
        s_phase = Phase::Bt;
        s_phaseStartMs = now;
      } else {
        // Falhou: permanece no BLE (re-inicializa o NimBLE).
        sk2BleEnsure();
        sk2StartBleScan();
        s_phaseStartMs = now;
      }
#else
      // ESP32-S3 nao tem BT Classico: mantem BLE e reinicia o ciclo.
      s_phaseStartMs = now;
#endif
    }
  } else {
#if !BOARD_HAS_ESP32S3
    if (elapsed >= SK2_PHASE_MS) {
      sk2BtStopDiscovery();
      // Volta para o BLE.
      if (s_btUp) {
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable(ESP_BT_MODE_CLASSIC_BT);
        esp_bt_controller_deinit();
        s_btUp = false;
      }
      sk2BleEnsure();
      sk2StartBleScan();
      s_phase = Phase::Ble;
      s_phaseStartMs = now;
    }
#else
    s_phase = Phase::Ble;
    s_phaseStartMs = now;
#endif
  }
}

// ----------------------------------------------------------------------------
// Desenho
// ----------------------------------------------------------------------------
static void sk2PaintText(int x, int y, const char* s, uint16_t color, uint16_t bg) {
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(color, bg);
  tft.setCursor(x, y);
  tft.print(s ? s : "");
}

static uint16_t sk2ScoreColor(uint8_t score) {
  if (score >= SCORE_ALERT) return RED;
  if (score >= SCORE_SUSPECT) return ORANGE;
  return GREEN;
}

// Barra segmentada estilo "Contador Geiger" (20 segmentos).
static void sk2DrawMeter(int x, int y, int w, int h, float norm) {
  constexpr int SEGS = 20;
  constexpr int GAP = 2;
  const int segW = (w - (SEGS - 1) * GAP) / SEGS;
  if (norm < 0.0f) norm = 0.0f;
  if (norm > 1.0f) norm = 1.0f;
  const int filled = (int)(norm * SEGS + 0.5f);

  for (int i = 0; i < SEGS; i++) {
    const int sx = x + i * (segW + GAP);
    uint16_t c = GREEN;
    if (i >= 13) c = RED;
    else if (i >= 7) c = ORANGE;
    if (i < filled) tft.fillRect(sx, y, segW, h, c);
    else tft.drawRect(sx, y, segW, h, UI_DIM_TEXT);
  }
}

// Normaliza RSSI [-100,-30] dBm para [0,1] (quanto mais proximo, maior).
static float sk2RssiNorm(float rssi) {
  constexpr float LO = -100.0f;
  constexpr float HI = -30.0f;
  if (rssi < LO) rssi = LO;
  if (rssi > HI) rssi = HI;
  return (rssi - LO) / (HI - LO);
}

static void sk2DrawHeader(bool force) {
  static char cachePhase[32] = {0};

  const char* phaseStr;
  if (!s_scanning) phaseStr = "Parado";
#if !BOARD_HAS_ESP32S3
  else phaseStr = (s_phase == Phase::Ble) ? "Varredura BLE" : "Varredura BT";
#else
  else phaseStr = "Varredura BLE";
#endif

  char line[40];
  snprintf(line, sizeof(line), "%s | %d suspeito%s",
           phaseStr, s_hitCount, s_hitCount == 1 ? "" : "s");

  if (force || strcmp(cachePhase, line) != 0) {
    strncpy(cachePhase, line, sizeof(cachePhase) - 1);
    cachePhase[sizeof(cachePhase) - 1] = '\0';
    tft.fillRect(0, SK2_SUB_Y, SK2_W, 16, FEATURE_BG);
    sk2PaintText(6, SK2_SUB_Y, line, s_scanning ? ORANGE : UI_DIM_TEXT, FEATURE_BG);
  }
}

// ----------------------------------------------------------------------------
// Modo SCAN — o "chrome" (caixa + textos) e desenhado UMA vez. Depois disso so
// o indicador de atividade muda, e devagar. Antes, a caixa inteira era apagada
// e redesenhada ~20x/s, o que causava o piscar incomodo na tela.
// ----------------------------------------------------------------------------
static void sk2DrawScanChrome() {
  const int mainTop = sk2MainTop();
  const int mainBottom = sk2MainBottom();
  if (mainBottom <= mainTop) return;

  tft.fillRect(0, mainTop, SK2_W, mainBottom - mainTop, FEATURE_BG);
  tft.drawRect(SK2_BOX_X, mainTop, SK2_W - 2 * SK2_BOX_X, mainBottom - mainTop, UI_LINE);

  const int tx = SK2_BOX_X + SK2_BOX_PAD;   // recuo interno (tabulacao)
  sk2PaintText(tx, mainTop + 46, "Procurando modulos suspeitos...", UI_DIM_TEXT, FEATURE_BG);
  sk2PaintText(tx, mainTop + 62, "HC-05 / HC-06 / HM-10 / JDY", UI_DIM_TEXT, FEATURE_BG);
  sk2PaintText(tx, mainTop + 78, "BT-05 / CC41 / SPP / UART", UI_DIM_TEXT, FEATURE_BG);
  sk2PaintText(tx, mainTop + 100, "Aproxime-se do caixa ou da", UI_DIM_TEXT, FEATURE_BG);
  sk2PaintText(tx, mainTop + 114, "bomba para triangular o sinal.", UI_DIM_TEXT, FEATURE_BG);
  // Ressalva de responsabilidade, como na feature original ("a match is a
  // warning only, not proof of a skimmer").
  sk2PaintText(tx, mainTop + 132, "Alerta nao e prova de skimmer.", UI_DIM_TEXT, FEATURE_BG);

  s_lastSpinChar = '\0';  // forca redesenhar o indicador
}

// Indicador de atividade: UM unico caractere, trocado a cada ~420ms (calmo).
// Como o caractere e sempre monoespaçado (fonte 1), sobrescrever a mesma cela
// limpa o anterior sozinho — sem repintar o resto da caixa.
static void sk2UpdateScanIndicator() {
  static const char* kSpin = "|/-\\";
  static uint32_t lastSpinMs = 0;
  static int spinIdx = 0;

  const int mainTop = sk2MainTop();
  if (sk2MainBottom() <= mainTop) return;

  const uint32_t now = millis();
  if (s_scanning && (uint32_t)(now - lastSpinMs) >= 420) {
    lastSpinMs = now;
    spinIdx = (spinIdx + 1) & 3;
  }

  const char ch = s_scanning ? kSpin[spinIdx] : '-';
  if (ch == s_lastSpinChar) return;
  s_lastSpinChar = ch;

  char sp[2] = {ch, '\0'};
  sk2PaintText(SK2_W / 2 - 3, mainTop + 26, sp,
               s_scanning ? ORANGE : UI_DIM_TEXT, FEATURE_BG);
}

// ----------------------------------------------------------------------------
// Modo GEIGER — chrome desenhado uma vez por suspeito selecionado; so a barra
// e o texto do RSSI sao atualizados ao vivo.
// ----------------------------------------------------------------------------
static void sk2DrawGeigerChrome() {
  if (s_selected < 0 || s_selected >= s_hitCount) return;
  const Suspect& h = s_hits[s_selected];

  const int mainTop = sk2MainTop();
  const int mainBottom = sk2MainBottom();
  if (mainBottom <= mainTop) return;

  tft.fillRect(0, mainTop, SK2_W, mainBottom - mainTop, FEATURE_BG);
  tft.drawRect(SK2_BOX_X, mainTop, SK2_W - 2 * SK2_BOX_X, mainBottom - mainTop,
               h.score >= SCORE_ALERT ? RED : UI_LINE);

  const int tx = SK2_BOX_X + SK2_BOX_PAD;
  char title[SK2_NAME_LEN + 4];
  sk2Fit(title, sizeof(title), h.name[0] ? h.name : h.label, SK2_W - 124);
  sk2PaintText(tx, mainTop + 6, title, WHITE, FEATURE_BG);

  char mac[20];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           h.mac[0],h.mac[1],h.mac[2],h.mac[3],h.mac[4],h.mac[5]);
  sk2PaintText(tx, mainTop + 22, mac, UI_DIM_TEXT, FEATURE_BG);

  char score[24];
  snprintf(score, sizeof(score), "SCORE %u/100", (unsigned)h.score);
  sk2PaintText(SK2_W - SK2_BOX_X - SK2_BOX_PAD - tft.textWidth(score), mainTop + 6,
               score, sk2ScoreColor(h.score), FEATURE_BG);

  sk2PaintText(tx, mainTop + 90, h.label, ORANGE, FEATURE_BG);
  sk2PaintText(tx, mainTop + 106, h.isBle ? "Origem: BLE" : "Origem: BT Classico",
               UI_DIM_TEXT, FEATURE_BG);

  if (h.score >= SCORE_ALERT) {
    sk2PaintText(tx, mainTop + 126, "ALERTA: provavel skimmer!", RED, FEATURE_BG);
  } else {
    sk2PaintText(tx, mainTop + 126, "Suspeito - aproxime-se p/ confirmar",
                 ORANGE, FEATURE_BG);
  }

  // Forca o redesenho imediato da barra e do texto do RSSI.
  s_lastMeterFilled = -1;
  s_lastEmaShown = -9999;
}

static void sk2UpdateGeigerLive() {
  if (s_selected < 0 || s_selected >= s_hitCount) return;
  const Suspect& h = s_hits[s_selected];
  const int mainTop = sk2MainTop();
  const int mainBottom = sk2MainBottom();
  if (mainBottom <= mainTop) return;

  const int tx = SK2_BOX_X + SK2_BOX_PAD;
  const int mw = SK2_W - 2 * SK2_BOX_X - 2 * SK2_BOX_PAD;

  // Barra segmentada: so redesenha quando o numero de segmentos muda, o que
  // (com o EMA) acontece devagar — nada de piscar.
  const float norm = sk2RssiNorm(h.rssiEma);
  const int filled = (int)(norm * 20.0f + 0.5f);
  if (filled != s_lastMeterFilled) {
    s_lastMeterFilled = filled;
    sk2DrawMeter(tx, mainTop + 42, mw, 22, norm);
  }

  // Texto do RSSI: usa o valor suavizado (EMA), entao atualiza poucas vezes
  // por segundo em vez de a cada amostra crua.
  const int emaRounded = (int)(h.rssiEma + (h.rssiEma >= 0.0f ? 0.5f : -0.5f));
  if (emaRounded != s_lastEmaShown) {
    s_lastEmaShown = emaRounded;
    char rssi[32];
    snprintf(rssi, sizeof(rssi), "Sinal %d dBm", emaRounded);
    tft.fillRect(tx, mainTop + 72, mw, 10, FEATURE_BG);
    sk2PaintText(tx, mainTop + 72, rssi, WHITE, FEATURE_BG);
  }
}

// ----------------------------------------------------------------------------
// Lista de suspeitos — linha de 2 andares, como na feature original (nome +
// MAC), acrescentando o score. O dBm e atualizado ao vivo sem repintar a linha.
// ----------------------------------------------------------------------------
static void sk2PaintListRow(int listIndex) {
  if (listIndex < 0 || listIndex >= SK2_LIST_ROWS) return;
  const int absIndex = s_selected + listIndex;
  const int y = sk2ListTop() + listIndex * SK2_LIST_ROW_H;
  if (y + SK2_LIST_ROW_H > sk2ContentBottom()) return;

  const bool sel = (listIndex == 0);   // a janela comeca no selecionado
  const uint16_t bg = sel ? UI_FG : FEATURE_BG;

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.fillRect(0, y, SK2_W, SK2_LIST_ROW_H, FEATURE_BG);
  if (absIndex >= s_hitCount) return;

  const Suspect& h = s_hits[absIndex];
  if (sel) tft.fillRect(0, y, SK2_W, SK2_LIST_ROW_H, UI_FG);

  // Andar 1: nome (ou rotulo) a esquerda + score a direita.
  char name[20];
  sk2Fit(name, sizeof(name), h.name[0] ? h.name : h.label, SK2_W - 78);
  sk2PaintText(6, y + 3, name,
               sel ? WHITE : (h.score >= SCORE_ALERT ? RED : UI_DIM_TEXT), bg);

  char sc[16];
  snprintf(sc, sizeof(sc), "S:%u", (unsigned)h.score);
  sk2PaintText(SK2_W - 6 - tft.textWidth(sc), y + 3, sc, sk2ScoreColor(h.score), bg);

  // Andar 2: MAC a esquerda + dBm a direita.
  char mac[20];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           h.mac[0],h.mac[1],h.mac[2],h.mac[3],h.mac[4],h.mac[5]);
  sk2PaintText(6, y + 16, mac, sel ? ORANGE : UI_DIM_TEXT, bg);

  char rssi[14];
  snprintf(rssi, sizeof(rssi), "%d dBm", (int)h.rssiRaw);
  sk2PaintText(SK2_W - 6 - tft.textWidth(rssi), y + 16, rssi,
               sel ? ORANGE : UI_DIM_TEXT, bg);
}

static void sk2DrawList() {
  const int listTop = sk2ListTop();
  const int bottom = sk2ContentBottom();
  tft.fillRect(0, listTop, SK2_W, bottom - listTop, FEATURE_BG);

  tft.drawFastHLine(0, listTop - 1, SK2_W, UI_LINE);

  // Aviso temporario (feedback de botao) tem prioridade sobre o texto normal.
  if (s_hintText && (int32_t)(s_hintUntilMs - millis()) > 0) {
    sk2PaintText(6, listTop + 4, s_hintText, ORANGE, FEATURE_BG);
    return;
  }

  if (s_hitCount == 0) {
    sk2PaintText(6, listTop + 4,
                 s_scanning ? "Aguardando dispositivos..." : "Pressione SELECT p/ iniciar",
                 UI_DIM_TEXT, FEATURE_BG);
    return;
  }

  for (int i = 0; i < SK2_LIST_ROWS; i++) {
    sk2PaintListRow(i);
  }
}

// Atualiza SO o campo dBm das linhas visiveis (~1x/s), sem apagar nome/MAC —
// mesmo truque da feature original (paintRssiOnly) para a lista nao piscar.
static void sk2RefreshListRssi() {
  if (s_hitCount <= 0) return;
  const int listTop = sk2ListTop();
  tft.setTextFont(1);
  tft.setTextSize(1);
  for (int i = 0; i < SK2_LIST_ROWS; i++) {
    const int absIndex = s_selected + i;
    if (absIndex >= s_hitCount) break;
    const int y = listTop + i * SK2_LIST_ROW_H + 16;
    if (y + 8 > sk2ContentBottom()) break;
    const bool sel = (i == 0);
    const uint16_t bg = sel ? UI_FG : FEATURE_BG;
    char rssi[14];
    snprintf(rssi, sizeof(rssi), "%d dBm", (int)s_hits[absIndex].rssiRaw);
    tft.fillRect(SK2_W - 62, y, 56, 10, bg);
    sk2PaintText(SK2_W - 6 - tft.textWidth(rssi), y, rssi,
                 sel ? ORANGE : UI_DIM_TEXT, bg);
  }
}

// Mantem o indice selecionado dentro dos limites apos adicao/eviccao/limpeza
// (equivale ao clampSelection() da feature original).
static void sk2ClampSelection() {
  if (s_hitCount <= 0) { s_selected = 0; return; }
  if (s_selected < 0) s_selected = 0;
  if (s_selected >= s_hitCount) s_selected = s_hitCount - 1;
}

// Overlay de alerta nao-bloqueante (some sozinho apos SK2_ALERT_MS).
static void sk2DrawAlert() {
  const int mainTop = sk2MainTop();
  const int mainBottom = sk2MainBottom();
  const int h = mainBottom - mainTop;
  if (h <= 0) return;

  tft.fillRect(10, mainTop + 10, SK2_W - 20, 60, RED);
  tft.drawRect(10, mainTop + 10, SK2_W - 20, 60, WHITE);
  sk2PaintText(16, mainTop + 16, "!! SKIMMER DETECTADO !!", WHITE, RED);
  sk2PaintText(16, mainTop + 34, "Sinal Bluetooth suspeito", WHITE, RED);
  sk2PaintText(16, mainTop + 52, "Usando radar RSSI...", WHITE, RED);
}

// ----------------------------------------------------------------------------
// Botoes (nao-bloqueante)
// ----------------------------------------------------------------------------
// Aviso temporario mostrado na area da lista (feedback para botoes que, no
// estado atual, nao teriam nenhuma acao).
static void sk2ShowHint(const char* txt) {
  s_hintText = txt;
  s_hintUntilMs = millis() + 1300;
  s_needFullDraw = true;   // repinta a area da lista com o aviso
}

// Rótulos da barra de toque. Padrao do recurso:
//   LEFT = Exit | DOWN = Next | SELECT = Start/Stop | UP = Prev | RIGHT = Clear
// Next/Prev/Clear so fazem sentido quando existe ao menos um suspeito; sem
// nenhum, viram slots inativos (icone cinza) em vez de botoes "mortos".
static void sk2UpdateNavLabels() {
  if (!featureHasTouchNavBar()) return;
  const bool hasSuspects = (s_hitCount > 0);
  setTouchNavLabels("Exit",
                    hasSuspects ? "Next"  : nullptr,
                    s_scanning ? "Stop" : "Start",
                    hasSuspects ? "Prev"  : nullptr,
                    hasSuspects ? "Clear" : nullptr);
  redrawTouchButtonBar();
}

static void sk2HandleButtons() {
  const uint32_t now = millis();
  if ((uint32_t)(now - s_lastBtnMs) < SK2_BTN_MS) {
    (void)isButtonPressedEdge(BTN_SELECT);
    (void)isButtonPressedEdge(BTN_RIGHT);
    (void)isButtonPressedEdge(BTN_UP);
    (void)isButtonPressedEdge(BTN_DOWN);
    return;
  }

  // SELECT (centro): inicia / para a varredura.
  if (isButtonPressedEdge(BTN_SELECT)) {
    if (s_scanning) {
      sk2StopScan();
      sk2ShowHint("Varredura parada.");
    } else {
      sk2StartBleScan();
      s_phase = Phase::Ble;
      s_phaseStartMs = now;
      sk2ShowHint("Varredura iniciada.");
    }
    s_mainMode = -1;            // indicador/cabecalho precisam ser redesenhados
    s_lastBtnMs = now;
    sk2UpdateNavLabels();
    return;
  }

  // RIGHT: limpa a lista de suspeitos (so faz sentido se houver algum).
  if (isButtonPressedEdge(BTN_RIGHT)) {
    s_lastBtnMs = now;
    if (s_hitCount <= 0) {
      sk2ShowHint("Nada para limpar ainda.");
      return;
    }
    portENTER_CRITICAL(&s_mux);
    s_hitCount = 0;
    s_selected = 0;
    s_hasUpdates = true;
    portEXIT_CRITICAL(&s_mux);
    s_mainMode = -1;
    s_needFullDraw = true;
    sk2ShowHint("Lista limpa.");
    sk2UpdateNavLabels();       // volta ao estado sem suspeitos
    return;
  }

  // UP (Prev) / DOWN (Next): escolhe qual suspeito o radar Geiger rastreia.
  if (isButtonPressedEdge(BTN_UP)) {
    s_lastBtnMs = now;
    if (s_hitCount <= 0) {
      sk2ShowHint("Nenhum suspeito para navegar.");
      return;
    }
    s_selected = (s_selected <= 0) ? (s_hitCount - 1) : (s_selected - 1);
    s_mainMode = -1;
    s_needFullDraw = true;
    return;
  }

  if (isButtonPressedEdge(BTN_DOWN)) {
    s_lastBtnMs = now;
    if (s_hitCount <= 0) {
      sk2ShowHint("Nenhum suspeito para navegar.");
      return;
    }
    s_selected = (s_selected + 1) % s_hitCount;
    s_mainMode = -1;
    s_needFullDraw = true;
    return;
  }
}

static void sk2Teardown() {
  sk2StopScan();
  sk2BleTeardown();
#if !BOARD_HAS_ESP32S3
  if (s_btUp) {
    esp_bt_gap_cancel_discovery();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_deinit();
    s_btUp = false;
  }
#endif
  // Garante que o BLE fique de pe ao sair (outros recursos dependem dele).
  sk2BleEnsure();
}

// ----------------------------------------------------------------------------
// API publica
// ----------------------------------------------------------------------------
void bleSkimmer2Setup() {
  pauseBackgroundRadioTasks();
  setTouchButtonInputEnabled(true);
  setupTouchscreen();

  s_hitCount = 0;
  s_selected = 0;
  s_scanning = false;
  s_phase = Phase::Ble;
  s_phaseStartMs = 0;
  s_alertUntilMs = 0;
  s_scan = nullptr;
  s_listDirty = true;
  s_hasUpdates = false;
  s_needFullDraw = true;
  s_lastUiMs = 0;
  s_lastBtnMs = 0;
  s_drawnCount = -1;
  s_mainMode = -1;
  s_geigerSel = -1;
  s_lastSpinChar = '\0';
  s_lastMeterFilled = -1;
  s_lastEmaShown = -9999;
  s_lastStatusMs = 0;
  s_lastListRssiMs = 0;
  s_hintText = nullptr;
  s_hintUntilMs = 0;
  s_hintWasActive = false;
  s_alertShown = false;

  sk2ClearBody();
  float v = readBatteryVoltage();
  drawStatusBar(v, true);
  redrawTouchButtonBar();

  tft.drawFastHLine(0, 19, SK2_W, UI_LINE);
  sk2PaintText(6, SK2_TITLE_Y, "SKIMMER DETECT 2", WHITE, FEATURE_BG);
  tft.drawFastHLine(0, SK2_HDR_H, SK2_W, UI_LINE);

  sk2DrawHeader(true);
  sk2DrawScanChrome();
  sk2UpdateScanIndicator();
  sk2DrawList();

  // Inicia a varredura BLE (nao-bloqueante) ANTES de rotular a barra: assim o
  // botao central ja aparece como "Stop". Antes ele mostrava "Start" enquanto
  // a varredura ja estava rodando (rotulo definido cedo demais).
  sk2StartBleScan();
  s_phaseStartMs = millis();
  s_mainMode = 0;            // o chrome do modo scan ja foi desenhado acima
  sk2DrawHeader(true);
  sk2UpdateNavLabels();
}

void bleSkimmer2Loop() {
  // LEFT sai (nivel, para loops lentos tambem capturarem).
  if (feature_active && isButtonPressed(BTN_LEFT)) {
    sk2Teardown();
    feature_exit_requested = true;
    return;
  }

  sk2HandleButtons();
  sk2RunScanMachine();

  const uint32_t now = millis();
  bool needList = s_needFullDraw;

  // Coleta as atualizacoes vindas das callbacks (BLE/BT).
  if (s_hasUpdates) {
    int count = 0;
    portENTER_CRITICAL(&s_mux);
    needList = needList || s_listDirty;
    s_hasUpdates = false;
    s_listDirty = false;
    count = s_hitCount;
    portEXIT_CRITICAL(&s_mux);

    if (count != s_drawnCount) {
      needList = true;
      s_drawnCount = count;
      s_mainMode = -1;         // 0 <-> N suspeitos troca o painel scan/Geiger
      sk2DrawHeader(true);
      sk2UpdateNavLabels();    // Next/Prev/Clear passam a valer (ou nao)
    }
  }

  // Aviso temporario: ao aparecer/sumir, repinta so a area da lista.
  const bool hintActive =
      (s_hintText != nullptr) && ((int32_t)(s_hintUntilMs - now) > 0);
  if (hintActive != s_hintWasActive) {
    s_hintWasActive = hintActive;
    needList = true;
  }

  // Painel principal: o "chrome" (caixa/textos) so e redesenhado quando muda
  // de modo (scan <-> Geiger) ou de suspeito selecionado. Era isso que piscava.
  sk2ClampSelection();
  const int mode = (s_hitCount > 0) ? 1 : 0;
  const bool mainChanged = (mode != s_mainMode) ||
                           (mode == 1 && s_selected != s_geigerSel);
  if (mainChanged) {
    if (mode == 1) { sk2DrawGeigerChrome(); sk2UpdateGeigerLive(); }
    else           { sk2DrawScanChrome();  sk2UpdateScanIndicator(); }
    s_mainMode = mode;
    s_geigerSel = (mode == 1) ? s_selected : -1;
  }

  // Atualizacoes leves (~20Hz): indicador de scan ou barra/texto do Geiger.
  if ((uint32_t)(now - s_lastUiMs) >= SK2_UI_MS) {
    s_lastUiMs = now;
    sk2DrawHeader(false);
    if (mode == 1) sk2UpdateGeigerLive();
    else           sk2UpdateScanIndicator();
  }

  // Overlay de alerta: desenhado UMA vez e removido quando expira (sem piscar).
  const bool alertActive =
      (s_alertUntilMs != 0) && ((int32_t)(s_alertUntilMs - now) > 0);
  if (alertActive && !s_alertShown) {
    s_alertShown = true;
    sk2DrawAlert();
  } else if (!alertActive && s_alertShown) {
    s_alertShown = false;
    s_mainMode = -1;           // redesenha o painel sem o overlay
  }

  if (needList || s_needFullDraw) {
    sk2DrawList();
    s_needFullDraw = false;
  }

  // Barra de status ~1x/s (antes era chamada a cada ~2ms, o que tambem pesava).
  if ((uint32_t)(now - s_lastStatusMs) >= 1000) {
    s_lastStatusMs = now;
    updateStatusBar();
  }

  // dBm ao vivo na lista ~1x/s: atualiza SO o campo, sem repintar as linhas
  // (nao pisca) — mesmo comportamento da feature original.
  if (s_hitCount > 0 && !s_hintWasActive &&
      (uint32_t)(now - s_lastListRssiMs) >= 1000) {
    s_lastListRssiMs = now;
    sk2RefreshListRssi();
  }

  maintainTouchNavBar();

  // Libera a CPU para o WDT e para o resto do sistema (botoes responsivos).
  vTaskDelay(pdMS_TO_TICKS(2));
}

void exit() {
  sk2Teardown();
}

}  // namespace BleSkimmer2
