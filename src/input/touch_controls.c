/*
 * touch_controls.c - the on-screen controller (touch_controls.h).
 */
#include "touch_controls.h"
#include "recomp_config.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The controls. Where they go is lay_out's ("Edges"); what each looks like
 * and how it reads follows Steam Link's controller, as DroidDeck draws it. */
enum {
    C_LSTICK, C_RSTICK, C_DPAD, C_A, C_B, C_X, C_Y,
    C_WHITE, C_BLACK, C_LTRIG, C_RTRIG, C_BACK, C_START, C_LTHUMB, C_RTHUMB,
    C_COUNT
};

typedef struct {
    int      kind;
    unsigned rgb;
    int      analog;            /* bAnalogButtons index, or -1 */
    unsigned bits;              /* wButtons bits */
} control;

static const control k_controls[C_COUNT] = {
    [C_LSTICK] = { TOUCH_SHAPE_STICK,  0xE6E9EC, -1, 0 },
    [C_RSTICK] = { TOUCH_SHAPE_STICK,  0xE6E9EC, -1, 0 },
    [C_DPAD]   = { TOUCH_SHAPE_DPAD,   0xE6E9EC, -1, 0 },
    [C_A]      = { TOUCH_SHAPE_BUTTON, 0x488530, 0, 0 },
    [C_B]      = { TOUCH_SHAPE_BUTTON, 0xD34425, 1, 0 },
    [C_X]      = { TOUCH_SHAPE_BUTTON, 0x173CB8, 2, 0 },
    [C_Y]      = { TOUCH_SHAPE_BUTTON, 0xEBC846, 3, 0 },
    [C_WHITE]  = { TOUCH_SHAPE_PILL,   0xF4F4F4, 5, 0 },
    [C_BLACK]  = { TOUCH_SHAPE_PILL,   0x6A6D72, 4, 0 },
    [C_LTRIG]  = { TOUCH_SHAPE_PILL,   0xE6E9EC, 6, 0 },
    [C_RTRIG]  = { TOUCH_SHAPE_PILL,   0xE6E9EC, 7, 0 },
    [C_BACK]   = { TOUCH_SHAPE_PILL,   0xE6E9EC, -1, XBOX_GAMEPAD_BACK },
    [C_START]  = { TOUCH_SHAPE_PILL,   0xE6E9EC, -1, XBOX_GAMEPAD_START },
    [C_LTHUMB] = { TOUCH_SHAPE_PILL,   0xE6E9EC, -1, XBOX_GAMEPAD_LEFT_THUMB },
    [C_RTHUMB] = { TOUCH_SHAPE_PILL,   0xE6E9EC, -1, XBOX_GAMEPAD_RIGHT_THUMB },
};

/* Where each control is on the current screen, in pixels: a centre and a
 * box (a disc's box is its diameter both ways). */
typedef struct {
    float x, y, w, h;
} place;

#define MAX_FINGERS   10
#define FADE_AFTER_MS 4000
#define FADED         0.3f

typedef struct {
    SDL_FingerID id;
    int          control;       /* C_*, or -1 for a touch on nothing */
    float        x, y;          /* pixels */
} finger;

enum { MODE_OFF, MODE_AUTO, MODE_ON };

static struct {
    SDL_Mutex *lock;
    int        mode;
    int        started;
    int        shown;           /* auto: shown now */
    float      opacity, scale;
    int        w, h;            /* the screen the shapes were last asked for */
    int        laid_w, laid_h;  /* the screen `places` is for */
    place      places[C_COUNT];
    finger     fingers[MAX_FINGERS];
    int        nfingers;
    Uint64     last_touch_ms;
} T = { .mode = MODE_OFF, .opacity = 0.45f, .scale = 1.0f, .w = 1920, .h = 1080 };

static float minf(float a, float b) { return a < b ? a : b; }

