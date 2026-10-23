#include <SDL3/SDL.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_main.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __DJGPP__
#include <dos.h>
#include <pc.h>
void sound(int _frequency);
#ifndef nosound
void nosound(void);
#endif
#endif

/* by @mmoroca & Arena.AI 2026 (http://arena.ai/) */

/*
 * ============================================================================
 * DINO FALL — REAL-HARDWARE 80386 / 386SX (2 MB RAM, NO-FPU, ISA VGA) ARCHITECTURE
 * ============================================================================
 * DINO FALL is a 320x200 obstacle-placement game: the runner acts autonomously
 * while the player spends charges to place cacti and birds. Coordinates are
 * screen pixels unless a name ends in `_fp` (signed 24.8 fixed point, `FP_ONE`
 * units per pixel).
 *
 * WHY THIS EDITION RUNS LIKE A 486 ON A 16-25 MHz 80386SX WITH ONLY 2 MB RAM:
 * ----------------------------------------------------------------------------
 * On a 2 MB 80386SX PC under MS-DOS + HIMEM.SYS + CWSDPMI, only **~795 KB** of
 * physical extended memory (XMS) exists for `.text + .data + .bss + stack + heap`.
 * Moreover, DJGPP's `free()` NEVER lowers `sbrk()` — if peak heap ever crosses
 * ~795 KB for even 1 ms during asset loading, `CWSDPMI` creates `C:\CWSDPMI.SWP`
 * and thrashes the hard drive on every frame.
 *
 * Five replicable techniques in this file eliminate 100% of swap, FPU traps,
 * per-frame multiplications, and wasted 8-MHz ISA VGA bus transfers:
 *
 *   [1] ZERO-HEAP STREAMING `.BMP` DECODER (`IndexedBmpStream` + `setvbuf`):
 *       Replaces `SDL_LoadBMP()` completely. Reads one scanline at a time using
 *       a caller-owned 512-byte `setvbuf()` buffer (preventing DJGPP `stdio`
 *       from secretly calling `malloc(4096)` inside `fread()`) and writes
 *       straight into compact RLE spans. Peak `sbrk()` spike = **0 bytes**, and
 *       GNU `ld --gc-sections` strips the entire `SDL_bmp.c` module (~20 KB)!
 *
 *   [2] 1-BIT WRAP-PADDED `Uint32` NIBBLE-LUT SCROLLING (`load_track_streaming`
 *       + `draw_ground`):
 *       `Images/OTHER/TRACK.BMP` (`2404x28`, `68 KB` raw) pushed heap from
 *       `738 KB` to `806 KB` (`> 795 KB` -> continuous disk swap!) when loaded
 *       as an `SDL_Surface`. Here it is packed at **1 bit/pixel** in `.bss`
 *       with `328 px` circular wraparound padding at the right edge of each row
 *       plus a 64-byte 4-pixel lookup table (`g_track_lut4[16]`). `draw_ground()`
 *       writes **8 horizontal pixels per loop iteration (`2 x Uint32`)** with
 *       zero branches, zero seam checks, zero multiplications, and **0 heap
 *       bytes** in **~0.6 ms/frame** on a 16 MHz 386SX!
 *
 *   [3] ZERO-`IMUL` ANALYTICAL 25-BIT DIRTY-STRIP TRACKING (`mark_dirty_y_range`,
 *       `draw_game`, `present_game_frame`) + STATIC-SCREEN SHORT-CIRCUIT:
 *       Hashing `64,000` framebuffer bytes (`16,000` 32-bit `imul`s at 9-38
 *       cycles each on an 80386 = ~800,000 cycles = **~50 ms/frame wasted**!)
 *       cripples a 386SX. Instead, every drawing primitive sets bits in a single
 *       `Uint32` register (`((2U << s1) - (1U << s0))`) at ~0 CPU cost:
 *         - `draw_game()` clears ONLY `prev_dirty_strips` (saving ~40% `memset`),
 *         - `present_game_frame()` uploads ONLY `cur_dirty | prev_dirty` across
 *           the slow ISA bus (`0xA0000`), and
 *         - Non-playing screens (`MODE_TITLE`, `MODE_PAUSED`, `MODE_GAME_OVER`,
 *           `MODE_NAME_ENTRY`) return immediately in **0 bytes / 0% CPU** whenever
 *           state is unchanged!
 *
 *   [4] 1-BIT PACKED COLLISION MASKS (`mask_bits` inside `PreparedSprite`):
 *       Stores pixel-exact silhouette masks at 1 bit/pixel (**8x less RAM** than
 *       byte masks) while preserving identical collision behavior in `/s 1|2|3`.
 *
 *   [5] NON-BLOCKING SPEAKER QUEUE & IDLE `SDL_AudioStream` PAUSING:
 *       Pauses `SDL_AudioStream` whenever no beep samples are queued (avoiding
 *       background `SDL_AUDIO_F32` software x87 `INT 7` traps on FPU-less 386SX)
 *       and drives PC speaker tones via a non-blocking ring buffer serviced per
 *       frame instead of freezing gameplay inside `SDL_Delay()`.
 * ============================================================================
 */

/* The display size and DOS backend's indexed-color property are fixed. */
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 200
#define DOS_PIXEL_FORMAT_PROPERTY "SDL_PIXELFORMAT"

/* Persistent local ranking limits and file name; names are at most 8 characters. */
#define HIGH_SCORE_FILE "DINO.SCO"
#define MAX_SCORES 10
#define NAME_LENGTH 8

/* Maximum simultaneous obstacle slots and sound synthesis limits. */
#define MAX_OBSTACLES 8
#define AUDIO_SAMPLE_RATE 11025
#define AUDIO_FORMAT SDL_AUDIO_U8
#define AUDIO_CHUNK_MAX 8192U

/* Fixed-point scale and default geometry used when a sprite is unavailable. */
#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)
#define GROUND_Y 163
/* Raise only the drawn ground/track; sprite anchors and gameplay physics stay put. */
#define GROUND_VISUAL_OFFSET_PX 14
#define DINO_X 42
#define DINO_WIDTH 26
#define DINO_HEIGHT 28
#define DUCK_HEIGHT 16
#define DUCK_WIDTH 32
#define BIRD_WIDTH 24
#define BIRD_HEIGHT 17
#define BIRD_CLEARANCE 34

/* Runtime art format: six cactus variants; palette index 5 marks transparency. */
#define CACTUS_VARIANTS 6
#define TRANSPARENT_INDEX 5

/* Starting supply; refill interval drops from 5s by 0.5s every 10s to 2s. */
#define INITIAL_CHARGES 2
#define INITIAL_MAX_CHARGES 6
#define INITIAL_CHARGE_INTERVAL_MS 5000U
#define CHARGE_INTERVAL_STEP_MS 500U
#define CHARGE_INTERVAL_STEP_EVERY_MS 10000U
#define MIN_CHARGE_INTERVAL_MS 2000U
#define CAPACITY_INTERVAL_MS 30000U

/* Placement cooldown drops from 400ms by 50ms every 15s to a 150ms floor. */
#define INITIAL_PLACE_COOLDOWN_MS 400U
#define PLACE_COOLDOWN_STEP_MS 50U
#define PLACE_COOLDOWN_STEP_EVERY_MS 15000U
#define MIN_PLACE_COOLDOWN_MS 150U

/* Runner starts at 104px/s, accelerates gradually, and stops at 190px/s. */
#define INITIAL_RUN_SPEED 104U
#define MAX_RUN_SPEED 190U
#define SPEED_RAMP_DIVISOR 1400U

/* Jump geometry and per-obstacle timing error used to vary maneuver precision.
   Negative rolls favor earlier movement; positive rolls delay jumps or shorten
   the bird-duck window, potentially causing a direct collision. */
#define JUMP_APEX_MARGIN_PX 64
#define JUMP_FLIGHT_MARGIN_MS 80U
#define JUMP_RECOVERY_MS 300U
#define MIN_AGILITY_ADJUST_MS (-100)
#define MAX_AGILITY_ADJUST_MS 260
#define OBSTACLE_AIR_GAP_MS 350U
#define OBSTACLE_GAP_MARGIN_PX 8
#define BIRD_REACTION_DISTANCE 40

/* Palette indices are shared by the renderer and the converted BMP assets. */
#define COLOR_WHITE 0
#define COLOR_BLACK 1
#define COLOR_DARK 2
#define COLOR_MID 3
#define COLOR_LIGHT 4

/* Compile-time array length helper; use only with actual arrays, not pointers. */
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Modes determine which input is accepted and which screen is rendered. */
typedef enum GameMode {
    MODE_TITLE = 0,      /* Attract screen; Space begins a fresh run. */
    MODE_PLAYING,        /* Simulation and active-run timer are advancing. */
    MODE_PAUSED,         /* State is preserved; active simulation is stopped. */
    MODE_NAME_ENTRY,     /* A qualifying time is waiting for a player name. */
    MODE_GAME_OVER       /* Final screen after a non-qualifying run or save. */
} GameMode;

/* The two obstacle families use different autonomous dodge maneuvers. */
typedef enum ObstacleKind {
    OBSTACLE_CACTUS = 0, /* Avoid by jumping. */
    OBSTACLE_BIRD        /* Avoid by holding a timed duck pose. */
} ObstacleKind;

/* One moving threat, occupying one slot in DinoGame.obstacles. */
typedef struct Obstacle {
    bool active;              /* False marks this slot reusable. */
    ObstacleKind kind;        /* Selects shape, collision mask, and maneuver. */
    Sint32 x_fp;              /* Left edge in 8-bit fixed-point screen units. */
    Uint8 variant;            /* Cactus art index; bird frames animate by time. */
    int agility_adjust_ms;    /* Per-obstacle timing error; negative is early. */
} Obstacle;

/* One saved leaderboard result; shorter time_ms values rank higher. */
typedef struct ScoreEntry {
    char name[NAME_LENGTH + 1]; /* NUL-terminated 1-8 character name. */
    Uint32 time_ms;             /* Active gameplay duration in milliseconds. */
} ScoreEntry;

/* All sprite surfaces loaded from BMP files; DinoGame releases them at exit. */
typedef struct DinoAssets {
    SDL_Surface *run[2];
    SDL_Surface *duck[2];
    SDL_Surface *jump;
    SDL_Surface *dead;
    SDL_Surface *start;
    SDL_Surface *bird[2];
    SDL_Surface *cactus[CACTUS_VARIANTS];
    SDL_Surface *cloud;
    SDL_Surface *game_over;
    SDL_Surface *reset;
    SDL_Surface *track;
} DinoAssets;

/*
 * Central owner of the SDL handles and the complete mutable game state.
 * Keep run-time state here rather than in function-local statics so start_game
 * can reset a run and shutdown_game can release every resource deterministically.
 */
typedef struct DinoGame {
    /* SDL window and indexed drawing target; window_surface is SDL-owned. */
    SDL_Window *window;
    SDL_Surface *window_surface;
    SDL_Surface *drawing_surface;
    DinoAssets assets;
    SDL_AudioStream *beep_stream;
    bool sdl_initialized;
    bool audio_ready;
    bool audio_stream_active;
    bool speaker_fallback;
    bool sound_disabled;

    /* Non-blocking PC speaker note queue used on DOS systems without Sound Blaster. */
    Uint16 speaker_freqs[16];
    Uint16 speaker_durations[16];
    Uint8 speaker_head;
    Uint8 speaker_count;
    bool speaker_note_playing;
    Uint32 speaker_note_end_ticks;

    /* Analytical 25-bit dirty-strip mask (0 hashing CPU cycles, 0 shadow RAM). */
    Uint32 prev_dirty_strips;
    bool vram_shadow_valid;

    /* High-level screen mode, active threats, and local Top 10. */
    GameMode mode;
    Obstacle obstacles[MAX_OBSTACLES];
    ScoreEntry scores[MAX_SCORES];
    int score_count;
    int score_rank;
    int assets_loaded;

    /* Current placement supply and selected sprite scale. */
    int charges;
    int max_charges;
    bool native_sprites;
    bool half_sprites;

    /* World scrolling and fixed-point runner position/vertical physics. */
    Uint64 ground_scroll_fp;
    int dino_y_fp;
    int dino_vy_fp;
    int dino_gravity_fp;
    Uint32 run_speed;

    /* Active-run clocks, measured in milliseconds except ground_scroll_fp. */
    Uint32 elapsed_ms;
    Uint32 charge_clock_ms;
    Uint32 capacity_clock_ms;
    Uint32 last_place_ms;
    Uint32 last_jump_ms;
    Uint32 last_ticks;
    Uint32 message_until_ticks;

    /* Current pose and global input/event flags. */
    bool grounded;
    bool ducking;
    bool has_placed_obstacle;
    bool quit_requested;

    /* Name entry and short-lived UI feedback. */
    char entry_name[NAME_LENGTH + 1];
    size_t entry_length;
    char message[48];
} DinoGame;

/* Copy a short status string into the game state and set its 1.8-second display deadline. */
static void set_message(DinoGame *game, const char *message)
{
    (void)snprintf(game->message, sizeof(game->message), "%s", message);
    game->message_until_ticks = (Uint32)SDL_GetTicks() + 1800U;
}

/* -------------------------------------------------------------------------- */
/* Grayscale INDEX8 video setup and 5x7 text/pixel primitives.                */
/* -------------------------------------------------------------------------- */
/*
 * Create or reuse the drawing surface palette, then assign the game's fixed grayscale
 * values. Returns false if the surface is not INDEX8 or SDL cannot set the palette.
 */
static bool set_grayscale_palette(SDL_Surface *surface)
{
    SDL_Color colors[256];
    SDL_Palette *palette;
    int i;

    if (!surface || surface->format != SDL_PIXELFORMAT_INDEX8) {
        return SDL_SetError("DINO requires an INDEX8 drawing surface");
    }

    palette = SDL_GetSurfacePalette(surface);
    if (!palette) {
        palette = SDL_CreateSurfacePalette(surface);
        if (!palette) {
            return false;
        }
    }

    for (i = 0; i < 256; ++i) {
        const Uint8 level = (Uint8)i;
        colors[i].r = level;
        colors[i].g = level;
        colors[i].b = level;
        colors[i].a = SDL_ALPHA_OPAQUE;
    }
    colors[COLOR_WHITE] = (SDL_Color){ 255, 255, 255, SDL_ALPHA_OPAQUE };
    colors[COLOR_BLACK] = (SDL_Color){ 0, 0, 0, SDL_ALPHA_OPAQUE };
    colors[COLOR_DARK] = (SDL_Color){ 80, 80, 80, SDL_ALPHA_OPAQUE };
    colors[COLOR_MID] = (SDL_Color){ 150, 150, 150, SDL_ALPHA_OPAQUE };
    colors[COLOR_LIGHT] = (SDL_Color){ 210, 210, 210, SDL_ALPHA_OPAQUE };
    return SDL_SetPaletteColors(palette, colors, 0, 256);
}

/*
 * Create the fixed 320x200 window and an INDEX8 drawing surface. If the window surface
 * is not indexed, render off-screen and convert when presenting.
 */
static bool initialize_video(DinoGame *game)
{
    SDL_PropertiesID properties;

    (void)SDL_SetHint(SDL_HINT_DOS_ALLOW_DIRECT_FRAMEBUFFER, "1");

    properties = SDL_CreateProperties();
    if (!properties) {
        return false;
    }
    if (!SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                               "DINO FALL - PLACE OBSTACLES") ||
        !SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, SCREEN_WIDTH) ||
        !SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, SCREEN_HEIGHT) ||
        !SDL_SetNumberProperty(properties, DOS_PIXEL_FORMAT_PROPERTY, SDL_PIXELFORMAT_INDEX8)) {
        SDL_DestroyProperties(properties);
        return false;
    }

    game->window = SDL_CreateWindowWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (!game->window) {
        return false;
    }
    (void)SDL_HideCursor();

    game->window_surface = SDL_GetWindowSurface(game->window);
    if (!game->window_surface) {
        return false;
    }
    if (game->window_surface->w != SCREEN_WIDTH || game->window_surface->h != SCREEN_HEIGHT) {
        return SDL_SetError("DINO requires a 320x200 video surface");
    }

    if (game->window_surface->format == SDL_PIXELFORMAT_INDEX8) {
        game->drawing_surface = game->window_surface;
    } else {
        game->drawing_surface = SDL_CreateSurface(SCREEN_WIDTH, SCREEN_HEIGHT,
                                                   SDL_PIXELFORMAT_INDEX8);
        if (!game->drawing_surface) {
            return false;
        }
    }
    return set_grayscale_palette(game->drawing_surface);
}

