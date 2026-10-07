#include "LangInfo.h"

// Order matches wifi_page0_items in ESP32-DIV.ino.
const InfoText wifi_page0_info[8] = {
    {"Analyzes network traffic by capturing data packets in the air, allowing you to visualize activity, protocol types, and the volume of information flowing in the environment.",
     "Analisa o trafego da rede capturando pacotes de dados no ar, permitindo visualizar a atividade, os tipos de protocolo e o volume de informacoes trafegadas no ambiente."},
    {"Instantly creates dozens of fake Wi-Fi networks, flooding the available connections list on nearby devices with random or custom names.",
     "Cria dezenas de redes Wi-Fi falsas de forma instantanea, inundando a lista de conexoes disponiveis nos dispositivos proximos com nomes aleatorios ou personalizados."},
    {"Disconnects targets from a Wi-Fi network by sending spoofed deauthentication packets, tricking both the router and the device into dropping the connection.",
     "Desconecta alvos de uma rede Wi-Fi enviando pacotes de desautenticacao falsificados, enganando o roteador e o dispositivo para que encerrem a conexao."},
    {"Sends thousands of mass network search requests to nearby routers, testing the equipment's stability against overload and denial-of-service attacks.",
     "Envia milhares de solicitacoes de busca de rede em massa para roteadores proximos, testando a estabilidade dos equipamentos contra ataques de sobrecarga e negacao de servico."},
    {"Actively listens to the environment for deauthentication packets, acting as an alarm to indicate if any nearby network is under a disconnection attack.",
     "Escuta ativamente o ambiente em busca de pacotes de desautenticacao, servindo como um alarme para indicar se alguma rede proxima esta sofrendo um ataque de desconexao."},
    {"Scans local radio channels to map all Wi-Fi networks and the devices (clients) connected to them, displaying information such as signal strength, channel, and MAC address.",
     "Varre os canais de radio locais para mapear todas as redes Wi-Fi e os dispositivos (clientes) conectados a elas, exibindo informacoes como forca do sinal, canal e endereco MAC."},
    {"Stands up a fake open network (Evil Twin) that redirects anyone who connects to a controlled login page, used in phishing tests to capture passwords.",
     "Levanta uma rede aberta falsa (Evil Twin) que redireciona qualquer pessoa conectada para uma pagina de login controlada, usada em testes de phishing para capturar senhas."},
    {"Discovers the name of hidden Wi-Fi networks by waiting for or forcing a legitimate device to connect, capturing the network name when association packets are exchanged.",
     "Descobre o nome de redes Wi-Fi ocultas aguardando ou forcando a conexao de um dispositivo legitimo, capturando o nome da rede no momento em que os pacotes de associacao sao trocados."},
};

// Order matches bluetooth_page0_items in ESP32-DIV.ino.
const InfoText bluetooth_page0_info[8] = {
    {"Floods the Bluetooth spectrum attempting to disrupt and block nearby connections.",
     "Inunda o espectro Bluetooth para tentar interromper e bloquear conexoes proximas."},
    {"Spoofs Bluetooth device identifications to impersonate other devices.",
     "Falsifica identificacoes de dispositivos Bluetooth para se passar por outros aparelhos."},
    {"Sends malformed packets aimed at crashing or generating constant pairing notifications on nearby Apple devices.",
     "Envia pacotes malformados com o objetivo de travar ou gerar notificacoes de pareamento constantes em dispositivos Apple proximos."},
    {"Emulates an Apple AirTag signal, creating fake trackers visible to iPhones in the area.",
     "Emula o sinal de um Apple AirTag, criando rastreadores falsos visiveis para iPhones na area."},
    {"Scans the area to detect, list, and track the presence of legitimate Apple AirTags around you.",
     "Varre a area para detectar, listar e rastrear a presenca de Apple AirTags legitimos ao redor."},
    {"Captures raw data packets for monitoring and technical analysis of local traffic.",
     "Captura pacotes brutos de dados para monitoramento e analise tecnica do trafego local."},
    {"Lists active Bluetooth Low Energy (BLE) devices in the area, displaying MAC addresses and signal strength.",
     "Lista os dispositivos Bluetooth Low Energy (BLE) ativos na area, exibindo enderecos MAC e a intensidade do sinal."},
    {"Emulates a keyboard via Bluetooth connection to rapidly inject automated commands and scripts into the target device.",
     "Emula um teclado via conexao Bluetooth para injetar comandos automaticos e scripts rapidamente no aparelho alvo."},
};

