/* sdl_main.c -- The Lion King playable SDL3 host.
 *
 * usage: tlk_sdl --rom <path.sfc> [options]
 *
 * Runs exactly the emulation path of tlk_headless: same sources, same host
 * contract (headless_host.c), same RtlRunFrame + beam frame driver, same
 * non-extended APU timeline (the framework desktop host turns on
 * RtlEnableExtendedFrameTiming, which bypasses TLK's NTSC SPC ratio; this
 * host never calls it). Everything runs on one thread, so RtlApuLock stays a
 * no-op and the audio device only receives what the main loop queues.
 *
 * Pacing: 60.0988 Hz NTSC (21477272 / 357366) on a wall-clock deadline, or
 * vsync=1 in the ini. Audio: RtlRenderAudio resamples the DSP's 32040 Hz
 * output onto the device rate (with its own ring servo); this host queues
 * rate/60.0988 frames per field, trimmed by up to +/-0.5% to hold the device
 * queue at ~3 fields of latency, so neither side drifts.
 *
 * Options:
 *   --rom path            ROM (required; also rom= in the ini)
 *   --config path         ini file (default tlk_sdl.ini, or $TLK_SDL_INI)
 *   --input tape          per-frame u16 LE $4218 tape (same as tlk_headless)
 *   --input-polled tape   one tape word per game poll (same as tlk_headless)
 *   --input-raw tape      replay a last_session.rec recording (pad word per frame)
 *   --no-record           do not write last_session.rec (live play records by default;
 *                         --frames runs never record unless --record is given)
 *   --record path         record to path instead (also with --input, to test replay)
 *   --lle                 SNESRECOMP_LLE_BOUNCE=0 (same as tlk_headless)
 *   --interp-targets f    SNESRECOMP_LLE_INTERP_TARGET_FILE
 *   --frames N            quit after N frames, print final hashes
 *   --hash-at a,b,...     print frame_sha256 after those frames
 *   --dump-png path       write the last frame as PNG
 *   --no-pace             run as fast as possible (proof runs)
 *   --no-audio            do not open an audio device
 *   --wav path [--wav-seconds S]  record the queued audio (default 10 s)
 *   --scale N  --fullscreen
 *   --save-at N state     write a savestate after frame N (proof runs)
 *   --load state          start from a savestate (frame count continues)
 *   --load-at N state     load a savestate after frame N, in process (proof runs)
 *   --menu-at N           open the settings menu after frame N (screenshots)
 *   --menu-page controls|level|about  with --menu-at: open the Controls (remap) or Level select section, or the About window
 *   --speedrun off|practice  practice mode on or off (overrides the ini)
 *   --practice-level N    level select N (0-13) after frame 1 (proof runs)
 *   --invul N             with --practice-level: debug-menu INVUL 0 off, 1 health, 2 lives
 *   --dump-window path    write the last presented window as PNG (with OpenGL: the
 *                         whole window at its size, shader and menu included)
 *   --selftest-remap      drive the menu/remap/hotplug code with synthetic SDL
 *                         events and a virtual gamepad, print PASS/FAIL, exit
 *   --list-drivers        print the SDL version and the video/audio drivers
 *                         compiled into this binary, exit
 *
 * Without --rom (or rom= in the ini), the .sfc/.smc files next to the binary
 * and in the current directory are verified and the first match is used. If
 * none matches (or the ini's rom is gone), an interactive run asks with the
 * system file dialog and saves the choice as rom= in the ini.
 *
 * Keys (default): arrows, Z=B X=A A=Y S=X Q=L W=R Enter=Start RShift=Select,
 * F11 or Alt+Enter fullscreen. F1-F4 save state slot 1-4, F5-F8 (or
 * Shift+F1-F4) load it; slots are tlk_slotN.state next to the ini. Hold ` (pad:
 * right stick click) for turbo: turbo_speed times faster, 4 by default, sound
 * muted; rebindable on the Controls page. Practice mode (menu, speedrun =
 * practice) adds a level timer, lag counter, input display and best times; F9
 * restarts the level. Tab or Esc
 * opens the settings menu (emulation pauses): arrows move and change values,
 * Enter selects, Esc or Backspace goes back and closes, Tab closes; System >
 * Quit (pressed twice) quits. Every change applies at once and is saved to the
 * ini. Gamepads default to by position (south=B, east=A, west=Y, north=X);
 * Guide or Back+Start opens the menu. The Controls section rebinds a key or a
 * gamepad button per SNES button (whichever is pressed next), with Reset
 * defaults; bindings go to the ini (b = Z ..., pad_b = a ...). Pads are
 * hotplugged.
 *
 * The menu is recomp-ui's runtime menu (recomp-ui/, MIT): this file gives it
 * rows and callbacks, tlk_menu_imgui.cpp draws it with Dear ImGui over the
 * OpenGL presenter (fonts from assets/fonts next to the binary). Without
 * OpenGL it draws its compact form into the 256x224 picture.
 *
 * Video goes through snesrecomp's OpenGL 3.3 presenter (desktop/opengl.c):
 * the menu's Shader row cycles the bundled presets in assets/shaders next
 * to the binary; Aspect, Smooth and Int. scale apply with or without one.
 * HD Mode 7 (Graphics) binds snesrecomp's 2-4x Mode 7 surface and shows it in
 * place of the native picture, on either presenter.
 * Without OpenGL 3.3 (or with opengl = 0) the plain SDL renderer is used and
 * shaders are unavailable. None of this touches s_pixels, which the hashes,
 * --dump-png and the gates read. */
#include "tlk_game.h"
#include "verified_rom.h"
#include "tlk_font8x8.h"
#include "tlk_savestate.h"
#include "tlk_wide.h"

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "sha256.h"
#include "snes/interp_bridge.h"
#include "snes/joypad.h"
#include "snes/ppu.h"
#include "snes/snes.h"

#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL.h>

/* snesrecomp's desktop OpenGL presenter (GLSL shader presets, aspect). */
#include "opengl.h"
#include "util.h"
#include "gl_core_3_1.h"
void snesrecomp_opengl_set_overlay(void (*draw)(int, int));   /* cmake/runtime-patches/opengl.hunks */
void OpenGLRenderer_Create(struct RendererFuncs *funcs);
void TlkVideoConfig(const char *shader, int aspect, int smooth);   /* tlk_video.c */
void TlkAssetPath(const char *rel, char *out, size_t n);           /* tlk_assets.c */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tlk_compat.h"
#include "tlk_menu_imgui.h"

enum { kWidth = 256, kHeight = 224, kBpp = 4 };
static uint8_t s_pixels[kWidth * kHeight * kBpp];
/* What is shown: the PPU frame plus the menu/toast overlay. Hashes and PNG
 * dumps read s_pixels, never this. */
static uint32_t s_screen[kWidth * kHeight];

#define NTSC_FPS (21477272.0 / 357366.0) /* 60.0988 */

/* ── PNG writer (stored deflate), same as headless_main.c ───────────────── */
static uint32_t s_crc_table[256];
static void CrcInit(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        s_crc_table[n] = c;
    }
}
static uint32_t CrcUpdate(uint32_t c, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) c = s_crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c;
}
static void Be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static int WriteChunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8], tail[4];
    Be32(hdr, len); memcpy(hdr + 4, type, 4);
    uint32_t crc = CrcUpdate(0xffffffffu, hdr + 4, 4);
    crc = CrcUpdate(crc, data, len) ^ 0xffffffffu;
    Be32(tail, crc);
    return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) &&
           fwrite(tail, 1, 4, f) == 4;
}
static int WritePng(const char *path, const uint8_t *px, int w, int h, int pitch) {
    const size_t row = (size_t)w * 3 + 1, raw_len = row * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        uint8_t *d = raw + row * (size_t)y;
        const uint8_t *s = px + (size_t)pitch * (size_t)y;
        *d++ = 0;
        for (int x = 0; x < w; x++) { d[0] = s[x * 4 + 2]; d[1] = s[x * 4 + 1]; d[2] = s[x * 4 + 0]; d += 3; }
    }
    const size_t nblocks = (raw_len + 65534) / 65535, z_len = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(z_len);
    if (!z) { free(raw); return 0; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        z[o++] = (uint8_t)(off + n == raw_len);
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + off, n); o += n;
    }
    Be32(z + o, (b << 16) | a); o += 4;
    free(raw);
    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return 0; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    uint8_t ihdr[13];
    Be32(ihdr, (uint32_t)w); Be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    int ok = fwrite(sig, 1, 8, f) == 8 && WriteChunk(f, "IHDR", ihdr, 13) &&
             WriteChunk(f, "IDAT", z, (uint32_t)o) && WriteChunk(f, "IEND", NULL, 0);
    free(z);
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* ── Input tape (same semantics as tlk_headless) ────────────────────────── */
static uint16_t *s_tape;
static size_t s_tape_frames;
static int s_polled;
static int s_raw_tape;  /* --input-raw: words are the host pad value, not $4218 */
static size_t s_poll_idx;

static int LoadTape(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len % 2 != 0) { fclose(f); return 0; }
    s_tape_frames = (size_t)len / 2;
    s_tape = (uint16_t *)calloc(s_tape_frames ? s_tape_frames : 1, 2);
    uint8_t *bytes = (uint8_t *)malloc((size_t)len ? (size_t)len : 1);
    int ok = s_tape && bytes && fread(bytes, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (ok)
        for (size_t i = 0; i < s_tape_frames; i++)
            s_tape[i] = (uint16_t)(bytes[2 * i] | (bytes[2 * i + 1] << 8));
    free(bytes);
    return ok;
}
static uint32_t TapeFrame(long frame) {
    if (!s_tape || frame < 0 || (size_t)frame >= s_tape_frames) return 0;
    return s_raw_tape ? s_tape[frame] : joypad_auto_read_word(s_tape[frame]) & 0x0fffu;
}
/* Live sessions record the pad word fed to each emulated frame (u16 LE, from
 * power-on) so a bug seen while playing can be replayed with --input-raw. */
static FILE *s_rec;
static void RecStop(const char *why) {
    if (!s_rec) return;
    fclose(s_rec); s_rec = NULL;
    fprintf(stderr, "input recording stopped: %s\n", why);
}
static uint16_t TapePolled(void) {
    if (!s_tape || s_poll_idx >= s_tape_frames) return 0;
    return joypad_auto_read_word(s_tape[s_poll_idx]) & 0x0fffu;
}
/* Game pad reads this frame; a frame with none is a lag frame (same rule as
 * tlk_headless --ram-log). */
static unsigned long s_polls_frame;
static void PollHook(CpuState *cpu, uint32_t pc24) {
    (void)cpu; (void)pc24;
    s_polls_frame++;
    if (!s_polled) return;   /* practice mode only counts reads */
    uint16_t w = TapePolled();
    g_snes->input1_currentState = w;
    joypad_set_pad(0, w);
    if (s_poll_idx < s_tape_frames) s_poll_idx++;
}
/* Task CC: the same poll when the pad read runs compiled (cpu_state.hunks aot-pad-read-hook). */
extern void (*g_tlk_aot_pad_read_hook)(void);
static void PollHookCompiled(void) { PollHook(&g_cpu, 0xC0A07C); }

/* ── Config ─────────────────────────────────────────────────────────────── */
/* Runtime pad bits: 0 B, 1 Y, 2 Select, 3 Start, 4 Up, 5 Down, 6 Left,
 * 7 Right, 8 A, 9 X, 10 L, 11 R. Binding 12 is turbo (hold to fast-forward),
 * not a pad bit. */
enum { kSnesButtons = 12, kTurbo = 12, kBindings = 13 };
static const char *const kButtonNames[kBindings] = {
    "b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r", "turbo" };
static const SDL_Keycode kDefaultKeys[kBindings] = {
    SDLK_Z, SDLK_A, SDLK_RSHIFT, SDLK_RETURN, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
    SDLK_X, SDLK_S, SDLK_Q, SDLK_W, SDLK_GRAVE };
/* Gamepad defaults are by position (south=B, east=A, west=Y, north=X). The
 * left stick and the analog triggers always add d-pad / L / R on top. */
static const SDL_GamepadButton kDefaultPad[kBindings] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_STICK };
static SDL_Keycode s_keys[kBindings];
static SDL_GamepadButton s_pad_btn[kBindings];
static void ResetBindings(void) {
    memcpy(s_keys, kDefaultKeys, sizeof s_keys);
    memcpy(s_pad_btn, kDefaultPad, sizeof s_pad_btn);
}
static int s_scale = 3, s_fullscreen, s_vsync, s_audio = 1, s_audio_rate = 48000, s_volume = 100;
/* Turbo: while its key or button is held, run s_turbo_speed frames per shown frame. */
static int s_turbo_speed = 4, s_turbo_held;
/* Video: aspect is a SnesDisplayAspect (default 8:7 = square pixels, the
 * picture as it always was here); shader "" = none, else a .glslp/.glsl path.
 * Shaders need OpenGL 3.3 (opengl = 0 forces the plain SDL renderer). */
static int s_aspect = kSnesDisplayAspect_SquarePixels8x7, s_smooth, s_integer, s_opengl = 1;
static char s_shader[1024];
static const char *const kShaders[][2] = {
    { "", "off" },
    { "assets/shaders/crt-soft.glslp", "CRT" },
    { "assets/shaders/lcd-grid.glslp", "LCD grid" },
    { "assets/shaders/sharp.glslp", "Sharpen" },
    { "assets/shaders/warm-composite.glslp", "Composite" } };
enum { kShaderCount = sizeof kShaders / sizeof kShaders[0] };
/* HD Mode 7 (snesrecomp's PpuBindMode7HdSurface): 0 = off, else 2..4. The PPU
 * also draws each line into s_hd at that scale, resampling Mode 7 lines at the
 * finer grid and copying every other line pixel for pixel. Only what is shown
 * changes: s_pixels, the hashes and --dump-png stay native. */
static int s_hd_mode7;
static uint32_t *s_hd;
static const char *const kHdMode7Choices[] = { "off", "2x", "3x", "4x" };
/* Widescreen (tlk_wide.c): 0 off, 1 16:9, 2 21:9, 3 the window's shape. Levels
 * are drawn wider by a host compositor; other screens stay 4:3 with black
 * sides. s_wide_hud puts the HUD at the picture's edges. Hashes and
 * --dump-png stay native. */
static int s_wide, s_wide_hud = 1;
/* Widescreen enemies: 1 widens the game's spawn window by the margin, so
 * enemies are created out of view (tlk_wide.c TlkWideSetSpawnLead). This
 * changes the game: recordings and tapes made without it do not replay. */