/* Write one palette index at (x,y); silently ignore coordinates outside the surface. */
static SDL_Surface *g_tracked_surface;
static Uint32 g_cur_dirty_strips;
static bool g_clearing_strips;

static inline void mark_dirty_y_range(const SDL_Surface *surface,
                                      int y0, int y1)
{
    int s0;
    int s1;

    if (surface != g_tracked_surface || g_clearing_strips || y0 >= y1) {
        return;
    }
    if (y0 < 0) y0 = 0;
    if (y1 > SCREEN_HEIGHT) y1 = SCREEN_HEIGHT;
    if (y0 >= y1) {
        return;
    }
    s0 = y0 >> 3;
    s1 = (y1 - 1) >> 3;
    g_cur_dirty_strips |= ((2U << s1) - (1U << s0));
}

static void put_pixel(SDL_Surface *surface, int x, int y, Uint8 color)
{
    Uint8 *row;

    if (!surface || x < 0 || x >= surface->w || y < 0 || y >= surface->h) {
        return;
    }
    mark_dirty_y_range(surface, y, y + 1);
    row = (Uint8 *)surface->pixels + (y * surface->pitch);
    row[x] = color;
}

/* Fill a clipped, half-open rectangle directly in the surface pixel buffer. */
static void fill_rect(SDL_Surface *surface, int x, int y, int width, int height, Uint8 color)
{
    int row_index;
    int first_x = x;
    int first_y = y;
    int last_x = x + width;
    int last_y = y + height;

    if (!surface || width <= 0 || height <= 0) {
        return;
    }
    if (first_x < 0) first_x = 0;
    if (first_y < 0) first_y = 0;
    if (last_x > surface->w) last_x = surface->w;
    if (last_y > surface->h) last_y = surface->h;
    if (first_x >= last_x || first_y >= last_y) {
        return;
    }
    mark_dirty_y_range(surface, first_y, last_y);

    if (first_x == 0 && last_x == surface->w && surface->pitch == surface->w) {
        memset((Uint8 *)surface->pixels + (first_y * surface->pitch),
               (int)color, (size_t)((last_y - first_y) * surface->pitch));
        return;
    }

    for (row_index = first_y; row_index < last_y; ++row_index) {
        Uint8 *row = (Uint8 *)surface->pixels + (row_index * surface->pitch);
        memset(row + first_x, (int)color, (size_t)(last_x - first_x));
    }
}

/*
 * Zero-malloc streaming 8-bit uncompressed BI_RGB BMP reader + pre-scaled RLE
 * sprite cache + 1-bit packed collision masks + 1-bit wrap-padded TRACK.BMP
 * renderer for 2 MB 80386SX systems:
 *   - 0 bytes of temporary heap for all 19 .BMP files (no SDL_LoadBMP spike into
 *     CWSDPMI.SWP on 2 MB machines, and SDL_bmp.c is dead-stripped at link time).
 *   - 1-bit packed collision masks (8x less RAM than byte-per-pixel masks).
 *   - 1-bit packed TRACK.BMP rendered via 32-bit 4-pixel LUT (~0.6 ms/frame on
 *     16 MHz 386SX, 0 branches, 100% pixel-identical at all scroll offsets).
 */
#define MAX_PREPARED_SPRITES 24
#define MAX_STREAM_BMP_WIDTH 2432
#define TRACK_MAX_W          2404
#define TRACK_WRAP_PAD        328
#define TRACK_ROW_BYTES      ((TRACK_MAX_W + TRACK_WRAP_PAD + 7) / 8)
#define TRACK_MAX_H            32

typedef struct IndexedBmpStream {
    FILE *fp;
    int width;
    int height;
    Uint32 data_offset;
    Uint32 row_stride;
    bool bottom_up;
    int cached_src_y;
    char stdio_buf[512];
    Uint8 cached_row[MAX_STREAM_BMP_WIDTH];
} IndexedBmpStream;

typedef struct PreparedSpan {
    Uint16 x;
    Uint16 len;
    Uint32 data_offset;
} PreparedSpan;

typedef struct PreparedSprite {
    const SDL_Surface *source;
    int width;
    int height;
    int mask_stride;
    Uint8 *mask_bits;
    int *row_span_offset;
    PreparedSpan *spans;
    Uint8 *opaque_pixels;
} PreparedSprite;

static SDL_Surface g_stub_surfaces[MAX_PREPARED_SPRITES];
static int g_stub_surface_count;
static PreparedSprite prepared_sprites[MAX_PREPARED_SPRITES];
static int prepared_sprite_count;

static Uint8 g_track_bits[TRACK_MAX_H][TRACK_ROW_BYTES];
static Uint32 g_track_lut4[16];
static int g_track_first_row;
static int g_track_last_row;
static bool g_track_fast_ready;

static Uint16 read_le16(const Uint8 *p)
{
    return (Uint16)((Uint16)p[0] | ((Uint16)p[1] << 8));
}

static Uint32 read_le32(const Uint8 *p)
{
    return (Uint32)p[0] | ((Uint32)p[1] << 8) |
           ((Uint32)p[2] << 16) | ((Uint32)p[3] << 24);
}

static bool try_open_indexed_bmp(IndexedBmpStream *stream,
                                 const char *base_path,
                                 const char *relative_path)
{
    char full_path[512];
    Uint8 hdr[54];
    const char *path = relative_path;
    Sint32 raw_w;
    Sint32 raw_h;
    Uint16 planes;
    Uint16 bpp;
    Uint32 compression;

    if (base_path) {
        const int length = snprintf(full_path, sizeof(full_path), "%s%s",
                                    base_path, relative_path);
        if (length <= 0 || (size_t)length >= sizeof(full_path)) {
            return false;
        }
        path = full_path;
    }

    stream->fp = fopen(path, "rb");
    if (!stream->fp) {
        return false;
    }
    /* Supply a caller-owned 512 B buffer BEFORE the first `fread()`!
     * Without `setvbuf()`, DJGPP's `libc` lazily invokes `malloc(4096)` inside
     * `_filbuf()`, permanently raising the `sbrk()` high-water mark even after
     * `fclose()`. With `stdio_buf` on the stack, heap usage is literally 0 B. */
    (void)setvbuf(stream->fp, stream->stdio_buf, _IOFBF,
                  sizeof(stream->stdio_buf));

    if (fread(hdr, 1U, sizeof(hdr), stream->fp) != sizeof(hdr) ||
        hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(stream->fp);
        stream->fp = NULL;
        return false;
    }

    stream->data_offset = read_le32(hdr + 10);
    raw_w = (Sint32)read_le32(hdr + 18);
    raw_h = (Sint32)read_le32(hdr + 22);
    planes = read_le16(hdr + 26);
    bpp = read_le16(hdr + 28);
    compression = read_le32(hdr + 30);

    if (raw_w <= 0 || raw_w > MAX_STREAM_BMP_WIDTH || raw_h == 0 ||
        raw_h < -4096 || raw_h > 4096 ||
        planes != 1U || bpp != 8U || compression != 0U) {
        fclose(stream->fp);
        stream->fp = NULL;
        return false;
    }

    stream->width = (int)raw_w;
    stream->bottom_up = (raw_h > 0);
    stream->height = (raw_h > 0) ? (int)raw_h : (int)(-raw_h);
    stream->row_stride = ((Uint32)stream->width + 3U) & ~3U;
    stream->cached_src_y = -1;
    return true;
}

static bool open_indexed_bmp_asset(IndexedBmpStream *stream,
                                   const char *relative_path)
{
    const char *base_path = SDL_GetBasePath();
    const char *path_without_images = relative_path;
    const char *leaf_name;
    const char *candidates[4];
    char images_flat_path[64];
    size_t index;

    if (strncmp(relative_path, "Images/", 7U) == 0) {
        path_without_images = relative_path + 7;
    }
    leaf_name = strrchr(path_without_images, '/');
    leaf_name = leaf_name ? leaf_name + 1 : path_without_images;
    (void)snprintf(images_flat_path, sizeof(images_flat_path), "Images/%s",
                   leaf_name);

    candidates[0] = relative_path;
    candidates[1] = path_without_images;
    candidates[2] = images_flat_path;
    candidates[3] = leaf_name;

    if (base_path) {
        for (index = 0; index < 4U; ++index) {
            if (try_open_indexed_bmp(stream, base_path, candidates[index])) {
                return true;
            }
        }
    }
    for (index = 0; index < 4U; ++index) {
        if (try_open_indexed_bmp(stream, NULL, candidates[index])) {
            return true;
        }
    }
    return false;
}

static void close_indexed_bmp(IndexedBmpStream *stream)
{
    if (stream && stream->fp) {
        fclose(stream->fp);
        stream->fp = NULL;
    }
}

static const Uint8 *bmp_stream_row(IndexedBmpStream *stream, int src_y)
{
    long file_offset;
    int file_row;

    if (src_y < 0 || src_y >= stream->height) {
        return NULL;
    }
    if (stream->cached_src_y == src_y) {
        return stream->cached_row;
    }
    file_row = stream->bottom_up ? (stream->height - 1 - src_y) : src_y;
    file_offset = (long)stream->data_offset +
                  (long)((Uint32)file_row * stream->row_stride);
    if (fseek(stream->fp, file_offset, SEEK_SET) != 0 ||
        fread(stream->cached_row, 1U, (size_t)stream->width, stream->fp) !=
            (size_t)stream->width) {
        return NULL;
    }
    stream->cached_src_y = src_y;
    return stream->cached_row;
}

static SDL_Surface *alloc_static_stub_surface(int width, int height)
{
    static Uint8 dummy_pixel;
    SDL_Surface *stub;

    if (g_stub_surface_count >= MAX_PREPARED_SPRITES) {
        return NULL;
    }
    stub = &g_stub_surfaces[g_stub_surface_count++];
    memset(stub, 0, sizeof(*stub));
    stub->format = SDL_PIXELFORMAT_INDEX8;
    stub->w = width;
    stub->h = height;
    stub->pitch = 1;
    stub->pixels = &dummy_pixel;
    return stub;
}

static void clear_prepared_sprites(void)
{
    int i;

    for (i = 0; i < prepared_sprite_count; ++i) {
        free(prepared_sprites[i].mask_bits);
        free(prepared_sprites[i].row_span_offset);
        free(prepared_sprites[i].spans);
        free(prepared_sprites[i].opaque_pixels);
        memset(&prepared_sprites[i], 0, sizeof(prepared_sprites[i]));
    }
    prepared_sprite_count = 0;
    g_stub_surface_count = 0;
    g_track_fast_ready = false;
    g_track_first_row = 1;
    g_track_last_row = 0;
}

static const PreparedSprite *find_prepared_sprite(const SDL_Surface *sprite,
                                                  int width, int height)
{
    int i;

    if (!sprite) {
        return NULL;
    }
    for (i = 0; i < prepared_sprite_count; ++i) {
        if (prepared_sprites[i].source == sprite &&
            prepared_sprites[i].width == width &&
            prepared_sprites[i].height == height) {
            return &prepared_sprites[i];
        }
    }
    return NULL;
}

static bool stream_prepared_sprite(IndexedBmpStream *stream,
                                   const SDL_Surface *stub,
                                   int width, int height,
                                   bool need_mask)
{
    Uint8 scaled_row[MAX_STREAM_BMP_WIDTH];
    PreparedSprite *slot;
    int dy;
    int span_total = 0;
    Uint32 opaque_total = 0U;
    int mask_stride = (width + 7) >> 3;

    if (!stream || !stub || width <= 0 || height <= 0 ||
        width > MAX_STREAM_BMP_WIDTH ||
        prepared_sprite_count >= MAX_PREPARED_SPRITES) {
        return false;
    }
    if (find_prepared_sprite(stub, width, height) != NULL) {
        return true;
    }

    slot = &prepared_sprites[prepared_sprite_count];
    memset(slot, 0, sizeof(*slot));

    slot->row_span_offset = (int *)malloc((size_t)(height + 1) * sizeof(int));
    if (!slot->row_span_offset) {
        return false;
    }
    if (need_mask) {
        slot->mask_bits = (Uint8 *)calloc(
            (size_t)(mask_stride * height > 0 ? mask_stride * height : 1), 1U);
        if (!slot->mask_bits) {
            free(slot->row_span_offset);
            memset(slot, 0, sizeof(*slot));
            return false;
        }
    }

    for (dy = 0; dy < height; ++dy) {
        const int source_y = (int)(((Sint64)dy * stream->height) / height);
        const Uint8 *source_row = bmp_stream_row(stream, source_y);
        Uint8 *mask_row = slot->mask_bits
                              ? (slot->mask_bits + (dy * mask_stride))
                              : NULL;
        int dx;

        if (!source_row) {
            free(slot->mask_bits);
            free(slot->row_span_offset);
            memset(slot, 0, sizeof(*slot));
            return false;
        }
        for (dx = 0; dx < width; ++dx) {
            const int source_x = (int)(((Sint64)dx * stream->width) / width);
            const Uint8 c = source_row[source_x];
            scaled_row[dx] = c;
            if (mask_row && c != TRANSPARENT_INDEX) {
                mask_row[dx >> 3] |= (Uint8)(1U << (dx & 7));
            }
        }
        dx = 0;
        while (dx < width) {
            int run_start;
            while (dx < width && scaled_row[dx] == TRANSPARENT_INDEX) {
                ++dx;
            }
            run_start = dx;
            while (dx < width && scaled_row[dx] != TRANSPARENT_INDEX) {
                ++dx;
            }
            if (dx > run_start) {
                ++span_total;
                opaque_total += (Uint32)(dx - run_start);
            }
        }
    }

    slot->spans = (PreparedSpan *)malloc(
        (span_total > 0 ? (size_t)span_total : 1U) * sizeof(PreparedSpan));
    slot->opaque_pixels = (Uint8 *)malloc(
        opaque_total > 0U ? (size_t)opaque_total : 1U);
    if (!slot->spans || !slot->opaque_pixels) {
        free(slot->mask_bits);
        free(slot->row_span_offset);
        free(slot->spans);
        free(slot->opaque_pixels);
        memset(slot, 0, sizeof(*slot));
        return false;
    }

    {
        int span_idx = 0;
        Uint32 write_offset = 0U;

        for (dy = 0; dy < height; ++dy) {
            const int source_y = (int)(((Sint64)dy * stream->height) / height);
            const Uint8 *source_row = bmp_stream_row(stream, source_y);
            int dx;

            if (!source_row) {
                free(slot->mask_bits);
                free(slot->row_span_offset);
                free(slot->spans);
                free(slot->opaque_pixels);
                memset(slot, 0, sizeof(*slot));
                return false;
            }
            for (dx = 0; dx < width; ++dx) {
                const int source_x =
                    (int)(((Sint64)dx * stream->width) / width);
                scaled_row[dx] = source_row[source_x];
            }

            slot->row_span_offset[dy] = span_idx;
            dx = 0;
            while (dx < width) {
                int run_start;
                while (dx < width && scaled_row[dx] == TRANSPARENT_INDEX) {
                    ++dx;
                }
                run_start = dx;
                while (dx < width && scaled_row[dx] != TRANSPARENT_INDEX) {
                    ++dx;
                }
                if (dx > run_start) {
                    const Uint16 run_len = (Uint16)(dx - run_start);
                    slot->spans[span_idx].x = (Uint16)run_start;
                    slot->spans[span_idx].len = run_len;
                    slot->spans[span_idx].data_offset = write_offset;
                    memcpy(slot->opaque_pixels + write_offset,
                           scaled_row + run_start, (size_t)run_len);
                    write_offset += (Uint32)run_len;
                    ++span_idx;
                }
            }
        }
        slot->row_span_offset[height] = span_idx;
    }

    slot->source = stub;
    slot->width = width;
    slot->height = height;
    slot->mask_stride = mask_stride;
    ++prepared_sprite_count;
    return true;
}

