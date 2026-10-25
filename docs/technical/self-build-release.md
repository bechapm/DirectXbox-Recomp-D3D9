# Self-building releases

A per-game download with no game code in it. The player unpacks it and starts
the launcher, which asks for their own copy of the disc (an image or a folder),
recompiles the game from it on their machine, and then shows the usual settings
and Play. Linux, macOS (Apple Silicon) and Windows. What differs on the Mac
and on Windows is in [macOS](#macos) and [Windows](#windows).

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
                          (and on a Mac an xcrun that fails, below)
python/                   CPython 3.13 (python-build-standalone) with Capstone
engine/                   the runtime, prebuilt (make_engine_package.py)
toolkit/                  the pipeline: tools/, scripts/recompile.py, config/,
                          templates/runtime, src/hle (only what git tracks)
recipe/                   the title's CMakeLists, src/ and config/
dxc/                      the shader compiler the renderer loads
LICENSE NOTICE.txt README.txt
```

TimeSplitters 2's is 637 MB unpacked, 497 MB of it the toolchain (zig is most).

## The player's folder

`$XDG_DATA_HOME/xboxrecomp/builds/<TITLEID>` (`~/.local/share/...`; on a Mac
`~/Library/Application Support/xboxrecomp/builds/<TITLEID>`), or
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
the same on `macos-15` with `tools/macos/fetch_vulkan_sdk.sh` in place of the
Linux dependencies, and attach both archives to a GitHub release from one job
that runs after both. A repository without the submodule checks the toolkit
out beside itself (Rainbow-six-three-recomp's workflow). No game files are involved: the
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
"DirectXboxRecomp - <game>". The front ends gained a job (a child process read a line at a time,
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

## macOS

A Mac release is an application, `<Title>.app`, for Apple Silicon and macOS 13
or later, and the player's machine needs nothing installed: not Xcode, not its
command line tools.

```
<Title>.app/Contents/MacOS/<name>      the launcher
<Title>.app/Contents/Resources/        everything the Linux release has beside
                                       its launcher, plus:
  runtime/                             what the builder puts beside the game:
    libSDL3.0.dylib                      the SDL the game links (@executable_path)
    libvulkan.1.dylib libMoltenVK.dylib  the Vulkan loader and its driver
    vulkan/icd.d/MoltenVK_icd.json       the driver's manifest, beside the loader
    libdxcompiler.dylib                  DXC
  licenses/                            the loader's and MoltenVK's
  icon.icns
README.txt                             beside the .app in the archive too
```

The archive is `<name>-<version>-macos-arm64.tar.xz`. The player's folder is
`~/Library/Application Support/xboxrecomp/builds/<TITLEID>`, beside the
runtime's settings and saves.

**How the game is built without an SDK.** zig is given an explicit target,
`aarch64-macos.13.0`, which keeps it to its own macOS libc headers and
libSystem stubs, and it builds its own libc++, so what it links needs only
libSystem. Two things in the engine are not covered by that, and are dealt with
in the engine rather than by asking for Xcode:

- Apple's frameworks. SDL is the only part of the engine that links them, so
  `make_release` builds it apart, with Xcode, as a dylib
  (`XBOXRECOMP_EXTERNAL_SDL`, `SDL3_DIR`), and the engine package carries it.
  `host_mac.c` looks up the three Objective-C runtime functions it calls
  instead of linking libobjc, and `xbox_host` links no frameworks of its own.
  A game links `@rpath/libSDL3.0.dylib` and libSystem, nothing else.
- Two SDK-only headers, `<mach/mach_vm.h>` and `<os/os_sync_wait_on_address.h>`:
  `src/platform/darwin_compat.h` declares what is used from them (weakly, for
  the macOS 14.4 one) when they are missing.

zig cannot use the SDK's framework headers alongside its own, so the
launcher, which draws through Core Text and Core Animation, is built with
Xcode as well, from the toolkit's sources. It shares no C++ with the engine.

On a Mac without the command line tools `/usr/bin/xcrun` is a stub that
offers to install them, in a dialog, and CMake runs `xcrun --show-sdk-path`
when it is given no SDK. The toolchain carries a `bin/xcrun` that fails
quietly, and the builder puts that folder first on `PATH`.

**What goes beside the game.** The builder copies `runtime/` beside the
executable rather than linking it, so moving the application after a build
does not break the game; on APFS the copy is a clone. The renderer finds a
Vulkan loader beside the executable first, and the manifest beside the loader
in `vulkan/icd.d` (`rhi_vulkan.c`), so no Vulkan SDK on the machine is
involved.

**Gatekeeper.** The release is not signed with a Developer ID. A downloaded
copy is quarantined, every file of it, and macOS checks each quarantined tool
the build starts. The README asks the player to run
`xattr -dr com.apple.quarantine "<path>/<Title>.app"` before the first open.
A player who uses Open Anyway instead gets the launcher, which then tries to
clear the flag from its own bundle; opened from a translocated copy (before
it has been moved out of the folder it was unpacked in) it says to move it
first. Whether macOS lets an ad-hoc-signed application clear its own flag
after Open Anyway has not been tested: it needs a person at the dialogs.

**Making one** needs Xcode, Cargo and a Vulkan SDK (`VULKAN_SDK`, or
`--vulkan-sdk`); otherwise the same command. CI installs LunarG's SDK with
`tools/macos/fetch_vulkan_sdk.sh` (pinned, checked, no administrator).

**Measured** (Oct 2026, M4 MacBook, under `sandbox-exec` with Xcode, the
command line tools, Homebrew, the Vulkan SDK and every checkout unreadable,
and `xcrun`, `git`, `cc` and `ld` not executable):

| | TimeSplitters 2 (split2-recomp) | Rainbow Six 3 |
|---|---|---|
| Release, unpacked | 637 MB | 636 MB |
| Build from a folder, first time | 42 s | 121 s |
| The same through the launcher | 50 s | -- |
| From a disc image (1.8 GB extracted) | 58 s | -- |
| Rebuild from the same folder | 7 s | -- |
| Lifted C against a desk lift | identical | identical |
| Extracted files against the folder | identical | -- |
| Running, against a desk build | ~118 fps uncapped in the front end, sound heard | 60 fps, same swaps, draws and audio health over 45 s |

Rainbow Six 3's own lift (`scripts/release_lift.py`) undoes a hard-drive
copy's patches before lifting, as its desk build does, so its release accepts
both the disc's `default.xbe` and a hard-drive copy's.

## Windows

A Windows release is a folder like Linux's, with `<name>.exe` as the launcher,
published as `<name>-<version>-windows-x86_64.zip`, for 64-bit Windows 10 or
11 with nothing installed: not Visual Studio, not a redistributable. The
player's folder is `%LOCALAPPDATA%\xboxrecomp\builds\<TITLEID>`.

**The compiler.** zig again, for `x86_64-windows-gnu`: MinGW-w64's headers and
import libraries, and the UCRT every Windows 10 has, so the game imports
only Windows' own DLLs (Direct3D 11, D3DCompiler_47, XAudio2 2.9, XInput 1.4,
Media Foundation, the UCRT). The renderer is Direct3D 11, so a Windows release
carries no DXC. Everything is built with zig -- the engine, the launcher, the
player's game -- except the builder, which Cargo links with Visual Studio's
linker on CI, with the C runtime linked in (`+crt-static`).

The engine needed little for MinGW:

- `xbox_winnt.h` defined `__debugbreak` for every compiler but MSVC, and
  MinGW declares it itself; `kernel_path.c` keeps `kernel.h`'s `KernelMode`
  macro out of `shlobj.h`, which reaches a member of that name in MinGW.
- MinGW's `xaudio2fx.h` and `xapofx.h` (from Wine) have no parameter blocks,
  SDK creators or I3DL2 conversion: `src/hle/xaudio2_mingw.h` declares them
  (XAudio2 2.9's layouts) and carries FAudio's `ReverbConvertI3DL2ToNative`
  (zlib). XAudio2_9.dll exports the rest.
- MinGW names import libraries after their DLLs: `cmake/mingw_libs.cmake`
  makes `xinput` and `d3dcompiler` targets for `xinput1_4` and
  `d3dcompiler_47`, so the titles' own CMakeLists (which say `xinput`) are
  unchanged; `xaudio2_9` and the WaitOnAddress API set are named outright.
- MinGW defines `__forceinline` as `extern inline`, which a static function
  cannot be: the lifted code's runtime headers use `RECOMP_FORCEINLINE`.
- `RECOMP_SAMPLE`'s stack walk needs SEH, which MinGW does not have: there
  it samples the pc alone.

**The wrappers.** CMake's compiler can be no shell script on Windows, so the
toolchain's `zcc.exe`, `zc++.exe`, `zar.exe`, `zranlib.exe` and `zrc.exe` are
one small program (`tools/release/zigwrap.c`, built by the release's zig) that
runs `zig.exe` with its name's arguments and the caller's, unchanged but for
`-mwindows`, which zig ignores and becomes `-Wl,--subsystem,windows` (or the
game would open with a console). `zrc` is `zig rc`, CMake's resource compiler
for the executables' icon.

**The builder** links a folder's directories with junctions (a symbolic link
needs Developer Mode or an administrator) and hard-links its files, copying
one on another drive; strips `\\?\` from the paths it hands the tools, and
gives CMake forward slashes; starts every tool with no console window; and
checks a stale lock with `OpenProcess`.

**The launcher** (`launcher_win32.c`) runs the builder in a job object that
kills everything in it when closed, so Cancel, or closing the window, stops
the compilers too, and reads its output through a pipe it peeks at; it asks
for the disc with the shell's own open dialog (`IFileOpenDialog`).

**Not signed.** Windows may say it protected the PC the first time the
launcher starts (SmartScreen): More info, Run anyway. The tools it starts are
not asked about.

**Tested** only as far as a Mac can: the engine, the launcher and a whole
TimeSplitters 2 build cross-compiled with the release's zig, importing only
system DLLs, and the builder type-checked for Windows. CI builds the release
on `windows-2022` and runs the builder's tests there (the junction mirror
included). A player-side build and a run need a Windows machine.

## Testing without anyone's screen

- `SDL_VIDEODRIVER=offscreen RECOMP_LAUNCHER_SHOT=<file.bmp>
  [RECOMP_LAUNCHER_SHOT_FRAMES=<n>]` draws the launcher into a file and quits,
  without opening a window. On a Mac too: the launcher asks SDL for no OpenGL
  there, which offscreen does not have.
- `RECOMP_LAUNCHER_BUILD=<disc>` starts building at once: the whole path
  through the launcher, from a script (and a front end that knows the disc).
- `XBOXRECOMP_BUILD_DIR=<dir>` keeps a test's builds out of the player's folder.
- `flatpak-spawn --host <release>/bin/xboxrecomp-build build --source <disc>`
  from the SDK is the clean-machine check: bare SteamOS has no compiler,
  CMake, headers or Capstone.
- On a Mac, `sandbox-exec -f <profile> env -i HOME=$HOME PATH=/usr/bin:/bin
  <release>/bin/xboxrecomp-build build --source <disc>` with a profile that
  denies reading `/Applications/Xcode.app`, `/Library/Developer`,
  `/opt/homebrew`, `/usr/local` and the checkouts, and executing
  `/usr/bin/xcrun`, `git`, `cc` and `ld`. Do not quarantine a test copy and
  start it: Gatekeeper then waits on a dialog, and locks the bundle.

## Not done yet

- Windows: a player-side build and a run on a real machine (above); the
  launcher passes paths in the system's ANSI code page, so a path with
  characters outside it is not reachable.
- macOS: no Developer ID signing or notarisation, and the Open Anyway path
  untested (above). Intel Macs are not built for.
- The desktop file dialogs are built but not exercised here (offscreen has
  none); in Game Mode (gamescope) they may not appear, so the path box stays.
- `process_alive` has no Windows form yet; there a dead build's lock is removed by hand.
- An image is extracted, not read in place; the pipeline is still Python.
