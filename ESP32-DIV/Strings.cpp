#include "Strings.h"
#include "SettingsStore.h"

// One row per StrKey, {EN, PT-BR, ES}, matching declaration order in
// Strings.h exactly (STR_KEY_COUNT rows, compiler enforced by the array
// size below). PT-BR/ES are kept strictly ASCII (no accents) on purpose --
// these strings are drawn with TFT_eSPI's stock bitmap fonts, which only
// cover plain ASCII. See i18n/README.md to add a language or a new key.
const LocStr STRINGS[STR_KEY_COUNT] = {
    // Main menu tiles
    {"WiFi", "WiFi", "WiFi"},                                   // STR_TILE_WIFI
    {"2.4GHz", "2.4GHz", "2.4GHz"},                              // STR_TILE_24GHZ
    {"More", "Mais", "Mas"},                                     // STR_TILE_MORE
    {"Settings", "Configuracoes", "Ajustes"},                    // STR_TILE_SETTINGS
    {"Bluetooth", "Bluetooth", "Bluetooth"},                     // STR_TILE_BLUETOOTH
    {"SubGHz", "SubGHz", "SubGHz"},                               // STR_TILE_SUBGHZ
    {"Tools", "Ferramentas", "Herramientas"},                    // STR_TILE_TOOLS
    {"About", "Sobre", "Acerca de"},                              // STR_TILE_ABOUT

    // "More" tile's own grid
    {"IR Remote", "Controle IR", "Control IR"},                  // STR_TILE_IR
    {"RFID/NFC", "RFID/NFC", "RFID/NFC"},                        // STR_TILE_RFID
    {"GPS", "GPS", "GPS"},                                        // STR_TILE_GPS
    {"Back", "Voltar", "Volver"},                                 // STR_MORE_BACK
    {"Main Menu", "Menu Principal", "Menu Principal"},           // STR_MAIN_MENU

    // Shared trailing nav entry of every paged submenu list
    {"Back to Main Menu", "Voltar ao Menu Principal", "Volver al Menu Principal"}, // STR_BACK_TO_MAIN_MENU

    // Paged-submenu footer's page-advance button
    {"Next Page", "Proxima", "Proxima"},                          // STR_PAGED_NEXT_PAGE
    {"Previous", "Anterior", "Anterior"},                         // STR_PAGED_PREV_PAGE

    // Settings tile's own row labels
    {"Brightness", "Brilho", "Brillo"},                          // STR_SETTINGS_BRIGHTNESS
    {"Theme", "Tema", "Tema"},                                   // STR_SETTINGS_THEME
    {"Accent", "Destaque", "Acento"},                            // STR_SETTINGS_ACCENT
    {"NeoPixel", "NeoPixel", "NeoPixel"},                        // STR_SETTINGS_NEOPIXEL
    {"Auto Scan", "Scan Automatico", "Escaneo Automatico"},      // STR_SETTINGS_AUTO_SCAN
    {"Language", "Idioma", "Idioma"},                            // STR_SETTINGS_LANGUAGE

    // Settings screen's own footer buttons
    {"Back", "Voltar", "Volver"},                                 // STR_SETTINGS_BACK
    {"Save", "Salvar", "Guardar"},                                // STR_SETTINGS_SAVE

    // Settings > Theme row's two values
    {"Dark", "Escuro", "Oscuro"},                                 // STR_SETTINGS_THEME_DARK
    {"Light", "Claro", "Claro"},                                  // STR_SETTINGS_THEME_LIGHT

    // Every on/off switch row's value label (mixed case so PT/ES fit
    // beside the toggle; EN stays ON/OFF to match the original styling)
    {"ON", "Ligado", "Encendido"},                                // STR_SETTINGS_ON
    {"OFF", "Desligado", "Apagado"},                              // STR_SETTINGS_OFF

    // Settings > Accent row's color names
    {"Orange", "Laranja", "Naranja"},                             // STR_ACCENT_ORANGE
    {"Green", "Verde", "Verde"},                                  // STR_ACCENT_GREEN
    {"Red", "Vermelho", "Rojo"},                                  // STR_ACCENT_RED
    {"Cyan", "Ciano", "Cian"},                                    // STR_ACCENT_CYAN
    {"Purple", "Roxo", "Morado"},                                 // STR_ACCENT_PURPLE
    {"Yellow", "Amarelo", "Amarillo"},                            // STR_ACCENT_YELLOW
    {"White", "Branco", "Blanco"},                                // STR_ACCENT_WHITE

    // About screen
    {"by ", "Por ", "Por "},                                      // STR_ABOUT_BY
    {"Board", "Placa", "Placa"},                                  // STR_ABOUT_BOARD
    {"Built-in", "Embutido", "Integrado"},                        // STR_ABOUT_BUILTIN
    {"Installed", "Instalado", "Instalado"},                      // STR_ABOUT_INSTALLED
    {"Supported", "Suportado", "Soportado"},                      // STR_ABOUT_SUPPORTED
    {"Unsupported", "Nao suportado", "No soportado"},             // STR_ABOUT_UNSUPPORTED
    {"SELECT / tap to go back", "Toque para voltar", "Toque para volver"}, // STR_ABOUT_TAP_TO_GO_BACK

    // WiFi features
    {"Packet Monitor", "Monitor de Pacotes", "Monitor de Paquetes"},             // STR_WIFI_PACKET_MONITOR
    {"Beacon Spammer", "Spammer de Beacons", "Spammer de Beacons"},              // STR_WIFI_BEACON_SPAMMER
    {"WiFi 2.4GHz Deauther", "Desautenticador WiFi 2.4GHz", "Desautenticador WiFi 2.4GHz"}, // STR_WIFI_24GHZ_DEAUTHER
    {"Probe Request Flood", "Inundacao de Probe Request", "Inundacion de Probe Request"},   // STR_WIFI_PROBE_REQUEST_FLOOD
    {"Deauth Detector", "Detector de Desautenticacao", "Detector de Desautenticacion"},     // STR_WIFI_DEAUTH_DETECTOR
    {"WiFi 2.4GHz Scanner", "Scanner WiFi 2.4GHz", "Escaner WiFi 2.4GHz"},       // STR_WIFI_24GHZ_SCANNER
    {"Captive Portal", "Portal Cativo", "Portal Cautivo"},                       // STR_WIFI_CAPTIVE_PORTAL
    {"Hidden SSID Revealer", "Revelador de SSID Oculto", "Revelador de SSID Oculto"},       // STR_WIFI_HIDDEN_SSID_REVEALER
    {"WPS Scanner", "Scanner WPS", "Escaner WPS"},                              // STR_WIFI_WPS_SCANNER
    {"ARP Scanner", "Scanner ARP", "Escaner ARP"},                              // STR_WIFI_ARP_SCANNER
    {"Karma Attack", "Ataque Karma", "Ataque Karma"},                           // STR_WIFI_KARMA_ATTACK
    {"Channel Graph", "Grafico de Canais", "Grafico de Canales"},               // STR_WIFI_CHANNEL_GRAPH

    // Bluetooth features
    {"BLE Jammer", "Bloqueador BLE", "Bloqueador BLE"},                         // STR_BT_BLE_JAMMER
    {"BLE Spoofer", "Falsificador BLE", "Suplantador BLE"},                     // STR_BT_BLE_SPOOFER
    {"Sour Apple", "Sour Apple", "Sour Apple"},                                 // STR_BT_SOUR_APPLE
    {"AirTag Spoofer", "Falsificador de AirTag", "Suplantador de AirTag"},      // STR_BT_AIRTAG_SPOOFER
    {"AirTag Sniffer", "Sniffer de AirTag", "Sniffer de AirTag"},               // STR_BT_AIRTAG_SNIFFER
    {"Sniffer", "Sniffer", "Sniffer"},                                         // STR_BT_SNIFFER
    {"BLE Scanner", "Scanner BLE", "Escaner BLE"},                             // STR_BT_BLE_SCANNER
    {"BLE Rubber Ducky", "BLE Rubber Ducky", "BLE Rubber Ducky"},              // STR_BT_BLE_RUBBER_DUCKY
    {"Skimmer Detect", "Deteccao de Skimmer", "Deteccion de Skimmer"},         // STR_BT_SKIMMER_DETECT

    // nRF24 features
    {"Scanner", "Scanner", "Escaner"},                                          // STR_NRF_SCANNER
    {"Proto Kill", "Derrubar Protocolo", "Derribar Protocolo"},                 // STR_NRF_PROTO_KILL
    {"ESB Sniffer", "Sniffer ESB", "Sniffer ESB"},                             // STR_NRF_ESB_SNIFFER
    {"ESB Replay", "Replay ESB", "Replay ESB"},                                // STR_NRF_ESB_REPLAY
    {"MouseJack Scan", "Scan MouseJack", "Escaneo MouseJack"},                 // STR_NRF_MOUSEJACK_SCAN
    {"MouseJack Inject", "Injecao MouseJack", "Inyeccion MouseJack"},          // STR_NRF_MOUSEJACK_INJECT

    // SubGHz features
    {"Replay Attack", "Ataque de Replay", "Ataque de Replay"},                 // STR_SUBGHZ_REPLAY_ATTACK
    {"SubGHz Jammer", "Bloqueador SubGHz", "Bloqueador SubGHz"},               // STR_SUBGHZ_JAMMER
    {"De Bruijn / Brute", "De Bruijn / Forca Bruta", "De Bruijn / Fuerza Bruta"}, // STR_SUBGHZ_DE_BRUIJN_BRUTE
    {"Jamming Detector", "Detector de Interferencia", "Detector de Interferencia"}, // STR_SUBGHZ_JAMMING_DETECTOR
    {"Saved Profiles", "Perfis Salvos", "Perfiles Guardados"},                 // STR_SUBGHZ_SAVED_PROFILE

    // Tools features
    {"Serial Monitor", "Monitor Serial", "Monitor Serie"},                     // STR_TOOLS_SERIAL_MONITOR
    {"Update Firmware", "Atualizar Firmware", "Actualizar Firmware"},          // STR_TOOLS_UPDATE_FIRMWARE
    {"Touch Calibrate", "Calibrar Touch", "Calibrar Tactil"},                  // STR_TOOLS_TOUCH_CALIBRATE
    {"SD File Manager", "Gerenciador de Arquivos SD", "Administrador de Archivos SD"}, // STR_TOOLS_SD_FILE_MANAGER

    // RFID features
    {"Card Reader", "Leitor de Cartao", "Lector de Tarjeta"},                  // STR_RFID_CARD_READER
    {"Card Clone", "Clonar Cartao", "Clonar Tarjeta"},                         // STR_RFID_CARD_CLONE
    {"Erase", "Apagar", "Borrar"},                                             // STR_RFID_ERASE
    {"Dump", "Dump", "Dump"},                                                  // STR_RFID_DUMP
    {"Decode Access", "Decodificar Acesso", "Decodificar Acceso"},            // STR_RFID_DECODE_ACCESS
    {"Jam Reader", "Bloquear Leitor", "Bloquear Lector"},                     // STR_RFID_JAM_READER
    {"Tag Disrupt", "Interromper Tag", "Interrumpir Tag"},                    // STR_RFID_TAG_DISRUPT
    {"Disrupt Emulate", "Interromper e Emular", "Interrumpir y Emular"},      // STR_RFID_DISRUPT_EMULATE

    // GPS features
    {"Wardriver", "Wardriver", "Wardriver"},                                  // STR_GPS_WARDRIVER
    {"Satellite Scanner", "Scanner de Satelites", "Escaner de Satelites"},    // STR_GPS_SATELLITE_SCANNER

    // IR features
    {"Record", "Gravar", "Grabar"},                                           // STR_IR_RECORD
    {"Saved Profile", "Perfil Salvo", "Perfil Guardado"},                     // STR_IR_SAVED_PROFILE
    {"Universal Controller", "Controle Universal", "Control Universal"},     // STR_IR_UNIVERSAL_CONTROLLER
    {"Universal Controller A/C", "Controle Universal A/C", "Control Universal A/C"}, // STR_IR_UNIVERSAL_CONTROLLER_AC

    // Settings > Language picker footer hint (two lines, kept short to fit 240px)
    {"UP/DOWN: move   SELECT: apply", "UP/DOWN: mover   SELECT: aplicar", "UP/DOWN: mover   SELECT: aplicar"}, // STR_LANG_PICKER_HINT1
    {"LEFT: cancel", "LEFT: cancelar", "LEFT: cancelar"},        // STR_LANG_PICKER_HINT2

    // SubGHz > Replay Attack nav-bar labels
    {"Exit", "Sair", "Salir"},                                   // STR_NAV_EXIT
    {"Send", "Enviar", "Enviar"},                                // STR_NAV_SEND
    {"Save", "Salvar", "Guardar"},                               // STR_NAV_SAVE
    {"Freq+", "Freq+", "Freq+"},                                 // STR_NAV_FREQ_UP
    {"Freq-", "Freq-", "Freq-"},                                 // STR_NAV_FREQ_DOWN
    {"No valid signal", "Nenhum sinal valido", "Ninguna senal valida"}, // STR_REPLAY_NO_SIGNAL

    // More SubGHz nav-bar labels (short to fit 48px cells)
    {"Back", "Voltar", "Volver"},                                // STR_NAV_BACK
    {"On/Off", "On/Off", "On/Off"},                              // STR_NAV_TOGGLE
    {"Auto", "Auto", "Auto"},                                    // STR_NAV_AUTO
    {"Next", "Prox.", "Sig."},                                   // STR_NAV_NEXT
    {"Prev", "Ant.", "Ant."},                                    // STR_NAV_PREV
    {"TX", "TX", "TX"},                                          // STR_NAV_TX
    {"Delete", "Apagar", "Borrar"},                              // STR_NAV_DELETE
    {"View", "Ver", "Ver"},                                      // STR_NAV_VIEW
    {"Go", "Ir", "Ir"},                                          // STR_NAV_GO
    {"Sel", "Sel", "Sel"},                                       // STR_NAV_SEL
    {"Log", "Log", "Log"},                                       // STR_NAV_LOG
    {"Reset", "Zerar", "Reset"},                                 // STR_NAV_RESET
    {"Rename", "Renom.", "Renom."},                              // STR_NAV_RENAME

    // Saved-profile View screen delete confirmation (body text)
    {"Press Delete again to confirm", "Aperte Apagar de novo p/ confirmar", "Pulsa Borrar de nuevo p/ confirmar"}, // STR_PROFILE_DELETE_CONFIRM
};

const char* t(StrKey key) {
  if (key >= STR_KEY_COUNT) return "?";  // guard against a bad/stale index
  return locText(STRINGS[key], settings().infoLang);
}
