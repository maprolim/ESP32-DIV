# Adding or editing a language

Current scope: tile names, feature names inside every tile's menu, Settings'
own row labels, and the per-feature "info" screens (the page BTN_RIGHT opens
on a highlighted feature). Everything else in the firmware is still
English/Portuguese-hardcoded and out of scope for now.

## Where things live

| What | File | Shape |
|---|---|---|
| Language list + fallback logic | `Lang.h` | `enum Lang`, `LANG_COUNT`, `LANG_NAMES`, `LocStr` |
| Short labels (tiles, feature names, Settings rows) | `Strings.h` / `Strings.cpp` | `enum StrKey` + `STRINGS[]` table |
| Long paragraphs (info screens) | `LangInfo.h` / `LangInfo.cpp` | one array per tile, `InfoText` (= `LocStr`) |

Short labels and long paragraphs are deliberately kept in **separate
files**. A label table with 70 one-line rows is easy to scan; mixing in
paragraph-length strings would make every row wrap and bury the labels. The
long-text arrays also predate `Strings.cpp` (they were the original
info-screen mechanism) and are indexed positionally against each tile's
item array instead of a shared key enum -- see the comment at the top of
`LangInfo.h` for that array-order convention.

Both files resolve through the same pattern: a `LocStr` holds one
`const char*` per language, and `t(key)` / `locText(...)` picks the string
for `settings().infoLang` (the single language setting, applied app-wide),
falling back to English if the current language's slot is empty, and to a
visible `"?"` if even English is missing (should never happen -- the arrays
are sized at compile time, so a short table is a compile error, not a
silent gap).

## ASCII only -- no accents

All translations in `Strings.cpp` and `LangInfo.cpp` must stay within plain
ASCII (codes 32-127). Every screen that renders these strings uses
TFT_eSPI's stock bitmap fonts (`tft.setTextFont(1)`/`setTextFont(2)`), and
those fonts don't have accented glyphs -- a `ç`, `ã`, `é`, `ñ`, `¿` etc.
would render as a blank/garbled box on the device. Earlier revisions of
this i18n work embedded a custom accented font, but it cost ~39KB of flash
(about 2.5% of the available program space) for a benefit judged not worth
that budget long-term, so it was reverted. This may be revisited later.

When adding a translation, write it with the closest unaccented ASCII
spelling (`"Configuracoes"`, `"Conexao"`, `"Espanol"`, not
`"Configurações"`/`"Conexión"`/`"Español"`). `¿`/`¡` become `?`/`!`.

## Adding a new language

1. In `Lang.h`: add a slot to `enum Lang` (keep `LANG_EN = 0` first, it is
   the fallback), bump `LANG_COUNT`, and add its short code to `LANG_NAMES`
   (shown in Settings > Language).
2. In `Strings.cpp`: add one more string to every `{...}` row, in the same
   order as `Lang.h`. The array size is `STR_KEY_COUNT` x `LANG_COUNT`
   (checked at compile time) -- the compiler will error on any row you
   missed. Keep the new strings ASCII-only (see above).
3. In `LangInfo.cpp`: same thing, one more string per `{...}` entry in each
   of the 8 arrays, also ASCII-only.
4. Build and flash; pick the new language under Settings > Language.

You do not need to touch `ESP32-DIV.ino` to add a language -- only to add a
new *key* (a new tile or feature), see below.

## Adding a new string (new tile/feature/info text)

- Tile or feature name: add a `STR_...` entry to the enum in `Strings.h`
  (anywhere before `STR_KEY_COUNT`), add the matching row to `STRINGS[]` in
  `Strings.cpp` in the same position, then use the key where the old
  literal string used to be in `ESP32-DIV.ino` (the item arrays there hold
  `StrKey` values, resolved through `t(key)` wherever they're drawn or
  measured).
- Info paragraph: add one more `InfoText` entry to the relevant array in
  `LangInfo.cpp`, in the same order as that tile's feature array in
  `ESP32-DIV.ino` (see the comment above each array).
