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
     * trigger on the left button and Y on the back side button. The
     * wheel is not a source, and must be refused without spoiling the rest. */
    fputs("{ \"version\": 1, \"controllers\": [ { \"port\": 1, \"device\": \"keyboard\",\n"
          "  \"bindings\": { \"rtrigger\": [\"mouse:left\"], \"y\": [\"mouse:x1\"],\n"
          "                  \"x\": [\"mouse:wheel\"] } } ] }\n", f);
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

    remove(path);
    if (failures)
        printf("input_mouse: %d failure(s)\n", failures);
    else
        printf("input_mouse: ok\n");
    return failures ? 1 : 0;
}
