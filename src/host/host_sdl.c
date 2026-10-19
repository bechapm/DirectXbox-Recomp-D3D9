/*
 * host_sdl.c -- the main-thread loop and the game window, on SDL3 (host.h).
 */
#include "host.h"
#include "host_mac.h"

#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ loop */

typedef struct host_call {
    void (*fn)(void *);
    void *arg;
    int done;
    struct host_call *next;
} host_call;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static host_call *g_head, *g_tail;
static int        g_running;          /* recomp_host_loop_run is serving calls */
static int        g_quit, g_quit_code;
static int        g_no_loop;          /* Apple: waited for a loop that never came */
static pthread_t  g_main_thread;
static int        g_video;            /* SDL video started (main thread only writes) */
static Uint32     g_wake_event;       /* user event that wakes SDL_WaitEvent */

static void dispatch_event(const SDL_Event *e);

/* Wake the loop wherever it is waiting: on the condition before video is
 * up, in SDL_WaitEvent after. Called with g_lock held. */
static void wake_locked(void)
{
    pthread_cond_broadcast(&g_cond);
    if (g_video && g_wake_event) {
        SDL_Event e;

        SDL_zero(e);
        e.type = g_wake_event;
        SDL_PushEvent(&e);
    }
}

/* Run what the guest threads have queued. Called with g_lock held; drops it
 * around each call so a call may itself ask for another. */
static void run_calls_locked(void)
{
    while (g_head) {
        host_call *c = g_head;

        g_head = c->next;
        if (!g_head)
            g_tail = NULL;
        pthread_mutex_unlock(&g_lock);
        c->fn(c->arg);
        pthread_mutex_lock(&g_lock);
        c->done = 1;
        pthread_cond_broadcast(&g_cond);
    }
}

int recomp_host_loop_run(void)
{
    int code;

    /* Here rather than when the window opens: a run with no window can be
     * napped too, once its terminal is hidden. */
    host_mac_keep_awake();
    pthread_mutex_lock(&g_lock);
    g_main_thread = pthread_self();
    g_running = 1;
    pthread_cond_broadcast(&g_cond);      /* calls waiting for the loop to start */
    for (;;) {
        while (!g_quit && !g_head && !g_video)
            pthread_cond_wait(&g_cond, &g_lock);
        run_calls_locked();
        if (g_quit)
            break;
        if (g_video) {
            SDL_Event e;

            pthread_mutex_unlock(&g_lock);
            if (SDL_WaitEventTimeout(&e, 100)) {
                do
                    dispatch_event(&e);
                while (SDL_PollEvent(&e));
            }
            pthread_mutex_lock(&g_lock);
        }
    }
    /* Nothing is served after this; a call already queued still runs, so
     * no caller waits forever, and later ones run on their own thread. */
    g_running = 0;
    run_calls_locked();
    code = g_quit_code;
    pthread_mutex_unlock(&g_lock);
    return code;
}

void recomp_host_loop_quit(int code)
{
    pthread_mutex_lock(&g_lock);
    if (!g_quit) {
        g_quit = 1;
        g_quit_code = code;
    }
    wake_locked();
    pthread_mutex_unlock(&g_lock);
}

/* Called with g_lock held. */
static int on_main_thread_locked(void)
{
#ifdef __APPLE__
    return pthread_main_np() != 0;
#else
    return g_running && pthread_equal(pthread_self(), g_main_thread);
#endif
}