// Order matches nrf_submenu_items in ESP32-DIV.ino, skipping "Back to Main Menu".
const InfoText nrf_info[6] = {
    {"Performs a general scan to find devices operating and emitting signals on the configured radio frequency.",
     "Faz uma varredura geral para encontrar dispositivos operando e emitindo sinais na frequencia de radio configurada."},
    {"Attempts to drop or disrupt communication of specific wireless radio protocols.",
     "Tenta derrubar ou interromper a comunicacao de protocolos especificos de radio sem fio."},
    {"Captures Enhanced ShockBurst protocol packets, widely used in wireless mouse and keyboard communications.",
     "Captura pacotes do protocolo Enhanced ShockBurst, muito utilizado na comunicacao de mouses e teclados sem fio."},
    {"Resends previously captured ESB packets to repeat the legitimate command of a keyboard or mouse.",
     "Reenvia pacotes ESB capturados anteriormente para repetir o comando legitimo de um teclado ou mouse."},
    {"Actively searches for 2.4GHz wireless mice and keyboards that have known vulnerabilities (MouseJack flaw).",
     "Procura ativamente por mouses e teclados sem fio de 2.4GHz que possuam vulnerabilidades conhecidas (falha MouseJack)."},
    {"Exploits found flaws in wireless peripherals to remotely inject malicious keystrokes into the victim's computer.",
     "Explora as falhas encontradas em perifericos sem fio para injetar digitacao maliciosa remotamente no computador da vitima."},
};

// Order matches subghz_submenu_items in ESP32-DIV.ino, skipping "Back to Main Menu".
const InfoText subghz_info[5] = {
    {"Records a captured radio signal (like an alarm or gate remote) and transmits it again to trigger the system.",
     "Grava um sinal de radio capturado (como o de um alarme ou controle de portao) e o transmite novamente para acionar o sistema."},
    {"Generates intentional noise on frequencies below 1GHz to block the reception of remote control and alarm signals.",
     "Gera ruido intencional em frequencias abaixo de 1GHz para bloquear a recepcao de sinais de controles remotos e alarmes."},
    {"Attempts to guess the code of older radio receivers (like DIP switch gates) by generating and transmitting continuous brute-force sequences.",
     "Tenta adivinhar o codigo de receptores de radio antigos (como portoes de chave DIP) gerando e transmitindo sequencias continuas de forca bruta."},
    {"Listens to the environment to passively identify if there is any signal jamming device operating in the area.",
     "Escuta o ambiente para identificar de forma passiva se existe algum aparelho bloqueador de sinal operando na area."},
    {"Allows you to access and transmit radio signals you had previously saved to the memory card.",
     "Permite acessar e transmitir os sinais de radio que voce ja havia salvo no cartao de memoria."},
};

// Order matches tools_submenu_items in ESP32-DIV.ino, skipping "Back to Main Menu".
const InfoText tools_info[4] = {
    {"Shows the logs, debugging info, and internal system messages of the device directly on its own screen.",
     "Mostra os logs, informacoes de debug e as mensagens do sistema interno do aparelho diretamente na propria tela."},
    {"Allows you to update or reinstall the device's operating system by executing the update file directly from the SD card.",
     "Permite atualizar ou reinstalar o sistema operacional do aparelho executando o arquivo de atualizacao direto pelo cartao SD."},
    {"Launches the calibration screen to readjust the precision and alignment of the display's touchscreen function.",
     "Inicia a tela de calibracao para reajustar a precisao e o alinhamento da funcao touchscreen do visor."},
    {"Opens an internal folder explorer so you can navigate, view, or delete captures and files stored on the memory card.",
     "Abre um explorador interno de pastas para que voce possa navegar, visualizar ou excluir as capturas e arquivos armazenados no cartao de memoria."},
};

