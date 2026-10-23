/*
 * host_window_focus.c -- prove a background window never takes the focus.
 *
 * Shaped like a title: main() runs the host loop, a second thread is "the
 * game" and opens the window, asks for its Metal layer (what the renderer
 * does), and samples twice a second whether the window has the keyboard
 * focus and whether this process is the active application. Exit code 0
 * only if neither was ever true. RECOMP_WINDOW_BACKGROUND is forced to 1:
 * there is deliberately no switch to run it in the foreground.
 */
#include "host.h"

#include <SDL3/SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_seconds = 5;
static int g_failures;

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void *game_thread(void *arg)
{
    host_window *w;
    void *layer;
    int i, pw = 0, ph = 0;

    (void)arg;
    w = host_window_open(640, 480, "host_window_focus", NULL);
    if (!w) {
        fprintf(stderr, "FAIL: no window\n");
        recomp_host_loop_quit(2);
        return NULL;
    }
    layer = host_window_present_target(w);
    host_window_drawable_size(w, &pw, &ph);
    printf("window open: drawable %dx%d, metal layer %p, background %d, shown %d\n",
           pw, ph, layer, host_window_background(),
           !(SDL_GetWindowFlags(host_window_sdl(w)) & SDL_WINDOW_HIDDEN));
    /* A window that is not there would pass trivially. */
    if (!layer || (SDL_GetWindowFlags(host_window_sdl(w)) & SDL_WINDOW_HIDDEN)) {
        printf("FAIL: window hidden or without a Metal layer\n");
        g_failures++;
    }
    /* A background window must refuse to cover the screen. */
    host_window_set_fullscreen(w, 1);
    if (host_window_is_fullscreen(w) && host_window_background()) {
        printf("FAIL: background window went fullscreen\n");
        g_failures++;
    }
    for (i = 0; i < g_seconds * 2; i++) {
        int focus = host_window_has_focus(w);
        int active = host_app_is_active();

        printf("t=%.1fs focus=%d app_active=%d\n", i * 0.5, focus, active);
        fflush(stdout);
        if (focus || active)
            g_failures++;
        sleep_ms(500);
    }
    recomp_host_loop_quit(g_failures ? 1 : 0);
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t t;
    pthread_attr_t attr;
    int i, code;

    setenv("RECOMP_WINDOW_BACKGROUND", "1", 1);
    for (i = 1; i < argc; i++)
        if (atoi(argv[i]) > 0)
            g_seconds = atoi(argv[i]);
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&t, &attr, game_thread, NULL) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        return 2;
    }
    code = recomp_host_loop_run();
    pthread_join(t, NULL);
    printf("%s: focus or activation seen %d times\n", code == 0 ? "PASS" : "FAIL", g_failures);
    return code;
}