void recomp_host_call_main(void (*fn)(void *), void *arg)
{
    host_call c;

    pthread_mutex_lock(&g_lock);
    if (on_main_thread_locked()) {
        pthread_mutex_unlock(&g_lock);
        fn(arg);
        return;
    }
#ifdef __APPLE__
    /* AppKit only works on the main thread, so a call is queued even before
     * the loop has started (the title thread is created first and may get
     * here before main() reaches recomp_host_loop_run), and dropped once the
     * loop has quit: run on this thread it would throw inside AppKit and
     * turn a clean exit into an abort. A dropped call leaves its result
     * as the caller initialised it (no window, size 0). */
    if (!g_running && !g_quit && !g_no_loop) {
        struct timespec until;

        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 10;
        while (!g_running && !g_quit &&
               pthread_cond_timedwait(&g_cond, &g_lock, &until) == 0)
            ;
        if (!g_running && !g_quit) {
            g_no_loop = 1;
            fprintf(stderr, "[HOST] no main-thread loop is running (main.c does not "
                    "call recomp_host_loop_run): no window on this platform\n");
        }
    }
    if (g_quit || g_no_loop) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
#else
    /* No main-thread rule here: without a running loop, run it in place. */
    if (!g_running) {
        pthread_mutex_unlock(&g_lock);
        fn(arg);
        return;
    }
#endif
    c.fn = fn;
    c.arg = arg;
    c.done = 0;
    c.next = NULL;
    if (g_tail)
        g_tail->next = &c;
    else
        g_head = &c;
    g_tail = &c;
    wake_locked();
    while (!c.done)
        pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);
}

/* ---------------------------------------------------------------- window */

struct host_window {
    SDL_Window *sdl;
    SDL_WindowID id;
    host_window_callbacks cb;
    int background;
#ifdef __APPLE__
    SDL_MetalView view;
#endif
    void *layer;
    SDL_AtomicInt focus;
    SDL_AtomicInt fullscreen;
};

static host_window g_window;
static int         g_window_open;     /* main thread only */

int host_window_background(void)
{
    const char *bg = getenv("RECOMP_WINDOW_BACKGROUND");

    return bg && *bg && strcmp(bg, "0") != 0;
}

/* Main thread: start SDL's video the first time a window is wanted. */
static int start_video(int background)
{
    if (g_video)
        return 1;
    /* SDL would otherwise turn SIGINT/SIGTERM into a quit event, and a
     * timeout's SIGTERM must keep killing the process. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    if (background) {
        /* Not "regular" (no Dock icon, no activation at launch), and neither
         * showing nor raising the window activates it. */
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[HOST] SDL video failed to start: %s\n", SDL_GetError());
        return 0;
    }
    if (background)
        host_mac_set_accessory();
    g_wake_event = SDL_RegisterEvents(1);
    pthread_mutex_lock(&g_lock);
    g_video = 1;
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[HOST] SDL %d.%d.%d video (%s)%s\n", SDL_MAJOR_VERSION,
            SDL_MINOR_VERSION, SDL_MICRO_VERSION, SDL_GetCurrentVideoDriver(),
            background ? ", background: no activation, no focus" : "");
    return 1;
}

typedef struct {
    int width, height;
    const char *title;
    const host_window_callbacks *cb;
    host_window *result;
} open_request;

static void report_size(host_window *w)
{
    int pw = 0, ph = 0;

    if (w->cb.on_resize && SDL_GetWindowSizeInPixels(w->sdl, &pw, &ph) && pw > 0 && ph > 0)
        w->cb.on_resize(w->cb.user, pw, ph);
}