static void put(int c, float x, float y, float w, float h)
{
    T.places[c].x = x;
    T.places[c].y = y;
    T.places[c].w = w;
    T.places[c].h = h;
}

/* The "Edges" layout, chosen from the mock-ups (Oct 2026): everything along
 * the left and right edges so the middle of the picture stays clear. Large
 * triggers in the top corners, where index fingers rest, White and Black
 * under them, the d-pad and face buttons at mid height, the sticks at the
 * bottom and their clicks just inside them; Back and Start top centre.
 *
 * Drawn on a 1920x1080 screen; u scales it by the screen's height, and the
 * two sides are measured from their own edges, so a wider screen moves them
 * apart rather than stretching them. */
static void lay_out(void)
{
    float w = (float)T.w, h = (float)T.h;
    float u = h / 1080.0f * T.scale;
#define L(x) ((x) * u)
#define R(x) (w - (x) * u)

    put(C_LTRIG,  L(200), L(100), L(320), L(140));
    put(C_RTRIG,  R(200), L(100), L(320), L(140));
    put(C_WHITE,  L(140), L(262), L(200), L(96));
    put(C_BLACK,  R(140), L(262), L(200), L(96));
    put(C_DPAD,   L(240), L(520), L(236), L(236));
    put(C_LSTICK, L(262), h - L(220), L(300), L(300));
    put(C_RSTICK, R(262), h - L(220), L(300), L(300));
    put(C_LTHUMB, L(520), h - L(70), L(130), L(72));
    put(C_RTHUMB, R(520), h - L(70), L(130), L(72));
    put(C_BACK,   w / 2 - L(100), L(56), L(170), L(72));
    put(C_START,  w / 2 + L(100), L(56), L(170), L(72));
    put(C_A, R(240),       L(620), L(116), L(116));
    put(C_B, R(240) + L(100), L(520), L(116), L(116));
    put(C_X, R(240) - L(100), L(520), L(116), L(116));
    put(C_Y, R(240),       L(420), L(116), L(116));
#undef L
#undef R
    T.laid_w = T.w;
    T.laid_h = T.h;
}

static const place *at(int c)
{
    if (T.laid_w != T.w || T.laid_h != T.h)
        lay_out();
    return &T.places[c];
}

static float cx(int c) { return at(c)->x; }
static float cy(int c) { return at(c)->y; }
static float radius(int c) { return at(c)->w * 0.5f; }

/* How far (x, y) is from a control's centre, in multiples of its size: 1 is
 * its edge. A pill measures against its own box. */
static float reach(int c, float x, float y)
{
    const place *p = at(c);
    float dx = x - p->x, dy = y - p->y;

    if (k_controls[c].kind == TOUCH_SHAPE_PILL) {
        float ax = fabsf(dx) / (p->w * 0.5f), ay = fabsf(dy) / (p->h * 0.5f);
        return ax > ay ? ax : ay;
    }
    return sqrtf(dx * dx + dy * dy) / (p->w * 0.5f);
}

/* The control under (x, y), the nearest when two overlap; -1 for none. */
static int hit(float x, float y, int buttons_only)
{
    int c, best = -1;
    float best_reach = 1.0f;

    for (c = 0; c < C_COUNT; c++) {
        float d;
        if (buttons_only && (k_controls[c].kind == TOUCH_SHAPE_STICK ||
                             k_controls[c].kind == TOUCH_SHAPE_DPAD))
            continue;
        d = reach(c, x, y);
        if (d <= best_reach) {
            best_reach = d;
            best = c;
        }
    }
    return best;
}

static int is_button(int c)
{
    return c >= 0 && k_controls[c].kind != TOUCH_SHAPE_STICK &&
           k_controls[c].kind != TOUCH_SHAPE_DPAD;
}

static finger *find_finger(SDL_FingerID id)
{
    int i;
    for (i = 0; i < T.nfingers; i++)
        if (T.fingers[i].id == id)
            return &T.fingers[i];
    return NULL;
}