static int s_wide_spawn, s_wide_extra;
static const char *const kWideSpawnChoices[] = { "as the game", "out of view" };
static const char *const kWideChoices[] = { "off", "16:9", "21:9", "fit window" };
static const char *const kWideIni[] = { "off", "16:9", "21:9", "adaptive" };
static const char *const kWideHudChoices[] = { "in place", "at the edges" };
static uint32_t s_wide_px[TLK_WIDE_MAX_WIDTH * TLK_WIDE_HEIGHT];
static double s_wide_aspect;   /* aspect of the picture being shown, 0 when native */
static int s_practice;   /* speedrun = practice: level timer, lag, inputs, best times, restart */
/* How the open menu covers the game, in percent: dimming of the picture
 * behind it, and how solid its panel is (see recomp_runtime_ui_set_backdrop). */
static int s_menu_dim = RECOMP_RUNTIME_UI_DEFAULT_DIM_PERCENT, s_menu_opacity = 100;
static char s_rom_cfg[1024];
static const char *s_cfg_path;

static char *Trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* Plain "key = value" lines; [sections] and ; or # comments are ignored.
 * Keys: rom, scale, fullscreen, vsync, audio, audio_rate, volume, shader,
 * aspect (4:3 8:7 1:1), smooth, integer_scale, opengl, hd_mode7 (0 or 2-4),
 * widescreen (off 16:9 21:9 adaptive), widescreen_hud (edges or in place),
 * widescreen_enemies (as the game or out of view), turbo_speed (2-16),
 * speedrun (off or practice), menu_dim and menu_opacity (0-100), and one per binding
 * (b y select start up down left right a x l r turbo) holding an SDL key name such
 * as "Z", "Return", "Right Shift", "Up". */
static void LoadConfig(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = Trim(line);
        if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = Trim(s), *v = Trim(eq + 1);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (!strcmp(k, "rom")) snprintf(s_rom_cfg, sizeof s_rom_cfg, "%s", v);
        else if (!strcmp(k, "scale")) s_scale = atoi(v);
        else if (!strcmp(k, "fullscreen")) s_fullscreen = atoi(v);
        else if (!strcmp(k, "vsync")) s_vsync = atoi(v);
        else if (!strcmp(k, "audio")) s_audio = atoi(v);
        else if (!strcmp(k, "audio_rate")) s_audio_rate = atoi(v);
        else if (!strcmp(k, "volume")) s_volume = atoi(v);
        else if (!strcmp(k, "shader")) snprintf(s_shader, sizeof s_shader, "%s", v);
        else if (!strcmp(k, "aspect")) {
            uint8_t a;
            if (SnesDisplayAspect_Parse(v, &a)) s_aspect = a;
            else fprintf(stderr, "config: unknown aspect '%s' (4:3, 8:7 or 1:1)\n", v);
        }
        else if (!strcmp(k, "smooth")) s_smooth = atoi(v);
        else if (!strcmp(k, "integer_scale")) s_integer = atoi(v);
        else if (!strcmp(k, "opengl")) s_opengl = atoi(v);
        else if (!strcmp(k, "hd_mode7")) s_hd_mode7 = atoi(v) >= 2 ? SDL_min(atoi(v), 4) : 0;
        else if (!strcmp(k, "widescreen")) {
            s_wide = 0;
            for (int i = 1; i < 4; i++) if (!strcmp(v, kWideIni[i])) s_wide = i;
            if (!s_wide && strcmp(v, "off")) fprintf(stderr, "config: unknown widescreen '%s' (off, 16:9, 21:9, adaptive)\n", v);
        }
        else if (!strcmp(k, "widescreen_hud")) s_wide_hud = strcmp(v, "in place") != 0;
        else if (!strcmp(k, "widescreen_enemies")) s_wide_spawn = !strcmp(v, "out of view");
        else if (!strcmp(k, "turbo_speed")) s_turbo_speed = SDL_clamp(atoi(v), 2, 16);
        else if (!strcmp(k, "speedrun")) s_practice = !strcmp(v, "practice");
        else if (!strcmp(k, "menu_dim")) s_menu_dim = SDL_clamp(atoi(v), 0, 100);
        else if (!strcmp(k, "menu_opacity")) s_menu_opacity = SDL_clamp(atoi(v), 0, 100);
        else if (!strncmp(k, "pad_", 4)) {
            for (int i = 0; i < kBindings; i++)
                if (!strcmp(k + 4, kButtonNames[i])) {
                    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(v);
                    if (b != SDL_GAMEPAD_BUTTON_INVALID) s_pad_btn[i] = b;
                    else fprintf(stderr, "config: unknown gamepad button '%s' for %s\n", v, k);
                }
        } else {
            for (int i = 0; i < kBindings; i++)
                if (!strcmp(k, kButtonNames[i])) {
                    SDL_Keycode kc = SDL_GetKeyFromName(v);
                    if (kc != SDLK_UNKNOWN) s_keys[i] = kc;
                    else fprintf(stderr, "config: unknown key name '%s' for %s\n", v, k);
                }
        }
    }
    fclose(f);
}

/* Rewrites the ini with the current settings (comments are not kept). */
static int SaveConfig(void) {
    FILE *f = fopen(s_cfg_path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", s_cfg_path); return 0; }
    fprintf(f, "; tlk_sdl settings (rewritten by the in-game settings menu)\n");
    if (s_rom_cfg[0]) fprintf(f, "rom = %s\n", s_rom_cfg);
    fprintf(f, "scale = %d\nfullscreen = %d\nvsync = %d\naudio = %d\naudio_rate = %d\nvolume = %d\n",
            s_scale, s_fullscreen, s_vsync, s_audio, s_audio_rate, s_volume);
    fprintf(f, "; shader: empty, a bundled assets/shaders/*.glslp or your own .glslp/.glsl path\n");
    fprintf(f, "shader = %s\naspect = %s\nsmooth = %d\ninteger_scale = %d\nopengl = %d\n",
            s_shader, SnesDisplayAspect_Name(s_aspect), s_smooth, s_integer, s_opengl);
    fprintf(f, "; Mode 7 drawn at 2-4x the native resolution (0 = off)\nhd_mode7 = %d\n", s_hd_mode7);
    fprintf(f, "; levels drawn wider: off, 16:9, 21:9 or adaptive (the window's shape)\nwidescreen = %s\n"
               "; HUD in widescreen: edges or in place\nwidescreen_hud = %s\n",
            kWideIni[s_wide], s_wide_hud ? "edges" : "in place");
    fprintf(f, "; widescreen enemies: \"as the game\" or \"out of view\" (created beyond the wider picture;\n"
               "; changes the game, so recordings and tapes made without it do not replay)\nwidescreen_enemies = %s\n",
            kWideSpawnChoices[s_wide_spawn]);
    fprintf(f, "; hold turbo to run turbo_speed times faster\nturbo_speed = %d\n", s_turbo_speed);
    fprintf(f, "speedrun = %s\n", s_practice ? "practice" : "off");
    fprintf(f, "; settings menu: game dimming and panel opacity, percent\nmenu_dim = %d\nmenu_opacity = %d\n",
            s_menu_dim, s_menu_opacity);
    fprintf(f, "; keys use SDL key names\n");
    for (int i = 0; i < kBindings; i++) fprintf(f, "%s = %s\n", kButtonNames[i], SDL_GetKeyName(s_keys[i]));
    fprintf(f, "; gamepad buttons use SDL names (a = south, b = east, x = west, y = north,\n"
               "; back, start, guide, leftshoulder, rightshoulder, dpup, dpdown, ...)\n");
    for (int i = 0; i < kBindings; i++)
        fprintf(f, "pad_%s = %s\n", kButtonNames[i], SDL_GetGamepadStringForButton(s_pad_btn[i]));
    return fclose(f) == 0;
}

/* Last directory separator in a path ('\\' too on Windows), or NULL. */
static const char *LastSep(const char *p) {
    const char *sl = strrchr(p, '/');
#ifdef _WIN32
    const char *bs = strrchr(p, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
#endif
    return sl;
}

/* A file next to the ini. */
static void SidePath(const char *name, char *out, size_t n) {
    const char *sl = LastSep(s_cfg_path);
    if (sl) snprintf(out, n, "%.*s/%s", (int)(sl - s_cfg_path), s_cfg_path, name);
    else snprintf(out, n, "%s", name);
}

/* Savestate slot path: tlk_slotN.state in the ini's directory. */
static void SlotPath(int slot, char *out, size_t n) {
    char name[32];
    snprintf(name, sizeof name, "tlk_slot%d.state", slot);
    SidePath(name, out, n);
}

/* ── Overlay: 8x8 text in the 256x224 logical space ────────────────────── */
static char s_toast[64];
static uint64_t s_toast_until;

static void Toast(const char *msg) {
    snprintf(s_toast, sizeof s_toast, "%s", msg);
    s_toast_until = SDL_GetTicksNS() + 2000000000ull;
    fprintf(stderr, "%s\n", msg);
}

/* The overlay is drawn in software into s_screen, so a shader treats it like
 * the game picture and both presenters show the same thing. */
static uint32_t s_ink;
static void Ink(int r, int g, int b) { s_ink = (uint32_t)(r << 16 | g << 8 | b); }

static void DrawText(int x, int y, const char *t) {
    for (; *t; t++, x += 8) {
        int c = (unsigned char)*t;
        if (c < 32 || c > 126) c = '?';
        for (int r = 0; r < 8; r++) {
            uint8_t bits = kTlkFont8x8[c - 32][r];
            for (int b = 0; b < 8; b++)
                if ((bits & (1u << b)) && x + b >= 0 && x + b < kWidth && y + r >= 0 && y + r < kHeight)
                    s_screen[(y + r) * kWidth + x + b] = s_ink;
        }
    }
}

/* Black at alpha 230/255 over the picture. */
static void Panel(int x, int y, int w, int h) {
    for (int j = y; j < y + h && j < kHeight; j++)
        for (int i = x; i < x + w && i < kWidth; i++) {
            uint32_t p = s_screen[j * kWidth + i];
            uint32_t r = (p >> 16 & 255) * 25 / 255, g = (p >> 8 & 255) * 25 / 255, b = (p & 255) * 25 / 255;
            s_screen[j * kWidth + i] = r << 16 | g << 8 | b;
        }
}

/* Host state shared by main() and the event handler. */
static SDL_Window *win;
static SDL_Renderer *ren;      /* plain SDL presenter, when OpenGL is off or unavailable */
static SDL_Texture *tex;
static struct RendererFuncs s_gl;
static int s_gl_on;            /* the OpenGL presenter owns the window */
static SDL_AudioStream *astream;
static int pace = 1, running = 1;
static long frame;
static const char *input_path;
static double audio_acc;

/* Level select names, debug-menu order. */
static const char *const kSelectNames[14] = {
    "Pridelands", "Monkeys", "Graveyard", "Stampede", "Exile", "Hakuna", "Destiny",
    "Be Prepared", "Return", "Pride Rock", "Bug Toss", "Bug Hunt 1", "Bug Hunt 2", "Bug Hunt 3" };
/* Debug-menu INVUL ($7FFF9C): 1 refills health every frame ($C00201), 2 resets
 * lives to 3 before each death ($C08AD9). */
static const char *const kInvulNames[3] = { "Off", "Infinite health", "Infinite lives" };
static int s_sel_level, s_sel_invul;
/* Menu state the rest of the host reads. The menu itself (rows, cursor,
 * drawing) is recomp-ui's runtime menu, built by MenuCreate. s_menu_gl: it
 * is drawn with Dear ImGui over the OpenGL presenter, else into s_screen. */
static RecompRuntimeUi *s_ui;
static int s_menu_open, s_menu_gl, s_key_sel, s_key_wait;
static int s_about;   /* the About window is over the menu; any back/accept closes it */

/* Short on-screen names by position; the ini keeps SDL's names. */
static const char *PadName(SDL_GamepadButton b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "Bottom";
    case SDL_GAMEPAD_BUTTON_EAST: return "Right";
    case SDL_GAMEPAD_BUTTON_WEST: return "Left";
    case SDL_GAMEPAD_BUTTON_NORTH: return "Top";
    case SDL_GAMEPAD_BUTTON_BACK: return "Back";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
    case SDL_GAMEPAD_BUTTON_START: return "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "L3";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "R3";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-Right";
    default: {
        const char *s = SDL_GetGamepadStringForButton(b);
        return s ? s : "-";
    }
    }
}

/* Index of s_shader in kShaders, or -1 for a custom path from the ini. */
static int ShaderIndex(void) {
    for (int i = 0; i < kShaderCount; i++)
        if (!strcmp(s_shader, kShaders[i][0])) return i;
    return -1;
}

static void DrawToast(void) {
    if (!s_toast[0] || SDL_GetTicksNS() > s_toast_until) return;
    int w = (int)strlen(s_toast) * 8 + 8, y = s_practice && !s_menu_open ? 24 : 4;   /* practice: under its timer */
    Panel(4, y, w, 16);
    Ink(255, 255, 255);
    DrawText(8, y + 4, s_toast);
}

/* ── Practice mode ──────────────────────────────────────────────────────── */
/* Reads WRAM only, never writes it, so emulation and recordings are the same
 * with it on or off. Timing follows SBDWolf's practice hack (timer.asm): the
 * level timer starts when the fade-in after a level load ends, counts frames
 * that are neither fading ($0056 bit 0) nor paused ($0A89), and stops on level
 * complete (the level number $7FFF9E changes with the load trigger $A692 set;
 * Pride Rock: $A6C2 set) or on a
 * death (lives $7FFFAA drop). Only a run that entered the level fresh (not a
 * respawn or a savestate) counts for best times, as the hack skips
 * checkpoints. F9 reloads an automatic savestate taken when the level's load
 * finished. */
enum { PR_IDLE, PR_ARMED, PR_RUNNING, PR_DONE, PR_DIED };
enum { kPrLevels = 10, kPrDiffs = 4 };   /* best times: levels 0-9 (as the hack), by difficulty */
static const char *const kPrLevelNames[kPrLevels] = {
    "Pridelands", "Monkeys", "Graveyard", "Stampede", "Exile",
    "Hakuna", "Destiny", "Be Prepared", "Return", "Pride Rock" };   /* short: toasts fit 256 px */
static struct {
    int st, level, fresh, eligible, loading, hook;
    int run_on, run_done, run_dirty;   /* run timer: everything from game start */
    long frames, lag, run_frames;
    uint16_t lvl, state, lives, fin, pad, cpx, cpy;
    uint8_t fade;
} pr;
static long s_best[kPrLevels][kPrDiffs];   /* frames, 0 = none */
/* Checkpoint splits: level time when the checkpoint ($7FFFA4/A6) changed, keyed by its position. */
typedef struct { int l, d; uint16_t x, y; long fr; } PrCp;
static PrCp s_cp_best[64];
static int s_cp_n;
/* TAS reference: 6987M (tapes/6987M.read.tape) measured with these same rules. */
static const long kTasLevel[kPrLevels] = { 1390, 7757, 4240, 4185, 1431, 4016, 4716, 2695, 2491, 3064 };
static const PrCp kTasCp[] = { { 0, 0, 0x04B0, 0x0070, 904 }, { 1, 0, 0x1270, 0x01E0, 4562 },
                               { 6, 0, 0x08C0, 0x0270, 1920 } };

static uint16_t Wram16(uint32_t off) { return (uint16_t)(g_ram[off] | g_ram[off + 1] << 8); }

static void PrTimesPath(char *out, size_t n) { SidePath("tlk_practice_times.txt", out, n); }
static void PrRestartPath(char *out, size_t n) { SidePath("tlk_practice_restart.state", out, n); }

/* "level difficulty frames" per line. */
static void PrLoadTimes(void) {
    char path[1100], line[64];
    PrTimesPath(path, sizeof path);
    memset(s_best, 0, sizeof s_best);
    s_cp_n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int l, d;
        unsigned x, y;
        long fr;
        if (sscanf(line, "cp %d %d %x %x %ld", &l, &d, &x, &y, &fr) == 5) {
            if (s_cp_n < 64 && fr > 0) s_cp_best[s_cp_n++] = (PrCp){ l, d, (uint16_t)x, (uint16_t)y, fr };
        } else if (sscanf(line, "%d %d %ld", &l, &d, &fr) == 3 && l >= 0 && l < kPrLevels && d >= 0 && d < kPrDiffs && fr > 0)
            s_best[l][d] = fr;
    }
    fclose(f);
}
static void PrSaveTimes(void) {
    char path[1100];
    PrTimesPath(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "; tlk_sdl practice best times: level difficulty frames (60.0988 per second)\n"
               "; checkpoints: cp level difficulty x y frames\n");
    for (int l = 0; l < kPrLevels; l++)
        for (int d = 0; d < kPrDiffs; d++)
            if (s_best[l][d]) fprintf(f, "%d %d %ld\n", l, d, s_best[l][d]);
    for (int i = 0; i < s_cp_n; i++)
        fprintf(f, "cp %d %d %04X %04X %ld\n", s_cp_best[i].l, s_cp_best[i].d, s_cp_best[i].x, s_cp_best[i].y, s_cp_best[i].fr);
    fclose(f);
}

