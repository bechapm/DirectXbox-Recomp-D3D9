/*
 * launcher.h -- between launcher.c, which is what the launcher is, and the
 * front end that gives it a window on one kind of host.
 *
 * launcher.c holds the settings, the tabs, the pad and the drawing, and
 * knows nothing of windows. A front end owns the window and its events and
 * turns them into the handful of calls below; it also answers the few
 * questions that have no portable answer (lhost_*). Windows:
 * launcher_win32.c. Everywhere else: launcher_sdl.c, on SDL3, which the pads
 * already need.
 */
#ifndef XBOXRECOMP_LAUNCHER_H
#define XBOXRECOMP_LAUNCHER_H

#include <stddef.h>
#include <stdint.h>

#define LAUNCHER_WINDOW_W 900
#define LAUNCHER_WINDOW_H 800           /* eight Video rows, their help, the footer */

/* Keys, as the front end reports them and as launcher.c asks after them. A
 * letter or a digit is itself, upper case ('A'..'Z', '0'..'9'), as a
 * Windows virtual key is; the rest are below, clear of both. */
enum {
    LKEY_UP = 0x100, LKEY_DOWN, LKEY_LEFT, LKEY_RIGHT,
    LKEY_RETURN, LKEY_ESCAPE, LKEY_TAB, LKEY_BACK, LKEY_SPACE,
    LKEY_LSHIFT, LKEY_RSHIFT, LKEY_LCTRL, LKEY_RCTRL
};

/* Mouse buttons, as the front end reports them to launcher_mouse_button. */
enum { LMOUSE_LEFT = 1, LMOUSE_RIGHT, LMOUSE_MIDDLE, LMOUSE_X1, LMOUSE_X2 };

/* ---- launcher.c, for the front end ---- */

/* Before the window: finds the game, reads the settings and bindings. */
void        launcher_init(void);
/* The game's own name, for the window's caption. */
const char *launcher_title(void);

void launcher_key(int key, int shift);  /* a key pressed (repeats too) */
/* A character typed: printable ASCII, 8 Backspace, 3 copy, 22 paste --
 * what Windows' WM_CHAR gives for Ctrl+C and Ctrl+V. */
void launcher_char(unsigned ch);
void launcher_click(int x, int y);      /* the left button, in window units */
/* Any mouse button pressed (LMOUSE_*), the left one included, before its
 * launcher_click. Returns 1 when the press was the answer to a binding
 * being captured: it means nothing else, and the click is not sent. */
int  launcher_mouse_button(int button);
void launcher_hover(int x, int y);
/* The wheel turned: rows > 0 scrolls the list down, by whole rows, with the
 * pointer at (x, y) in window units. */
void launcher_wheel(int rows, int x, int y);
void launcher_tick(void);               /* sixty times a second: pad, glow */
void launcher_resize(int w, int h);     /* the client area, in window units */
void launcher_draw(void);               /* into the theme's back buffer */

/* ---- the front end, for launcher.c ---- */

uint64_t lhost_ticks(void);             /* milliseconds, from anywhere */
int      lhost_key_held(int key);       /* down right now */
/* The clipboard's text, its first line or all of it, into `out`. 0 when
 * there is none. */
int      lhost_paste(char *out, size_t n);
int      lhost_copy(const char *text);  /* 1 when it is on the clipboard */
/* Start `exe` with `dir` as its working directory. Says why itself when
 * it cannot, and returns 0. */
int      lhost_launch(const char *exe, const char *dir);
void     lhost_close(void);             /* end the launcher, after this event */
/* Run fn on a thread of its own, detached. 0 when there is no thread. */
int      lhost_thread(void (*fn)(void));
/* A flag one thread raises for another to see, with what was written
 * before it. */
void     lhost_publish(volatile long *flag, long value);
long     lhost_read(volatile long *flag);
/* `path` made absolute, or copied as it is. */
void     lhost_full_path(const char *path, char *out, size_t n);

#endif /* XBOXRECOMP_LAUNCHER_H */
