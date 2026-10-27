# Android

TimeSplitters 2 plays on an Android handheld: an AYANEO Pocket Fit (Snapdragon
8 Gen 3, Adreno 750, Android 14) runs it at 60 fps in Siberia, with sound, the
console's pacing, widescreen, an in-game menu and touch controls. The work is on
`feat/android` (toolkit) and `feat/android` in split2-recomp. This page is what
someone picking it up needs: how the app is put together, how to build and test
it, and what is not done.

The port is the Linux one on arm64 with Android's C library: memory, faults,
threads and the guest lock are the macOS arm64 paths (`memory-layout.md`), the
renderer is the Vulkan RHI (`vulkan-backend.md`), and the window, sound and pads
are SDL3. What is Android's own is below.

**Guest threads run on the prime core** (`RECOMP_GUEST_ONE_CPU`, on by default
on Android only, `=0` to switch off). The guest lock lets one guest thread run
at a time, so a title with several busy threads hands the lock over thousands
of times a second, and each handoff wakes a different host thread; a phone's
scheduler wakes such a thread onto whichever core it likes, often a small one at
a low clock. Pinned to the fastest core (`cpu_capacity` 1024, cpu7 on a
Snapdragon 8 Gen 3) a handoff is a context switch. Gladius's Sofdec intro went
from 38-39 ms a frame to its own 30 fps, and its handoffs from ~5500 to ~60
per 5 s; TimeSplitters 2, nearly one thread, never showed the problem. Host
threads (renderer, audio, timers) are given the whole CPU set back when they
start (`w32_thread_unpin`), since a Linux thread inherits its creator's mask.

## Building an APK

