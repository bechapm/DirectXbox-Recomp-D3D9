# Self-building releases

A per-game download with no game code in it. The player unpacks it and starts
the launcher, which asks for their own copy of the disc (an image or a folder),
recompiles the game from it on their machine, and then shows the usual settings
and Play. Linux first; Windows (llvm-mingw) and macOS follow.

Lifted C is derived from game code, so it can only be made on the player's
machine from their own disc. Everything else the build needs travels with the
release: a compiler, CMake, Ninja, Python, the pipeline and a prebuilt engine.

## What a player does

1. Unpack, start `./<name>` (for TimeSplitters 2, `./timesplitters2`).
2. On the first-run screen, choose **Disc image...**, **Folder...**, or type or
   paste a path. Choosing in a dialog starts the build.
3. Watch the build (about 80 s on a 6-core machine for TimeSplitters 2, longer
   on a Steam Deck), then the normal launcher appears. Press Play.

An image is extracted once; a folder is mirrored with links and never written
to. A disc whose `default.xbe` is not one the release lists is refused with the
title, region and SHA-1 it got. A newer release finds the old build stale and
offers **Build again**, with the same disc remembered.

## The release

Made by `tools/release/make_release.py`:

```
<name>                    the launcher (src/launcher), which the player starts
release.json              title, title id, version, executable, accepted SHA-1s
bin/xboxrecomp-build      the builder (tools/builder, Rust)
bin/XbSymbolDatabaseCLI   names the XDK functions in the player's own XBE
toolchain/                zig 0.17, CMake 4.4, Ninja 1.13, zcc/zc++/zar/zranlib
python/                   CPython 3.13 (python-build-standalone) with Capstone
engine/                   the runtime, prebuilt (make_engine_package.py)
toolkit/                  the pipeline: tools/, scripts/recompile.py, config/,
                          templates/runtime, src/hle
recipe/                   the title's CMakeLists, src/ and config/
dxc/                      the shader compiler the renderer loads
LICENSE NOTICE.txt README.txt
```

TimeSplitters 2's is 637 MB unpacked, 497 MB of it the toolchain (zig is most).

## The player's folder

`$XDG_DATA_HOME/xboxrecomp/builds/<TITLEID>` (`~/.local/share/...`), or
`$XBOXRECOMP_BUILD_DIR/<TITLEID>` for tests. The release itself can be
read-only.

```
game/            the extracted image, or a mirror of links to the folder
                 (folders symlinked, files hard-linked; its own UDATA/TDATA,
                 so saves never land in the player's copy -- E: is the game
                 folder)
<exe>            the game, beside game/, where its main.c looks
libdxcompiler.so a link to the release's, beside the game, where the renderer looks
project/ pipe/ build/ zig-cache/ tmp/   the build, kept for incremental rebuilds
source.json      what was chosen (an image's size and time, to skip re-extraction)
build.json       what it was built from: release version, XBE SHA-1
build.log        everything the tools printed: the file to attach to a bug report
build.lock       held during a build; a dead build's is taken over
```

About 450 MB besides the game files for TimeSplitters 2.

## Making one

In the Freedesktop SDK on SteamOS (with the `llvm22` and `rust-stable`
extensions), or on a distribution with the toolkit's dependencies and Cargo:

```bash
python3 -m tools.release.make_release config/releases/timesplitters2.json \
    --out ../release/timesplitters2 [--work ../release-work] [--cache ../downloads] \
    [--version v1.0] [--archive]
```

`--archive` also writes `<release_name>-<version>-linux-x86_64.tar.xz` beside
`--out`, which unpacks as `<release_name>/`: the file a release publishes.

The recipe, `config/releases/<title>.json`:

```json
{
  "title_name": "TimeSplitters 2",
  "title_id": "4553000A",
  "project": "../../titles/timesplitters2",
  "exe": "timesplitters2_recomp",
  "launcher": "timesplitters2_recomp_launcher",
  "release_name": "timesplitters2",
  "images": [{"sha1": "2809eb147385723eaa90c425be89ee4db033fc5a", "name": "PAL"}]
}
```

`project` is relative to the recipe, so a title in its own repository carries
its recipe and says `".."`. The release copies the project's tracked
`CMakeLists.txt`, `src/` and `config/`, plus anything the recipe lists in
`include`. A plain `xbe_sha1` list is still read, as images without names.

Each image may name a `config` folder in the project holding that image's
`seeds.json`, `xdk_symbols.json` and `xbe_patches.json`; without one the
builder looks in `config/`. Without `xdk_symbols.json` it runs XbSymbolDatabase
on the player's XBE, as `tools.xdk_symbols` does at a desk.

A title whose lift does more than the pipeline (split2-recomp writes a region
header and merges per-region overrides first) names its own script instead:

```json
"lift": ["scripts/build.py", "--game", "{xbe_dir}", "--no-build", "--relift"]
```

The builder runs it with the bundled Python, from the copied project, with
`{xbe_dir}` replaced by a folder holding only the player's `default.xbe`, and
with `xboxrecomp` in the project linked to the release's toolkit. The script
must leave the lifted code where the title's CMakeLists expects it; the
builder then configures and builds as usual.
A file the title's CMakeLists requires before it configures and the lift
writes goes in the recipe's `placeholders`, so the launcher, which is built
without a lift, can configure: `"placeholders": ["src/recomp/ts2_region.h"]`.

## Releasing from a title's repository

