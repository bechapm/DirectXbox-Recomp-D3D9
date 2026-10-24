"""
Self-check for fast_decode.disasm against Capstone's own binding.

Run: py -3 tools/disasm/test_fast_decode.py

Both pipeline stages decode through fast_decode, which reads cs_disasm's array
directly instead of letting the binding copy each instruction. Every value it
yields has to be the one the binding would have given, or the lifted C
changes. These compare the two on instructions chosen for the fields that are
easy to get wrong -- a segment override, a negative displacement, a negative
immediate, a scaled index -- and on a stretch of random bytes, which also
exercises Capstone stopping at an invalid opcode.
"""

import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402

from tools.disasm import fast_decode  # noqa: E402

BASE = 0x00011000


def _cs(detail=True):
    cs = Cs(CS_ARCH_X86, CS_MODE_32)
    cs.detail = detail
    return cs


def _key(insn, detail=True):
    ops = []
    for op in (insn.operands if detail else ()):
        mem = op.mem if op.type == 3 else None
        ops.append((op.type, op.imm, op.reg, op.size,
                    None if mem is None else
                    (mem.segment, mem.base, mem.index, mem.scale, mem.disp)))
    return (insn.address, insn.size, insn.mnemonic, insn.op_str,
            bytes(insn.bytes), ops)


def _same(code, detail=True):
    cs = _cs(detail)
    want = [_key(i, detail) for i in cs.disasm(code, BASE)]
    got = [_key(i, detail) for i in fast_decode.disasm(cs, code, BASE)]
    assert got == want, (code.hex(), got, want)
    return got


def test_the_fast_path_is_in_use():
    # Without it every check below compares the binding with itself.
    assert fast_decode._FAST


def test_operand_fields_match_the_binding():
    got = _same(bytes.fromhex(
        "64a100000000"          # mov eax, fs:[0]           segment override
        "8b45f8"                # mov eax, [ebp - 8]        negative disp
        "6aff"                  # push -1                   negative imm
        "ff2485e0793c00"        # jmp [eax*4 + 0x3c79e0]    scaled index
        "e8fbffffff"            # call $                    relative target
        "c20800"))              # ret 8
    assert [g[2] for g in got] == ["mov", "mov", "push", "jmp", "call", "ret"]


def test_without_detail_there_are_no_operands():
    got = _same(bytes.fromhex("8b45f8c3"), detail=False)
    assert all(g[5] == [] for g in got)


def test_random_bytes_match_the_binding():
    rng = random.Random(0x4553000A)
    code = bytes(rng.randrange(256) for _ in range(4096))
    off = 0
    while off < len(code):
        got = _same(code[off:])
        off += 1 if not got else got[-1][0] - BASE + got[-1][1]


def test_a_stop_part_way_frees_and_resumes():
    cs = _cs()
    code = bytes.fromhex("9090909090c3")
    for insn in fast_decode.disasm(cs, code, BASE):
        break
    assert [_key(i) for i in fast_decode.disasm(cs, code, BASE)] == \
        [_key(i) for i in cs.disasm(code, BASE)]


def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("  ok  %s" % fn.__name__)
    print("%d checks passed" % len(fns))


if __name__ == "__main__":
    _run()