An APK carries no game code. The player installs it, chooses their own copy
of the game (a folder or a disc image), and the app builds the game on the
device ([Building on the device](#building-on-the-device)). So the APK needs
no game to make either:

```bash
python3 xboxrecomp/tools/android/fetch_deps.py        # once: into xboxrecomp/third_party/android
python3 xboxrecomp/tools/android/build_apk.py --package org.xboxrecomp.ts2 \
    --label "TimeSplitters 2" --recipe release/split2.json
```

`fetch_deps.py` fetches what the APK carries besides the engine, at the
versions it was tried with: python.org's CPython 3.14.8 for Android, zig 0.17.0
for aarch64 Linux, Capstone 5.0.7 (built with the NDK) and DXC (built by
`build_dxc.sh`: nobody publishes an Android DXC, and the renderer compiles HLSL
at run time; `HLSL_DISABLE_SOURCE_GENERATION` is on, because regenerating DXC's
headers runs the host's clang-format and a newer one fails the build).

`build_apk.py` then, like a desktop release (`make_release.py`), configures a
copy of the title's recipe in `build/android-release` with a placeholder for
the lifted code, builds the engine (`libmain.so`) and Python's launcher
(`libpyexec.so`), makes the build kit (`make_kit.py`) from that build's
compile flags, and packs everything (`make_apk.py`): 37 s from clean, 89 MB.

| In the APK | What it is |
|---|---|
| `lib/arm64-v8a/libmain.so` | the engine, every library linked whole, compiled with `XBOXRECOMP_SPLIT_TITLE` |
| `libpython3.14.so`, `libpyexec.so` | Python, and a program that starts it (`android/pyexec.c`) |
| `libzig.so` | zig: the compiler, named as a library |
| `libxbsdbcli.so` | XbSymbolDatabase's CLI, which names the XDK functions in the player's XBE; linked as the kit's `bin/XbSymbolDatabaseCLI`, where a desktop release has it |
| `libcapstone.so` | Capstone, for the pipeline |
| `libdxcompiler.so` | DXC, for the renderer |
| `assets/buildkit.zip` | 18 MB: Python's standard library (no tests, no GUI), the pipeline and the title's recipe as a desktop release takes them, the headers and flags the title's sources compile with, the NDK sysroot, zig's library without other platforms' C libraries |

Programs and loadable code are libraries because Android starts programs only
from an app's library folder and loads code only from there or its internal
folder, never from shared storage. `extractNativeLibs` is on so they are real
files there (and compressed in the APK). There is no Gradle: SDL's Java
activity and `android/java` are compiled with javac and d8, the manifest
(`android/AndroidManifest.xml.in`) is linked with aapt2, and the APK is signed
with a key made once and kept in the per-user folder (`android/signing.p12`):
an update signed with another key is refused, and replacing the app to change
it deletes its saves. The icon is the toolkit's unless the recipe names one
(`"android": {"icon": ...}`; `self-build-release.md`, "Android"). Needs the
Android SDK (build tools, platform 34+), an NDK and a JDK.

A title's `CMakeLists.txt`, on Android, makes its sources an object library
and calls the toolkit's `xboxrecomp_android_libraries` with it
(`cmake/android.cmake`), compiling its `main.c` with `main=recomp_title_main`;
split2-recomp's is the example.

Install with `adb install -r <apk>`; `-r` keeps the app's folder and saves.

## Building on the device

`BuildActivity` unpacks the kit into the internal folder once per install,
then runs `tools/android/device_build.py` with the APK's Python. It does what
`xboxrecomp-build` does on a desktop, and reports progress the same way, as
JSON lines (copied to logcat as `xboxrecomp-build`; `build.log` is in the
internal folder, which adb cannot read):

1. the game: an image extracted once (`tools.xiso`), into the app's external
   folder; a folder read where it is
2. the check: `default.xbe`'s SHA-1 against the recipe's images
3. the lift: the title's own recipe (`scripts/build.py --no-build --relift`
   for TimeSplitters 2) on a private copy of the XBE, in a copy of the
   project with `xboxrecomp/` linked to the kit's pipeline. A recipe that
   names no lift is lifted as the desktop builder lifts it: the XDK symbols
   from the recipe's `config/xdk_symbols.json`, else found with
   XbSymbolDatabase, then `scripts/recompile.py` with the image's seeds and
   XBE patches
4. the compile: the lifted C and the title's sources, by zig, eight at a time,
   with the recorded flags (`{kit}` and `{project}` filled in) and a libc
   paths file naming the NDK sysroot (`ZIG_LIBC`)
5. the link: `libgame.so` against the APK's `libmain.so`, with
   `-Bsymbolic`, into the internal folder, and a stamp (source, kit version)

The next start with the same copy and kit goes straight to the game; an
updated APK (a new kit) builds again by itself. On the Pocket Fit,
TimeSplitters 2 from `/sdcard/roms/xbox` builds in 99 s with the screen on
(lift 39 s, compile 50 s, link 10 s).

**The engine and the title are two libraries.** Android binds every symbol
when a library loads, so `libmain.so` cannot name what `libgame.so` defines.
With `XBOXRECOMP_SPLIT_TITLE`, the engine reaches the title only through what
`src/host/android_title.c` fills in once `libgame.so` is loaded: the
`HLE_ORIGINAL` / `HLE_IMPORT_VAR` slots (registered by name, `hle.h`),
trampolines for `recomp_lookup` and `recomp_lookup_manual`, and
`recomp_title_symbol` for a weak engine default the title overrides
(`recomp_xbe_patches`: without it TS2 ran on the unpatched image, a white
front end and a fault at guest `0xFFFFFFFD`). The lifter's output does not
change. Uncapped, Siberia runs at about 133 fps split against 153 fps as one
library; the gap is likely the title reaching the engine's thread-local CPU
state across the library boundary, and does not show at 60.

Two things Python for Android lacks: a program to start (hence `pyexec`), and
`_multiprocessing` (no named semaphores), so the lifter lifts one function at
a time there by itself. `scripts/recompile.py` can also run its stages in one
interpreter (`RECOMP_IN_PROCESS=1`, or when there is no Python executable),
with output identical to separate processes, and takes `--hle-symbols` for a
game folder that is only read.

For development, a `libgame.so` built elsewhere can be pushed to
`<files>/libgame.so`; the app copies it in before loading (adb cannot reach
the internal folder). `RECOMP_ANDROID_PROBE` (`android_build.c`) runs the
feasibility probes instead of the title.

## How the app works

**Three activities.** `SetupActivity` is the launcher entry and finds the
game; `BuildActivity` builds it (above); `RecompActivity` is SDL's activity
with the engine in `libmain.so` (SDL is static, so there is no `libSDL3.so`),
which loads the title's `libgame.so`.

**Finding the game** (`SetupActivity.java`). A folder chosen before, then a copy
in the app's own folder (`<files>/game`, where `adb push` puts one), then: it
asks for all-files access and looks under `roms/`, `Download/`, `games/` and SD
cards for disc images (`.iso`) and for a `default.xbe` whose SHA-1 is one of
the release's images (the manifest's `xboxrecomp.xbe_sha1`). One matching
folder starts and is remembered; otherwise the copies found are listed, and
none asks for a path. An image is checked when it is built. Shared storage ignores case, so `roms/` and
`ROMs/` are one folder and matches are de-duplicated by case. The game is read
where it is and never written to.

**The entry** (`src/host/android_main.c`). `SDL_main` sends stdout and stderr
down a pipe to logcat (tag `xboxrecomp`) and to `<files>/xboxrecomp.log`, reads
`RECOMP_*` switches from `<files>/recomp_env.txt`, sets the folders, and calls
the title's `main` (renamed `recomp_title_main`). `<files>` is
`/storage/emulated/0/Android/data/<package>/files`: no permission needed, adb can
reach it, an uninstall deletes it.

| Folder | Default | Holds |
|---|---|---|
| `RECOMP_GAME_DIR` | what SetupActivity found, else `<files>/game` | the game, read only |
| `RECOMP_USER_DIR` | `<files>/user` | settings (`titles/<id>.conf`), input bindings, caches, saves |
| `RECOMP_HDD_DIR` | `<files>/hdd` | E: (`\Device\Harddisk0\Partition1`): TS2's `UDATA` profile |

E: is the game's own folder on a desktop. Here the game may be a ROM folder
other programs share, so E: is the app's.

**The window and the swap chain.** The window is fullscreen with the system bars
hidden. The Pocket Fit's panel is portrait underneath: the surface reports a 90
degree transform with a landscape extent. The swap chain asks for no
pre-rotation and lets the system rotate the frame, and the `SUBOPTIMAL` Android
then returns on every acquire and present is not a reason to rebuild it
(`SC.unrotated`, `rhi_vulkan.c`). Drawing pre-rotated ourselves would save the
compositor's rotation; it has not been needed.

**Pause and resume.** Android destroys the window when the app is paused and
makes a new one on resume. The host hands the renderer the window as SDL has it
now (`RhiNativeWindow.current`, read under SDL's property lock with a reference
taken, because SDL releases the window whether or not a swap chain uses it);
before each frame the renderer compares, drops the surface and swap chain when
the window is gone and makes both again when it is back. Meanwhile the title is
held at its next `Swap` (`host_suspended`, set from an SDL event watch on
`WILL_ENTER_BACKGROUND` / `DID_ENTER_FOREGROUND`), so the game does not play on
unseen.

**The in-game menu** (`GameMenu.java`). Back opens a panel down the right:
Resume, Settings, Quit game. A controller's own Back button is left to the game.
The title is held while the menu is open (`HOST_SUSPEND_MENU`; the reasons are
bits, so coming back from the background with the menu open keeps it held).
Settings call into `android_main.c` over JNI and apply at once where the runtime
can change them: the frame rate counter and cap (`hle_settings.h`, the desktop's
F9 and F10) and the touch controls. Each change asks for a redraw
(`host_request_redraw`): the held thread presents its frame again with the
change (`xbox_D3D8Represent`), so it shows behind the menu while the game stays
where it is. Widescreen and resolution are read only at start and are named
"(requires restart)". Every change is written to the title's settings file.
Quit game asks first, in a modal with No focused.

The menu moves its own focus for a pad (d-pad between rows, left and right on
sliders and in the question, A or Enter to press, B back a page, Start closes).
Two things learned the hard way: SDL's activity takes the arrow keys before
Android's focus movement sees them, and since Android 8 any clickable view is
focusable, so the menu's shade held the focus and A "pressed" it.

**Touch controls** (`src/input/touch_controls.c`, `src/d3d/d3d8_touch.c`). A
whole Xbox pad in the "Edges" layout, chosen from mock-ups: triggers in the top
corners, White and Black under them, d-pad and face buttons at mid height,
sticks at the bottom, Back and Start top centre, sized from the screen height
with each side measured from its own edge. How each control reads follows Steam
Link's controller as DroidDeck uses it (a stick is its offset clamped to the
ring, eight 67.5 degree d-pad sectors, a finger slides between buttons). They
are added to controller 1 in `recomp_bindings_sample` and drawn over the
presented frame, bars included, as distance functions in one pass. `auto` (the
Android default) shows them with no pad connected and hides them when a pad is
connected or pressed; a touch brings them back. `touch_controls`,
`touch_opacity` and `touch_scale` are in the settings file and the menu.