static bool SDLCALL on_event(void *user, SDL_Event *e)
{
    float x, y;
    finger *f;

    (void)user;
    /* auto: a pad that arrives after start-up (enumeration is not done
     * when this starts) hides the controls as a press on it would. */
    if (e->type == SDL_EVENT_GAMEPAD_ADDED && T.mode == MODE_AUTO) {
        SDL_LockMutex(T.lock);
        if (T.shown && T.nfingers == 0) {
            T.shown = 0;
            fprintf(stderr, "[INPUT] touch controls hidden: a pad is connected (a touch brings them back)\n");
        }
        SDL_UnlockMutex(T.lock);
        return true;
    }
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION &&
        e->type != SDL_EVENT_FINGER_UP && e->type != SDL_EVENT_FINGER_CANCELED)
        return true;
    if (T.mode == MODE_OFF)
        return true;
    SDL_LockMutex(T.lock);
    x = e->tfinger.x * (float)T.w;
    y = e->tfinger.y * (float)T.h;
    T.last_touch_ms = SDL_GetTicks();
    f = find_finger(e->tfinger.fingerID);
    switch (e->type) {
    case SDL_EVENT_FINGER_DOWN:
        if (!T.shown) {
            /* The first touch only brings the controls back. */
            T.shown = 1;
            break;
        }
        if (!f && T.nfingers < MAX_FINGERS) {
            f = &T.fingers[T.nfingers++];
            f->id = e->tfinger.fingerID;
            f->control = hit(x, y, 0);
            f->x = x;
            f->y = y;
        }
        break;
    case SDL_EVENT_FINGER_MOTION:
        if (f) {
            f->x = x;
            f->y = y;
            /* A finger on a button can slide onto another. */
            if (is_button(f->control) && reach(f->control, x, y) > 1.0f) {
                int over = hit(x, y, 1);
                if (over >= 0)
                    f->control = over;
            }
        }
        break;
    default:                            /* up, cancelled */
        if (e->type == SDL_EVENT_FINGER_CANCELED) {
            T.nfingers = 0;
        } else if (f) {
            *f = T.fingers[--T.nfingers];
        }
        break;
    }
    SDL_UnlockMutex(T.lock);
    return true;
}

static float setting_float(const char *env, const char *key, float def, float lo, float hi)
{
    const char *v = recomp_config_lookup(env, key);
    float f;

    if (!v || !*v)
        return def;
    f = (float)atof(v);
    return f < lo ? lo : f > hi ? hi : f;
}

int recomp_touch_enabled(void)
{
    recomp_touch_init();
    return T.mode != MODE_OFF;
}

