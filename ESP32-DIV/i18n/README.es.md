# Añadir o editar un idioma

Alcance actual: nombres de los tiles, nombres de las features dentro del
menú de cada tile, las etiquetas de las propias filas de Settings, y las
pantallas de "info" de cada feature (la página que abre BTN_RIGHT sobre una
feature seleccionada). El resto del firmware sigue con texto fijo en
inglés/portugués y queda fuera de alcance por ahora.

## Dónde está cada cosa

| Qué | Archivo | Forma |
|---|---|---|
| Lista de idiomas + fallback | `Lang.h` | `enum Lang`, `LANG_COUNT`, `LANG_NAMES`, `LocStr` |
| Etiquetas cortas (tiles, nombres de feature, filas de Settings) | `Strings.h` / `Strings.cpp` | `enum StrKey` + tabla `STRINGS[]` |
| Párrafos largos (pantallas de info) | `LangInfo.h` / `LangInfo.cpp` | un array por tile, `InfoText` (= `LocStr`) |

Las etiquetas cortas y los párrafos largos se mantienen a propósito en
**archivos separados**. Una tabla de etiquetas con ~70 filas de una línea
es fácil de leer de un vistazo; mezclar ahí strings del largo de un
párrafo haría que cada fila se corte y enterraría las etiquetas. Los
arrays de texto largo también son anteriores a `Strings.cpp` (eran el
mecanismo original de las pantallas de info) y se indexan por la posición
en el array de items de cada tile, no por una clave compartida -- ver el
comentario al inicio de `LangInfo.h` para esa convención de orden.

Ambos archivos se resuelven con el mismo patrón: un `LocStr` guarda un
`const char*` por idioma, y `t(key)` / `locText(...)` elige la cadena para
`settings().infoLang` (el único ajuste de idioma, aplicado a toda la app),
recurriendo a inglés si el slot del idioma actual está vacío, y a un `"?"`
visible si hasta el inglés falta (no debería pasar nunca -- los arrays
tienen tamaño fijo en tiempo de compilación, así que una tabla corta es un
error de compilación, no un hueco silencioso).

## Solo ASCII -- sin tildes

Todas las traducciones en `Strings.cpp` y `LangInfo.cpp` deben mantenerse
dentro del ASCII puro (códigos 32-127). Toda pantalla que dibuja estas
cadenas usa las fuentes bitmap integradas de TFT_eSPI
(`tft.setTextFont(1)`/`setTextFont(2)`), y esas fuentes no tienen glifos
acentuados -- una `ç`, `ã`, `é`, `ñ`, `¿`, etc. se vería como un cuadro en
blanco/corrupto en el dispositivo. Versiones anteriores de este trabajo de
i18n incluían una fuente acentuada personalizada, pero costaba ~39KB de
flash (cerca del 2,5% del espacio de programa disponible) por un beneficio
que no se consideró que valiera ese presupuesto a largo plazo, así que se
revirtió. Esto puede reconsiderarse más adelante.

Al añadir una traducción, escríbela con la grafía ASCII sin tilde más
cercana (`"Configuraciones"`, `"Conexion"`, `"Espanol"`, no
`"Configuraciones"` con tilde/`"Conexión"`/`"Español"`). `¿`/`¡` se
convierten en `?`/`!`.

## Añadir un idioma nuevo

1. En `Lang.h`: agrega un slot a `enum Lang` (mantén `LANG_EN = 0` primero,
   es el fallback), aumenta `LANG_COUNT`, y añade su código corto en
   `LANG_NAMES` (se muestra en Settings > Language).
2. En `Strings.cpp`: agrega una cadena más a cada fila `{...}`, en el mismo
   orden que `Lang.h`. El tamaño del array es `STR_KEY_COUNT` x
   `LANG_COUNT` (verificado en tiempo de compilación) -- el compilador
   marcará error en cualquier fila que falte. Mantén las cadenas nuevas
   solo en ASCII (ver arriba).
3. En `LangInfo.cpp`: lo mismo, una cadena más por entrada `{...}` en cada
   uno de los 8 arrays, también solo en ASCII.
4. Compila y graba; elige el nuevo idioma en Settings > Language.

No hace falta tocar `ESP32-DIV.ino` para añadir un idioma -- solo para
añadir una *clave* nueva (un tile o feature nuevo), ver abajo.

## Añadir una cadena nueva (tile/feature/texto de info nuevo)

- Nombre de tile o feature: agrega una entrada `STR_...` al enum en
  `Strings.h` (en cualquier lugar antes de `STR_KEY_COUNT`), agrega la fila
  correspondiente en `STRINGS[]` en `Strings.cpp` en la misma posición, y
  usa la clave donde antes estaba el literal de texto en `ESP32-DIV.ino`
  (los arrays de items ahí guardan valores `StrKey`, resueltos mediante
  `t(key)` en todos los lugares donde se dibujan o se miden).
- Texto de info: agrega una entrada `InfoText` más al array correspondiente
  en `LangInfo.cpp`, en el mismo orden que el array de features de ese
  tile en `ESP32-DIV.ino` (ver el comentario encima de cada array).