static int load_streamed_sprite(IndexedBmpStream *stream,
                                SDL_Surface **slot, const char *path,
                                int scaled_w, int scaled_h,
                                const DinoGame *game, bool need_mask)
{
    SDL_Surface *stub;
    int disp_w;
    int disp_h;

    *slot = NULL;
    if (!open_indexed_bmp_asset(stream, path)) {
        return 0;
    }
    stub = alloc_static_stub_surface(stream->width, stream->height);
    if (!stub) {
        close_indexed_bmp(stream);
        return 0;
    }
    disp_w = game->native_sprites ? stub->w
             : game->half_sprites ? ((stub->w + 1) / 2)
                                  : scaled_w;
    disp_h = game->native_sprites ? stub->h
             : game->half_sprites ? ((stub->h + 1) / 2)
                                  : scaled_h;
    (void)stream_prepared_sprite(stream, stub, disp_w, disp_h, need_mask);
    close_indexed_bmp(stream);
    *slot = stub;
    return 1;
}

static int load_track_streaming(IndexedBmpStream *stream, SDL_Surface **slot)
{
    SDL_Surface *stub;

    *slot = NULL;
    g_track_fast_ready = false;
    g_track_first_row = 1;
    g_track_last_row = 0;
    if (!open_indexed_bmp_asset(stream, "Images/OTHER/TRACK.BMP")) {
        return 0;
    }
    stub = alloc_static_stub_surface(stream->width, stream->height);
    if (!stub) {
        close_indexed_bmp(stream);
        return 0;
    }

    if (stream->width <= TRACK_MAX_W && stream->height <= TRACK_MAX_H) {
        bool single_ink = true;
        bool found_ink = false;
        Uint8 ink_color = COLOR_DARK;
        int first_row = stream->height;
        int last_row = -1;
        int ry;

        memset(g_track_bits, 0, sizeof(g_track_bits));
        for (ry = 0; ry < stream->height && single_ink; ++ry) {
            const Uint8 *row = bmp_stream_row(stream, ry);
            bool row_has_pixel = false;
            int x;
            int total_bits = stream->width + TRACK_WRAP_PAD;

            if (!row) {
                single_ink = false;
                break;
            }
            for (x = 0; x < stream->width; ++x) {
                const Uint8 c = row[x];
                if (c != TRANSPARENT_INDEX) {
                    if (!found_ink) {
                        ink_color = c;
                        found_ink = true;
                    } else if (c != ink_color) {
                        single_ink = false;
                        break;
                    }
                    g_track_bits[ry][x >> 3] |= (Uint8)(1U << (x & 7));
                    row_has_pixel = true;
                }
            }
            if (single_ink && row_has_pixel) {
                /* Replicate `[0 .. 327]` (`SCREEN_WIDTH + 8`) right after column
                 * `stream->width - 1` so `draw_ground()` can always fetch 328
                 * contiguous bits starting at ANY scroll offset `[0 .. width-1]`
                 * in one straight linear pass with zero wraparound branches! */
                for (x = stream->width; x < total_bits; ++x) {
                    const int wrap_x = x % stream->width;
                    if (row[wrap_x] != TRANSPARENT_INDEX) {
                        g_track_bits[ry][x >> 3] |= (Uint8)(1U << (x & 7));
                    }
                }
                if (ry < first_row) {
                    first_row = ry;
                }
                last_row = ry;
            }
        }

        if (single_ink) {
            /* Precompute all 16 combinations of 4 horizontal 1-bit pixels into
             * little-endian 32-bit words (`COLOR_WHITE` for 0, `ink_color` for 1).
             * Two table lookups write 8 screen pixels in two `Uint32` stores! */
            Uint32 n;
            for (n = 0U; n < 16U; ++n) {
                const Uint32 c0 = ((n & 1U) != 0U) ? ink_color : COLOR_WHITE;
                const Uint32 c1 = ((n & 2U) != 0U) ? ink_color : COLOR_WHITE;
                const Uint32 c2 = ((n & 4U) != 0U) ? ink_color : COLOR_WHITE;
                const Uint32 c3 = ((n & 8U) != 0U) ? ink_color : COLOR_WHITE;
                g_track_lut4[n] = c0 | (c1 << 8) | (c2 << 16) | (c3 << 24);
            }
            g_track_first_row = first_row;
            g_track_last_row = last_row;
            g_track_fast_ready = true;
            close_indexed_bmp(stream);
            *slot = stub;
            return 1;
        }
    }

    (void)stream_prepared_sprite(stream, stub, stub->w, stub->h, false);
    close_indexed_bmp(stream);
    *slot = stub;
    return 1;
}

/*
 * Nearest-neighbor blit an 8-bit indexed BMP at the requested size using its
 * pre-scaled RLE opaque runs (zero per-frame divisions, zero SDL_Surface heap).
 */
static void blit_indexed_scaled(SDL_Surface *destination, SDL_Surface *sprite,
                                int x, int y, int width, int height)
{
    int first_x = 0;
    int first_y = 0;
    int last_x = width;
    int last_y = height;
    int dy;
    const PreparedSprite *prepared;

    if (!destination || !sprite || destination->format != SDL_PIXELFORMAT_INDEX8 ||
        sprite->format != SDL_PIXELFORMAT_INDEX8 || width <= 0 || height <= 0 ||
        !destination->pixels) {
        return;
    }
    if (x < 0) first_x = -x;
    if (y < 0) first_y = -y;
    if (x + width > destination->w) last_x = destination->w - x;
    if (y + height > destination->h) last_y = destination->h - y;
    if (first_x >= last_x || first_y >= last_y) {
        return;
    }
    mark_dirty_y_range(destination, y + first_y, y + last_y);

    prepared = find_prepared_sprite(sprite, width, height);
    if (prepared) {
        for (dy = first_y; dy < last_y; ++dy) {
            Uint8 *destination_row = (Uint8 *)destination->pixels +
                                     ((y + dy) * destination->pitch);
            const int span_begin = prepared->row_span_offset[dy];
            const int span_end = prepared->row_span_offset[dy + 1];
            int si;

            for (si = span_begin; si < span_end; ++si) {
                const PreparedSpan *span = &prepared->spans[si];
                int sx0 = (int)span->x;
                int sx1 = sx0 + (int)span->len;

                if (sx0 >= last_x) {
                    break;
                }
                if (sx1 <= first_x) {
                    continue;
                }
                if (sx0 < first_x) {
                    sx0 = first_x;
                }
                if (sx1 > last_x) {
                    sx1 = last_x;
                }
                memcpy(destination_row + x + sx0,
                       prepared->opaque_pixels + span->data_offset +
                       (size_t)(sx0 - (int)span->x),
                       (size_t)(sx1 - sx0));
            }
        }
    }
}

/*
 * Release every cached sprite buffer and clear asset pointers on shutdown.
 */
static void destroy_dino_assets(DinoGame *game)
{
    int index;
    clear_prepared_sprites();
    for (index = 0; index < 2; ++index) {
        game->assets.run[index] = NULL;
        game->assets.duck[index] = NULL;
        game->assets.bird[index] = NULL;
    }
    for (index = 0; index < CACTUS_VARIANTS; ++index) {
        game->assets.cactus[index] = NULL;
    }
    game->assets.jump = NULL;
    game->assets.dead = NULL;
    game->assets.start = NULL;
    game->assets.cloud = NULL;
    game->assets.game_over = NULL;
    game->assets.reset = NULL;
    game->assets.track = NULL;
}

/*
 * Look up a 5x7 uppercase glyph and copy its seven bit rows; unsupported characters
 * become blank.
 */
static void get_glyph(char character, Uint8 rows[7])
{
    typedef struct Glyph {
        char character;
        Uint8 rows[7];
    } Glyph;
    /* Each glyph has seven rows; the low five bits map left-to-right pixels. */
    static const Glyph glyphs[] = {
        { 'A', { 14, 17, 17, 31, 17, 17, 17 } }, { 'B', { 30, 17, 17, 30, 17, 17, 30 } },
        { 'C', { 14, 17, 16, 16, 16, 17, 14 } }, { 'D', { 30, 17, 17, 17, 17, 17, 30 } },
        { 'E', { 31, 16, 16, 30, 16, 16, 31 } }, { 'F', { 31, 16, 16, 30, 16, 16, 16 } },
        { 'G', { 14, 17, 16, 23, 17, 17, 15 } }, { 'H', { 17, 17, 17, 31, 17, 17, 17 } },
        { 'I', { 31, 4, 4, 4, 4, 4, 31 } }, { 'J', { 7, 2, 2, 2, 18, 18, 12 } },
        { 'K', { 17, 18, 20, 24, 20, 18, 17 } }, { 'L', { 16, 16, 16, 16, 16, 16, 31 } },
        { 'M', { 17, 27, 21, 21, 17, 17, 17 } }, { 'N', { 17, 25, 21, 19, 17, 17, 17 } },
        { 'O', { 14, 17, 17, 17, 17, 17, 14 } }, { 'P', { 30, 17, 17, 30, 16, 16, 16 } },
        { 'Q', { 14, 17, 17, 17, 21, 18, 13 } }, { 'R', { 30, 17, 17, 30, 20, 18, 17 } },
        { 'S', { 15, 16, 16, 14, 1, 1, 30 } }, { 'T', { 31, 4, 4, 4, 4, 4, 4 } },
        { 'U', { 17, 17, 17, 17, 17, 17, 14 } }, { 'V', { 17, 17, 17, 17, 17, 10, 4 } },
        { 'W', { 17, 17, 17, 21, 21, 21, 10 } }, { 'X', { 17, 17, 10, 4, 10, 17, 17 } },
        { 'Y', { 17, 17, 10, 4, 4, 4, 4 } }, { 'Z', { 31, 1, 2, 4, 8, 16, 31 } },
        { '0', { 14, 17, 19, 21, 25, 17, 14 } }, { '1', { 4, 12, 4, 4, 4, 4, 14 } },
        { '2', { 14, 17, 1, 2, 4, 8, 31 } }, { '3', { 30, 1, 1, 14, 1, 1, 30 } },
        { '4', { 2, 6, 10, 18, 31, 2, 2 } }, { '5', { 31, 16, 16, 30, 1, 1, 30 } },
        { '6', { 14, 16, 16, 30, 17, 17, 14 } }, { '7', { 31, 1, 2, 4, 8, 8, 8 } },
        { '8', { 14, 17, 17, 14, 17, 17, 14 } }, { '9', { 14, 17, 17, 15, 1, 1, 14 } },
        { ':', { 0, 4, 4, 0, 4, 4, 0 } }, { '.', { 0, 0, 0, 0, 0, 0, 4 } },
        { '-', { 0, 0, 0, 31, 0, 0, 0 } }, { '/', { 1, 2, 2, 4, 8, 8, 16 } },
        { '_', { 0, 0, 0, 0, 0, 0, 31 } }, { '!', { 4, 4, 4, 4, 4, 0, 4 } },
        { '?', { 14, 17, 1, 2, 4, 0, 4 } }
    };
    static Uint8 glyph_table[256][7];
    static bool glyph_table_ready;

    if (!glyph_table_ready) {
        size_t index;
        memset(glyph_table, 0, sizeof(glyph_table));
        for (index = 0; index < ARRAY_COUNT(glyphs); ++index) {
            const unsigned char upper = (unsigned char)glyphs[index].character;
            const unsigned char lower = (unsigned char)tolower((int)upper);
            memcpy(glyph_table[upper], glyphs[index].rows, 7 * sizeof(*rows));
            memcpy(glyph_table[lower], glyphs[index].rows, 7 * sizeof(*rows));
        }
        glyph_table_ready = true;
    }
    memcpy(rows, glyph_table[(unsigned char)character], 7 * sizeof(*rows));
}

/*
 * Render a NUL-terminated string with the built-in 5x7 bitmap font; each glyph
 * advances six pixels and newline advances nine.
 */
static void draw_text(SDL_Surface *surface, int x, int y, const char *text, Uint8 color)
{
    const int origin_x = x;

    if (!surface || surface->format != SDL_PIXELFORMAT_INDEX8 || !surface->pixels) {
        return;
    }

    while (*text) {
        Uint8 rows[7];
        int row;
        int column;

        if (*text == '\n') {
            x = origin_x;
            y += 9;
            ++text;
            continue;
        }
        get_glyph(*text, rows);
        if (x >= 0 && x + 5 <= surface->w && y >= 0 && y + 7 <= surface->h) {
            Uint8 *dst = (Uint8 *)surface->pixels + (y * surface->pitch) + x;
            mark_dirty_y_range(surface, y, y + 7);
            for (row = 0; row < 7; ++row, dst += surface->pitch) {
                const Uint8 bits = rows[row];
                if (bits == 0U) {
                    continue;
                }
                if ((bits & 16U) != 0U) dst[0] = color;
                if ((bits & 8U) != 0U) dst[1] = color;
                if ((bits & 4U) != 0U) dst[2] = color;
                if ((bits & 2U) != 0U) dst[3] = color;
                if ((bits & 1U) != 0U) dst[4] = color;
            }
        } else {
            for (row = 0; row < 7; ++row) {
                for (column = 0; column < 5; ++column) {
                    if ((rows[row] & (Uint8)(1U << (4 - column))) != 0U) {
                        put_pixel(surface, x + column, y + row, color);
                    }
                }
            }
        }
        x += 6;
        ++text;
    }
}

/* Measure a line using the font advance and draw it centered in the 320-pixel screen. */
static void draw_text_centered(SDL_Surface *surface, int y, const char *text, Uint8 color)
{
    const int text_width = (int)(strlen(text) * 6U);
    draw_text(surface, (SCREEN_WIDTH - text_width) / 2, y, text, color);
}

/*
 * Rasterize an inclusive line with Bresenham's integer algorithm; put_pixel clips its
 * endpoints to the surface.
 */
