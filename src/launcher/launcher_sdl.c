/*
 * launcher_sdl.c -- the launcher's window off Windows (launcher.h), on
 * SDL3, which the pads already need.
 *
 * The theme draws into its own buffer at the display's resolution
 * (theme_set_scale, from the window's pixel density), and each frame of it
 * goes on screen through Core Animation on macOS (launcher_cocoa.m), where
 * the toolkit's SDL has no renderer, and through an SDL renderer's
 * streaming texture on Linux, where it has one for this (the root
 * CMakeLists) because Wayland gives a window no other way to show a
 * buffer. Frames, the pad and the glow run at sixty a second off the
 * clock, as the Windows launcher's 16 ms timer does.
 *
 * RECOMP_LAUNCHER_SHOT=<file.bmp> writes the theme's buffer after half a
 * second (thirty frames) and quits, so the drawing can be checked from a
 * script without capturing anyone's screen.
 *
 * The game is started with posix_spawn, beside the launcher, in its own
 * directory, with this environment -- so a RECOMP_* variable set for the
 * launcher reaches the game -- and the launcher then ends.
 */
#include <SDL3/SDL.h>

#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "launcher.h"
#include "launcher_theme.h"

extern char **environ;

#include "launcher_theme_buf.h"

#if defined(__APPLE__)
void launcher_cocoa_present(void *nswindow, void *cgimage);   /* launcher_cocoa.m */
void CGImageRelease(void *image);                             /* CoreGraphics */
#endif

static SDL_Window   *g_win;
static int           g_quit;

/* ------------------------------------------------------------ the host */

uint64_t lhost_ticks(void)
{
    return SDL_GetTicks();
}

static SDL_Scancode scancode_of(int key)
{
    if (key >= 'A' && key <= 'Z')
        return (SDL_Scancode)(SDL_SCANCODE_A + (key - 'A'));
    if (key == '0')
        return SDL_SCANCODE_0;          /* after 9, as on the keyboard */
    if (key >= '1' && key <= '9')
        return (SDL_Scancode)(SDL_SCANCODE_1 + (key - '1'));
    switch (key) {
    case LKEY_UP:     return SDL_SCANCODE_UP;
    case LKEY_DOWN:   return SDL_SCANCODE_DOWN;
    case LKEY_LEFT:   return SDL_SCANCODE_LEFT;
    case LKEY_RIGHT:  return SDL_SCANCODE_RIGHT;
    case LKEY_RETURN: return SDL_SCANCODE_RETURN;
    case LKEY_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case LKEY_TAB:    return SDL_SCANCODE_TAB;
    case LKEY_BACK:   return SDL_SCANCODE_BACKSPACE;
    case LKEY_SPACE:  return SDL_SCANCODE_SPACE;
    case LKEY_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case LKEY_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case LKEY_LCTRL:  return SDL_SCANCODE_LCTRL;
    case LKEY_RCTRL:  return SDL_SCANCODE_RCTRL;
    default:          return SDL_SCANCODE_UNKNOWN;
    }
}

int lhost_key_held(int key)
{
    SDL_Scancode sc = scancode_of(key);
    int n = 0;
    const bool *state = SDL_GetKeyboardState(&n);

    return sc != SDL_SCANCODE_UNKNOWN && (int)sc < n && state[sc];
}

int lhost_paste(char *out, size_t n)
{
    char *text;

    if (!n || !SDL_HasClipboardText())
        return 0;
    text = SDL_GetClipboardText();
    if (!text)
        return 0;
    snprintf(out, n, "%s", text);
    SDL_free(text);
    return out[0] != '\0';
}

int lhost_copy(const char *text)
{
    return SDL_SetClipboardText(text) ? 1 : 0;
}

int lhost_launch(const char *exe, const char *dir)
{
    posix_spawn_file_actions_t fa;
    char *argv[2];
    pid_t pid;
    int err;

    argv[0] = (char *)exe;
    argv[1] = NULL;
    posix_spawn_file_actions_init(&fa);
    /* The game's own directory, whatever this one was started in. macOS
     * 26 names the call without its _np; older systems have only that. */
#if defined(__APPLE__) && defined(__MAC_26_0)
    if (__builtin_available(macOS 26.0, *))
        posix_spawn_file_actions_addchdir(&fa, dir);
    else
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        posix_spawn_file_actions_addchdir_np(&fa, dir);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    err = posix_spawn(&pid, exe, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);

    if (err != 0) {
        char msg[PATH_MAX + 200];

        snprintf(msg, sizeof msg,
                 "The game could not be started.\n\n%s\n\n%s\n\n"
                 "The launcher expects it beside itself.", exe, strerror(err));
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cannot start the game", msg, g_win);
        return 0;
    }
    return 1;
}

