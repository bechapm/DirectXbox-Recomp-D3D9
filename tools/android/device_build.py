#!/usr/bin/env python3
"""device_build.py - the title's game library, built on the player's device.

An Android APK carries no game code. The first time it starts (and after an
update), the app runs this, with the Python the APK carries (libpyexec.so),
to make libgame.so from the player's own copy of the disc. It does on the
device what xboxrecomp-build (tools/builder) does on a desktop:

  1. the game: an ISO is extracted once; a folder is read where it is
  2. the check: default.xbe's SHA-1 must be one the release lists
  3. the lift: the title's own recipe (release.json's "lift"), on a private
     copy of the XBE, in a copy of the title's project
  4. the compile: the lifted C and the title's own sources, with zig, with
     the flags the APK's build recorded (android-build.json)
  5. the link: libgame.so against the APK's libmain.so, and a stamp saying
     what it was built from

Progress goes to stdout, one JSON object a line, for the app to draw:
  {"step": "lift", "done": 2, "total": 5, "what": "..."}
  {"result": "ok", "game_dir": "...", "image": "PAL"}
  {"result": "error", "message": "..."}
Everything the tools print goes to <work>/build.log.

    libpyexec.so device_build.py --kit <dir> --source <folder or .iso>
        --work <dir> --game-out <dir> --libdir <dir> --out <libgame.so>
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

LOG = None


def progress(step, done, total, what=""):
    print(json.dumps({"step": step, "done": done, "total": total, "what": what}), flush=True)


def finish(result, **kw):
    print(json.dumps(dict(result=result, **kw)), flush=True)


class Failure(Exception):
    pass


def log(msg):
    LOG.write(msg + "\n")
    LOG.flush()
    # And to stdout, which the app copies to logcat; the app reads only the
    # JSON lines for its screen.
    print(msg, flush=True)


def run(cmd, cwd=None, env=None, what="a step"):
    log("$ " + " ".join(str(c) for c in cmd))
    t = time.time()
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       errors="replace")
    if r.stdout.strip():
        log(r.stdout.rstrip())
    log(f"  exit {r.returncode} after {time.time() - t:.1f} s")
    if r.returncode != 0:
        raise Failure(f"{what} failed (exit {r.returncode}); see build.log")


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---- 1. the game ------------------------------------------------------------

def game_folder(source, game_out, toolkit, xbe_name):
    """The folder the game runs from: the source itself, or an ISO's files."""
    if os.path.isdir(source):
        if not os.path.isfile(os.path.join(source, xbe_name)):
            raise Failure(f"there is no {xbe_name} in {source}")
        return source
    if not os.path.isfile(source):
        raise Failure(f"{source} is not a folder or a disc image")
    st = os.stat(source)
    stamp = os.path.join(game_out, ".extracted-from")
    mark = f"{source}\n{st.st_size}\n{int(st.st_mtime)}\n"
    if os.path.isfile(stamp) and open(stamp).read() == mark and \
            os.path.isfile(os.path.join(game_out, xbe_name)):
        return game_out
    sys.path.insert(0, toolkit)
    from tools.xiso.xdvdfs import Xiso
    shutil.rmtree(game_out, ignore_errors=True)
    os.makedirs(game_out, exist_ok=True)
    with Xiso(source) as iso:
        entries = [(d, e) for d, e in iso.walk()]
        files = [(d, e) for d, e in entries if not e.is_dir]
        for i, (d, e) in enumerate(files):
            rel = (d + "/" + e.name).lstrip("/")
            dest = os.path.join(game_out, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            iso.extract(e, dest)
            if i % 20 == 0 or i == len(files) - 1:
                progress("game", i + 1, len(files), rel)
    open(stamp, "w").write(mark)
    return game_out


# ---- 3. the lift ------------------------------------------------------------

def lift(kit, spec, work, game, image, env):
    project = os.path.join(work, "project")
    xdir = os.path.join(work, "xbe")
    # A fresh copy of the title's project each time: the lift writes into it.
    shutil.rmtree(project, ignore_errors=True)
    shutil.copytree(os.path.join(kit, "recipe"), project, symlinks=True)
    os.symlink(os.path.join(kit, "toolkit"), os.path.join(project, "xboxrecomp"))
    # The pipeline writes beside the XBE it is given, so it gets a copy.
    os.makedirs(xdir, exist_ok=True)
    xbe = os.path.join(xdir, spec["xbe"])
    shutil.copyfile(os.path.join(game, spec["xbe"]), xbe)

    hook = spec.get("lift")
    if not hook:
        raise Failure("release.json names no lift")
    cmd = [sys.executable, os.path.join(project, hook[0])] + \
          [a.replace("{xbe_dir}", xdir) for a in hook[1:]]
    progress("lift", 0, 1, image)
    log(f"lifting {image} with the title's recipe")
    run(cmd, cwd=project, env=env, what="the translation to C")
    progress("lift", 1, 1, image)
    return project


# ---- 4-5. compile and link --------------------------------------------------

def expand(args, kit, project):
    return [a.replace("{kit}", kit).replace("{project}", project) for a in args]


def zig_env(kit, work, env):
    sysroot = os.path.join(kit, "sysroot")
    crt = os.path.join(sysroot, "usr", "lib", "aarch64-linux-android", "30")
    libc = os.path.join(work, "libc.txt")
    with open(libc, "w") as f:
        f.write(f"include_dir={sysroot}/usr/include\n"
                f"sys_include_dir={sysroot}/usr/include/aarch64-linux-android\n"
                f"crt_dir={crt}\ncc_dir={crt}\n"
                "msvc_lib_dir=\nkernel32_lib_dir=\ndarwin_sdk_dir=\n")
    e = dict(env)
    e["ZIG_LIBC"] = libc
    e["ZIG_LIB_DIR"] = os.path.join(kit, "zig", "lib")
    e["ZIG_GLOBAL_CACHE_DIR"] = os.path.join(work, "zig-cache")
    e["ZIG_LOCAL_CACHE_DIR"] = os.path.join(work, "zig-cache")
    return e


def compile_all(kit, spec, project, work, zig, env, jobs):
    obj = os.path.join(work, "obj")
    shutil.rmtree(obj, ignore_errors=True)
    os.makedirs(obj)
    units = []
    for t in spec["title_sources"]:
        units.append((os.path.join(project, t["src"]), expand(t["flags"], kit, project)))
    gen = os.path.join(project, spec["gen_dir"])
    gen_flags = expand(spec["gen_flags"], kit, project)
    for name in sorted(os.listdir(gen)):
        if name.endswith(".c"):
            units.append((os.path.join(gen, name), gen_flags))
    target = spec.get("target", "aarch64-linux-android")

    def one(i_unit):
        i, (src, flags) = i_unit
        out = os.path.join(obj, f"{i:04d}-{os.path.basename(src)}.o")
        cmd = [zig, "cc", "-target", target, "-c", *flags, "-o", out, src]
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        return src, out, r.returncode, (r.stdout + r.stderr)

    objects, done, failed = [], 0, []
    progress("compile", 0, len(units))
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for src, out, rc, text in pool.map(one, enumerate(units)):
            done += 1
            if text.strip():
                log(f"{os.path.basename(src)}:\n{text.rstrip()}")
            if rc != 0:
                failed.append(os.path.basename(src))
            objects.append(out)
            progress("compile", done, len(units), os.path.basename(src))
    if failed:
        raise Failure(f"{len(failed)} file(s) did not compile ({', '.join(failed[:3])}); see build.log")
    return objects


def link(spec, objects, libdir, out, zig, env, work, kit):
    progress("link", 0, 1)
    tmp = out + ".partial"
    # Android's own libraries (libm, libdl) as stubs to link against: zig
    # looks for a -l only in the -L folders.
    crt = os.path.join(kit, "sysroot", "usr", "lib", "aarch64-linux-android", "30")
    cmd = [zig, "cc", "-target", spec.get("target", "aarch64-linux-android"), "-shared",
           "-o", tmp, *objects, "-L" + libdir, "-L" + crt, "-lmain",
           "-Wl,-soname,libgame.so", *spec.get("link_flags", [])]
    run(cmd, env=env, what="linking the game")
    os.replace(tmp, out)
    progress("link", 1, 1)


def main():
    global LOG
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--kit", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--game-out", required=True)
    ap.add_argument("--libdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    os.makedirs(a.work, exist_ok=True)
    LOG = open(os.path.join(a.work, "build.log"), "w", buffering=1)
    spec = json.load(open(os.path.join(a.kit, "android-build.json")))
    zig = os.path.join(a.libdir, "libzig.so")
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONUNBUFFERED="1",
               TMPDIR=os.path.join(a.work, "tmp"))
    os.makedirs(env["TMPDIR"], exist_ok=True)
    t0 = time.time()
    try:
        game = game_folder(a.source, a.game_out, os.path.join(a.kit, "toolkit"), spec["xbe"])
        progress("check", 0, 1)
        digest = sha1_of(os.path.join(game, spec["xbe"]))
        image = next((i for i in spec["images"] if i["sha1"] == digest), None)
        if not image:
            raise Failure(f"this {spec['xbe']} (SHA-1 {digest}) is not a copy of "
                          f"{spec['title_name']} this release was made for")
        progress("check", 1, 1, image.get("name", ""))
        project = lift(a.kit, spec, a.work, game, image.get("name", ""), env)
        zenv = zig_env(a.kit, a.work, env)
        objects = compile_all(a.kit, spec, project, a.work, zig, zenv, a.jobs)
        link(spec, objects, a.libdir, a.out, zig, zenv, a.work, a.kit)
        with open(os.path.join(a.work, "stamp.json"), "w") as f:
            json.dump({"source": a.source, "sha1": digest, "kit": spec.get("kit_version"),
                       "game_dir": game, "image": image.get("name", "")}, f)
        log(f"built in {time.time() - t0:.1f} s")
        finish("ok", game_dir=game, image=image.get("name", ""), seconds=round(time.time() - t0, 1))
        return 0
    except Failure as e:
        log(f"FAILED: {e}")
        finish("error", message=str(e))
        return 1
    except Exception as e:  # noqa: BLE001 -- the app shows whatever went wrong
        import traceback
        log(traceback.format_exc())
        finish("error", message=f"{type(e).__name__}: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
