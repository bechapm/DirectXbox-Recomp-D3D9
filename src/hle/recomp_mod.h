/*
 * Mods: libraries a player drops beside the game, loaded when the game's
 * Direct3D device is made (recomp_mods.c). A mod changes or adds to a title
 * without being built into it -- RTX Remix support for one game, a tool,
 * a fix someone wants to try -- so the title's own build stays the port.
 *
 * Where: the folder `mods` beside the executable; every .dll (Windows) or
 * .so (elsewhere) in it, in name order. RECOMP_MOD_LIBS=0 loads none;
 * RECOMP_MOD_LIBS_DIR names another folder. (Not to be confused with a
 * title's own asset mods, such as split2-recomp's RECOMP_MODS folders.)
 *
 * What a mod exports: one C function,
 *
 *     int recomp_mod_init(const RecompModApi *api);
 *
 * called once, on the thread making the device, before the title's first
 * draw. It returns nonzero to stay loaded; zero (not this title, say) and
 * it is unloaded again. The api it is handed lives for the whole run.
 *
 * This header is the whole contract, and stands alone: a mod is built
 * against a copy of it, not against the toolkit. Fields are only ever
 * added at the end; a mod checks `size` before using one newer than it
 * knows, and `version` names the newest set.
 */
#ifndef RECOMP_MOD_H
#define RECOMP_MOD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_MOD_API_VERSION 1

typedef struct RecompModApi {
    uint32_t version;               /* RECOMP_MOD_API_VERSION when made */
    uint32_t size;                  /* sizeof(RecompModApi) when made */

    /* ---- version 1 ---- */

    /* The title running: its certificate's title ID (0x4553000A for
     * TimeSplitters 2), and its name, UTF-8 (NULL if it has none). Which
     * release it is the mod reads from the certificate in guest memory
     * (the XBE header is at 0x00010000, as on the console). */
    uint32_t    (*title_id)(void);
    const char *(*title_name)(void);

    /* A host pointer to guest address va. Guest memory is one block, so
     * the pointer is good for as far as the title's own data runs. */
    void       *(*guest_pointer)(uint32_t va);

    /* fn(user) once a frame, after the title's frame is shown. Up to 16
     * across all mods; returns 0 when full. */
    int         (*on_frame)(void (*fn)(void *user), void *user);

    /* The title's vertex-program constants as they stand for the draw
     * being made, count registers from first, numbered as the toolkit's
     * logs and xbox_D3D8SetInterpRegisters number them. Returns how many
     * were copied. */
    uint32_t    (*vertex_constants)(uint32_t first, uint32_t count, float (*out)[4]);

    /* The camera RTX Remix is told for each vertex-program draw under the
     * D3D9 renderer (xbox_D3D8SetRemixViewSource): source fills a D3D
     * row-vector VIEW and returns nonzero, or returns 0 for the default.
     * NULL stops asking. Nothing happens under other renderers. */
    void        (*set_remix_view_source)(int (*source)(float view[16]));

    /* A line in the game's log (stderr), prefixed "[MOD]". */
    void        (*log)(const char *line);
} RecompModApi;

typedef int (*RecompModInitFn)(const RecompModApi *api);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_MOD_H */
