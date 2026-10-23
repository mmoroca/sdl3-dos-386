cmake_minimum_required(VERSION 3.16)
# by @mmoroca & Arena.AI 2026 (http://arena.ai/)
project(sdl3_dos_386 LANGUAGES C)

# ==============================================================================
# ARCHITECTURAL GUIDE: COMPILING & LINKING SDL3 FOR REAL 80386 / 386SX (2 MB RAM)
# ==============================================================================
# A real 1990-1992 80386SX DOS machine with 2 MB total RAM has NO hardware FPU,
# NO i486/Pentium instructions (BSWAP, CMPXCHG, XADD, CPUID, RDTSC, CMOVcc), a
# 16-bit external memory data bus, an 8/16-bit ISA VGA card, and only ~795 KB of
# free physical extended memory (XMS) left for CWSDPMI once MS-DOS and conventional
# memory are accounted for (`640 KB` conv + `384 KB` UMA + `1024 KB` XMS).
#
# If process memory (`.text + .data + .bss + stack + peak sbrk() heap`) exceeds
# ~795 KB for even a single millisecond at startup, DJGPP's `sbrk()` never shrinks
# back down on `free()`, forcing `CWSDPMI` to page live working-set pages to disk
# (`C:\CWSDPMI.SWP`) on every frame.
#
# The flags and linker wraps below solve all ISA, FPU, BIOS/DPMI, and RAM traps
# cleanly from CMake WITHOUT modifying a single file inside the `SDL/` submodule:
#
#   1. `-march=i386 -mtune=i386`:
#      Forbids GCC from emitting i486+ instructions (`bswap`, `cmpxchg`, `xadd`)
#      or i686 `cmov*` instructions that trigger `SIGILL` (`Exception 06h`).
#   2. `-fno-tree-dse -fno-dse`:
#      Protects unpatched SDL3 checkouts from GCC GIMPLE dead-store elimination
#      removing `int has_CPUID = 0;` before `CPU_haveCPUID()`'s inline asm block
#      (fixed upstream in https://github.com/libsdl-org/SDL/pull/16573).
#   3. `-Os -DNDEBUG -ffunction-sections -fdata-sections` + `-Wl,--gc-sections`
#      + `-DSDL_LEAN_AND_MEAN=1 -DSDL_HAVE_BLIT_1=1`:
#      Strips ~1.1 MB of unused pixel converters, software renderers, and dead
#      codecs (e.g. `SDL_bmp.c` when streaming BMP rows directly in `dinorun.c`),
#      shrinking executables from ~1.7 MB down to ~507-574 KB total footprint while
#      preserving `SDL_BlitSurface` for 8-bit `INDEX8` surfaces.
#   4. `-Wl,--whole-archive -lemu -Wl,--no-whole-archive` (NEVER `-msoft-float`):
#      Upstream DJGPP `libgcc.a` is built without `t-softfp` soft-float helpers,
#      so `-msoft-float` fails to link `__addsf3`/`__mulsf3`. Linking DJGPP's
#      `libemu.a` handles occasional startup/shutdown x87 instructions inside
#      `libc`/`SDL` on FPU-less 386SX/486SX hardware via `INT 7` emulation.
#      CRITICAL: each emulated x87 instruction traps into ring-0/ring-3 and costs
#      ~3,000+ clock cycles on a 386SX, so per-frame game/audio code MUST use
#      100% pure integer / fixed-point math and pause idle `SDL_AudioStream`s!
#   5. GNU `ld` `--wrap=...` symbol interposition (`dos_compat.c`):
#      Replaces real-mode BIOS IRQ1/INT 16h round-trips, `delay()` (`rdtsc` /
#      broken BIOS `INT 15h AH=86h`), plain non-VESA VGA text restoration, and
#      full-screen 64 KB ISA VGA blits at link time with zero changes to `SDL/`.
# ==============================================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Apply strict i386 ISA + code-size flags BEFORE configuring `SDL/` so every
# object inside `libSDL3.a` is compiled with `-march=i386 -Os -ffunction-sections`.
if(CMAKE_SYSTEM_NAME STREQUAL "DOS")
    add_compile_options(
        -Os
        -DNDEBUG
        -march=i386
        -mtune=i386
        -fno-tree-dse
        -fno-dse
        -ffunction-sections
        -fdata-sections
        -DSDL_LEAN_AND_MEAN=1
        -DSDL_HAVE_BLIT_1=1
    )
endif()

