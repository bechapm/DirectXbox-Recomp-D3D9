#!/usr/bin/env python3
"""
Is this branch worse than its base? An A/B regression pass over the titles.

    py -3 scripts/regress.py <change-ref> [--base origin/main] [--titles a,b,...]

Two persistent worktrees, games/_regress/base and games/_regress/change, are
moved to the two refs. Each title is lifted only as far back as the diff
requires and built incrementally, one title at a time, then run on both sides
-- one driven and one idle run per side, titles in parallel -- and the result
is a table plus a frame comparison.

Why it is built this way. A pass used to take one to two hours, and almost
none of that was the runs. Measured on 3 Oct 2026:

  * Fresh worktrees meant cold builds. Moving a built worktree to the next
    commit leaves every untouched generated chunk compiled: a runtime-only
    change relinked six titles in 47 s, a lifter change (#159) lifted and
    rebuilt them in 8.5 min, against an hour cold.
  * Several titles built at once fought over the CPU. Each title already
    compiles its chunks in parallel (/MP), so two to four at a time ran 80
    compilers on 20 threads: TimeSplitters 2 took 748 s that way and 221 s
    alone. So builds here are sequential; only the runs are parallel.
  * Most changes do not need a full lift. A src/ change needs none, and a
    translator change only the last stage (40-90 s, against 4-9 min).
    Each work dir remembers which commit produced its stages (stamp.json),
    and the start stage is chosen from the diff since then.

Every run gets a fresh RECOMP_SAVE_DIR and reads the game through a mirror
(asset folders linked, TDATA/CACHE copied, UDATA never), so the player's saves
and game folders are never touched. Launcher settings a player tuned
(%APPDATA%/xboxrecomp/titles/*.conf) are pinned back to defaults through the
environment, which wins over the file.

Nothing runs while a title is already running: that is someone playing.

Off Windows (macOS, Linux) the same pass works with three differences: the
builds are Ninja into titles/<title>/build/Release/, every run is muted and
in the background (compat_matrix.background_env, the switches the agents'
run-title.sh uses), and the runs go one title at a time unless --jobs says
otherwise. --build-prefix wraps each build command (e.g. a build-slot script
and `nice`), so a pass shares the machine politely.
"""

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))
import compat_matrix as cm  # noqa: E402  (run_title, discover, default_input_seq)


def _main_checkout():
    """The checkout that owns .git: the game folders live only there, and the
    regress area is shared by whichever worktree runs this script."""
    r = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                       cwd=ROOT, capture_output=True, text=True)
    return Path(r.stdout.strip()).parent if r.returncode == 0 else ROOT


MAIN = _main_checkout()
GAMES = MAIN / "games"
REGRESS = GAMES / "_regress"
MIRROR = REGRESS / "mirror"
SIDES = ("base", "change")

DEFAULT_TITLES = ["timesplitters2", "futureperfect", "outrun2", "black",
                  "mvc2", "mkda", "burnout2"]

# Third-party trees a title build needs. Submodules and the two unpacked
# toolchains are not part of a worktree checkout, so they are linked in from
# this checkout.
THIRD_PARTY = ["SDL", "volk", "Vulkan-Headers", "VulkanMemoryAllocator",
               "XbSymbolDatabase", "dxc", "ffmpeg", "libsamplerate", "miniupnp"]

# Which paths force which pipeline stage. "parse" reruns everything.
STAGE_PATHS = {
    "parse": ["tools/xbe_parser"],
    "disasm": ["tools/disasm", "tools/func_id", "tools/xdk_symbols",
               "config/seeds", "scripts/recompile.py"],
    "lift": ["tools/recomp", "templates", "src/hle", "config"],
}

# Launcher settings back to defaults (recomp_config_lookup: the env wins).
PIN_ENV = {"RECOMP_RES_SCALE": "1", "RECOMP_ANISO": "1", "RECOMP_WIDESCREEN": "0",
           "RECOMP_FPS_OVERLAY": "0", "RECOMP_FRAME_INTERP": "0", "RECOMP_VRR": "0",
           "RECOMP_FULLSCREEN": "0", "RECOMP_MUTE": "1"}
# OutRun 2 hangs at the AM2 logo on several cores without this.
TITLE_ENV = {"outrun2": {"RECOMP_GUEST_ONE_CPU": "1"}}

FPS_RE = re.compile(r"\[FPS\] t=\s*([\d.]+)s\s+([\d.]+) fps \((\d+) swaps\)")


