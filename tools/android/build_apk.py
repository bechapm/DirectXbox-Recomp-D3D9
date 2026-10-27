#!/usr/bin/env python3
"""build_apk.py - a title's Android APK, which builds its game on the device.

    python3 xboxrecomp/tools/android/build_apk.py --package org.xboxrecomp.ts2 \\
        --label "TimeSplitters 2" [--project .] [--recipe release/x.json] [--out <apk>]

The APK carries no game code. It carries the engine (libmain.so), what the
device builds the game with -- Python (libpython3.14.so, libpyexec.so),
Capstone, zig, and the build kit in its assets (make_kit.py) -- and DXC. On
first launch the app builds libgame.so from the player's own copy
(device_build.py, android/java/.../BuildActivity.java).

So this needs no game either: like a desktop release (make_release.py) it
configures a copy of the title's recipe with a placeholder for the lifted
code, which is enough for the engine, Python's launcher and the flags the
device compiles with. The recipe (--recipe, else the one in <project>/release)
says which images of the game the release is for, and how the title lifts.

Fetch the rest first: tools/android/fetch_deps.py (into
<toolkit>/third_party/android, or --deps).

The launcher icon is --icon, else the one the recipe names, else the
toolkit's own:

    "android": {"icon": "release/android/icon.png"}

The path is relative to the project. A PNG, square, 192 pixels or more;
make_apk.py scales it to every density.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLKIT = os.path.abspath(os.path.join(HERE, "..", ".."))


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def find_sdk(arg):
    for d in (arg, os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"),
              os.path.expanduser("~/Library/Android/sdk"), os.path.expanduser("~/Android/Sdk")):
        if d and os.path.isdir(d):
            return d
    sys.exit("build_apk: no Android SDK (give --sdk or set ANDROID_HOME)")


def find_ndk(arg, sdk):
    if arg:
        return arg
    if os.environ.get("ANDROID_NDK_HOME"):
        return os.environ["ANDROID_NDK_HOME"]
    # The newest release NDK (not a beta): its source.properties has no "beta".
    best = None
    for d in glob.glob(os.path.join(sdk, "ndk", "*")):
        props = os.path.join(d, "source.properties")
        if not os.path.isfile(props) or "beta" in open(props).read():
            continue
        ver = [int(x) for x in os.path.basename(d).split(".") if x.isdigit()]
        if best is None or ver > best[0]:
            best = (ver, d)
    if not best:
        sys.exit("build_apk: no NDK under %s/ndk (give --ndk or set ANDROID_NDK_HOME)" % sdk)
    return best[1]


def find_recipe(project, recipe):
    if recipe:
        return os.path.abspath(recipe)
    found = sorted(glob.glob(os.path.join(project, "release", "*.json")))
    if len(found) != 1:
        sys.exit("build_apk: name the release recipe with --recipe "
                 f"({len(found)} in {project}/release)")
    return found[0]


def recipe_icon(project, recipe):
    """The icon the release recipe names under "android", or None."""
    with open(recipe, encoding="utf-8") as f:
        icon = (json.load(f).get("android") or {}).get("icon")
    if not icon:
        return None
    path = os.path.join(project, icon)
    if not os.path.isfile(path):
        sys.exit("build_apk: %s names the icon %s, which is not there" % (recipe, path))
    return path


def configured_copy(project, recipe, work):
    """The title's recipe in a folder of its own, with a placeholder for the
    lifted code and whatever else its CMakeLists wants before it configures
    (make_release.py does the same for a desktop launcher)."""
    sys.path.insert(0, os.path.join(TOOLKIT, "tools", "release"))
    import make_release
    lift = recipe.get("lift")
    wanted = set(recipe.get("include", [])) | ({lift[0]} if lift else set())
    copy = os.path.join(work, "project")
    shutil.rmtree(copy, ignore_errors=True)
    for rel in make_release.tracked_files(Path(project), ["."]):
        if not (rel == "CMakeLists.txt" or rel.startswith(("src/", "config/")) or rel in wanted
                or any(rel.startswith(w.rstrip("/") + "/") for w in wanted)):
            continue
        if "/recomp/gen/" in rel:
            continue
        os.makedirs(os.path.dirname(os.path.join(copy, rel)), exist_ok=True)
        shutil.copy2(os.path.join(project, rel), os.path.join(copy, rel))
    os.symlink(TOOLKIT, os.path.join(copy, "xboxrecomp"))
    gen = os.path.join(copy, "src", "recomp", "gen")
    os.makedirs(gen, exist_ok=True)
    with open(os.path.join(gen, "placeholder.c"), "w") as f:
        f.write("/* build_apk.py: the APK carries no lifted code. */\n"
                "int xboxrecomp_android_placeholder;\n")
    for rp in recipe.get("placeholders", []):
        os.makedirs(os.path.dirname(os.path.join(copy, rp)), exist_ok=True)
        with open(os.path.join(copy, rp), "w") as f:
            f.write("/* build_apk.py: a placeholder; the device's lift writes it. */\n")
    return copy


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--project", default=".")
    ap.add_argument("--package", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--out")
    ap.add_argument("--recipe", help="the release recipe (default: the one in <project>/release)")
    ap.add_argument("--icon", help="the launcher icon, a PNG (default: the recipe's "
                                   "android.icon, else the toolkit's)")
    ap.add_argument("--deps", default=os.path.join(TOOLKIT, "third_party", "android"),
                    help="what fetch_deps.py fetched")
    ap.add_argument("--sdk")
    ap.add_argument("--ndk")
    ap.add_argument("--api", default="30")
    ap.add_argument("--version-code", default="1")
    ap.add_argument("--version-name", default="1.0")
    a = ap.parse_args()

    project = os.path.abspath(a.project)
    recipe_path = find_recipe(project, a.recipe)
    recipe = json.load(open(recipe_path, encoding="utf-8"))
    sdk = find_sdk(a.sdk)
    ndk = find_ndk(a.ndk, sdk)
    deps = os.path.abspath(a.deps)
    need = {
        "dxc": os.path.join(deps, "dxc", "lib", "libdxcompiler.so"),
        "python": os.path.join(deps, "python", "prefix", "lib", "libpython3.14.so"),
        "zig": os.path.join(deps, "zig", "zig"),
        "capstone": os.path.join(deps, "capstone", "libcapstone.so"),
    }
    missing = [k for k, v in need.items() if not os.path.isfile(v)]
    if missing:
        sys.exit(f"build_apk: no {', '.join(missing)} in {deps} (run tools/android/fetch_deps.py)")
    icon = a.icon or recipe_icon(project, recipe_path)

    work = os.path.join(project, "build", "android-release")
    os.makedirs(work, exist_ok=True)
    copy = configured_copy(project, recipe, work)
    build = os.path.join(work, "build")
    if not os.path.isfile(os.path.join(build, "build.ninja")):
        run(["cmake", "-S", copy, "-B", build, "-G", "Ninja",
             "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(ndk, "build", "cmake", "android.toolchain.cmake"),
             "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-" + a.api,
             "-DANDROID_STL=c++_static", "-DCMAKE_BUILD_TYPE=Release",
             "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
             # The toolkit linked into the copy: a title whose CMakeLists
             # looks for it beside its own checkout (../DirectXbox-Recomp)
             # finds it here, as a desktop release passes its engine.
             "-DXBOXRECOMP_DIR=" + os.path.join(copy, "xboxrecomp"),
             # The headers of the DXC the APK carries: without them the
             # engine has no renderer (src/d3d/CMakeLists.txt refuses).
             "-DXBOXRECOMP_DXC_INCLUDE=" + os.path.join(deps, "dxc", "include", "dxc")])
    run(["cmake", "--build", build, "--target", "main", "pyexec"])

    # XbSymbolDatabase's tool, which names the XDK functions in the player's
    # XBE for a recipe that does not carry them (as a desktop release's
    # bin/XbSymbolDatabaseCLI does). An executable, shipped as a library for
    # the same reason as zig below.
    xbsdb = os.path.join(work, "xbsdb")
    if not os.path.isfile(os.path.join(xbsdb, "build.ninja")):
        run(["cmake", "-S", os.path.join(TOOLKIT, "third_party", "XbSymbolDatabase"), "-B", xbsdb,
             "-G", "Ninja",
             "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(ndk, "build", "cmake", "android.toolchain.cmake"),
             "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-" + a.api,
             "-DCMAKE_BUILD_TYPE=Release", "-DXBSDB_BUILD_UNITTEST=OFF"])
    run(["cmake", "--build", xbsdb, "--target", "XbSymbolDatabaseCLI"])
    libxbsdb = os.path.join(work, "libxbsdbcli.so")
    shutil.copy2(os.path.join(xbsdb, "projects", "cli", "XbSymbolDatabaseCLI"), libxbsdb)

    # libzig.so: the compiler, named as a library so the APK carries it in
    # its library folder, where Android lets a program be started.
    libzig = os.path.join(work, "libzig.so")
    if not os.path.isfile(libzig) or os.path.getmtime(libzig) < os.path.getmtime(need["zig"]):
        shutil.copy2(need["zig"], libzig)
    kit = os.path.join(work, "buildkit.zip")
    run([sys.executable, os.path.join(HERE, "make_kit.py"), "--build", build,
         "--project", project, "--spec-project", copy, "--recipe", recipe_path,
         "--python", os.path.join(deps, "python", "prefix"),
         "--zig-lib", os.path.join(deps, "zig", "lib"),
         "--capstone", os.path.join(deps, "capstone", "python", "capstone"),
         "--ndk", ndk, "--out", kit])

    images = recipe.get("images") or [{"sha1": h} for h in recipe.get("xbe_sha1", [])]
    out = a.out or os.path.join(project, "build", a.package + ".apk")
    env = dict(os.environ, ANDROID_HOME=sdk)
    libs = [os.path.join(build, "libmain.so"), os.path.join(build, "xboxrecomp", "libpyexec.so"),
            need["python"], need["capstone"], libzig, libxbsdb, need["dxc"]]
    cmd = [sys.executable, os.path.join(HERE, "make_apk.py")]
    for lib in libs:
        cmd += ["--lib", lib]
    cmd += ["--asset", kit, "--package", a.package, "--label", a.label,
            "--xbe", recipe.get("xbe", "default.xbe"),
            "--xbe-sha1", ",".join(i["sha1"] for i in images),
            "--min-sdk", a.api, "--version-code", a.version_code,
            "--version-name", a.version_name, "--strip", "--out", out]
    if icon:
        cmd += ["--icon", icon]
    run(cmd, env=env)


if __name__ == "__main__":
    main()
