"""Turn a prebuilt engine build into a package a release can carry.

    python3 -m tools.release.make_engine_package --build <engine build> --out <dir>

A toolkit built with --target xboxrecomp_engine exports its libraries
(cmake/prebuilt_engine.cmake), but the export names files by absolute path:
the libraries in the build folder and the headers in the source tree. This
copies what the export and SDL's own package refer to -- the libraries, and
the header folders as headers only -- into one folder, rewrites every path
relative to it, and adds the CMakeLists a title's add_subdirectory finds
there (tools/release/engine_package). A title pointed at the result with
XBOXRECOMP_DIR builds against the engine with no toolkit checkout, which is
how a self-building release compiles a player's game.

The package is checked before it is written: a path still naming the build
or the source tree is an error, because it would work here and nowhere else.
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
HEADER_SUFFIXES = {".h", ".hpp", ".hh", ".inc", ".inl"}
PATH_RE = re.compile(r'/[^"\s;)>]+')


def fail(msg):
    print(f"make_engine_package: {msg}", file=sys.stderr)
    sys.exit(1)


def copy_headers(src_dir, dst_dir):
    """Every header under src_dir, at the same place under dst_dir."""
    n = 0
    for f in src_dir.rglob("*"):
        if f.is_file() and f.suffix in HEADER_SUFFIXES:
            out = dst_dir / f.relative_to(src_dir)
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            n += 1
    return n


def place(path, build, source, out):
    """Where a path from the build or the source tree lives in the package."""
    p = Path(path).resolve()
    if p == build or build in p.parents:
        return out / "build" / p.relative_to(build)
    if p == source or source in p.parents:
        return out / p.relative_to(source)
    return None


def carry(text, build, source, out, stats):
    """Copy everything `text` names under build or source into the package."""
    for raw in set(PATH_RE.findall(text)):
        dst = place(raw, build, source, out)
        if dst is None:
            continue
        src = Path(raw).resolve()
        if src.is_dir():
            stats["headers"] += copy_headers(src, dst)
        elif src.is_file():
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            stats["files"] += 1


def rewrite(text, build, source, up):
    """Absolute build and source paths -> the package, from a file `up`
    levels below its root. The build folder first: it may sit in the source."""
    prefix = ('get_filename_component(_xr_pkg "${CMAKE_CURRENT_LIST_DIR}'
              + "/.." * up + '" ABSOLUTE)\n')
    for root, repl in sorted(((str(build), "${_xr_pkg}/build"),
                              (str(source), "${_xr_pkg}")),
                             key=lambda t: -len(t[0])):
        text = text.replace(root, repl)
    return prefix + text


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True, help="a toolkit build made with "
                    "--target xboxrecomp_engine")
    ap.add_argument("--out", required=True, help="the package folder to write "
                    "(replaced if it exists)")
    args = ap.parse_args()

    build = Path(args.build).resolve()
    out = Path(args.out).resolve()
    info = build / "xboxrecomp-engine.cmake"
    targets = build / "xboxrecomp-engine-targets.cmake"
    cache = build / "CMakeCache.txt"
    for f in (info, targets, cache):
        if not f.is_file():
            fail(f"{build} is not an engine build: no {f.name}")
    m = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", cache.read_text(), re.M)
    if not m:
        fail(f"{cache} names no source directory")
    source = Path(m.group(1)).resolve()

    if out.exists():
        if not (out / "xboxrecomp-engine.cmake").is_file():
            fail(f"{out} exists and is not an engine package; not replacing it")
        shutil.rmtree(out)
    out.mkdir(parents=True)
    stats = {"files": 0, "headers": 0}

    # The engine's libraries and the header folders they publish.
    text = targets.read_text()
    carry(text, build, source, out, stats)
    for lib in re.findall(r'IMPORTED_LOCATION[A-Z_]*\s+"([^"]+)"', text):
        if not Path(lib).is_file():
            fail(f"{lib} is not built: run --target xboxrecomp_engine first")
    (out / targets.name).write_text(rewrite(text, build, source, 0))

    # SDL's own package, which the import finds with find_package.
    info_text = info.read_text()
    sdl_m = re.search(r'XBOXRECOMP_ENGINE_SDL3_DIR "([^"]*)"', info_text)
    sdl_rel = ""
    if sdl_m and sdl_m.group(1):
        sdl = Path(sdl_m.group(1)).resolve()
        sdl_out = place(sdl, build, source, out)
        sdl_out.mkdir(parents=True, exist_ok=True)
        up = len(sdl_out.relative_to(out).parts)
        for f in sorted(sdl.glob("SDL3*.cmake")):
            t = f.read_text()
            carry(t, build, source, out, stats)
            (sdl_out / f.name).write_text(rewrite(t, build, source, up))
        sdl_rel = sdl_out.relative_to(out).as_posix()

    # What the import compares, with the package as its own source.
    info_text = re.sub(r'(XBOXRECOMP_ENGINE_SOURCE_DIR) "[^"]*"', r'\1 "PACKAGE"', info_text)
    info_text = re.sub(r'(XBOXRECOMP_ENGINE_SDL3_DIR) "[^"]*"', rf'\1 "{sdl_rel}"', info_text)
    (out / info.name).write_text(info_text)

    # What the package's CMakeLists uses: the import, the lifted-code flags,
    # xbox_config's source and the toolkit's public headers.
    (out / "cmake").mkdir(exist_ok=True)
    for f in ("prebuilt_engine.cmake", "lifted_code.cmake", "title_icon.cmake"):
        shutil.copy2(source / "cmake" / f, out / "cmake" / f)
    # The icon title_icon.cmake names (the SDL windows' copy is a header).
    (out / "assets" / "icon").mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "assets" / "icon" / "xboxrecomp.ico", out / "assets" / "icon")
    (out / "src" / "config").mkdir(parents=True, exist_ok=True)
    for f in (source / "src" / "config").iterdir():
        if f.is_file() and (f.suffix in HEADER_SUFFIXES | {".c"} or f.name == "CMakeLists.txt"):
            shutil.copy2(f, out / "src" / "config" / f.name)
    if (source / "include").is_dir():
        stats["headers"] += copy_headers(source / "include", out / "include")
    # The runtime headers lifted code is written against, which
    # xbox_memory_layout.h includes from the source tree's templates/runtime.
    stats["headers"] += copy_headers(source / "templates" / "runtime",
                                     out / "templates" / "runtime")
    shutil.copy2(ROOT / "tools" / "release" / "engine_package" / "CMakeLists.txt",
                 out / "CMakeLists.txt")

    # Nothing may still point at the machine this was made on.
    for f in out.rglob("*.cmake"):
        t = f.read_text()
        for root in (str(build), str(source)):
            if root in t:
                fail(f"{f.relative_to(out)} still names {root}")

    size = sum(f.stat().st_size for f in out.rglob("*") if f.is_file())
    print(f"engine package: {out} ({stats['files']} libraries and files, "
          f"{stats['headers']} headers, {size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