/* Frames as m:ss.cc of real time. */
static void PrFmt(char *out, size_t n, long frames) {
    long cs = (long)(frames * 100.0 / NTSC_FPS + 0.5);
    snprintf(out, n, "%ld:%02ld.%02ld", cs / 6000, cs / 100 % 60, cs % 100);
}

/* Signed difference, "+0:01.23" / "-0:01.23". */
static void PrDelta(char *out, size_t n, long diff) {
    char t[16];
    PrFmt(t, sizeof t, diff < 0 ? -diff : diff);
    snprintf(out, n, "%c%s", diff < 0 ? '-' : '+', t);
}

/* Take the current WRAM as the baseline, so the next frame sees no transition. */
static void PrSync(void) {
    pr.lvl = Wram16(0x1FF9E); pr.state = Wram16(0x0008); pr.lives = Wram16(0x1FFAA);
    pr.fin = Wram16(0xA6C2); pr.fade = g_ram[0x0056];
    pr.cpx = Wram16(0x1FFA4); pr.cpy = Wram16(0x1FFA6);
}

/* After a savestate load: time from here, not for best times. F9 sets eligible. */
static void PrAfterLoad(int eligible) {
    PrSync();
    pr.level = pr.lvl; pr.fresh = 0; pr.eligible = eligible; pr.loading = 0;
    pr.run_dirty = 1;
    pr.frames = 0; pr.lag = 0;
    pr.st = (pr.fade & 1) ? PR_ARMED : PR_RUNNING;
}

static void PrFinish(void) {
    char t[16], msg[64];
    PrFmt(t, sizeof t, pr.frames);
    pr.st = PR_DONE;
    int l = pr.level, d = Wram16(0x1FF92);
    if (l == 9) pr.run_done = 1;
    const char *name = l >= 0 && l < kPrLevels ? kPrLevelNames[l] : "Bonus";
    if (!pr.eligible || l < 0 || l >= kPrLevels || d < 0 || d >= kPrDiffs) {
        snprintf(msg, sizeof msg, "%s %s", name, t);
    } else if (!s_best[l][d] || pr.frames < s_best[l][d]) {
        long old = s_best[l][d];
        s_best[l][d] = pr.frames;
        PrSaveTimes();
        if (old) {
            char dt[16];
            PrFmt(dt, sizeof dt, old - pr.frames);
            snprintf(msg, sizeof msg, "%s %s best -%s", name, t, dt);
        } else snprintf(msg, sizeof msg, "%s %s best", name, t);
    } else {
        char dt[16];
        PrFmt(dt, sizeof dt, pr.frames - s_best[l][d]);
        snprintf(msg, sizeof msg, "%s %s +%s", name, t, dt);
    }
    Toast(msg);
}

/* Simba reached a checkpoint: level time so far against the best and the TAS. */
static void PrCheckpoint(uint16_t x, uint16_t y) {
    char t[16], dt[16], msg[64];
    int l = pr.level, d = Wram16(0x1FF92);
    PrFmt(t, sizeof t, pr.frames);
    snprintf(msg, sizeof msg, "CP %s", t);
    PrCp *b = NULL;
    for (int i = 0; i < s_cp_n; i++)
        if (s_cp_best[i].l == l && s_cp_best[i].d == d && s_cp_best[i].x == x && s_cp_best[i].y == y) b = &s_cp_best[i];
    if (pr.eligible && b && pr.frames >= b->fr) {
        PrDelta(dt, sizeof dt, pr.frames - b->fr);
        snprintf(msg, sizeof msg, "CP %s %s", t, dt);
    } else if (pr.eligible) {
        if (b) {
            PrDelta(dt, sizeof dt, pr.frames - b->fr);
            snprintf(msg, sizeof msg, "CP %s best %s", t, dt);
            b->fr = pr.frames;
        } else {
            for (size_t i = 0; i < sizeof kTasCp / sizeof kTasCp[0]; i++)
                if (kTasCp[i].l == l && kTasCp[i].x == x && kTasCp[i].y == y) {
                    PrDelta(dt, sizeof dt, pr.frames - kTasCp[i].fr);
                    snprintf(msg, sizeof msg, "CP %s TAS %s", t, dt);
                }
            if (s_cp_n < 64) s_cp_best[s_cp_n++] = (PrCp){ l, d, x, y, pr.frames };
        }
        PrSaveTimes();
    }
    Toast(msg);
}

/* Once per emulated frame, after RtlRunFrame. */
static void PracticeFrame(uint16_t pad) {
    uint16_t lvl = Wram16(0x1FF9E), state = Wram16(0x0008), lives = Wram16(0x1FFAA);
    uint16_t fin = Wram16(0xA6C2), pause = Wram16(0x0A89), demo = Wram16(0x1E03) & 0x8000;
    uint8_t fade = g_ram[0x0056];
    pr.pad = pad;
    uint16_t cpx = Wram16(0x1FFA4), cpy = Wram16(0x1FFA6);
    if (lvl != pr.lvl) {   /* run timer: starts leaving the title ($0E), stops back on it */
        if (pr.lvl == 0x0E && !demo) { pr.run_on = 1; pr.run_done = 0; pr.run_frames = 0; pr.run_dirty = lvl != 0; }
        if (lvl == 0x0E) { pr.run_on = 0; pr.run_frames = 0; }
    }
    if (pr.run_on && !pr.run_done) pr.run_frames++;
    if (lvl != pr.lvl) {   /* level complete ($A692 set), or game start / game over (not) */
        if (pr.st == PR_RUNNING && Wram16(0xA692)) PrFinish();
        else if (pr.st == PR_RUNNING || pr.st == PR_ARMED) pr.st = PR_IDLE;
        pr.fresh = 1;
    } else if (pr.st == PR_RUNNING && lvl == 9 && fin && !pr.fin) {
        PrFinish();                                   /* Pride Rock: the last hit on Scar */
    } else if (pr.st == PR_RUNNING && (cpx != pr.cpx || cpy != pr.cpy)) {
        PrCheckpoint(cpx, cpy);
    } else if (pr.st == PR_RUNNING && lives < pr.lives) {
        pr.st = PR_DIED;
        pr.fresh = 0;
    }
    if (state == 0 || state == 0x8000) pr.loading = 1;   /* $0008: 0 loading, 4 playing (6 in between) */
    if (state == 4 && pr.loading) {   /* a load just finished */
        pr.loading = 0;
        if (!demo && lvl != 0x0E) {     /* $0E: title */
            pr.st = PR_ARMED; pr.level = lvl; pr.eligible = pr.fresh;
            if (pr.fresh && lvl < kPrLevels) {
                char path[1100];
                PrRestartPath(path, sizeof path);
                tlk_state_save(path, (uint32_t)frame);
            }
            pr.fresh = 0;
        }
    }
    if (pr.st == PR_ARMED && !(fade & 1)) { pr.st = PR_RUNNING; pr.frames = 0; pr.lag = 0; }
    if (pr.st == PR_RUNNING && !(fade & 1) && !pause) {
        pr.frames++;
        if (!s_polls_frame) pr.lag++;
    }
    pr.lvl = lvl; pr.state = state; pr.lives = lives; pr.fin = fin; pr.fade = fade;
    pr.cpx = cpx; pr.cpy = cpy;
}

/* Grey label, then the value in the current ink; returns the next x. */
static int PrLabel(int x, int y, const char *label) {
    uint32_t ink = s_ink;
    Ink(150, 150, 150);
    DrawText(x, y, label);
    s_ink = ink;
    return x + (int)strlen(label) * 8;
}

/* Two rows at the top, between the health meter (ends x~42) and the roar
 * meter (starts x~215): level time and T(AS), then R(UN) and L(AG), in fixed
 * columns so nothing jumps while the digits change. */
static void DrawPractice(void) {
    char t[16], lvl[16], tas[16], run[16], lag[16];
    if (pr.st == PR_IDLE) snprintf(t, sizeof t, "-:--.--");
    else PrFmt(t, sizeof t, pr.st == PR_ARMED ? 0 : pr.frames);
    snprintf(lvl, sizeof lvl, "%s%s", pr.st == PR_DIED ? "x" : "", t);
    tas[0] = run[0] = 0;
    if (pr.level >= 0 && pr.level < kPrLevels && pr.st != PR_IDLE) PrFmt(tas, sizeof tas, kTasLevel[pr.level]);
    if (pr.run_on) {
        PrFmt(t, sizeof t, pr.run_frames);
        snprintf(run, sizeof run, "%s%s", t, pr.run_dirty ? "*" : "");
    }
    snprintf(lag, sizeof lag, "%ld", pr.lag);
    enum { kX = 60, kY = 4 };   /* 17 characters wide, centred */
    Panel(kX - 2, kY - 2, 17 * 8 + 4, 20);
    if (pr.st == PR_DONE) Ink(120, 255, 120);
    else if (pr.st == PR_DIED) Ink(255, 120, 120);
    else Ink(255, 255, 255);
    DrawText(kX, kY, lvl);
    Ink(200, 200, 200);
    DrawText(PrLabel(kX + 9 * 8, kY, "T"), kY, tas);
    DrawText(PrLabel(kX, kY + 9, "R"), kY + 9, run);
    DrawText(PrLabel(kX + 12 * 8, kY + 9, "L"), kY + 9, lag);
    /* Inputs, bottom right: d-pad, face buttons, shoulders, Select (s), Start (S). */
    static const struct { char c; int bit; } kIn[] = {
        { '<', 6 }, { '^', 4 }, { 'v', 5 }, { '>', 7 }, { 'B', 0 }, { 'A', 8 }, { 'Y', 1 },
        { 'X', 9 }, { 'L', 10 }, { 'R', 11 }, { 's', 2 }, { 'S', 3 } };
    int n = (int)(sizeof kIn / sizeof kIn[0]), ix = 252 - n * 8, iy = 212;
    Panel(ix - 2, iy - 2, n * 8 + 4, 12);
    for (int i = 0; i < n; i++) {
        char g[2] = { kIn[i].c, 0 };
        if (pr.pad & (1u << kIn[i].bit)) Ink(255, 210, 80);
        else Ink(70, 70, 70);
        DrawText(ix + i * 8, iy, g);
    }
}

/* Level select: reload the power-on savestate and play the debug-menu
 * recipe of tlk-map/make_level_tape.py (task F): title -> Options -> B A R R Y
 * opens INVUL / LEVEL, each A on INVUL or LEVEL adds one, Start, then Up+Start
 * on the title starts at that level. Input only, no WRAM writes. */
static uint16_t s_macro[2048];
static int s_macro_n, s_macro_i;

static void MacroWait(int n) { while (n-- > 0 && s_macro_n < 2048) s_macro[s_macro_n++] = 0; }
static void MacroTap(uint16_t w) {   /* 2 frames held, 4 released */
    for (int i = 0; i < 2 && s_macro_n < 2048; i++) s_macro[s_macro_n++] = w;
    MacroWait(4);
}
static void BuildLevelMacro(int level, int invul) {
    enum { B = 1 << 0, Y = 1 << 1, START = 1 << 3, UP = 1 << 4, DOWN = 1 << 5, A = 1 << 8, R = 1 << 11 };
    s_macro_n = 0; s_macro_i = 0;
    MacroWait(700); MacroTap(DOWN); MacroWait(10); MacroTap(START); MacroWait(80);
    MacroTap(B); MacroWait(20); MacroTap(A); MacroWait(20); MacroTap(R); MacroWait(20);
    MacroTap(R); MacroWait(20); MacroTap(Y); MacroWait(60);
    if (invul) {   /* the cursor starts on LEVEL (measured): Up to INVUL */
        MacroTap(UP); MacroWait(20);
        for (int i = 0; i < invul; i++) { MacroTap(A); MacroWait(20); }
    }
    MacroTap(DOWN); MacroWait(20);
    for (int i = 0; i < level; i++) { MacroTap(A); MacroWait(20); }
    MacroWait(20); MacroTap(START); MacroWait(60); MacroTap(UP); MacroWait(20); MacroTap(START);
}

static void PrBootPath(char *out, size_t n) { SidePath("tlk_practice_boot.state", out, n); }
static int LoadStateLive(const char *path);
static void CloseMenu(void);
static void StartLabel(void);

static void PrStartLevel(int level) {
    char path[1100], msg[64];
    PrBootPath(path, sizeof path);
    if (!LoadStateLive(path)) { Toast("Level select: no power-on state"); return; }
    BuildLevelMacro(level, s_sel_invul);
    s_sel_level = level;
    StartLabel();
    PrSync();
    pr.st = PR_IDLE; pr.fresh = 0; pr.loading = 0; pr.frames = 0; pr.lag = 0;
    pr.run_on = 0; pr.run_frames = 0;
    snprintf(msg, sizeof msg, "Loading %s", kSelectNames[level]);
    Toast(msg);
    CloseMenu();
}

