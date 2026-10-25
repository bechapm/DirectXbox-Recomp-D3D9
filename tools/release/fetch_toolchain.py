"""Fetch the tools a self-building release carries, pinned and checked.

    python3 -m tools.release.fetch_toolchain --out <dir> [--cache <dir>]

A player's machine has no compiler, no CMake and no Python we can rely on
(SteamOS has none of the first two and no Capstone), so the release brings
its own, all of which may be redistributed:

    toolchain/zig      zig 0.17 (MIT): clang, lld, and glibc and libc++ for
                       any target, so it builds for glibc 2.38 on any host
    toolchain/cmake    CMake 4.4 (BSD-3)
    toolchain/ninja    Ninja 1.13 (Apache-2.0)
    toolchain/bin      zcc, zc++, zar, zranlib: the compiler CMake is given
    python/            CPython 3.13 from python-build-standalone (PSF), with
                       Capstone 5.0.7 (BSD-3): the pipeline's interpreter

Every download is the exact file below, checked against its SHA-256 before
it is unpacked. Downloads are kept in --cache, so a second run is offline.

The wrappers fix two things about zig. `zig cc` builds for the host's libc
unless told otherwise, and the host's is whatever the player has; the
target pins glibc 2.38, which SteamOS (2.41) and current distributions
have. And `zig cc` at -O0 adds UB-sanitizer traps, which in lifted code would
turn guest undefined behaviour into a crash; -fno-sanitize=all keeps them
out of every build. (At -O1 to -O3 it adds none, and maps all three to the
same optimisation.)

On a Mac the target is macOS 13 on arm64. zig brings its own macOS libc
headers and libSystem stubs, and links its own libc++, so a player needs
neither Xcode nor its command line tools: the game links only libSystem and
the SDL dylib the engine package carries (make_release.py).
"""

import argparse
import hashlib
import os
import shutil
import stat
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

PYPI = "https://files.pythonhosted.org/packages"
GH_RAW = "https://raw.githubusercontent.com"
PBS = ("https://github.com/astral-sh/python-build-standalone/releases/download/"
       "20261021/cpython-3.13.17%2B20261021-")
