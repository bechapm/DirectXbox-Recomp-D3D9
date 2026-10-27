#!/usr/bin/env python3
"""make_apk.py - wrap a title's libmain.so in an installable APK.

No Gradle: the shell is SDL's Java activity plus android/ in the toolkit,
compiled with javac and d8, linked with aapt2, aligned with zipalign and
signed with apksigner, all from the Android SDK's build tools. That is the
whole of what an APK needs here, and it is what a self-building release can
run on a player's machine with no network after the SDK is fetched.

    python3 tools/android/make_apk.py --lib build/android/libmain.so \\
        --lib <dxc>/libdxcompiler.so --package org.xboxrecomp.ts2 \\
        --label "TimeSplitters 2" --out ts2.apk

The launcher icon is --icon (a PNG, square, 192 pixels or more; or an SVG
when rsvg-convert is installed), else the toolkit's own
(assets/icon/png/512.png). It is scaled to the five launcher densities with
Pillow when it is installed and otherwise by the small PNG reader and writer
below, so no player's machine needs anything extra for it.

The signing key is made once and kept (--keystore, default
<per-user folder>/android/signing.p12): Android refuses an update signed with
another key, and replacing an app to change its key deletes its data, saves
included.

A published release is signed with one key that every build uses, so each
release installs over the last: ANDROID_SIGNING_KEY, the PKCS12 keystore
base64-encoded (a CI secret), with its password in ANDROID_SIGNING_PASSWORD.
The password reaches apksigner through the environment, never on its command
line, so it is never printed.
"""
import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

TOOLKIT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SHELL = os.path.join(TOOLKIT, "android")
SDL_JAVA = os.path.join(TOOLKIT, "third_party", "SDL", "android-project", "app",
                        "src", "main", "java")