/* Lag counting needs the pad-read hook (the same one --input-polled uses);
 * it only counts unless a polled tape is playing. Armed once, after SnesInit. */
static int s_snes_ready;
static void PrEnable(void) {
    if (!s_snes_ready) return;
    if (!pr.hook) {
        interp_bridge_add_pre_opcode_hook(0xC0A07C, PollHook);
        interp_bridge_add_pre_opcode_hook(0x00A07C, PollHook);
        g_tlk_aot_pad_read_hook = PollHookCompiled;
        pr.hook = 1;
    }
    PrLoadTimes();
    PrSync();
    pr.st = PR_IDLE;
}

/* ── Presenting ─────────────────────────────────────────────────────────── */
static void ApplyVideoConfig(void) { TlkVideoConfig(s_shader, s_aspect, s_smooth); }

/* sw x sh is the presented picture, native or HD; the maths runs on the
 * native size so integer scaling still counts game pixels. */
static void ComputeViewport(int sw, int sh, int dw, int dh, SnesDisplayViewport *vp) {
    if (s_wide_aspect > 0 && sh == kHeight && sw > kWidth) {   /* widescreen: its own shape */
        *vp = SnesDisplayAspect_FitViewport(s_wide_aspect, dw, dh);
        if (s_integer && vp->height >= kHeight) {   /* whole multiples of the game's lines */
            int h = vp->height / kHeight * kHeight, w = (int)(h * s_wide_aspect + 0.5);
            *vp = (SnesDisplayViewport){ (dw - w) / 2, (dh - h) / 2, w, h };
        }
        return;
    }
    if (sw > kWidth) { sh = sh * kWidth / sw; sw = kWidth; }
    SnesDisplayAspect_ComputeViewport(sw, sh, dw, dh, SnesDisplayAspect_Clamp(s_aspect), false,
                                      s_integer != 0, vp);
}

/* Window size for the scale and aspect: height = 224 * scale. */
static int WindowWidth(void) {
    /* Widescreen: the window takes the picture's shape (fit window starts at 16:9). */
    if (s_wide) return (int)(kHeight * s_scale * (s_wide == 2 ? 21.0 / 9.0 : 16.0 / 9.0) + 0.5);
    return SnesDisplayAspect_ComputeWindowWidth(kWidth, kHeight, kHeight * s_scale, SnesDisplayAspect_Clamp(s_aspect));
}
static void SizeWindow(void) {
    if (!s_fullscreen) SDL_SetWindowSize(win, WindowWidth(), kHeight * s_scale);
}

static int TurboHeld(void);

/* --dump-window with the OpenGL presenter: read the next composited window
 * (shader and ImGui menu included) back before it is swapped. */
static const char *s_dump_req;
static int s_dump_ok;

static void ReadBackWindow(int w, int h) {
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4), *row = (uint8_t *)malloc((size_t)w * 4);
    s_dump_ok = 0;
    if (px && row) {
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);
        for (int y = 0; y < h / 2; y++) {   /* GL rows run bottom-up */
            memcpy(row, px + (size_t)y * w * 4, (size_t)w * 4);
            memcpy(px + (size_t)y * w * 4, px + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
            memcpy(px + (size_t)(h - 1 - y) * w * 4, row, (size_t)w * 4);
        }
        s_dump_ok = WritePng(s_dump_req, px, w, h, w * 4);
    }
    free(px);
    free(row);
}

/* The presenter's overlay hook: the menu, drawn after the game picture (and
 * its shader) over the whole window, just before the swap. */
static void DrawOverlay(int w, int h) {
    if (s_menu_open && s_menu_gl) TlkMenuImguiDraw(s_ui, &s_about);
    if (s_dump_req) ReadBackWindow(w, h);
}

/* (Re)binds the PPU's HD Mode 7 surface for s_hd_mode7. Takes effect from
 * the next frame drawn. */
static void ApplyHdMode7(void) {
    PpuBindMode7HdSurface(g_ppu, NULL, 0, 0, 0, 0, 0);
    free(s_hd);
    s_hd = NULL;
    int sc = s_hd_mode7;
    if (sc < 2) return;
    size_t pitch = (size_t)kWidth * sc * 4, cap = pitch * kHeight * sc;
    s_hd = (uint32_t *)calloc(1, cap);
    if (!s_hd || !PpuBindMode7HdSurface(g_ppu, s_hd, cap, pitch, kWidth, kHeight, (unsigned)sc)) {
        fprintf(stderr, "HD Mode 7: cannot bind a %dx surface\n", sc);
        free(s_hd);
        s_hd = NULL;
        s_hd_mode7 = 0;
    }
}

/* The HD picture into dst (pitch in bytes), with every overlay pixel (those
 * where s_screen differs from s_pixels) scaled up over it. */
static void ComposeHd(uint8_t *dst, int pitch) {
    const int sc = s_hd_mode7, w = kWidth * sc;
    for (int y = 0; y < kHeight * sc; y++) memcpy(dst + (size_t)y * pitch, s_hd + (size_t)y * w, (size_t)w * 4);
    for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < kWidth; x++) {
            uint32_t p, o = s_screen[y * kWidth + x];
            memcpy(&p, s_pixels + (y * kWidth + x) * 4, 4);
            if (o == p) continue;
            for (int j = 0; j < sc; j++) {
                uint32_t *row = (uint32_t *)(dst + (size_t)(y * sc + j) * pitch) + x * sc;
                for (int i = 0; i < sc; i++) row[i] = o;
            }
        }
}

/* Width of the widescreen picture for the current setting and window, 0 when
 * the native picture is shown. Widescreen wins over HD Mode 7, which this game
 * never uses. */
static int WideWidth(void) {
    s_wide_aspect = 0;
    s_wide_extra = 0;
    if (!s_wide || !TlkWideEnabled()) return 0;
    double aspect = s_wide == 1 ? 16.0 / 9.0 : 21.0 / 9.0;
    if (s_wide == 3) {
        int dw = 0, dh = 0;
        SDL_GetWindowSizeInPixels(win, &dw, &dh);
        aspect = dw > 0 && dh > 0 ? (double)dw / dh : 16.0 / 9.0;
    }
    SnesDisplayFrame f = SnesDisplayAspect_ComputeAdaptiveFrame(kWidth, kHeight, TLK_WIDE_MAX_WIDTH, aspect,
                                                                SnesDisplayAspect_Clamp(s_aspect));
    if (f.width <= kWidth) return 0;
    s_wide_aspect = f.aspect;
    s_wide_extra = f.extra;
    return f.width;
}

/* The widescreen picture into dst (pitch in bytes), with every overlay pixel
 * (where s_screen differs from s_pixels) over its native centre. */
static void ComposeWide(uint8_t *dst, int pitch, int w) {
    const int extra = (w - kWidth) / 2;
    if (!TlkWideDraw(s_wide_px, w, s_wide_hud != 0)) {   /* nothing captured yet: native, centred */
        memset(s_wide_px, 0, (size_t)w * kHeight * 4);
        for (int y = 0; y < kHeight; y++) memcpy(s_wide_px + (size_t)y * w + extra, s_pixels + y * kWidth * 4, kWidth * 4);
    }
    for (int y = 0; y < kHeight; y++) {
        uint32_t *row = (uint32_t *)(dst + (size_t)y * pitch);
        memcpy(row, s_wide_px + (size_t)y * w, (size_t)w * 4);
        for (int x = 0; x < kWidth; x++) {
            uint32_t p, o = s_screen[y * kWidth + x];
            memcpy(&p, s_pixels + (y * kWidth + x) * 4, 4);
            if (o != p) row[extra + x] = o;
        }
    }
}

/* Shows s_pixels plus the overlay (menu when open, toast, turbo tag). */
static void Present(void) {
    memcpy(s_screen, s_pixels, sizeof s_screen);
    if (s_practice && !s_menu_open) DrawPractice();
    DrawToast();
    if (TurboHeld() && !s_menu_open) {
        char t[8];
        snprintf(t, sizeof t, ">>%dx", s_turbo_speed);
        Panel(kWidth - 48, 4, 44, 16);
        Ink(255, 255, 255);
        DrawText(kWidth - 44, 8, t);
    }
    /* Without ImGui the same menu is drawn into the 256x224 picture. */
    if (s_menu_open && !s_menu_gl) recomp_runtime_ui_render_argb8888(s_ui, s_screen, kWidth, kHeight, kWidth * 4);
    const int sc = s_hd ? s_hd_mode7 : 1, ww = WideWidth();
    const int tw = ww ? ww : kWidth * sc, th = ww ? kHeight : kHeight * sc;
    if (s_gl_on) {
        uint8 *px;
        int pitch;
        s_gl.BeginDraw(tw, th, &px, &pitch);
        if (ww) ComposeWide(px, pitch, ww);
        else if (sc > 1) ComposeHd(px, pitch);
        else for (int y = 0; y < kHeight; y++) memcpy(px + (size_t)y * pitch, s_screen + y * kWidth, kWidth * 4);
        s_gl.EndDraw();
        return;
    }
    int dw = 0, dh = 0;
    SDL_GetCurrentRenderOutputSize(ren, &dw, &dh);
    SnesDisplayViewport vp;
    ComputeViewport(ww ? tw : kWidth, ww ? th : kHeight, dw, dh, &vp);
    SDL_FRect dst = { (float)vp.x, (float)vp.y, (float)vp.width, (float)vp.height };
    if (tex && (tex->w != tw || tex->h != th)) {   /* HD Mode 7 or widescreen changed: a texture of the new size */
        SDL_DestroyTexture(tex);
        tex = NULL;
    }
    if (!tex) {
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, tw, th);
        if (!tex) return;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    }
    SDL_SetTextureScaleMode(tex, s_smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    if (ww || sc > 1) {
        void *px;
        int pitch;
        if (!SDL_LockTexture(tex, NULL, &px, &pitch)) return;
        if (ww) ComposeWide((uint8_t *)px, pitch, ww);
        else ComposeHd((uint8_t *)px, pitch);
        SDL_UnlockTexture(tex);
    } else SDL_UpdateTexture(tex, NULL, s_screen, kWidth * kBpp);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, NULL, &dst);
    if (s_dump_req) {   /* --dump-window: the rendered window, as with OpenGL */
        SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL), *xrgb = NULL;
        if (shot) xrgb = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_XRGB8888);
        s_dump_ok = xrgb && WritePng(s_dump_req, (const uint8_t *)xrgb->pixels, xrgb->w, xrgb->h, xrgb->pitch);
        SDL_DestroySurface(xrgb);
        SDL_DestroySurface(shot);
    }
    SDL_RenderPresent(ren);
}

static void SetVsync(void) {
    int on = (s_vsync && pace) ? 1 : 0;
    if (s_gl_on) {
        snesrecomp_opengl_set_vsync(on);
        if (s_gl.Reconfigure) s_gl.Reconfigure();
    } else SDL_SetRenderVSync(ren, on);
}

/* OpenGL 3.3 presenter when allowed and available, else the SDL renderer
 * (also what the dummy video driver gets, e.g. --selftest-remap). */
static int OpenVideo(void) {
    Uint32 flags = SDL_WINDOW_RESIZABLE | (s_fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    int w = WindowWidth(), h = kHeight * s_scale;
    ApplyVideoConfig();
    if (s_opengl) {
        OpenGLRenderer_Create(&s_gl);   /* sets the GL 3.3 core attributes */
        win = SDL_CreateWindow("The Lion King", w, h, flags | SDL_WINDOW_OPENGL);
        /* Probe for a 3.3 context first: the presenter exits if it gets none. */
        SDL_GLContext probe = win ? SDL_GL_CreateContext(win) : NULL;
        if (probe) {
            SDL_GL_DestroyContext(probe);
            snesrecomp_opengl_set_vsync((s_vsync && pace) ? 1 : 0);
            snesrecomp_opengl_set_viewport(ComputeViewport);
            s_gl_on = s_gl.Initialize(win);
        }
        if (s_gl_on) {   /* the presenter's context is current here */
            char font[1200];
            TlkAssetPath("assets/fonts/LatoLatin-Regular.ttf", font, sizeof font);
            s_menu_gl = TlkMenuImguiInit(win, font, 18.0f);   /* recomp-ui's theme size */
            if (!s_menu_gl) fprintf(stderr, "menu: Dear ImGui did not start; drawing it in the game picture\n");
            snesrecomp_opengl_set_overlay(DrawOverlay);
        }
        if (!s_gl_on) {
            fprintf(stderr, "OpenGL 3.3 unavailable (%s): plain SDL renderer, no shaders\n", SDL_GetError());
            if (win) SDL_DestroyWindow(win);
            win = NULL;
        }
    }
    if (!s_gl_on) {
        SDL_GL_ResetAttributes();
        win = SDL_CreateWindow("The Lion King", w, h, flags);
        ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
        if (!ren) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 0; }
        SDL_SetRenderVSync(ren, (s_vsync && pace) ? 1 : 0);
        /* The PPU leaves the top byte 0: an alpha format would draw every pixel transparent. */
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    }
    return 1;
}

/* ── Live input ─────────────────────────────────────────────────────────── */
static uint16_t s_kbd_pad;
static SDL_Gamepad *s_pad;
static int s_pad_hold;   /* after the menu closes: ignore the pad until all of it is released */

static void OpenFirstGamepad(void) {
    if (s_pad) return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) {
        s_pad = SDL_OpenGamepad(ids[0]);
        if (s_pad) {
            char msg[64];
            snprintf(msg, sizeof msg, "Gamepad: %.50s", SDL_GetGamepadName(s_pad));
            Toast(msg);
        }
    }
    SDL_free(ids);
}

static uint16_t GamepadState(void) {
    if (!s_pad) return 0;
    uint16_t w = 0;
    for (int i = 0; i < kSnesButtons; i++)
        if (SDL_GetGamepadButton(s_pad, s_pad_btn[i])) w |= (uint16_t)(1u << i);
    const int dz = 16000;
    int ax = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX);
    int ay = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (ax < -dz) w |= 1u << 6;
    if (ax > dz) w |= 1u << 7;
    if (ay < -dz) w |= 1u << 4;
    if (ay > dz) w |= 1u << 5;
    if (SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > dz) w |= 1u << 10;
    if (SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > dz) w |= 1u << 11;
    int any = w != 0 || SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_GUIDE);
    if (s_pad_hold) {
        if (any) return 0;
        s_pad_hold = 0;
    }
    return w;
}

