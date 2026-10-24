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
    LICENSE NOTICE.txt README.txt

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
has. Nothing here starts a title.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# What the toolkit's pipeline needs at a player's machine, beyond tools/.
TOOLKIT_FILES = ["scripts/recompile.py", "LICENSE"]
TOOLKIT_DIRS = ["config", "templates/runtime", "src/hle"]
# Not needed to lift a title: the Rust builder's sources, platform scripts.
TOOLS_SKIP = {"builder", "linux", "macos", "vk_spike", "release", "__pycache__"}

# The build machine's headers for what SDL loads at run time, after zig's own
# libc headers, and its libraries for the names SDL records.
HOST_FLAGS = ["-DCMAKE_C_FLAGS=-idirafter /usr/include",
              "-DCMAKE_CXX_FLAGS=-idirafter /usr/include",
              "-DCMAKE_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu"]

NOTICE = """\
{title} for Linux, a self-building release of the xboxrecomp toolkit.

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
  The DirectX Shader Compiler (dxc/LICENSE-MS.txt, dxc/LICENSE-LLVM.txt)
"""

README = """\
{title}
{line}

Start ./{name}. The first time, it asks for your copy of the game: the disc
image (.iso or .xiso) or the folder its files are in. It then builds the game
from it, which takes a minute or two, and opens the settings; press Play.

What it builds is kept in {userdir}: the game's files, the build, and the
log (build.log) to attach to a bug report. An image is extracted there once;
a folder is linked, never copied or changed.

This release works from these images of the game's default.xbe (SHA-1):
{hashes}
Any other is refused, because it has not been tested.
"""


def log(msg):
    print(f"[make_release] {msg}", flush=True)


