"""Assemble a self-building release for one title: everything but the game.

    python3 -m tools.release.make_release config/releases/<title>.json \\
        --out <dir> [--work <dir>] [--cache <dir>] [--version <string>]

A release is a folder a player unpacks and starts. It carries no game code:
the launcher asks for the player's own disc, and xboxrecomp-build recompiles
it with what the release brings (docs/technical/self-build-release.md):

    <name>                 the launcher, which the player starts
    release.json           the title, this release's version, and the
                           default.xbe images it was made for
    bin/xboxrecomp-build   the builder (tools/builder)
    bin/XbSymbolDatabaseCLI  names the XDK functions in the player's image
    toolchain/ python/     zig, CMake, Ninja; CPython with Capstone
    engine/                the runtime, prebuilt (make_engine_package.py)
    toolkit/               the pipeline: tools/, scripts/recompile.py,
                           config/, templates/runtime, src/hle
    recipe/                the title's project: its CMakeLists, src/ (main.c,
                           recomp_manual.c, any other sources) and config/
    dxc/                   the shader compiler the renderer loads
    icon.png               the toolkit's icon, for a desktop entry or Steam
    LICENSE NOTICE.txt README.txt

On a Mac the release is an application, <Title>.app: the launcher is
Contents/MacOS/<name>, and everything above is in Contents/Resources, with
runtime/ beside it -- what the builder puts beside the game it builds (the
SDL dylib, the Vulkan loader, MoltenVK and its manifest, DXC). zig builds the
player's game with no SDK, so the engine is built with it the same way, and
the parts that need Apple's SDK are built here with Xcode's compiler and
carried prebuilt: SDL, as a dylib, and the launcher.

The recipe (config/releases/<title>.json, or one in the title's own
repository) names the title, its project (relative to the recipe), the
executable and launcher targets, and the default.xbe images the title is
known to work from, each with a name and, when the title keeps one folder of
addresses per image, that folder; the builder refuses any other image. A
title whose lift does more than the pipeline names its own script as
`lift`, which the builder runs instead (docs/technical/self-build-release.md).

Run it where the toolkit builds (the Freedesktop SDK on SteamOS, with the
rust-stable extension for the builder): the engine and launcher compile
against this machine's SDL, X11, Wayland and audio headers, with the
release's own zig, so what they need at run time is only what every player
has. On a Mac it needs Xcode, Cargo and a Vulkan SDK (VULKAN_SDK, or
--vulkan-sdk). Nothing here starts a title.
"""

import argparse
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT))
from tools.release.fetch_toolchain import fetch, host_platform  # noqa: E402

PLATFORM = host_platform()
MACOS = PLATFORM.startswith("macos")
WINDOWS = PLATFORM.startswith("windows")
EXE = ".exe" if WINDOWS else ""
# The oldest macOS a release runs on (fetch_toolchain.py's target says the same).
MACOS_MIN = "13.0"

# What the toolkit's pipeline needs at a player's machine, beyond tools/.
TOOLKIT_FILES = ["scripts/recompile.py", "LICENSE"]
TOOLKIT_DIRS = ["config", "templates/runtime", "src/hle"]
# Not needed to lift a title: the Rust builder's sources, platform scripts.
TOOLS_SKIP = {"builder", "linux", "macos", "vk_spike", "release", "__pycache__"}

# The build machine's headers for what SDL loads at run time, after zig's own
# libc headers, and its libraries for the names SDL records. Linux only: on a
# Mac, SDL is built apart, with Xcode (build_sdl_macos).
HOST_FLAGS = [] if MACOS or WINDOWS else ["-DCMAKE_C_FLAGS=-idirafter /usr/include",
                               "-DCMAKE_CXX_FLAGS=-idirafter /usr/include",
                               "-DCMAKE_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu"]

# SDL as the runtime builds it (CMakeLists.txt), as a dylib.
SDL_OPTIONS = ["-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST_LIBRARY=OFF",
               "-DSDL_TESTS=OFF", "-DSDL_EXAMPLES=OFF", "-DSDL_INSTALL=OFF",
               "-DSDL_CAMERA=OFF", "-DSDL_GPU=OFF", "-DSDL_RENDER=OFF"]