def log(msg):
    print(f"[regress {time.strftime('%H:%M:%S')}] {msg}", flush=True)


def git(*args, cwd=ROOT, check=True):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit(f"git {' '.join(args)} failed in {cwd}:\n{r.stderr.strip()}")
    return r.stdout.strip()


def title_running():
    if os.name != "nt":
        # A title is started by path, so its command line has a "/" right
        # before "<name>_recomp"; a build's "--target <name>_recomp" does not.
        r = subprocess.run(["pgrep", "-f", r"/[A-Za-z0-9]+_recomp( |$)"],
                           capture_output=True, text=True)
        return r.returncode == 0 and bool(r.stdout.strip())
    out = subprocess.run(["tasklist"], capture_output=True, text=True).stdout
    return "_recomp.exe" in out


def check_nobody_playing(run_lock, why):
    """Stop if someone is playing. With --run-lock, a title that runs while
    the lock is held is a scripted run, and every run of this pass takes the
    same lock (as the agents' run-title.sh does), so the pass carries on and
    its runs queue behind it; a title running with the lock free is a person."""
    if not title_running():
        return
    if run_lock and Path(run_lock).exists():
        log("a scripted title run holds the run lock; this pass's runs will wait for it")
        return
    sys.exit(why)


# ── worktrees ──────────────────────────────────────────────────────────────

def _is_link(p):
    return p.is_symlink() or (hasattr(os.path, "isjunction") and os.path.isjunction(p))


def unlink_third_party(wt):
    """Remove the links (never their targets). git cannot run in a worktree
    while they exist: SDL's .git file points at a module path that is not
    there."""
    for name in THIRD_PARTY:
        p = wt / "third_party" / name
        if _is_link(p):
            if os.name == "nt":
                os.rmdir(p)          # removes a junction, not what it points at
            else:
                p.unlink()


def link_third_party(wt):
    for name in THIRD_PARTY:
        src, dst = MAIN / "third_party" / name, wt / "third_party" / name
        if not src.is_dir() or _is_link(dst):
            continue
        if dst.is_dir():
            if any(dst.iterdir()):
                continue             # a real checkout is there; leave it
            dst.rmdir()
        if os.name == "nt":
            subprocess.run(["cmd", "/c", "mklink", "/J", str(dst), str(src)],
                           check=True, capture_output=True)
        else:
            dst.symlink_to(src, target_is_directory=True)


def move_worktree(side, ref):
    """Put games/_regress/<side> at ref, creating it the first time."""
    wt = REGRESS / side
    sha = git("rev-parse", "--verify", f"{ref}^{{commit}}")
    if not wt.exists():
        REGRESS.mkdir(parents=True, exist_ok=True)
        git("worktree", "add", "--detach", str(wt), sha)
    else:
        unlink_third_party(wt)
        if git("rev-parse", "HEAD", cwd=wt) != sha:
            dirty = [l for l in git("status", "--porcelain", "--untracked-files=no",
                                    "--ignore-submodules=all", cwd=wt).splitlines()
                     if not l[3:].startswith("third_party/")]
            if dirty:
                raise SystemExit(f"{wt} has local changes; not moving it:\n" + "\n".join(dirty))
            git("checkout", "--detach", sha, cwd=wt)
    link_third_party(wt)
    return wt, sha


# ── lift and build ─────────────────────────────────────────────────────────

def work_dir(wt, title):
    return wt / "games" / "_pipeline" / "_regress" / title


def start_stage(wt, sha, title, force):
    """The earliest stage whose inputs changed since the stamped commits."""
    stamp_path = work_dir(wt, title) / "stamp.json"
    if force or not stamp_path.is_file():
        return "parse"
    stamp = json.loads(stamp_path.read_text())
    since = {"parse": stamp.get("parse"), "disasm": stamp.get("disasm"),
             "lift": stamp.get("lift")}
    for stage in ("parse", "disasm", "lift"):
        paths = list(STAGE_PATHS[stage])
        if stage == "lift":
            paths.append(f"titles/{title}/src/recomp_manual.c")
        if not since[stage]:
            return stage
        if git("diff", "--name-only", since[stage], sha, "--", *paths, cwd=wt):
            return stage
    return None