void lhost_close(void)
{
    g_quit = 1;
}

static int thread_main(void *fn)
{
    ((void (*)(void))fn)();
    return 0;
}

int lhost_thread(void (*fn)(void))
{
    SDL_Thread *t = SDL_CreateThread(thread_main, "launcher", (void *)fn);

    if (!t)
        return 0;
    SDL_DetachThread(t);
    return 1;
}

void lhost_publish(volatile long *flag, long value)
{
    __atomic_store_n(flag, value, __ATOMIC_RELEASE);
}

long lhost_read(volatile long *flag)
{
    return __atomic_load_n(flag, __ATOMIC_ACQUIRE);
}

void lhost_full_path(const char *path, char *out, size_t n)
{
    char full[PATH_MAX];

    if (realpath(path, full))
        snprintf(out, n, "%s", full);
    else
        snprintf(out, n, "%s", path);
}

/* ---------------------------------------------------------------- window */

static int key_of(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP:       return LKEY_UP;
    case SDLK_DOWN:     return LKEY_DOWN;
    case SDLK_LEFT:     return LKEY_LEFT;
    case SDLK_RIGHT:    return LKEY_RIGHT;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return LKEY_RETURN;
    case SDLK_ESCAPE:   return LKEY_ESCAPE;
    case SDLK_TAB:      return LKEY_TAB;
    default:            return 0;
    }
}

