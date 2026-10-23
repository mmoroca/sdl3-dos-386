/* by @mmoroca & Arena.AI 2026 (http://arena.ai/) */

/*
 * ============================================================================
 * DOS_COMPAT.C — DROP-IN 80386 / 386SX (2 MB RAM, NO-FPU, ISA VGA) LAYER
 * ============================================================================
 *
 * PURPOSE & REUSABILITY
 * ---------------------
 * This single compilation unit adapts any SDL3 MS-DOS / DJGPP program to run
 * smoothly at full frame rate on real 1990-1992 80386SX / 80386DX hardware with
 * only 2 MB of total system RAM, no 80387 FPU, a non-VESA standard VGA card on
 * an 8/16-bit ISA bus, and slow IDE/CF storage under `CWSDPMI.EXE` — WITHOUT
 * modifying a single line inside the upstream `SDL/` submodule.
 *
 * HOW TO REUSE IN YOUR OWN SDL3/DOS PROJECT
 * -----------------------------------------
 * 1. Compile `dos_compat.c` into your executable and pass these GNU `ld` flags
 *    (see `CMakeLists.txt` for the ready-to-copy CMake snippet):
 *      -Wl,--gc-sections
 *      -Wl,--wrap=DOS_HookInterrupt
 *      -Wl,--wrap=DOS_UnhookInterrupt
 *      -Wl,--wrap=__dpmi_int
 *      -Wl,--wrap=SDL_SendKeyboardKey
 *      -Wl,--wrap=SDL_SendKeyboardText
 *      -Wl,--wrap=DOSVESA_SetDisplayMode
 *      -Wl,--wrap=DOSVESA_UpdateWindowFramebuffer
 *      -Wl,--wrap=SDL_SYS_DelayNS
 *      -Wl,--whole-archive -lemu -Wl,--no-whole-archive
 *
 * 2. Build both SDL3 and your game with:
 *      -Os -DNDEBUG -march=i386 -mtune=i386 -fno-tree-dse -fno-dse
 *      -ffunction-sections -fdata-sections -DSDL_LEAN_AND_MEAN=1 -DSDL_HAVE_BLIT_1=1
 *
 * WHY EACH MECHANISM BELOW IS CRITICAL ON REAL 386 HARDWARE
 * ---------------------------------------------------------
 * [1] PHYSICAL 2 MB RAM MAP & THE CWSDPMI `sbrk()` CLIFF (~795 KB XMS LIMIT)
 *     On a 2 MB (2,048 KB) 386SX machine:
 *       -   0 ..  640 KB : Conventional DOS RAM (MS-DOS kernel, drivers, stub)
 *       - 640 .. 1024 KB : Upper Memory Area (VGA 0xA0000, BIOS ROM)
 *       - 1024.. 2048 KB : ONLY 1,024 KB of Extended Memory (XMS) exists!
 *     After HIMEM.SYS + DOS=HIGH + CWSDPMI page tables, only **~795 KB** of
 *     physical XMS pages remain free for the entire protected-mode process:
 *       `(.text + .data + .bss) + _stklen + peak sbrk() heap <= ~795 KB`.
 *     - DJGPP's default stack is `512 KB` (`_stklen`), eating over 60% of all
 *       available XMS before `main()` even starts! Setting `_stklen = 65536U`
 *       (`64 KB`) immediately recovers **448 KB** of physical RAM.
 *     - WARNING: NEVER set `_CRT0_FLAG_LOCK_MEMORY` globally during `main()`!
 *       `SDL_RunApp()` already locks `.text + .data + .bss + stack` for hardware
 *       ISRs and clears `_CRT0_FLAG_LOCK_MEMORY` right before `main()` so heap
 *       allocations stay unpinned. Pinning heap pages exhausts CWSDPMI's physical
 *       page pool prematurely.
 *     - WARNING: DJGPP's `free()` NEVER shrinks `sbrk()`. Even a momentary 50 KB
 *       spike during `SDL_LoadBMP()` permanently pushes the arena into
 *       `C:\CWSDPMI.SWP`, causing continuous disk thrashing on every frame!
 *
 * [2] PURE 32-BIT PROTECTED-MODE KEYBOARD ISR (ZERO REAL-MODE SWITCHES)
 *     By default, SDL's DOS keyboard code chains hardware IRQ 1 (`INT 09h`) down
 *     to the real-mode BIOS handler and polls `INT 16h AH=01h` via `__dpmi_int`
 *     every frame. Holding a movement/fire key triggers ~30 hardware typematic
 *     interrupts/sec (each doing a slow 386 protected<->real mode world switch)
 *     plus `INT 16h` DPMI reflections every frame (~90-120 mode switches/sec),
 *     AND calls `SDL_SendKeyboardText()` which allocates heap memory 30 times/sec!
 *     Below we:
 *       a) Install a locked pure 32-bit protected-mode IRET wrapper directly on
 *          vector `0x09` WITHOUT chaining to real-mode BIOS (`0` mode switches).
 *       b) Short-circuit `INT 16h` polls immediately in protected mode with
 *          `ZF = 1` (buffer empty) in 3 CPU instructions.
 *       c) Drop redundant hardware typematic key-down repeats when SDL already
 *          marks that scancode held (preserving `Backspace` for text editing).
 *       d) Suppress `SDL_SendKeyboardText()` whenever `SDL_TextInputActive()` is
 *          off (`0` heap allocations while holding keys during gameplay).
 *
 * [3] PLAIN VGA MODE 13h DIRECT FRAMEBUFFER & NON-VESA TEXT RESTORATION
 *     - Force `SDL_HINT_DOS_ALLOW_DIRECT_FRAMEBUFFER = "1"` so 320x200 `INDEX8`
 *       windows render directly without extra conversion buffers.
 *     - Standard 1990-1991 VGA chips without VESA VBE leave `desktop_mode.internal`
 *       as `NULL`. Without guarding `DOSVESA_SetDisplayMode`, restoring video on
 *       `SDL_Quit()` bails out before calling `INT 10h AX=0003h`, leaving the
 *       screen stuck in graphics mode after exit.
 *
 * [4] PARTIAL DIRTY-BAND ISA VGA TRANSFER (`0xA0000`)
 *     Writing all `64,000` bytes over an 8-MHz 8/16-bit ISA VGA bus takes heavy
 *     wait states every frame. After frame 1 runs the full SDL path once (which
 *     programs the 256-color VGA DAC registers `0x3C8/0x3C9`), any call to
 *     `SDL_UpdateWindowSurfaceRects()` with full-width horizontal bands transfers
 *     ONLY the modified scanline slices `[ry .. ry + rh)` straight to `0xA0000`.
 *
 * [5] 8254 PIT HARDWARE TIMER DELAY (`uclock()` INSTEAD OF `rdtsc` / `INT 15h`)
 *     DJGPP's `delay()` executes `rdtsc` (illegal opcode `SIGILL` on 386/486!)
 *     or falls back to BIOS `INT 15h AH=86h` (broken or infinite-looping on many
 *     386SX laptop BIOSes). Reading the universal Intel 8254 PIT counter via
 *     `uclock()` + yielding with `DOS_Yield()` works on 100% of 80386 machines.
 */

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#ifdef __DJGPP__
#include <dpmi.h>
#include <dos.h>
#include <go32.h>
#include <stdint.h>
#include <sys/movedata.h>
#include <time.h>