/* Turbo key or pad button held (the pad waits like GamepadState after the menu). */
static int TurboHeld(void) {
    return s_turbo_held || (s_pad && !s_pad_hold && SDL_GetGamepadButton(s_pad, s_pad_btn[kTurbo]));
}

/* ── Settings menu: recomp-ui's runtime menu ───────────────────────────── */
/* The rows, in order; sections appear in first-use order. Every change is
 * applied at once and saved to the ini (the menu calls MenuSave after it). */
enum {
    R_SCALE, R_FULLSCREEN, R_VSYNC, R_ASPECT, R_DIM, R_OPACITY,
    R_SHADER, R_HD_MODE7, R_WIDE, R_WIDE_HUD, R_WIDE_SPAWN, R_SMOOTH, R_INTEGER,
    R_AUDIO, R_VOLUME,
    R_LEVEL, R_INVUL, R_START, R_PRACTICE,
    R_BIND0, R_RESET = R_BIND0 + kBindings,
    R_RESUME, R_QUIT,
    R_ABOUT, R_AUTHOR, R_SOURCE, R_COUNT };

static const char *const kAspectChoices[] = { "4:3 TV", "8:7 pixel", "1:1" };
static const char *const kBindNames[kBindings] = {
    "B", "Y", "Select", "Start", "Up", "Down", "Left", "Right", "A", "X", "L", "R", "Turbo" };
/* Shader names, plus "custom" when the ini names a shader that is not a bundled preset. */
static const char *s_shader_names[kShaderCount + 1];
static char s_shader_custom[1024];
/* Binding rows show their binding in the label ("B:  Z  /  Bottom"). */
static char s_bind_label[kBindings][64];
/* The level select's action row names the level it goes to. */
static char s_start_label[48];
/* About: the first row opens tlk_menu_imgui.cpp's About window (OpenGL menu
 * only); the author and source rows open their page in the browser. */
#define AUTHOR_URL "https://kinduff.com"
#define SOURCE_URL "https://github.com/kinduff/TLKRecomp"

#define MENU_ROW(key, sec, label, desc, type, lo, hi, step, ch, n) \
    { key, sec, label, desc, RECOMP_RUNTIME_UI_##type, lo, hi, step, ch, n, NULL }
#define BIND_ROW(i) MENU_ROW("tlk.bind", "Controls", s_bind_label[i], \
    "Enter, then the new key or button.", ACTION, 0, 0, 0, NULL, 0)
static RecompRuntimeUiItem s_rows[R_COUNT] = {
    [R_SCALE] = MENU_ROW(RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE, "Display", "Window scale",
                         "Window height, times 224 pixels.", INT, 1, 8, 1, NULL, 0),
    [R_FULLSCREEN] = MENU_ROW("tlk.fullscreen", "Display", "Fullscreen",
                              "Also F11 or Alt+Enter.", BOOL, 0, 1, 1, NULL, 0),
    [R_VSYNC] = MENU_ROW("tlk.vsync", "Display", "Vsync",
                         "Pace to the display, not the 60.1 Hz clock.", BOOL, 0, 1, 1, NULL, 0),
    [R_ASPECT] = MENU_ROW("tlk.aspect", "Display", "Aspect",
                          "Picture shape: a TV, or square pixels.", CHOICE, 0, 2, 1, kAspectChoices, 3),
    [R_DIM] = MENU_ROW(RECOMP_RUNTIME_UI_KEY_MENU_DIM, "Display", "Game dimming",
                       "How dark the game is behind the menu.", INT, 0, 100, 10, NULL, 0),
    [R_OPACITY] = MENU_ROW(RECOMP_RUNTIME_UI_KEY_MENU_OPACITY, "Display", "Menu opacity",
                           "How solid the menu's background is.", INT, 0, 100, 10, NULL, 0),
    [R_SHADER] = MENU_ROW("tlk.shader", "Graphics", "Shader",
                          "Presets in assets/shaders.", CHOICE, 0, kShaderCount - 1, 1,
                          s_shader_names, kShaderCount),
    [R_HD_MODE7] = MENU_ROW("tlk.hd_mode7", "Graphics", "HD Mode 7",
                            "Mode 7 scenes drawn at a finer resolution.", CHOICE, 0, 3, 1,
                            kHdMode7Choices, 4),
    [R_WIDE] = MENU_ROW("tlk.widescreen", "Graphics", "Widescreen",
                        "Experimental. Levels drawn wider; other screens stay 4:3.", CHOICE, 0, 3, 1, kWideChoices, 4),
    [R_WIDE_HUD] = MENU_ROW("tlk.widescreen_hud", "Graphics", "Widescreen HUD",
                            "Health, roar and lives in widescreen.", CHOICE, 0, 1, 1, kWideHudChoices, 2),
    [R_WIDE_SPAWN] = MENU_ROW("tlk.widescreen_enemies", "Graphics", "Widescreen enemies",
                              "Out of view: created beyond the wider picture. Changes the game.",
                              CHOICE, 0, 1, 1, kWideSpawnChoices, 2),
    [R_SMOOTH] = MENU_ROW(RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER, "Graphics", "Smooth",
                          "Bilinear filtering.", BOOL, 0, 1, 1, NULL, 0),
    [R_INTEGER] = MENU_ROW(RECOMP_RUNTIME_UI_KEY_INTEGER_SCALE, "Graphics", "Integer scaling",
                           "Whole multiples of the game's pixels.", BOOL, 0, 1, 1, NULL, 0),
    [R_AUDIO] = MENU_ROW(RECOMP_RUNTIME_UI_KEY_AUDIO, "Audio", "Audio",
                         "Sound on or off.", BOOL, 0, 1, 1, NULL, 0),
    [R_VOLUME] = MENU_ROW(RECOMP_RUNTIME_UI_KEY_VOLUME, "Audio", "Volume",
                          "Percent.", INT, 0, 100, 10, NULL, 0),
    [R_LEVEL] = MENU_ROW("tlk.level", "Level select", "Level",
                         "Nothing happens until Start below.", CHOICE, 0, 13, 1, kSelectNames, 14),
    [R_INVUL] = MENU_ROW("tlk.invul", "Level select", "Invulnerable",
                         "The game's debug option. Applies on Start below.",
                         CHOICE, 0, 2, 1, kInvulNames, 3),
    [R_START] = MENU_ROW("tlk.start_level", "Level select", s_start_label,
                         "Press Enter/A: restarts the game at that level.", ACTION, 0, 0, 0, NULL, 0),
    [R_PRACTICE] = MENU_ROW("tlk.practice", "Practice", "Practice mode",
                            "Timer, lag, inputs, best times. F9 restarts.",
                            BOOL, 0, 1, 1, NULL, 0),
    [R_BIND0 + 0] = BIND_ROW(0),   [R_BIND0 + 1] = BIND_ROW(1),   [R_BIND0 + 2] = BIND_ROW(2),
    [R_BIND0 + 3] = BIND_ROW(3),   [R_BIND0 + 4] = BIND_ROW(4),   [R_BIND0 + 5] = BIND_ROW(5),
    [R_BIND0 + 6] = BIND_ROW(6),   [R_BIND0 + 7] = BIND_ROW(7),   [R_BIND0 + 8] = BIND_ROW(8),
    [R_BIND0 + 9] = BIND_ROW(9),   [R_BIND0 + 10] = BIND_ROW(10), [R_BIND0 + 11] = BIND_ROW(11),
    [R_BIND0 + 12] = BIND_ROW(12),
    [R_RESET] = MENU_ROW("tlk.reset_bindings", "Controls", "Reset defaults",
                         "Default keys and gamepad buttons.", ACTION, 0, 0, 0, NULL, 0),
    [R_RESUME] = MENU_ROW(RECOMP_RUNTIME_UI_KEY_RESUME, "System", "Resume",
                          "Back to the game.", ACTION, 0, 0, 0, NULL, 0),
    [R_QUIT] = MENU_ROW(RECOMP_RUNTIME_UI_KEY_QUIT, "System", "Quit",
                        "Close the game. Press twice.", ACTION, 0, 0, 0, NULL, 0),
    [R_ABOUT] = MENU_ROW("tlk.about", "About", "Why this port exists",
                         "Enter/A to read.", ACTION, 0, 0, 0, NULL, 0),
    [R_AUTHOR] = MENU_ROW("tlk.author", "About", "By Alejandro AR",
                          "kinduff.com", ACTION, 0, 0, 0, NULL, 0),
    [R_SOURCE] = MENU_ROW("tlk.source", "About", "Source code",
                          "github.com/kinduff/TLKRecomp", ACTION, 0, 0, 0, NULL, 0),
};

static void StartLabel(void) {
    snprintf(s_start_label, sizeof s_start_label, "Start %s  [Enter/A]", kSelectNames[s_sel_level]);
}

static void BindLabels(void) {
    for (int i = 0; i < kBindings; i++) {
        if (s_key_wait && i == s_key_sel)
            snprintf(s_bind_label[i], sizeof s_bind_label[i], "%s:  press a key or button", kBindNames[i]);
        else
            snprintf(s_bind_label[i], sizeof s_bind_label[i], "%s:  %s  /  %s", kBindNames[i],
                     SDL_GetKeyName(s_keys[i]), PadName(s_pad_btn[i]));
    }
}

static void ApplyBackdrop(void) {
    recomp_runtime_ui_set_backdrop(s_ui, s_menu_dim / 100.0f, s_menu_opacity / 100.0f);
}

static void SetAudio(int on) {
    s_audio = on;
    if (s_audio && !astream) {
        if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
        SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, s_audio_rate };
        astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (astream) SDL_ResumeAudioStreamDevice(astream);
        else { fprintf(stderr, "audio: %s\n", SDL_GetError()); s_audio = 0; }
    } else if (!s_audio && astream) { SDL_DestroyAudioStream(astream); astream = NULL; }
}

static int MenuGet(void *ctx, const RecompRuntimeUiItem *item, int *v) {
    (void)ctx;
    switch ((int)(item - s_rows)) {
    case R_SCALE: *v = s_scale; break;
    case R_FULLSCREEN: *v = s_fullscreen; break;
    case R_VSYNC: *v = s_vsync; break;
    case R_ASPECT: *v = SnesDisplayAspect_Clamp(s_aspect); break;
    case R_DIM: *v = s_menu_dim; break;
    case R_OPACITY: *v = s_menu_opacity; break;
    case R_SHADER: { int sh = ShaderIndex(); *v = sh >= 0 ? sh : kShaderCount; break; }
    case R_HD_MODE7: *v = s_hd_mode7 ? s_hd_mode7 - 1 : 0; break;
    case R_WIDE: *v = s_wide; break;
    case R_WIDE_HUD: *v = s_wide_hud; break;
    case R_WIDE_SPAWN: *v = s_wide_spawn; break;
    case R_SMOOTH: *v = s_smooth; break;
    case R_INTEGER: *v = s_integer; break;
    case R_AUDIO: *v = s_audio; break;
    case R_VOLUME: *v = s_volume; break;
    case R_PRACTICE: *v = s_practice; break;
    case R_LEVEL: *v = s_sel_level; break;
    case R_INVUL: *v = s_sel_invul; break;
    default: return 0;
    }
    return 1;
}

static int MenuSet(void *ctx, const RecompRuntimeUiItem *item, int v) {
    (void)ctx;
    int r = (int)(item - s_rows);
    switch (r) {
    case R_SCALE: s_scale = v; SizeWindow(); break;
    case R_FULLSCREEN: s_fullscreen = v; SDL_SetWindowFullscreen(win, s_fullscreen); break;
    case R_VSYNC: s_vsync = v; SetVsync(); break;
    case R_ASPECT: s_aspect = v; ApplyVideoConfig(); SizeWindow(); break;
    case R_DIM: s_menu_dim = v; ApplyBackdrop(); break;
    case R_OPACITY: s_menu_opacity = v; ApplyBackdrop(); break;
    case R_SHADER:
        snprintf(s_shader, sizeof s_shader, "%s", v < kShaderCount ? kShaders[v][0] : s_shader_custom);
        ApplyVideoConfig();
        if (s_gl.Reconfigure) s_gl.Reconfigure();
        break;
    case R_HD_MODE7: s_hd_mode7 = v ? v + 1 : 0; ApplyHdMode7(); break;
    case R_WIDE: s_wide = v; TlkWideEnable(s_wide != 0); SizeWindow(); break;
    case R_WIDE_HUD: s_wide_hud = v; break;
    case R_WIDE_SPAWN: s_wide_spawn = v; break;
    case R_SMOOTH: s_smooth = v; ApplyVideoConfig(); break;
    case R_INTEGER: s_integer = v; break;
    case R_AUDIO: SetAudio(v); return s_audio == v;
    case R_VOLUME: s_volume = v; break;
    case R_PRACTICE:
        s_practice = v;
        if (s_practice) PrEnable();
        break;
    case R_LEVEL: case R_INVUL:   /* not in the ini; 0 keeps this hint in place of "Saved" */
        if (r == R_LEVEL) s_sel_level = v;
        else s_sel_invul = v;
        StartLabel();
        recomp_runtime_ui_set_status(s_ui, "Now Enter/A on Start");
        return 0;
    default: return 0;
    }
    return 1;
}

static int MenuEnabled(void *ctx, const RecompRuntimeUiItem *item) {
    (void)ctx;
    switch ((int)(item - s_rows)) {
    case R_SHADER: return s_gl_on;
    case R_ABOUT: return s_menu_gl;
    default: return 1;
    }
}

static void MenuSave(void *ctx) {
    (void)ctx;
    if (!SaveConfig()) recomp_runtime_ui_set_status(s_ui, "Cannot write the ini");
}

static void OpenMenu(void) { recomp_runtime_ui_open(s_ui); }
static void CloseMenu(void) { recomp_runtime_ui_close(s_ui); }

/* The menu opened or closed (by a key, a row, or the menu itself). */
static void MenuShown(void *ctx, int open) {
    (void)ctx;
    s_menu_open = open;
    s_key_wait = 0;
    s_about = 0;
    BindLabels();
    if (open) { s_kbd_pad = 0; s_turbo_held = 0; TlkMenuImguiOpened(); }
    else s_pad_hold = 1;
}

static int MenuRun(void *ctx, const RecompRuntimeUiItem *item) {
    (void)ctx;
    int r = (int)(item - s_rows);
    if (r >= R_BIND0 && r < R_BIND0 + kBindings) {   /* the next key or button press binds */
        s_key_sel = r - R_BIND0;
        s_key_wait = 1;
        BindLabels();
        return 0;
    }
    switch (r) {
    case R_START: PrStartLevel(s_sel_level); return 1;
    case R_ABOUT: s_about = 1; return 0;
    case R_RESET:
        ResetBindings();
        BindLabels();
        MenuSave(NULL);
        return 1;
    case R_RESUME: CloseMenu(); return 1;
    case R_QUIT:
        if (!recomp_runtime_ui_confirm(s_ui, item, "Press again to quit")) return 0;
        running = 0;
        return 1;
    case R_AUTHOR: case R_SOURCE:
        if (SDL_OpenURL(r == R_AUTHOR ? AUTHOR_URL : SOURCE_URL)) return 1;
        recomp_runtime_ui_set_status(s_ui, "Cannot open the browser");
        return 0;
    default: return 0;
    }
}

