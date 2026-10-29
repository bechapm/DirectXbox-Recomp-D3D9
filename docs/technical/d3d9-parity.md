# The D3D9 backend against D3D11: where they differ, and how that is checked

`rhi_d3d9.c` is meant to draw what the D3D11 backend draws, for every title,
not only for TimeSplitters 2 under RTX Remix. This page lists every place the
two take different paths, what each costs, and how it is tested, so that a gap
shows up here before a title trips over it. [d3d9-remix.md](d3d9-remix.md) is
the design; this is the audit.

Three kinds of difference are kept apart, because each has a different owner:

| Kind | Meaning | Fixed in |
|---|---|---|
| **Backend** | Plain D3D9 (no Remix) draws something other than D3D11. | `rhi_d3d9.c`, the D3D9 branches in `src/d3d` |
| **Remix hint** | Plain D3D9 is right, but what Remix is told about a draw (UI, sky, camera) is wrong. | `remix_draw` in `rhi_d3d9.c`, the title's camera source |
| **Remix side** | Remix gets the right description and still needs settings, lights or replacement assets. | `rtx.conf`, the Remix project |

Gaps that every backend shares (vertex blending, point sprites) are not the
D3D9 backend's; they are listed at the end so that they are not mistaken for
it.

## How a difference is found

1. **D3D11 against plain D3D9.** The same title, the same scripted input and
   save, at the console's own settings, so that no enhancement is mistaken
   for a backend difference: `RECOMP_RES_SCALE=1`, `RECOMP_WIDESCREEN=0`,
   `RECOMP_ANISO=1`, `RECOMP_FRAME_INTERP=1` (off), no FPS overlay. Each
   enhancement is then checked on its own, one backend at 1x against the
   same with it on. Plain D3D9 is `RECOMP_D3D8_BACKEND=d3d9` with the
   system's `d3d9.dll` and `RECOMP_D3D9_REMIX_2D=0`. Frames are matched by
   number: the input script runs on the title's Swaps
   (`RECOMP_INPUT_SEQ_SWAPS=1`), the title's clock waits for them
   (`RECOMP_VBLANK_LOCKSTEP=1`, since a renderer that presents slower would
   otherwise see the title catch up in bursts of vblanks), and
   `RECOMP_HLE_D3D8_DUMP_AT=<from>:<count>:<every>` with
   `RECOMP_HLE_D3D8_DUMP_SIZE=<w>x<h>` writes the same swaps from each run.
   Loads still take as many swaps as the disc does, so runs can sit a swap
   or three apart; the comparison aligns them by content. Scenes that do not
   repeat at all (a title reading the time, bots, effects) are compared
   from one run instead: `RECOMP_D3D8_CAPTURE_AT=<from>:<count>:<every>`
   captures the frames, and `d3d8_replay` draws each through D3D11 and
   through plain D3D9, so both pictures are of the very same frame. Mean absolute difference
   and the share of pixels past a small threshold, per frame. Anything real
   here is a **backend** difference.
2. **Plain D3D9 against Remix, in the same run.** Under Remix every draw is
   still rasterised into the scene target, so a read-back of that target just
   before Remix composites is the D3D9 picture of the very frame Remix shows.
   A frame that is right there and wrong in Remix's output is a **Remix hint**
   or **Remix side** difference; Remix's debug views (`rtx.debugView.debugViewIdx`)
   and a control run with one change say which.
3. **The synthetic capture.** `tests/d3d8_capture --write` builds a frame
   with no game data (fixed function, a screen copy, a pre-clear copy, rectangle
   clears, a vertex program with a declaration and combiners);
   `tests/d3d8_replay` draws it. Run through both backends, the two pictures
   should match; this runs anywhere Windows does, without a disc.

## Where the D3D9 backend takes its own path

