#!/usr/bin/env python3
"""make_kit.py - what an APK needs to build its game on the device.

An APK carries no game code; the device makes it (device_build.py). This
writes the rest, as one zip the APK keeps in its assets and the app unpacks
into its internal folder:

  python/              python.org's CPython for Android: the standard
                       library (tests and GUI left out) and Capstone's
                       Python half (its library is one of the APK's)
  toolkit/             the pipeline, the same files a desktop release takes
                       (tools/release/make_release.py), and device_build.py
  recipe/              the title's project as a release takes it: its
                       sources, config and lift script, no lifted code
  headers/             the headers the title's own sources include
  sysroot/             the NDK's headers and start files for API 30, which
                       zig compiles and links against
  zig/lib/             zig's own library, without the C libraries of other
                       platforms
  android-build.json   the release (images, lift) and the compile and link
                       flags, taken from the APK's own build of the title
                       (its compile_commands.json), with {kit} and
                       {project} for where they end up on the device

    python3 make_kit.py --build <build/android> --project <title repo>
        --recipe <release/x.json> --python <prefix> --zig-lib <zig>/lib
        --capstone <site-packages/capstone> --ndk <ndk> --out kit.zip
"""
import argparse
import hashlib
import json
import os
import shlex
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLKIT = HERE.parent.parent
sys.path.insert(0, str(TOOLKIT / "tools" / "release"))
import make_release  # noqa: E402  (its file selection, kept in one place)

HEADER_EXT = (".h", ".hpp", ".inc", ".def")
PYTHON_SKIP = {"test", "idlelib", "tkinter", "turtledemo", "ensurepip", "__pycache__"}
ZIG_KEEP = {"c", "compiler", "compiler_rt", "include", "std", "zig.h", "c.zig",
            "compiler_rt.zig", "ubsan_rt.zig", "fuzzer.zig"}
API = "30"


class Kit:
    def __init__(self, path):
        self.z = zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=6)
        self.names = set()
        self.digest = hashlib.sha1()

    def add(self, src, arc):
        arc = arc.replace(os.sep, "/")
        if arc in self.names:
            return
        self.names.add(arc)
        self.z.write(src, arc)
        st = os.stat(src)
        self.digest.update(f"{arc}:{st.st_size}:{int(st.st_mtime)}".encode())

    def add_text(self, arc, text):
        self.names.add(arc)
        self.z.writestr(arc, text)
        self.digest.update(text.encode())

    def add_tree(self, root, arc_root, skip=lambda rel: False):
        root = Path(root)
        for dirpath, dirs, files in os.walk(root):
            rel_dir = Path(dirpath).relative_to(root)
            dirs[:] = [d for d in dirs if not skip(rel_dir / d)]
            for f in files:
                rel = rel_dir / f
                if not skip(rel):
                    self.add(Path(dirpath) / f, f"{arc_root}/{rel.as_posix()}")


def compile_spec(build, project, kit_headers):
    """Flags for the title's own sources and for lifted code, from the
    build's compile_commands.json, and the header folders they name."""
    cc = json.load(open(build / "compile_commands.json"))
    project = project.resolve()
    gen_dir = project / "src" / "recomp" / "gen"
    title, gen_flags = [], None
    for entry in cc:
        f = Path(entry["file"]).resolve()
        try:
            rel = f.relative_to(project)
        except ValueError:
            continue
        if rel.parts[0] in ("xboxrecomp", "build"):
            continue
        args = shlex.split(entry["command"])[1:]
        flags, i = [], 0
        while i < len(args):
            a = args[i]
            if a in ("-o", "-MT", "-MF"):
                i += 2
                continue
            if a in ("-c", "-MD", "-g", "-no-canonical-prefixes") or a.startswith(
                    ("--target=", "--sysroot=", "--gcc-toolchain=")) or Path(a).resolve() == f:
                i += 1
                continue
            if a == "-I" or a.startswith("-I"):
                d = args[i + 1] if a == "-I" else a[2:]
                i += 2 if a == "-I" else 1
                flags.append("-I" + map_include(Path(d).resolve(), project, build, kit_headers))
                continue
            flags.append(a)
            i += 1
        if gen_dir in f.parents:
            gen_flags = gen_flags or flags
        else:
            title.append({"src": rel.as_posix(), "flags": flags})
    if gen_flags is None or not title:
        sys.exit("make_kit: no lifted or title sources in compile_commands.json")
    return title, gen_flags


