# A Direct3D 9 backend for RTX Remix

**Status, October 2026:** runs TimeSplitters 2 at full speed under Windows'
own D3D9 and under RTX Remix 1.6.1, which path-traces its levels in the
game's own world space (the USA release hands its camera over; see below).
A captured Siberia frame replays pixel for pixel the same as D3D11. `RECOMP_D3D8_BACKEND=d3d9` chooses it. This note is the design
and what is left; `src/d3d/rhi_d3d9.c` and `rhi_d3d9.h` are the reference.

## Why D3D9

NVIDIA RTX Remix path-traces Direct3D 9 titles and lets their textures,
meshes and lights be replaced. It is a D3D9 runtime (dxvk-remix): its
`d3d9.dll` beside an executable receives that executable's D3D9 calls. The
64-bit runtime is used directly; the bridge is only for 32-bit titles.

Remix does not read shaders. It rebuilds the scene from what a fixed-function
D3D9 title sets around each draw: `WORLD`, `VIEW` and `PROJECTION`, the texture
in stage 0, and the vertex declaration's `POSITION`, `NORMAL` and `TEXCOORD`.
For a draw through a vertex shader it captures the positions the shader writes
(`rtx.useVertexCapture`, on by default) and carries them back to world space
through the inverse `PROJECTION` and `VIEW`. So the backend's job is less
"run on D3D9" than "look like a D3D9 game": fixed function where the title was
fixed function, the camera where a vertex program hides it, and nothing drawn
that Remix could take for the scene when it is not.

## Where the seam goes

The renderer already draws through `rhi.h` (`vulkan-backend.md`). D3D9 is a
third backend behind it, and where D3D9 has to draw differently, the renderer
asks `rhi_d3d9_active()` and calls `rhi_d3d9.h`. Under the other two backends
nothing changes: the shared HLSL keeps its D3D11 text in the `#else` of each
`#ifdef RHI_D3D9`.

| What | D3D11 / Vulkan | D3D9 |
| --- | --- | --- |
| Fixed-function draw | the generic FFP shaders | D3D9's own fixed function: no shader, transforms, lights, material, render and texture-stage states handed over (`d3d8_shaders.c`, `d9_ff_prepare_draw`) |
| Pre-transformed (XYZRHW) | divide by the target's guest size in the VS | `POSITION` x, y, z under an orthographic `PROJECTION` of the guest target, so the viewport squeeze still applies and Remix sees 2D as orthographic |
| Vertex program | vs_5_0 | vs_3_0 from the same generator: constants in registers c0, c192, c196; inputs by the XDK's register convention (v0 `POSITION`, v2 `NORMAL`, v3 `COLOR0`, v9 `TEXCOORD0`...) |
| Pixel stage under a program | the generic FFP PS, or the combiner translator | a ps_3_0 written per combination of stage operations (`d9_ps_source`) |
| Register combiner program | translated, Shader Model 5 | translated, ps_3_0 under a vertex program and ps_2_a under fixed function (D3D9 pairs fixed function only with ps_2_x); one too long for ps_2_a draws stage 0's texture times diffuse |
| Alpha test | in the pixel shader | D3D9's own render state, title draws only (`rhi_d3d9_title_draw`) |
| Clear with rectangles | a quad per rectangle | D3D9 `Clear` with rectangles |
| Screen copy | a draw | `StretchRect` |
| Depth copy | a draw | the same draw in Shader Model 3, sampling `INTZ` |
| Display resolve | a box-filter draw to the swap chain | `StretchRect` to a back buffer the scene's size; `Present` stretches it into the window, bars drawn with GDI |
| Touch controls | drawn | not drawn |

## What Remix is told

- **Fixed-function draws** carry the title's own `WORLD`, `VIEW` and
  `PROJECTION` (Hor+ applied to the projection as the D3D11 path applies it).