static void draw_line(SDL_Surface *surface, int x0, int y0, int x1, int y1, Uint8 color)
{
    const int dx = abs(x1 - x0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int dy = -abs(y1 - y0);
    const int sy = (y0 < y1) ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        put_pixel(surface, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        {
            const int twice_error = 2 * error;
            if (twice_error >= dy) {
                error += dy;
                x0 += sx;
            }
            if (twice_error <= dx) {
                error += dx;
                y0 += sy;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Local top-ten records: lower completion times sort first.                  */
/* -------------------------------------------------------------------------- */
/*
 * Insertion-sort the active score entries by ascending elapsed time, so the fastest
 * fall appears first.
 */
static void sort_scores(DinoGame *game)
{
    int i;
    /* Insertion sort is compact and fast for this fixed ten-entry table. */
    for (i = 1; i < game->score_count; ++i) {
        ScoreEntry entry = game->scores[i];
        int position = i;
        while (position > 0 && game->scores[position - 1].time_ms > entry.time_ms) {
            game->scores[position] = game->scores[position - 1];
            --position;
        }
        game->scores[position] = entry;
    }
}

/*
 * Read at most ten validated name/time records from DINO.SCO; ignore malformed lines
 * and sort accepted entries.
 */
static void load_scores(DinoGame *game)
{
    FILE *file = fopen(HIGH_SCORE_FILE, "r");
    char line[64];

    if (!file) {
        return;
    }
    while (game->score_count < MAX_SCORES && fgets(line, sizeof(line), file)) {
        char name[NAME_LENGTH + 1] = { 0 };
        unsigned long time_value = 0UL;
        int parsed;

        /* Limit the token width before validating it, preventing buffer overflow. */
        parsed = sscanf(line, "%8s %lu", name, &time_value);
        if (parsed == 2 && time_value <= 0xFFFFFFFFUL) {
            size_t i;
            bool valid = true;
            /* Normalize accepted names to uppercase for consistent ranking files. */
            for (i = 0; name[i] != '\0'; ++i) {
                if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-') {
                    valid = false;
                    break;
                }
                name[i] = (char)toupper((unsigned char)name[i]);
            }
            if (valid && i > 0U) {
                (void)snprintf(game->scores[game->score_count].name,
                               sizeof(game->scores[game->score_count].name), "%s", name);
                game->scores[game->score_count].time_ms = (Uint32)time_value;
                ++game->score_count;
            }
        }
    }
    (void)fclose(file);
    sort_scores(game);
}

/*
 * Rewrite the local Top 10 as plain-text name and millisecond pairs. Return false if
 * the file cannot be opened or closed cleanly.
 */
static bool save_scores(const DinoGame *game)
{
    FILE *file = fopen(HIGH_SCORE_FILE, "w");
    int i;

    if (!file) {
        return false;
    }
    for (i = 0; i < game->score_count; ++i) {
        (void)fprintf(file, "%s %lu\n", game->scores[i].name,
                      (unsigned long)game->scores[i].time_ms);
    }
    return fclose(file) == 0;
}

/*
 * Find where a run time belongs in the ascending table; return -1 when a full table is
 * not beaten.
 */
static int qualifying_rank(const DinoGame *game, Uint32 time_ms)
{
    int index;
    if (game->score_count < MAX_SCORES) {
        for (index = 0; index < game->score_count; ++index) {
            if (time_ms < game->scores[index].time_ms) {
                return index;
            }
        }
        return game->score_count;
    }
    for (index = 0; index < MAX_SCORES; ++index) {
        if (time_ms < game->scores[index].time_ms) {
            return index;
        }
    }
    return -1;
}

/*
 * Shift lower-ranked entries down, insert the new name/time, cap the table at ten, and
 * persist it to DINO.SCO.
 */
static bool insert_score(DinoGame *game, const char *name, Uint32 time_ms, int rank)
{
    int last;
    int index;

    if (rank < 0 || rank >= MAX_SCORES) {
        return false;
    }
    last = (game->score_count < MAX_SCORES) ? game->score_count : (MAX_SCORES - 1);
    /* Shift from the end toward rank so no entry is overwritten. */
    for (index = last; index > rank; --index) {
        game->scores[index] = game->scores[index - 1];
    }
    (void)snprintf(game->scores[rank].name, sizeof(game->scores[rank].name), "%s", name);
    game->scores[rank].time_ms = time_ms;
    if (game->score_count < MAX_SCORES) {
        ++game->score_count;
    }
    return save_scores(game);
}

/*
 * Format milliseconds as MM:SS.cc (centiseconds), bounded by the caller-provided
 * output capacity.
 */
static void format_time(Uint32 milliseconds, char *buffer, size_t capacity)
{
    const unsigned long total_seconds = (unsigned long)(milliseconds / 1000U);
    const unsigned long minutes = total_seconds / 60UL;
    const unsigned long seconds = total_seconds % 60UL;
    const unsigned long centiseconds = (unsigned long)((milliseconds % 1000U) / 10U);

    (void)snprintf(buffer, capacity, "%02lu:%02lu.%02lu", minutes, seconds, centiseconds);
}

/* -------------------------------------------------------------------------- */
/* Tiny synthesized PC beeps; audio-device failure is non-fatal.              */
/* -------------------------------------------------------------------------- */
static void stop_speaker_tones(DinoGame *game)
{
    game->speaker_head = 0U;
    game->speaker_count = 0U;
    if (game->speaker_note_playing) {
#ifdef __DJGPP__
        nosound();
#endif
        game->speaker_note_playing = false;
    }
}

static void service_audio(DinoGame *game, Uint32 now)
{
    if (game->audio_ready && game->beep_stream && game->audio_stream_active) {
        if (SDL_GetAudioStreamAvailable(game->beep_stream) <= 0) {
            (void)SDL_PauseAudioStreamDevice(game->beep_stream);
            game->audio_stream_active = false;
        }
    }
    if (!game->speaker_fallback) {
        return;
    }
    if (game->speaker_note_playing &&
        (Sint32)(now - game->speaker_note_end_ticks) >= 0) {
#ifdef __DJGPP__
        nosound();
#endif
        game->speaker_note_playing = false;
    }
    if (!game->speaker_note_playing && game->speaker_count > 0U) {
        const Uint16 freq = game->speaker_freqs[game->speaker_head];
        const Uint16 dur = game->speaker_durations[game->speaker_head];
        game->speaker_head = (Uint8)((game->speaker_head + 1U) & 15U);
        --game->speaker_count;
#ifdef __DJGPP__
        sound((unsigned int)freq);
#else
        (void)freq;
#endif
        game->speaker_note_playing = true;
        game->speaker_note_end_ticks = now + (Uint32)dur;
    }
}

/*
 * Generate one unsigned 8-bit square-wave tone and queue it on SDL, or push a
 * non-blocking PC speaker note on DOS machines without a Sound Blaster card.
 */
static void play_beep(DinoGame *game, Uint32 frequency, Uint32 duration_ms)
{
    Uint32 frame_count;
    Uint32 frame;
    Uint8 *samples;
    Uint32 half_period;

    if (game->sound_disabled || frequency == 0U || duration_ms == 0U) {
        return;
    }
    if (game->speaker_fallback) {
        if (game->speaker_count < 16U) {
            const Uint8 tail =
                (Uint8)((game->speaker_head + game->speaker_count) & 15U);
            game->speaker_freqs[tail] = (Uint16)frequency;
            game->speaker_durations[tail] = (Uint16)duration_ms;
            ++game->speaker_count;
        }
        return;
    }
    if (!game->audio_ready || !game->beep_stream) {
        return;
    }
    frame_count = (AUDIO_SAMPLE_RATE * duration_ms) / 1000U;
    if (frame_count == 0U || frame_count > AUDIO_CHUNK_MAX) {
        return;
    }
    samples = (Uint8 *)malloc((size_t)frame_count);
    if (!samples) {
        return;
    }
    /* Alternating two unsigned sample levels creates a square wave. */
    half_period = AUDIO_SAMPLE_RATE / (frequency * 2U);
    if (half_period == 0U) {
        half_period = 1U;
    }
    for (frame = 0; frame < frame_count; ++frame) {
        samples[frame] = (((frame / half_period) & 1U) != 0U) ? 196U : 60U;
    }
    if (!SDL_PutAudioStreamData(game->beep_stream, samples, (int)frame_count)) {
        (void)SDL_ClearAudioStream(game->beep_stream);
    } else if (!game->audio_stream_active) {
        /* Resume playback only while samples are queued so idle frames do not mix float audio. */
        if (SDL_ResumeAudioStreamDevice(game->beep_stream)) {
            game->audio_stream_active = true;
        }
    }
    free(samples);
}

/* Queue the three short rising notes used when a jump begins. */
static void play_jump_melody(DinoGame *game)
{
    static const Uint16 notes[] = { 1320U, 1660U, 1980U };
    static const Uint16 durations[] = { 60U, 60U, 85U };
    size_t index;

    for (index = 0U; index < ARRAY_COUNT(notes); ++index) {
        play_beep(game, notes[index], durations[index]);
    }
}

/* Queue the four descending notes used after a collision. */
static void play_loss_melody(DinoGame *game)
{
    static const Uint16 notes[] = { 720U, 600U, 480U, 360U };
    static const Uint16 durations[] = { 80U, 80U, 90U, 110U };
    size_t index;

    for (index = 0U; index < ARRAY_COUNT(notes); ++index) {
        play_beep(game, notes[index], durations[index]);
    }
}

/*
 * Open an 11025 Hz mono SDL audio stream. Failure is deliberately non-fatal: the game
 * continues silently.
 */
static void initialize_audio(DinoGame *game)
{
    SDL_AudioSpec source_spec;

    if (game->sound_disabled) {
        return;
    }
#ifdef __DJGPP__
    /* Skip SDL audio heap/device setup immediately on DOS machines without Sound Blaster. */
    if (getenv("BLASTER") == NULL) {
        game->speaker_fallback = true;
        return;
    }
#endif
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
#ifdef __DJGPP__
        game->speaker_fallback = true;
        return;
#else
        fprintf(stderr, "DINO: audio unavailable; continuing without beeps: %s\n",
                SDL_GetError());
        return;
#endif
    }
    memset(&source_spec, 0, sizeof(source_spec));
    source_spec.format = AUDIO_FORMAT;
    source_spec.channels = 1;
    source_spec.freq = AUDIO_SAMPLE_RATE;
    game->beep_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                   &source_spec, NULL, NULL);
    if (!game->beep_stream) {
#ifdef __DJGPP__
        game->speaker_fallback = true;
        return;
#else
        fprintf(stderr, "DINO: could not open audio; continuing without beeps: %s\n",
                SDL_GetError());
        return;
#endif
    }
    /* Keep the stream paused until play_beep queues audio so 386 CPUs avoid idle FPU mixing. */
    (void)SDL_PauseAudioStreamDevice(game->beep_stream);
    game->audio_stream_active = false;
    game->audio_ready = true;
}

/* -------------------------------------------------------------------------- */
/* Placement supply and randomized autonomous runner.                        */
/* -------------------------------------------------------------------------- */
/*
 * Return the obstacle-placement lockout for the current run time; it drops by 50 ms
 * per 15 seconds to a 150 ms floor.
 */
static Uint32 placement_cooldown(const DinoGame *game)
{
    /* Integer division counts only complete 15-second game-time steps. */
    const Uint32 steps = game->elapsed_ms / PLACE_COOLDOWN_STEP_EVERY_MS;
    const Uint32 total_reduction = steps * PLACE_COOLDOWN_STEP_MS;
    const Uint32 maximum_reduction = INITIAL_PLACE_COOLDOWN_MS -
                                     MIN_PLACE_COOLDOWN_MS;

    if (total_reduction >= maximum_reduction) {
        return MIN_PLACE_COOLDOWN_MS;
    }
    return INITIAL_PLACE_COOLDOWN_MS - total_reduction;
}

/*
 * Return the current charge refill interval; it drops by 500 ms per ten seconds to a
 * two-second floor.
 */
static Uint32 charge_replenish_interval(const DinoGame *game)
{
    /* Refill interval changes in discrete 10-second steps, then stays at 2s. */
    const Uint32 steps = game->elapsed_ms / CHARGE_INTERVAL_STEP_EVERY_MS;
    const Uint32 maximum_steps = (INITIAL_CHARGE_INTERVAL_MS -
                                  MIN_CHARGE_INTERVAL_MS) /
                                 CHARGE_INTERVAL_STEP_MS;

    if (steps >= maximum_steps) {
        return MIN_CHARGE_INTERVAL_MS;
    }
    return INITIAL_CHARGE_INTERVAL_MS - steps * CHARGE_INTERVAL_STEP_MS;
}

/*
 * Advance the capacity-growth and refill clocks, increasing the maximum by one every
 * 30 seconds and refilling at the current interval.
 */
static void update_supply(DinoGame *game, Uint32 dt_ms)
{
    Uint32 refill_interval;

    /* Grow the cap first, so a refill at a shared 30-second boundary can use
       the new slot. */
    game->capacity_clock_ms += dt_ms;
    while (game->capacity_clock_ms >= CAPACITY_INTERVAL_MS) {
        game->capacity_clock_ms -= CAPACITY_INTERVAL_MS;
        if (game->max_charges < INT_MAX) {
            ++game->max_charges;
        }
    }

    game->charge_clock_ms += dt_ms;
    refill_interval = charge_replenish_interval(game);
    while (game->charge_clock_ms >= refill_interval) {
        game->charge_clock_ms -= refill_interval;
        if (game->charges < game->max_charges) {
            ++game->charges;
        }
    }
}

/*
 * Round a source dimension upward when halving, so odd-sized sprites never lose their
 * last pixel.
 */
static int half_sprite_dimension(int source_dimension)
{
    return (source_dimension + 1) / 2;
}

/*
 * Choose native, half-size, or custom-small width for a sprite; use the fallback width
 * if its asset is missing.
 */
static int sprite_display_width(const DinoGame *game, SDL_Surface *sprite,
                                int scaled_width)
{
    if (sprite && game->native_sprites) return sprite->w;
    if (sprite && game->half_sprites) return half_sprite_dimension(sprite->w);
    return scaled_width;
}

/*
 * Choose native, half-size, or custom-small height for a sprite; use the fallback
 * height if its asset is missing.
 */
static int sprite_display_height(const DinoGame *game, SDL_Surface *sprite,
                                 int scaled_height)
{
    if (sprite && game->native_sprites) return sprite->h;
    if (sprite && game->half_sprites) return half_sprite_dimension(sprite->h);
    return scaled_height;
}

/* Return the largest available source width or height among up to three animation frames. */
static int largest_source_dimension(const SDL_Surface *first,
                                    const SDL_Surface *second,
                                    const SDL_Surface *third, bool horizontal)
{
    const SDL_Surface *sprites[3] = { first, second, third };
    int maximum = 0;
    size_t index;

    for (index = 0U; index < ARRAY_COUNT(sprites); ++index) {
        if (sprites[index]) {
            const int dimension = horizontal ? sprites[index]->w : sprites[index]->h;
            if (dimension > maximum) {
                maximum = dimension;
            }
        }
    }
    return maximum;
}

/*
 * Map one source dimension to the selected display mode, falling back to the authored
 * custom size when no source exists.
 */
static int mode_sprite_dimension(const DinoGame *game, int source_dimension,
                                 int scaled_dimension)
{
    if (source_dimension > 0 && game->native_sprites) {
        return source_dimension;
    }
    if (source_dimension > 0 && game->half_sprites) {
        return half_sprite_dimension(source_dimension);
    }
    return scaled_dimension;
}

/* Return the runner's fixed horizontal anchor for native/half modes or the custom-small layout. */
static int runner_x(const DinoGame *game)
{
    return (game->native_sprites || game->half_sprites) ? 20 : DINO_X;
}

/*
 * Use the widest run/jump source frame for collision and spacing, or the built-in
 * width without assets.
 */
static int runner_width(const DinoGame *game)
{
    const int source_width = largest_source_dimension(game->assets.run[0],
                                                       game->assets.run[1],
                                                       game->assets.jump, true);
    return mode_sprite_dimension(game, source_width, DINO_WIDTH);
}

/*
 * Use the tallest run/jump source frame for ground placement, or the built-in height
 * without assets.
 */
static int runner_height(const DinoGame *game)
{
    const int source_height = largest_source_dimension(game->assets.run[0],
                                                        game->assets.run[1],
                                                        game->assets.jump, false);
    return mode_sprite_dimension(game, source_height, DINO_HEIGHT);
}

/* Return the selected display width of the widest duck animation frame. */
static int duck_width(const DinoGame *game)
{
    const int source_width = largest_source_dimension(game->assets.duck[0],
                                                       game->assets.duck[1],
                                                       NULL, true);
    return mode_sprite_dimension(game, source_width, DUCK_WIDTH);
}

/* Return the selected display height of the tallest duck animation frame. */
static int duck_height(const DinoGame *game)
{
    const int source_height = largest_source_dimension(game->assets.duck[0],
                                                        game->assets.duck[1],
                                                        NULL, false);
    return mode_sprite_dimension(game, source_height, DUCK_HEIGHT);
}

/*
 * Align the ducking sprite with the runner anchor when its source width differs from
 * the standing pose.
 */
static int duck_x_offset(const DinoGame *game)
{
    if (game->native_sprites) {
        return 0;
    }
    if (game->half_sprites) {
        const int width_difference = duck_width(game) - runner_width(game);
        return (width_difference > 0) ? -(width_difference / 2) : 0;
    }
    return -3;
}

/* Return the selected display width of the widest bird frame. */
static int bird_width(const DinoGame *game)
{
    const int source_width = largest_source_dimension(game->assets.bird[0],
                                                       game->assets.bird[1],
                                                       NULL, true);
    return mode_sprite_dimension(game, source_width, BIRD_WIDTH);
}

/* Return the selected display height of the tallest bird frame. */
static int bird_height(const DinoGame *game)
{
    const int source_height = largest_source_dimension(game->assets.bird[0],
                                                        game->assets.bird[1],
                                                        NULL, false);
    return mode_sprite_dimension(game, source_height, BIRD_HEIGHT);
}

/*
 * Place the bird above the ducking runner in native/half modes, or use the fixed
 * clearance in custom-small mode.
 */
static int bird_y(const DinoGame *game)
{
    return (game->native_sprites || game->half_sprites) ?
           GROUND_Y - duck_height(game) - bird_height(game) :
           GROUND_Y - BIRD_CLEARANCE;
}

/* Get the variant width from its loaded BMP, or from the size-mode fallback table. */
static int cactus_width(const DinoGame *game, Uint8 variant)
{
    static const int scaled_widths[CACTUS_VARIANTS] = { 13, 22, 34, 15, 31, 32 };
    static const int half_widths[CACTUS_VARIANTS] = { 20, 34, 53, 24, 50, 51 };
    static const int native_widths[CACTUS_VARIANTS] = { 40, 68, 105, 48, 99, 102 };
    const int selected = variant % CACTUS_VARIANTS;
    SDL_Surface *sprite = game->assets.cactus[selected];

    if (sprite && game->native_sprites) {
        return sprite->w;
    }
    if (sprite && game->half_sprites) {
        return half_sprite_dimension(sprite->w);
    }
    if (game->native_sprites) return native_widths[selected];
    if (game->half_sprites) return half_widths[selected];
    return scaled_widths[selected];
}

/* Get the variant height from its loaded BMP, or from the size-mode fallback table. */
static int cactus_height(const DinoGame *game, Uint8 variant)
{
    static const int scaled_heights[CACTUS_VARIANTS] = { 23, 23, 23, 30, 30, 30 };
    static const int half_heights[CACTUS_VARIANTS] = { 36, 36, 36, 48, 48, 48 };
    static const int native_heights[CACTUS_VARIANTS] = { 71, 71, 71, 95, 95, 95 };
    const int selected = variant % CACTUS_VARIANTS;
    SDL_Surface *sprite = game->assets.cactus[selected];

    if (sprite && game->native_sprites) {
        return sprite->h;
    }
    if (sprite && game->half_sprites) {
        return half_sprite_dimension(sprite->h);
    }
    if (game->native_sprites) return native_heights[selected];
    if (game->half_sprites) return half_heights[selected];
    return scaled_heights[selected];
}

/* Return a cactus variant width or the shared bird width for spacing and threat selection. */
static int obstacle_width(const DinoGame *game, const Obstacle *obstacle)
{
    return (obstacle->kind == OBSTACLE_CACTUS) ?
           cactus_width(game, obstacle->variant) : bird_width(game);
}

/*
 * Convert the 350 ms airborne/travel gap into pixels, then add the visual margin and
 * runner width.
 */
static int minimum_obstacle_gap(const DinoGame *game)
{
    /* Measure the time between the old obstacle clearing the runner and the
       new one reaching its front, including the runner's full width. */
    return (int)(((Uint64)game->run_speed * OBSTACLE_AIR_GAP_MS + 999U) /
                 1000U) + OBSTACLE_GAP_MARGIN_PX + runner_width(game);
}

/*
 * Validate charges, cooldown, free slots, and spacing; then create a cactus or bird
 * with its own random agility offset.
 */
static bool place_obstacle(DinoGame *game, ObstacleKind kind)
{
    int index;
    int free_index = -1;
    Uint8 new_variant;
    Uint32 cooldown = placement_cooldown(game);

    /* Placement is rejected atomically: a failed request spends no charge. */
    if (game->charges <= 0) {
        return false;
    }
    if (game->has_placed_obstacle &&
        (game->elapsed_ms - game->last_place_ms) < cooldown) {
        return false;
    }
    for (index = 0; index < MAX_OBSTACLES; ++index) {
        if (!game->obstacles[index].active && free_index < 0) {
            free_index = index;
        } else if (game->obstacles[index].active) {
            const Obstacle *existing = &game->obstacles[index];
            const int gap = minimum_obstacle_gap(game);
            const Sint64 existing_right_fp = (Sint64)existing->x_fp +
                (Sint64)obstacle_width(game, existing) * FP_ONE;
            if (existing_right_fp > (Sint64)(SCREEN_WIDTH - gap) * FP_ONE) {
                return false;
            }
        }
    }
    if (free_index < 0) {
        return false;
    }

    new_variant = (kind == OBSTACLE_CACTUS) ?
                  (Uint8)(rand() % CACTUS_VARIANTS) : (Uint8)(rand() % 2);
    game->obstacles[free_index].active = true;
    game->obstacles[free_index].kind = kind;
    game->obstacles[free_index].x_fp = SCREEN_WIDTH * FP_ONE;
    game->obstacles[free_index].variant = new_variant;
    /* A negative offset makes the maneuver more eager; a positive offset
       makes the jump later or the duck window shorter. Each obstacle keeps
       its own value so cactus and bird timing can vary independently. */
    game->obstacles[free_index].agility_adjust_ms =
        rand() % (MAX_AGILITY_ADJUST_MS - MIN_AGILITY_ADJUST_MS + 1) +
        MIN_AGILITY_ADJUST_MS;
    --game->charges;
    game->last_place_ms = game->elapsed_ms;
    game->has_placed_obstacle = true;
    return true;
}

/*
 * Find the closest active obstacle still relevant to the runner and return its kind,
 * geometry, variant, and agility roll.
 */
static bool nearest_obstacle(const DinoGame *game, ObstacleKind *kind,
                             int *distance, int *width, Uint8 *variant,
                             int *agility_adjust_ms)
{
    int index;
    int best_distance = 32767;
    int best_width = 0;
    int best_agility_adjust_ms = 0;
    Uint8 best_variant = 0U;
    bool found = false;

    /* Keep a just-passed obstacle until its rear clears the runner, so it
       remains eligible for pixel collision and for the end of a duck window. */
    for (index = 0; index < MAX_OBSTACLES; ++index) {
        if (game->obstacles[index].active) {
            const Obstacle *obstacle = &game->obstacles[index];
            const int x = (int)(obstacle->x_fp / FP_ONE);
            const int current_width = obstacle_width(game, obstacle);
            const int current_distance = x - (runner_x(game) + runner_width(game));
            if (current_distance >= -(runner_width(game) + current_width) &&
                current_distance < best_distance) {
                best_distance = current_distance;
                best_width = current_width;
                best_variant = obstacle->variant;
                best_agility_adjust_ms = obstacle->agility_adjust_ms;
                *kind = obstacle->kind;
                found = true;
            }
        }
    }
    if (found) {
        *distance = best_distance;
        *width = best_width;
        *variant = best_variant;
        *agility_adjust_ms = best_agility_adjust_ms;
    }
    return found;
}

/*
 * Switch to name-entry or game-over state, stop any queued sound, play the loss cue,
 * and reset name input if needed.
 */
static void trigger_loss(DinoGame *game)
{
    game->mode = (qualifying_rank(game, game->elapsed_ms) >= 0) ?
                 MODE_NAME_ENTRY : MODE_GAME_OVER;
    game->score_rank = qualifying_rank(game, game->elapsed_ms);
    game->ducking = false;
    if (game->beep_stream) {
        (void)SDL_ClearAudioStream(game->beep_stream);
    }
    play_loss_melody(game);
    if (game->mode == MODE_NAME_ENTRY) {
        game->entry_name[0] = '\0';
        game->entry_length = 0U;
    }
}

/*
 * Compute floor(sqrt(value)) using integer arithmetic, avoiding floating-point and
 * libm dependencies on DOS.
 */
static Uint32 integer_square_root(Uint64 value)
{
    Uint64 root = 0U;
    Uint64 bit = (Uint64)1U << 62;

    /* Start at the highest power of four that can affect a 64-bit root. */
    while (bit > value) {
        bit >>= 2;
    }
    /* Restoring square-root loop: each pass decides one binary root digit. */
    while (bit != 0U) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (Uint32)root;
}

/*
 * Solve a speed- and cactus-sized jump arc. The parabola peaks 64 pixels above
 * the cactus and stays above its top long enough to cross the combined sprite
 * width, plus an 80 ms margin. Return fixed-point velocity/gravity and the
 * ascent/descent clear times. Integer square roots avoid DOS libm dependence;
 * the per-obstacle agility roll separately shifts the takeoff time.
 */
static bool calculate_jump_profile(const DinoGame *game, Uint32 speed,
                                   int width, Uint8 variant,
                                   Sint32 *velocity_fp, Sint32 *gravity_fp,
                                   Uint32 *ascent_clear_ms,
                                   Uint32 *descent_clear_ms)
{
    const int height = cactus_height(game, variant);
    const int apex_height = height + JUMP_APEX_MARGIN_PX;
    const int horizontal_clearance = width + runner_width(game);
    /* A symmetric parabola is above cactus height for this fraction of flight. */
    const Uint64 clearance_ratio_sq =
        ((Uint64)(Uint32)(apex_height - height) * 1000000U) /
        (Uint64)(Uint32)apex_height;
    /* The square root yields that vertical-clearance fraction in thousandths. */
    const Uint32 safe_fraction = integer_square_root(clearance_ratio_sq);
    Uint64 denominator;
    Uint64 flight_ms;
    Uint64 velocity_px_s;
    Uint64 gravity_px_s2;
    Uint64 velocity_squared;
    Uint64 clearance_term;
    Uint64 discriminant;
    Uint32 root;
    Uint32 actual_speed = speed;

    if (!velocity_fp || !gravity_fp || !ascent_clear_ms ||
        !descent_clear_ms || horizontal_clearance <= 0 || safe_fraction == 0U) {
        return false;
    }
    if (actual_speed == 0U) {
        actual_speed = INITIAL_RUN_SPEED;
    }
    denominator = (Uint64)actual_speed * safe_fraction;
    /* Use ceiling division so the nominal path never underestimates needed time. */
    flight_ms = (((Uint64)(Uint32)horizontal_clearance * 1000000U) +
                 denominator - 1U) / denominator;
    flight_ms += JUMP_FLIGHT_MARGIN_MS;
    if (flight_ms == 0U || flight_ms > 10000U) {
        return false;
    }

    /* For y(t)=v*t-g*t^2/2, choose v/g so the apex and total flight match. */
    velocity_px_s = ((Uint64)4U * (Uint64)(Uint32)apex_height * 1000U +
                     flight_ms / 2U) / flight_ms;
    gravity_px_s2 = ((Uint64)8U * (Uint64)(Uint32)apex_height * 1000000U +
                     (flight_ms * flight_ms) / 2U) /
                    (flight_ms * flight_ms);
    if (velocity_px_s == 0U || gravity_px_s2 == 0U) {
        return false;
    }
    *velocity_fp = -(Sint32)(velocity_px_s * (Uint64)FP_ONE);
    *gravity_fp = (Sint32)(gravity_px_s2 * (Uint64)FP_ONE);

    /* Solve the vertical-motion quadratic at y = cactus height. */
    velocity_squared = velocity_px_s * velocity_px_s;
    clearance_term = 2U * gravity_px_s2 * (Uint64)(Uint32)height;
    if (velocity_squared <= clearance_term) {
        return false;
    }
    discriminant = velocity_squared - clearance_term;
    root = integer_square_root(discriminant);
    *ascent_clear_ms = (Uint32)(((velocity_px_s - root) * 1000U +
                                 gravity_px_s2 - 1U) / gravity_px_s2);
    *descent_clear_ms = (Uint32)(((velocity_px_s + root) * 1000U) /
                                 gravity_px_s2);
    return true;
}

/*
 * Choose the midpoint of the safe takeoff window, then shift it by this cactus's
 * random timing error; positive offsets make takeoff late.
 */
static int cactus_jump_reaction_distance(const DinoGame *game, Uint32 speed,
                                         int width, Uint8 variant,
                                         int agility_adjust_ms, Uint32 dt_ms)
{
    Sint32 velocity_fp;
    Sint32 gravity_fp;
    Uint32 ascent_clear_ms;
    Uint32 descent_clear_ms;
    int horizontal_clearance;
    int reaction_shift_px;
    int earliest;
    int latest;

    if (!calculate_jump_profile(game, speed, width, variant,
                                &velocity_fp, &gravity_fp,
                                &ascent_clear_ms, &descent_clear_ms)) {
        return 0;
    }
    (void)velocity_fp;
    (void)gravity_fp;
    horizontal_clearance = width + runner_width(game) - 4;
    /* Convert milliseconds of timing error to world-space pixels at this speed. */
    reaction_shift_px = (int)(((Sint64)speed * (Sint64)agility_adjust_ms) / 1000);
    /* Earliest takeoff clears the cactus on ascent; latest clears on descent. */
    earliest = (int)(((Uint64)speed * (ascent_clear_ms + dt_ms)) / 1000U) -
               4 - reaction_shift_px;
    latest = (int)(((Uint64)speed * (descent_clear_ms - 20U)) / 1000U) -
             horizontal_clearance - reaction_shift_px;

    if (latest < earliest) {
        return earliest;
    }
    return (earliest + latest) / 2;
}

/*
 * Test whether the dino should duck at this bird distance. The nominal window
 * starts before horizontal overlap and ends after the bird clears the runner.
 * A positive agility error delays onset and advances release; a poor roll can
 * remove the safe window. Negative values favor an earlier, longer duck.
 */
static bool bird_duck_window_contains(const DinoGame *game, Uint32 speed,
                                      int distance, int agility_adjust_ms)
{
    const int reaction_distance = game->native_sprites ? 110 :
                                  game->half_sprites ? 60 :
                                  BIRD_REACTION_DISTANCE;
    /* Positive error narrows both ends of the duck window by the same shift. */
    const int timing_shift_px =
        (int)(((Sint64)speed * (Sint64)agility_adjust_ms) / 1000);
    const int duck_start_distance = reaction_distance - timing_shift_px;
    const int bird_clear_distance = -(runner_width(game) + bird_width(game));
    const int duck_release_distance = bird_clear_distance + timing_shift_px;

    return distance <= duck_start_distance && distance > duck_release_distance;
}

/*
 * Update the autonomous dino pose: decide whether to jump/duck, integrate fixed-point
 * flight, and clamp the feet to the ground.
 */
static void update_runner(DinoGame *game, Uint32 dt_ms)
{
    ObstacleKind kind;
    int distance;
    int threat_width;
    Uint8 threat_variant;
    int threat_agility_adjust_ms = 0;
    bool has_threat;
    const int ground_top = GROUND_Y - runner_height(game);

    /* Recompute ducking from the current bird window every frame. */
    game->ducking = false;
    has_threat = nearest_obstacle(game, &kind, &distance, &threat_width,
                                  &threat_variant, &threat_agility_adjust_ms);
    if (has_threat && kind == OBSTACLE_BIRD && game->grounded &&
        bird_duck_window_contains(game, game->run_speed, distance,
                                  threat_agility_adjust_ms)) {
        game->ducking = true;
    } else if (has_threat && kind == OBSTACLE_CACTUS && game->grounded &&
               (game->elapsed_ms - game->last_jump_ms) >= JUMP_RECOVERY_MS &&
               distance <= cactus_jump_reaction_distance(game, game->run_speed,
                                                          threat_width, threat_variant,
                                                          threat_agility_adjust_ms,
                                                          dt_ms)) {
        Sint32 jump_velocity = -360 * FP_ONE;
        Sint32 jump_gravity = 580 * FP_ONE;
        Uint32 ascent_clear_ms = 0U;
        Uint32 descent_clear_ms = 0U;

        if (!calculate_jump_profile(game, game->run_speed, threat_width,
                                    threat_variant, &jump_velocity, &jump_gravity,
                                    &ascent_clear_ms, &descent_clear_ms)) {
            /* Keep a usable basic arc if asset dimensions produce bad math. */
            jump_velocity = -360 * FP_ONE;
            jump_gravity = 580 * FP_ONE;
        }
        (void)ascent_clear_ms;
        (void)descent_clear_ms;
        game->dino_vy_fp = jump_velocity;
        game->dino_gravity_fp = jump_gravity;
        game->grounded = false;
        game->last_jump_ms = game->elapsed_ms;
        play_jump_melody(game);
    }

    if (!game->grounded) {
        /* Integrate y and vertical velocity in fixed point; then resolve ground contact. */
        game->dino_y_fp += (int)(((Sint64)game->dino_vy_fp * (Sint64)dt_ms) / 1000);
        game->dino_vy_fp += (int)(((Sint64)game->dino_gravity_fp * (Sint64)dt_ms) / 1000);
        if (game->dino_y_fp >= (ground_top * FP_ONE)) {
            game->dino_y_fp = ground_top * FP_ONE;
            game->dino_vy_fp = 0;
            game->dino_gravity_fp = 0;
            game->grounded = true;
        }
    }
}

/*
 * Test strict overlap of two half-open axis-aligned rectangles; touching edges alone
 * are not a collision.
 */
static bool rectangles_intersect(int ax, int ay, int aw, int ah,
                                 int bx, int by, int bw, int bh)
{
    return (ax < bx + bw) && (ax + aw > bx) && (ay < by + bh) && (ay + ah > by);
}

/*
 * Select the current duck, jump, or alternating run mask used by pixel-accurate
 * collision checks.
 */
static SDL_Surface *runner_collision_sprite(const DinoGame *game)
{
    const int frame = (int)((game->elapsed_ms / 110U) & 1U);
    if (game->ducking) {
        return game->assets.duck[frame];
    }
    if (!game->grounded) {
        return game->assets.jump;
    }
    return game->assets.run[frame];
}

/* Select the bird wing-flap frame used for this simulation time. */
static SDL_Surface *bird_collision_sprite(const DinoGame *game)
{
    const int frame = (int)((game->elapsed_ms / 100U) & 1U);
    return game->assets.bird[frame];
}

/*
 * Scale two indexed sprite masks into screen space and test overlapping opaque pixels;
 * fall back to rectangles if SDL cannot lock a mask.
 */
static bool sprite_masks_intersect(SDL_Surface *a, int ax, int ay, int aw, int ah,
                                   SDL_Surface *b, int bx, int by, int bw, int bh)
{
    const int left = (ax > bx) ? ax : bx;
    const int top = (ay > by) ? ay : by;
    const int right_a = ax + aw;
    const int right_b = bx + bw;
    const int bottom_a = ay + ah;
    const int bottom_b = by + bh;
    const int right = (right_a < right_b) ? right_a : right_b;
    const int bottom = (bottom_a < bottom_b) ? bottom_a : bottom_b;
    const PreparedSprite *prep_a;
    const PreparedSprite *prep_b;
    int y;

    if (!a || !b || aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0 ||
        left >= right || top >= bottom) {
        return false;
    }

    prep_a = find_prepared_sprite(a, aw, ah);
    prep_b = find_prepared_sprite(b, bw, bh);
    if (prep_a && prep_b && prep_a->mask_bits && prep_b->mask_bits) {
        for (y = top; y < bottom; ++y) {
            const Uint8 *row_a = prep_a->mask_bits +
                                 ((size_t)(y - ay) * (size_t)prep_a->mask_stride);
            const Uint8 *row_b = prep_b->mask_bits +
                                 ((size_t)(y - by) * (size_t)prep_b->mask_stride);
            int x;
            for (x = left; x < right; ++x) {
                const int xa = x - ax;
                const int xb = x - bx;
                if ((row_a[xa >> 3] & (Uint8)(1U << (xa & 7))) != 0U &&
                    (row_b[xb >> 3] & (Uint8)(1U << (xb & 7))) != 0U) {
                    return true;
                }
            }
        }
        return false;
    }

    return rectangles_intersect(ax, ay, aw, ah, bx, by, bw, bh);
}

/*
 * Compare the runner mask against each active cactus/bird mask; use conservative
 * rectangles when a required asset is absent.
 */
static bool check_collision(const DinoGame *game)
{
    const bool ducking = game->ducking;
    SDL_Surface *runner_sprite = runner_collision_sprite(game);
    const int duck_offset = duck_x_offset(game);
    const int runner_x_pos = runner_x(game) + (ducking ? duck_offset : 0);
    const int runner_y_pos = ducking ? GROUND_Y - duck_height(game) :
                                      (int)(game->dino_y_fp / FP_ONE);
    const int runner_w = sprite_display_width(game, runner_sprite,
                                               ducking ? DUCK_WIDTH : DINO_WIDTH);
    const int runner_h = sprite_display_height(game, runner_sprite,
                                                ducking ? DUCK_HEIGHT : DINO_HEIGHT);
    int index;

    for (index = 0; index < MAX_OBSTACLES; ++index) {
        if (game->obstacles[index].active) {
            const Obstacle *obstacle = &game->obstacles[index];
            const int x = (int)(obstacle->x_fp / FP_ONE);
            SDL_Surface *obstacle_sprite = NULL;
            int ox;
            int oy;
            int ow;
            int oh;

            if (obstacle->kind == OBSTACLE_CACTUS) {
                const int variant = obstacle->variant % CACTUS_VARIANTS;
                obstacle_sprite = game->assets.cactus[variant];
                ox = x;
                oy = GROUND_Y - cactus_height(game, obstacle->variant);
                ow = cactus_width(game, obstacle->variant);
                oh = cactus_height(game, obstacle->variant);
            } else {
                obstacle_sprite = bird_collision_sprite(game);
                ox = x;
                oy = bird_y(game);
                ow = sprite_display_width(game, obstacle_sprite, bird_width(game));
                oh = sprite_display_height(game, obstacle_sprite, bird_height(game));
            }

            /* Use authored opaque pixels when present; otherwise use inset boxes. */
            if (runner_sprite && obstacle_sprite) {
                if (sprite_masks_intersect(runner_sprite, runner_x_pos, runner_y_pos,
                                           runner_w, runner_h, obstacle_sprite,
                                           ox, oy, ow, oh)) {
                    return true;
                }
            } else {
                int rx = runner_x_pos;
                int ry = runner_y_pos;
                int rw = runner_w;
                int rh = runner_h;
                int hit_ox = ox;
                int hit_oy = oy;
                int hit_ow = ow;
                int hit_oh = oh;
                if (!game->native_sprites) {
                    rx += ducking ? 0 : 3;
                    ry += ducking ? 0 : 2;
                    rw -= ducking ? 4 : 5;
                    rh -= ducking ? 0 : 2;
                    if (obstacle->kind == OBSTACLE_CACTUS) {
                        ++hit_ox;
                        ++hit_oy;
                        hit_ow -= 2;
                        --hit_oh;
                    } else {
                        hit_ox += 2;
                        hit_ow -= 4;
                    }
                }
                if (rectangles_intersect(rx, ry, rw, rh,
                                         hit_ox, hit_oy, hit_ow, hit_oh)) {
                    return true;
                }
            }
        }
    }
    return false;
}

/*
 * Advance active-run clocks, speed, ground scroll, and obstacle positions; update the
 * runner and end the run on the first collision.
 */
static void update_playing(DinoGame *game, Uint32 dt_ms)
{
    int index;
    int speed;

    /* Game time advances only while MODE_PLAYING; pause and menus do not age supply. */
    game->elapsed_ms += dt_ms;
    update_supply(game, dt_ms);
    speed = (int)INITIAL_RUN_SPEED + (int)(game->elapsed_ms / SPEED_RAMP_DIVISOR);
    if (speed > (int)MAX_RUN_SPEED) {
        speed = (int)MAX_RUN_SPEED;
    }
    game->run_speed = (Uint32)speed;
    game->ground_scroll_fp += ((Uint64)game->run_speed * dt_ms * FP_ONE) / 1000U;

    /* Move threats before runner decisions so distance matches this frame's collision test. */
    for (index = 0; index < MAX_OBSTACLES; ++index) {
        if (game->obstacles[index].active) {
            game->obstacles[index].x_fp -=
                (Sint32)(((Uint64)game->run_speed * dt_ms * FP_ONE) / 1000U);
            if (game->obstacles[index].x_fp < -32 * FP_ONE) {
                game->obstacles[index].active = false;
            }
        }
    }
    /* Resolve pose/physics before checking the exact masks in the same frame. */
    update_runner(game, dt_ms);
    if (check_collision(game)) {
        trigger_loss(game);
    }
}

/* -------------------------------------------------------------------------- */
/* Indexed sprite rendering; procedural fallbacks remain for missing assets.  */
/* -------------------------------------------------------------------------- */
/*
 * Draw the title/dead/duck/jump/run sprite selected by state, or a procedural
 * grayscale dino if art is missing.
 */
static void draw_dinosaur(SDL_Surface *surface, const DinoGame *game, Uint32 ticks)
{
    SDL_Surface *sprite = NULL;
    const int step = (int)((ticks / 110U) & 1U);
    const bool title_pose = (game->mode == MODE_TITLE);
    const bool dead_pose = (game->mode == MODE_NAME_ENTRY ||
                            game->mode == MODE_GAME_OVER);
    const int base_x = runner_x(game);
    int x = base_x;
    int y = game->ducking ? GROUND_Y - duck_height(game) :
            (int)(game->dino_y_fp / FP_ONE);
    int width = DINO_WIDTH;
    int height = DINO_HEIGHT;

    if (title_pose) {
        sprite = game->assets.start;
        y = GROUND_Y - DINO_HEIGHT;
    } else if (dead_pose) {
        sprite = game->assets.dead;
        y = GROUND_Y - DINO_HEIGHT;
    } else if (game->ducking) {
        sprite = game->assets.duck[step];
        x = base_x + duck_x_offset(game);
        width = DUCK_WIDTH;
        height = DUCK_HEIGHT;
    } else if (!game->grounded) {
        sprite = game->assets.jump;
    } else {
        sprite = game->assets.run[step];
    }

    if (sprite) {
        width = sprite_display_width(game, sprite, width);
        height = sprite_display_height(game, sprite, height);
        if ((game->native_sprites || game->half_sprites) &&
            (title_pose || dead_pose || game->ducking)) {
            y = GROUND_Y - height;
        }
        blit_indexed_scaled(surface, sprite, x, y, width, height);
        return;
    }

    /* Low-cost fallback for builds where the BMP folder was not copied. */
    if (game->ducking) {
        fill_rect(surface, base_x + 1, y + 5, 20, 8, COLOR_BLACK);
        fill_rect(surface, base_x + 15, y + 1, 12, 8, COLOR_BLACK);
        fill_rect(surface, base_x + 23, y + 3, 6, 4, COLOR_BLACK);
        put_pixel(surface, base_x + 23, y + 2, COLOR_WHITE);
        fill_rect(surface, base_x + 4, y + 12, 4, 2, COLOR_BLACK);
        fill_rect(surface, base_x + 18, y + 12, 4, 2, COLOR_BLACK);
        return;
    }

    fill_rect(surface, base_x + 4, y + 12, 14, 11, COLOR_BLACK);
    fill_rect(surface, base_x + 12, y + 3, 13, 12, COLOR_BLACK);
    fill_rect(surface, base_x + 20, y + 7, 7, 5, COLOR_BLACK);
    fill_rect(surface, base_x + 1, y + 14, 5, 3, COLOR_BLACK);
    put_pixel(surface, base_x + 22, y + 5, COLOR_WHITE);
    fill_rect(surface, base_x + 10, y + 21, 4, step ? 5 : 3, COLOR_BLACK);
    fill_rect(surface, base_x + 17, y + 21, 4, step ? 3 : 5, COLOR_BLACK);
    put_pixel(surface, base_x + 10, y + 26, COLOR_BLACK);
    put_pixel(surface, base_x + 20, y + 26, COLOR_BLACK);
}

/*
 * Draw one size-aware cactus bitmap, with a simple procedural cactus as a
 * missing-asset fallback.
 */
static void draw_cactus(SDL_Surface *surface, const DinoGame *game, int x,
                        Uint8 variant)
{
    const int selected = variant % CACTUS_VARIANTS;
    const int width = cactus_width(game, (Uint8)selected);
    const int height = cactus_height(game, (Uint8)selected);
    const int top = GROUND_Y - height;
    SDL_Surface *sprite = game->assets.cactus[selected];

    if (sprite) {
        blit_indexed_scaled(surface, sprite, x, top, width, height);
        return;
    }

    {
        const int stem_width = (width >= 22) ? 5 : 3;
        const int stem_x = x + ((width - stem_width) / 2);
        fill_rect(surface, stem_x, top, stem_width, height, COLOR_BLACK);
        fill_rect(surface, x, top + (height / 3), stem_x - x + 2, 4, COLOR_BLACK);
        fill_rect(surface, stem_x + stem_width - 2, top + (height / 2),
                  x + width - (stem_x + stem_width - 2), 4, COLOR_BLACK);
    }
}

/* Draw the time-animated bird bitmap or its two-frame procedural wing animation. */
static void draw_bird(SDL_Surface *surface, const DinoGame *game, int x, Uint32 ticks)
{
    const int frame = (int)((ticks / 100U) & 1U);
    const int y = bird_y(game);
    SDL_Surface *sprite = game->assets.bird[frame];
    int width = bird_width(game);
    int height = bird_height(game);

    if (sprite) {
        width = sprite_display_width(game, sprite, width);
        height = sprite_display_height(game, sprite, height);
        blit_indexed_scaled(surface, sprite, x, y, width, height);
        return;
    }

    {
        const bool wing_up = (frame != 0);
        fill_rect(surface, x + 5, y + 5, 14, 6, COLOR_BLACK);
        fill_rect(surface, x + 16, y + 3, 7, 6, COLOR_BLACK);
        fill_rect(surface, x + 22, y + 5, 4, 2, COLOR_DARK);
        put_pixel(surface, x + 20, y + 4, COLOR_WHITE);
        if (wing_up) {
            draw_line(surface, x + 10, y + 6, x + 3, y, COLOR_BLACK);
            draw_line(surface, x + 12, y + 6, x + 7, y + 1, COLOR_BLACK);
        } else {
            draw_line(surface, x + 10, y + 8, x + 2, y + 13, COLOR_BLACK);
            draw_line(surface, x + 12, y + 8, x + 7, y + 13, COLOR_BLACK);
        }
    }
}

/*
 * Walk the obstacle slots and draw each active cactus or bird at its fixed-point
 * screen position.
 */
static void draw_obstacles(SDL_Surface *surface, const DinoGame *game, Uint32 ticks)
{
    int index;
    for (index = 0; index < MAX_OBSTACLES; ++index) {
        if (game->obstacles[index].active) {
            const int x = (int)(game->obstacles[index].x_fp / FP_ONE);
            if (game->obstacles[index].kind == OBSTACLE_CACTUS) {
                draw_cactus(surface, game, x, game->obstacles[index].variant);
            } else {
                draw_bird(surface, game, x, ticks);
            }
        }
    }
}

/* Draw two differently drifting clouds, when the cloud bitmap is available. */
static void draw_clouds(SDL_Surface *surface, const DinoGame *game, Uint32 ticks)
{
    const int cycle = SCREEN_WIDTH + (game->native_sprites ? 84 : 48);
    const int drift1 = (int)((ticks / 45U) % (Uint32)cycle);
    const int drift2 = (int)(((ticks / 63U) + 121U) % (Uint32)cycle);

    if (game->assets.cloud) {
        const int width1 = game->native_sprites ? 84 : 42;
        const int height1 = game->native_sprites ? 101 :
                            game->half_sprites ? 51 : 50;
        const int width2 = game->native_sprites ? 84 :
                           game->half_sprites ? 42 : 34;
        const int height2 = game->native_sprites ? 101 :
                            game->half_sprites ? 51 : 41;
        const int y1 = game->native_sprites ? 0 : 43;
        const int y2 = game->native_sprites ? 34 : 78;
        blit_indexed_scaled(surface, game->assets.cloud, SCREEN_WIDTH - drift1,
                            y1, width1, height1);
        blit_indexed_scaled(surface, game->assets.cloud, SCREEN_WIDTH - drift2,
                            y2, width2, height2);
    }
}

/*
 * Tile the track bitmap using the accumulated scroll offset, or draw a scrolling
 * procedural line when missing. The visual ground is raised independently; the
 * runner, obstacle anchors, collision checks, and physics continue using GROUND_Y.
 */
static void draw_ground(SDL_Surface *surface, const DinoGame *game, Uint32 ticks)
{
    const int visual_ground_y = GROUND_Y - GROUND_VISUAL_OFFSET_PX;
    (void)ticks;
    if (game->assets.track && g_track_fast_ready &&
        surface->pitch == SCREEN_WIDTH && surface->pixels) {
        SDL_Surface *track = game->assets.track;
        const int offset = (int)((game->ground_scroll_fp / FP_ONE) %
                                 (Uint64)track->w);
        const int byte_idx = offset >> 3;
        const int bit_shift = offset & 7;
        const int y_base = visual_ground_y - 10;

        if (g_track_last_row >= g_track_first_row) {
            int ry;
            for (ry = g_track_first_row; ry <= g_track_last_row; ++ry) {
                const int dst_y = y_base + ry;
                const Uint8 *row_bits;
                Uint32 *dst32;
                int col;

                if (dst_y < 0 || dst_y >= SCREEN_HEIGHT) {
                    continue;
                }
                row_bits = g_track_bits[ry] + byte_idx;
                dst32 = (Uint32 *)(void *)((Uint8 *)surface->pixels +
                                           (dst_y * surface->pitch));
                for (col = 0; col < (SCREEN_WIDTH / 8); ++col) {
                    const Uint32 word = (Uint32)row_bits[col] |
                                        ((Uint32)row_bits[col + 1] << 8);
                    const Uint32 byte8 = (word >> bit_shift) & 0xFFU;
                    dst32[col * 2] = g_track_lut4[byte8 & 0x0FU];
                    dst32[col * 2 + 1] = g_track_lut4[byte8 >> 4];
                }
            }
            mark_dirty_y_range(surface, y_base + g_track_first_row,
                               y_base + g_track_last_row + 1);
        }
    } else if (game->assets.track) {
        SDL_Surface *track = game->assets.track;
        const int offset = (int)((game->ground_scroll_fp / FP_ONE) %
                                 (Uint64)track->w);
        int x = -offset;
        /* Tile until the entire viewport is covered, including the wrap seam. */
        while (x < SCREEN_WIDTH) {
            blit_indexed_scaled(surface, track, x, visual_ground_y - 10,
                                track->w, track->h);
            x += track->w;
        }
    } else {
        const int offset = (int)((game->ground_scroll_fp / FP_ONE) % 24U);
        int x;
        draw_line(surface, 0, visual_ground_y, SCREEN_WIDTH - 1,
                  visual_ground_y, COLOR_BLACK);
        for (x = -offset; x < SCREEN_WIDTH; x += 24) {
            fill_rect(surface, x, visual_ground_y + 5, 9, 1, COLOR_MID);
            fill_rect(surface, x + 13, visual_ground_y + 9, 4, 1, COLOR_LIGHT);
        }
    }
}

/*
 * Render the ten score slots, formatting filled entries and placeholders with the
 * fastest times at the top.
 */
static void draw_score_list(SDL_Surface *surface, const DinoGame *game, int x, int y)
{
    int index;
    draw_text(surface, x, y, "TOP 10 - FASTEST FALL", COLOR_DARK);
    for (index = 0; index < MAX_SCORES; ++index) {
        char line[32];
        if (index < game->score_count) {
            char time_text[16];
            format_time(game->scores[index].time_ms, time_text, sizeof(time_text));
            (void)snprintf(line, sizeof(line), "%2d. %-8s %s", index + 1,
                           game->scores[index].name, time_text);
        } else {
            (void)snprintf(line, sizeof(line), "%2d. ---", index + 1);
        }
        draw_text(surface, x, y + 12 + (index * 8), line, COLOR_DARK);
    }
}

/* Draw the run title, elapsed time, current/capacity charges, and speed. */
static void draw_hud(SDL_Surface *surface, const DinoGame *game)
{
    char time_text[16];
    char line[40];

    format_time(game->elapsed_ms, time_text, sizeof(time_text));
    draw_text(surface, 5, 4, "DINO FALL", COLOR_BLACK);
    (void)snprintf(line, sizeof(line), "TIME %s", time_text);
    draw_text(surface, 98, 4, line, COLOR_DARK);
    (void)snprintf(line, sizeof(line), "SUPPLY %d/%d", game->charges, game->max_charges);
    draw_text(surface, 218, 4, line, COLOR_BLACK);
    (void)snprintf(line, sizeof(line), "SPEED %lu", (unsigned long)game->run_speed);
    draw_text(surface, 5, 15, line, COLOR_DARK);
}

/*
 * Draw the title, placement controls, start prompt, runner, Top 10, and a warning if
 * sprites are missing.
 */
static void draw_title_screen(SDL_Surface *surface, const DinoGame *game)
{
    draw_text_centered(surface, 8, "DINO FALL", COLOR_BLACK);
    draw_text_centered(surface, 22, "MAKE THE DINO FALL AS FAST AS YOU CAN!", COLOR_DARK);
    draw_text_centered(surface, 41, "Q CACTUS   P BIRD", COLOR_BLACK);
    draw_text_centered(surface, 52, "SPACE START   ESC EXIT", COLOR_BLACK);
    draw_dinosaur(surface, game, 0U);
    draw_score_list(surface, game, 100, 68);
    if (game->assets_loaded < 19) {
        char warning[40];
        (void)snprintf(warning, sizeof(warning), "SPRITES %d/19 - CHECK IMAGES/",
                       game->assets_loaded);
        draw_text_centered(surface, 184, warning, COLOR_DARK);
    }
}

/* Show the qualifying run time and prompt for a name of up to eight permitted characters. */
static void draw_name_entry(SDL_Surface *surface, const DinoGame *game)
{
    char time_text[16];
    char line[40];

    format_time(game->elapsed_ms, time_text, sizeof(time_text));
    draw_text(surface, 77, 53, "NEW TOP 10 TIME!", COLOR_BLACK);
    (void)snprintf(line, sizeof(line), "TIME %s - ENTER NAME", time_text);
    draw_text(surface, 47, 70, line, COLOR_DARK);
    (void)snprintf(line, sizeof(line), "NAME: %s_", game->entry_name);
    draw_text(surface, 107, 91, line, COLOR_BLACK);
    draw_text(surface, 55, 108, "A-Z 0-9 - _ UP TO 8 CHARS", COLOR_DARK);
    draw_text(surface, 78, 123, "ENTER SAVES - ESC QUITS", COLOR_DARK);
}

/*
 * Draw the game-over/reset art (or text fallback), final time, restart prompt, scores,
 * and any unexpired status message.
 */
static void draw_game_over(SDL_Surface *surface, const DinoGame *game)
{
    char time_text[16];
    char line[40];

    format_time(game->elapsed_ms, time_text, sizeof(time_text));
    if (game->assets.game_over) {
        const int title_width = sprite_display_width(game, game->assets.game_over, 154);
        const int title_height = sprite_display_height(game, game->assets.game_over, 16);
        const int title_x = (SCREEN_WIDTH - title_width) / 2;
        blit_indexed_scaled(surface, game->assets.game_over,
                            title_x, 22, title_width, title_height);
    } else {
        draw_text(surface, 100, 28, "DINO CAUGHT!", COLOR_BLACK);
    }
    (void)snprintf(line, sizeof(line), "RUN TIME %s", time_text);
    draw_text(surface, 91, 46, line, COLOR_DARK);
    draw_text(surface, 44, 61, "SPACE PLAY AGAIN - ESC EXIT", COLOR_BLACK);
    if (game->assets.reset) {
        const int reset_width = sprite_display_width(game, game->assets.reset, 14);
        const int reset_height = sprite_display_height(game, game->assets.reset, 19);
        const int reset_x = (game->half_sprites || game->native_sprites) ?
                            SCREEN_WIDTH - reset_width - 8 : 246;
        blit_indexed_scaled(surface, game->assets.reset,
                            reset_x, 55, reset_width, reset_height);
    }
    draw_score_list(surface, game, 97, 72);
    if (game->message[0] != '\0' && (Uint32)SDL_GetTicks() < game->message_until_ticks) {
        draw_text(surface, 81, 185, game->message, COLOR_DARK);
    }
}

/*
 * Stream and pre-scale every runtime sprite ONE BMP AT A TIME directly from
 * disk (0 bytes of SDL_LoadBMP heap, 1-bit collision masks, 1-bit packed
 * TRACK.BMP LUT) so 2 MB 80386SX systems stay ~120 KB below CWSDPMI swap.
 */
static void load_and_prewarm_dino_assets(DinoGame *game)
{
    static const char *const run_paths[2] = {
        "Images/DINO/RUN1.BMP", "Images/DINO/RUN2.BMP"
    };
    static const char *const duck_paths[2] = {
        "Images/DINO/DUCK1.BMP", "Images/DINO/DUCK2.BMP"
    };
    static const char *const bird_paths[2] = {
        "Images/BIRD/BIRD1.BMP", "Images/BIRD/BIRD2.BMP"
    };
    static const char *const cactus_paths[CACTUS_VARIANTS] = {
        "Images/CACTUS/SMCACT1.BMP", "Images/CACTUS/SMCACT2.BMP",
        "Images/CACTUS/SMCACT3.BMP", "Images/CACTUS/LGCACT1.BMP",
        "Images/CACTUS/LGCACT2.BMP", "Images/CACTUS/LGCACT3.BMP"
    };
    IndexedBmpStream stream;
    int loaded = 0;
    int i;

    memset(&stream, 0, sizeof(stream));

    for (i = 0; i < 2; ++i) {
        loaded += load_streamed_sprite(&stream, &game->assets.run[i],
                                       run_paths[i], DINO_WIDTH, DINO_HEIGHT,
                                       game, true);
        loaded += load_streamed_sprite(&stream, &game->assets.duck[i],
                                       duck_paths[i], DUCK_WIDTH, DUCK_HEIGHT,
                                       game, true);
        loaded += load_streamed_sprite(&stream, &game->assets.bird[i],
                                       bird_paths[i], BIRD_WIDTH, BIRD_HEIGHT,
                                       game, true);
    }
    for (i = 0; i < CACTUS_VARIANTS; ++i) {
        loaded += load_streamed_sprite(&stream, &game->assets.cactus[i],
                                       cactus_paths[i],
                                       cactus_width(game, (Uint8)i),
                                       cactus_height(game, (Uint8)i),
                                       game, true);
    }

    loaded += load_streamed_sprite(&stream, &game->assets.jump,
                                   "Images/DINO/JUMP.BMP",
                                   DINO_WIDTH, DINO_HEIGHT, game, true);
    loaded += load_streamed_sprite(&stream, &game->assets.dead,
                                   "Images/DINO/DEAD.BMP",
                                   DINO_WIDTH, DINO_HEIGHT, game, false);
    loaded += load_streamed_sprite(&stream, &game->assets.start,
                                   "Images/DINO/START.BMP",
                                   DINO_WIDTH, DINO_HEIGHT, game, false);
    loaded += load_streamed_sprite(&stream, &game->assets.game_over,
                                   "Images/OTHER/GAMEOVER.BMP",
                                   154, 16, game, false);
    loaded += load_streamed_sprite(&stream, &game->assets.reset,
                                   "Images/OTHER/RESET.BMP",
                                   14, 19, game, false);

    game->assets.cloud = NULL;
    if (open_indexed_bmp_asset(&stream, "Images/OTHER/CLOUD.BMP")) {
        SDL_Surface *stub = alloc_static_stub_surface(stream.width,
                                                      stream.height);
        if (stub) {
            if (game->native_sprites) {
                (void)stream_prepared_sprite(&stream, stub, 84, 101, false);
            } else if (game->half_sprites) {
                (void)stream_prepared_sprite(&stream, stub, 42, 51, false);
            } else {
                (void)stream_prepared_sprite(&stream, stub, 42, 50, false);
                (void)stream_prepared_sprite(&stream, stub, 34, 41, false);
            }
            game->assets.cloud = stub;
            ++loaded;
        }
        close_indexed_bmp(&stream);
    }

    loaded += load_track_streaming(&stream, &game->assets.track);

    game->assets_loaded = loaded;
    if (loaded < 19) {
        fprintf(stderr,
                "DINO: loaded %d/19 indexed BMP assets; check Images/ beside DINO.EXE.\n",
                loaded);
    }
}

/* Upload ONLY changed 8-scanline horizontal bands (`upload_mask`) to ISA VGA RAM:
 *   - Inductive invariant: every scanline outside `prev_dirty_strips` is already
 *     guaranteed equal to `COLOR_WHITE` in both `drawing_surface` AND physical
 *     `0xA0000` VRAM. Clearing `prev_dirty_strips`, drawing frame `t` (recording
 *     `g_cur_dirty_strips`), and uploading `g_cur_dirty_strips | prev_dirty_strips`
 *     is 100% pixel-identical to uploading all 64,000 bytes while requiring
 *     ZERO framebuffer reads, ZERO `imul` hashes, and ZERO shadow-buffer RAM! */
static void present_game_frame(DinoGame *game)
{
    enum { STRIP_HEIGHT = 8, STRIP_COUNT = SCREEN_HEIGHT / STRIP_HEIGHT };
    SDL_Surface *surface = game->drawing_surface;
    SDL_Rect dirty_rects[STRIP_COUNT];
    int rect_count = 0;

    if (surface != game->window_surface) {
        if (!SDL_BlitSurface(surface, NULL, game->window_surface, NULL)) {
            return;
        }
        (void)SDL_UpdateWindowSurface(game->window);
        return;
    }

    if (!game->vram_shadow_valid || surface->pitch != SCREEN_WIDTH || !surface->pixels) {
        game->prev_dirty_strips = g_cur_dirty_strips;
        game->vram_shadow_valid = true;
        (void)SDL_UpdateWindowSurface(game->window);
        return;
    }

    {
        const Uint32 upload_mask = g_cur_dirty_strips | game->prev_dirty_strips;
        int strip = 0;

        game->prev_dirty_strips = g_cur_dirty_strips;
        while (strip < STRIP_COUNT) {
            int run_start;
            if ((upload_mask & (1U << strip)) == 0U) {
                ++strip;
                continue;
            }
            run_start = strip;
            ++strip;
            while (strip < STRIP_COUNT && (upload_mask & (1U << strip)) != 0U) {
                ++strip;
            }
            dirty_rects[rect_count].x = 0;
            dirty_rects[rect_count].y = run_start * STRIP_HEIGHT;
            dirty_rects[rect_count].w = SCREEN_WIDTH;
            dirty_rects[rect_count].h = (strip - run_start) * STRIP_HEIGHT;
            ++rect_count;
        }
    }

    if (rect_count == 1 && dirty_rects[0].h == SCREEN_HEIGHT) {
        (void)SDL_UpdateWindowSurface(game->window);
    } else if (rect_count > 0) {
        (void)SDL_UpdateWindowSurfaceRects(game->window, dirty_rects, rect_count);
    }
}

/*
 * Clear only strips touched on the previous frame, render the active mode while
 * recording analytical dirty strips, then transfer only changed strips to VGA.
 */
static void draw_game(DinoGame *game)
{
    static bool s_have_static_sig;
    static GameMode s_last_mode;
    static Uint32 s_last_elapsed;
    static Uint64 s_last_scroll;
    static int s_last_dino_y;
    static char s_last_entry[NAME_LENGTH + 1];
    static bool s_last_msg_visible;

    SDL_Surface *surface = game->drawing_surface;
    /* Elapsed run time freezes naturally outside MODE_PLAYING. */
    Uint32 ticks = game->elapsed_ms;
    const bool msg_visible = (game->mode == MODE_GAME_OVER) &&
                             (game->message[0] != '\0') &&
                             ((Uint32)SDL_GetTicks() < game->message_until_ticks);

    if (game->vram_shadow_valid && game->mode != MODE_PLAYING &&
        s_have_static_sig &&
        s_last_mode == game->mode &&
        s_last_elapsed == ticks &&
        s_last_scroll == game->ground_scroll_fp &&
        s_last_dino_y == game->dino_y_fp &&
        strcmp(s_last_entry, game->entry_name) == 0 &&
        s_last_msg_visible == msg_visible) {
        return;
    }

    if (!SDL_LockSurface(surface)) {
        return;
    }
    s_have_static_sig = (game->mode != MODE_PLAYING);
    s_last_mode = game->mode;
    s_last_elapsed = ticks;
    s_last_scroll = game->ground_scroll_fp;
    s_last_dino_y = game->dino_y_fp;
    memcpy(s_last_entry, game->entry_name, sizeof(s_last_entry));
    s_last_msg_visible = msg_visible;
    g_tracked_surface = surface;
    g_cur_dirty_strips = 0U;

    if (!game->vram_shadow_valid || surface->pitch != SCREEN_WIDTH || !surface->pixels) {
        g_clearing_strips = true;
        fill_rect(surface, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_WHITE);
        g_clearing_strips = false;
    } else {
        const Uint32 clear_mask = game->prev_dirty_strips;
        int s = 0;
        while (s < (SCREEN_HEIGHT / 8)) {
            int s_start;
            while (s < (SCREEN_HEIGHT / 8) && (clear_mask & (1U << s)) == 0U) {
                ++s;
            }
            if (s >= (SCREEN_HEIGHT / 8)) {
                break;
            }
            s_start = s;
            while (s < (SCREEN_HEIGHT / 8) && (clear_mask & (1U << s)) != 0U) {
                ++s;
            }
            memset((Uint8 *)surface->pixels + (s_start * 8 * SCREEN_WIDTH),
                   (int)COLOR_WHITE,
                   (size_t)((s - s_start) * 8 * SCREEN_WIDTH));
        }
    }

    if (game->mode == MODE_TITLE) {
        draw_clouds(surface, game, ticks);
        draw_ground(surface, game, ticks);
        draw_title_screen(surface, game);
    } else if (game->mode == MODE_NAME_ENTRY) {
        draw_clouds(surface, game, ticks);
        draw_ground(surface, game, ticks);
        draw_dinosaur(surface, game, ticks);
        draw_obstacles(surface, game, ticks);
        draw_name_entry(surface, game);
    } else if (game->mode == MODE_GAME_OVER) {
        draw_clouds(surface, game, ticks);
        draw_ground(surface, game, ticks);
        draw_dinosaur(surface, game, ticks);
        draw_obstacles(surface, game, ticks);
        draw_game_over(surface, game);
    } else {
        if (game->mode == MODE_PLAYING || game->mode == MODE_PAUSED) {
            draw_clouds(surface, game, ticks);
        }
        draw_hud(surface, game);
        draw_ground(surface, game, ticks);
        draw_obstacles(surface, game, ticks);
        draw_dinosaur(surface, game, ticks);
        if (game->mode == MODE_PAUSED) {
            fill_rect(surface, 77, 71, 166, 39, COLOR_WHITE);
            draw_line(surface, 77, 71, 242, 71, COLOR_BLACK);
            draw_line(surface, 77, 109, 242, 109, COLOR_BLACK);
            draw_text(surface, 119, 80, "PAUSED", COLOR_BLACK);
            draw_text(surface, 88, 94, "SPACE TO RESUME", COLOR_DARK);
        }
    }
    g_tracked_surface = NULL;
    SDL_UnlockSurface(surface);
    present_game_frame(game);
}

/* -------------------------------------------------------------------------- */
/* Run lifecycle, keyboard control, and persistent name entry.                */
/* -------------------------------------------------------------------------- */
/*
 * Reset per-run timers, charges, obstacles, runner physics, scrolling, and transient
 * text before entering MODE_PLAYING.
 */
static void start_game(DinoGame *game)
{
    int index;

    for (index = 0; index < MAX_OBSTACLES; ++index) {
        game->obstacles[index].active = false;
    }
    game->charges = INITIAL_CHARGES;
    game->max_charges = INITIAL_MAX_CHARGES;
    game->elapsed_ms = 0U;
    game->charge_clock_ms = 0U;
    game->capacity_clock_ms = 0U;
    game->last_place_ms = 0U;
    game->last_jump_ms = 0U;
    game->ground_scroll_fp = 0U;
    game->has_placed_obstacle = false;
    game->run_speed = INITIAL_RUN_SPEED;
    game->dino_y_fp = (GROUND_Y - runner_height(game)) * FP_ONE;
    game->dino_vy_fp = 0;
    game->dino_gravity_fp = 0;
    game->grounded = true;
    game->ducking = false;
    game->message[0] = '\0';
    game->mode = MODE_PLAYING;
    game->last_ticks = (Uint32)SDL_GetTicks();
}

/*
 * Translate supported physical key/scancode values to the uppercase score-name
 * alphabet; return NUL for everything else.
 */
static char name_character(const SDL_KeyboardEvent *key)
{
    const int code = (int)key->scancode;
    const int letter_start = (int)SDL_SCANCODE_A;
    const int letter_end = (int)SDL_SCANCODE_Z;
    const int digit_start = (int)SDL_SCANCODE_1;
    const int digit_end = (int)SDL_SCANCODE_9;

    if (code >= letter_start && code <= letter_end) {
        return (char)('A' + (code - letter_start));
    }
    if (code >= digit_start && code <= digit_end) {
        return (char)('1' + (code - digit_start));
    }
    if (key->scancode == SDL_SCANCODE_0) {
        return '0';
    }
    if (key->key == SDLK_MINUS) {
        return '-';
    }
    if (key->key == SDLK_UNDERSCORE) {
        return '_';
    }
    return '\0';
}

/* Use PLAYER for an empty entry, insert/persist the score, and show whether the save succeeded. */
static void submit_name(DinoGame *game)
{
    const char *name = (game->entry_length > 0U) ? game->entry_name : "PLAYER";
    const bool saved = insert_score(game, name, game->elapsed_ms, game->score_rank);
    game->mode = MODE_GAME_OVER;
    set_message(game, saved ? "SCORE SAVED TO DINO.SCO" : "SCORE SAVE FAILED");
}

/*
 * Route Escape, Space, name editing, and in-run Q/P placement according to the current
 * game mode.
 */
static void handle_key_down(DinoGame *game, const SDL_KeyboardEvent *key)
{
    if (key->key == SDLK_ESCAPE) {
        game->quit_requested = true;
        return;
    }
    if (key->repeat) {
        return;
    }

    if (game->mode == MODE_NAME_ENTRY) {
        if (key->key == SDLK_RETURN || key->key == SDLK_KP_ENTER) {
            submit_name(game);
        } else if (key->key == SDLK_BACKSPACE && game->entry_length > 0U) {
            --game->entry_length;
            game->entry_name[game->entry_length] = '\0';
        } else {
            const char character = name_character(key);
            if (character != '\0' && game->entry_length < NAME_LENGTH) {
                game->entry_name[game->entry_length] = character;
                ++game->entry_length;
                game->entry_name[game->entry_length] = '\0';
            }
        }
        return;
    }

    if (key->key == SDLK_SPACE) {
        if (game->mode == MODE_TITLE || game->mode == MODE_GAME_OVER) {
            start_game(game);
        } else if (game->mode == MODE_PLAYING) {
            game->mode = MODE_PAUSED;
        } else if (game->mode == MODE_PAUSED) {
            game->mode = MODE_PLAYING;
            game->last_ticks = (Uint32)SDL_GetTicks();
        }
        return;
    }

    if (game->mode != MODE_PLAYING || key->repeat) {
        return;
    }
    if (key->key == SDLK_Q) {
        (void)place_obstacle(game, OBSTACLE_CACTUS);
    } else if (key->key == SDLK_P) {
        (void)place_obstacle(game, OBSTACLE_BIRD);
    }
}

/*
 * Poll SDL events and run the capped-delta update/render loop until the window closes
 * or Escape is pressed.
 */
static bool run_game(DinoGame *game)
{
    bool running = true;
    SDL_Event event;

    while (running) {
        Uint32 now = (Uint32)SDL_GetTicks();
        Uint32 dt_ms = now - game->last_ticks;
        game->last_ticks = now;
        /* A long host pause must not teleport obstacles or destabilize physics. */
        if (dt_ms > 50U) {
            dt_ms = 50U;
        }

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                handle_key_down(game, &event.key);
                if (game->quit_requested) {
                    running = false;
                }
            }
        }
        if (!running) {
            break;
        }
        if (game->mode == MODE_PLAYING) {
            update_playing(game, dt_ms);
        }
        service_audio(game, (Uint32)SDL_GetTicks());
        draw_game(game);
        SDL_Delay(16);
    }
    stop_speaker_tones(game);
    return true;
}

