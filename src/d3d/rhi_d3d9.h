/**
 * rhi_d3d9.h -- what only the Direct3D 9 backend (rhi_d3d9.c) does.
 *
 * The D3D9 backend exists for tools that hook Direct3D 9 and work from what
 * a title draws there. The most demanding, RTX Remix, does not read shaders:
 * it rebuilds the scene from the fixed-function state a D3D9 title sets
 * around each draw -- WORLD, VIEW and PROJECTION, the texture in stage 0,
 * the vertex declaration's POSITION, NORMAL, COLOR0 and TEXCOORD -- and, for
 * a draw through a vertex shader, from the positions that shader writes,
 * carried back into world space through VIEW and PROJECTION. So the backend
 * draws the way a PC D3D9 game of the era would.
 * docs/technical/d3d9-backend.md is the design.
 *
 * So under this backend the renderer draws differently from D3D11 and
 * Vulkan in three places, and these are the calls it makes there:
 *
 *  - A fixed-function draw is drawn by D3D9's own fixed-function pipeline:
 *    its transforms, lights, material and texture stages are handed over as
 *    D3D9 state (rhi_d3d9_set_*), and no shader is bound.
 *  - A vertex program is compiled to Shader Model 3 (vs_3_0, which D3D9
 *    pairs only with ps_3_0), and the pixel stage under it is the texture
 *    stages again, as a ps_3_0 d3d8_shaders.c builds with each stage's
 *    operation written into the code: the fixed-function pixel shader is
 *    Shader Model 4 and up (integer constants, bit tests).
 *  - A register-combiner program is translated as under the other backends,
 *    into Shader Model 3 under a vertex program and Shader Model 2 (ps_2_a)
 *    under fixed function, which D3D9 pairs only with ps_2_x
 *    (NV2ACombinerState.host). A tool that ignores pixel shaders (Remix)
 *    sees only the texture stages; the shaders shape what is rasterised.
 *  - Each program draw's projection is handed to mods
 *    (rhi_d3d9_program_projection), so that a mod can tell a tool how to
 *    follow the positions the program writes back into the world.
 *
 * Every call here does nothing unless the D3D9 backend is the one in use,
 * so the renderer calls them without asking first where that is simpler.
 *
 * Numbers are D3D9's, which for the render states used here are PC
 * Direct3D 8's as well -- the numbering src/d3d keeps its own state in
 * (d3d8_xbox.h). One texture-stage state is not: d3d8_xbox.h keeps
 * TEXTURETRANSFORMFLAGS at 23, and D3D9's is RHI_D3D9_TSS_TEXTURETRANSFORMFLAGS.
 * No platform header is included: this compiles on Linux.
 */
#ifndef XBOXRECOMP_RHI_D3D9_H
#define XBOXRECOMP_RHI_D3D9_H

#include <stdint.h>

#include "rhi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* D3D9 numbers that src/d3d's own enumerations (d3d8_xbox.h) differ on. */
#define RHI_D3D9_TSS_TEXTURETRANSFORMFLAGS  24

/* Whether the D3D9 backend is the one drawing. */
int  rhi_d3d9_active(void);

/* ---- vertex programs ------------------------------------------------------------- */

/* The constant registers a vertex program's uniform buffers are uploaded to
 * (D3D9 has no constant buffers): b1, its c0-c191, at c0; b2, the
 * screen-space undo, at c192; b3, its input registers' current values, at
 * c196. d3d8_vsh.c declares them there under RHI_D3D9. */
#define RHI_D3D9_VSH_CONST_REG        0
#define RHI_D3D9_VSH_SCREENSPACE_REG  192
#define RHI_D3D9_VSH_VERTEXDATA_REG   196

/* The D3D9 input semantic NV2A vertex register v<reg> has by the XDK's
 * register convention: v0 POSITION, v1 BLENDWEIGHT, v2 NORMAL, v3 and v4
 * COLOR0 and 1, v9-v12 TEXCOORD0-3. It is the fallback: d3d8_vsh.c names a
 * program's inputs by where they go (vsh_input_attrs), and rhi_d3d9.c reads
 * any plain "ATTR<reg>" element through this. *index receives the semantic
 * index. */
const char *rhi_d3d9_attr_semantic(int reg, int *index);

/* ---- fixed function ---------------------------------------------------------------- */

/* D3DRENDERSTATETYPE / D3DTEXTURESTAGESTATETYPE / D3DTRANSFORMSTATETYPE
 * values, set as they are (cached, so setting the same value again is
 * free). Only the fixed-function states belong here; blend, depth, stencil,
 * raster and sampler state come from rhi.h's state objects. The alpha test
 * (D3DRS_ALPHATESTENABLE, which D3D9 applies under a pixel shader as well)
 * is kept for the title's own draws: see rhi_d3d9_title_draw. */
void rhi_d3d9_set_render_state(uint32_t state, uint32_t value);
/* The next draw is the title's, under the alpha test it set. Every other
 * draw -- the overlay, the display passes -- is drawn without one. `two_d`
 * is the renderer's verdict that it is screen-space (d3d8_SetTwoDSqueeze);
 * `offscreen`, that it goes to a texture the title renders into rather than
 * its back buffer. Both are passed on to mods (RecompModDraw). */
void rhi_d3d9_title_draw(int two_d, int offscreen);
/* After rhi_d3d9_title_draw: the title's own test (xbox_D3D8SetScreenSpaceTest)
 * called the next draw, a fixed-function one through the world's projection,
 * screen-space, which mods are told (RecompModDraw.title_2d). `w` is the
 * draw's clip w (its depth in front of the camera), the same at every vertex
 * of such a draw, or 0 if not known. */