NOTICE = """\
{title} for {os_name}, a self-building release of the xboxrecomp toolkit.

It contains no game code or data. It recompiles your own copy of the game,
on your machine, the first time you start it.

xboxrecomp is free software under the GNU General Public License, version 3
or later (LICENSE). This release also carries, each under its own licence:

  zig 0.17 (MIT), which includes LLVM/clang and lld (Apache-2.0 with LLVM
    exceptions), musl and glibc headers and stubs, and libc++/libunwind
  CMake 4.4 (BSD-3-Clause) and Ninja 1.13 (Apache-2.0)
  CPython 3.13 (Python Software Foundation License), from
    python-build-standalone, with Capstone 5.0.7 (BSD-3-Clause)
  SDL 3.4 (zlib), miniupnpc 2.3 (BSD-3-Clause) and libsamplerate
    (BSD-2-Clause), linked into the game's runtime
  XbSymbolDatabase (MIT), from the Cxbx-Reloaded project
  The DirectX Shader Compiler ({dxc_licenses})
{extra}"""

NOTICE_MACOS = """\
  The Vulkan loader and MoltenVK (Apache-2.0, licenses/), from the LunarG
    Vulkan SDK, in runtime/ with DXC, and put beside the game it builds
"""

README = """\
{title}
{line}

{start} The first time, it asks for your copy of the game: the disc
image (.iso or .xiso) or the folder its files are in. It then builds the game
from it, which takes a minute or two, and opens the settings; press Play.

What it builds is kept in {userdir}: the game's files, the build, and the
log (build.log) to attach to a bug report. An image is extracted there once;
a folder is linked, never copied or changed.

This release works from these images of the game's default.xbe (SHA-1):
{hashes}
Any other is refused, because it has not been tested.

{shortcut}

This is an independent project, not affiliated with, endorsed by or sponsored
by Microsoft Corporation. Xbox, DirectX and Direct3D are trademarks of the
Microsoft group of companies, used here only to say what this works with.
{title} is a trademark of its owner. This release contains no game code or
assets: the game is built on your computer from your own copy.
"""


def log(msg):
    print(f"[make_release] {msg}", flush=True)


def run(cmd, **kw):
    log("$ " + " ".join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode:
        sys.exit(f"make_release: failed ({r.returncode}): {cmd[0]}")


def tracked_files(repo, paths):
    """The files git tracks under `paths` of `repo`, relative to it."""
    r = subprocess.run(["git", "-C", str(repo), "ls-files", "-z", "--", *paths],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"make_release: git ls-files failed in {repo}: {r.stderr.strip()}")
    return [f for f in r.stdout.split("\0") if f and (repo / f).is_file()]


README_LINUX_START = "Start ./{name}."
README_LINUX_SHORTCUT = """\
icon.png is the icon to give a desktop shortcut, or a Steam shortcut
(Add a Non-Steam Game, then Properties)."""

README_WINDOWS_START = """\
Start {name}.exe. This release is not signed, so the first time Windows may
say it protected your PC: choose More info, then Run anyway."""
README_WINDOWS_SHORTCUT = """\
It needs 64-bit Windows 10 or 11, and nothing else installed: everything it
builds the game with is in this folder."""

README_MACOS_START = """\
Move {app} to Applications (or any folder you like). This release is not
signed with an Apple Developer ID, so before you open it the first time,
open Terminal and run

    xattr -dr com.apple.quarantine "/Applications/{app}"

(with wherever you put it). That clears the flag macOS puts on everything
downloaded, from the application and the tools it builds the game with.
Then open it.

(System Settings, Privacy & Security, Open Anyway also opens it, and it then
tries to clear the same flag itself. If the build then stops because macOS
will not run one of its tools, quit and run the command above.)
"""
README_MACOS_SHORTCUT = """\
It needs macOS {macos_min} or later on Apple Silicon. Nothing else is
installed: not Xcode, not its command line tools."""


def cm(path):
    """A path for a CMake -D: forward slashes, which it never reads as escapes."""
    return Path(path).as_posix()


def toolchain_args(tc):
    args = [f"-DCMAKE_MAKE_PROGRAM={cm(tc / f'ninja/bin/ninja{EXE}')}",
            f"-DCMAKE_C_COMPILER={cm(tc / f'bin/zcc{EXE}')}",
            f"-DCMAKE_CXX_COMPILER={cm(tc / f'bin/zc++{EXE}')}",
            f"-DCMAKE_AR={cm(tc / f'bin/zar{EXE}')}",
            f"-DCMAKE_RANLIB={cm(tc / f'bin/zranlib{EXE}')}",
            "-DCMAKE_BUILD_TYPE=Release", "-G", "Ninja"]
    if WINDOWS:
        # zig rc, for the executables' icon (cmake/title_icon.cmake).
        args.append(f"-DCMAKE_RC_COMPILER={cm(tc / 'bin/zrc.exe')}")
    return args


def build_sdl_macos(work, tc, env):
    """SDL as a dylib, with Xcode's compiler: its Cocoa, Metal and audio code
    needs Apple's SDK, which zig cannot use (fetch_toolchain.py). Its build
    folder is inside the engine's, so the engine package carries it. Only C
    crosses into it, so mixing compilers is safe."""
    sdl = work / "engine" / "sdl"
    run([tc / "cmake/bin/cmake", "-S", ROOT / "third_party/SDL", "-B", sdl, "-G", "Ninja",
         f"-DCMAKE_MAKE_PROGRAM={tc / 'ninja/bin/ninja'}", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_C_COMPILER=clang", "-DCMAKE_OBJC_COMPILER=clang",
         f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_MIN}", "-DCMAKE_OSX_ARCHITECTURES=arm64",
         *SDL_OPTIONS], env=env)
    run([tc / "cmake/bin/cmake", "--build", sdl], env=env)
    return sdl