static void on_key(const SDL_KeyboardEvent *e)
{
    /* Command on a Mac, Control elsewhere: the clipboard's keys, given to
     * the launcher as the control characters Windows' WM_CHAR makes of
     * Ctrl+C and Ctrl+V. */
    int clip = (e->mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0;

    if (clip && e->key == SDLK_C) {
        launcher_char(3);
        return;
    }
    if (clip && e->key == SDLK_V) {
        launcher_char(22);
        return;
    }
    if (clip && e->key == SDLK_Q) {
        g_quit = 1;
        return;
    }
    if (e->key == SDLK_BACKSPACE) {
        launcher_char(8);
        return;
    }
    launcher_key(key_of(e->key), (e->mod & SDL_KMOD_SHIFT) != 0);
}

static void on_text(const char *text)
{
    for (; *text; text++)
        if ((unsigned char)*text >= 32 && (unsigned char)*text < 127)
            launcher_char((unsigned char)*text);
}

/* ------------------------------------------------------------- present */

#if defined(__APPLE__)
static void *g_nswindow;

static int present_open(void)
{
    g_nswindow = SDL_GetPointerProperty(SDL_GetWindowProperties(g_win),
                                        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (!g_nswindow) {
        fprintf(stderr, "launcher: no Cocoa window to draw in\n");
        return 0;
    }
    return 1;
}

static void present(void)
{
    void *image = theme_image();

    if (image) {
        launcher_cocoa_present(g_nswindow, image);
        CGImageRelease(image);
    }
}

static void present_close(void) { }
#else
static SDL_Renderer *g_renderer;
static SDL_Texture  *g_texture;
static int           g_tex_w, g_tex_h;

static int present_open(void)
{
    g_renderer = SDL_CreateRenderer(g_win, NULL);
    if (!g_renderer) {
        fprintf(stderr, "launcher: SDL_CreateRenderer: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetRenderVSync(g_renderer, 1);
    return 1;
}

/* The buffer, 0x00RRGGBB top row first, is SDL's XRGB8888 as it stands. */
static void present(void)
{
    if (!theme_px || theme_w <= 0 || theme_h <= 0)
        return;
    if (!g_texture || g_tex_w != theme_w || g_tex_h != theme_h) {
        if (g_texture)
            SDL_DestroyTexture(g_texture);
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, theme_w, theme_h);
        if (!g_texture)
            return;
        g_tex_w = theme_w;
        g_tex_h = theme_h;
    }
    SDL_UpdateTexture(g_texture, NULL, theme_px, theme_w * 4);
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
}

static void present_close(void)
{
    if (g_texture)
        SDL_DestroyTexture(g_texture);
    if (g_renderer)
        SDL_DestroyRenderer(g_renderer);
    g_texture = NULL;
    g_renderer = NULL;
}
#endif

/* RECOMP_LAUNCHER_SHOT: the buffer as a 24-bit BMP, bottom row first. */
static void write_shot(const char *path)
{
    uint8_t hdr[54];
    uint32_t row = ((uint32_t)theme_w * 3 + 3) & ~3u, size;
    uint8_t *line;
    FILE *f;
    int x, y, i;

    if (!theme_px || theme_w <= 0 || theme_h <= 0 || !(f = fopen(path, "wb"))) {
        fprintf(stderr, "launcher: RECOMP_LAUNCHER_SHOT: cannot write %s\n", path);
        return;
    }
    size = 54 + row * (uint32_t)theme_h;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    for (i = 0; i < 4; i++) {
        hdr[2 + i]  = (uint8_t)(size >> (8 * i));
        hdr[18 + i] = (uint8_t)((uint32_t)theme_w >> (8 * i));
        hdr[22 + i] = (uint8_t)((uint32_t)theme_h >> (8 * i));
    }
    hdr[10] = 54; hdr[14] = 40; hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    line = calloc(1, row);
    for (y = theme_h - 1; line && y >= 0; y--) {
        const uint32_t *src = theme_px + (size_t)y * theme_w;
        for (x = 0; x < theme_w; x++) {
            line[x * 3 + 0] = (uint8_t)src[x];
            line[x * 3 + 1] = (uint8_t)(src[x] >> 8);
            line[x * 3 + 2] = (uint8_t)(src[x] >> 16);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "launcher: wrote %s (%dx%d)\n", path, theme_w, theme_h);
}

/* The window's size and its pixels per unit, to the theme. */
static void fit(void)
{
    int w, h;

    SDL_GetWindowSize(g_win, &w, &h);
    theme_set_scale(SDL_GetWindowPixelDensity(g_win));
    launcher_resize(w, h);
}

int main(int argc, char **argv)
{
    const char *shot = getenv("RECOMP_LAUNCHER_SHOT");
    int frames = 0;
    uint64_t next_tick;

    (void)argc; (void)argv;

    launcher_init();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "launcher: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g_win = SDL_CreateWindow(launcher_title(), LAUNCHER_WINDOW_W, LAUNCHER_WINDOW_H,
                             SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_win) {
        fprintf(stderr, "launcher: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    if (!present_open()) {
        SDL_DestroyWindow(g_win);
        SDL_Quit();
        return 1;
    }
    SDL_StartTextInput(g_win);
    fit();

    next_tick = SDL_GetTicks();
    while (!g_quit) {
        SDL_Event e;
        uint64_t now;
        int ticks = 0;

        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                g_quit = 1;
                break;
            case SDL_EVENT_KEY_DOWN:
                on_key(&e.key);
                break;
            case SDL_EVENT_TEXT_INPUT:
                on_text(e.text.text);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                int b = e.button.button == SDL_BUTTON_LEFT   ? LMOUSE_LEFT
                      : e.button.button == SDL_BUTTON_RIGHT  ? LMOUSE_RIGHT
                      : e.button.button == SDL_BUTTON_MIDDLE ? LMOUSE_MIDDLE
                      : e.button.button == SDL_BUTTON_X1     ? LMOUSE_X1
                      : e.button.button == SDL_BUTTON_X2     ? LMOUSE_X2 : 0;

                if (b && !launcher_mouse_button(b) && b == LMOUSE_LEFT)
                    launcher_click((int)e.button.x, (int)e.button.y);
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
                launcher_hover((int)e.motion.x, (int)e.motion.y);
                break;
            case SDL_EVENT_MOUSE_WHEEL: {
                /* Whole notches, positive up; a flipped ("natural") wheel
                 * reports the other way round. */
                int rows = e.wheel.integer_y;

                if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                    rows = -rows;
                if (rows)
                    launcher_wheel(-rows, (int)e.wheel.mouse_x, (int)e.wheel.mouse_y);
                break;
            }
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                fit();
                break;
            default:
                break;
            }
        }
        if (g_quit)
            break;

        /* Sixty a second off the clock; a stall is not made up for all at
         * once, or the glow and a held direction would race. */
        now = SDL_GetTicks();
        while (now >= next_tick && ticks < 4) {
            launcher_tick();
            next_tick += 16;
            ticks++;
        }
        if (now >= next_tick)
            next_tick = now + 16;

        if (ticks) {
            launcher_draw();
            present();
            if (shot && *shot && ++frames == 30) {
                write_shot(shot);
                g_quit = 1;
            }
        }

        /* Until the next tick, or an event: whichever is first. */
        now = SDL_GetTicks();
        if (next_tick > now)
            SDL_WaitEventTimeout(NULL, (Sint32)(next_tick - now));
    }

    SDL_StopTextInput(g_win);
    present_close();
    SDL_DestroyWindow(g_win);
    theme_shutdown();
    SDL_Quit();
    return 0;
}