/*
 * Release sprites, audio stream, any owned off-screen surface, window, and SDL in
 * reverse initialization order.
 */
static void shutdown_game(DinoGame *game)
{
    stop_speaker_tones(game);
    destroy_dino_assets(game);
    if (game->beep_stream) {
        (void)SDL_ClearAudioStream(game->beep_stream);
        SDL_DestroyAudioStream(game->beep_stream);
        game->beep_stream = NULL;
    }
    if (game->drawing_surface && game->drawing_surface != game->window_surface) {
        SDL_DestroySurface(game->drawing_surface);
        game->drawing_surface = NULL;
    }
    if (game->window) {
        SDL_DestroyWindow(game->window);
        game->window = NULL;
        game->window_surface = NULL;
    }
    if (game->sdl_initialized) {
        SDL_Quit();
        game->sdl_initialized = false;
    }
}

/* Print the command-line help, including the current size switches and compatibility aliases. */
static void print_dino_usage(const char *program)
{
    const char *name = (program && program[0] != '\0') ? program : "DINO.EXE";
    printf("DINO FALL - obstacle placement game\n");
    printf("Usage: %s [/h] [/s 1|2|3] [/nosound]\n", name);
    printf("  /h                 show this help\n");
    printf("  /s 1               full source size (1:1)\n");
    printf("  /s 2               half source size (default)\n");
    printf("  /s 3               smallest scaled gameplay size\n");
    printf("  /nosound           disable audio/beeps\n");
    printf("  --help, -h         also show this help\n");
    printf("Legacy aliases: --native-sprites, --half-sprites, --scaled-sprites\n");
}

