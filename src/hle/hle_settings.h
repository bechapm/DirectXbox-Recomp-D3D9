/*
 * hle_settings.h - the settings a player can change while a title runs.
 *
 * What the desktop's F9 and F10 do, for a host with no keyboard: Android's
 * in-game menu (src/host/android_main.c) calls these. Each takes effect at
 * the next frame and needs no restart. Any thread.
 */
#ifndef XBOXRECOMP_HLE_SETTINGS_H
#define XBOXRECOMP_HLE_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The frame-rate counter in the top-left corner (F9). */
int  hle_fps_overlay(void);
void hle_set_fps_overlay(int on);

/* The frame cap (F10): "adaptive", "60", "30" or "off". */
const char *hle_frame_cap(void);
void        hle_set_frame_cap(const char *name);

#ifdef __cplusplus
}
#endif

#endif
