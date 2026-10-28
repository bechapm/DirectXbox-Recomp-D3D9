"""Run a lifted flag setter and every consumer after it on this host, against x86.

The lifter's flag rules are prose about the SDM turned into C strings, and a
string can be read and found plausible while being wrong at one width or one
operand shape. This compiles what the lifter actually emits -- the setter and a
jcc after it, through lift_basic_block, against the real recomp_types.h -- and
sweeps it over operand values, comparing each branch with what the SDM says the
flags are. Tests in this directory build their cases on it; it has no tests of
its own because every one of theirs is one.

Three things are deliberate:

  * The registers start with their upper bits dirty, so a lifting that reads
    or writes 32 bits where it should touch 8 or 16 cannot pass by accident.
  * The vectors reach the lifted code through an array, never as literals. A
    constant the compiler can fold hides undefined behaviour in the emitted C:
    test_lifter_sf_overflow.py has the measured case.
  * A flag the SDM leaves undefined is not checked. The reference returns None
    for it and every condition that reads it is skipped for that vector.

Nothing here is specific to one fix. A case is (instruction, operands, jcc), and
`reference` knows the flags of the integer setters by name.
"""
import os
import random
import shutil
import subprocess
import tempfile
from pathlib import Path

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block

_ROOT = Path(__file__).resolve().parents[2]

START, TAKEN = 0x10000, 0x10100

# Every condition, in the order the expected-outcome bitmask uses.
JCCS = ("jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja",
        "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg")

# The flags each condition reads.
_READS = {"jo": "O", "jno": "O", "jb": "C", "jae": "C", "je": "Z", "jne": "Z",
          "jbe": "CZ", "ja": "CZ", "js": "S", "jns": "S", "jp": "P",
          "jnp": "P", "jl": "SO", "jge": "SO", "jle": "ZSO", "jg": "ZSO"}

REG = {1: ("al", "bl", "cl"), 2: ("ax", "bx", "cx"), 4: ("eax", "ebx", "ecx")}
EAX_DIRT, EBX_DIRT, ECX_DIRT, EDX_INIT = 0xA5A50000, 0x5A5A0000, 0x3C3C0000, 0x5A5A5A5A


def reg(name):
    return Operand(type="reg", reg=name)


def imm(value):
    return Operand(type="imm", imm=value & 0xFFFFFFFF)


def _parity(r):
    return int(bin(r & 0xFF).count("1") % 2 == 0)


def _sx(v, w):
    bits = w * 8
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def reference(m, w, a, b, cf_in):
    """(result, flags) after `m dst, src` per the SDM, or None if not modelled.

    a is the destination's value, b the source's (or the count, for a shift),
    both already at width w. flags maps C/Z/S/O/P to 0, 1 or None (undefined).
    mul and imul write edx:eax; their result is returned as (low, high).
    """
    bits = w * 8
    mask = (1 << bits) - 1
    top = 1 << (bits - 1)
    a &= mask

    def szp(r):
        return {"Z": int(r == 0), "S": int(bool(r & top)), "P": _parity(r)}

    if m in ("add", "adc"):
        b &= mask
        c = cf_in if m == "adc" else 0
        full = a + b + c
        r = full & mask
        of = int((a & top) == (b & top) and (r & top) != (a & top))
        return r, dict(szp(r), C=int(full > mask), O=of)
    if m in ("sub", "sbb", "cmp"):
        b &= mask
        c = cf_in if m == "sbb" else 0
        r = (a - b - c) & mask
        of = int((a & top) != (b & top) and (r & top) != (a & top))
        return (a if m == "cmp" else r), dict(szp(r), C=int(a < b + c), O=of)
    if m in ("and", "or", "xor"):
        b &= mask
        r = {"and": a & b, "or": a | b, "xor": a ^ b}[m]
        return r, dict(szp(r), C=0, O=0)
    if m == "neg":
        r = (-a) & mask
        return r, dict(szp(r), C=int(a != 0), O=int(a == top))
    if m in ("shl", "shr", "sar"):
        c = b & 31
        if c == 0:
            # Nothing changes, CF included. The others are not "undefined",
            # they are the previous setter's -- which here is no setter.
            return a, {"C": cf_in, "Z": None, "S": None, "O": None, "P": None}
        if m == "shl":
            r = (a << c) & mask
            cf = (a >> (bits - c)) & 1 if c <= bits else None
            of = ((r >> (bits - 1)) ^ cf) if c == 1 else None
        elif m == "shr":
            r = a >> c
            cf = (a >> (c - 1)) & 1 if c <= bits else None
            of = (a >> (bits - 1)) if c == 1 else None
        else:
            s = _sx(a, w)
            r = (s >> c) & mask
            cf = (s >> (c - 1)) & 1
            of = 0 if c == 1 else None
        return r, dict(szp(r), C=cf, O=of)
    if m in ("mul", "imul"):
        b &= mask
        full = _sx(a, w) * _sx(b, w) if m == "imul" else a * b
        lo = full & mask
        fits = (_sx(lo, w) == full) if m == "imul" else (full >> bits) == 0
        hi = (full >> bits) & mask
        flags = {"C": int(not fits), "O": int(not fits),
                 "Z": None, "S": None, "P": None}
        return (lo, hi), flags
    if m == "imul2":
        b &= mask
        full = _sx(a, w) * _sx(b, w)
        lo = full & mask
        fits = _sx(lo, w) == full
        return lo, {"C": int(not fits), "O": int(not fits),
                    "Z": None, "S": None, "P": None}
    return None