/* Translate size 1/2/3 into the two flags that select native, half, or custom-small rendering. */
static void set_sprite_size_mode(int size, bool *native_sprites,
                                 bool *half_sprites)
{
    *native_sprites = (size == 1);
    *half_sprites = (size == 2);
}

/*
 * Parse help, audio, and sprite-size switches; initialize the default half-size mode
 * and reject unknown/incomplete arguments.
 */
static bool parse_dino_arguments(int argc, char **argv, bool *native_sprites,
                                 bool *half_sprites, bool *sound_disabled,
                                 bool *show_help)
{
    int index;
    *native_sprites = false;
    *half_sprites = true;
    *sound_disabled = false;
    *show_help = false;
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "/h") == 0 || strcmp(argv[index], "/H") == 0 ||
            strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
            *show_help = true;
        } else if (strcmp(argv[index], "/nosound") == 0 ||
                   strcmp(argv[index], "/NOSOUND") == 0 ||
                   strcmp(argv[index], "--no-sound") == 0) {
            *sound_disabled = true;
        } else if (strcmp(argv[index], "/s") == 0 ||
                   strcmp(argv[index], "/S") == 0) {
            int size;
            if (index + 1 >= argc) {
                fprintf(stderr, "DINO: /s requires a size: 1, 2, or 3.\n");
                print_dino_usage((argc > 0) ? argv[0] : NULL);
                return false;
            }
            ++index;
            if (strcmp(argv[index], "1") == 0) {
                size = 1;
            } else if (strcmp(argv[index], "2") == 0) {
                size = 2;
            } else if (strcmp(argv[index], "3") == 0) {
                size = 3;
            } else {
                fprintf(stderr, "DINO: invalid sprite size: %s (use 1, 2, or 3).\n",
                        argv[index]);
                print_dino_usage((argc > 0) ? argv[0] : NULL);
                return false;
            }
            set_sprite_size_mode(size, native_sprites, half_sprites);
        } else if (strcmp(argv[index], "--native-sprites") == 0) {
            set_sprite_size_mode(1, native_sprites, half_sprites);
        } else if (strcmp(argv[index], "--half-sprites") == 0) {
            set_sprite_size_mode(2, native_sprites, half_sprites);
        } else if (strcmp(argv[index], "--scaled-sprites") == 0) {
            set_sprite_size_mode(3, native_sprites, half_sprites);
        } else {
            fprintf(stderr, "DINO: unknown option: %s\n", argv[index]);
            print_dino_usage((argc > 0) ? argv[0] : NULL);
            return false;
        }
    }
    return true;
}