/* A rebind finished: bind = 1 keeps the new key/button, 0 cancels. */
static void BindDone(int bind) {
    char msg[48];
    s_key_wait = 0;
    BindLabels();
    if (bind) {
        MenuSave(NULL);
        snprintf(msg, sizeof msg, "%.47s", s_bind_label[s_key_sel]);
    } else snprintf(msg, sizeof msg, "Cancelled");
    recomp_runtime_ui_set_status(s_ui, msg);
}

static void MenuCreate(void) {
    for (int i = 0; i < kShaderCount; i++) s_shader_names[i] = kShaders[i][1];
    if (s_shader[0] && ShaderIndex() < 0) {   /* keep the ini's own shader as a choice */
        snprintf(s_shader_custom, sizeof s_shader_custom, "%s", s_shader);
        s_shader_names[kShaderCount] = "custom";
        s_rows[R_SHADER].maximum = kShaderCount;
        s_rows[R_SHADER].choice_count = kShaderCount + 1;
    }
    BindLabels();
    StartLabel();
    RecompRuntimeUiConfig c;
    memset(&c, 0, sizeof c);
    c.title = "The Lion King";
    c.subtitle = "Settings";
    c.items = s_rows;
    c.item_count = R_COUNT;
    c.callbacks.get_value = MenuGet;
    c.callbacks.set_value = MenuSet;
    c.callbacks.run_action = MenuRun;
    c.callbacks.is_enabled = MenuEnabled;
    c.callbacks.save = MenuSave;
    c.callbacks.visibility_changed = MenuShown;
    c.theme = "snes";
    c.accept_label = "Enter/A";
    c.back_label = "Esc/B";
    s_ui = recomp_runtime_ui_create(&c);
    ApplyBackdrop();
}

/* Opens the menu on a section by its place in the list (--menu-page). */
static void MenuOpenSection(int section) {
    OpenMenu();
    for (int i = 0; i < section; i++) recomp_runtime_ui_handle_input(s_ui, RECOMP_RUNTIME_UI_INPUT_DOWN, 1, 0);
    recomp_runtime_ui_handle_input(s_ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
}
enum { kSectionLevel = 3, kSectionControls = 5, kSectionAbout = 7 };   /* Display, Graphics, Audio, Level select, Practice, Controls, System, About */

/* Load a savestate during play (F5-F8, F9). */
static int LoadStateLive(const char *path) {
    uint32_t hf = 0;
    if (!tlk_state_load(path, &hf)) return 0;
    TlkWideReset();
    if (input_path && !s_polled) frame = (long)hf;
    RecStop("a savestate was loaded (the recording replays only up to here)");
    audio_acc = 0;
    if (astream) SDL_ClearAudioStream(astream);
    return 1;
}

static void HandleKey(const SDL_KeyboardEvent *ke) {
    int down = ke->down;
    SDL_Keycode k = ke->key;
    if (s_menu_open) {
        if (!down) return;
        if (s_key_wait) {          /* rebinding: next key wins, Esc cancels */
            if (k != SDLK_ESCAPE) s_keys[s_key_sel] = k;
            BindDone(k != SDLK_ESCAPE);
            return;
        }
        if (s_about && k != SDLK_TAB) {   /* Tab still closes the whole menu */
            if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE || k == SDLK_RETURN || k == SDLK_KP_ENTER) s_about = 0;
            return;
        }
        RecompRuntimeUiInput in;
        if (k == SDLK_TAB) in = RECOMP_RUNTIME_UI_INPUT_TOGGLE;
        else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) in = RECOMP_RUNTIME_UI_INPUT_BACK;
        else if (k == SDLK_UP) in = RECOMP_RUNTIME_UI_INPUT_UP;
        else if (k == SDLK_DOWN) in = RECOMP_RUNTIME_UI_INPUT_DOWN;
        else if (k == SDLK_LEFT) in = RECOMP_RUNTIME_UI_INPUT_LEFT;
        else if (k == SDLK_RIGHT) in = RECOMP_RUNTIME_UI_INPUT_RIGHT;
        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) in = RECOMP_RUNTIME_UI_INPUT_ACCEPT;
        else return;
        recomp_runtime_ui_handle_input(s_ui, in, 1, ke->repeat);
        return;
    }
    if (down && !ke->repeat) {
        if (k == SDLK_ESCAPE || k == SDLK_TAB) { OpenMenu(); return; }
        if (k == SDLK_F11 || (k == SDLK_RETURN && (ke->mod & SDL_KMOD_ALT))) {
            s_fullscreen = !s_fullscreen;
            SDL_SetWindowFullscreen(win, s_fullscreen);
            return;
        }
        if (k == SDLK_F9 && s_practice) {
            char path[1100];
            PrRestartPath(path, sizeof path);
            if (LoadStateLive(path)) { PrAfterLoad(1); Toast("Restart level"); }
            else Toast("No level start yet");
            return;
        }
        int fn = k >= SDLK_F1 && k <= SDLK_F8 ? (int)(k - SDLK_F1) : -1;
        if (fn >= 0) {
            int load = fn >= 4 || (ke->mod & SDL_KMOD_SHIFT);
            int slot = fn % 4 + 1;
            char path[1100], msg[64];
            SlotPath(slot, path, sizeof path);
            if (load) {
                if (LoadStateLive(path)) {
                    if (s_practice) PrAfterLoad(0);
                    snprintf(msg, sizeof msg, "Loaded slot %d", slot);
                } else snprintf(msg, sizeof msg, "Slot %d: no state", slot);
            } else
                snprintf(msg, sizeof msg, tlk_state_save(path, (uint32_t)frame) ? "Saved slot %d" : "Slot %d: save failed", slot);
            Toast(msg);
            return;
        }
    }
    if (k == SDLK_RETURN && (ke->mod & SDL_KMOD_ALT)) return;
    if (k == s_keys[kTurbo]) s_turbo_held = down;
    for (int i = 0; i < kSnesButtons; i++)
        if (s_keys[i] == k) {
            if (down) s_kbd_pad |= (uint16_t)(1u << i);
            else s_kbd_pad &= (uint16_t)~(1u << i);
        }
}

/* Gamepad buttons drive the menu: Guide (or Back+Start) opens it; in it the
 * d-pad moves and changes values, South (A) selects, East (B) goes back,
 * Start or Guide closes. */
static void HandlePadButton(const SDL_GamepadButtonEvent *ge) {
    if (!ge->down) return;
    SDL_GamepadButton b = (SDL_GamepadButton)ge->button;
    if (!s_menu_open) {
        int chord = (b == SDL_GAMEPAD_BUTTON_START && s_pad && SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_BACK)) ||
                    (b == SDL_GAMEPAD_BUTTON_BACK && s_pad && SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_START));
        if (b == SDL_GAMEPAD_BUTTON_GUIDE || chord) OpenMenu();
        return;
    }
    if (s_key_wait) {   /* rebinding: next gamepad button wins, Guide cancels */
        if (b != SDL_GAMEPAD_BUTTON_GUIDE) s_pad_btn[s_key_sel] = b;
        BindDone(b != SDL_GAMEPAD_BUTTON_GUIDE);
        return;
    }
    if (s_about && b != SDL_GAMEPAD_BUTTON_START && b != SDL_GAMEPAD_BUTTON_GUIDE) {
        if (b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_SOUTH) s_about = 0;
        return;
    }
    RecompRuntimeUiInput in;
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: in = RECOMP_RUNTIME_UI_INPUT_UP; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: in = RECOMP_RUNTIME_UI_INPUT_DOWN; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: in = RECOMP_RUNTIME_UI_INPUT_LEFT; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: in = RECOMP_RUNTIME_UI_INPUT_RIGHT; break;
    case SDL_GAMEPAD_BUTTON_SOUTH: in = RECOMP_RUNTIME_UI_INPUT_ACCEPT; break;
    case SDL_GAMEPAD_BUTTON_EAST: in = RECOMP_RUNTIME_UI_INPUT_BACK; break;
    case SDL_GAMEPAD_BUTTON_START:
    case SDL_GAMEPAD_BUTTON_GUIDE: in = RECOMP_RUNTIME_UI_INPUT_TOGGLE; break;
    default: return;
    }
    recomp_runtime_ui_handle_input(s_ui, in, 1, 0);
}

static void HandleEvent(const SDL_Event *ev) {
    if (s_menu_open) TlkMenuImguiEvent(ev);   /* mouse and window state for ImGui */
    switch (ev->type) {
    case SDL_EVENT_QUIT: running = 0; break;
    case SDL_EVENT_GAMEPAD_ADDED: OpenFirstGamepad(); break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (s_pad && ev->gdevice.which == SDL_GetGamepadID(s_pad)) {
            SDL_CloseGamepad(s_pad); s_pad = NULL;
            Toast("Gamepad disconnected");
            OpenFirstGamepad();
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: HandlePadButton(&ev->gbutton); break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: HandleKey(&ev->key); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: s_kbd_pad = 0; s_turbo_held = 0; break;
    default: break;
    }
}

/* ── --selftest-remap: drive the menu with synthetic SDL events ─────────── */
static int s_st_fail;
static void StCheck(int ok, const char *what) {
    printf("remap_selftest: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) s_st_fail++;
}
static void StPump(void) {
    SDL_Event ev;
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&ev)) HandleEvent(&ev);
}
static void StKey(SDL_Keycode k, int down) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.key = k;
    ev.key.down = down != 0;
    SDL_PushEvent(&ev);
    StPump();
}
static void StTap(SDL_Keycode k) { StKey(k, 1); StKey(k, 0); }
static void StPadEvent(SDL_GamepadButton b) {   /* button event without a device */
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    ev.gbutton.button = (Uint8)b;
    ev.gbutton.down = true;
    SDL_PushEvent(&ev);
    StPump();
}

static int RemapSelfTest(void) {
    /* Save/reload goes to a scratch ini so the player's settings are never touched. */
    static char st_ini[512];
    const char *tmp = getenv("TMPDIR");
#ifdef _WIN32
    if (!tmp || !*tmp) tmp = getenv("TEMP");   /* no /tmp on Windows */
#endif
    snprintf(st_ini, sizeof st_ini, "%s/tlk_sdl_selftest.ini", tmp && *tmp ? tmp : "/tmp");
    s_cfg_path = st_ini;
    StCheck(!s_menu_open, "menu starts closed");
    StKey(SDLK_Z, 1);
    StCheck(s_kbd_pad == 1u, "default Z presses B");
    StKey(SDLK_Z, 0);
    StTap(SDLK_TAB);
    StCheck(s_menu_open, "Tab opens the settings menu");
    for (int i = 0; i < kSectionControls; i++) StTap(SDLK_DOWN);
    StTap(SDLK_RETURN);
    StTap(SDLK_RETURN);
    StCheck(s_key_wait && s_key_sel == 0, "Controls opens on B, Enter waits for a key");
    StTap(SDLK_K);
    StCheck(!s_key_wait && s_keys[0] == SDLK_K, "K is now bound to B");
    StTap(SDLK_RETURN);
    StTap(SDLK_ESCAPE);
    StCheck(!s_key_wait && s_keys[0] == SDLK_K && s_menu_open, "Esc cancels a rebind and stays in the menu");

    /* Gamepad navigation and binding with button events. */
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    StPadEvent(SDL_GAMEPAD_BUTTON_SOUTH);
    StCheck(s_key_wait && s_key_sel == 1, "pad d-pad down + South rebinds Y");
    StPadEvent(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    StCheck(!s_key_wait && s_pad_btn[1] == SDL_GAMEPAD_BUTTON_LEFT_STICK, "pad L3 is now bound to Y");

    /* Every rebind is saved; Reset defaults is saved too. */
    ResetBindings();
    LoadConfig(s_cfg_path);
    StCheck(s_keys[0] == SDLK_K && s_pad_btn[1] == SDL_GAMEPAD_BUTTON_LEFT_STICK, "rebinds were saved to the ini");
    for (int i = 1; i < kBindings; i++) StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    StPadEvent(SDL_GAMEPAD_BUTTON_SOUTH);
    StCheck(s_keys[0] == SDLK_Z && s_pad_btn[1] == SDL_GAMEPAD_BUTTON_WEST, "Reset defaults restores Z / West");
    LoadConfig(s_cfg_path);
    StCheck(s_keys[0] == SDLK_Z && s_pad_btn[1] == SDL_GAMEPAD_BUTTON_WEST, "the reset was saved");
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_DOWN);   /* wraps to B */
    StPadEvent(SDL_GAMEPAD_BUTTON_SOUTH);
    StTap(SDLK_K);
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    StPadEvent(SDL_GAMEPAD_BUTTON_SOUTH);
    StPadEvent(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    StCheck(s_keys[0] == SDLK_K && s_pad_btn[1] == SDL_GAMEPAD_BUTTON_LEFT_STICK,
            "down from Reset wraps to B; K and L3 bound again");
    StPadEvent(SDL_GAMEPAD_BUTTON_EAST);
    StCheck(s_menu_open && !s_key_wait, "pad East leaves Controls, menu stays open");

    /* Level select section, two up from Controls: works with practice off
     * (off here whatever the player's ini says). */
    s_practice = 0;
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_UP);
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_UP);
    StPadEvent(SDL_GAMEPAD_BUTTON_SOUTH);
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    StCheck(!s_practice && s_sel_level == 13, "Left on Level wraps to the last level, practice off");
    StCheck(strstr(s_start_label, "Bug Hunt 3") != NULL, "the Start row names the level");
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    StCheck(s_sel_level == 0 && s_sel_invul == 2, "Left on Invulnerable wraps to infinite lives");
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    StCheck(s_sel_invul == 0, "Right wraps it back to off");
    StPadEvent(SDL_GAMEPAD_BUTTON_EAST);

    /* Practice section, one up from Controls. */
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    StPadEvent(SDL_GAMEPAD_BUTTON_SOUTH);
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    StCheck(s_practice, "Right turns practice mode on");
    s_practice = 0;
    LoadConfig(s_cfg_path);
    StCheck(s_practice, "practice mode was saved to the ini");
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    StCheck(!s_practice, "Left turns it off again");
    Present();   /* draws the menu once (the dummy driver has no OpenGL: the 256x224 path) */
    StPadEvent(SDL_GAMEPAD_BUTTON_START);
    StCheck(!s_menu_open, "pad Start closes the menu");
    StKey(SDLK_K, 1);
    StCheck(s_kbd_pad == 1u, "K now presses B in game");
    StKey(SDLK_K, 0);
    StKey(SDLK_Z, 1);
    StCheck(s_kbd_pad == 0, "Z no longer presses B");
    StKey(SDLK_Z, 0);

    /* Turbo: a binding on the Controls page, not a pad bit. */
    StKey(SDLK_GRAVE, 1);
    StCheck(TurboHeld() && s_kbd_pad == 0, "default ` holds turbo and presses no button");
    StKey(SDLK_GRAVE, 0);
    StCheck(!TurboHeld(), "releasing ` ends turbo");
    StTap(SDLK_TAB);
    StCheck(s_menu_open, "Tab reopens the menu");
    StTap(SDLK_DOWN);   /* the menu reopens on the last section, Practice */
    StTap(SDLK_RETURN);
    StTap(SDLK_UP);
    StTap(SDLK_UP);
    StTap(SDLK_RETURN);
    StCheck(s_key_wait && s_key_sel == kTurbo, "Up from B wraps past Reset to the turbo row");
    StTap(SDLK_T);
    StCheck(!s_key_wait && s_keys[kTurbo] == SDLK_T, "T is now bound to turbo");
    StTap(SDLK_TAB);
    StKey(SDLK_T, 1);
    StCheck(TurboHeld(), "T holds turbo in game");
    StKey(SDLK_T, 0);
    ResetBindings();
    StCheck(s_keys[kTurbo] == SDLK_GRAVE && s_pad_btn[kTurbo] == SDL_GAMEPAD_BUTTON_RIGHT_STICK,
            "defaults: turbo ` / R3");
    LoadConfig(s_cfg_path);
    StCheck(s_keys[kTurbo] == SDLK_T && s_keys[0] == SDLK_K, "ini reload restores turbo T");

    /* Esc backs out and closes; Quit takes two presses. */
    StTap(SDLK_TAB);
    StTap(SDLK_ESCAPE);
    StCheck(!s_menu_open && running, "Esc closes the menu (no longer quits)");
    StTap(SDLK_TAB);
    StTap(SDLK_DOWN);   /* Controls -> System */
    StTap(SDLK_RETURN);
    StTap(SDLK_DOWN);
    StTap(SDLK_RETURN);
    StCheck(running && s_menu_open, "first press on Quit only asks");
    StTap(SDLK_RETURN);
    StCheck(!running, "second press quits");
    running = 1;
    StTap(SDLK_ESCAPE);
    StTap(SDLK_ESCAPE);
    StCheck(!s_menu_open, "Esc twice backs out of System and closes");

    /* Hotplug and live state with an SDL virtual gamepad. */
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.name = "tlk virtual pad";
    SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    StCheck(vid != 0, "virtual gamepad attached");
    StPump();
    StCheck(s_pad != NULL, "hotplug event opened it");
    if (s_pad) {
        SDL_Joystick *js = SDL_GetGamepadJoystick(s_pad);
        s_pad_hold = 0;
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
        StPump();
        StCheck(GamepadState() == (1u << 1), "virtual L3 reads as Y");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_LEFT_STICK, false);
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_WEST, true);
        StPump();
        StCheck(GamepadState() == 0, "virtual West (old Y) reads nothing");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_WEST, false);
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_EAST, true);
        StPump();
        StCheck(GamepadState() == (1u << 8), "virtual East reads as A (default)");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_EAST, false);
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
        StPump();
        StCheck(TurboHeld() && GamepadState() == 0, "virtual R3 holds turbo and presses no button");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
        StPump();
        StCheck(!TurboHeld(), "releasing R3 ends turbo");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_GUIDE, true);
        StPump();
        StCheck(s_menu_open, "virtual Guide opens the menu");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_GUIDE, false);
        StPump();
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, true);
        StPump();
        StCheck(!s_menu_open, "virtual Start closes it");
        StCheck(GamepadState() == 0, "Start still held after closing is not sent to the game");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, false);
        StPump();
        GamepadState();
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, true);
        StPump();
        StCheck(GamepadState() == (1u << 3), "after release, Start reaches the game again");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, false);
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_BACK, true);
        StPump();
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, true);
        StPump();
        StCheck(s_menu_open, "virtual Back+Start opens the menu");
        SDL_DetachVirtualJoystick(vid);
        StPump();
        StCheck(s_pad == NULL, "unplug closes it");
    }
    ResetBindings();
    printf("remap_selftest: %s (%d failed)\n", s_st_fail ? "FAIL" : "PASS", s_st_fail);
    return s_st_fail ? 1 : 0;
}