A title's repository carries the recipe and a workflow run on version tags
(split2-recomp's `.github/workflows/release.yml` is the model): check out with
the toolkit submodule, install the toolkit's Linux build dependencies and
DXC, run `make_release` from the submodule with `--version <tag> --archive`,
and attach the archive to a GitHub release. No game files are involved: the
release carries the tools and the title's project, never lifted code. The
toolkit's own CI builds the TimeSplitters 2 release on every push as a check,
and publishes nothing.

## How the pieces fit

**Toolchain** (`tools/release/fetch_toolchain.py`): every download pinned to one
file and checked against its SHA-256. The wrappers target glibc 2.38 and pass
`-fno-sanitize=all`, because `zig cc` at -O0 adds UB-sanitizer traps that would
turn guest undefined behaviour into crashes; at -O1 to -O3 it adds none and
maps all three to one optimisation. Everything the release builds needs glibc
2.38 or older: SteamOS (2.41) and current distributions.

**Engine** (`tools/release/make_engine_package.py`): #84's prebuilt-engine export
made relocatable. It copies the libraries the export names, the header folders
they publish (headers only), SDL's own CMake package and `templates/runtime`,
rewrites every path relative to the package, and refuses to write one that
still names the machine it was made on. The package's `CMakeLists.txt`
(`tools/release/engine_package`) is what a title's
`add_subdirectory(${XBOXRECOMP_DIR})` finds: it imports the engine, builds
`xbox_config` with the title's own widescreen and timing settings, and applies
`cmake/lifted_code.cmake`. The import compares a package by compiler identity,
not path. The engine is built with the release's zig, and with the build
machine's headers searched after zig's own libc headers
(`-idirafter /usr/include`) for what SDL loads at run time: Wayland, X11,
PipeWire, PulseAudio, ALSA, udev. OpenSSL is gone: nothing used it.

**Builder** (`tools/builder`): extract or mirror, check the hash, name the XDK
functions, run `scripts/recompile.py` with the bundled Python, configure and
build with the bundled CMake and zig against the engine package, install,
stamp. Progress is one JSON object a line on stdout:

```json
{"event":"progress","stage":"compile","label":"Compiling","done":12,"total":61,"detail":"recomp_0011.c","overall":0.62}
{"event":"error","message":"This isn't a copy of TimeSplitters 2 ...","unsupported":true}
{"event":"done","exe":".../timesplitters2_recomp","game_dir":".../game"}
```

`xboxrecomp-build status` reports `ready`, `stale`, `unbuilt` or `no-source`.
Exit 2 means a disc the release does not support. Linked by zig, it needs glibc
2.34.

**Launcher** (`src/launcher`): `release.json` beside it switches it into release
mode. Until the build is ready it shows the first-run or build screen instead
of the tabs; the build screen's bar carries a moving highlight, so a long stage
on one percentage still looks alive, and a finished build says so, with how
long it took, until the player presses OK. At every start it checks that the
game folder's links still resolve (a picked folder is linked, not copied); if
the folder was moved or deleted it asks where the files are now and runs
`xboxrecomp-build relink`, which lays the game folder out again and rebuilds
nothing, refusing a different copy of the game (a full build is offered
instead). `status` reports `moved` for the same case. Every launcher's window is titled
"DirectXRecomp - <game>". The front ends gained a job (a child process read a line at a time,
in its own process group so Cancel stops the compilers too) and a picker
(`SDL_ShowOpenFileDialog`/`SDL_ShowOpenFolderDialog`). The SDL front end has
both; the Windows one returns 0 for now. Closing the window cancels a build.

## Measured (Oct 2026, 6-core SteamOS machine, games running alongside)

| | TimeSplitters 2 | Turok: Evolution |
|---|---|---|
| Build on bare SteamOS from a folder | 76 s | 68 s |
| From a disc image (1.8 GB extracted) | 79 s | -- |
| Rebuild from the same image | 9 s | -- |
| Lifted C against a full checkout's | identical | identical |
| Extracted files against the folder | identical | -- |
| Capped, in level | 60 fps, 7,479 swaps | 118.8 fps, 16,114 swaps |
| Uncapped, in level | 377 fps (clang 383, GCC 369) | 131 fps (clang ~128, GCC ~133) |
| Undecoded MMIO accesses | 0 | 0 |

The release-built games behave as the GCC and clang builds do: same verdicts,
swap counts within 1%, same audio health.

## Testing without anyone's screen

- `SDL_VIDEODRIVER=offscreen RECOMP_LAUNCHER_SHOT=<file.bmp>
  [RECOMP_LAUNCHER_SHOT_FRAMES=<n>]` draws the launcher into a file and quits,
  without opening a window.
- `RECOMP_LAUNCHER_BUILD=<disc>` starts building at once: the whole path
  through the launcher, from a script (and a front end that knows the disc).
- `XBOXRECOMP_BUILD_DIR=<dir>` keeps a test's builds out of the player's folder.
- `flatpak-spawn --host <release>/bin/xboxrecomp-build build --source <disc>`
  from the SDK is the clean-machine check: bare SteamOS has no compiler,
  CMake, headers or Capstone.

## Not done yet

- Windows (llvm-mingw) and macOS releases; the Win32 launcher's job and picker.
- The desktop file dialogs are built but not exercised here (offscreen has
  none); in Game Mode (gamescope) they may not appear, so the path box stays.
- No archive or signature step: `make_release` writes a folder.
- `process_alive` is Linux-only; elsewhere a dead build's lock is removed by hand.
- An image is extracted, not read in place; the pipeline is still Python.