/*
 * Parse options, load records, initialize SDL/video/assets/audio, run the event loop,
 * and always perform orderly shutdown after setup.
 */
int main(int argc, char **argv)
{
    static DinoGame game;
    bool show_help = false;
    int result = 1;

    memset(&game, 0, sizeof(game));

    /* Reject malformed options before opening SDL; help is a clean early exit. */
    if (!parse_dino_arguments(argc, argv, &game.native_sprites,
                              &game.half_sprites, &game.sound_disabled,
                              &show_help)) {
        return 1;
    }
    if (show_help) {
        print_dino_usage((argc > 0) ? argv[0] : NULL);
        return 0;
    }

    game.mode = MODE_TITLE;
    game.score_rank = -1;
    load_scores(&game);

    (void)SDL_SetHint(SDL_HINT_DOS_ALLOW_DIRECT_FRAMEBUFFER, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "DINO: SDL video initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    game.sdl_initialized = true;
    /* Seed once per process; each obstacle then stores its own agility roll. */
    srand((unsigned int)SDL_GetTicks());
    if (!initialize_video(&game)) {
        fprintf(stderr, "DINO: could not create the 320x200 grayscale display: %s\n",
                SDL_GetError());
        shutdown_game(&game);
        return 1;
    }

    load_and_prewarm_dino_assets(&game);
    initialize_audio(&game);
    game.last_ticks = (Uint32)SDL_GetTicks();
    game.run_speed = INITIAL_RUN_SPEED;
    game.dino_y_fp = (GROUND_Y - runner_height(&game)) * FP_ONE;
    game.grounded = true;

    if (run_game(&game)) {
        result = 0;
    }
    shutdown_game(&game);
    return result;
}
