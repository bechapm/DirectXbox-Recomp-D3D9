/*
 * recomp_mods.h - the toolkit's side of mods (recomp_mod.h is the mods').
 */
#ifndef RECOMP_MODS_H
#define RECOMP_MODS_H

#include <stdint.h>

/* What the program loading the mods has of the title: the game's HLE
 * (hle_d3d8.c), or d3d8_replay with a capture in its place. The loader
 * itself knows neither, so it builds into both. A NULL function answers
 * a mod with nothing: 0, NULL, or no note stored. */
typedef struct RecompModsHost {
    int         replaying;
    uint32_t    (*title_id)(void);
    const char *(*title_name)(void);
    void       *(*guest_pointer)(uint32_t va);
    uint32_t    (*vertex_constants)(uint32_t first, uint32_t count, float (*out)[4]);
    void        (*capture_note)(uint32_t tag, const void *data, uint32_t bytes);
} RecompModsHost;

/* Loads the mods beside the program, once; later calls do nothing. Both
 * callers make it before their Direct3D device, so that a mod's device
 * hooks and vertex HLSL are in place for it. host is copied. */
void recomp_mods_load(const RecompModsHost *host);

/* Once a frame, after it is shown: the mods' on_frame calls. */
void recomp_mods_frame(void);

/* Just before a draw is written to a frame capture (hle_d3d8_record.c):
 * the mods' capture_on_draw calls. */
void recomp_mods_capture_draw(void);

/* A MOD_NOTE chunk reached in a replay: the mods' replay_on_note calls. */
void recomp_mods_replay_note(uint32_t tag, const void *data, uint32_t bytes);

#endif