def taken(jcc, f):
    """Whether jcc branches on flags f, or None if it reads an undefined one."""
    if any(f.get(flag) is None for flag in _READS[jcc]):
        return None
    c, z, s, o, p = f.get("C"), f.get("Z"), f.get("S"), f.get("O"), f.get("P")
    return {"jo": o, "jno": not o, "jb": c, "jae": not c, "je": z,
            "jne": not z, "jbe": c or z, "ja": not c and not z,
            "js": s, "jns": not s, "jp": p, "jnp": not p,
            "jl": s != o, "jge": s == o, "jle": z or s != o,
            "jg": not z and s == o}[jcc]


def lift_with_jcc(mnemonic, operands, jcc, needs_cf=True, lifter=None):
    """The C statements for `mnemonic operands; jcc TAKEN`, as one block."""
    lifter = lifter or Lifter()
    lifter.func_start, lifter.func_end = START, START + 0x1000
    lifter.needs_cf = needs_cf
    setter = Instruction(START, 3, mnemonic, "", "", operands=list(operands))
    branch = Instruction(START + 3, 2, jcc, "", "", jump_target=TAKEN)
    stmts, _ = lift_basic_block(
        lifter, BasicBlock(start=START, instructions=[setter, branch]))
    return stmts


def condition_of(stmts):
    """The emitted jcc line (the last statement of a lift_with_jcc block)."""
    return stmts[-1]


def edge_values(w):
    """Operand values that sit on every flag boundary at width w."""
    bits = w * 8
    mask = (1 << bits) - 1
    top = 1 << (bits - 1)
    vals = {0, 1, 2, 3, 0x7F, 0x80, 0x81, 0xFF, top - 1, top, top + 1,
            mask - 1, mask, 0x55 & mask, 0xAA & mask}
    return sorted(v & mask for v in vals)


def value_pairs(w, extra=0, seed=1):
    """Every pair of edge values, plus `extra` random pairs."""
    vals = edge_values(w)
    pairs = [(a, b) for a in vals for b in vals]
    rng = random.Random(seed * 1000 + w)
    mask = (1 << (w * 8)) - 1
    pairs += [(rng.randrange(mask + 1), rng.randrange(mask + 1))
              for _ in range(extra)]
    return pairs


PRELUDE_FOR_TESTS = _PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS struct recomp_cpu g_cpu;
uint8_t guest[0x1000];
uint32_t eax, ecx, edx, ebx, esi, edi, ebp, esp;
uint32_t _fa, _fb; int32_t _fas, _fbs; int _cf; int _flags;

/* One vector: the operands, the carry in, the conditions that must branch
 * (a bit per JCCS entry), which of them the SDM defines, and eax/edx after. */