def run(cmd, **kw):
    log("$ " + " ".join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode:
        sys.exit(f"make_release: failed ({r.returncode}): {cmd[0]}")


def copy_tree(src, dst, skip=()):
    shutil.copytree(src, dst, dirs_exist_ok=True, symlinks=False,
                    ignore=shutil.ignore_patterns("__pycache__", "test_*.py", "*.pyc", *skip))


def toolchain_args(tc):
    return [f"-DCMAKE_MAKE_PROGRAM={tc / 'ninja/bin/ninja'}",
            f"-DCMAKE_C_COMPILER={tc / 'bin/zcc'}",
            f"-DCMAKE_CXX_COMPILER={tc / 'bin/zc++'}",
            f"-DCMAKE_AR={tc / 'bin/zar'}", f"-DCMAKE_RANLIB={tc / 'bin/zranlib'}",
            "-DCMAKE_BUILD_TYPE=Release", "-G", "Ninja"]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recipe", help="config/releases/<title>.json")
    ap.add_argument("--out", required=True, help="the release folder (replaced)")
    ap.add_argument("--work", help="builds kept between runs (default <out>/../release-work)")
    ap.add_argument("--cache", help="downloads (default <out>/../downloads)")
    ap.add_argument("--version", help="this release's version (default: date and commit)")
    ap.add_argument("--archive", action="store_true",
                    help="also write <name>-<version>-linux-x86_64.tar.xz beside --out")
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
    # Zig's cache is shared; the builds are per release, because CMake keeps
    # a build folder on the compiler it was first configured with, and each
    # release's compiler is its own.
    zig_cache = work / "zig-cache"
    work = work / recipe["release_name"]
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(zig_cache),
               ZIG_LOCAL_CACHE_DIR=str(zig_cache))

    if args.version:
        version = args.version
    else:
        sha = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True).stdout.strip() or "unknown"
        version = time.strftime("%Y%m%d") + "-" + sha
    log(f"{recipe['title_name']} {version} -> {out}")

    # The compiler, CMake, Ninja and Python everything below is built with.
    run([sys.executable, "-m", "tools.release.fetch_toolchain", "--out", out,
         "--cache", cache], cwd=ROOT)
    tc = out / "toolchain"
    cmake = tc / "cmake/bin/cmake"

    # The engine, built with that compiler, then packaged.
    eng = work / "engine"
    run([cmake, "-S", ROOT, "-B", eng, *toolchain_args(tc), *HOST_FLAGS], env=env)
    run([cmake, "--build", eng, "--target", "xboxrecomp_engine"], env=env)
    run([sys.executable, "-m", "tools.release.make_engine_package", "--build", eng,
         "--out", out / "engine"], cwd=ROOT)

    # XbSymbolDatabase's tool, which names the XDK functions in a player's XBE.
    xbsdb = work / "xbsdb"
    run([cmake, "-S", ROOT / "third_party/XbSymbolDatabase", "-B", xbsdb,
         *toolchain_args(tc)], env=env)
    run([cmake, "--build", xbsdb, "--target", "XbSymbolDatabaseCLI"], env=env)
    (out / "bin").mkdir()
    shutil.copy2(xbsdb / "projects/cli/XbSymbolDatabaseCLI", out / "bin")

    # The builder, linked by zig for glibc 2.38 like everything else.
    cargo_env = dict(env, CARGO_TARGET_DIR=str(work / "cargo"),
                     CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=str(tc / "bin/zcc"))
    run(["cargo", "build", "--release", "--locked"], cwd=ROOT / "tools/builder", env=cargo_env)
    shutil.copy2(work / "cargo/release/xboxrecomp-build", out / "bin")

    # The pipeline, as much of the toolkit as lifting a title reads.
    tk = out / "toolkit"
    copy_tree(ROOT / "tools", tk / "tools", skip=TOOLS_SKIP)
    for d in TOOLKIT_DIRS:
        copy_tree(ROOT / d, tk / d, skip=("releases",))
    for f in TOOLKIT_FILES:
        (tk / f).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / f, tk / f)

    # The title's project, without generated code or build output.
    tracked = subprocess.run(["git", "-C", str(project), "ls-files", "-z", "."],
                             capture_output=True, text=True).stdout.split("\0")
    lift = recipe.get("lift")
    wanted = set(recipe.get("include", [])) | ({lift[0]} if lift else set())
    for rel in filter(None, tracked):
        # What a title's build reads: its CMakeLists, its sources and its own
        # config (seeds, XDK symbols), and what the recipe adds (its own lift
        # script). Not other scripts, a submodule, or lifted code.
        if not (rel == "CMakeLists.txt" or rel.startswith(("src/", "config/"))
                or rel in wanted or any(rel.startswith(w.rstrip("/") + "/") for w in wanted)):
            continue
        if "/recomp/gen/" in rel or not (project / rel).is_file():
            continue
        (out / "recipe" / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(project / rel, out / "recipe" / rel)

    # The launcher: the title's own target, built against the engine above.
    # The project needs a lifted source to configure; the launcher uses none.
    lproj = work / "launcher-project"
    shutil.rmtree(lproj, ignore_errors=True)
    shutil.copytree(out / "recipe", lproj)
    (lproj / "src/recomp/gen").mkdir(parents=True, exist_ok=True)
    (lproj / "src/recomp/gen/placeholder.c").write_text(
        "/* make_release.py: the launcher needs no lifted code. */\n"
        "int xboxrecomp_release_placeholder;\n")
    # Anything else the title's CMakeLists insists on before it configures,
    # which its lift writes (split2-recomp: the region header).
    for rel in recipe.get("placeholders", []):
        (lproj / rel).parent.mkdir(parents=True, exist_ok=True)
        (lproj / rel).write_text("/* make_release.py: a placeholder for the launcher's build. */\n")
    lbuild = work / "launcher"
    run([cmake, "-S", lproj, "-B", lbuild, *toolchain_args(tc), *HOST_FLAGS,
         f"-DXBOXRECOMP_DIR={ROOT}", f"-DXBOXRECOMP_PREBUILT={eng}"], env=env)
    run([cmake, "--build", lbuild, "--target", recipe["launcher"]], env=env)
    shutil.copy2(lbuild / recipe["launcher"], out / recipe["release_name"])

    # The shader compiler the renderer loads, with its licences.
    (out / "dxc").mkdir()
    for f in ("lib/libdxcompiler.so", "lib/libdxil.so", "LICENSE-MS.txt", "LICENSE-LLVM.txt"):
        src = ROOT / "third_party/dxc" / f
        if src.is_file():
            shutil.copy2(src, out / "dxc" / Path(f).name)

    release = {
        "title_name": recipe["title_name"],
        "title_id": recipe["title_id"],
        "version": version,
        "exe": recipe["exe"],
        "images": images,
    }
    if lift:
        release["lift"] = lift
    (out / "release.json").write_text(json.dumps(release, indent=2) + "\n")
    shutil.copy2(ROOT / "LICENSE", out / "LICENSE")
    (out / "NOTICE.txt").write_text(NOTICE.format(title=recipe["title_name"]))
    (out / "README.txt").write_text(README.format(
        title=recipe["title_name"], line="=" * len(recipe["title_name"]),
        name=recipe["release_name"],
        userdir=f"~/.local/share/xboxrecomp/builds/{recipe['title_id']}",
        hashes="".join(f"  {i['sha1']}  {i.get('name', '')}\n" for i in images)))

    size = sum(f.lstat().st_size for f in out.rglob("*") if f.is_file() and not f.is_symlink())
    log(f"done: {out} ({size / 1e6:.0f} MB)")

    # One file to publish: <name>-<version>-linux-x86_64.tar.xz beside the
    # folder, unpacking as <name>/.
    if args.archive:
        archive = out.parent / f"{recipe['release_name']}-{version}-linux-x86_64.tar.xz"
        log(f"archiving to {archive}")
        with tarfile.open(archive, "w:xz", preset=6) as tar:
            tar.add(out, arcname=recipe["release_name"],
                    filter=lambda ti: None if ti.name.endswith(marker) else ti)
        log(f"archive: {archive} ({archive.stat().st_size / 1e6:.0f} MB)")


if __name__ == "__main__":
    main()