# Static DOS build with unused SDL3 subsystems compiled out to cut RAM by ~67%.
# NOTE: `SDL_JOYSTICK` MUST remain `ON` because SDL3's DOS video backend
# (`SDL_dosvideo.c`) directly calls DOS joystick initialization entry points.
set(SDL_SHARED OFF CACHE BOOL "Build SDL shared library" FORCE)
set(SDL_STATIC ON CACHE BOOL "Build SDL static library" FORCE)
set(SDL_AUDIO ON CACHE BOOL "Build SDL audio support" FORCE)
set(SDL_VIDEO ON CACHE BOOL "Build SDL video support" FORCE)
set(SDL_JOYSTICK ON CACHE BOOL "Build SDL joystick support" FORCE)
set(SDL_RENDER OFF CACHE BOOL "Build SDL 2D render subsystem" FORCE)
set(SDL_GPU OFF CACHE BOOL "Build SDL GPU subsystem" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "Build SDL camera subsystem" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "Build SDL haptic subsystem" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "Build SDL sensor subsystem" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "Build SDL dialog subsystem" FORCE)
set(SDL_TRAY OFF CACHE BOOL "Build SDL tray subsystem" FORCE)
set(SDL_PROCESS OFF CACHE BOOL "Build SDL process subsystem" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "Build SDL HIDAPI support" FORCE)
if(CMAKE_SYSTEM_NAME STREQUAL "DOS")
    set(SDL_DISKAUDIO OFF CACHE BOOL "Support disk audio driver" FORCE)
    set(SDL_DUMMYAUDIO OFF CACHE BOOL "Support dummy audio driver" FORCE)
    set(SDL_DUMMYVIDEO OFF CACHE BOOL "Support dummy video driver" FORCE)
endif()
set(SDL_OPENGL OFF CACHE BOOL "Include OpenGL support" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "Include OpenGL ES support" FORCE)
set(SDL_ASSEMBLY OFF CACHE BOOL "Enable SIMD assembly routines" FORCE)
set(SDL_TESTS OFF CACHE BOOL "Build SDL tests" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "Build SDL examples" FORCE)

# SDL3 remains an external submodule / directory and is never vendored or modified.
set(SDL3_DOS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDL" CACHE PATH
    "Path to the SDL3 source tree containing the DOS backend")
if(NOT EXISTS "${SDL3_DOS_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "SDL3 sources were not found at '${SDL3_DOS_SOURCE_DIR}'. "
        "Clone https://github.com/libsdl-org/SDL into SDL/ or set "
        "-DSDL3_DOS_SOURCE_DIR=/path/to/SDL.")
endif()
add_subdirectory("${SDL3_DOS_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/SDL" EXCLUDE_FROM_ALL)

# Shared 80386 low-memory & hardware compatibility layer (`dos_compat.c`).
# Reusable drop-in target: link any new SDL3/DOS executable against `dos_compat`
# to inherit the 64 KB stack, pure protected-mode IRQ 1 keyboard driver, PIT
# timer pacing, non-VESA VGA restoration, partial scanline VGA updates, and x87
# software emulation automatically.
add_library(dos_compat OBJECT dos_compat.c)
target_link_libraries(dos_compat PUBLIC SDL3::SDL3)
if(CMAKE_SYSTEM_NAME STREQUAL "DOS")
    target_link_options(dos_compat PUBLIC
        "-Wl,--gc-sections"
        "-Wl,--wrap=DOS_HookInterrupt"
        "-Wl,--wrap=DOS_UnhookInterrupt"
        "-Wl,--wrap=__dpmi_int"
        "-Wl,--wrap=SDL_SendKeyboardKey"
        "-Wl,--wrap=SDL_SendKeyboardText"
        "-Wl,--wrap=DOSVESA_SetDisplayMode"
        "-Wl,--wrap=DOSVESA_UpdateWindowFramebuffer"
        "-Wl,--wrap=SDL_SYS_DelayNS")
    if(NOT "${CMAKE_EXE_LINKER_FLAGS}" MATCHES "-lemu")
        target_link_options(dos_compat PUBLIC
            "-Wl,--whole-archive" "-lemu" "-Wl,--no-whole-archive")
    endif()
endif()

set(SDL3_386_TARGETS hello mistify sbtest modplay showcase shooter fontdemo dino)

add_executable(hello main.c)
add_executable(mistify mistify.c)
add_executable(sbtest soundtest.c)
add_executable(modplay modplay.c)
add_executable(showcase showcase.c)
add_executable(shooter shooter.c)
add_executable(fontdemo fontdemo.c)
add_executable(dino dinorun.c)

foreach(tgt IN LISTS SDL3_386_TARGETS)
    target_link_libraries(${tgt} PRIVATE dos_compat)
    if(CMAKE_SYSTEM_NAME STREQUAL "DOS" AND CMAKE_STRIP)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND "${CMAKE_STRIP}" --strip-all "$<TARGET_FILE:${tgt}>"
            COMMENT "Stripping symbols from ${tgt}.exe for fast 386 disk startup")
    endif()
endforeach()

file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/Images"
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/SHOWCASE"
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

add_custom_command(TARGET showcase POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/SHOWCASE"
            "${CMAKE_CURRENT_BINARY_DIR}/SHOWCASE")
add_custom_command(TARGET shooter POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/SHOWCASE"
            "${CMAKE_CURRENT_BINARY_DIR}/SHOWCASE")
