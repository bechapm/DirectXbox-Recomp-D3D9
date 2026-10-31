# Mods

A recompiled title is a native program, so a player can change it the way PC
games have long been changed: by dropping a library beside it. The toolkit
gives such libraries one small, versioned C contract,
[`src/hle/recomp_mod.h`](../../src/hle/recomp_mod.h), so that a change to a
title can live outside the title's build: the port stays the port, and a mod
is something a player chooses to add.

This page is what a mod can do, how it is loaded, and what belongs in one
rather than in the toolkit or a title's port.

## What a mod is for

Anything that changes or adds to a title without being part of it:

- **Graphics add-ons:** a texture or model pack, a post-processing pass, a
  HUD tweak, support for a D3D9 tool such as RTX Remix
  ([d3d9-backend.md](d3d9-backend.md)) that needs to be told things about
  one title (its camera, which 2D is HUD).
- **Tools:** a frame debugger's markers, a texture or mesh dumper, an
  overlay showing the title's own state.
- **Experiments and fixes:** something a player wants to try on one title,
  without a rebuilt executable.

What a mod gets is general: nothing in the contract names a title or a
particular tool. A mod for one title checks the title it is loaded into and
unloads itself from any other.

## Loading

- Every `.dll` (Windows) or `.so` (elsewhere) in the folder `mods` beside
  the executable, in name order, that exports `recomp_mod_init`.
- `RECOMP_MOD_LIBS=0` loads none; `RECOMP_MOD_LIBS_DIR` names another folder.
- They are loaded once, as the title makes its Direct3D device (before the
  first draw). `recomp_mod_init` returns nonzero to stay loaded.
- `d3d8_replay` loads them too when it replays a frame capture, so a mod's
  effect can be checked on the same frames again and again.

A title's own asset mods (for example split2-recomp's `RECOMP_MODS` folders,
which replace files the title reads) are a different thing: they change data,
not code.

## What a mod can do

| Area | What the contract gives |
| --- | --- |
| The title | its ID and name; read access to its memory (`guest_pointer`); a call once a frame; the vertex-program constants of the draw in hand; a log line |
| Draws (Direct3D 9 renderer) | a call before and after every draw, with what the renderer knows of it (the title's or the toolkit's, 2D or 3D, fixed function or program, the program's projection, textures and screen copies bound, depth state, vertices and indices); the draw may be changed for that one call or left out |
| Device events | device made, clears, copies of the screen into a texture (the mod may supply the surface to read instead), before `Present` |
| Shaders and state | vertex HLSL added in front of every program (`RHI_MOD_POSITION`); render states as last set; the fixed-function transforms; the projection |
| Textures | textures the CPU rewrites after they were drawn with can be made anew, for tools that recognise a texture by its contents |
| Captures | a mod's own notes stored in a frame capture as it is made and handed back at the same point of a replay, for a mod that reads the title's memory |

The per-draw hooks exist under the Direct3D 9 renderer today; under D3D11
and Vulkan a mod still loads and gets the title-level calls.

## The contract

`recomp_mod.h` stands alone: a mod is built against a copy of it, not
against the toolkit. Fields are only ever added at the end; a mod checks the
table's `size` before using one newer than it knows, and `version` names the
newest set. The header itself is the reference for every field.

A minimal mod:

```c
#include "recomp_mod.h"

static const RecompModApi *g_api;

static void each_frame(void *user)
{
    (void)user;
    /* read the title's state through g_api->guest_pointer, ... */
}

int recomp_mod_init(const RecompModApi *api)
{
    if (api->version < 3 || api->title_id() != 0x12345678u)
        return 0;                       /* not ours: unload */
    g_api = api;
    api->on_frame(each_frame, NULL);
    return 1;
}
```

## Where a change belongs

- **The toolkit** gets what serves any title: renderer behaviour every
  title needs, and general hooks a mod can use. A hook is described by what
  it does (a draw, a copy, a texture rewritten), never by the one tool or
  title that first wanted it.
- **A title's port** gets what that title needs on any renderer: its fixes,
  its controls, its own modding support (reading its archives, unlocking its
  content).
- **A mod** gets what one title needs for one tool or one change, for
  example RTX Remix support for a particular game: its camera, its HUD
  rules, its configuration.