**Faults.** The MMIO emulation's SIGSEGV/SIGBUS handlers sit behind ART's own
(libsigchain): ART takes the faults in managed code and passes the rest on. No
change was needed.

## Testing on a device

Every run follows the device rules: our own package only (never another app or
its data), `RECOMP_MUTE=1`, a time limit, fresh saves; no uninstall, settings
change or reboot without asking. Only `install`, `logcat`, `screencap`, input
injection and files pushed into the app's own folder.

```bash
P=/sdcard/Android/data/org.xboxrecomp.ts2.dev/files
R=$P/runs/<label>
adb shell "mkdir -p $R/hdd $R/saves $R/user && chmod 777 $R $R/hdd $R/saves $R/user"
printf 'RECOMP_MUTE=1\nRECOMP_SAVE_DIR=%s/saves\nRECOMP_HDD_DIR=%s/hdd\nRECOMP_USER_DIR=%s/user\n' $R $R $R > env.txt
adb push env.txt $P/recomp_env.txt
adb shell am start -n org.xboxrecomp.ts2.dev/org.xboxrecomp.app.RecompActivity
sleep 90; adb exec-out screencap -p > shot.png
adb shell am force-stop org.xboxrecomp.ts2.dev
adb pull $P/xboxrecomp.log
adb shell rm $P/recomp_env.txt          # or the player's next launch is muted
```

