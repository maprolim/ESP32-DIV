# Relatório de mudanças — fork ESP32-DIV (CiferTech)

Gerado comparando o working tree atual com `HEAD` (`90f7967`, igual a
`origin/main`, v1.7.2) via `git diff HEAD`. Cobre os 9 arquivos modificados
(`ESP32-DIV.ino`, `config.h`*, `gps.cpp`, `icon.h`, `ir.cpp`, `ir.h`,
`shared.h`, `utils.cpp`, `utils.h`, `wifi.cpp`) e os 2 arquivos novos
(`hwdetect.cpp`, `hwdetect.h`).

\* `config.h` aparece em "Changes to be committed" mas o diff contra HEAD
está vazio — sem alteração líquida, não entra no relatório.

**Escopo/uso:** como já registrado no `HANDOFF.md`, este aparelho é usado
só para estudo de cibersegurança em equipamento/rede próprios ou com
autorização. Isso vale também para as mudanças do Deauther/Probe Flood
abaixo.

---

## 1. Bateria via IP5306 (I2C) — `utils.cpp`, `shared.h`, `utils.h`

**Problema original:** no V2, `BATTERY_ADC_PIN = -1` (o divisor fica
grampeado pelo transistor do buzzer no GPIO2) → `readBatteryVoltage()`
sempre lia ~0% pela ADC inválida.

**O que foi feito:**
- `shared.h`: comentário documentando a causa (pino ADC inválido).
- `utils.cpp`: nova leitura via I2C no IP5306 (endereço `0x75`):
  - `i2cBusScan()` — scanner de diagnóstico no boot (chamado 1x em
    `initPcf8574Buttons()`), lista os endereços I2C encontrados.
  - `ip5306ReadReg()` / `readIp5306Percent()` — lê o registrador `0x78`,
    decodifica o nível em degraus de 25% (`100 - popcount(nibble_alto)*25`),
    com retries e cache (não derruba o barramento, não "latcha" ausência).
  - `readBatteryVoltage()` passa a sintetizar uma tensão 3.00–4.20V a
    partir do % lido quando `BATTERY_ADC_PIN < 0`, mantendo o resto da UI
    (que já trabalha em tensão) sem alteração.
  - `ip5306ChargeStatus()` — lê os registradores `0x70`/`0x71` para saber
    se está **na bateria / carregando / conectado e cheio**.
  - `drawStatusBar()` / `statusBarTask()`: indicador de bateria agora
    anima o preenchimento (1/4→4/4) enquanto carrega, e mostra `!` depois
    da porcentagem quando conectado e cheio.
