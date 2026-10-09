# Adicionando ou editando um idioma

Escopo atual: nomes dos tiles, nomes das features dentro do menu de cada
tile, os rótulos das próprias linhas do Settings, e as telas de "info" de
cada feature (a página que o BTN_RIGHT abre quando uma feature está
selecionada). O resto do firmware ainda está com texto fixo em
inglês/português e fica fora do escopo por enquanto.

## Onde fica cada coisa

| O quê | Arquivo | Formato |
|---|---|---|
| Lista de idiomas + fallback | `Lang.h` | `enum Lang`, `LANG_COUNT`, `LANG_NAMES`, `LocStr` |
| Rótulos curtos (tiles, nomes de feature, linhas do Settings) | `Strings.h` / `Strings.cpp` | `enum StrKey` + tabela `STRINGS[]` |
| Parágrafos longos (telas de info) | `LangInfo.h` / `LangInfo.cpp` | um array por tile, `InfoText` (= `LocStr`) |

Rótulos curtos e parágrafos longos ficam de propósito em **arquivos
separados**. Uma tabela de rótulos com ~70 linhas de uma linha cada é fácil
de ler de relance; misturar strings do tamanho de um parágrafo ali faria
cada linha quebrar e enterraria os rótulos. Os arrays de texto longo também
são anteriores ao `Strings.cpp` (eram o mecanismo original das telas de
info) e são indexados pela posição no array de itens de cada tile, não por
uma chave compartilhada -- ver o comentário no topo de `LangInfo.h` para
essa convenção de ordem.

Os dois arquivos resolvem pelo mesmo padrão: um `LocStr` guarda um
`const char*` por idioma, e `t(key)` / `locText(...)` escolhe a string para
`settings().infoLang` (a única configuração de idioma, aplicada no app
inteiro), caindo para inglês se o slot do idioma atual estiver vazio, e
para um `"?"` visível se até o inglês estiver faltando (nunca deveria
acontecer -- os arrays têm tamanho fixo em tempo de compilação, então uma
tabela curta vira erro de compilação, não um buraco silencioso).

## Só ASCII -- sem acento

Todas as traduções em `Strings.cpp` e `LangInfo.cpp` precisam ficar dentro
do ASCII puro (códigos 32-127). Toda tela que desenha essas strings usa as
fontes bitmap padrão da TFT_eSPI (`tft.setTextFont(1)`/`setTextFont(2)`), e
essas fontes não têm glifos acentuados -- um `ç`, `ã`, `é`, `ñ`, `¿` etc.
apareceria como um quadrado em branco/corrompido no aparelho. Versões
anteriores desse trabalho de i18n embutiam uma fonte acentuada customizada,
mas isso custava ~39KB de flash (cerca de 2,5% do espaço de programa
disponível) por um ganho que não foi considerado valer esse orçamento no
longo prazo, então foi revertido. Isso pode ser reavaliado depois.

Ao adicionar uma tradução, escreva com a grafia ASCII sem acento mais
próxima (`"Configuracoes"`, `"Conexao"`, `"Espanol"`, não
`"Configurações"`/`"Conexión"`/`"Español"`). `¿`/`¡` viram `?`/`!`.

## Adicionando um novo idioma

1. Em `Lang.h`: acrescente um slot em `enum Lang` (mantenha `LANG_EN = 0`
   primeiro, é o fallback), aumente `LANG_COUNT`, e adicione o código curto
   em `LANG_NAMES` (aparece em Settings > Language).
2. Em `Strings.cpp`: acrescente mais uma string em cada linha `{...}`, na
   mesma ordem de `Lang.h`. O tamanho do array é `STR_KEY_COUNT` x
   `LANG_COUNT` (checado em tempo de compilação) -- o compilador vai
   acusar erro em qualquer linha que você esquecer. Mantenha as strings
   novas só em ASCII (ver acima).
3. Em `LangInfo.cpp`: a mesma coisa, mais uma string por entrada `{...}`
   em cada um dos 8 arrays, também só em ASCII.
4. Compile e grave; escolha o novo idioma em Settings > Language.

Não é preciso mexer no `ESP32-DIV.ino` para adicionar um idioma -- só para
adicionar uma *chave* nova (um tile ou feature novo), ver abaixo.

## Adicionando uma string nova (tile/feature/texto de info novo)

- Nome de tile ou feature: acrescente uma entrada `STR_...` no enum em
  `Strings.h` (em qualquer lugar antes de `STR_KEY_COUNT`), acrescente a
  linha correspondente em `STRINGS[]` em `Strings.cpp` na mesma posição, e
  use a chave onde antes estava o literal de texto em `ESP32-DIV.ino` (os
  arrays de item lá guardam valores `StrKey`, resolvidos via `t(key)` em
  todo lugar onde são desenhados ou medidos).
- Texto de info: acrescente mais uma entrada `InfoText` no array
  correspondente em `LangInfo.cpp`, na mesma ordem do array de features
  daquele tile em `ESP32-DIV.ino` (ver o comentário acima de cada array).