# One set per host a release is made for: (url, sha256).
DOWNLOADS = {
    "linux-x86_64": {
        "zig": (f"{PYPI}/py3/z/ziglang/ziglang-0.17.0-py3-none-manylinux_2_12_x86_64."
                "manylinux2010_x86_64.musllinux_1_1_x86_64.whl",
                "97079827064a7492cce5541ceb438ae8003ce1e54e399676da2d62fb7a5dc08b"),
        "cmake": (f"{PYPI}/py3/c/cmake/cmake-4.4.4-py3-none-manylinux2014_x86_64."
                  "manylinux_2_17_x86_64.whl",
                  "f773a0544c66370408f451acd487c2c643b0dfebb48c5d64f3638bc1bb820238"),
        "ninja": (f"{PYPI}/py3/n/ninja/ninja-1.13.2-py3-none-manylinux2014_x86_64."
                  "manylinux_2_17_x86_64.whl",
                  "65a24341b5ac09fcadcc37082660be40a94174e51a937fabf6e2cae26225fa2c"),
        "python": (f"{PBS}x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
                   "f829adc9d888e76c8801b4799ed444188d35239fcae3a9808a99ec5cb0d8d92c"),
        "capstone": (f"{PYPI}/15/3e/3a4f45dd4eda6fc8051f76bf3ed50ead6040d827000371211fbaaf057625/"
                     "capstone-5.0.7-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
                     "b809a9654844ce0d35099121a851ddd2ab2689df1ff6687037babcedcaae6391"),
    },
    "macos-arm64": {
        "zig": (f"{PYPI}/37/fa/54dd03b21fa790a85e1de39eeda9703386fe519490598abdb38894a1c3b3/"
                "ziglang-0.17.0-py3-none-macosx_13_0_arm64.whl",
                "87274af689c4128051b26c80f6f676757e0a81c2fdc137fd8b5679e928e4bc4f"),
        "cmake": (f"{PYPI}/3f/02/86b484e16c91de4bdf90dfab37a33c3e5512bc30d3a494759976df0943b9/"
                  "cmake-4.4.4-py3-none-macosx_10_10_universal2.whl",
                  "883962a72b1b3a16ff445427f24c8caca9797dd9e563bbe208cbcd89e877f9d3"),
        "ninja": (f"{PYPI}/b5/b8/90a9518f2264637084d6199bc20c2f3fb97fefc4c474d277720150c3bd6d/"
                  "ninja-1.13.2-py3-none-macosx_10_9_universal2.whl",
                  "fd82e26c0706ad4ab88e5fdd26f3fab0a987a90f810160f6c322e752c6af298b"),
        "python": (f"{PBS}aarch64-apple-darwin-install_only_stripped.tar.gz",
                   "8dc60610a82f1e73be7faf964383e02520f95f6e39592a5c37c3809f5a7b798e"),
        "capstone": (f"{PYPI}/8f/43/29cd1dbdb2b55bf339bca36444809503b8311dcc04898726a7e35b47ac86/"
                     "capstone-5.0.7-py3-none-macosx_11_0_arm64.whl",
                     "acb89f5bf6f625745a104a3a44819d3acea173228055c1eadc60d2282ae490bb"),
        # The licences of what a Mac release takes from the Vulkan SDK, which
        # carries none of them itself (make_release.py). Third: the name kept
        # in the cache, since two are LICENSE.txt.
        "dxc-license": (f"{GH_RAW}/microsoft/DirectXShaderCompiler/v1.8.2505/LICENSE.TXT",
                        "27a49e35d1da96eba18fba54bc882667ff0ff8c0254f16f2b6e165d605ba7df8",
                        "LICENSE-DXC.txt"),
        "dxc-notices": (f"{GH_RAW}/microsoft/DirectXShaderCompiler/v1.8.2505/ThirdPartyNotices.txt",
                        "19512a5d0a015ef16d167272c164da49a604614a786206212a80ad486ed0be6d",
                        "ThirdPartyNotices-DXC.txt"),
        "moltenvk-license": (f"{GH_RAW}/KhronosGroup/MoltenVK/v1.3.0/LICENSE",
                             "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30",
                             "LICENSE-MoltenVK.txt"),
        "loader-license": (f"{GH_RAW}/KhronosGroup/Vulkan-Loader/vulkan-sdk-1.4.357.0/LICENSE.txt",
                           "43c0a37e6a0fa7ff3c843b3ec5a4fac84b712558ddac103fbd4c1649662a9ece",
                           "LICENSE-Vulkan-Loader.txt"),
    },
}

# What zig is told to build for: glibc 2.38 on Linux, macOS 13 on a Mac.
# An explicit target also keeps zig to its own libc headers and libSystem
# stubs on a Mac, rather than looking for an SDK the player does not have.
TARGETS = {
    "linux-x86_64": "x86_64-linux-gnu.2.38",
    "macos-arm64": "aarch64-macos.13.0",
}


def host_platform():
    """The release being made is for this machine: linux-x86_64 or macos-arm64."""
    import platform
    machine = platform.machine().lower()
    if sys.platform.startswith("linux") and machine in ("x86_64", "amd64"):
        return "linux-x86_64"
    if sys.platform == "darwin" and machine == "arm64":
        return "macos-arm64"
    fail(f"no release toolchain for {sys.platform} {machine} yet")


def wrappers(target):
    """What CMake is given as the compiler and archiver."""
    return {
        "zcc": f"cc -target {target} -fno-sanitize=all",
        "zc++": f"c++ -target {target} -fno-sanitize=all",
        "zar": "ar",
        "zranlib": "ranlib",
    }


def fail(msg):
    print(f"fetch_toolchain: {msg}", file=sys.stderr)
    sys.exit(1)


def fetch(name, cache, plat):
    url, sha, *keep_as = DOWNLOADS[plat][name]
    path = cache / (keep_as[0] if keep_as else url.rsplit("/", 1)[1].replace("%2B", "+"))
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != sha:
        print(f"  downloading {path.name}")
        tmp = path.with_suffix(path.suffix + ".part")
        with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
        got = hashlib.sha256(tmp.read_bytes()).hexdigest()
        if got != sha:
            tmp.unlink()
            fail(f"{path.name}: SHA-256 {got}, expected {sha}")
        tmp.replace(path)
    return path


