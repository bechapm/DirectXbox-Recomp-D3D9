/*
 * d3d8_touch.h -- the on-screen controls, drawn over the presented frame.
 *
 * After the display resolve has put the scene and its bars into the swap
 * chain, the controls (src/input/touch_controls.h) are drawn over the whole
 * window, bars included, at the window's own resolution: they are part of the
 * device, not of the game's picture. What to draw comes from a source the
 * caller sets (src/hle); with none, or nothing shown, this draws nothing.
 *
 * One pass, one full-window triangle: each control is a distance function in
 * the pixel shader, so the edges stay sharp at any size and nothing is
 * uploaded but a constant buffer.
 */
#ifndef XBOXRECOMP_D3D8_TOUCH_H
#define XBOXRECOMP_D3D8_TOUCH_H

#include "platform/touch_shape.h"
#include "rhi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fills out[] with up to max shapes for a w x h window and sets *opacity;
 * returns how many. Called from the presenting thread, once a frame. */
typedef int (*d3d8_touch_source)(TouchShape *out, int max, int w, int h, float *opacity);

void d3d8_touch_set_source(d3d8_touch_source fn);

/* Draws the controls into `out` (the swap chain), w x h pixels. */
void d3d8_touch_draw(RhiView *out, unsigned w, unsigned h);

void d3d8_touch_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