struct vec { uint32_t a, b; int cf; uint32_t want, defined, eax, edx; };
'''


def find_cc():
    return shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")


def compile_and_run(source, opt="-O2"):
    """Build and run a C program against templates/runtime. (rc, output)."""
    cc = find_cc()
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "t.c")
        exe = os.path.join(tmp, "t")
        with open(src, "w") as f:
            f.write(source)
        built = subprocess.run(
            [cc, "-std=gnu11", opt, "-w", "-fwrapv",
             "-I", str(_ROOT / "templates" / "runtime"), src, "-o", exe, "-lm"],
            capture_output=True, text=True)
        if built.returncode != 0:
            raise AssertionError(built.stderr[-3000:])
        ran = subprocess.run([exe], capture_output=True, text=True)
        return ran.returncode, ran.stdout + ran.stderr


def sweep(cases, opt="-O2"):
    """Run each case over its vectors. Returns (rc, output); rc 0 is a pass.

    A case is a dict:
      name     -- a C identifier
      stmts    -- {jcc: [C statements]} from lift_with_jcc, one per condition
      vectors  -- [(a, b, cf_in)] ; a seeds the destination register's low
                  bits, b the source's (ebx or ecx family), cf_in seeds _cf
      expect   -- function (a, b, cf_in) -> (result, flags) at the case's width
      width    -- operand width in bytes
      dest     -- "eax" family register name the result lands in, and
      wide     -- True for mul/imul, whose result is edx:eax
    """
    parts = [_PRELUDE]
    mains = []
    for case in cases:
        name, w = case["name"], case["width"]
        mask = (1 << (w * 8)) - 1
        rows = []
        for a, b, cf in case["vectors"]:
            result, flags = case["expect"](a, b, cf)
            want_bits = defined = 0
            for k, jcc in enumerate(JCCS):
                t = taken(jcc, flags)
                if t is not None and jcc in case["stmts"]:
                    defined |= 1 << k
                    want_bits |= int(bool(t)) << k
            if case.get("wide"):
                lo, hi = result
                if w == 1:
                    want_eax = (EAX_DIRT & ~0xFFFF) | (hi << 8) | lo
                    want_edx = EDX_INIT
                else:
                    want_eax = (EAX_DIRT & ~mask) | lo
                    want_edx = (EDX_INIT & ~mask) | hi
            else:
                want_eax = (EAX_DIRT & ~mask) | (result & mask)
                want_edx = EDX_INIT
            rows.append(f"{{0x{a:X}u, 0x{b:X}u, {cf}, 0x{want_bits:X}u, "
                        f"0x{defined:X}u, 0x{want_eax:08X}u, 0x{want_edx:08X}u}}")
        parts.append(f"static const struct vec {name}_v[] = {{\n  "
                     + ",\n  ".join(rows) + "\n};\n")
        for k, jcc in enumerate(JCCS):
            # A condition the case leaves out still runs the setter (under
            # some other condition), so the value written is checked anyway.
            stmts = case["stmts"].get(jcc) or next(iter(case["stmts"].values()))
            body = "\n    ".join(stmts)
            parts.append(f"static int {name}_{jcc}(void) {{\n    {body}\n"
                         f"    return 0;\nloc_{TAKEN:08X}: return 1;\n}}\n")
        fns = ", ".join(f"{name}_{jcc}" for jcc in JCCS)
        mains.append(f"""
    {{
        static int (*const fn[])(void) = {{ {fns} }};
        for (unsigned i = 0; i < sizeof {name}_v / sizeof {name}_v[0]; i++) {{
            const struct vec *v = &{name}_v[i];
            for (unsigned k = 0; k < {len(JCCS)}; k++) {{
                eax = 0x{EAX_DIRT:08X}u & ~0x{mask:X}u; eax |= v->a;
                ebx = 0x{EBX_DIRT:08X}u & ~0x{mask:X}u; ebx |= v->b;
                ecx = 0x{ECX_DIRT:08X}u & ~0x{mask:X}u; ecx |= v->b;
                edx = 0x{EDX_INIT:08X}u; _cf = v->cf; _flags = 0;
                int got = fn[k]();
                if ((v->defined >> k) & 1u) {{
                    int want = (int)((v->want >> k) & 1u);
                    if (got != want && failures++ < 20)
                        printf("{name} %s a=0x%X b=0x%X cf=%d: got %d want %d\\n",
                               names[k], v->a, v->b, v->cf, got, want);
                }}
                if ((eax != v->eax || edx != v->edx) && failures++ < 20)
                    printf("{name} a=0x%X b=0x%X cf=%d: eax=%08X edx=%08X"
                           " want %08X %08X\\n", v->a, v->b, v->cf,
                           eax, edx, v->eax, v->edx);
            }}
        }}
    }}""")
    names = ", ".join(f'"{j}"' for j in JCCS)
    parts.append("int main(void) {\n    int failures = 0;\n"
                 f"    static const char *const names[] = {{ {names} }};\n"
                 + "".join(mains)
                 + '\n    if (failures) printf("%d failure(s)\\n", failures);'
                 + '\n    else printf("OK\\n");'
                 + "\n    return failures ? 1 : 0;\n}\n")
    return compile_and_run("".join(parts), opt=opt)


def case(name, mnemonic, operands, width, vectors, expect, wide=False,
         needs_cf=True, lifter_factory=Lifter, jccs=JCCS):
    """A sweep case for `mnemonic operands` followed by each of `jccs`.

    A condition left out is not run; the value written still is."""
    return {
        "name": name, "width": width, "vectors": vectors, "expect": expect,
        "wide": wide,
        "stmts": {jcc: lift_with_jcc(mnemonic, operands, jcc,
                                     needs_cf=needs_cf,
                                     lifter=lifter_factory())
                  for jcc in jccs},
    }
