#!/usr/bin/env python3
"""fetch_deps.py - what an Android APK carries besides the engine.

    python3 tools/android/fetch_deps.py [--deps third_party/android] [--ndk <ndk>]

Into --deps (default third_party/android, ignored by git):

  python/      python.org's CPython for Android (aarch64), the embeddable
               package: libpython3.14.so and the standard library
  zig/         zig for aarch64 Linux: the compiler the device builds with,
               shipped as libzig.so
  capstone/    Capstone 5.0.7 for Android (libcapstone.so, x86 only, built
               here with the NDK) and its Python package
  dxc/         DXC for Android (libdxcompiler.so), from build_dxc.sh: the
               renderer compiles its HLSL at run time

Each is fetched once; a folder already there is kept. The versions are the
ones the APK's build has been run with.
"""
import argparse
import glob
import io
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLKIT = os.path.abspath(os.path.join(HERE, "..", ".."))

PYTHON = "3.14.8"
ZIG = "0.17.0"
CAPSTONE = "5.0.7"
DXC = "v1.9.2609"


def fetch(url):
    print(f"fetch_deps: {url}", flush=True)
    # curl where there is one: a python.org Python on a Mac has no CA
    # certificates until its Install Certificates step has been run.
    if shutil.which("curl"):
        return subprocess.run(["curl", "-fsSL", url], check=True, stdout=subprocess.PIPE).stdout
    with urllib.request.urlopen(url) as r:
        return r.read()


def run(cmd, **kw):
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def find_ndk(arg):
    if arg:
        return arg
    if os.environ.get("ANDROID_NDK_HOME"):
        return os.environ["ANDROID_NDK_HOME"]
    sdk = os.environ.get("ANDROID_HOME") or os.path.expanduser("~/Library/Android/sdk")
    best = None
    for d in glob.glob(os.path.join(sdk, "ndk", "*")):
        props = os.path.join(d, "source.properties")
        if os.path.isfile(props) and "beta" not in open(props).read():
            ver = [int(x) for x in os.path.basename(d).split(".") if x.isdigit()]
            if best is None or ver > best[0]:
                best = (ver, d)
    if not best:
        sys.exit("fetch_deps: no NDK (give --ndk or set ANDROID_NDK_HOME)")
    return best[1]


def python(deps):
    out = os.path.join(deps, "python")
    if os.path.isfile(os.path.join(out, "prefix", "lib", "libpython3.14.so")):
        return
    data = fetch(f"https://www.python.org/ftp/python/{PYTHON}/python-{PYTHON}-aarch64-linux-android.tar.gz")
    os.makedirs(out, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(out, filter="data")


def zig(deps):
    out = os.path.join(deps, "zig")
    if os.path.isfile(os.path.join(out, "zig")):
        return
    data = fetch(f"https://ziglang.org/download/{ZIG}/zig-aarch64-linux-{ZIG}.tar.xz")
    tmp = out + ".partial"
    shutil.rmtree(tmp, ignore_errors=True)
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(tmp, filter="data")
    shutil.rmtree(out, ignore_errors=True)
    os.rename(os.path.join(tmp, f"zig-aarch64-linux-{ZIG}"), out)
    shutil.rmtree(tmp, ignore_errors=True)


def capstone(deps, ndk):
    out = os.path.join(deps, "capstone")
    if os.path.isfile(os.path.join(out, "libcapstone.so")) and \
            os.path.isfile(os.path.join(out, "python", "capstone", "__init__.py")):
        return
    src = os.path.join(out, "src")
    if not os.path.isdir(src):
        run(["git", "clone", "-q", "--depth", "1", "--branch", CAPSTONE,
             "https://github.com/capstone-engine/capstone.git", src])
    build = os.path.join(out, "build")
    run(["cmake", "-S", src, "-B", build, "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake",
         "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-30",
         "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=ON",
         "-DCAPSTONE_BUILD_TESTS=OFF", "-DCAPSTONE_BUILD_CSTOOL=OFF",
         "-DCAPSTONE_ARCHITECTURE_DEFAULT=OFF", "-DCAPSTONE_X86_SUPPORT=ON"],
        stdout=subprocess.DEVNULL)
    run(["ninja", "-C", build], stdout=subprocess.DEVNULL)
    shutil.copy2(os.path.join(build, "libcapstone.so"), os.path.join(out, "libcapstone.so"))
    # Its Python half: the ctypes binding, from the same source tree.
    pkg = os.path.join(out, "python", "capstone")
    shutil.rmtree(pkg, ignore_errors=True)
    shutil.copytree(os.path.join(src, "bindings", "python", "capstone"), pkg,
                    ignore=shutil.ignore_patterns("lib", "include", "__pycache__", "*.so", "*.dylib"))


def dxc(deps, ndk):
    out = os.path.join(deps, "dxc")
    if os.path.isfile(os.path.join(out, "lib", "libdxcompiler.so")):
        return
    # The copy third_party/dxc-android already holds, if build_dxc.sh made one.
    old = os.path.join(TOOLKIT, "third_party", "dxc-android")
    if os.path.isfile(os.path.join(old, "lib", "libdxcompiler.so")):
        shutil.copytree(old, out, dirs_exist_ok=True)
        return
    src = os.path.join(deps, "dxc-src")
    if not os.path.isdir(src):
        run(["git", "clone", "-q", "--depth", "1", "--branch", DXC, "--recurse-submodules",
             "--shallow-submodules", "https://github.com/microsoft/DirectXShaderCompiler.git", src])
    work = os.path.join(deps, "dxc-build")
    run(["sh", os.path.join(HERE, "build_dxc.sh"), src, work, ndk])
    shutil.copytree(os.path.join(work, "android", "out"), out, dirs_exist_ok=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--deps", default=os.path.join(TOOLKIT, "third_party", "android"))
    ap.add_argument("--ndk")
    a = ap.parse_args()
    ndk = find_ndk(a.ndk)
    os.makedirs(a.deps, exist_ok=True)
    python(a.deps)
    zig(a.deps)
    capstone(a.deps, ndk)
    dxc(a.deps, ndk)
    print(f"fetch_deps: everything is in {a.deps}")


if __name__ == "__main__":
    main()