void rhi_d3d9_title_says_2d(float w);
/* Whether a vertex program that is a plain transform is drawn by the
 * fixed-function pipeline: when a mod asked for it, or as
 * RECOMP_D3D9_FIXED_TRANSFORM (settings file d3d9_fixed_transform) says, 0
 * never and 1 always. Off without a mod, since D3D9 itself gains nothing and
 * has no depth clamp there: a vertex past the far plane is cut. */
int  rhi_d3d9_fixed_transform_on(void);
/* After rhi_d3d9_title_draw: the next draw is that program's, so drawn;
 * mods are told it is the program's all the same (RecompModDraw). */
void rhi_d3d9_title_fixed_transform(RhiShader *program);
void rhi_d3d9_set_texture_stage(uint32_t stage, uint32_t type, uint32_t value);
/* A row-vector matrix, D3DMATRIX order (m[row * 4 + column]). */
void rhi_d3d9_set_transform(uint32_t type, const float m[16]);
/* `light` is a D3DLIGHT8 (the same 104 bytes as a D3DLIGHT9), or NULL to
 * leave the light as it is and only enable or disable it. */
void rhi_d3d9_set_light(uint32_t index, const void *light, int enable);
/* A D3DMATERIAL8 (the same 68 bytes as a D3DMATERIAL9). */
void rhi_d3d9_set_material(const void *material);

/* ---- what replaces a draw -------------------------------------------------------- */

/* D3D8's Clear with rectangles (d3d8_clear.c), by D3D9's own Clear, which
 * takes rectangles: a quad would be a draw, and Remix would take it for
 * geometry. `what` is RHI_CLEAR_DEPTH, RHI_CLEAR_STENCIL and
 * RHI_D3D9_CLEAR_COLOR; the rectangles are in the target's pixels and are
 * clipped to width x height. 0 on success, -1 when the backend is not D3D9. */
enum { RHI_D3D9_CLEAR_COLOR = 4 };
int  rhi_d3d9_clear_rects(RhiView *color, RhiView *depth, uint32_t width, uint32_t height,
                          const RhiRect *rects, uint32_t count, uint32_t what,
                          const float rgba[4], float z, uint8_t stencil);

/* A rectangle of a sampled view's image (its first level) copied, scaled
 * if the rectangles differ, into a render-target view: StretchRect, which
 * converts between render-target formats as it goes. NULL is the whole of
 * either. For the screen copies (d3d8_screencopy.c), which are draws under
 * the other backends. 0 on success. */
int  rhi_d3d9_copy_rect(RhiView *src, const RhiRect *src_rect, RhiView *dst,
                        const RhiRect *dst_rect);

/* ---- what a D3D9 tool is told ---------------------------------------------------- */

/* The projection of the next draw's vertex program, as a D3D row-vector
 * PROJECTION (d3d8_vsh.c), or NULL when the program projects through
 * nothing it can tell. For mods (RecompModDraw.program_projection): D3D9
 * itself ignores the transforms under a program. */
void rhi_d3d9_program_projection(const float proj[16]);

/* A test switch: leave out the next draw (d3d8_vsh.c's RECOMP_D3D9_SKIP_VS). */
void rhi_d3d9_skip_next_draw(void);

/* Put the finished scene on the screen: copy `scene` (the scene target's
 * sampled view) to the back buffer, which the backend keeps the scene's
 * size so that Remix takes the scene's draws as the screen's, and have the
 * next rhi_present show it in the window's client area at the given shape,
 * with bars around it. The D3D9 replacement for d3d8_display_resolve: a
 * copy, not a draw, so Remix never sees it. 0 on success. */
int  rhi_d3d9_present_scene(RhiView *scene, uint32_t shape_w, uint32_t shape_h);

/* ---- mods (src/hle/recomp_mod.h, version 2) --------------------------------------- */

#include "../hle/recomp_mod.h"

/* What recomp_mods.c hands a mod as d3d9_*: hooks on the device being made,
 * each draw, each clear and each present; HLSL for every vertex program;
 * the render states as set; the fixed-function projection with the
 * viewport correction; and textures made anew when written after use.
 * Each on_* returns 0 when full. No-ops off Windows. */
int  rhi_d3d9_mod_on_device(RecompModDeviceFn fn, void *user);
int  rhi_d3d9_mod_on_draw(RecompModBeforeDraw before, RecompModAfterDraw after, void *user);
int  rhi_d3d9_mod_on_clear(RecompModClearFn fn, void *user);
int  rhi_d3d9_mod_on_present(RecompModDeviceFn fn, void *user);
int  rhi_d3d9_mod_vertex_hlsl(const char *hlsl);
void rhi_d3d9_mod_fixed_transform(int on);
int  rhi_d3d9_mod_render_state(uint32_t state, uint32_t *value);
void rhi_d3d9_mod_set_projection(const float m[16]);
void rhi_d3d9_mod_remake_written_textures(int on);
int  rhi_d3d9_mod_on_copy(RecompModCopyFn fn, void *user);
void rhi_d3d9_mod_set_transform(uint32_t type, const float m[16]);

/* A vertex program's whole text as this renderer compiles it for vs_3_0, for a
 * test that has no device: malloc'd, *total its length. Windows only. */
char *rhi_d3d9_vertex_text(const char *hlsl, size_t len, size_t *total);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RHI_D3D9_H */
