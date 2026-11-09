#ifndef SKIMMER_DETECT2_H
#define SKIMMER_DETECT2_H

// ============================================================================
//  Skimmer Detect 2 — Detector avancado de skimmers Bluetooth
// ----------------------------------------------------------------------------
//  Diferenca em relacao ao "Skimmer Detect" (namespace BleSkimmer):
//    * Varredura hibrida (DUAL-MODE): alterna Bluetooth Classico (Inquiry) e
//      BLE (Scan). No ESP32-S3 (placa ESP32-DIV V2) o Bluetooth Classico nao
//      existe em hardware, entao o modo Classico e desativado em tempo de
//      compilacao e o scanner roda 100% em BLE — a UI informa isso.
//    * Motor de heuristica em 3 camadas (OUI/MAC, Classe+Nome, Servicos/UUID)
//      que pontua (score 0..100) em vez de simplesmente listar tudo.
//    * UI nao-bloqueante com "Contador Geiger" para localizar fisicamente o
//      alvo: grafico de barras suavizado por EMA (Media Movel Exponencial).
//
//  A interface publica segue exatamente o padrao dos demais namespaces do
//  menu Bluetooth (bleSkimmerSetup/bleSkimmerLoop/exit), para integrar no
//  dispatch de ESP32-DIV.ino sem surpresas.
// ============================================================================

namespace BleSkimmer2 {

// Chamado ao entrar no recurso. Inicializa a tela, o radio (NimBLE e, quando
// suportado, Bluetooth Classico) e inicia a primeira fase da varredura.
void bleSkimmer2Setup();

// Loop de servico. Deve ser chamado em um while(!feature_exit_requested).
// Nunca bloqueia: entre cada iteracao ha yield()/vTaskDelay, e os botoes
// fisicos (LEFT = sair, SELECT/UP/DOWN = navegar, RIGHT = limpar) continuam
// responsivos.
void bleSkimmer2Loop();

// Chamado ao sair (ou quando feature_exit_requested vira true). Libera o
// radio e o heap de forma ordenada para evitar leak e reset por WDT.
void exit();

}  // namespace BleSkimmer2

#endif  // SKIMMER_DETECT2_H