static void open_on_main(void *arg)
{
    open_request *req = arg;
    host_window *w = &g_window;
    int background = host_window_background();
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE |
                            SDL_WINDOW_HIGH_PIXEL_DENSITY;

    if (g_window_open) {
        req->result = w;
        return;
    }
    if (!start_video(background))
        return;
#ifdef __APPLE__
    flags |= SDL_WINDOW_METAL;
#endif
    if (background)
        flags |= SDL_WINDOW_NOT_FOCUSABLE;
    w->sdl = SDL_CreateWindow(req->title ? req->title : "xboxrecomp",
                              req->width, req->height, flags);
    if (!w->sdl) {
        fprintf(stderr, "[HOST] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return;
    }
    w->id = SDL_GetWindowID(w->sdl);
    w->background = background;
    memset(&w->cb, 0, sizeof w->cb);
    if (req->cb)
        w->cb = *req->cb;
    SDL_SetAtomicInt(&w->focus, 0);
    SDL_SetAtomicInt(&w->fullscreen, 0);
    SDL_ShowWindow(w->sdl);
    if (background) {
#ifdef __APPLE__
        host_mac_order_back(SDL_GetPointerProperty(SDL_GetWindowProperties(w->sdl),
                                                   SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                                                   NULL));
#endif
    }
    g_window_open = 1;
    req->result = w;
    report_size(w);
}

host_window *host_window_open(int width, int height, const char *title,
                              const host_window_callbacks *callbacks)
{
    open_request req;

    memset(&req, 0, sizeof req);
    req.width = width > 0 ? width : 640;
    req.height = height > 0 ? height : 480;
    req.title = title;
    req.cb = callbacks;
    recomp_host_call_main(open_on_main, &req);
    return req.result;
}

static void layer_on_main(void *arg)
{
    host_window *w = arg;

#ifdef __APPLE__
    if (!w->layer && w->sdl) {
        w->view = SDL_Metal_CreateView(w->sdl);
        if (w->view)
            w->layer = SDL_Metal_GetLayer(w->view);
        if (!w->layer)
            fprintf(stderr, "[HOST] no Metal layer for the window: %s\n", SDL_GetError());
    }
#else
    (void)w;
#endif
}

void *host_window_metal_layer(host_window *w)
{
    if (!w)
        return NULL;
    recomp_host_call_main(layer_on_main, w);
    return w->layer;
}

struct SDL_Window *host_window_sdl(host_window *w)
{
    return w ? w->sdl : NULL;
}

typedef struct {
    host_window *w;
    int width, height;
    int on;
    const char *text;
} window_op;

static void size_on_main(void *arg)
{
    window_op *op = arg;

    if (!SDL_GetWindowSizeInPixels(op->w->sdl, &op->width, &op->height))
        op->width = op->height = 0;
}

void host_window_drawable_size(host_window *w, int *width, int *height)
{
    window_op op;

    memset(&op, 0, sizeof op);
    op.w = w;
    if (w && w->sdl)
        recomp_host_call_main(size_on_main, &op);
    if (width)
        *width = op.width;
    if (height)
        *height = op.height;
}

static void fullscreen_on_main(void *arg)
{
    window_op *op = arg;
    host_window *w = op->w;
    int on = op->on ? 1 : 0;

    if (!w->sdl || on == SDL_GetAtomicInt(&w->fullscreen))
        return;
    if (on && w->background) {
        fprintf(stderr, "[HOST] fullscreen refused: background window\n");
        return;
    }
    /* NULL mode: the desktop's own, a borderless window covering the screen. */
    SDL_SetWindowFullscreenMode(w->sdl, NULL);
    if (!SDL_SetWindowFullscreen(w->sdl, on)) {
        fprintf(stderr, "[HOST] fullscreen %s failed: %s\n", on ? "on" : "off", SDL_GetError());
        return;
    }
    SDL_SetAtomicInt(&w->fullscreen, on);
    fprintf(stderr, on ? "[HOST] fullscreen (Option+Enter for a window)\n" : "[HOST] windowed\n");
    if (w->cb.on_fullscreen)
        w->cb.on_fullscreen(w->cb.user, on);
}

static void relative_on_main(void *arg)
{
    window_op *op = arg;
    host_window *w = op->w;

    if (!w->sdl)
        return;
    if (op->on && w->background) {
        fprintf(stderr, "[HOST] mouse capture refused: background window\n");
        return;
    }
    if (!SDL_SetWindowRelativeMouseMode(w->sdl, op->on ? true : false))
        fprintf(stderr, "[HOST] mouse capture %s failed: %s\n", op->on ? "on" : "off",
                SDL_GetError());
}

void host_window_set_relative_mouse(host_window *w, int on)
{
    window_op op;

    if (!w)
        return;
    memset(&op, 0, sizeof op);
    op.w = w;
    op.on = on;
    recomp_host_call_main(relative_on_main, &op);
}

void host_window_set_fullscreen(host_window *w, int on)
{
    window_op op;

    if (!w)
        return;
    memset(&op, 0, sizeof op);
    op.w = w;
    op.on = on;
    recomp_host_call_main(fullscreen_on_main, &op);
}

int host_window_is_fullscreen(host_window *w)
{
    return w ? SDL_GetAtomicInt(&w->fullscreen) : 0;
}

int host_window_has_focus(host_window *w)
{
    return w ? SDL_GetAtomicInt(&w->focus) : 0;
}

static void title_on_main(void *arg)
{
    window_op *op = arg;

    if (op->w->sdl && op->text)
        SDL_SetWindowTitle(op->w->sdl, op->text);
}

void host_window_set_title(host_window *w, const char *utf8)
{
    window_op op;

    if (!w || !utf8)
        return;
    memset(&op, 0, sizeof op);
    op.w = w;
    op.text = utf8;
    recomp_host_call_main(title_on_main, &op);
}

static void close_on_main(void *arg)
{
    host_window *w = arg;

#ifdef __APPLE__
    if (w->view)
        SDL_Metal_DestroyView(w->view);
    w->view = NULL;
#endif
    w->layer = NULL;
    if (w->sdl)
        SDL_DestroyWindow(w->sdl);
    w->sdl = NULL;
    w->id = 0;
    g_window_open = 0;
}

void host_window_close(host_window *w)
{
    if (w && w->sdl)
        recomp_host_call_main(close_on_main, w);
}

static void active_on_main(void *arg)
{
    *(int *)arg = host_mac_app_is_active();
}

int host_app_is_active(void)
{
    int active = 0;

    if (g_video)
        recomp_host_call_main(active_on_main, &active);
    return active;
}

/* ---------------------------------------------------------------- events */

static void window_closed(host_window *w)
{
    fprintf(stderr, "[HOST] window closed by the user\n");
    fflush(stderr);
    if (w->cb.on_close)
        w->cb.on_close(w->cb.user);
    else
        recomp_host_loop_quit(0);
}

static void dispatch_event(const SDL_Event *e)
{
    host_window *w = &g_window;

    if (!g_window_open)
        return;
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e->window.windowID == w->id)
            window_closed(w);
        break;
    case SDL_EVENT_QUIT:                     /* Cmd+Q, the app menu */
        window_closed(w);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (e->window.windowID == w->id)
            SDL_SetAtomicInt(&w->focus, 1);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (e->window.windowID == w->id) {
            SDL_SetAtomicInt(&w->focus, 0);
            /* Their releases go to whatever has the focus now. */
            if (w->cb.on_mouse_button) {
                int b;

                for (b = SDL_BUTTON_LEFT; b <= SDL_BUTTON_X2; b++)
                    w->cb.on_mouse_button(w->cb.user, b, 0);
            }
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        /* Only the held pointer's: a free one crossing the window is
         * someone on their way to another. */
        if (e->motion.windowID == w->id && w->cb.on_mouse_motion &&
            SDL_GetWindowRelativeMouseMode(w->sdl))
            w->cb.on_mouse_motion(w->cb.user, e->motion.xrel, e->motion.yrel);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.windowID == w->id && w->cb.on_mouse_button)
            w->cb.on_mouse_button(w->cb.user, e->button.button, e->button.down ? 1 : 0);
        break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        /* Also what the green button and the system's own fullscreen
         * gesture send, so the renderer hears of those too. */
        if (e->window.windowID == w->id) {
            int on = e->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;

            if (SDL_GetAtomicInt(&w->fullscreen) != on) {
                SDL_SetAtomicInt(&w->fullscreen, on);
                if (w->cb.on_fullscreen)
                    w->cb.on_fullscreen(w->cb.user, on);
            }
        }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (e->window.windowID == w->id && w->cb.on_resize && e->window.data1 > 0 &&
            e->window.data2 > 0)
            w->cb.on_resize(w->cb.user, e->window.data1, e->window.data2);
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat || e->key.windowID != w->id)
            break;
        if (e->key.key == SDLK_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) {
            window_op op;

            memset(&op, 0, sizeof op);
            op.w = w;
            op.on = !SDL_GetAtomicInt(&w->fullscreen);
            fullscreen_on_main(&op);
        } else if (w->cb.on_key) {
            int k = e->key.key == SDLK_F9 ? HOST_KEY_F9 :
                    e->key.key == SDLK_F10 ? HOST_KEY_F10 :
                    e->key.key == SDLK_F11 ? HOST_KEY_F11 : 0;

            if (k)
                w->cb.on_key(w->cb.user, k);
        }
        break;
    default:
        break;
    }
}
