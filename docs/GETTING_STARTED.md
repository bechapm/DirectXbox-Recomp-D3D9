# Getting Started with Xbox Static Recompilation

This guide walks you through recompiling your first Xbox game, from extracting the XBE to getting the game running on Windows.

## What You Need

The Python pipeline — Steps 1 through 5, everything that turns an XBE into C
— is pure Python plus Capstone and runs anywhere. The platform question is
only about building and running the *game*:

| | Pipeline | Game build | Graphics backend |
|---|---|---|---|
| **Windows 10/11** | yes | MSVC | D3D11 (default) or Vulkan, the complete path |
| **Linux** | yes | Clang (GCC works, 3x slower) | Vulkan, the same renderer, on X11 or Wayland; TimeSplitters 2 runs (x86-64, SteamOS) |
| **macOS** | yes (Apple Silicon) | Clang | Vulkan on MoltenVK, the same renderer; TimeSplitters 2 and BLiNX run |

Every platform runs the same renderer (`src/d3d`, through `rhi.h`); only the
graphics API under it differs (`docs/technical/vulkan-backend.md`). Windows is
still where titles are proven first.

- **Python 3.10+** with `capstone` installed (`pip install capstone`)
- **CMake 3.20+**
- A C compiler: **Visual Studio 2022** (MSVC, C/C++ desktop workload) on
  Windows; Clang elsewhere. GCC builds the same thing on Linux, but lifted
  code takes it about three times as long (TimeSplitters 2: 90 s against
  28 s), so give a fresh build directory `CC=clang CXX=clang++`.
  `scripts/compat_matrix.py` and `scripts/regress.py` do that themselves when
  clang is on `PATH`
- **Linux**: `bash tools/linux/install_deps.sh` installs the system libraries
  and clang (apt/pacman/dnf, needs sudo; on SteamOS it says how to build in
  the Freedesktop SDK Flatpak instead, with its LLVM extension for clang),
  and `bash tools/linux/fetch_dxc.sh` puts DXC, the shader compiler no
  distribution packages, in `third_party/dxc`.
  The Vulkan loader and drivers are the desktop's own (Mesa)
- **macOS**: the [Vulkan SDK](https://vulkan.lunarg.com/) (MoltenVK, the loader,
  DXC). Installed system-wide it is found on its own; otherwise set `VULKAN_SDK`
- An original Xbox game disc image (ISO/XISO) — you must own the game