def unzip_prefix(whl, prefix, dest):
    """The files under `prefix` in a wheel, into dest, keeping their modes."""
    with zipfile.ZipFile(whl) as z:
        for info in z.infolist():
            if not info.filename.startswith(prefix) or info.filename.endswith("/"):
                continue
            out = dest / info.filename[len(prefix):]
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(z.read(info))
            mode = (info.external_attr >> 16) & 0o777
            if mode:
                out.chmod(mode)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="the release folder to fill")
    ap.add_argument("--cache", help="where downloads are kept (default <out>/../downloads)")
    args = ap.parse_args()
    plat = host_platform()
    out = Path(args.out).resolve()
    cache = Path(args.cache).resolve() if args.cache else out.parent / "downloads"
    cache.mkdir(parents=True, exist_ok=True)
    tc = out / "toolchain"
    for d in ("zig", "cmake", "ninja", "bin"):
        shutil.rmtree(tc / d, ignore_errors=True)
    shutil.rmtree(out / "python", ignore_errors=True)

    unzip_prefix(fetch("zig", cache, plat), "ziglang/", tc / "zig")
    for f in ("__init__.py", "__main__.py"):
        (tc / "zig" / f).unlink(missing_ok=True)
    unzip_prefix(fetch("cmake", cache, plat), "cmake/data/", tc / "cmake")
    unzip_prefix(fetch("ninja", cache, plat), "ninja-1.13.2.data/scripts/", tc / "ninja" / "bin")
    for f in [tc / "zig" / "zig", tc / "ninja" / "bin" / "ninja",
              *(tc / "cmake" / "bin").iterdir()]:
        f.chmod(f.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    (tc / "bin").mkdir(parents=True)
    for name, args_ in wrappers(TARGETS[plat]).items():
        w = tc / "bin" / name
        w.write_text('#!/bin/sh\n'
                     '# The release toolchain (tools/release/fetch_toolchain.py).\n'
                     'd=$(dirname "$(readlink -f "$0")")\n'
                     f'exec "$d/../zig/zig" {args_} "$@"\n')
        w.chmod(0o755)
    if plat.startswith("macos"):
        # CMake asks xcrun for an SDK when none is given. On a Mac without
        # the command line tools, /usr/bin/xcrun is a stub that offers to
        # install them, in a dialog, in the middle of a build; zig needs no
        # SDK, so the builder puts this first on PATH instead.
        w = tc / "bin" / "xcrun"
        w.write_text('#!/bin/sh\n'
                     '# The release toolchain (tools/release/fetch_toolchain.py): no SDK here.\n'
                     'exit 1\n')
        w.chmod(0o755)

    with tarfile.open(fetch("python", cache, plat)) as t:
        t.extractall(out, filter="tar")          # unpacks as python/
    # What the pipeline never loads: Tk and IDLE, pip's bootstrap, the C
    # headers and the man pages. About 20 MB of the 130.
    py = out / "python"
    stdlib = next((py / "lib").glob("python3.*"))
    for p in [*(py / "lib").glob("tcl*"), *(py / "lib").glob("tk*"),
              *(py / "lib").glob("itcl*"), *(py / "lib").glob("libtcl*"),
              stdlib / "tkinter", stdlib / "idlelib", stdlib / "turtledemo",
              stdlib / "ensurepip", py / "include", py / "share"]:
        if p.is_dir():
            shutil.rmtree(p)
        elif p.exists():
            p.unlink()
    site = stdlib / "site-packages"
    with zipfile.ZipFile(fetch("capstone", cache, plat)) as z:
        z.extractall(site)
    for so in [*site.rglob("*.so"), *site.rglob("*.dylib")]:
        so.chmod(0o755)

    sizes = {d: sum(f.lstat().st_size for f in (out / d).rglob("*")
                    if f.is_file() and not f.is_symlink())
             for d in ("toolchain", "python")}
    print(f"toolchain: {sizes['toolchain'] / 1e6:.0f} MB, python: "
          f"{sizes['python'] / 1e6:.0f} MB, in {out}")


if __name__ == "__main__":
    main()