def lift(wt, sha, t, stage):
    work = work_dir(wt, t["name"]) / "out"
    work.mkdir(parents=True, exist_ok=True)
    xbe = t["game_dir"] / "default.xbe"
    cmd = [sys.executable, str(wt / "scripts" / "recompile.py"), str(xbe),
           "--json", str(work / "default_analysis.json"),
           "--work-dir", str(work), "--project", str(wt / "titles" / t["name"])]
    if stage != "parse":
        cmd += ["--from", stage]
    t0 = time.time()
    with open(work.parent / "lift.log", "w", encoding="utf-8", errors="replace") as f:
        rc = subprocess.run(cmd, cwd=wt, stdout=f, stderr=subprocess.STDOUT).returncode
    if rc == 0:
        stamp_path = work.parent / "stamp.json"
        stamp = json.loads(stamp_path.read_text()) if stamp_path.is_file() else {}
        for s in ("parse", "disasm", "lift")[("parse", "disasm", "lift").index(stage):]:
            stamp[s] = sha
        stamp_path.write_text(json.dumps(stamp, indent=1))
    return rc, time.time() - t0


def build(wt, name, prefix=()):
    """Incremental build; configures only the first time (the projects glob
    their generated sources with CONFIGURE_DEPENDS)."""
    project = wt / "titles" / name
    bdir = cm.build_dir(project)
    logs = work_dir(wt, name)
    logs.mkdir(parents=True, exist_ok=True)
    cmake = cm.find_cmake()
    t0 = time.time()
    with open(logs / "build.log", "w", encoding="utf-8", errors="replace") as f:
        if not (bdir / "CMakeCache.txt").is_file():
            if os.name == "nt":
                cfg = [cmake, "-S", str(project), "-B", str(bdir),
                       "-G", "Visual Studio 16 2019", "-A", "x64"]
            else:
                cfg = cm.configure_cmd(project)
            rc = subprocess.run([*prefix, *cfg],
                                stdout=f, stderr=subprocess.STDOUT).returncode
            if rc:
                return rc, time.time() - t0
        rc = subprocess.run([*prefix, *cm.build_cmd(project, name)],
                            stdout=f, stderr=subprocess.STDOUT).returncode
    return rc, time.time() - t0


# ── game mirrors and runs ──────────────────────────────────────────────────

def ensure_mirror(game_dir):
    dst = MIRROR / game_dir.name
    dst.mkdir(parents=True, exist_ok=True)
    for e in sorted(game_dir.iterdir()):
        target = dst / e.name
        if target.exists() or _is_link(target):
            continue
        if os.name != "nt":
            # A clone, not a link: Partition1 maps to the game folder, so a
            # link would let a run write into the real one. Copy-on-write
            # (cm.clone_path) makes the whole folder free to copy.
            if not cm.is_player_data(e.name):
                cm.clone_path(e, target)
            continue
        if e.is_dir():
            if e.name.upper().startswith("UDATA"):
                continue                     # the player's saves: never
            if e.name.upper() in ("TDATA", "CACHE"):
                shutil.copytree(e, target)   # written by the title: a copy
            elif os.name == "nt":
                subprocess.run(["cmd", "/c", "mklink", "/J", str(target), str(e)],
                               check=True, capture_output=True)
            else:
                target.symlink_to(e, target_is_directory=True)
        else:
            shutil.copy2(e, target)
    return dst


def reset_mirror(game_dir, debris):
    """Whatever the last run wrote goes to debris (never deleted); TDATA and
    CACHE start from the real folder's copy."""
    m = MIRROR / game_dir.name
    for name in ("UDATA", "TDATA", "CACHE"):
        p = m / name
        if p.exists():
            debris.mkdir(parents=True, exist_ok=True)
            p.rename(debris / name)
    for name in ("TDATA", "CACHE"):
        src = game_dir / name
        if src.is_dir():
            if os.name != "nt":
                cm.clone_path(src, m / name)
            else:
                shutil.copytree(src, m / name)


