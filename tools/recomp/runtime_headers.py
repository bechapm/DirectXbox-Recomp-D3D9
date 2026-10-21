"""The runtime headers that travel with generated code, and which file needs which.

templates/runtime/recomp_types.h is the register model every generated file
includes. Its SSE/XMM/MMX half lives in recomp_types_simd.h, which
recomp_types.h includes unless RECOMP_NO_SIMD is defined first. Most chunks
use none of it -- 3 of TimeSplitters 2's 28 do -- so a generated file that
names nothing the SIMD header defines says so with RECOMP_NO_SIMD, and a
change to that header rebuilds only the files that use it.

The names are read from the header itself, so a helper added there is
covered here without a second list to keep in step. A wrong answer cannot
pass silently: a file that claims RECOMP_NO_SIMD and then uses a SIMD helper
fails to compile (an undeclared helper is an error in every build).
"""

import functools
import os
import re
import sys

RUNTIME_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..",
                                            "templates", "runtime"))

# Copied into gen/ beside the generated code, in this order. recomp_cpu.h is
# the register file recomp_types.h includes; titles' recomp_manual.c includes
# the gen/ copy too, so it always matches the code it was lifted with.
RUNTIME_HEADERS = ("recomp_types.h", "recomp_types_simd.h", "recomp_cpu.h",
                   "recomp_cxx.h")
SIMD_HEADER = "recomp_types_simd.h"

NO_SIMD_DEFINE = "#define RECOMP_NO_SIMD"

_C_KEYWORDS = frozenset(("if", "for", "while", "switch", "return", "sizeof",
                         "do", "else", "case", "default", "defined"))


@functools.lru_cache(maxsize=None)
def simd_names(header_path=None):
    """Every identifier recomp_types_simd.h defines: macros, functions,
    generated helpers, types and the register variables."""
    path = header_path or os.path.join(RUNTIME_DIR, SIMD_HEADER)
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    names = set(re.findall(r"^\s*#\s*define\s+(\w+)", text, re.M))
    # static inline <type> NAME(args) {   (args may span lines)
    names.update(re.findall(r"\b(\w+)\s*\([^;{}()]*\)\s*\{", text))
    # helpers built by the generator macros: RECOMP_XMM_LANEWISE(XMM_ADD, ...)
    names.update(re.findall(r"^\s*RECOMP_\w+\(\s*(\w+)\s*,", text, re.M))
    names.update(re.findall(r"\btypedef\s+union\s+(\w+)", text))
    names.update(re.findall(r"^\}\s*(\w+)\s*;", text, re.M))
    for decl in re.findall(r"^\s*extern\s+[^;(]*?\b(?:\w+)\s+((?:\w+\s*,\s*)*\w+)\s*;",
                           text, re.M):
        names.update(n.strip() for n in decl.split(","))
    names -= _C_KEYWORDS
    # A generator macro's body looks like a definition of its parameter:
    # `#define RECOMP_XMM_LANEWISE(name, expr) static inline RecompXmm name(...) {`
    for params in re.findall(r"^\s*#\s*define\s+\w+\(([^)]*)\)", text, re.M):
        names -= {p.strip() for p in params.split(",")}
    names -= {"RECOMP_TYPES_SIMD_H", "RECOMP_XMM_DEFINED", "RECOMP_MMX_DEFINED"}
    return frozenset(names)


# Most SIMD names share one of these, so a scan looks for them with str.find
# (C speed) and checks the identifier around each hit; a name with none of
# them is looked for by itself. A regex alternation of every name took 5-9 s
# per title, which is a real share of a lift.
_ANCHORS = ("mm", "MM", "Mm", "cvt", "sat_")


@functools.lru_cache(maxsize=None)
def _simd_anchors(header_path=None):
    names = simd_names(header_path)
    extra = sorted(n for n in names if not any(a in n for a in _ANCHORS))
    return tuple(_ANCHORS) + tuple(extra)


def _is_ident(ch):
    return ch.isalnum() or ch == "_"


def uses_simd(text, header_path=None):
    """True if `text` (generated C) names anything recomp_types_simd.h defines."""
    names = simd_names(header_path)
    n = len(text)
    for anchor in _simd_anchors(header_path):
        i = text.find(anchor)
        while i != -1:
            s, e = i, i + len(anchor)
            while s > 0 and _is_ident(text[s - 1]):
                s -= 1
            while e < n and _is_ident(text[e]):
                e += 1
            if text[s:e] in names:
                return True
            i = text.find(anchor, e)
    return False


def simd_opt_out(text):
    """The lines to put before the runtime include of a generated file whose
    body is `text`: the RECOMP_NO_SIMD define if it needs no SIMD, else none."""
    return [] if uses_simd(text) else [NO_SIMD_DEFINE]


def refresh_runtime_headers(output_dir, out=sys.stderr):
    """Copy the runtime headers into output_dir, writing only those that differ.

    The lifter and these headers are two halves of one contract, so every lift
    refreshes them (translator.py says why at length). Writing only a header
    that changed keeps an unchanged one's mtime, so the build does not
    recompile what did not change. Returns the names written.
    """
    written = []
    for name in RUNTIME_HEADERS:
        src = os.path.join(RUNTIME_DIR, name)
        dst = os.path.join(output_dir, name)
        try:
            with open(src, encoding="utf-8") as fh:
                want = fh.read()
            have = None
            if os.path.exists(dst):
                with open(dst, encoding="utf-8") as fh:
                    have = fh.read()
            if have != want:
                with open(dst, "w", encoding="utf-8") as fh:
                    fh.write(want)
                written.append(name)
                print("  %s %s (runtime register model)"
                      % ("refreshed" if have is not None else "wrote", name),
                      file=out)
        except OSError as exc:
            print(f"  WARNING: could not write {name} ({exc}); copy it from "
                  f"templates/runtime/ by hand or the build will not find it",
                  file=out)
    return written
