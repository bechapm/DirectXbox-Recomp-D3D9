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

#define RECOMP_MOD_API_VERSION 2

/* ---- version 2: the Direct3D 9 renderer (RECOMP_D3D8_BACKEND=d3d9) ----
 *
 * A mod hears of each draw the D3D9 renderer makes, after the renderer has
 * set the device up for it and before it is drawn, and again after. In
 * between it may change the device's state for that one draw -- what it
 * changes it puts back in `after` -- or have the draw left out. Under the
 * other renderers none of this is called. */

/* What the renderer knows of a draw. Fields are only added at the end. */
typedef struct RecompModDraw {
    uint32_t size;                  /* sizeof(RecompModDraw) when made */
    void    *device;                /* the IDirect3DDevice9 */
    int      title;                 /* the title's own draw, not the toolkit's
                                       (its overlay, its display passes) */
    int      two_d;                 /* the title's draw is screen-space 2D: no
                                       projection, or an orthographic one */
    int      title_2d;              /* fixed function: the title's own 2D test
                                       took it for 2D (two_d is then set too) */
    float    clip_w;                /* then: its mean clip w, or 0 */
    int      offscreen;             /* drawn into a texture, not the back buffer */
    int      program;               /* through a vertex program */
    uint64_t program_id;            /* the program's bytecode hash, or 0 */
    uint32_t primitive;             /* D3DPRIMITIVETYPE */
    uint32_t primitives;
    void    *textures[8];           /* IDirect3DBaseTexture9 per stage, or NULL */
    uint64_t texture_ids[8];        /* what each holds: stable across a replay */
    uint32_t screen_copies;         /* bit per stage: a copy of the screen */
    int      depth_state;           /* a depth state is bound; then: */
    int      depth_enable, depth_write;
    uint32_t depth_func;            /* D3DCMPFUNC */
    int      projection_set;        /* fixed function: the title set a projection */
    float    projection[16];        /* fixed function: it, as the title gave it */
    float    world[16], view[16];   /* fixed function: as the device has them */
} RecompModDraw;

#define RECOMP_DRAW_AS_IS 0         /* before's answers */
#define RECOMP_DRAW_SKIP  1         /* left out: before changed nothing */

typedef int  (*RecompModBeforeDraw)(RecompModDraw *draw, void *user);
typedef void (*RecompModAfterDraw)(RecompModDraw *draw, void *user);
typedef void (*RecompModDeviceFn)(void *device, void *user);
/* what: RECOMP_CLEAR_* bits; whole: the whole surface, not rectangles. */
typedef void (*RecompModClearFn)(uint32_t what, float z, int whole, void *user);
#define RECOMP_CLEAR_COLOR   1u
#define RECOMP_CLEAR_DEPTH   2u
#define RECOMP_CLEAR_STENCIL 4u

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

    /* ---- version 2: the Direct3D 9 renderer (see above) ---- */

    /* Each returns 0 if there is no room left (8 of each across mods). */
    int   (*d3d9_on_device)(RecompModDeviceFn fn, void *user);    /* made */
    int   (*d3d9_on_draw)(RecompModBeforeDraw before, RecompModAfterDraw after,
                          void *user);
    int   (*d3d9_on_clear)(RecompModClearFn fn, void *user);
    int   (*d3d9_on_present)(RecompModDeviceFn fn, void *user);   /* before Present */

    /* HLSL put in front of every vertex program, from init only (before
     * the first is compiled). It may define RHI_MOD_POSITION(p), which is
     * then run on each output position (float4, clip space); the
     * renderer's own rhi_depth_clamp (c254) is declared before it, and its
     * w is always 0. Returns 0 if it does not fit. */
    int   (*d3d9_vertex_hlsl)(const char *hlsl);

    /* A render state as the renderer last set it: returns 0 if it has not
     * set it (the device's default stands), else fills value. */
    int   (*d3d9_render_state)(uint32_t state, uint32_t *value);

    /* The fixed-function PROJECTION for the draw in hand, with the
     * renderer's viewport correction folded in, as it sets the title's. */
    void  (*d3d9_set_projection)(const float m[16]);

    /* On: a CPU-filled texture whose level 0 changes after a draw used it
     * is made anew (a new IDirect3DTexture9, its texels copied over), for
     * a D3D9 runtime that knows a texture by its contents, read once. */
    void  (*d3d9_remake_written_textures)(int on);
} RecompModApi;

typedef int (*RecompModInitFn)(const RecompModApi *api);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_MOD_H */