def run_once(side, wt, t, mode, seconds, out_root, run_lock=None):
    name = t["name"]
    tag = f"{side}-{mode}-{name}-{time.strftime('%H%M%S')}"
    out_dir = out_root / side / mode / name
    out_dir.mkdir(parents=True, exist_ok=True)
    save = out_root / "saves" / tag
    save.mkdir(parents=True, exist_ok=True)
    reset_mirror(t["game_dir"], out_root / "debris" / tag)
    env = dict(PIN_ENV, RECOMP_GAME_DIR=str(MIRROR / t["game_dir"].name),
               RECOMP_SAVE_DIR=str(save), **TITLE_ENV.get(name, {}))
    spec = {"name": name, "project": wt / "titles" / name,
            "exe": cm.title_exe(wt / "titles" / name, name),
            "game_dir": MIRROR / t["game_dir"].name, "fresh_saves": False,
            "run_lock": run_lock,
            "input_seq": cm.default_input_seq(seconds) if mode == "driven" else None}
    s = cm.run_title(spec, seconds, out_dir, env)
    text = (out_dir / f"{name}.err").read_text(encoding="utf-8", errors="replace") \
        if (out_dir / f"{name}.err").is_file() else ""
    live = [float(f) for _t, f, n in FPS_RE.findall(text) if int(n) > 0]
    s["fps"] = round(sum(live) / len(live), 1) if live else None
    s["saves_isolated"] = "[PATH] saves in" in text and str(save) in text
    s["unresolved"] = sorted(set(re.findall(
        r"\[ICALL\] (?:unknown|unresolved (?:call |jump )?) ?target (0x[0-9A-Fa-f]+)", text)))[:5]
    s["frames"] = str(out_dir / "frames")
    s["audio"] = cm.audio_health(text)
    return s


def audio_worse(b, c):
    """Whether the change's sound is worse than the base's: it went wrong
    where the base was right, or it hears a smaller share of the voices it
    starts (more than one voice's worth), or it refuses sound the base did
    not."""
    if c is None:
        return b is not None
    if cm.audio_verdict(c) != "ok" and (b is None or cm.audio_verdict(b) == "ok"):
        return True
    if b is None:
        return False

    def share(a):
        return a["buffers_heard"] / a["buffers_used"] if a["buffers_used"] else 1.0
    used = max(b["buffers_used"], c["buffers_used"], 1)
    if share(c) < share(b) - 1.0 / used:
        return True
    return c["refused"] > 0 and b["refused"] == 0


def run_title_chain(t, sides, seconds, out_root, run_lock=None):
    """One driven and one idle run per side, alternating sides."""
    res = {}
    for mode, secs in (("driven", seconds), ("idle", seconds + 5)):
        for side in SIDES:
            res[(side, mode)] = run_once(side, sides[side], t, mode, secs, out_root,
                                         run_lock)
    for side in SIDES:
        reset_mirror(t["game_dir"], out_root / "debris" / f"{side}-end-{t['name']}")
    return t["name"], res


def frame_diff(a_dir, b_dir, name):
    """Mean |difference| (0-255) of the numbered captures, pairwise by index."""
    a = sorted(Path(a_dir).glob(f"{name}[0-9]*.bmp"))
    b = sorted(Path(b_dir).glob(f"{name}[0-9]*.bmp"))
    out = []
    for x, y in zip(a, b):
        bx, by = x.read_bytes()[54:], y.read_bytes()[54:]
        if len(bx) != len(by) or not bx:
            out.append(None)
            continue
        sx, sy = bx[::97], by[::97]
        out.append(round(sum(abs(p - q) for p, q in zip(sx, sy)) / len(sx), 1))
    return out


