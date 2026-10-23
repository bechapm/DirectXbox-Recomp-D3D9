/*
 * launcher_theme_buf.h -- the back buffer, between the arithmetic that
 * draws into it (launcher_theme.c) and the host half that owns it and its
 * text (launcher_theme_gdi.c on Windows, launcher_theme_coretext.c on
 * macOS, launcher_theme_freetype.c on Linux), and on Linux the window that
 * shows it (launcher_sdl.c). Not for the launcher itself, which goes
 * through launcher_theme.h.
 */
#ifndef XBOXRECOMP_LAUNCHER_THEME_BUF_H
#define XBOXRECOMP_LAUNCHER_THEME_BUF_H

#include <stdint.h>

/* 0x00RRGGBB, top row first, theme_w by theme_h pixels: the window's size
 * times theme_scale, which is the display's pixels per window unit (1 on
 * Windows, where the launcher asks not to be scaled; 2 on a Retina Mac).
 * Every ThemeRect is in window units and is scaled on the way in. */
extern uint32_t *theme_px;
extern int       theme_w, theme_h;
extern double    theme_scale;

#endif /* XBOXRECOMP_LAUNCHER_THEME_BUF_H */