> **On the `py -3` in every command below.** That is the Windows Python
> Launcher. On Linux and macOS use `python3` instead — `python3 -m tools.disasm
> ...` and so on. Some Windows installs (notably the Microsoft Store one) have
> no `py` either; `python` works there.
- A tool to extract ISO files ([extract-xiso](https://github.com/XboxDev/extract-xiso) or [xdvdfs](https://github.com/antangelo/xdvdfs))

Optional but very helpful:
- **xemu** — Xbox emulator for live debugging via GDB stub
- **Ghidra/IDA** — for manual analysis of tricky functions
- **Your game's PC port source** (if leaked/available) — for understanding game logic

## Step 0: Set Up Your Project

Two directories, and it matters which is which:

- **`xboxrecomp/`** — this toolkit. Building it produces **static libraries only**. It has no `main()`, so it can never produce a game `.exe`, no matter how you invoke CMake.
- **`my_xbox_game/`** — *your* project, copied from `templates/new-game/`. This is what builds the `.exe`; it links the toolkit's libraries and holds the generated code.

```bash
# Clone the toolkit
git clone https://github.com/sp00nznet/xboxrecomp.git

# Create your game-specific repo from the template
cp -r xboxrecomp/templates/new-game my_xbox_game   # Windows cmd: xcopy /E /I xboxrecomp\templates\new-game my_xbox_game
cd my_xbox_game
git init

# Sanity-check the toolkit builds (optional -- your project builds it too,
# via add_subdirectory). This yields libraries, not an executable.
cmake -S ../xboxrecomp -B ../xboxrecomp/build
cmake --build ../xboxrecomp/build --config Release
```

## Step 1: Extract the XBE

Extract `default.xbe` and game data files from your disc image:

```bash
# Using extract-xiso
extract-xiso -x "My Game.iso" -d game_files/

# Or using xdvdfs
xdvdfs unpack "My Game.iso" game_files/
```

You should now have:
```
game_files/
├── default.xbe          # The game executable
├── Data/                # Game data (textures, models, levels)
├── Video/               # FMV files (XMV format)
└── ...                  # Other game-specific files
```

## Step 2: Parse the XBE

```bash
py -3 -m tools.xbe_parser game_files/default.xbe --json game_files/mygame_analysis.json
```

The `--json` file is **required by Step 3** — the disassembler reads the section
layout from it. Name it `<anything>_analysis.json` and keep it next to the XBE;
Step 3 finds it automatically.

This outputs:
- **Entry point** — where execution starts (e.g., `0x001D2807`)
- **Section layout** — VA, size, and raw offset for .text, .rdata, .data, and XDK lib sections
- **Kernel imports** — which of the 147 kernel functions the game uses
- **XDK version** — helps identify which XDK libraries were linked

**Write these values down.** You'll need them to configure `xbox_memory_layout.h`.

## Step 3: Disassemble

```bash
py -3 -m tools.disasm game_files/default.xbe --text-only -v
```

This typically takes 30-60 seconds for a 2-3 MB .text section. Output goes to `tools/disasm/output/`:

- `functions.json` — every detected function with address, size, instruction count
- `xrefs.json` — call graph (who calls whom)

Check the stats: you should find thousands of functions. A typical Xbox game has 10,000-25,000 functions including CRT and middleware.

## Step 4: Identify Library Functions

```bash
py -3 -m tools.func_id game_files/default.xbe -v
```

This classifies functions into categories:
- **CRT** — C runtime (malloc, free, memcpy, etc.) — usually safe to leave as-is
- **RW** — RenderWare engine (if applicable) — may need manual overrides
- **XDK** — Xbox SDK library code (D3D8, DirectSound, etc.) — often needs stubs
- **GAME** — Game-specific code — your main focus
- **STUB** — Empty/trivial functions — safe to ignore

## Step 4.5: Recover Calling Conventions

```bash
py -3 -m tools.abi_analysis game_files/default.xbe -v
#    Output: tools/abi_analysis/output/abi_functions.json
```

**Do not skip this one.** `tools.recomp` looks for `abi_functions.json`, and
when it is missing it warns once and then falls back to `cdecl` / 0 parameters
/ `int_or_void` for *every function in the game*. That is not a failure you
see — the recompile succeeds, the code builds, and the signatures are wrong,
which is the same quiet shape as most of the bugs in the changelog.

It recovers the calling convention (including `thiscall`, from a read of `ecx`
before any write), the parameter count from the `ret` immediate, return-type
hints and the frame shape.

## Step 4.6 (optional): Recover real names with Ghidra

Everything is `sub_0004F8B5` by default, and reading a 500,000-line call graph
of those is the slow part of every bring-up. If you have Ghidra, its Function ID
databases recognise the statically linked MSVC CRT and XDK helpers and give a
few hundred of them their real names — `malloc`, `_ftol`, `__SEH_prolog`,
`memcpy`, the 64-bit math helpers. Those are exactly the functions you would
otherwise waste a day identifying by hand.

Do it **before** Step 5. The recompiler reads the `name` field out of
`functions.json` and emits it as the C function name, so names applied now show
up in the generated code, in crash traces, and in `RECOMP_ABI_CHECK` reports.

```bash
# Analyse the XBE headless and export what Ghidra found
XBE=game_files/default.xbe tools/ghidra_naming/run_ghidra.sh

# Turn the export into a clean {address: name} map, and write the names into
# functions.json. Without --apply it only reports what it would do.
py -3 tools/ghidra_naming/merge_names.py --apply
```

Only meaningful names survive the merge — Ghidra's own `FUN_*`/`LAB_*`/`DAT_*`
placeholders are dropped, names are sanitised to valid C identifiers, and
collisions get the address appended.

Expect a few hundred names, not thousands: proprietary game code has no
signatures to match, so it keeps `sub_`. On the Xbox Dashboard this recovered
133 CRT/XDK names including the LZX and XIP decompressors, which is what made
the asset loader legible at all.

Ghidra ships no XBE loader, so the pipeline flattens the XBE into a raw image
at the right base address first — that is why the addresses line up with
`functions.json` exactly. (There is a community one,
[XboxDev/ghidra-xbe](https://github.com/XboxDev/ghidra-xbe); it warns about a
version mismatch on current Ghidra and works anyway. The flattening path does
not need it.) The runner takes the newest `ghidra_*_PUBLIC` under
`/c/tools/ghidra`; set `GHIDRA_ROOT` if you keep them elsewhere, or
`GHIDRA_HOME` to name one install exactly. Full detail, including optional
decompilation and why function seeding is off by default, is in
[tools/ghidra_naming/README.md](../tools/ghidra_naming/README.md).

Re-run Step 3 after this and the names are lost — `tools.disasm` rewrites
`functions.json` from scratch. Re-apply with `merge_names.py --apply`; the
Ghidra export is cached, so it takes seconds.

## Step 5: Recompile

Steps 2-4.6 all run from inside the `xboxrecomp` clone (that's where `tools/` lives).
Point the generated code at **your** project with `--gen-dir`:

```bash
py -3 -m tools.recomp game_files/default.xbe --all --split 1000 \
    --gen-dir ../my_xbox_game/src/recomp/gen
```

This is the big one — it can take 5-15 minutes for a large game. Output:

```
../my_xbox_game/src/recomp/gen/
├── recomp_0000.c          # Functions 0-999
├── recomp_0001.c          # Functions 1000-1999
├── ...                    # More splits
├── recomp_dispatch.c      # ICALL dispatch table (binary search)
├── recomp_funcs.h         # Forward declarations
└── recomp_stubs.c         # Stubs for unresolvable targets
```

Without `--gen-dir` the code lands in `src/game/recomp/gen/` **inside the toolkit clone**,
where no build target compiles it — which is why a plain toolkit build then still
produces only libraries. `src/recomp/gen/` is the path the template's CMakeLists globs.

## Step 6: Create Your Game Project

**This is the step that produces the `.exe`.** The toolkit has no `main()`; your
project does. If you copied `templates/new-game/` in Step 0 it is already in
place, and there are exactly three things to fill in.

`my_xbox_game/` now looks like:

```
my_xbox_game/
+-- CMakeLists.txt          # Builds the .exe, links the xboxrecomp libraries
+-- src/
    +-- main.c              # Host entry: loads XBE, inits kernel, calls the guest
    +-- recomp_manual.c     # Your hand-written function overrides
    +-- recomp/gen/         # Generated code from Step 5, including:
        +-- recomp_funcs.h     #   declarations for every lifted function
        +-- recomp_types.h     #   the runtime register model, MEM/ICALL macros
        +-- recomp_0000.c ...  #   the code itself
```

`recomp_types.h` is written there by Step 5, not something you supply. It used
to live only in `templates/runtime/`, and the first sign of that was
`error C1083: Cannot open include file: 'recomp_types.h'` at build time. If you
see that error, Step 5 did not complete — check its output for the
`wrote .../recomp_types.h` line. The pipeline never overwrites an existing copy,
so an edited one survives regeneration; delete it to get the current one back.

**1. `CMakeLists.txt`** — set the project name and the path to your toolkit clone:

```cmake
project(my_game C)                                    # names the .exe

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")
```

Everything else is already wired: it `add_subdirectory`s the toolkit (so you do
not have to build it separately), globs `src/recomp/gen/*.c` with
`CONFIGURE_DEPENDS`, links the `xboxrecomp` umbrella target plus the Windows
SDK libraries, and sets `/bigobj` and `/LARGEADDRESSAWARE`.

**2. `src/main.c`** — fill in the three constants from your Step 2 output:

```c
#define YOUR_GAME_ENTRY_POINT   0x001D2807          /* XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "game/default.xbe"
#define YOUR_GAME_DIR           "game"
```

The template already does the rest of the boot in order: install the VEH crash
handler, load the XBE, `xbox_MemoryLayoutInit`, `xbox_kernel_init`,
`xbox_path_init`, `xbox_kernel_bridge_init`, set `g_esp = XBOX_STACK_TOP`,
`recomp_dispatch_init()`, then call `xbe_entry_point()` — the generated function
named after your entry point VA.

**3. Game data** — put the extracted `default.xbe` and data files where
`YOUR_GAME_DIR` points, relative to the `.exe`.

## Step 7: Build and Crash

From `my_xbox_game/` — **not** from the toolkit clone:

```bash
cmake -S . -B build
cmake --build build --config Release
build\Release\my_game.exe 2>stderr.txt      # named after project() in your CMakeLists
```

This configure step builds the toolkit's libraries as a subdirectory *and* links
them into your `.exe`. If you get libraries and no `.exe`, you are building the
toolkit's `CMakeLists.txt` instead of your project's.

**It will crash.** That's expected and normal. The stderr log tells you what happened.

### Building the runtime once (optional)

Every title builds the same runtime libraries, about 450 files. Build them once
and point titles at that build, and a title compiles only its own code:

```bash
# Once, and again whenever you like (titles bring it up to date themselves)
cmake -S <toolkit> -B <toolkit>/build-engine -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build <toolkit>/build-engine --target xboxrecomp_engine

# A title's first configure
cmake -S . -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DXBOXRECOMP_PREBUILT=<toolkit>/build-engine
```

The engine has to come from the same toolkit tree as the title, with the same
compiler, build type and `RECOMP_ABI_CHECK`; the configure says so if not. The
title's own widescreen and timing settings still apply: `xbox_config` is always
built per title. On a 6-core Linux machine with clang, a clean TimeSplitters 2
build takes 33 s instead of 41 s, and the engine 9 s, once.

Lifted code is optimised as the build type says (`-O3` in Release). For quicker
iteration, `-DXBOXRECOMP_LIFTED_OPT=1` (off MSVC) builds it at `-O1`: TimeSplitters
2 then takes 27 s, but runs 10% slower uncapped; Turok runs no slower. `-O2`
compiles no faster than `-O3`.
Not yet with `XBOXRECOMP_MACOS_APP`.

### Running it: never from a build step

No CMake target, custom command, post-build step or ctest may start a title.
A title run is a game run: it opens a window, plays sound, and writes saves
(into the game folder's `UDATA`, which is the console's E: drive, and into the
per-user partition images). Run titles only from a shell, or in automation
through the harness that mutes them, keeps them in the background and gives
them a throwaway copy of the game folder and their own saves (on this
project's agents' Macs, `run-title.sh`). This rule exists because a bundling
target once had an empty variable for its interpreter, so its command became
the title itself, and building it ran the game, unmuted and in front, on a
player's real saves. `scripts/make_macos_app.py` now refuses to start any
program but `otool`, `install_name_tool`, `codesign` and `cp`, and has a
`--dry-run`.

### macOS: an application bundle (for your own machine)

```bash
cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DXBOXRECOMP_MACOS_APP=ON -DXBOXRECOMP_APP_NAME="My Game"
cmake --build build-mac              # also builds build-mac/My Game.app
# or, for an executable you already have:
python3 <toolkit>/scripts/make_macos_app.py build-mac/my_game --name "My Game" [--dry-run]
```

Ninja and Makefiles are single-configuration generators: the build type is
chosen when you configure, and `--config` on the build line does nothing.
The toolkit defaults an empty `CMAKE_BUILD_TYPE` to Release (before October
2026 it did not, and lifted code was compiled at `-O0`); say
`-DCMAKE_BUILD_TYPE=Debug` or `RelWithDebInfo` when you want those.
`-DXBOXRECOMP_MACOS_APP_DRY_RUN=ON` makes the target print what it would do.
The `.app` carries the Vulkan loader, MoltenVK and its driver manifest, DXC,
and every non-system library the title links (Homebrew's `libcrypto`), signed
ad hoc. Put the game folder (the one with `default.xbe`) beside it, named
`game`; `Contents/Resources/game` also works, but the title writes its saves
into its game folder, and writing inside the bundle breaks its signature. A
run started from the Finder logs to `~/Library/Logs/xboxrecomp/<exe>.log`.
FFmpeg is not bundled (Homebrew's is a GPL build); XMV movies play when one is
installed and are skipped otherwise.

**The `.app` contains the title's recompiled code. It is for your machine
only: never put one in CI artifacts or a release, never share it.**

## Step 8: Debug Iteratively

This is where the real work begins. The general pattern:

1. **Run** — game crashes
2. **Read stderr** — look for ICALL failures, bad memory access, assertion failures
3. **Identify the problem** — usually one of:
   - Missing ICALL target (function pointer the dispatch table doesn't know about)
   - Bad memory access (pointer to Xbox memory that hasn't been mapped)
   - Unimplemented kernel function (game calls a function we stubbed)
   - Corrupted vtable (native pointer where Xbox VA expected)
4. **Fix it** — add a manual override, stub the function, fix the dispatch table
5. **Rebuild and repeat**

### Common First Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| Crash in ICALL dispatch | Unknown function pointer | Add function to dispatch table or stub it |
| Access violation at 0xFD...... | GPU MMIO access | Initialize NV2A and MMIO hooks |
| Access violation at 0xFE...... | APU MMIO access | Initialize APU and MMIO hooks |
| "SKIP-READ" in stderr | Access to unmapped memory | Check mirror views; might be native ptr confusion |
| Infinite loop | Game waiting for hardware | Stub the wait function or fake the hardware state |
| Stack overflow | Recursive calls or wrong ESP | Check stack setup, ensure ESP starts correctly |

### The ICALL Trace

The most powerful debugging tool. When an indirect call fails, the trace shows:

```
[ICALL] unknown target 0x001A3F50 from RVA 0x000165F0
```

This tells you: function at 0x000165F0 tried to call 0x001A3F50, but it's not in the dispatch table. Usually means you need to add it to `recomp_dispatch.c` or create a manual override.

### Manual Overrides

When a recompiled function doesn't work (crashes, loops forever, reads hardware), replace it:

```c
// In recomp_manual.c
void sub_001A3F50(void) {
    // The original function reads GPU registers we haven't set up.
    // For now, just return success.
    eax = 1;
    esp += 4; return;  // Clean up fake return address
}
```

Register your override in the manual lookup table so ICALLs find it.

## Step 9: Get to Menus

The typical boot sequence for an Xbox game:

1. **CRT startup** — `_mainCRTStartup` → `main()` or `WinMain()`
2. **Hardware init** — D3D device creation, DirectSound init, input setup
3. **Asset loading** — textures, models, levels from disc
4. **Menu system** — title screen, main menu
5. **Gameplay** — the actual game

Each phase introduces new challenges. Hardware init needs working kernel + D3D stubs. Asset loading needs file I/O. Menus need rendering. Gameplay needs everything.

Focus on getting past each phase one at a time.

## Step 10: Beyond Boot

Once the game boots and shows something on screen, you're past the hardest part. From here:

- **Add missing features** — audio, input, save/load
- **Fix rendering** — texture formats, shader states, blend modes
- **Optimize** — profile, find bottlenecks, add proper shader support
- **Mod** — the generated C code is yours to modify. Add HD support, widescreen, new features.

## Tips

- **Start with a simple game** — pick something small with a known engine (RenderWare games are good targets)
- **Use xemu for reference** — run the game in xemu with GDB debugging to understand what memory addresses mean
- **Keep notes** — document every address, every function you identify, every patch you make
- **Don't try to fix everything at once** — stub what you can, fix what you must
- **The 80/20 rule applies** — 80% of functions "just work" in recompiled form. The other 20% is where you spend your time.
- **Read the technical docs** — especially [indirect-calls.md](technical/indirect-calls.md) and [lessons-learned.md](technical/lessons-learned.md)