# ── main ───────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("change", help="branch, tag or commit to test")
    ap.add_argument("--base", default="origin/main", help="what to compare against")
    ap.add_argument("--titles", default=",".join(DEFAULT_TITLES))
    ap.add_argument("--seconds", type=int, default=70)
    ap.add_argument("--force-lift", action="store_true",
                    help="full lift on both sides, ignoring the stamps")
    ap.add_argument("--no-run", action="store_true", help="lift and build only")
    ap.add_argument("--jobs", type=int, default=None,
                    help="titles run at once (default: all of them on Windows, "
                         "one at a time elsewhere)")
    ap.add_argument("--build-prefix", default="",
                    help="a command each configure and build runs under, e.g. "
                         "'nice' or a build-slot script; split like a shell")
    ap.add_argument("--run-lock", metavar="DIR",
                    help="a lock directory each run holds (mkdir), so only one "
                         "game runs at a time on this machine")
    args = ap.parse_args()
    prefix = shlex.split(args.build_prefix)
    jobs = args.jobs or None

    check_nobody_playing(args.run_lock,
                         "A title is running -- someone is playing. Not starting.")
    wanted = [n.strip() for n in args.titles.split(",") if n.strip()]
    titles = cm.discover(wanted)
    for t in titles:                       # game data lives in the main checkout
        t["game_dir"] = GAMES / t["game"] if t["game"] else None
    titles = [t for t in titles if t["game_dir"] and t["game_dir"].is_dir()]
    missing = sorted(set(wanted) - {t["name"] for t in titles})
    if missing:
        log(f"skipped (no project or no game folder): {', '.join(missing)}")
    if not titles:
        sys.exit("no titles to run")

    sides, shas = {}, {}
    for side, ref in (("base", args.base), ("change", args.change)):
        sides[side], shas[side] = move_worktree(side, ref)
        log(f"{side}: {ref} at {shas[side][:9]}")
    changed = git("diff", "--name-only", shas["base"], shas["change"])
    log(f"{len(changed.splitlines())} files differ between the sides")

    timings = {}
    for t in titles:
        for side in SIDES:
            wt, sha = sides[side], shas[side]
            stage = start_stage(wt, sha, t["name"], args.force_lift)
            lt = 0.0
            if stage:
                rc, lt = lift(wt, sha, t, stage)
                if rc:
                    log(f"{side} {t['name']}: lift from {stage} FAILED "
                        f"(see {work_dir(wt, t['name']) / 'lift.log'})")
                    continue
            rc, bt = build(wt, t["name"], prefix)
            timings[(side, t["name"])] = (stage, lt, bt, rc)
            log(f"{side} {t['name']}: lift {'from ' + stage if stage else 'none'}"
                f" {lt:.0f}s, build {bt:.0f}s{' FAILED' if rc else ''}")
    if args.no_run:
        return

    check_nobody_playing(args.run_lock,
                         "A title started while building -- someone is playing. Not running.")
    out_root = REGRESS / "runs" / time.strftime("%Y%m%d-%H%M%S")
    out_root.mkdir(parents=True)
    for t in titles:
        ensure_mirror(t["game_dir"])
    log(f"running {len(titles)} titles x 4 runs into {out_root}")
    t0 = time.time()
    results = {}
    if jobs is None:
        jobs = len(titles) if os.name == "nt" else 1
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
        for name, res in ex.map(lambda t: run_title_chain(t, sides, args.seconds, out_root,
                                                          args.run_lock),
                                titles):
            results[name] = res
    log(f"runs took {time.time() - t0:.0f}s")

    lines = [f"# Regression: {args.change} ({shas['change'][:9]}) vs {args.base} "
             f"({shas['base'][:9]})", "",
             "| title | mode | side | verdict | swaps | fps | lit | motion | crashed | unresolved "
             "| audio |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]
    worse = []
    for name, res in results.items():
        for mode in ("driven", "idle"):
            b, c = res[("base", mode)], res[("change", mode)]
            for side, s in (("base", b), ("change", c)):
                lines.append(f"| {name} | {mode} | {side} | {s['verdict']} | {s['swaps']} | "
                             f"{s['fps']} | {s['lit']} | {s['motion']} | "
                             f"{'yes ' + str(s['exit']) if s['crashed'] else 'no'} | "
                             f"{' '.join(s['unresolved']) or '-'} | "
                             f"{cm.audio_cell(s.get('audio'))} |")
                if not s["saves_isolated"]:
                    log(f"WARNING {name} {side} {mode}: no [PATH] line naming its own save "
                        f"dir -- this build may have used the player's saves")
            if (c["crashed"] and not b["crashed"]) or (b["swaps"] and not c["swaps"]) \
                    or (c["unresolved"] and not b["unresolved"]) \
                    or audio_worse(b.get("audio"), c.get("audio")):
                worse.append(f"{name} {mode}")
    lines += ["", "Frame difference by capture index (mean |diff| 0-255; timing shifts "
              "show as isolated spikes, a rendering change as a run of them):", ""]
    for name, res in results.items():
        for mode in ("driven", "idle"):
            d = frame_diff(res[("base", mode)]["frames"], res[("change", mode)]["frames"], name)
            lines.append(f"- {name} {mode}: {' '.join('-' if x is None else str(x) for x in d)}")
    lines += ["", "Build: " + ", ".join(
        f"{s} {n} {'from ' + st if st else 'no lift'} {lt:.0f}+{bt:.0f}s"
        for (s, n), (st, lt, bt, _rc) in timings.items())]
    lines += ["", ("**Worse on:** " + ", ".join(worse)) if worse else
              "No title is worse on crashes, swaps, unresolved calls or audio "
              "(one run per side and mode cannot see an intermittent fault)."]
    report = "\n".join(lines)
    (out_root / "report.md").write_text(report, encoding="utf-8")
    (out_root / "results.json").write_text(json.dumps(
        {n: {f"{k[0]}-{k[1]}": v for k, v in r.items()} for n, r in results.items()},
        indent=1, default=str), encoding="utf-8")
    print(report)


if __name__ == "__main__":
    main()