// Order matches rfid_submenu_items in ESP32-DIV.ino, skipping "Back to Main Menu".
const InfoText rfid_info[8] = {
    {"Scans proximity cards or tags to display their open data and Unique Identification (UID) number.",
     "Escaneia cartoes ou tags de aproximacao para exibir seus dados abertos e o numero de identificacao unico (UID)."},
    {"Copies the extracted data from an original card directly onto a compatible and rewritable blank card.",
     "Copia os dados extraidos de um cartao original diretamente para um cartao virgem compativel e regravavel."},
    {"Formats and completely erases all data written on a rewritable card or tag.",
     "Formata e apaga completamente todos os dados gravados em um cartao ou tag regravavel."},
    {"Deeply extracts and saves the entire block and sector structure of the card's memory to a file on the SD.",
     "Extrai de forma profunda e salva toda a estrutura de blocos e setores da memoria do cartao em um arquivo no SD."},
    {"Uses known key dictionaries to attempt cracking the security of protected access cards to read their content.",
     "Usa dicionarios de chaves conhecidas para tentar quebrar a seguranca de cartoes de acesso protegidos para conseguir ler seu conteudo."},
    {"Emits continuous noise to temporarily interfere with the operation of a physical wall reader.",
     "Emite ruido continuo para interferir temporariamente no funcionamento de um leitor fisico de parede."},
    {"Causes purposeful instability in the communication between the tag and the reader, corrupting the reading momentarily.",
     "Causa instabilidade proposital na comunicacao entre a tag e o leitor, corrompendo a leitura momentaneamente."},
    {"Works by emulating a virtual card while simultaneously applying interference techniques against the target reader.",
     "Funciona emulando um cartao virtual e, simultaneamente, aplica tecnicas de interferencia contra o leitor alvo."},
};

// Order matches gps_submenu_items in ESP32-DIV.ino, skipping "Back to Main Menu".
const InfoText gps_info[2] = {
    {"Associates a wireless network scan with location data obtained from the GPS module, mapping and saving the networks geographically to the SD card.",
     "Associa uma varredura de redes sem fio com os dados de localizacao obtidos do modulo GPS, mapeando e salvando as redes geograficamente no cartao SD."},
    {"Checks the current reception of the attached GPS module, showing how many satellites are visible to ensure location accuracy.",
     "Verifica a recepcao atual do modulo GPS acoplado, mostrando quantos satelites estao visiveis para garantir a precisao da localizacao."},
};

// Order matches ir_submenu_items in ESP32-DIV.ino, skipping "Back to Main Menu".
const InfoText ir_info[4] = {
    {"Reads, identifies, and records the infrared signal code of any remote control pointed at the device's sensor.",
     "Le, identifica e grava o codigo do sinal infravermelho de qualquer controle remoto que for apontado para o sensor do aparelho."},
    {"Loads saved infrared signal profiles so you can retransmit them.",
     "Carrega perfis de sinais infravermelhos salvos para que voce possa retransmiti-los."},
    {"Fires power-off codes in rapid succession to attempt shutting down various generic TV brands at once.",
     "Dispara codigos de desligamento em sequencia rapida para tentar desligar diversas marcas genericas de televisao de uma so vez."},
    {"Transmits a bank of generic commands to attempt controlling or shutting down different brands of air conditioners.",
     "Transmite um banco de comandos genericos para tentar controlar ou desligar diferentes marcas de aparelhos de ar-condicionado."},
};
