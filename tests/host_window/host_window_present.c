/*
 * host_window_present.c -- the renderer presents into the host shell's
 * window, the way the D3D8 replacement's shadow device does, and the window
 * still never takes the focus.
 *
 * main() runs the host loop; a second thread opens the window, hands the
 * renderer its CAMetalLayer as the device window (hle_d3d8.c shadow_window),
 * reports its pixel size through xbox_D3D8SetWindowSize, and clears and
 * swaps a colour cycle for a few seconds. Fails if the device cannot be
 * made, if what reached the swap chain is not the clear colour, or if the
 * window ever has the focus or the process is ever the active application.
 * RECOMP_WINDOW_BACKGROUND is forced on. Needs a display and a Vulkan
 * device; run by hand (check_focus.sh beside it adds the outside view).
 */
#include "host.h"
#include "d3d8_xbox.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_seconds = 3;
static int g_failures;

static void on_resize(void *user, int w, int h)
{
    (void)user;
    xbox_D3D8SetWindowSize((UINT)w, (UINT)h);
}

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *game_thread(void *arg)
{
    host_window_callbacks cb;
    host_window *w;
    D3DPRESENT_PARAMETERS pp;
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev = NULL;
    void *layer;
    double t0, last_sample = 0;
    unsigned long frames = 0;
    HRESULT hr;

    (void)arg;
    memset(&cb, 0, sizeof cb);
    cb.on_resize = on_resize;
    w = host_window_open(640, 480, "host_window_present", &cb);
    layer = w ? host_window_present_target(w) : NULL;
    if (!layer) {
        printf("FAIL: no window or no Metal layer\n");
        recomp_host_loop_quit(2);
        return NULL;
    }
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = (HWND)layer;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    d3d = xbox_Direct3DCreate8(0);
    hr = d3d ? d3d->lpVtbl->CreateDevice(d3d, 0, 1, (HWND)layer, 0, &pp, &dev) : E_FAIL;
    if (FAILED(hr) || !dev) {
        printf("FAIL: CreateDevice 0x%08lX\n", (unsigned long)hr);
        recomp_host_loop_quit(3);
        return NULL;
    }
    xbox_D3D8SetPresentInterval(0);          /* as the shadow device */
    xbox_D3D8KeepPresented(TRUE);
    t0 = now_s();
    while (now_s() - t0 < g_seconds) {
        D3DCOLOR c = 0xFF000000u | ((frames * 3u) & 0xFFu) << 16 | 0x40u << 8 | 0xC0u;

        dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, c, 1.0f, 0);
        dev->lpVtbl->Swap(dev, 0);
        frames++;
        if (frames == 10) {
            UINT pw = 0, ph = 0;
            const uint8_t *px = xbox_D3D8Presented(&pw, &ph);

            /* RGBA; the clear was G=0x40, B=0xC0 (R cycles). */
            if (!px || !pw || px[1] < 0x3E || px[1] > 0x42 || px[2] < 0xBE || px[2] > 0xC2) {
                printf("FAIL: presented pixel R=%02X G=%02X B=%02X (%ux%u), want G=40 B=C0\n",
                       px ? px[0] : 0, px ? px[1] : 0, px ? px[2] : 0, pw, ph);
                g_failures++;
            } else {
                printf("presented %ux%u, first pixel R=%02X G=%02X B=%02X\n", pw, ph,
                       px[0], px[1], px[2]);
            }
            xbox_D3D8KeepPresented(FALSE);   /* a readback a frame costs pace */
        }
        if (now_s() - last_sample >= 0.5) {
            int focus = host_window_has_focus(w), active = host_app_is_active();

            last_sample = now_s();
            printf("t=%.1fs frames=%lu focus=%d app_active=%d\n", last_sample - t0, frames,
                   focus, active);
            fflush(stdout);
            if (focus || active)
                g_failures++;
        }
    }
    printf("%lu frames in %d s = %.0f fps into a background window\n", frames, g_seconds,
           frames / (double)g_seconds);
    recomp_host_loop_quit(g_failures ? 1 : 0);
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t t;
    pthread_attr_t attr;
    int code;

    setenv("RECOMP_WINDOW_BACKGROUND", "1", 1);
    if (argc > 1 && atoi(argv[1]) > 0)
        g_seconds = atoi(argv[1]);
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&t, &attr, game_thread, NULL) != 0)
        return 2;
    code = recomp_host_loop_run();
    /* The device and its swap chain go with the process; nothing here waits
     * for the game thread to tear them down. */
    printf("%s: focus or activation or a wrong picture %d times\n", code == 0 ? "PASS" : "FAIL",
           g_failures);
    fflush(stdout);
    _exit(code);
}