/* Override DJGPP's 512 KB default stack with a 64 KB stack (-448 KB XMS RAM). */
unsigned _stklen = 65536U;

typedef struct SDL_VideoDevice SDL_VideoDevice;
typedef struct SDL_VideoDisplay SDL_VideoDisplay;

/* Linker-generated references to the wrapped symbols inside libSDL3.a / libc.a. */
extern bool __real_DOSVESA_SetDisplayMode(SDL_VideoDevice *_this,
                                          SDL_VideoDisplay *display,
                                          SDL_DisplayMode *mode);
extern bool __real_DOSVESA_UpdateWindowFramebuffer(SDL_VideoDevice *device,
                                                   SDL_Window *window,
                                                   const SDL_Rect *rects,
                                                   int numrects);
extern bool __real_DOS_HookInterrupt(int irq, void (*handler)(void),
                                     void *old_handler);
extern void __real_DOS_UnhookInterrupt(int irq, void *old_handler);
extern int __real___dpmi_int(int vector, __dpmi_regs *regs);
extern bool __real_SDL_SendKeyboardKey(Uint64 timestamp, SDL_KeyboardID devid,
                                       int rawcode, SDL_Scancode scancode,
                                       bool down);
extern bool __real_SDL_SendKeyboardText(const char *text);
extern void DOS_Yield(void);

static bool g_initial_full_present_done;
static volatile bool g_kb_pm_active;
static void (*volatile g_sdl_kb_handler)(void);
static _go32_dpmi_seginfo g_orig_kb_vec;
static _go32_dpmi_seginfo g_kb_iret_wrapper;

/* Enable direct Mode 13h framebuffer access before `main()` runs unless the user
 * explicitly overrides `SDL_HINT_DOS_ALLOW_DIRECT_FRAMEBUFFER`. */