static int mode_from(const char *v, int fallback)
{
    if (!v || !*v)
        return fallback;
    if (!strcmp(v, "on") || !strcmp(v, "1"))
        return MODE_ON;
    if (!strcmp(v, "off") || !strcmp(v, "0"))
        return MODE_OFF;
    if (!strcmp(v, "auto"))
        return MODE_AUTO;
    return fallback;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* auto: shown now only with no pad to play with. */
static int no_pad(void)
{
    int n = 0;
    SDL_JoystickID *pads = SDL_GetGamepads(&n);
    SDL_free(pads);
    return n == 0;
}

void recomp_touch_init(void)
{
    if (T.started)
        return;
    T.started = 1;
#if defined(__ANDROID__)
    T.mode = mode_from(recomp_config_lookup("RECOMP_TOUCH_CONTROLS", "touch_controls"), MODE_AUTO);
#else
    /* A desktop has a pad, a keyboard and a mouse; the overlay is a phone's.
     * The settings file's default was "auto" everywhere, so a desktop run
     * showed the controls whenever no pad was connected. */
    T.mode = MODE_OFF;
    (void)mode_from;
#endif
    T.opacity = setting_float("RECOMP_TOUCH_OPACITY", "touch_opacity", 0.45f, 0.1f, 1.0f);
    T.scale = setting_float("RECOMP_TOUCH_SCALE", "touch_scale", 1.0f, 0.5f, 2.0f);
    /* The lock and the watch exist whatever the mode, so the controls can
     * be switched on while the title runs (recomp_touch_configure). */
    T.lock = SDL_CreateMutex();
    if (!T.lock) {
        T.mode = MODE_OFF;
        return;
    }
    T.shown = T.mode == MODE_ON || (T.mode == MODE_AUTO && no_pad());
    T.last_touch_ms = SDL_GetTicks();
    SDL_AddEventWatch(on_event, NULL);
    if (T.mode != MODE_OFF)
        fprintf(stderr, "[INPUT] touch controls %s (%s), opacity %.2f, scale %.2f\n",
                T.mode == MODE_ON ? "on" : "auto", T.shown ? "shown" : "hidden until a touch",
                T.opacity, T.scale);
}

void recomp_touch_configure(const char *mode, float opacity, float scale)
{
    recomp_touch_init();
    if (!T.lock)
        return;
    SDL_LockMutex(T.lock);
#if defined(__ANDROID__)
    if (mode)
        T.mode = mode_from(mode, T.mode);
#else
    (void)mode;                         /* Android only (recomp_touch_init) */
#endif
    if (opacity > 0.0f)
        T.opacity = clampf(opacity, 0.1f, 1.0f);
    if (scale > 0.0f && clampf(scale, 0.5f, 2.0f) != T.scale) {
        T.scale = clampf(scale, 0.5f, 2.0f);
        T.laid_w = 0;                   /* lay out again */
    }
    T.shown = T.mode == MODE_ON || (T.mode == MODE_AUTO && no_pad());
    if (T.mode == MODE_OFF)
        T.nfingers = 0;
    T.last_touch_ms = SDL_GetTicks();
    SDL_UnlockMutex(T.lock);
}

const char *recomp_touch_mode(void)
{
    recomp_touch_init();
    return T.mode == MODE_ON ? "on" : T.mode == MODE_OFF ? "off" : "auto";
}

float recomp_touch_opacity(void) { recomp_touch_init(); return T.opacity; }
float recomp_touch_scale(void)   { recomp_touch_init(); return T.scale; }

/* What one stick finger says, as a deflection of -32767..32767 a side. */
static void stick_value(int c, const finger *f, SHORT *sx, SHORT *sy)
{
    float r = radius(c);
    float dx = (f->x - cx(c)) / r, dy = (f->y - cy(c)) / r;
    float d = sqrtf(dx * dx + dy * dy);

    if (d > 1.0f) {
        dx /= d;
        dy /= d;
    }
    *sx = (SHORT)(dx * 32767.0f);
    *sy = (SHORT)(-dy * 32767.0f);      /* the pad's y is up */
}

/* The d-pad's directions under one finger: eight sectors, each direction
 * covering 67.5 degrees so a diagonal presses two; nothing in the middle. */
static unsigned dpad_bits(const finger *f)
{
    float dx = f->x - cx(C_DPAD), dy = f->y - cy(C_DPAD);
    float a;
    unsigned bits = 0;

    if (sqrtf(dx * dx + dy * dy) < radius(C_DPAD) * 0.22f)
        return 0;
    a = atan2f(-dy, dx) * (180.0f / 3.14159265f);
    if (a < 0)
        a += 360.0f;
    if (a < 67.5f || a > 292.5f)   bits |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (a >= 22.5f && a <= 157.5f)  bits |= XBOX_GAMEPAD_DPAD_UP;
    if (a >= 112.5f && a <= 247.5f) bits |= XBOX_GAMEPAD_DPAD_LEFT;
    if (a >= 202.5f && a <= 337.5f) bits |= XBOX_GAMEPAD_DPAD_DOWN;
    return bits;
}

static int pad_pressed(const XBOX_GAMEPAD *p)
{
    int i;
    if (p->wButtons)
        return 1;
    for (i = 0; i < 8; i++)
        if (p->bAnalogButtons[i] > XBOX_ANALOG_BUTTON_THRESHOLD)
            return 1;
    return abs(p->sThumbLX) > 12000 || abs(p->sThumbLY) > 12000 ||
           abs(p->sThumbRX) > 12000 || abs(p->sThumbRY) > 12000;
}

static void take_larger(SHORT *dst, SHORT v)
{
    if (abs(v) > abs(*dst))
        *dst = v;
}

void recomp_touch_apply(XBOX_GAMEPAD *pad)
{
    int i;

    recomp_touch_init();
    if (T.mode == MODE_OFF || !pad)
        return;
    SDL_LockMutex(T.lock);
    if (T.mode == MODE_AUTO && T.shown && T.nfingers == 0 && pad_pressed(pad)) {
        T.shown = 0;                    /* a real pad is being played */
        fprintf(stderr, "[INPUT] touch controls hidden: a pad is in use (a touch brings them back)\n");
    }
    for (i = 0; i < T.nfingers && T.shown; i++) {
        const finger *f = &T.fingers[i];
        const control *c;
        SHORT sx, sy;

        if (f->control < 0)
            continue;
        c = &k_controls[f->control];
        switch (c->kind) {
        case TOUCH_SHAPE_STICK:
            stick_value(f->control, f, &sx, &sy);
            if (f->control == C_LSTICK) {
                take_larger(&pad->sThumbLX, sx);
                take_larger(&pad->sThumbLY, sy);
            } else {
                take_larger(&pad->sThumbRX, sx);
                take_larger(&pad->sThumbRY, sy);
            }
            break;
        case TOUCH_SHAPE_DPAD:
            pad->wButtons |= (WORD)dpad_bits(f);
            break;
        default:
            pad->wButtons |= (WORD)c->bits;
            if (c->analog >= 0)
                pad->bAnalogButtons[c->analog] = 255;
            break;
        }
    }
    SDL_UnlockMutex(T.lock);
}

int recomp_touch_shapes(TouchShape *out, int max, int w, int h, float *opacity)
{
    int c, n = 0, i;
    Uint64 idle;

    if (T.mode == MODE_OFF || !T.lock || w <= 0 || h <= 0)
        return 0;
    SDL_LockMutex(T.lock);
    T.w = w;
    T.h = h;
    if (!T.shown) {
        SDL_UnlockMutex(T.lock);
        return 0;
    }
    idle = SDL_GetTicks() - T.last_touch_ms;
    if (opacity)
        *opacity = T.opacity * (T.nfingers == 0 && idle > FADE_AFTER_MS ? FADED : 1.0f);
    for (c = 0; c < C_COUNT && n < max; c++) {
        TouchShape *s = &out[n++];
        const control *k = &k_controls[c];

        memset(s, 0, sizeof *s);
        s->kind = k->kind;
        s->x = cx(c);
        s->y = cy(c);
        s->r = radius(c);
        s->rgb = k->rgb;
        if (k->kind == TOUCH_SHAPE_PILL) {
            /* Rounded squares, as DroidDeck draws its shoulders. */
            s->w = at(c)->w;
            s->h = at(c)->h;
            s->r = minf(s->w, s->h) * 0.22f;
        }
        for (i = 0; i < T.nfingers; i++) {
            const finger *f = &T.fingers[i];
            if (f->control != c)
                continue;
            if (k->kind == TOUCH_SHAPE_STICK) {
                SHORT sx, sy;
                stick_value(c, f, &sx, &sy);
                s->kx = (float)sx / 32767.0f * s->r;
                s->ky = -(float)sy / 32767.0f * s->r;
                s->pressed = 1;
            } else if (k->kind == TOUCH_SHAPE_DPAD) {
                s->pressed |= dpad_bits(f);
            } else {
                s->pressed = 1;
            }
        }
    }
    SDL_UnlockMutex(T.lock);
    return n;
}