`RECOMP_INPUT_SEQ` (split2-recomp's `CLAUDE.md`) walks a fresh save to Siberia.
A copy of the game for testing goes in `<files>/game` (`cp -r` on the device,
then `chmod -R a+rwX`, since files the shell writes are not the app's).

Things that cost time:

- **Someone else may be holding the device.** Check
  `dumpsys activity activities | grep Resumed:` and `dumpsys power | grep
  mWakefulness` before a run. A screen that has gone to sleep sends the app to
  the background as soon as it starts (black screenshots), and keys injected
  while the app is not in front go to whatever is.
- **Files the app writes cannot be read or deleted from adb** (`Permission
  denied` under `<files>`), so give each run its own folders rather than
  cleaning up after one.
- **`adb install -r` closes the running app.**
- **A release APK cannot be profiled**; build a side-by-side package with
  `build_apk.py --package <release package>.dev --profileable`, which
  `simpleperf record --app <package>` accepts. It has its own data and needs
  all-files access once (`adb shell appops set <package>
  MANAGE_EXTERNAL_STORAGE allow`, our own package).

## What the desktop shares

These changes run on every host and need a Windows regression before `main`:
the flip gate's hold and redraw (`xbox_memory_layout.c`), `host_suspended` and
`host_request_redraw` (`host_main_*.c`), the frame rate counter drawn at present
time over the swap chain (its size follows the picture; on Windows its GDI text
is scaled in whole steps), the settings file's touch lines, and the swap chain's
handling of a rotated surface (which a desktop never reports).

## Not done

- **A release workflow.** The APK carries no game code and builds the game on
  the device, so it is a thing that can be published; nothing makes one in CI
  or on a tag yet. There will be no Google Play builds.
- **Disc images have not been tried on a device** (the path is written:
  `tools.xiso`, an extract into the app's external folder).
- **The split's uncapped cost** (above).
- **The DXT fallback and the unsupported-GPU notice.** Adreno has BC textures;
  Mali devices are untested and unsupported for now.
- **An arm64 Linux CI job**, the first CI run of the arm64 fault and memory
  paths.
- **Input remapping in the menu** (a separate branch).
- **A per-title icon for TimeSplitters 2.**
- **Building on the device.** Out of scope.

## The Pocket Fit

| | |
|---|---|
| SoC / GPU | Snapdragon 8 Gen 3 (`pineapple`), Adreno 750, driver 0762.24 (Jul 2024) |
| Vulkan | 1.3.128; BC and ETC2 textures, dual-source blend, geometry shaders |
| Android | 14 (API 34), 4 KB pages, 12 GB |
| Screen | 1920x1080 landscape (panel 1080x1920), 440 dpi |
| TS2 | 60 fps in Siberia, about 1.5 cores busy, 48.7 MB APK |
