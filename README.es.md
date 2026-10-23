# SDL3 DOS 386 — Edición para Hardware Real 80386 / 386SX (2 MB de RAM, sin FPU)

[English](README.md) | **Español**

> [!TIP]
> **Repositorio hermano (`486DX+` / Pentium / DOSBox):** ¿Buscas el código C de referencia limpio, directo e idiomático sobre SDL3 sin ganchos de bajo nivel DPMI/enlazador (para equipos con `4+ MB` de RAM o emuladores actuales)? Visita el repositorio principal **[`mmoroca/sdl3-dos-demo`](https://github.com/mmoroca/sdl3-dos-demo)**, del cual parte esta edición optimizada para `80386`.

Colección independiente en C11 de demos, herramientas y juegos arcade sobre **SDL3 para MS-DOS (`320×200` `INDEX8` VGA Modo 13h)** diseñada desde cero para ejecutarse con **la fluidez de un `486` y cero paginación a disco (`CWSDPMI.SWP = 0`) en ordenadores reales `80386SX` / `80386DX` de 1990–1992 con solo `2 MB` de RAM total y sin coprocesador matemático `80387`** — sin modificar un solo archivo del submódulo externo `SDL/`.

Desarrollado por **@mmoroca & [Arena.AI](http://arena.ai/) 2026**.

---

## Validación en Hardware Real: ADL Computers Terminator 386

Probado y validado en silicio real de principios de los 90 (**ADL Computers Terminator 386** — Intel `80386SX`, `2 MB` de RAM total, sin coprocesador `80387`, pantalla LCD pasiva conectada a VGA ISA estándar sin BIOS VESA VBE, MS-DOS + `CWSDPMI.EXE`), ejecutando todas las demos y juegos a pantalla completa con total fluidez y **el indicador `Hard Disk` completamente apagado (`0 B` de swap)** — además de DOSBox / DOSBox-X / FreeDOS y sistemas actuales (Linux, Windows, macOS).

![ADL Computers Terminator 386 ejecutando SDL3 Systems Showcase](docs/adl-terminator-386.jpg)

> **¿Lo has probado en tu propio ordenador clásico MS-DOS?** Tanto si es un `386SX`, `386DX`, `486SX`, `486DX`, portátil de época, PS/2 o clónico ISA, nos encantará saber qué tal funciona: déjanos un mensaje en **[GitHub Issues](https://github.com/mmoroca/sdl3-dos-386/issues)** contando el modelo, CPU, RAM, tarjeta de vídeo/sonido y tu experiencia.

---

## Programas Incluidos

Los ocho ejecutables caben holgadamente dentro del **techo físico de `~795 KB` de XMS libre** de un PC DOS con `2 MB` de RAM bajo `CWSDPMI.EXE` (`~508 KB – 574 KB` de imagen residente):

| Ejecutable DOS | Código Fuente | Imagen en RAM (`.text+.data+.bss`) | Qué demuestra y optimizaciones para 386 |
| :--- | :--- | :---: | :--- |
| **`HELLO.EXE`** | [`main.c`](main.c) | `507,928 B` | Esqueleto mínimo `320×200` `INDEX8` enlazado con `dos_compat.c` (`0 %` de tráfico ISA en reposo). |
| **`MISTIFY.EXE`** | [`mistify.c`](mistify.c) | `512,024 B` | Salvapantallas de polígonos en coma fija `16.16` (`0` instrucciones `float`) y rotación de paleta VGA DAC. |
| **`FONTDEMO.EXE`** | [`fontdemo.c`](fontdemo.c) | `516,632 B` | Renderizador UTF-8 multifuente (`3×5`, `5×7` y `8×8` completa en español con `áéíóúüñ` y `¡¿`) en `.rodata` (`0 B` de heap). |
| **`SBTEST.EXE`** | [`soundtest.c`](soundtest.c) | `540,184 B` | Sintetizador entero `SDL_AUDIO_U8` (`11025 Hz` mono) y panel de diagnóstico Sound Blaster / `BLASTER`. |
| **`MODPLAY.EXE`** | [`modplay.c`](modplay.c) | `550,424 B` | Reproductor ProTracker `.MOD` de 4 canales en coma fija entera `16.16` con refresco parcial por rectángulos sucios a `10 Hz`. |
| **`DINO.EXE`** | [`dinorun.c`](dinorun.c) | `564,760 B` | **Dino Fall**: carga `.BMP` por streaming con `0 B` de heap transitorio, scroll `TRACK.BMP` de 1 bit + LUT `Uint32`, máscaras de colisión de 1 bit y *dirty strips* analíticos con `0` `imul`. |
| **`SHOWCASE.EXE`** | [`showcase.c`](showcase.c) | `567,832 B` | Plantilla base de 4 paneles (info de sistema, mezclador `U8`, inspector de teclado/joystick/texto y minijuego). Véase [`SHOWCASE_GUIDE.md`](SHOWCASE_GUIDE.md). |
| **`SHOOTER.EXE`** | [`shooter.c`](shooter.c) | `574,488 B` | Arcade espacial a pantalla completa capaz de mover **hasta 256 OVNIs simultáneos** en un `386SX` real mediante tramos RLE pre-cocinados, colisión por máscaras `Uint32` y autodisparo `F` (anti-*ghosting* 2KRO). |

![SDL3 DOS Showcase](docs/showcase.png)

![SDL3 Space Shooter](docs/shooter.png)

> **Registro de diagnóstico opcional (`/L`)**: La escritura de archivos `.LOG` está **desactivada por defecto** en todos los programas para no penalizar el bucle en tiempo real con accesos FAT16 en discos IDE/CF antiguos. Pasa `/l` (o `/log`) por línea de comandos (por ejemplo `SHOOTER.EXE /L` o `SHOWCASE.EXE /L`) únicamente cuando quieras generar trazas en disco.

---

## Cómo Optimizar `CONFIG.SYS`, `AUTOEXEC.BAT` y `CWSDPMI.EXE` en Equipos de `2 MB` de RAM

En un `80386` con solo **`2 MB` (`2.048 KB`) de RAM total**, únicamente existen **`1.024 KB` de Memoria Extendida (XMS) física** por encima del primer megabyte. Cada kilobyte que consuman al arrancar cachés de disco (`SMARTDRV`), gestores EMS (`EMM386`) o residentes (TSR) se descuenta directamente de la RAM física disponible para los ejecutables de 32 bits DJGPP/SDL3, obligando a `CWSDPMI.EXE` a paginar en el disco duro (`C:\CWSDPMI.SWP`).

### Configuración Limpia Recomendada (`2 MB` de RAM — Máxima XMS y Cero Swap)

1. Copia **`CWSDPMI.EXE`** (incluido en el paquete oficial `csdpmi*b.zip` de DJGPP) en la misma carpeta que los ejecutables `.EXE` o en un directorio del `PATH` como `C:\DOS`.
2. Mantén un **`CONFIG.SYS`** limpio para que **`CWSDPMI.EXE` trabaje directamente sobre `HIMEM.SYS` (modo XMS puro en Anillo 0)** sin cargar `EMM386.EXE`:

```dos
DEVICE=C:\DOS\HIMEM.SYS /TESTMEM:OFF
DOS=HIGH
FILES=20
BUFFERS=15
STACKS=9,256
```

3. Evita cargar **`SMARTDRV.EXE`** u otros residentes que reserven XMS en **`AUTOEXEC.BAT`**:

```dos
@ECHO OFF
PROMPT $P$G
PATH C:\DOS

REM Opcional — definir SOLO si el equipo dispone realmente de tarjeta Sound Blaster o compatible:
REM SET BLASTER=A220 I5 D1 T3
```

### Por Qué Estos Ajustes Marcan toda la Diferencia en un `386` con `2 MB`

- **No cargues `SMARTDRV.EXE` cuando vayas justo de RAM**: En un PC con `2 MB`, `SMARTDRV` reserva por defecto **`256–512 KB` de XMS física** para caché de disco, dejando a `CWSDPMI` con menos de `400 KB` libres y provocando paginación constante.
- **Prescinde de `EMM386.EXE` si no necesitas memoria expandida (EMS)**: Con `EMM386` activo, `CWSDPMI` pasa de XMS nativo a **VCPI** (modo Virtual 8086), perdiendo **`~64–160 KB` de RAM extendida** en tablas de páginas y añadiendo sobrecarga de CPU en cada interrupción hardware.
- **Truco de menú de arranque para MS-DOS 6.0 / 6.22**: No hace falta renunciar a tu configuración habitual del día a día. Puedes añadir una entrada `[JUEGOS386]` en `C:\CONFIG.SYS` para elegir al encender el equipo entre tu arranque completo y el modo DPMI limpio:

```dos
[MENU]
MENUITEM=NORMAL,Arranque habitual de MS-DOS
MENUITEM=JUEGOS386,Modo limpio 386 DPMI (Maxima XMS - Sin Swap)
MENUDEFAULT=NORMAL,5

[JUEGOS386]
DEVICE=C:\DOS\HIMEM.SYS /TESTMEM:OFF
DOS=HIGH
FILES=20
BUFFERS=15
STACKS=9,256

[NORMAL]
REM Tus lineas habituales de CONFIG.SYS aqui...
```

---

## Guía de Ingeniería: Cómo Hacer Volar SDL3 en un `80386SX` Real (`2 MB` de RAM, sin FPU)

Todos los archivos del repositorio ([`CMakeLists.txt`](CMakeLists.txt), [`dos_compat.c`](dos_compat.c), [`dinorun.c`](dinorun.c), [`shooter.c`](shooter.c), [`showcase.c`](showcase.c), [`modplay.c`](modplay.c)) están exhaustivamente comentados para que cualquier desarrollador pueda comprender el porqué físico de cada cuello de botella y replicar todas las soluciones directamente en sus propios juegos o utilidades SDL3/DOS.

### 1. El Precipicio Físico de `2 MB` de RAM (`~795 KB` de XMS Libre)

En un PC `80386SX` con `2 MB` (`2.048 KB`) de RAM total ejecutando MS-DOS + `HIMEM.SYS` (`DOS=HIGH`) + `CWSDPMI.EXE`:

- `0 – 640 KB`: Memoria convencional DOS (núcleo MS-DOS, controladores y stub real-mode).
- `640 – 1024 KB`: Área de Memoria Superior (`0xA0000` VGA, ROMs de BIOS).
- `1024 – 2048 KB`: **Solo existen `1.024 KB` de Memoria Extendida (XMS) física.**

Descontando tablas de páginas y buffers de transferencia de `HIMEM.SYS` y `CWSDPMI`, **quedan únicamente `~795 KB` de páginas físicas XMS libres** para todo el proceso en modo protegido de 32 bits:

$$\text{Código (`.text`)} + \text{Datos/BSS} + \text{Pila (`_stklen`)} + \text{Pico de Heap (`sbrk()`)} \le \mathbf{\sim 795\text{ KB}}$$

Cuatro trampas silenciosas de DJGPP disparan el uso por encima de `795 KB` si no se neutralizan explícitamente:

1. **La pila por defecto de `512 KB` en DJGPP**: `_stklen` reserva `512 KB` físicos antes de entrar en `main()`. Redefinir `unsigned _stklen = 65536U;` (`64 KB`) en [`dos_compat.c`](dos_compat.c) recupera de golpe **`448 KB`** de RAM física.
2. **La marca de agua monotónica de `sbrk()`**: En DJGPP, `free()` **jamás devuelve páginas físicas al servidor DPMI**. Si durante la carga inicial un `SDL_LoadBMP("Images/OTHER/TRACK.BMP")` suma `68 KB` sobre `738 KB` (`806 KB > 795 KB`) aunque sea durante **1 milisegundo**, `CWSDPMI` abre `C:\CWSDPMI.SWP` y pagina a disco sin parar durante toda la partida.
3. **El `malloc(4096)` oculto dentro de `fread()` en `libc.a`**: La primera lectura con `fopen()` + `fread()` reserva en secreto un buffer de `4.096 B` en el heap dentro de `_filbuf()`. Llamar a `setvbuf(fp, buf_en_pila, _IOFBF, 512)` justo después de `fopen()` deja el heap transitorio en **literalmente `0 bytes`**.
4. **No activar `_CRT0_FLAG_LOCK_MEMORY` globalmente durante `main()`**: `SDL_RunApp()` ya bloquea `.text + .data + .bss + pila` para las ISRs hardware y desactiva `_CRT0_FLAG_LOCK_MEMORY` justo antes de `main()`. Bloquear también el heap agota prematuramente las páginas físicas bloqueables de `CWSDPMI`.

---

### 2. Carga `.BMP` en Streaming con `0 B` de Heap y RLE Pre-Cocinado (`dinorun.c` y `shooter.c`)

- **Decodificador `.BMP` en streaming fila a fila (`IndexedBmpStream` en [`dinorun.c`](dinorun.c))**: Lee una sola línea de escaneo cada vez y construye directamente tramos opacos pre-escalados (`PreparedSpan`) y máscaras de colisión compactadas a **`1 bit/píxel`** (`8×` menos memoria que 1 byte/píxel). Al eliminar toda llamada a `SDL_LoadBMP_IO`, el enlazador con `-Wl,--gc-sections` descarta por completo el módulo `SDL_bmp.c` (`~20 KB` menos de `.text`) y el pico transitorio de heap es de **`0 bytes`**.
- **Sondeo previo de hardware de audio ([`shooter.c`](shooter.c), [`showcase.c`](showcase.c))**: El dispositivo de audio de SDL se abre **antes** de reservar los `~291 KB` de clips `.WAV`. En portátiles `386SX` de `2 MB` sin Sound Blaster no se aloja un solo byte de audio en el heap.
- **Liberación inmediata de pantalla de presentación ([`shooter.c`](shooter.c))**: `UFO.BMP` (`320×200`, `64 KB`) se destruye en cuanto termina o se salta con `Esc` la pantalla inicial.

---

### 3. Scroll de `TRACK.BMP` con `1 Bit/Píxel` + Relleno Circular + LUT `Uint32` (`dinorun.c`)

`Images/OTHER/TRACK.BMP` (`2404 × 28`, `68 KB` en crudo) usa un único color de tinta opaco sobre fondo transparente:

1. `load_track_streaming()` empaqueta las `2.404` columnas a **`1 bit/píxel`** (`g_track_bits` en `.bss`) y añade **`328` bits (`SCREEN_WIDTH + 8`) de réplica circular** (`x % ancho`) al final de cada fila.
2. Una tabla `Uint32` de 16 entradas (`g_track_lut4[16]`, solo `64 bytes`) traduce cada *nibble* de 4 bits directamente en **4 píxeles `INDEX8` empaquetados en una palabra de 32 bits**.
3. `draw_ground()` extrae 8 bits alineados al píxel exacto de desplazamiento `(word >> bit_shift) & 0xFF` y escribe **8 píxeles horizontales por iteración (`2 × Uint32`)**, **sin saltos condicionales, sin costuras de unión, sin multiplicaciones y con `0 B` de heap**, tardando apenas **`~0,6 ms/fotograma`** en un `386SX` a `16 MHz`.

---

### 4. Eliminación de `imul` por Píxel y Reducción del Tráfico al Bus ISA VGA (`dinorun.c` + `dos_compat.c`)

- **Por qué hashear el framebuffer hunde a un `80386`**: En un `80386`, cada `imul` de 32 bits cuesta **`9 – 38` ciclos**. Hashear `16.000` palabras por fotograma consume **`~800.000` ciclos (`~50 ms/fotograma` a `16 MHz`)** antes de dibujar un solo sprite.
- **Máscara analítica de 25 franjas en un único `Uint32`**: Dividiendo `320×200` en `25` franjas horizontales de `8` líneas, toda la pantalla cabe en un registro de 32 bits:
  ```c
  g_cur_dirty_strips |= ((2U << s1) - (1U << s0));
  ```
  - **Borrado parcial `memset`**: `draw_game()` borra **únicamente `prev_dirty_strips`**, ahorrando `~40 %` de escritura en RAM.
  - **Subida parcial por franjas a `0xA0000`**: `present_game_frame()` fusiona bits contiguos de `cur_dirty_strips | prev_dirty_strips` y transfiere al bus ISA de `8 MHz` solo las líneas modificadas (`-19 %` a `-29 %` de tráfico ISA jugando).
  - **Cortocircuito de pantalla estática**: En `MODE_TITLE`, `MODE_PAUSED`, `MODE_GAME_OVER` y `MODE_NAME_ENTRY` (sin pulsar teclas), `draw_game()` retorna al instante con **`0 bytes` transferidos y `0 %` de carga de CPU**.

---

### 5. ISA `i386`, Bug Upstream `CPU_haveCPUID` (`SDL#16573`) y Cero Coma Flotante

- **Generación estricta `i386`**: Compilar SDL3 y los juegos con `-march=i386 -mtune=i386` impide la emisión de instrucciones `486+` (`bswap`, `cmpxchg`, `xadd`) o `cmov*` (`SIGILL` en `80386`).
- **Corrección upstream de `CPU_haveCPUID()` ([`libsdl-org/SDL#16573`](https://github.com/libsdl-org/SDL/pull/16573))**: En versiones sin parchear de SDL3, `: "=m" (has_CPUID)` hacía que la optimización DSE de GCC eliminase `int has_CPUID = 0;`, haciendo creer a SDL que un `80386` soporta `CPUID` y ejecutando `0F A2` (`SIGILL` al arrancar). `CMakeLists.txt` añade `-fno-tree-dse -fno-dse` como protección automática, y [`libsdl-org/SDL#16573`](https://github.com/libsdl-org/SDL/pull/16573) lo resuelve de raíz en upstream con `: "+r" (has_CPUID)` y `"cc"`.
- **Emulación x87 (`libemu.a`) y cero `float` en tiempo real**: Se enlaza `-Wl,--whole-archive -lemu -Wl,--no-whole-archive` para cubrir inicializaciones puntuales en `libc`/`SDL` sin `SIGNOFP`, **eliminando al `100 %` las operaciones `float`/`double` por fotograma** (cada instrucción x87 emulada por `INT 7` cuesta **`~3.000+` ciclos** en un `386SX`). Además, los `SDL_AudioStream` inactivos se pausan siempre que quedan en silencio.

---

### 6. Teclado IRQ 1 en Modo Protegido Puro, Temporizador PIT y VGA Estándar (`dos_compat.c`)

Mediante `--wrap=...` de GNU `ld` (**cero parches sobre `SDL/`**), [`dos_compat.c`](dos_compat.c) resuelve cinco cuellos de botella críticos en una sola biblioteca reutilizable:

| Símbolo interceptado (`--wrap`) | Cuello de botella real en `80386` resuelto |
| :--- | :--- |
| `DOS_HookInterrupt` / `DOS_UnhookInterrupt` | Instala un wrapper `IRET` DPMI de 32 bits bloqueado en memoria directamente en `INT 09h` sin encadenar a la BIOS en modo real (`0` cambios de modo protegido↔real al mantener teclas pulsadas). |
| `__dpmi_int` (`0x16`) | Responde al instante en modo protegido (`ZF = 1`) al sondeo `INT 16h` de SDL en cada fotograma (~60 reflexiones DPMI/s ahorradas). |
| `SDL_SendKeyboardKey` / `SDL_SendKeyboardText` | Descarta ráfagas *typematic* hardware de `30 Hz` sobre teclas ya pulsadas (conservando `Backspace`) y evita reservas de heap en eventos de texto cuando `SDL_TextInputActive()` está inactivo. |
| `DOSVESA_SetDisplayMode` | Protege `mode->internal == NULL` para que tarjetas VGA estándar de 1990–1991 sin BIOS VESA restauran limpiamente el modo texto `80×25` (`INT 10h AX=0003h`) al salir. |
| `DOSVESA_UpdateWindowFramebuffer` | Ejecuta el camino completo de SDL en el fotograma 1 (programando la paleta VGA DAC `0x3C8/0x3C9`) y transfiere con `dosmemput()` solo las franjas modificadas a `0xA0000` en los siguientes. |
| `SDL_SYS_DelayNS` | Sustituye `delay()` (`rdtsc` -> `SIGILL` en 386/486 o bloqueos en BIOS `INT 15h AH=86h`) por el temporizador hardware Intel 8254 PIT vía `uclock()` + `DOS_Yield()`. |

---

## Compilación Cruzada para DOS (`80386` / `2 MB` de RAM)

1. Clona el repositorio con submódulos (SDL3 se mantiene externo; nunca se vendoriza ni modifica):

```bash
git clone --recurse-submodules https://github.com/mmoroca/sdl3-dos-386.git
cd sdl3-dos-386
```

2. Configura y compila todos los ejecutables `i386` recortados (`strip --strip-all`) con tu cadena cruzada DJGPP (`CMakeLists.txt` aplica automáticamente `-Os -march=i386 -mtune=i386 -fno-tree-dse -fno-dse -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--whole-archive -lemu -Wl,--no-whole-archive` y todos los `--wrap=...` cuando `CMAKE_SYSTEM_NAME=DOS`):

```bash
cmake -S . -B build-dos \
  -DCMAKE_SYSTEM_NAME=DOS \
  -DCMAKE_C_COMPILER=i586-pc-msdosdjgpp-gcc \
  -DCMAKE_AR=i586-pc-msdosdjgpp-ar \
  -DCMAKE_RANLIB=i586-pc-msdosdjgpp-ranlib \
  -DCMAKE_STRIP=i586-pc-msdosdjgpp-strip \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-dos -j
```

3. Copia los `.EXE` generados junto con `CWSDPMI.EXE`, `Images/` y `SHOWCASE/` a tu unidad DOS.

### Cómo Reutilizar `dos_compat` en Tus Propios Programas SDL3/DOS

Copia [`dos_compat.c`](dos_compat.c) en tu proyecto junto con el bloque `if(CMAKE_SYSTEM_NAME STREQUAL "DOS")` y `add_library(dos_compat OBJECT dos_compat.c)` de [`CMakeLists.txt`](CMakeLists.txt), y enlaza tu ejecutable con:

```cmake
target_link_libraries(tu_programa PRIVATE dos_compat)
```

---

## Repositorios Relacionados

- **[`mmoroca/sdl3-dos-demo`](https://github.com/mmoroca/sdl3-dos-demo)** — Edición principal de referencia con código C limpio e idiomático sobre SDL3 para `486DX+` (`4+ MB` de RAM), DOSBox / DOSBox-X y escritorio actual.
- **[`mmoroca/sdl3-dos-386`](https://github.com/mmoroca/sdl3-dos-386)** — Edición optimizada para `80386 / 386SX` sin FPU y `2 MB` de RAM (`VGA ISA`) + capa reutilizable `dos_compat.c`.
- **[`libsdl-org/SDL#16573`](https://github.com/libsdl-org/SDL/pull/16573)** — Pull Request oficial upstream que corrige el ensamblador inline de `CPU_haveCPUID()` en procesadores `i386` (`80386` / `80486` sin `CPUID`).

---

## Conversor de Assets y Fuentes Bitmap (Sin Pillow)

- **`tools/convert_assets.py`**: Conversor autónomo basado exclusivamente en la biblioteca estándar de Python 3 (`zlib` + `struct`, **sin exigir Pillow**) que transforma recursivamente árboles de imágenes PNG a `.BMP` indexados sin comprimir con nombres DOS `8.3` y paleta compartida RGB `3:3:2` de 256 colores (`índice 5` transparente).
- **`tools/genfont.py`**: Generador sin dependencias externas de hojas `.PNG` a cabeceras C en `.rodata` con soporte completo de tablas Unicode / UTF-8 (`fonts/font_3x5.h`, `fonts/font_5x7.h`, `fonts/nueva_font_5x7.h`, `fonts/font_8x8.h`).

---

## Licencias y Créditos

- **Código fuente propio en C, capa de compatibilidad 80386 (`dos_compat.c`) y utilidades Python**: Publicados bajo **Licencia MIT** — por **@mmoroca & [Arena.AI](http://arena.ai/) 2026**. Véase [`LICENSE`](LICENSE).
- **SDL3 (`SDL/`)**: Submódulo Git externo distribuido bajo **Licencia zlib** (`https://github.com/libsdl-org/SDL`). SDL3 no se vendoriza ni cambia de licencia en este repositorio; véase [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
- **Gráficos y sonidos de Showcase y Shooter (`SHOWCASE/`)**: Space Shooter Remastered de [Kenney](https://kenney.nl), cedidos al dominio público bajo **CC0 1.0 Universal** (atribución voluntaria; no se les aplica licencia MIT).
- **Sprites de Dino Fall (`Images/`)**: Creados para [`mmoroca/sdl3-dino-fall`](https://github.com/mmoroca/sdl3-dino-fall) e incluidos con autorización de `@mmoroca` (conservan sus condiciones originales; no se afirma que tengan licencia MIT).