def map_include(d, project, build, kit_headers):
    """Where an include folder is on the device: in the title's project, or
    among the headers the kit carries."""
    for root, name in ((build, "build"), (TOOLKIT, "toolkit")):
        try:
            rel = d.relative_to(root)
        except ValueError:
            continue
        kit_headers.add((d, f"headers/{name}/{rel.as_posix()}"))
        return "{kit}/headers/" + f"{name}/{rel.as_posix()}".rstrip("/.")
    try:
        return "{project}/" + d.relative_to(project).as_posix()
    except ValueError:
        sys.exit(f"make_kit: an include folder outside the project and toolkit: {d}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", required=True)
    ap.add_argument("--project", required=True)
    ap.add_argument("--spec-project", help="the configured copy of the project the build "
                    "was made from (build_apk.py), where its paths point; default --project")
    ap.add_argument("--recipe", required=True)
    ap.add_argument("--python", required=True, help="the CPython for Android prefix")
    ap.add_argument("--zig-lib", required=True)
    ap.add_argument("--capstone", required=True, help="capstone's Python package folder")
    ap.add_argument("--ndk", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    build, project = Path(a.build).resolve(), Path(a.project).resolve()
    recipe = json.loads(Path(a.recipe).read_text())
    kit = Kit(a.out + ".partial")

    # Python, without what no build reads.
    pylib = Path(a.python) / "lib" / "python3.14"
    kit.add_tree(pylib, "python/lib/python3.14",
                 skip=lambda rel: bool(set(rel.parts) & PYTHON_SKIP))
    kit.add_tree(a.capstone, "python/lib/python3.14/site-packages/capstone",
                 skip=lambda rel: rel.parts[0] == "lib" or "__pycache__" in rel.parts)

    # The pipeline, as a desktop release takes it.
    for f in make_release.tracked_files(TOOLKIT, ["tools", *make_release.TOOLKIT_DIRS,
                                                  *make_release.TOOLKIT_FILES]):
        parts = Path(f).parts
        if (parts[0] == "tools" and len(parts) > 1 and parts[1] in make_release.TOOLS_SKIP) \
                or "__pycache__" in parts or Path(f).name.startswith("test_") \
                or f.startswith("config/releases/"):
            continue
        kit.add(TOOLKIT / f, f"toolkit/{f}")
    kit.add(HERE / "device_build.py", "toolkit/tools/android/device_build.py")

    # The title's project, as a desktop release takes it.
    lift = recipe.get("lift")
    wanted = set(recipe.get("include", [])) | ({lift[0]} if lift else set())
    for rel in make_release.tracked_files(project, ["."]):
        if not (rel == "CMakeLists.txt" or rel.startswith(("src/", "config/")) or rel in wanted
                or any(rel.startswith(w.rstrip("/") + "/") for w in wanted)):
            continue
        if "/recomp/gen/" in rel:
            continue
        kit.add(project / rel, f"recipe/{rel}")

    # The flags, and the headers they reach.
    headers = set()
    spec_project = Path(a.spec_project).resolve() if a.spec_project else project
    title, gen_flags = compile_spec(build, spec_project, headers)
    # Engine headers also reach the runtime templates by relative path
    # (src/kernel/xbox_memory_layout.h: ../../templates/runtime/recomp_cpu.h).
    headers.add((TOOLKIT / "templates" / "runtime", "headers/toolkit/templates/runtime"))
    for d, arc in sorted(headers):
        if d.is_dir():
            kit.add_tree(d, arc, skip=lambda rel: rel.suffix and rel.suffix not in HEADER_EXT
                         or "__pycache__" in rel.parts)

    # The NDK sysroot: headers, and the start files and stubs for API 30.
    sysroot = next(Path(a.ndk, "toolchains", "llvm", "prebuilt").glob("*")) / "sysroot"
    kit.add_tree(sysroot / "usr" / "include", "sysroot/usr/include")
    kit.add_tree(sysroot / "usr" / "lib" / "aarch64-linux-android" / API,
                 f"sysroot/usr/lib/aarch64-linux-android/{API}")

    # zig's library, without other platforms' C libraries and runtimes.
    zl = Path(a.zig_lib)
    for entry in sorted(zl.iterdir()):
        if entry.name not in ZIG_KEEP:
            continue
        if entry.is_dir():
            kit.add_tree(entry, f"zig/lib/{entry.name}")
        else:
            kit.add(entry, f"zig/lib/{entry.name}")

    spec = {
        "title_name": recipe["title_name"],
        "title_id": recipe.get("title_id"),
        "images": recipe.get("images") or [{"sha1": h} for h in recipe.get("xbe_sha1", [])],
        "xbe": recipe.get("xbe", "default.xbe"),
        "lift": lift,
        "placeholders": recipe.get("placeholders", []),
        "target": "aarch64-linux-android",
        "title_sources": title,
        "gen_dir": "src/recomp/gen",
        "gen_flags": gen_flags,
        "link_flags": ["-Wl,-Bsymbolic", "-lm", "-ldl"],
        "kit_version": kit.digest.hexdigest()[:16],
    }
    kit.add_text("android-build.json", json.dumps(spec, indent=1))
    kit.z.close()
    os.replace(a.out + ".partial", a.out)
    print(f"make_kit: {a.out} ({os.path.getsize(a.out) / 1e6:.1f} MB, "
          f"{len(kit.names)} files, kit {spec['kit_version']})")


if __name__ == "__main__":
    main()