DEFAULT_ICON = os.path.join(TOOLKIT, "assets", "icon", "png", "512.png")
# The launcher densities and their icon sizes in pixels (48 dp each).
DENSITIES = [("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)]
KEY_ALIAS = "xboxrecomp"
KEY_PASS = "xboxrecomp"   # protects nothing: the key lives in the player's own folder


def sdk_dir(arg):
    for d in (arg, os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"),
              os.path.expanduser("~/Library/Android/sdk"), os.path.expanduser("~/Android/Sdk")):
        if d and os.path.isdir(os.path.join(d, "platforms")):
            return d
    sys.exit("make_apk: no Android SDK (give --sdk or set ANDROID_HOME)")


def newest(pattern, what):
    found = sorted(glob.glob(pattern), key=lambda p: [int(x) if x.isdigit() else 0
                                                     for x in os.path.basename(p).replace("-", ".").split(".")])
    if not found:
        sys.exit("make_apk: no %s (%s)" % (what, pattern))
    return found[-1]


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def default_keystore():
    if sys.platform == "win32":
        base = os.environ.get("APPDATA", os.path.expanduser("~"))
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(os.environ.get("RECOMP_USER_DIR") or os.path.join(base, "xboxrecomp"),
                        "android", "signing.p12")


def ensure_key(path):
    if os.path.exists(path):
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    print("make_apk: making the signing key %s (keep it: an update needs the same key)" % path)
    run(["keytool", "-genkeypair", "-keystore", path, "-storetype", "PKCS12",
         "-storepass", KEY_PASS, "-keypass", KEY_PASS, "-alias", KEY_ALIAS,
         "-keyalg", "RSA", "-keysize", "3072", "-validity", "36500",
         "-dname", "CN=xboxrecomp player build"])


# --- The launcher icon ------------------------------------------------------
#
# A PNG reader and writer in the standard library, enough for an icon: 8-bit
# greyscale, RGB, palette and their alpha forms, not interlaced. Anything else
# is used through Pillow, or refused with a message saying so.

def read_png(path):
    """(width, height, RGBA bytes) of a PNG."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, plte, trns = 8, [], None, None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            plte = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if depth != 8 or interlace or ctype not in (0, 2, 3, 4, 6):
        raise ValueError("bit depth %d, colour type %d%s: only 8-bit, not interlaced, "
                         "is read without Pillow" % (depth, ctype, ", interlaced" if interlace else ""))
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    prev = bytearray(stride)
    rows = []
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    px = bytearray(w * h * 4)
    o = 0
    for line in rows:
        for x in range(w):
            if ctype == 6:
                px[o:o + 4] = line[x * 4:x * 4 + 4]
            elif ctype == 2:
                r, g, b = line[x * 3:x * 3 + 3]
                opaque = trns is None or len(trns) < 6 or (r, g, b) != struct.unpack(">HHH", trns)
                px[o:o + 4] = bytes((r, g, b, 255 if opaque else 0))
            elif ctype == 3:
                i = line[x]
                alpha = trns[i] if trns and i < len(trns) else 255
                px[o:o + 4] = bytes(plte[i * 3:i * 3 + 3]) + bytes((alpha,))
            else:
                v = line[x * bpp]
                alpha = line[x * 2 + 1] if ctype == 4 else (
                    0 if trns and len(trns) >= 2 and v == struct.unpack(">H", trns[:2])[0] else 255)
                px[o:o + 4] = bytes((v, v, v, alpha))
            o += 4
    return w, h, px


def write_png(path, w, h, px):
    raw = b"".join(b"\0" + bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    chunk = lambda kind, body: (struct.pack(">I", len(body)) + kind + body +
                                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def _weights(n_in, n_out):
    """For each output sample, the input samples it covers and by how much: an
    area average, so a large icon shrinks without aliasing."""
    scale = n_in / n_out
    out = []
    for i in range(n_out):
        lo, hi = i * scale, (i + 1) * scale
        taps = []
        for j in range(int(lo), min(n_in, int(hi) + 1)):
            cover = min(hi, j + 1) - max(lo, j)
            if cover > 1e-9:
                taps.append((j, cover / scale))
        out.append(taps)
    return out


def resize_rgba(w, h, px, size):
    """The image centred in a size x size square, aspect kept, transparent
    around it if it is not square. Averaged with premultiplied alpha, so a
    transparent pixel's colour does not bleed into its neighbours."""
    ow, oh = (size, max(1, round(size * h / w))) if w >= h else (max(1, round(size * w / h)), size)
    pre = [0.0] * (w * h * 4)
    for i in range(w * h):
        a = px[i * 4 + 3] / 255.0
        pre[i * 4:i * 4 + 4] = (px[i * 4] * a, px[i * 4 + 1] * a, px[i * 4 + 2] * a, px[i * 4 + 3])
    wx, wy = _weights(w, ow), _weights(h, oh)
    tmp = [0.0] * (ow * h * 4)
    for y in range(h):
        for x, taps in enumerate(wx):
            acc = [0.0, 0.0, 0.0, 0.0]
            for j, k in taps:
                s = (y * w + j) * 4
                for c in range(4):
                    acc[c] += pre[s + c] * k
            tmp[(y * ow + x) * 4:(y * ow + x) * 4 + 4] = acc
    out = bytearray(size * size * 4)
    x0, y0 = (size - ow) // 2, (size - oh) // 2
    for y, taps in enumerate(wy):
        for x in range(ow):
            acc = [0.0, 0.0, 0.0, 0.0]
            for j, k in taps:
                s = (j * ow + x) * 4
                for c in range(4):
                    acc[c] += tmp[s + c] * k
            a = acc[3]
            o = ((y0 + y) * size + x0 + x) * 4
            if a > 0.5:
                f = 255.0 / a
                out[o:o + 4] = bytes(min(255, int(acc[c] * f + 0.5)) for c in range(3)) + \
                    bytes((min(255, int(a + 0.5)),))
    return out


def launcher_icons(src, res):
    """res/mipmap-<density>/ic_launcher.png from src, at every density."""
    if src.lower().endswith(".svg"):
        rsvg = shutil.which("rsvg-convert")
        if not rsvg:
            sys.exit("make_apk: an SVG icon needs rsvg-convert (librsvg); give a PNG instead")
        for density, size in DENSITIES:
            d = os.path.join(res, "mipmap-" + density)
            os.makedirs(d, exist_ok=True)
            run([rsvg, "-w", str(size), "-h", str(size), "-a", "-o",
                 os.path.join(d, "ic_launcher.png"), src])
        return
    try:
        from PIL import Image
    except ImportError:
        Image = None
    if Image is not None:
        im = Image.open(src).convert("RGBA")
        w, h = im.size
    else:
        try:
            w, h, px = read_png(src)
        except ValueError as e:
            sys.exit("make_apk: cannot read the icon %s without Pillow (%s): save it as an "
                     "8-bit PNG or install Pillow" % (src, e))
    if min(w, h) < DENSITIES[-1][1]:
        print("make_apk: the icon %s is %dx%d; it is scaled up to %d and will look soft"
              % (src, w, h, DENSITIES[-1][1]))
    print("make_apk: launcher icon %s (%dx%d, scaled %s)"
          % (src, w, h, "with Pillow" if Image is not None else "without Pillow"), flush=True)
    for density, size in DENSITIES:
        d = os.path.join(res, "mipmap-" + density)
        os.makedirs(d, exist_ok=True)
        dst = os.path.join(d, "ic_launcher.png")
        if Image is not None:
            im2 = im.copy()
            im2.thumbnail((size, size), Image.LANCZOS)
            canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
            canvas.paste(im2, ((size - im2.width) // 2, (size - im2.height) // 2))
            canvas.save(dst)
        else:
            write_png(dst, size, size, resize_rgba(w, h, px, size))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lib", action="append", required=True,
                    help="a shared library for lib/arm64-v8a (libmain.so first)")
    ap.add_argument("--package", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--version-code", default="1")
    ap.add_argument("--version-name", default="1.0")
    ap.add_argument("--xbe", default="default.xbe",
                    help="the XBE the game boots, which the app's setup looks for "
                         "(a recipe's \"xbe\"; default.xbe)")
    ap.add_argument("--xbe-sha1", default="",
                    help="SHA-1s of the --xbe images the release is for, comma "
                         "separated: what the app's setup looks for on the device "
                         "(any copy if empty)")
    ap.add_argument("--icon", default=DEFAULT_ICON,
                    help="the launcher icon: a PNG, square, 192 px or more (or an SVG with "
                         "rsvg-convert installed); default the toolkit's own")
    ap.add_argument("--min-sdk", default="30")
    ap.add_argument("--target-sdk", default="34")
    ap.add_argument("--sdk")
    ap.add_argument("--keystore", default=None)
    ap.add_argument("--asset", action="append", default=[],
                    help="a file for the APK's assets/, stored as it is (the build kit)")
    ap.add_argument("--strip", action="store_true",
                    help="drop debug sections from the libraries (dynamic symbols stay)")
    ap.add_argument("--profileable", action="store_true",
                    help="let simpleperf profile the game from adb (android:profileable "
                         "shell=\"true\"): for a side-by-side dev package, not a release")
    a = ap.parse_args()

    sdk = sdk_dir(a.sdk)
    bt = newest(os.path.join(sdk, "build-tools", "*"), "build tools")
    platform = newest(os.path.join(sdk, "platforms", "android-*"), "platform")
    android_jar = os.path.join(platform, "android.jar")
    tool = lambda name: os.path.join(bt, name)
    keystore = a.keystore or default_keystore()

    work = tempfile.mkdtemp(prefix="xbr-apk-")
    try:
        # 1. Java: SDL's activity and ours, to one classes.dex.
        sources = glob.glob(os.path.join(SDL_JAVA, "org", "libsdl", "app", "*.java"))
        sources += glob.glob(os.path.join(SHELL, "java", "**", "*.java"), recursive=True)
        classes = os.path.join(work, "classes")
        os.makedirs(classes)
        run(["javac", "-nowarn", "-Xlint:none", "-encoding", "UTF-8", "--release", "11",
             "-classpath", android_jar, "-d", classes] + sources,
            stdout=subprocess.DEVNULL)
        class_files = glob.glob(os.path.join(classes, "**", "*.class"), recursive=True)
        dex = os.path.join(work, "dex")
        os.makedirs(dex)
        run([tool("d8"), "--release", "--min-api", a.min_sdk, "--lib", android_jar,
             "--output", dex] + class_files)

        # 2. The launcher icon, compiled to flat resources for the link.
        if not os.path.isfile(a.icon):
            sys.exit("make_apk: no icon %s" % a.icon)
        res = os.path.join(work, "res")
        launcher_icons(a.icon, res)
        flat = os.path.join(work, "flat")
        os.makedirs(flat)
        for density, _ in DENSITIES:
            run([tool("aapt2"), "compile", "-o", flat,
                 os.path.join(res, "mipmap-" + density, "ic_launcher.png")])
        flats = sorted(glob.glob(os.path.join(flat, "*.flat")))

        # 3. The manifest, linked with the icon into a resource-only APK.
        with open(os.path.join(SHELL, "AndroidManifest.xml.in"), encoding="utf-8") as f:
            manifest = f.read()
        for k, v in {"PACKAGE": a.package, "LABEL": a.label, "VERSION_CODE": a.version_code,
                     "VERSION_NAME": a.version_name, "MIN_SDK": a.min_sdk,
                     "XBE": a.xbe, "XBE_SHA1": a.xbe_sha1.lower(),
                     "TARGET_SDK": a.target_sdk}.items():
            manifest = manifest.replace("@%s@" % k, v.replace("&", "&amp;").replace('"', "&quot;"))
        manifest = manifest.replace(
            "@PROFILEABLE@", '<profileable android:shell="true" />' if a.profileable else "")
        man_path = os.path.join(work, "AndroidManifest.xml")
        with open(man_path, "w", encoding="utf-8") as f:
            f.write(manifest)
        base = os.path.join(work, "base.apk")
        run([tool("aapt2"), "link", "-I", android_jar, "--manifest", man_path,
             "-o", base] + sum((["-R", f] for f in flats), []))

        # 4. Code and libraries in, compressed: Android unpacks the libraries
        # at install (extractNativeLibs=true, android/AndroidManifest.xml.in).
        unaligned = os.path.join(work, "unaligned.apk")
        shutil.copy(base, unaligned)
        with zipfile.ZipFile(unaligned, "a") as z:
            z.write(os.path.join(dex, "classes.dex"), "classes.dex",
                    compress_type=zipfile.ZIP_DEFLATED)
            # Stored, not compressed again: the build kit is a zip of its own,
            # and the app opens it by file descriptor (openFd needs it stored).
            for asset in a.asset:
                z.write(asset, "assets/" + os.path.basename(asset),
                        compress_type=zipfile.ZIP_STORED)
            for lib in a.lib:
                src = lib
                if a.strip:
                    strip = newest(os.path.join(sdk, "ndk", "*", "toolchains", "llvm",
                                                "prebuilt", "*", "bin", "llvm-strip"), "llvm-strip")
                    src = os.path.join(work, os.path.basename(lib))
                    run([strip, "--strip-debug", "-o", src, lib])
                z.write(src, "lib/arm64-v8a/" + os.path.basename(lib),
                        compress_type=zipfile.ZIP_DEFLATED)

        # 5. Align (16 KB pages for the libraries), then sign.
        aligned = os.path.join(work, "aligned.apk")
        run([tool("zipalign"), "-P", "16", "-f", "4", unaligned, aligned])
        release_key = os.environ.get("ANDROID_SIGNING_KEY", "").strip()
        if release_key:
            # The release's key, from the CI secret: written out only for
            # apksigner, inside the folder removed below.
            import base64
            keystore = os.path.join(work, "release.p12")
            with open(keystore, "wb") as f:
                f.write(base64.b64decode(release_key))
            if not os.environ.get("ANDROID_SIGNING_PASSWORD"):
                sys.exit("make_apk: ANDROID_SIGNING_KEY is set but ANDROID_SIGNING_PASSWORD is not")
            print("make_apk: signing with the release key (ANDROID_SIGNING_KEY)", flush=True)
            # One key in the keystore, so no alias to name; one password for
            # the store and the key, as keytool makes a PKCS12 one.
            run([tool("apksigner"), "sign", "--ks", keystore,
                 "--ks-pass", "env:ANDROID_SIGNING_PASSWORD",
                 "--key-pass", "env:ANDROID_SIGNING_PASSWORD",
                 "--out", a.out, aligned])
        else:
            ensure_key(keystore)
            run([tool("apksigner"), "sign", "--ks", keystore, "--ks-pass", "pass:" + KEY_PASS,
                 "--ks-key-alias", KEY_ALIAS, "--key-pass", "pass:" + KEY_PASS,
                 "--out", a.out, aligned])
        print("make_apk: %s (%.1f MB)" % (a.out, os.path.getsize(a.out) / 1e6))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
