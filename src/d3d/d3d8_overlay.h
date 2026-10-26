/*
 * d3d8_overlay.h -- a line of text drawn over the finished frame.
 *
 * The renderer's own on-screen display: whatever the caller hands it, drawn
 * at the top-left of the picture, over the swap chain, as the frame is
 * presented. It knows
 * nothing about what the text says, so what to show and when to show it stays
 * with the caller (src/hle decides; see RECOMP_FPS_OVERLAY there).
 *
 * The text is rendered by GDI into a bitmap and uploaded, rather than drawn
 * from an embedded font, so it is a real font at a real size and there is no
 * glyph table to maintain.
 *
 * What it touches on the device: the render target, the viewport, the input
 * layout and topology, the vertex and pixel shader, one shader-resource slot
 * (8, past the four texture stages a title can use) and one vertex-shader
 * constant slot (7). It puts the render target and viewport back. Everything
 * else in that list is set again by the next draw -- the blend, depth and
 * rasterizer states come from d3d8_states_apply, which runs per draw -- with
 * one exception the caller must handle: d3d8_states_apply_sampler does not
 * re-bind an unchanged sampler, so nothing here binds a sampler at all (the
 * pixel shader reads the text by Load, at integer pixels, which is also what
 * keeps it crisp). No vertex buffer is bound either: the quad comes from
 * SV_VertexID, so a title's stream source survives the frame.
 */
#ifndef XBOXRECOMP_D3D8_OVERLAY_H
#define XBOXRECOMP_D3D8_OVERLAY_H

#include "rhi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The line to draw from now on; NULL or "" for none. It is drawn by every
 * present (d3d8_overlay_present), not into the title's frame, so a frame
 * presented again -- an in-between frame, or a held frame redrawn behind a
 * menu -- carries the line as it is now. Called on the presenting thread. */
void d3d8_overlay_set_text(const char *text);

/* Draws the line over `out` (the swap chain, w x h pixels) at the top-left
 * of the picture, which the display put at (pic_x, pic_y), pic_h pixels
 * tall; the glyphs are scaled with the picture, as they were when the line
 * was drawn into a 480-line frame. The bitmap is only re-rendered when the
 * text changes. A failure to create anything it needs disables it for the
 * rest of the run, once, with a line on stderr -- an overlay must never be
 * the reason a title stops drawing. */
void d3d8_overlay_present(RhiView *out, unsigned w, unsigned h,
                          unsigned pic_x, unsigned pic_y, unsigned pic_h);

/* Releases everything. Called from the device teardown. */
void d3d8_overlay_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