- **Pendência de limpeza:** `ip5306ChargeStatus()` e `drawStatusBar()`
  ainda têm blocos marcados `// TEMP DIAG` que imprimem no `Serial` e
  desenham um `s<N>` roxo na status bar a cada mudança de estado — útil
  pra bancada, mas **viola o guia de contribuição** ("Do not use
  `Serial.print` in production code paths") e precisa saída antes do PR.

**Status:** verificado em bancada (handoff registra 100% exibido).

---

## 2. GPS — "Display buffer failed" — `gps.cpp`

- `kScanSpriteHMin` reduzido de 200 para 90 (permite sprite menor quando a
  heap está baixa).
- Quando nenhum sprite aloca, em vez de só mostrar a mensagem de erro, o
  painel agora é desenhado **direto na TFT** via `tft.setViewport(...)` +
  `renderPanelGx(tft)` — a tela do Satellite Scanner funciona mesmo sem
  memória pro sprite.

**Status:** verificado (painel desenha; "NOFIX" remanescente é falta de
sinal do módulo GPS, não bug do firmware).

---

## 3. Beacon Spammer — lista de SSID customizável via SD — `wifi.cpp`

- `loadSsidListFromSd()`: lê `/ssid_list.txt` do SD (1 nome por linha,
  ≤32 chars, até `BEACON_SD_MAX_SSIDS=64`), chamado em `beaconSpamSetup()`.
- `activeSsid()` / `activeSsidCount()` substituem o array fixo
  `ssidList[]` nos pontos de uso (`output()`, `spammer()`,
  `beaconSpam()`), com fallback pro array embutido se o SD/arquivo não
  existir ou estiver vazio.
- Usa `isSDCardAvailable()` (o mesmo helper de montagem compartilhada do
  barramento SPI que os outros módulos já usam) — não introduz um
  caminho de SD paralelo.

**Status:** verificado (handoff registra teste com 3 nomes + fallback).

---

## 4. Captive Portal — página de login editável via SD — `wifi.cpp`

- Novo: na primeira execução, `cpSeedPortalFilesOnSD()` grava
  `/captive_portal/login.html` (com a página embutida atual) e um
  `/captive_portal/README.txt` explicando o contrato obrigatório (POST
  para `/login`, campo `password`, campo opcional `username`, botão
  submit) — sem isso o firmware não registra as credenciais digitadas.
- `cpLoadLoginPageFromSD()` recarrega `login.html` do SD em todo boot do
  Captive Portal (chamado em `cportalSetup()`), com fallback pra página
  embutida se o SD não montar, o arquivo faltar ou for muito pequeno.
- Nunca sobrescreve o arquivo do usuário (`cpWriteFileIfMissing` só grava
  se o arquivo não existir).

Isso é o mesmo padrão do item 3 (seed + fallback + não pisa no arquivo do
usuário), aplicado a outra feature. **Lembrete de memória:** isso já tinha
sido registrado como preferência de uso ([captive-portal-editavel]).

---

## 5. Deauther / Probe Request Flood — correção de bug real + remapeamento de botões — `wifi.cpp`

### 5.1 Bug corrigido (heap guard bloqueava o ataque)
`deautherLoop()` e `probeRequestFloodLoop()` abortavam o ataque
(`attack_running = false`) sempre que `ESP.getFreeHeap() < 80000`. Nesta
build (BLE + scanners de fundo + UI), a heap livre de baseline fica
**~77 KB** — ou seja, o guard original abortava o ataque *antes de
qualquer pacote ser enviado*, em praticamente toda execução. O limite foi
reduzido para **30000** (um frame de deauth/probe de 26 bytes não precisa
de quase nada de heap; 30 KB ainda é uma margem de segurança real contra
OOM).

> Nota: o `HANDOFF.md` tinha uma hipótese diferente (rádio ficar em modo
> STA no primeiro TX). Essa hipótese **não foi implementada** — nenhuma
> chamada a `esp_wifi_set_mode(WIFI_MODE_AP)` foi adicionada em
> `deautherSetup()`/início do ataque; o `checkApChannel()` que seta o modo
> AP continua rodando só a cada 15s, como antes. O fix que de fato entrou
> no código foi apenas o do guard de heap. Vale confirmar num
> aparelho-vítima próprio se isso resolveu o "não muda nada" por completo,
> ou se o atraso de até 15s pro modo AP subir ainda é perceptível.

### 5.2 Remapeamento de navegação (Deauther, Probe Flood, Packet Monitor, Beacon Spammer)
Padronização de layout em várias features de WiFi:
- Lista de scan: **LEFT = Exit, RIGHT = Rescan, SELECT = View/abrir alvo**
  (antes: LEFT = Rescan, SELECT = Exit, RIGHT = abrir alvo).
- Tela de ataque (View): **LEFT = Back (volta pra lista), SELECT =
  Start/Stop** (antes: LEFT = Start/Stop, RIGHT = Back).
- Beacon Spammer: **LEFT = Exit, SELECT = Start/Stop, RIGHT = Flood**,
  canal em **UP/DOWN** (antes canal era LEFT/RIGHT).
- Packet Monitor: **LEFT = Exit** (antes SELECT).

Cada ponto alterado tem comentário `// Layout remapeado: ...` explicando a
origem/destino do botão — útil pra revisão, mas também sinal de que isso
deveria ser um PR separado do fix de heap (são mudanças independentes no
mesmo arquivo).

---

## 6. Debounce central dos botões físicos — `ESP32-DIV.ino`

Reescrita de `isPhysicalButtonPressed()` / `isButtonPressedEdge()`:
- Novo modelo **assimétrico**: aperto aceito imediatamente (responsivo),
  solta só confirmada após `BTN_RELEASE_MS = 40ms` contínuos (filtra
  leitura espúria do PCF8574, que é lido por I2C concorrente com outro
  core). Resolve o "apertei 1x e contou 2x".
- Contadores de diagnóstico `g_btnRawDown[]` / `g_btnStbDown[]` (bordas
  cruas vs. debounced), expostos na tela de Settings.
- Nova `waitButtonReleased(int pin)` (em `utils.h`/`.ino`): bloqueia até o
  botão ficar solto de fato por `BTN_ACTION_RELEASE_MS = 60ms`, substitui
  os antigos `delay(200)` espalhados pelos handlers de menu — evita que
  um toque humano (>200ms) seja relido como uma 2ª ação (cursor andando
  demais, submenu relendo o toque que abriu ele, etc.).
- Essa troca foi aplicada em quase todos os handlers de submenu
  (`handleWiFiSubmenuButtons`, `handleBluetoothSubmenuButtons`,
  `handleNRFSubmenuButtons`, `handleSubGHzSubmenuButtons`,
  `handleToolsSubmenuButtons`, `handleOtherSubmenuButtons`,
  `handleButtons`) e no menu de Settings (`utils.cpp`).

**Isso já está registrado em memória** ([botoes-debounce-central]) como
regra: não ler o PCF cru, sempre passar por essa camada.

---

## 7. UX de navegação dos submenus — `ESP32-DIV.ino`

- **"<" físico volta ao menu anterior** em todos os submenus (WiFi,
  Bluetooth, NRF, SubGHz, Tools, Other/IR/RFID/GPS) — antes só dava pra
  voltar pelo touch ou não existia volta física consistente.
- **Grade do menu principal (2 colunas × 4 linhas)**: navegação LEFT/RIGHT
  corrigida pra andar em "ordem de leitura" (mesma linha troca de coluna;
  na borda, passa pra linha seguinte/anterior) em vez da lógica antiga.
- **Tela de informação por feature (BTN_RIGHT)**: `drawFeatureInfoScreen`
  / `showFeatureInfoScreen` — abre uma tela cheia com descrição em
  EN **e** PT-BR de cada item, pra quase todos os submenus (WiFi page0,
  Bluetooth page0, NRF, SubGHz, Tools, IR, RFID, GPS). Strings novas:
  `wifi_page0_info_en/pt`, `bluetooth_page0_info_en/pt`, `nrf_info_en/pt`,
  `subghz_info_en/pt`, `tools_info_en/pt`, `rfid_info_en/pt`,
  `gps_info_en/pt`, `ir_info_en/pt`.
- **Tela de Settings**: modelo de edição revisado — agora é preciso
  apertar SELECT para "entrar" na edição de uma opção (barra lateral fica
  verde) antes de LEFT/RIGHT alterarem o valor; ao saír com alterações
  pendentes, aparece um diálogo **"Salvar alterações?" (Salvar / Não
  salvar)** em vez de salvar/descartar silenciosamente.

---

## 8. Detecção de hardware no boot + seção "HARDWARE" no About — `hwdetect.cpp`/`.h` (novos), `ESP32-DIV.ino`

- Novo módulo `hwdetect.cpp`/`.h`: `hwDetectAll()` roda uma vez no
  `setup()` (antes dos scanners de fundo e do touchscreen, por causa do
  compartilhamento de GPIO5/barramentos SPI) e detecta:
  - **nRF24 (2.4GHz)** — `RF24::begin()` + `isChipConnected()`.
  - **CC1101 (Sub-GHz)** — leitura direta do pino MISO com *bounded wait*
    de CHIP_RDY (a lib SmartRC trava num `while` infinito se o chip não
    responder; isso foi contornado com timeout de 5ms e deselect
    explícito do SD/nRF24 pra não segurar o MISO em low). Esse gotcha já
    estava registrado em memória ([about-hardware-detection]).
  - **PN532 (NFC/RFID)** — reaproveita `RfidNfc::begin()`.
  - **GPS** — escuta a UART2 por até 1.2s esperando qualquer byte NMEA.
- `handleAboutPage()` (`.ino`) ganhou a seção **HARDWARE**, mostrando
  Built-in / Installed / Supported / Unsupported conforme o resultado do
  `g_hwPresence` cacheado.

---

## 9. Novo recurso: IR Universal Controller A/C + refator do Universal Controller — `ir.cpp`, `ir.h`

- Novo namespace **`IRUniversalAC`** (`ir.h`/`ir.cpp`): um controle
  universal dedicado a ar-condicionados (perfis `AcProfile` com
  defaults/comandos, tela de lista + detalhes + controle, favoritos/
  recentes salvos — mesmo padrão do `IRUniversalController` existente).
  Registrado no menu IR como item **"Universal Controller A/C"**
  (`ir_NUM_SUBMENU_ITEMS` 4→5, novo ícone `bitmap_icon_temp`).
- O `IRUniversalController` existente também foi reescrito por baixo
  (lista com cabeçalhos/seções, `rebuildOrder`/`rebuildRecent`, navegação
  por `ctrlNav`/`ctrlGo`) — é a maior parte das ~1700 linhas alteradas em
  `ir.cpp`; vale revisar com calma antes de abrir PR porque mistura
  "feature nova" com "reescrita de feature existente" no mesmo arquivo.

---

## 10. Miscelânea

- `icon.h`: novo ícone `bitmap_icon_house` (16×16, "casa") — usado no
  controle universal; mais limpeza de trailing whitespace na entrada do
  `bitmap_icon_power`.

---

## Antes de abrir o PR — checklist

O `CONTRIBUTING.md` do CiferTech pede PRs **focados** ("one feature or
fix per PR") contra a branch `dev`, sem `Serial.print` em produção, e
teste em hardware real. O estado atual é **um único working tree** com
pelo menos 6 mudanças independentes. Sugestão de split (branch por
tema, nomes seguindo o padrão `feat/`, `fix/` do próprio guia):

| Branch sugerida | Conteúdo |
|---|---|
| `fix/v2-battery-ip5306` | item 1 (tirar os blocos `TEMP DIAG` antes) |
| `fix/gps-display-buffer` | item 2 |
| `feat/beacon-custom-ssid-sd` | item 3 |
| `feat/captive-portal-editable-page` | item 4 |
| `fix/deauther-probe-heap-guard` | só o 5.1 (o fix de verdade) |
| `feat/wifi-nav-remap` | 5.2 (remap de botões) — ou junto do item 7 |
| `fix/button-debounce-central` | item 6 |
| `feat/submenu-ux-info-screens-settings-confirm` | item 7 |
| `feat/hardware-detection-about` | item 8 |
| `feat/ir-universal-ac` | item 9 (considerar separar a reescrita do controller existente, se possível, de uma reescrita pura de refactor) |

Outros pontos a resolver antes de submeter:
- Remover os blocos `// TEMP DIAG` em `utils.cpp` (`ip5306ChargeStatus()`
  e `drawStatusBar()`) — imprimem no Serial e desenham um `s<N>` extra na
  status bar; útil em bancada, não deve ir pro PR.
- Confirmar em hardware real (v2) cada item antes de marcar como testado
  no template de PR — o `CONTRIBUTING.md` exige isso explicitamente.
- Para o item 5.1 (Deauther/Probe), relembrar no corpo do PR que o teste
  só é válido contra um **aparelho-vítima próprio**, em bancada, com
  autorização — e citar que o guard de heap era o bug real (não a
  hipótese de modo STA do `HANDOFF.md`).