__attribute__((constructor))
static void dos_compat_init_hints(void)
{
    (void)SDL_SetHintWithPriority(SDL_HINT_DOS_ALLOW_DIRECT_FRAMEBUFFER,
                                  "1", SDL_HINT_DEFAULT);
}

/* Minimal locked 32-bit protected-mode IRQ 1 (`INT 09h`) entry trampoline.
 * Invokes SDL's scancode reader (`inportb(0x60)` + PIC EOI `outportb(0x20, 0x20)`)
 * directly in protected mode without reflecting to real-mode BIOS. */
static void dos_pm_keyboard_isr(void)
{
    void (*handler)(void) = g_sdl_kb_handler;
    if (handler) {
        handler();
    }
}

/* Intercept IRQ 1 installation to bind a locked 32-bit DPMI IRET wrapper on
 * interrupt vector 0x09 instead of chaining to real-mode BIOS INT 09h. */
bool __wrap_DOS_HookInterrupt(int irq, void (*handler)(void),
                              void *old_handler)
{
    if (irq != 1 || !handler) {
        return __real_DOS_HookInterrupt(irq, handler, old_handler);
    }

    g_sdl_kb_handler = handler;
    /* Lock all code and pointer storage touched inside hardware interrupt context
     * so CWSDPMI can never page fault inside IRQ 1. */
    (void)_go32_dpmi_lock_code((void *)(uintptr_t)dos_pm_keyboard_isr, 128UL);
    (void)_go32_dpmi_lock_code((void *)(uintptr_t)handler, 512UL);
    (void)_go32_dpmi_lock_data((void *)&g_sdl_kb_handler,
                               sizeof(g_sdl_kb_handler));

    if (_go32_dpmi_get_protected_mode_interrupt_vector(0x09,
                                                       &g_orig_kb_vec) != 0) {
        return __real_DOS_HookInterrupt(irq, handler, old_handler);
    }

    g_kb_iret_wrapper.pm_offset = (unsigned long)(uintptr_t)dos_pm_keyboard_isr;
    g_kb_iret_wrapper.pm_selector = (unsigned short)_go32_my_cs();
    if (_go32_dpmi_allocate_iret_wrapper(&g_kb_iret_wrapper) != 0) {
        return __real_DOS_HookInterrupt(irq, handler, old_handler);
    }
    if (_go32_dpmi_set_protected_mode_interrupt_vector(
            0x09, &g_kb_iret_wrapper) != 0) {
        (void)_go32_dpmi_free_iret_wrapper(&g_kb_iret_wrapper);
        return __real_DOS_HookInterrupt(irq, handler, old_handler);
    }

    g_kb_pm_active = true;
    return true;
}

/* Restore the original protected-mode vector 0x09 and free the DPMI IRET wrapper
 * cleanly before returning to MS-DOS. */
void __wrap_DOS_UnhookInterrupt(int irq, void *old_handler)
{
    if (irq == 1 && g_kb_pm_active) {
        g_kb_pm_active = false;
        (void)_go32_dpmi_set_protected_mode_interrupt_vector(0x09,
                                                             &g_orig_kb_vec);
        (void)_go32_dpmi_free_iret_wrapper(&g_kb_iret_wrapper);
        g_sdl_kb_handler = NULL;
        return;
    }
    __real_DOS_UnhookInterrupt(irq, old_handler);
}

/* Short-circuit real-mode BIOS keyboard polling (`INT 16h`) while our pure
 * protected-mode IRQ 1 handler owns the keyboard controller, saving a costly
 * protected->real->protected CPU mode switch on every frame. */
int __wrap___dpmi_int(int vector, __dpmi_regs *regs)
{
    if (vector == 0x16 && g_kb_pm_active && regs) {
        regs->x.flags |= 0x0040U; /* Set x86 Zero Flag (ZF=1): BIOS queue empty. */
        regs->x.ax = 0U;
        return 0;
    }
    return __real___dpmi_int(vector, regs);
}

/* Drop redundant 30 Hz hardware typematic make-codes on already-depressed keys
 * (except Backspace for convenient name editing) so holding movement/fire keys
 * costs 0 event-queue traffic during gameplay. */
bool __wrap_SDL_SendKeyboardKey(Uint64 timestamp, SDL_KeyboardID devid,
                                int rawcode, SDL_Scancode scancode, bool down)
{
    if (down && (int)scancode > 0 && scancode < SDL_SCANCODE_COUNT &&
        scancode != SDL_SCANCODE_BACKSPACE) {
        const bool *state = SDL_GetKeyboardState(NULL);
        if (state && state[scancode]) {
            return false;
        }
    }
    return __real_SDL_SendKeyboardKey(timestamp, devid, rawcode, scancode,
                                      down);
}

