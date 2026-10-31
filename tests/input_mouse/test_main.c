/*
 * test_main -- mouse: bindings and mouse look, through recomp_bindings_sample.
 *
 * What matters is what a title reads: a mouse button bound to a control
 * presses it and lets it go, mouse look turns the right stick the way the
 * mouse moved and stops when the mouse does, and neither leaves anything
 * held once the window lets the mouse go.
 */
#include "input_bindings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <time.h>
#endif

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

static void set_env(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* The wheel's clock, stepped by the test. */
static unsigned long long g_clock_ms;

static unsigned long long test_clock(void)
{
    return g_clock_ms;
}

/* A filter that moves a keyboard-or-mouse right trigger to the left one. */
static int g_filter_saw;

static void swap_filter(unsigned port, XBOX_GAMEPAD *pad, const XBOX_GAMEPAD *kbm)
{
    if (port != 0)
        return;
    g_filter_saw = kbm->bAnalogButtons[XBOX_BUTTON_RTRIGGER];
    if (g_filter_saw) {
        pad->bAnalogButtons[XBOX_BUTTON_RTRIGGER] = 0;
        pad->bAnalogButtons[XBOX_BUTTON_LTRIGGER] = (BYTE)g_filter_saw;
    }
}

static XBOX_GAMEPAD sample(void)
{
    XBOX_GAMEPAD pad;

    check(recomp_bindings_sample(0, &pad) == 1, "controller 1 samples");
    return pad;
}

int main(void)
{
    const char *path = "input_mouse_test.json";
    FILE *f = fopen(path, "wb");
    XBOX_GAMEPAD pad;
    int i;

    printf("input_mouse\n");
    if (!f) {
        printf("  FAIL: cannot write %s\n", path);
        return 1;
    }
    /* Controller 1 on the keyboard, so no pad is opened, with the right
     * trigger on the left button, Y on the back side button and the D-pad's
     * left and right on the wheel. A bare "wheel" is not a source, and must
     * be refused without spoiling the rest. */
    fputs("{ \"version\": 1, \"controllers\": [ { \"port\": 1, \"device\": \"keyboard\",\n"
          "  \"bindings\": { \"rtrigger\": [\"mouse:left\"], \"y\": [\"mouse:x1\"],\n"
          "                  \"x\": [\"mouse:wheel\"],\n"
          "                  \"dpad_left\": [\"mouse:wheel_up\"],\n"
          "                  \"dpad_right\": [\"mouse:wheel_down\"] } } ] }\n", f);
    fclose(f);
    set_env("RECOMP_INPUT_CONFIG", path);
    set_env("RECOMP_MOUSE_LOOK", "1");
    set_env("RECOMP_MOUSE_SENSITIVITY", "25");

    check(recomp_mouse_look() == 1, "mouse look is on for a controller that reads something");
    check((recomp_bindings_present_mask() & 1u) != 0, "controller 1 is present");

    /* ---- buttons */
    pad = sample();
    check(pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 0, "trigger starts released");
    recomp_mouse_button(RECOMP_MOUSE_LEFT, 1);
    pad = sample();
    check(pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255, "left button pulls the trigger");
    check(pad.bAnalogButtons[XBOX_BUTTON_Y] == 0, "and nothing else");
    recomp_mouse_button(RECOMP_MOUSE_LEFT, 0);
    pad = sample();
    check(pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 0, "releasing it lets go");
    recomp_mouse_button(RECOMP_MOUSE_X1, 1);
    recomp_mouse_button(RECOMP_MOUSE_LEFT, 1);
    recomp_mouse_release();
    pad = sample();
    check(pad.bAnalogButtons[XBOX_BUTTON_Y] == 0 &&
          pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 0,
          "the window letting the mouse go lets go of everything");

    /* ---- mouse look: right and forward is right and up */
    sleep_ms(30);
    pad = sample();                         /* starts the clock; nothing moved yet */
    check(pad.sThumbRX == 0 && pad.sThumbRY == 0, "a still mouse leaves the stick alone");
    sleep_ms(20);
    recomp_mouse_move(30.0f, -10.0f);
    sleep_ms(20);
    pad = sample();
    printf("  moved (30, -10): stick (%d, %d)\n", pad.sThumbRX, pad.sThumbRY);
    check(pad.sThumbRX > 0, "moving right pushes the stick right");
    check(pad.sThumbRY > 0, "moving forward pushes the stick up");
    check(pad.sThumbRX > pad.sThumbRY, "further moves further");
    /* No dead zone of our own: a slow movement is a small push, not a
     * jump to some starting point. 10 counts in ~40 ms at sensitivity 25
     * is a few percent of a push. */
    check(pad.sThumbRY < 32767 / 5, "a slow movement is a small push");
    check(pad.sThumbLX == 0 && pad.sThumbLY == 0, "the left stick is not touched");

    /* A fast flick is a full push and no more. */
    for (i = 0; i < 3; i++) {
        sleep_ms(15);
        recomp_mouse_move(-2000.0f, 0.0f);
    }
    sleep_ms(15);
    pad = sample();
    check(pad.sThumbRX == -32767, "a flick is a full push left, kept on the stick");

    /* And when the mouse stops, so does the stick. */
    for (i = 0; i < 3; i++) {
        sleep_ms(15);
        pad = sample();
    }
    check(pad.sThumbRX == 0 && pad.sThumbRY == 0, "a mouse that stops lets the stick go");

    /* ---- the wheel: a notch is one press and one release */
    {
        int presses = 0, held = 0, ms;

        /* Time steps by itself here: a loaded machine's sleeps overrun the
         * 40 ms between presses and would merge two (it did, on CI). */
        g_clock_ms = 1000;
        recomp_mouse_wheel_clock(test_clock);
        recomp_mouse_wheel(1.0f);
        pad = sample();
        check((pad.wButtons & XBOX_GAMEPAD_DPAD_LEFT) != 0, "a notch up presses its control");
        check((pad.wButtons & XBOX_GAMEPAD_DPAD_RIGHT) == 0, "and not the other way's");
        /* Two more, read at 60 polls a second: three presses in all. */
        recomp_mouse_wheel(0.5f);
        recomp_mouse_wheel(0.5f);           /* two halves are a notch */
        recomp_mouse_wheel(1.0f);
        held = 1;
        presses = 1;
        for (ms = 0; ms < 600; ms += 16) {
            int now;

            g_clock_ms += 16;
            pad = sample();
            now = (pad.wButtons & XBOX_GAMEPAD_DPAD_LEFT) != 0;
            if (now && !held)
                presses++;
            held = now;
        }
        printf("  three notches up: %d presses\n", presses);
        check(presses == 3, "three notches are three presses, none merged");
        check(!held, "and the last is let go");
        recomp_mouse_wheel(-1.0f);
        pad = sample();
        check((pad.wButtons & XBOX_GAMEPAD_DPAD_RIGHT) != 0, "a notch down presses the other");
        recomp_mouse_wheel_clock(NULL);
    }

    /* ---- direct: the title takes the motion, the stick gets none */
    {
        float dx = 0.0f, dy = 0.0f;

        recomp_mouse_set_direct(1);
        recomp_mouse_take(NULL, NULL);
        sleep_ms(20);
        recomp_mouse_move(40.0f, -8.0f);
        sleep_ms(20);
        pad = sample();
        check(pad.sThumbRX == 0 && pad.sThumbRY == 0, "direct: the stick is left alone");
        check(recomp_mouse_take(&dx, &dy) == 1 && dx == 40.0f && dy == -8.0f,
              "direct: the title takes what moved, in counts");
        check(recomp_mouse_take(&dx, &dy) == 0 && dx == 0.0f, "and takes it once");
        check(recomp_mouse_sensitivity() == 25, "the sensitivity setting is the title's to read");
        recomp_mouse_set_direct(0);
    }

    /* ---- a filter sees what the keyboard and mouse press */
    {
        recomp_bindings_set_filter(swap_filter);
        recomp_mouse_button(RECOMP_MOUSE_LEFT, 1);
        pad = sample();
        check(g_filter_saw == 255, "the filter is told the mouse pulls the trigger");
        check(pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 0 &&
              pad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 255,
              "and what it changes is what the title reads");
        recomp_mouse_button(RECOMP_MOUSE_LEFT, 0);
        recomp_bindings_set_filter(NULL);
    }

    remove(path);
    if (failures)
        printf("input_mouse: %d failure(s)\n", failures);
    else
        printf("input_mouse: ok\n");
    return failures ? 1 : 0;
}