/* ── WAV recorder ───────────────────────────────────────────────────────── */
static FILE *s_wav;
static uint32_t s_wav_frames, s_wav_limit;
static void Le32(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, f); }
static void Le16(FILE *f, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; fwrite(b, 1, 2, f); }
static void WavHeader(FILE *f, int rate, uint32_t frames) {
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); Le32(f, 36 + frames * 4); fwrite("WAVEfmt ", 1, 8, f);
    Le32(f, 16); Le16(f, 1); Le16(f, 2); Le32(f, (uint32_t)rate); Le32(f, (uint32_t)rate * 4);
    Le16(f, 4); Le16(f, 16); fwrite("data", 1, 4, f); Le32(f, frames * 4);
}

static void Usage(void) {
    fprintf(stderr,
        "usage: tlk_sdl --rom <path.sfc> [--config ini] [--scale N] [--fullscreen]\n"
        "               [--input tape | --input-polled tape | --input-raw rec] [--no-record] [--lle] [--interp-targets f]\n"
        "               [--frames N] [--hash-at a,b,..] [--dump-png path] [--no-pace]\n"
        "               [--no-audio] [--wav path [--wav-seconds S]]\n"
        "               [--save-at N state] [--load state] [--load-at N state]\n"
        "               [--menu-at N [--menu-page controls|level|about]] [--selftest-remap]\n"
        "               [--speedrun off|practice] [--practice-level N [--invul N]] [--dump-window path]\n"
        "               [--list-drivers]\n"
        "Without --rom (or rom= in the ini) the .sfc/.smc files next to the binary and in the\n"
        "current directory are tried. Needs The Lion King (USA), CRC32 C8FBFAA8.\n");
}

/* Probe by creating a file: access(W_OK) is unreliable on Windows. */
static int CwdWritable(void) {
    FILE *f = fopen(".tlk_write_probe", "wb");
    if (!f) return 0;
    fclose(f);
    remove(".tlk_write_probe");
    return 1;
}

/* No --rom: try every .sfc/.smc next to the binary, then next to the ini
 * (beside the .AppImage, or the per-user folder), then in the current
 * directory, and take the first that verifies. Prints what was rejected. */
static uint8_t *FindRom(size_t *size, char *found, size_t found_n) {
    const char *base = SDL_GetBasePath();
    char *cwd = SDL_GetCurrentDirectory();
    char ini_dir[1100] = "";
    const char *sl = LastSep(s_cfg_path);
    if (sl) snprintf(ini_dir, sizeof ini_dir, "%.*s/", (int)(sl - s_cfg_path), s_cfg_path);
    const char *dirs[3] = { base ? base : "./", ini_dir, "./" };
    const char *real[3] = { dirs[0], ini_dir, cwd ? cwd : "./" };  /* to skip repeats */
    int tried = 0;
    for (int d = 0; d < 3; d++) {
        int repeat = !real[d][0];
        for (int e = 0; e < d && !repeat; e++) repeat = !strcmp(real[d], real[e]);
        if (repeat) continue;
        for (int pat = 0; pat < 2; pat++) {
            int n = 0;
            char **names = SDL_GlobDirectory(dirs[d], pat ? "*.smc" : "*.sfc", SDL_GLOB_CASEINSENSITIVE, &n);
            for (int i = 0; names && i < n; i++) {
                char path[2048], err[256];
                snprintf(path, sizeof path, "%s%s%s", dirs[d],
                         LastSep(dirs[d]) == dirs[d] + strlen(dirs[d]) - 1 ? "" : "/", names[i]);
                tried++;
                uint8_t *rom = TlkReadVerifiedRom(path, size, err, sizeof err);
                if (rom) {
                    snprintf(found, found_n, "%s", path);
                    SDL_free(names);
                    SDL_free(cwd);
                    return rom;
                }
                uint32_t crc = 0;
                FILE *f = fopen(path, "rb");
                if (f) {
                    uint8_t buf[65536];
                    size_t r;
                    crc = 0xffffffffu;
                    while ((r = fread(buf, 1, sizeof buf, f)) > 0) crc = CrcUpdate(crc, buf, r);
                    crc ^= 0xffffffffu;
                    fclose(f);
                }
                fprintf(stderr, "skipping %s: CRC32 %08X, not The Lion King (USA)\n", path, crc);
            }
            SDL_free(names);
        }
    }
    SDL_free(cwd);
    fprintf(stderr,
        "No ROM given and no matching ROM found (%d candidate%s checked) next to the binary\n"
        "(%s), next to the ini or in the current directory.\n"
        "Run: tlk_sdl --rom \"/path/to/Lion King, The (USA).sfc\"\n"
        "The ROM is not included. It must be The Lion King (USA), CRC32 C8FBFAA8\n"
        "(headered copies are accepted).\n", tried, tried == 1 ? "" : "s", dirs[0]);
    return NULL;
}

/* No ROM found: ask with the system file dialog (portal/zenity on Linux,
 * native on Windows and macOS), verify it, and remember it as rom= in the
 * ini. A wrong file offers another try. Returns NULL on cancel or when no
 * dialog is available (headless, no portal). */
typedef struct { SDL_AtomicInt done; char path[2048]; } PickState;

static void SDLCALL PickDone(void *ud, const char *const *files, int filter) {
    PickState *st = ud;
    (void)filter;
    st->path[0] = 0;
    if (!files) fprintf(stderr, "file dialog: %s\n", SDL_GetError());
    else if (files[0]) snprintf(st->path, sizeof st->path, "%s", files[0]);
    SDL_SetAtomicInt(&st->done, 1);
}