- **Program draws** carry `PROJECTION` = the program's own projection
  register (`proj_reg`, as uploaded: Hor+ and camera zoom applied) transposed
  to row-vector form, `WORLD` = identity, and `VIEW` = the title's view. A
  title gives it with `xbox_D3D8SetRemixCamera` once a frame, or, when one
  frame is drawn under more than one camera state, with
  `xbox_D3D8SetRemixViewSource`, asked at every program draw. Identity until
  it does: the world Remix sees is then the camera's own space, which renders
  but moves with the camera, defeating temporal accumulation and anchoring
  replacements to the screen. TimeSplitters 2 (split2's `ts2_draw_world`)
  reads the view pointer in its render context at each draw: most of its
  frame is drawn before the function that would hand the view over, and
  given the view there those draws carried the frame before's.
- **A `POSITION` element.** Remix takes a draw's vertices from the
  declaration's `POSITION` and leaves out a draw without one. v0 is
  `POSITION`, but a program need not keep its position there (TimeSplitters 2
  keeps its level's in v1), so a declaration without v0 also declares its
  first three- or four-float register as `POSITION0`, unread by the program.
- **The viewport correction.** D3D9 needs the viewport inside the target and
  puts pixel centres half a pixel off D3D10's. The backend clamps the viewport
  and corrects clip space with `RHI_VP_ADJUST` (c255) in every vertex shader,
  and folds the same correction into `PROJECTION`, so what Remix inverts is
  exactly what was rasterised.
- **No depth clamp.** D3D9 always clips to `0 <= z <= w`; D3D11 runs the
  title's draws with depth clip off. Where the raster state asks for that,
  `RHI_VP_ADJUST` clamps each vertex's z into range (flag at c254), so a
  level's far walls and sky are drawn at the far plane rather than cut away.
- **The screen-space undo branches.** Flattened by fxc for Shader Model 3, the
  end of every generated vertex program blended the clip position with the
  raw screen position, whose z is in depth-buffer units (~1.6e7), and lost
  every clip z below 1 in fp32. It is `[branch]` under D3D9.
- **The back buffer is the scene's size**, because Remix takes only draws into
  a target of the back buffer's size for the scene. The window is filled by
  `Present`'s destination rectangle.

## D3D9 against D3D11, inside the backend

Constant buffers become CPU copies uploaded to fixed registers per (stage,
slot); index data stays on the CPU and each indexed draw copies its range into
a 16- or 32-bit streaming buffer; a draw with no vertex layout reads its
vertex number from an internal `TEXCOORD15` stream; formats D3D9 lacks are
converted on upload (R8G8B8A8 swapped to A8R8G8B8, R8/R8G8 and the 16-bit
one- and two-channel formats widened, A8 stored white as the Xbox samples it);
sampled depth is `INTZ`. There is no device `Reset`: a windowed device is not
lost, and the back buffer keeps its size.

## Trying it

1. Build as usual (MSVC). Nothing links `d3d9.lib`: `d3d9.dll` is loaded at
   run time, so Remix's is found before the system's.
2. Without Remix first: `RECOMP_D3D8_BACKEND=d3d9`, with the system's D3D9.
   The picture should match D3D11's apart from long register-combiner
   programs under fixed function ([d3d9-parity.md](d3d9-parity.md)).
3. With Remix: copy the 64-bit runtime (`d3d9.dll`, `.trex/` and the rest of
   the runtime package) beside the executable, and run with the same setting.
   Alt+X opens Remix's menu. Mark the HUD and font textures as UI textures
   there and save `rtx.conf` beside the executable. Add
   `rtx.minimizeBlasMerging = True`, or first-person weapons and other
   geometry carried with the camera are misplaced while it turns
   ([d3d9-parity.md](d3d9-parity.md)).

## Known gaps

[d3d9-parity.md](d3d9-parity.md) lists every place this backend draws
differently from D3D11, and what Remix is told about the title's 2D and sky
(`remix_draw` in `rhi_d3d9.c`: 2D drawn by programs is marked as UI with a
fixed texture, `rtx.uiTextures = 0xA2F17C4A8F3E3CEA`; the sky gets a viewport
Remix takes for sky). `RECOMP_D3D9_SKIP_ORTHO=1` leaves the 2D out, for
testing.

- The PAL release of TimeSplitters 2 does not hand its camera over yet.

- A register combiner program too long for ps_2_a, under fixed function, is
  approximated (stage 0's texture times diffuse). Under fixed function the
  combiners' fog register reads no fog; D3D9's fog unit fogs the result.
- Vertex blending (skinned fixed-function) is ignored, as under D3D11.
- Point sprites are one pixel, as under D3D11.
- Frame interpolation is untested under D3D9.
- `RECOMP_RES_SCALE` above 1 renders larger, but is not box filtered: the back
  buffer is the scene and `Present` scales it.
