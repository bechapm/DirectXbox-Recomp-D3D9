/*
 * host.h -- the host shell off Windows: the main-thread event loop and the
 * game window, on SDL3.
 *
 * Why a loop on the main thread. On macOS every window, view and Metal layer
 * belongs to the thread that ran main() (SDL's Cocoa driver refuses to start
 * anywhere else), while a title's D3D8 calls arrive on guest threads. So on
 * POSIX the title runs on a thread of its own (src/platform's
 * host_main_posix) and the main thread runs recomp_host_loop_run, which does
 * every piece of window work the guest threads ask for and delivers the
 * window's events. On Windows nothing here is used: the D3D8 replacement
 * keeps its own Win32 window thread (src/hle/hle_d3d8.c).
 *
 * The loop starts SDL's video only when the first window is asked for, so a
 * run with no window (RECOMP_HLE_D3D8=off) never becomes an application the
 * window server knows about, and cannot take the focus.
 *
 * RECOMP_WINDOW_BACKGROUND=1 opens the window without activating the process
 * or giving the window the keyboard focus, below the windows already there,
 * and never fullscreen -- the same promise the Win32 window makes
 * (SW_SHOWNOACTIVATE, HWND_BOTTOM), for a run a script drives while someone
 * works at the same desk. On macOS it also keeps the process out of the Dock
 * and the app switcher.
 */
#ifndef XBOXRECOMP_HOST_H
#define XBOXRECOMP_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ loop */

/* Run the host loop on the process's main thread until recomp_host_loop_quit
 * is called; returns the code it was given. */
int  recomp_host_loop_run(void);

/* From any thread: make recomp_host_loop_run return `code`. */
void recomp_host_loop_quit(int code);

/* Run fn(arg) on the main thread and wait for it; directly when called on
 * the main thread. On Apple a call from another thread is queued even before
 * the loop starts and dropped (fn never runs) after it has quit, or when no
 * loop has started within 10 seconds (a main.c that never runs one: the log
 * says so once, rather than the title hanging). Elsewhere, with no loop
 * running, fn runs on the calling thread. */
void recomp_host_call_main(void (*fn)(void *arg), void *arg);

/* ---------------------------------------------------------------- window */

typedef struct host_window host_window;

/* Keys the shell itself answers; everything else is the input bindings'. */
enum {
    HOST_KEY_F9 = 1,        /* frame-rate overlay */
    HOST_KEY_F10,           /* frame cap step */
    HOST_KEY_F11,           /* frame capture */
    HOST_KEY_FULLSCREEN,    /* Alt+Enter (Option+Enter, or Cmd+Enter, on a Mac) */
};

typedef struct {
    /* The user closed the window. Called on the main thread. NULL: the
     * process exits with code 0. */
    void (*on_close)(void *user);
    /* A shell key went down while the window had the focus. Main thread.
     * HOST_KEY_FULLSCREEN is handled before this is called. */
    void (*on_key)(void *user, int host_key);
    /* The drawable's size in pixels: once when the window opens (before
     * host_window_open returns) and after every change. Main thread. */
    void (*on_resize)(void *user, int width, int height);
    /* The window went fullscreen (1) or back (0), by key or by call. */
    void (*on_fullscreen)(void *user, int on);
    /* The mouse, while the window has the focus. Motion is reported only
     * while the window holds the pointer (host_window_set_relative_mouse),
     * as relative counts, x right and y down. Buttons are SDL's numbers:
     * 1 left, 2 middle, 3 right, 4 and 5 the side buttons. A window that
     * loses the focus reports every button up. Main thread. */
    void (*on_mouse_motion)(void *user, float dx, float dy);
    void (*on_mouse_button)(void *user, int button, int down);
    void *user;
} host_window_callbacks;

/* RECOMP_WINDOW_BACKGROUND is set (and not "0"). */
int host_window_background(void);

/* Open the game window, `width` x `height` pixels of client area, from any
 * thread; blocks until it exists. NULL on failure (the log says why). One
 * window per process is what the runtime needs; a second call returns the
 * first. `title` is UTF-8 and may be NULL. */
host_window *host_window_open(int width, int height, const char *title,
                              const host_window_callbacks *callbacks);

/* The window's CAMetalLayer (Apple only; NULL elsewhere), made on the main
 * thread the first time it is asked for and kept as long as the window. This
 * is what the Vulkan RHI is given as its window (vkCreateMetalSurfaceEXT). */
void *host_window_metal_layer(host_window *w);

/* The SDL_Window behind it, for code that needs SDL directly. */
struct SDL_Window *host_window_sdl(host_window *w);

/* Size of the drawable in pixels (Retina: larger than the window's points). */
void host_window_drawable_size(host_window *w, int *width, int *height);

/* Desktop fullscreen on or off. Refused in background mode. Any thread. */
void host_window_set_fullscreen(host_window *w, int on);
int  host_window_is_fullscreen(host_window *w);

/* Take the pointer while the window has the focus -- hidden, kept inside
 * the window, its motion reported to on_mouse_motion -- or give it back.
 * Switching to another window gives it back until this one is in front
 * again. Refused in background mode. Any thread. */
void host_window_set_relative_mouse(host_window *w, int on);

/* Whether the window has the keyboard focus right now. Any thread. */
int  host_window_has_focus(host_window *w);

void host_window_set_title(host_window *w, const char *utf8);

/* Destroy the window (its Metal layer with it). A later host_window_open
 * makes a new one. */
void host_window_close(host_window *w);

/* macOS: whether this process is the active application (the one in front).
 * 0 elsewhere. For tests that prove background mode keeps out of the way. */
int  host_app_is_active(void);

#ifdef __cplusplus
}
#endif

#endif