def vulkan_sdk(arg):
    """The Vulkan SDK's macOS folder: lib/ holds the loader, MoltenVK and DXC."""
    for d in filter(None, [arg, os.environ.get("VULKAN_SDK")]):
        d = Path(d).expanduser()
        for cand in (d, d / "macOS"):
            if (cand / "lib/libMoltenVK.dylib").is_file():
                return cand
    sys.exit("make_release: no Vulkan SDK (set VULKAN_SDK or pass --vulkan-sdk): "
             "a Mac release carries its loader, MoltenVK and DXC")


def copy_dylib(src, dst):
    """A dylib, followed through its links and thinned to arm64."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src.resolve(), dst)
    archs = subprocess.run(["lipo", "-archs", str(dst)], capture_output=True,
                           text=True).stdout.split()
    if len(archs) > 1 and "arm64" in archs:
        run(["lipo", dst, "-thin", "arm64", "-output", dst])


def macos_runtime(rel, sdk, sdl, cache):
    """runtime/: what the builder puts beside the game it builds, where the
    game looks for each (src/d3d/rhi_vulkan.c, rhi_vulkan_dxc.cpp, and the
    @executable_path rpath for SDL)."""
    rt = rel / "runtime"
    for name in ("libvulkan.1.dylib", "libMoltenVK.dylib", "libdxcompiler.dylib"):
        copy_dylib(sdk / "lib" / name, rt / name)
    copy_dylib(sdl / "libSDL3.0.dylib", rt / "libSDL3.0.dylib")
    icd = {"file_format_version": "1.0.0",
           "ICD": {"library_path": "../../libMoltenVK.dylib", "api_version": "1.4.0",
                   "is_portability_driver": True}}
    src = sdk / "share/vulkan/icd.d/MoltenVK_icd.json"
    if src.is_file():
        icd = json.loads(src.read_text())
        icd["ICD"]["library_path"] = "../../libMoltenVK.dylib"
    (rt / "vulkan/icd.d").mkdir(parents=True)
    (rt / "vulkan/icd.d/MoltenVK_icd.json").write_text(json.dumps(icd, indent=4) + "\n")
    # Their licences, which the SDK does not carry: DXC's in dxc/, as on
    # Linux, the loader's and MoltenVK's in licenses/.
    (rel / "dxc").mkdir(exist_ok=True)
    (rel / "licenses").mkdir(exist_ok=True)
    for name, folder in (("dxc-license", "dxc"), ("dxc-notices", "dxc"),
                         ("moltenvk-license", "licenses"), ("loader-license", "licenses")):
        f = fetch(name, cache, PLATFORM)
        shutil.copy2(f, rel / folder / f.name)


def macos_icon(dst):
    """The toolkit's icon as an .icns, from its PNGs."""
    iconset = dst.parent / "icon.iconset"
    iconset.mkdir()
    png = ROOT / "assets/icon/png"
    for size in (16, 32, 128, 256, 512):
        shutil.copy2(png / f"{size}.png", iconset / f"icon_{size}x{size}.png")
        if (png / f"{size * 2}.png").is_file():
            shutil.copy2(png / f"{size * 2}.png", iconset / f"icon_{size}x{size}@2x.png")
    run(["iconutil", "-c", "icns", iconset, "-o", dst])
    shutil.rmtree(iconset)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recipe", help="config/releases/<title>.json")
    ap.add_argument("--out", required=True, help="the release folder (replaced)")
    ap.add_argument("--work", help="builds kept between runs (default <out>/../release-work)")
    ap.add_argument("--cache", help="downloads (default <out>/../downloads)")
    ap.add_argument("--version", help="this release's version (default: date and commit)")
    ap.add_argument("--archive", action="store_true",
                    help=f"also write <name>-<version>-{PLATFORM}.tar.xz (.zip on Windows) "
                         "beside --out")
    ap.add_argument("--vulkan-sdk", help="macOS: the Vulkan SDK to take the loader, "
                    "MoltenVK and DXC from (default VULKAN_SDK)")
    args = ap.parse_args()

    recipe_path = Path(args.recipe).resolve()
    recipe = json.loads(recipe_path.read_text())
    # Relative to the recipe, so a title's own repository can carry its recipe
    # and say "..".
    project = (recipe_path.parent / recipe["project"]).resolve()
    images = recipe.get("images") or [{"sha1": h} for h in recipe.get("xbe_sha1", [])]
    if not images:
        sys.exit(f"make_release: {recipe_path} names no default.xbe images")
    for img in images:
        if img.get("config") and not (project / img["config"]).is_dir():
            sys.exit(f"make_release: {img['config']} (for {img['sha1']}) is not in {project}")
    sdk = vulkan_sdk(args.vulkan_sdk) if MACOS else None
    out = Path(args.out).resolve()
    work = Path(args.work).resolve() if args.work else out.parent / "release-work"
    cache = Path(args.cache).resolve() if args.cache else out.parent / "downloads"
    # Its own output, finished or not, is marked, so a folder it did not make
    # is never deleted.
    marker = ".made-by-make_release"
    if out.exists():
        if not (out / marker).is_file():
            sys.exit(f"make_release: {out} exists and is not a release; not replacing it")
        shutil.rmtree(out)
    out.mkdir(parents=True)
    (out / marker).write_text("tools/release/make_release.py\n")
    # Where release.json and everything beside it goes: the folder itself, or
    # on a Mac the application's Resources.
    # The application's file name: no colon, which Finder shows as a slash
    # ("Turok: Evolution" -> "Turok Evolution.app").
    app_name = recipe["title_name"].replace(":", "")
    app = out / f"{app_name}.app" if MACOS else None
    rel = app / "Contents/Resources" if MACOS else out
    rel.mkdir(parents=True, exist_ok=True)
    # Zig's cache is shared; the builds are per release, because CMake keeps
    # a build folder on the compiler it was first configured with, and each
    # release's compiler is its own.
    zig_cache = work / "zig-cache"
    work = work / recipe["release_name"]
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(zig_cache),
               ZIG_LOCAL_CACHE_DIR=str(zig_cache))
    # zig is given its macOS version by its target (fetch_toolchain.py), and
    # CMake would add a second from this.
    env.pop("MACOSX_DEPLOYMENT_TARGET", None)

    if args.version:
        version = args.version
    else:
        sha = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True).stdout.strip() or "unknown"
        version = time.strftime("%Y%m%d") + "-" + sha
    log(f"{recipe['title_name']} {version} ({PLATFORM}) -> {out}")

    # The compiler, CMake, Ninja and Python everything below is built with.
    run([sys.executable, "-m", "tools.release.fetch_toolchain", "--out", rel,
         "--cache", cache], cwd=ROOT)
    tc = rel / "toolchain"
    cmake = tc / f"cmake/bin/cmake{EXE}"

    # The engine, built with that compiler, then packaged. On a Mac, SDL first,
    # apart (build_sdl_macos), and the engine pointed at it.
    eng = work / "engine"
    sdl_args = []
    if MACOS:
        sdl = build_sdl_macos(work, tc, env)
        sdl_args = ["-DXBOXRECOMP_EXTERNAL_SDL=ON", f"-DSDL3_DIR={sdl}"]
    run([cmake, "-S", ROOT, "-B", eng, *toolchain_args(tc), *HOST_FLAGS, *sdl_args], env=env)
    run([cmake, "--build", eng, "--target", "xboxrecomp_engine"], env=env)
    run([sys.executable, "-m", "tools.release.make_engine_package", "--build", eng,
         "--out", rel / "engine"], cwd=ROOT)

    # XbSymbolDatabase's tool, which names the XDK functions in a player's XBE.
    xbsdb = work / "xbsdb"
    run([cmake, "-S", ROOT / "third_party/XbSymbolDatabase", "-B", xbsdb,
         *toolchain_args(tc)], env=env)
    run([cmake, "--build", xbsdb, "--target", "XbSymbolDatabaseCLI"], env=env)
    (rel / "bin").mkdir()
    shutil.copy2(xbsdb / f"projects/cli/XbSymbolDatabaseCLI{EXE}", rel / "bin")

    # The builder: linked by zig for glibc 2.38 like everything else on
    # Linux; by Xcode's linker on a Mac, for the same macOS as the rest; on
    # Windows by Visual Studio's, with the C runtime linked in, so it needs no
    # redistributable.
    cargo_env = dict(env, CARGO_TARGET_DIR=str(work / "cargo"))
    if MACOS:
        cargo_env["MACOSX_DEPLOYMENT_TARGET"] = MACOS_MIN
    elif WINDOWS:
        cargo_env["RUSTFLAGS"] = (cargo_env.get("RUSTFLAGS", "") + " -C target-feature=+crt-static").strip()
    else:
        cargo_env["CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER"] = str(tc / "bin/zcc")
    run(["cargo", "build", "--release", "--locked"], cwd=ROOT / "tools/builder", env=cargo_env)
    shutil.copy2(work / f"cargo/release/xboxrecomp-build{EXE}", rel / "bin")

    # The pipeline, as much of the toolkit as lifting a title reads. Only
    # what git tracks: a desk checkout also holds pipeline output, regression
    # frames and the like, made from someone's game.
    tk = rel / "toolkit"
    for f in tracked_files(ROOT, ["tools", *TOOLKIT_DIRS, *TOOLKIT_FILES]):
        parts = Path(f).parts
        if (parts[0] == "tools" and len(parts) > 1 and parts[1] in TOOLS_SKIP) \
                or "__pycache__" in parts or Path(f).name.startswith("test_") \
                or f.startswith("config/releases/"):
            continue
        (tk / f).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / f, tk / f)

    # The title's project, without generated code or build output.
    tracked = tracked_files(project, ["."])
    lift = recipe.get("lift")
    wanted = set(recipe.get("include", [])) | ({lift[0]} if lift else set())
    for rel_path in tracked:
        # What a title's build reads: its CMakeLists, its sources and its own
        # config (seeds, XDK symbols), and what the recipe adds (its own lift
        # script). Not other scripts, a submodule, or lifted code.
        if not (rel_path == "CMakeLists.txt" or rel_path.startswith(("src/", "config/"))
                or rel_path in wanted
                or any(rel_path.startswith(w.rstrip("/") + "/") for w in wanted)):
            continue
        if "/recomp/gen/" in rel_path:
            continue
        (rel / "recipe" / rel_path).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(project / rel_path, rel / "recipe" / rel_path)

    # The launcher: the title's own target. The project needs a lifted source
    # to configure; the launcher uses none.
    lproj = work / "launcher-project"
    shutil.rmtree(lproj, ignore_errors=True)
    shutil.copytree(rel / "recipe", lproj)
    (lproj / "src/recomp/gen").mkdir(parents=True, exist_ok=True)
    (lproj / "src/recomp/gen/placeholder.c").write_text(
        "/* make_release.py: the launcher needs no lifted code. */\n"
        "int xboxrecomp_release_placeholder;\n")
    # Anything else the title's CMakeLists insists on before it configures,
    # which its lift writes (split2-recomp: the region header).
    for rp in recipe.get("placeholders", []):
        (lproj / rp).parent.mkdir(parents=True, exist_ok=True)
        (lproj / rp).write_text("/* make_release.py: a placeholder for the launcher's build. */\n")
    lbuild = work / "launcher"
    if MACOS:
        # With Xcode's compiler, from the toolkit's sources: it draws through
        # Core Text and Core Animation, which need the SDK. It links only C
        # (settings, pads, addresses) and its own SDL, statically.
        launcher_args = ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={tc / 'ninja/bin/ninja'}",
                         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang",
                         "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_OBJC_COMPILER=clang",
                         f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_MIN}",
                         "-DCMAKE_OSX_ARCHITECTURES=arm64", f"-DXBOXRECOMP_DIR={ROOT}"]
    else:
        # Against the engine above.
        launcher_args = [*toolchain_args(tc), *HOST_FLAGS, f"-DXBOXRECOMP_DIR={cm(ROOT)}",
                         f"-DXBOXRECOMP_PREBUILT={cm(eng)}"]
    run([cmake, "-S", lproj, "-B", lbuild, *launcher_args], env=env)
    run([cmake, "--build", lbuild, "--target", recipe["launcher"]], env=env)
    if MACOS:
        (app / "Contents/MacOS").mkdir(parents=True)
        shutil.copy2(lbuild / recipe["launcher"], app / "Contents/MacOS" / recipe["release_name"])
    else:
        shutil.copy2(lbuild / f"{recipe['launcher']}{EXE}", out / f"{recipe['release_name']}{EXE}")

    # The shader compiler the renderer loads, with its licences. On a Mac,
    # with the Vulkan loader and MoltenVK in runtime/. Windows draws with
    # Direct3D 11 and the system's own D3DCompiler_47, so carries none.
    if not WINDOWS:
        (rel / "dxc").mkdir()
    if MACOS:
        macos_runtime(rel, sdk, sdl, cache)
    for f in ("lib/libdxcompiler.so", "lib/libdxil.so", "LICENSE-MS.txt", "LICENSE-LLVM.txt"):
        src = ROOT / "third_party/dxc" / f
        if src.is_file():
            shutil.copy2(src, rel / "dxc" / Path(f).name)

    release = {
        "title_name": recipe["title_name"],
        "title_id": recipe["title_id"],
        "version": version,
        "exe": recipe["exe"],
        "images": images,
    }
    if lift:
        release["lift"] = lift
    # The XBE the game boots, when not default.xbe (a language launcher's
    # disc); the builder checks and lifts that one.
    if recipe.get("xbe"):
        release["xbe"] = recipe["xbe"]
    (rel / "release.json").write_text(json.dumps(release, indent=2) + "\n")
    shutil.copy2(ROOT / "LICENSE", rel / "LICENSE")
    notice = NOTICE.format(
        title=recipe["title_name"],
        os_name="macOS" if MACOS else "Windows" if WINDOWS else "Linux",
        dxc_licenses="dxc/LICENSE-DXC.txt, dxc/ThirdPartyNotices-DXC.txt" if MACOS
        else "dxc/LICENSE-MS.txt, dxc/LICENSE-LLVM.txt",
        extra=NOTICE_MACOS if MACOS else "")
    if WINDOWS:
        notice = "".join(l for l in notice.splitlines(True) if "DirectX Shader Compiler" not in l)
        notice = notice.replace("musl and glibc headers and stubs", "MinGW-w64 headers and libraries")
    (rel / "NOTICE.txt").write_text(notice)
    if WINDOWS:
        userdir = f"%LOCALAPPDATA%\\xboxrecomp\\builds\\{recipe['title_id']}"
        start = README_WINDOWS_START.format(name=recipe["release_name"])
        shortcut = README_WINDOWS_SHORTCUT
    elif MACOS:
        userdir = f"~/Library/Application Support/xboxrecomp/builds/{recipe['title_id']}"
        start = README_MACOS_START.format(app=app.name)
        shortcut = README_MACOS_SHORTCUT.format(macos_min=MACOS_MIN)
    else:
        userdir = f"~/.local/share/xboxrecomp/builds/{recipe['title_id']}"
        start = README_LINUX_START.format(name=recipe["release_name"])
        shortcut = README_LINUX_SHORTCUT
    readme = README.format(
        title=recipe["title_name"], line="=" * len(recipe["title_name"]),
        start=start, shortcut=shortcut, userdir=userdir,
        hashes="".join(f"  {i['sha1']}  {i.get('name', '')}\n" for i in images))
    (rel / "README.txt").write_text(readme)
    # The toolkit's icon, for a desktop entry or a Steam shortcut; on a Mac,
    # the application's.
    shutil.copy2(ROOT / "assets/icon/png/256.png", rel / "icon.png")
    if MACOS:
        macos_icon(rel / "icon.icns")
        with open(app / "Contents/Info.plist", "wb") as f:
            plistlib.dump({
                "CFBundleDevelopmentRegion": "en",
                "CFBundleExecutable": recipe["release_name"],
                "CFBundleIdentifier": f"org.xboxrecomp.release.{recipe['release_name']}",
                "CFBundleInfoDictionaryVersion": "6.0",
                "CFBundleName": recipe["title_name"][:15],
                "CFBundleDisplayName": recipe["title_name"],
                "CFBundleIconFile": "icon.icns",
                "CFBundlePackageType": "APPL",
                "CFBundleShortVersionString": version,
                "CFBundleVersion": version,
                "LSMinimumSystemVersion": MACOS_MIN,
                "LSApplicationCategoryType": "public.app-category.games",
                "NSHighResolutionCapable": True,
                "NSSupportsAutomaticGraphicsSwitching": True,
            }, f)
        # Beside the application in the archive, where a player sees it first.
        (out / "README.txt").write_text(readme)
        # An ad-hoc signature over the whole bundle, last, once nothing in it
        # changes. The linker signs each executable, but alone that leaves
        # the bundle with no Info.plist bound and no resources sealed, which
        # a downloaded copy's Gatekeeper check reads as a broken signature:
        # "is damaged and can't be opened", with no way to open it. Sealed,
        # it is merely unidentified, which Open Anyway (or the README's
        # xattr) gets past. Nothing writes inside the bundle afterwards: the
        # builder works in the player's folder, and Python writes no .pyc.
        run(["codesign", "--force", "--deep", "--sign", "-", app])
        run(["codesign", "--verify", "--deep", "--strict", app])

    size = sum(f.lstat().st_size for f in out.rglob("*") if f.is_file() and not f.is_symlink())
    log(f"done: {out} ({size / 1e6:.0f} MB)")

    # One file to publish: <name>-<version>-<platform>.tar.xz beside the
    # folder, unpacking as <name>/; on Windows a .zip, which Explorer opens.
    if args.archive and WINDOWS:
        import zipfile
        archive = out.parent / f"{recipe['release_name']}-{version}-{PLATFORM}.zip"
        log(f"archiving to {archive}")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in sorted(out.rglob("*")):
                if f.is_file() and f.name != marker:
                    z.write(f, Path(recipe["release_name"]) / f.relative_to(out))
        log(f"archive: {archive} ({archive.stat().st_size / 1e6:.0f} MB)")
    elif args.archive:
        archive = out.parent / f"{recipe['release_name']}-{version}-{PLATFORM}.tar.xz"
        log(f"archiving to {archive}")
        with tarfile.open(archive, "w:xz", preset=6) as tar:
            tar.add(out, arcname=recipe["release_name"],
                    filter=lambda ti: None if ti.name.endswith(marker) else ti)
        log(f"archive: {archive} ({archive.stat().st_size / 1e6:.0f} MB)")


if __name__ == "__main__":
    main()