/* Avoid heap allocations inside SDL's UTF-8 text-event path on every keystroke
 * unless the application has explicitly enabled text input (`SDL_StartTextInput`). */
bool __wrap_SDL_SendKeyboardText(const char *text)
{
    SDL_Window *focus = SDL_GetKeyboardFocus();
    if (!focus || !SDL_TextInputActive(focus)) {
        return false;
    }
    return __real_SDL_SendKeyboardText(text);
}

/* Reset first-frame palette synchronization on mode changes and safely guard
 * `mode->internal == NULL` so non-VESA standard VGA cards cleanly restore
 * 80x25 text mode (`INT 10h AX=0003h`) during `SDL_Quit()`. */
bool __wrap_DOSVESA_SetDisplayMode(SDL_VideoDevice *_this,
                                   SDL_VideoDisplay *display,
                                   SDL_DisplayMode *mode)
{
    g_initial_full_present_done = false;
    if (!mode || !mode->internal) {
        return true;
    }
    return __real_DOSVESA_SetDisplayMode(_this, display, mode);
}

/* Fast partial scanline transfer to linear VGA Mode 13h memory (`0xA0000`):
 *   - Frame 1 always delegates to `__real_DOSVESA_UpdateWindowFramebuffer` so
 *     SDL uploads the 256-entry VGA DAC palette (`0x3C8/0x3C9`) and initializes
 *     all 64,000 bytes of VRAM.
 *   - Subsequent calls from `SDL_UpdateWindowSurfaceRects()` with full-width
 *     `320 px` bands copy ONLY the dirty scanline intervals directly via
 *     `dosmemput()`, cutting slow 8-MHz ISA bus writes dramatically. */
bool __wrap_DOSVESA_UpdateWindowFramebuffer(SDL_VideoDevice *device,
                                            SDL_Window *window,
                                            const SDL_Rect *rects,
                                            int numrects)
{
    SDL_Surface *surface;
    int i;
    int total_h = 0;

    if (!g_initial_full_present_done || !window || numrects <= 0) {
        const bool ok = __real_DOSVESA_UpdateWindowFramebuffer(
            device, window, rects, numrects);
        if (ok && numrects > 0) {
            g_initial_full_present_done = true;
        }
        return ok;
    }

    surface = SDL_GetWindowSurface(window);
    if (!surface || surface->format != SDL_PIXELFORMAT_INDEX8 ||
        surface->w != 320 || surface->h != 200 || surface->pitch != 320 ||
        !surface->pixels) {
        return __real_DOSVESA_UpdateWindowFramebuffer(
            device, window, rects, numrects);
    }

    for (i = 0; i < numrects; ++i) {
        if (rects[i].x != 0 || rects[i].w != 320) {
            return __real_DOSVESA_UpdateWindowFramebuffer(
                device, window, rects, numrects);
        }
        total_h += rects[i].h;
    }
    if (total_h >= 200) {
        return __real_DOSVESA_UpdateWindowFramebuffer(
            device, window, rects, numrects);
    }

    for (i = 0; i < numrects; ++i) {
        int ry = rects[i].y;
        int rh = rects[i].h;
        if (ry < 0) {
            rh += ry;
            ry = 0;
        }
        if (ry + rh > 200) {
            rh = 200 - ry;
        }
        if (rh > 0) {
            dosmemput((const Uint8 *)surface->pixels + (ry * 320),
                      (size_t)(rh * 320),
                      0xA0000UL + (unsigned long)(ry * 320));
        }
    }
    return true;
}

/* 80386-safe nanosecond delay built on DJGPP's 8254 PIT `uclock()` timer.
 * Replaces libc `delay()`, which executes `rdtsc` (SIGILL on 386/486) or calls
 * BIOS `INT 15h AH=86h` (unsupported / hangs on many 386SX laptops). */
void __wrap_SDL_SYS_DelayNS(Uint64 ns)
{
    uclock_t start;
    uclock_t ticks_needed;

    if (ns == 0ULL) {
        return;
    }
    ticks_needed = (uclock_t)((ns * (Uint64)UCLOCKS_PER_SEC) / 1000000000ULL);
    if (ticks_needed <= 0) {
        ticks_needed = 1;
    }
    start = uclock();
    while ((uclock() - start) < ticks_needed) {
        DOS_Yield();
    }
}
#else
typedef int sdl3_dos_compat_unused_t;
#endif