| Area | D3D11 | D3D9 | Status | Risk for other titles |
|---|---|---|---|---|
| Fixed-function vertex stage | The renderer's own shader | D3D9's fixed-function pipeline: transforms, lights, material, texture stages and fog handed over as D3D9 state (`d9_ff_prepare_draw`), because Remix reads that state | Checked on TS2's few fixed-function draws only | **High.** Titles that draw mostly through fixed function (Max Payne) exercise it far more than TS2. Each forwarded state is a place D3D9's rules, not the renderer's, decide the result. |
| Pre-transformed vertices (XYZRHW) | The renderer's shader: screen position from x, y, z, and clip w recovered from RHW, so textures and colours interpolate with perspective | D3D9's fixed function through a projection from the title's pixels (`d9_ff_prepare_draw`); RHW is not declared, so every vertex has w = 1 | **Gap** where RHW varies across a primitive: textures and colours interpolate linearly, not with perspective. Exact for 2D (RHW constant) | Medium: titles that project 3D on the CPU and draw it pre-transformed |
| Pixel stage under fixed function | Shader Model 5 | Texture stages as D3D9 states; register combiners as ps_2_a, since D3D9 pairs fixed-function vertices with ps_2_x only | A combiner program too long for ps_2_a is approximated (stage 0's texture times diffuse); its fog register reads no fog | **High** for titles with long combiner programs on fixed-function geometry |
| Pixel stage under a vertex program | Shader Model 5 | ps_3_0 (combiners) or a ps_3_0 written from the texture stages | Matches on TS2 | Medium: ps_3_0 limits (no integer ops, 224 constants) |
| Vertex programs | Shader Model 5 | vs_3_0; constants as registers (c0-191 program, c192-195 screen-space undo, c196 inputs, c254-255 backend) | Matches on TS2 | Medium: a title that relies on b0 (not uploaded) or on more than 192 constants |
| Depth clamp | Raster state | Emulated per vertex in `RHI_VP_ADJUST` (z clamped to 0..w when w > 0) | Matches on TS2 | Low; a triangle crossing w = 0 can differ from true clamping |
| Pixel centres, viewport | D3D10 rules, viewport at fractions of a pixel | Corrected in clip space (`RHI_VP_ADJUST`, the fixed-function projection); the whole-pixel viewport D3D9 needs is rounded outwards and cut back to D3D11's pixels with the scissor | Fixed: rounding to the nearest pixel lost the target's first column and row for every title (the console's half-pixel offset), found by the synthetic capture | Low |
| Formats | DXGI | Converted where D3D9 has no equal (RGBA swapped, R8/R8G8/16-bit widened, A8 stored white, R11G11B10 widened to half float); sampled depth is `INTZ` | All RHI colour formats mapped | Low; costs upload time and memory |
| Screen copies | A draw | `StretchRect` with point filtering (a draw would be taken by Remix for the scene) | Matches on TS2 | Low; a scaled copy filters as point, not as a draw would |
| Clears with rectangles | A draw | D3D9's `Clear` with rectangles | Matches on TS2 | Low |
| Depth copy to a texture | A draw that Loads the depth | The same draw in Shader Model 3: the `INTZ` depth sampled at texel centres with a point sampler, the bytes split arithmetically | New, untested. Under Remix a copy into a texture the back buffer's size may be dropped: Remix ignores a draw with no position into its primary target | Medium: Rainbow Six 3's depth of field and bloom read it |
| Shadow-map compare | Four texels Loaded, compared and filtered | The same four texels, each sampled at its centre (where any filter returns that texel), the size passed in `tex_scale[i].zw` | New, untested | Medium for titles with shadow buffers |
| Multisampling | Supported | None (`d9_sample_count_supported` is 1x only) | **Gap** | Low: titles that ask for it draw at 1x |
| 2D array images | Supported | First layer only | **Gap** | Low |
| Touch-screen overlay | Drawn | Not drawn (its shader tests bits) | By design | None for desktop play |
| Device reset | Swap chain follows the window | No `Reset`; the back buffer is the scene's size and `Present` scales it | By design | `RECOMP_RES_SCALE` above 1 is not box-filtered |
| Frame interpolation | Supported | Untested | Unknown | Unknown |

## What Remix is told (Remix hints)

These apply only when Remix's `d3d9.dll` is loaded (it exports
`remixapi_InitializeLibrary`), or with `RECOMP_D3D9_REMIX_2D=1`; plain D3D9
draws as above. They read draw state the renderer already has, never a
title's own data.

| Hint | Rule | Assumption | Known limits |
|---|---|---|---|
| Camera | Each program draw gets WORLD, VIEW and PROJECTION; VIEW comes from the title's camera source when it registers one (`xbox_D3D8SetRemixViewSource`), else from the renderer's guess | A title's real camera is known to its project | Titles with no camera source get the guess; TS2 PAL has none yet |
| 2D as UI | A title's program draw the renderer judges screen-space (its program uses no projection, or an orthographic one: the same verdict widescreen uses) is marked UI with a fixed texture in `rtx.uiTextures` | Remix composites at the first UI draw, so UI must come after the frame's 3D | Fixed-function 2D is left to Remix's own rule (orthographic, no depth write); 2D a title draws through its 3D camera (Max Payne's HUD) is not caught |
| Mid-frame 2D | A kind of 2D draw (program, texture, blend, topology) that was followed by 3D in the previous frame is left out | Frame structure changes slowly | One wrong frame when it changes; such 2D (TS2's muzzle glow) is not shown under Remix |
| Screen-copy passes | 2D that samples a copy of the screen is left out | The copy holds the rasterised picture, not Remix's | Post effects built on screen copies (blur, glow) are lost under Remix |
| Sky | A title's 3D program draw before any depth-writing draw in the frame, with depth not written and always passing, gets a viewport with minimum depth 1 (Remix's `rtx.skyMinZThreshold`) | Sky is drawn first, with depth off | A sky drawn after the world, or with a depth test, is ray traced as a box and blocks Remix's light |

## Gaps every backend shares

Not differences between backends, but gaps against the Xbox that all of them
show: vertex blending (skinned fixed-function draws) is ignored, and point
sprites are one pixel.

## Titles to check against

TimeSplitters 2 mostly draws through vertex programs and register combiners,
so it leaves the fixed-function path and the ps_2_a combiners nearly
untested. Max Payne (XDK 4134, much of its picture and HUD through fixed
function) and Rainbow Six 3 (XDK 5659) are the next titles to take through the
three comparisons above, then Gladius or Pro Evolution Soccer 5.