static uint8_t *PickRom(size_t *size, char *found, size_t found_n) {
    static const SDL_DialogFileFilter kFilters[] = {
        { "SNES ROM (*.sfc, *.smc)", "sfc;smc" }, { "All files", "*" } };
    const char *msg =
        "The game data is not included. Choose your own copy of\n"
        "The Lion King (USA) ROM (.sfc or .smc).";
    for (;;) {
        const SDL_MessageBoxButtonData pick[] = {
            { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose ROM..." },
            { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" } };
        const SDL_MessageBoxData box = { SDL_MESSAGEBOX_INFORMATION, NULL, "The Lion King",
                                         msg, 2, pick, NULL };
        int button = 1;
        if (SDL_ShowMessageBox(&box, &button) && button != 1) return NULL;
        PickState st;
        SDL_SetAtomicInt(&st.done, 0);
        SDL_ShowOpenFileDialog(PickDone, &st, NULL, kFilters, 2, NULL, false);
        while (!SDL_GetAtomicInt(&st.done)) {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        if (!st.path[0]) return NULL;
        char err[256];
        uint8_t *rom = TlkReadVerifiedRom(st.path, size, err, sizeof err);
        if (rom) {
            snprintf(found, found_n, "%s", st.path);
            return rom;
        }
        fprintf(stderr, "%s: %s\n", err, st.path);
        msg = "That file is not The Lion King (USA) ROM (CRC32 C8FBFAA8).\n"
              "Other regions, revisions and hacks are not supported.";
    }
}

/* What is shown: with OpenGL the whole window as composited (shader and menu
 * included, at window size), else the 256x224 picture plus overlays. */
static void DumpWindow(const char *path) {
    int ok;
    if (s_gl_on || ren) {
        s_dump_req = path;
        Present();
        s_dump_req = NULL;
        ok = s_dump_ok;
    } else ok = WritePng(path, (const uint8_t *)s_screen, kWidth, kHeight, kWidth * 4);
    if (ok) printf("dump_window=%s\n", path);
    else fprintf(stderr, "dump-window: cannot write %s\n", path);
}

static void PrintHash(const uint8_t *data, size_t n) {
    uint8_t h[32];
    sha256_compute(data, n, h);
    for (int i = 0; i < 32; i++) printf("%02x", h[i]);
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *dump_png = NULL, *wav_path = NULL;
    const char *hash_at = NULL;
    const char *cfg_path = getenv("TLK_SDL_INI") ? getenv("TLK_SDL_INI") : "tlk_sdl.ini";
    const char *save_path = NULL, *load_path = NULL, *load_at_path = NULL;
    long frames = 0, save_at = -1, load_at = -1, menu_at = -1;
    int practice_level = -1, menu_shown = 0;
    const char *dump_window = NULL;
    int cli_scale = 0, cli_fs = 0, no_audio = 0, no_record = 0;
    const char *record_path = NULL;
    double wav_seconds = 10.0;

    int selftest = 0, menu_page = -1;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--list-drivers")) {
            int v = SDL_GetVersion();
            printf("SDL %d.%d.%d (%s)\nvideo:", SDL_VERSIONNUM_MAJOR(v),
                   SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), SDL_GetRevision());
            for (int d = 0; d < SDL_GetNumVideoDrivers(); d++) printf(" %s", SDL_GetVideoDriver(d));
            printf("\naudio:");
            for (int d = 0; d < SDL_GetNumAudioDrivers(); d++) printf(" %s", SDL_GetAudioDriver(d));
            printf("\n");
            return 0;
        }
    ResetBindings();
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg_path = argv[i + 1];
    /* A .app started from Finder runs in /: no ini (or saves) can go there,
     * so they go to the per-user folder (~/Library/Application Support/TLKRecomp). */
    static char pref_ini[1100];
    if (!strcmp(cfg_path, "tlk_sdl.ini") && !getenv("TLK_SDL_INI") && !CwdWritable()) {
        char *pref = SDL_GetPrefPath("", "TLKRecomp");
        if (pref) {
            snprintf(pref_ini, sizeof pref_ini, "%stlk_sdl.ini", pref);
            cfg_path = pref_ini;
            SDL_free(pref);
        }
    }
    LoadConfig(cfg_path);
    s_cfg_path = cfg_path;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom") && v) { rom_path = v; i++; }
        else if (!strcmp(a, "--config") && v) i++;
        else if (!strcmp(a, "--input") && v) { input_path = v; i++; }
        else if (!strcmp(a, "--input-polled") && v) { input_path = v; s_polled = 1; i++; }
        else if (!strcmp(a, "--input-raw") && v) { input_path = v; s_raw_tape = 1; i++; }
        else if (!strcmp(a, "--no-record")) no_record = 1;
        else if (!strcmp(a, "--record") && v) { record_path = v; i++; }
        else if (!strcmp(a, "--lle")) setenv("SNESRECOMP_LLE_BOUNCE", "0", 1);
        else if (!strcmp(a, "--interp-targets") && v) { setenv("SNESRECOMP_LLE_INTERP_TARGET_FILE", v, 1); i++; }
        else if (!strcmp(a, "--frames") && v) { frames = strtol(v, NULL, 10); i++; }
        else if (!strcmp(a, "--hash-at") && v) { hash_at = v; i++; }
        else if (!strcmp(a, "--dump-png") && v) { dump_png = v; i++; }
        else if (!strcmp(a, "--wav") && v) { wav_path = v; i++; }
        else if (!strcmp(a, "--wav-seconds") && v) { wav_seconds = atof(v); i++; }
        else if (!strcmp(a, "--scale") && v) { cli_scale = atoi(v); i++; }
        else if (!strcmp(a, "--fullscreen")) cli_fs = 1;
        else if (!strcmp(a, "--no-pace")) pace = 0;
        else if (!strcmp(a, "--no-audio")) no_audio = 1;
        else if (!strcmp(a, "--save-at") && v && i + 2 < argc) { save_at = atol(v); save_path = argv[i + 2]; i += 2; }
        else if (!strcmp(a, "--load-at") && v && i + 2 < argc) { load_at = atol(v); load_at_path = argv[i + 2]; i += 2; }
        else if (!strcmp(a, "--load") && v) { load_path = v; i++; }
        else if (!strcmp(a, "--menu-at") && v) { menu_at = atol(v); i++; }
        else if (!strcmp(a, "--menu-page") && v) {
            menu_page = !strcmp(v, "controls") || !strcmp(v, "keys") ? kSectionControls
                      : !strcmp(v, "level") ? kSectionLevel
                      : !strcmp(v, "about") ? kSectionAbout : -1;
            i++;
        }
        else if (!strcmp(a, "--selftest-remap")) selftest = 1;
        else if (!strcmp(a, "--speedrun") && v) { s_practice = !strcmp(v, "practice"); i++; }
        else if (!strcmp(a, "--practice-level") && v) { practice_level = atoi(v); i++; }
        else if (!strcmp(a, "--invul") && v) { s_sel_invul = atoi(v); if (s_sel_invul < 0 || s_sel_invul > 2) s_sel_invul = 0; i++; }
        else if (!strcmp(a, "--dump-window") && v) { dump_window = v; i++; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { Usage(); return 0; }
        else { Usage(); return 2; }
    }
    if (!rom_path && s_rom_cfg[0]) rom_path = s_rom_cfg;
    if (cli_scale > 0) s_scale = cli_scale;
    if (s_scale < 1 || s_scale > 16) s_scale = 3;
    if (cli_fs) s_fullscreen = 1;
    if (no_audio) s_audio = 0;
    if (s_volume < 0) s_volume = 0;
    if (s_volume > 100) s_volume = 100;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 3;
    }
    /* No sound library or device (a sandbox, a bare system) is not fatal: the
     * game runs silent. */
    int audio_ok = !s_audio || SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!audio_ok) fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError());

    size_t rom_size = 0;
    char err[256], found[2048];
    CrcInit();
    uint8_t *rom = NULL;
    /* The picker is for players: a scripted run (--frames, a tape, the self
     * test) or an explicit --rom fails with a message instead. */
    int interactive = !frames && !input_path && !selftest;
    if (rom_path) {
        rom = TlkReadVerifiedRom(rom_path, &rom_size, err, sizeof err);
        if (!rom) {
            fprintf(stderr, "%s: %s\nNeeds The Lion King (USA), CRC32 C8FBFAA8.\n", err, rom_path);
            if (rom_path != s_rom_cfg || !interactive) return 2;
        }
    }
    if (!rom) {
        rom = FindRom(&rom_size, found, sizeof found);
        if (!rom && interactive) {
            rom = PickRom(&rom_size, found, sizeof found);
            if (rom) {
                snprintf(s_rom_cfg, sizeof s_rom_cfg, "%s", found);
                if (SaveConfig()) fprintf(stderr, "rom saved to %s\n", s_cfg_path);
            }
        }
        if (!rom) return 2;
        fprintf(stderr, "rom: %s\n", found);
    }
    if (input_path) {
        if (!LoadTape(input_path)) { fprintf(stderr, "cannot load tape %s\n", input_path); return 2; }
        fprintf(stderr, "input: %zu %s from %s\n", s_tape_frames, s_polled ? "polls" : "frames", input_path);
    }
    if (!OpenVideo()) return 3;
    MenuCreate();

    astream = NULL;
    /* The output rate is fixed for the session so turning audio on/off in the
     * menu does not change emulation (RtlRenderAudio runs either way below). */
    RtlSetAudioOutputRate(s_audio_rate);
    if (s_audio && audio_ok) {
        SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, s_audio_rate };
        astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!astream) fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError());
        else SDL_ResumeAudioStreamDevice(astream);
    }
    if (wav_path) {
        s_wav = fopen(wav_path, "wb");
        if (!s_wav) { fprintf(stderr, "cannot write %s\n", wav_path); return 7; }
        WavHeader(s_wav, s_audio_rate, 0);
        s_wav_limit = (uint32_t)(wav_seconds * s_audio_rate);
    }
    OpenFirstGamepad();
    if (selftest) {
        int rc = RemapSelfTest();
        SDL_Quit();
        free(rom);
        return rc;
    }

    RtlRegisterGame(&kTlkGameInfo);
    if (!SnesInit(rom, (int)rom_size)) { fprintf(stderr, "snesrecomp rejected the ROM\n"); return 4; }
    ApplyHdMode7();
    TlkWideSetRom(rom, rom_size);
    TlkWideEnable(s_wide != 0);   /* after SnesInit: the interp bridge hook table is runtime state */
    if (s_polled) {
        interp_bridge_add_pre_opcode_hook(0xC0A07C, PollHook);
        interp_bridge_add_pre_opcode_hook(0x00A07C, PollHook);
        g_tlk_aot_pad_read_hook = PollHookCompiled; /* task CC: compiled $C0A05D / $C0A073 */
        pr.hook = 1;
    }
    s_snes_ready = 1;

    frame = 0;
    if (load_path) {
        uint32_t hf = 0;
        if (!tlk_state_load(load_path, &hf)) { fprintf(stderr, "cannot load state %s\n", load_path); return 8; }
        frame = (long)hf;
        printf("loaded_state=%s at_frame=%ld\n", load_path, frame);
    }
    if (!load_path && (interactive || practice_level >= 0)) {   /* power-on state for level select */
        char boot[1100];
        PrBootPath(boot, sizeof boot);
        tlk_state_save(boot, 0);
    }
    if (s_practice) PrEnable();
    /* --frames runs are scripted proof runs: they must not clobber a player's recording. */
    if (record_path || (!input_path && !load_path && !no_record && frames == 0)) {
        char rec[1100];
        const char *sl = LastSep(s_cfg_path);
        if (record_path) snprintf(rec, sizeof rec, "%s", record_path);
        else if (sl) snprintf(rec, sizeof rec, "%.*s/last_session.rec", (int)(sl - s_cfg_path), s_cfg_path);
        else snprintf(rec, sizeof rec, "last_session.rec");
        s_rec = fopen(rec, "wb");
        if (s_rec) fprintf(stderr, "recording input to %s (replay: --input-raw %s)\n", rec, rec);
    }

    const double audio_per_frame = s_audio_rate / NTSC_FPS;
    const int audio_target = (int)(audio_per_frame * 3);  /* ~50 ms device queue */
    audio_acc = 0;
    int16_t abuf[4096 * 2];
    unsigned long underruns = 0, late_resyncs = 0;

    const uint64_t period_ns = (uint64_t)(1e9 / NTSC_FPS + 0.5);
    uint64_t t_start = SDL_GetTicksNS(), deadline = t_start;
    long frame_start = frame;
    running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) HandleEvent(&ev);
        if (!running) break;

        if (s_menu_open) {   /* paused: keep presenting the last frame plus the overlay */
            Present();
            if (dump_window && menu_shown) { DumpWindow(dump_window); running = 0; }   /* --menu-at screenshots */
            SDL_Delay(16);
            deadline = SDL_GetTicksNS();
            continue;
        }
        int macro = s_macro_i < s_macro_n;   /* level select: fast-forward its input */
        uint32_t pad = macro ? s_macro[s_macro_i++]
                     : input_path ? (s_polled ? TapePolled() : TapeFrame(frame))
                                  : (uint32_t)(s_kbd_pad | GamepadState());
        if (s_rec) {
            uint8_t b[2] = { (uint8_t)pad, (uint8_t)(pad >> 8) };
            fwrite(b, 1, 2, s_rec);
            if (frame % 60 == 0) fflush(s_rec);  /* keep it usable after a crash */
        }
        TlkWideSetSpawnLead(s_wide_spawn && WideWidth() ? s_wide_extra : 0);
        RtlRunFrame(pad);
        PpuBeginDrawing(g_ppu, s_pixels, kWidth * kBpp, kPpuRenderFlags_NewRenderer);
        kTlkGameInfo.draw_ppu_frame();
        TlkWideEndFrame(s_pixels);
        frame++;
        if (s_practice) PracticeFrame((uint16_t)pad);
        s_polls_frame = 0;
        if (g_fail) { fprintf(stderr, "runtime failure (g_fail) at frame %ld\n", frame); break; }
        if (save_path && frame == save_at) {
            if (!tlk_state_save(save_path, (uint32_t)frame)) { fprintf(stderr, "cannot save state %s\n", save_path); return 8; }
            printf("saved_state=%s at_frame=%ld\n", save_path, frame);
        }
        if (load_at_path && frame == load_at) {
            uint32_t hf = 0;
            if (!tlk_state_load(load_at_path, &hf)) { fprintf(stderr, "cannot load state %s\n", load_at_path); return 8; }
            TlkWideReset();
            printf("loaded_state=%s at_frame=%ld -> %u\n", load_at_path, frame, hf);
            frame = (long)hf;
            if (s_practice) PrAfterLoad(0);
            load_at_path = NULL;   /* once */
            Toast("Loaded state");
        }
        if (practice_level >= 0) { PrStartLevel(practice_level % 14); practice_level = -1; }
        if (menu_at >= 0 && frame == menu_at) {
            if (menu_page >= 0) MenuOpenSection(menu_page);
            if (menu_page == kSectionAbout) recomp_runtime_ui_handle_input(s_ui, RECOMP_RUNTIME_UI_INPUT_ACCEPT, 1, 0);
            else OpenMenu();
            menu_shown = 1;
            menu_at = -1;
        }

        if (hash_at) {
            /* list built once, any length (a fixed 1024-byte buffer silently dropped frames past ~2260). */
            static char *list;
            if (!list) {
                size_t n = strlen(hash_at) + 3;
                list = malloc(n);
                if (list) snprintf(list, n, ",%s,", hash_at);
            }
            char key[32];
            snprintf(key, sizeof key, ",%ld,", frame);
            if (list && strstr(list, key)) {
                printf("frame=%ld frame_sha256=", frame); PrintHash(s_pixels, sizeof s_pixels);
                printf(" wram_sha256="); PrintHash(g_ram, 0x20000); printf("\n");
                fflush(stdout);
            }
        }

        /* Turbo: only every s_turbo_speed-th frame is shown and paced, and the
         * sound is muted (still rendered, so the resampler keeps up). */
        int turbo = macro ? 30 : TurboHeld() ? s_turbo_speed : 1;   /* level select: fast-forward */
        int shown = frame % turbo == 0;

        /* Audio: one field's worth, trimmed to hold the device queue. */
        if (astream || s_wav) {
            double want = audio_per_frame;
            if (astream) {
                int queued = SDL_GetAudioStreamQueued(astream) / 4;
                if (queued == 0 && frame > 10 && pace && turbo == 1) underruns++;
                double e = (double)(audio_target - queued) / audio_target;
                if (e > 1) e = 1;
                if (e < -1) e = -1;
                want *= 1.0 + 0.005 * e;
            }
            audio_acc += want;
            int n = (int)audio_acc;
            audio_acc -= n;
            while (n > 0) {
                int c = n > 4096 ? 4096 : n;
                RtlRenderAudio(abuf, c, 2);
                if (s_volume < 100)
                    for (int j = 0; j < c * 2; j++) abuf[j] = (int16_t)(abuf[j] * s_volume / 100);
                if (astream && pace && turbo == 1) SDL_PutAudioStreamData(astream, abuf, c * 4);
                if (s_wav && s_wav_frames < s_wav_limit) {
                    uint32_t w = (uint32_t)c;
                    if (w > s_wav_limit - s_wav_frames) w = s_wav_limit - s_wav_frames;
                    fwrite(abuf, 4, w, s_wav);
                    s_wav_frames += w;
                }
                n -= c;
            }
        }

        if (shown || (dump_window && frames > 0 && frame >= frames)) Present();
        if (dump_window && frames > 0 && frame >= frames) DumpWindow(dump_window);

        if (frames > 0 && frame >= frames) break;

        if (macro) deadline = SDL_GetTicksNS();
        else if (pace && !s_vsync && shown) {
            deadline += period_ns;
            uint64_t now = SDL_GetTicksNS();
            if (now < deadline) SDL_DelayPrecise(deadline - now);
            else if (now - deadline > 5 * period_ns) { deadline = now; late_resyncs++; }
        }
    }
    RecStop("session ended");
    double wall = (double)(SDL_GetTicksNS() - t_start) / 1e9;

    printf("frames=%ld wall_s=%.3f fps=%.4f audio_underruns=%lu late_resyncs=%lu\n",
           frame, wall, wall > 0 ? (frame - frame_start) / wall : 0, underruns, late_resyncs);
    if (s_polled) printf("tape_words_used=%zu/%zu\n", s_poll_idx, s_tape_frames);
    printf("frame_sha256="); PrintHash(s_pixels, sizeof s_pixels);
    printf("\nwram_sha256="); PrintHash(g_ram, 0x20000); printf("\n");
    int rc = g_fail ? 6 : 0;
    if (dump_png) {
        if (WritePng(dump_png, s_pixels, kWidth, kHeight, kWidth * kBpp)) printf("dump_png=%s\n", dump_png);
        else rc = 7;
    }
    if (s_wav) {
        WavHeader(s_wav, s_audio_rate, s_wav_frames);
        fclose(s_wav);
        printf("wav=%s frames=%u rate=%d\n", wav_path, s_wav_frames, s_audio_rate);
    }
    if (astream) SDL_DestroyAudioStream(astream);
    if (s_pad) SDL_CloseGamepad(s_pad);
    TlkMenuImguiShutdown();
    recomp_runtime_ui_destroy(s_ui);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(rom);
    free(s_tape);
    return rc;
}
