"""
Self-check: FunctionDetector._pass_imm_ref_targets refuses a target that the
code before it runs straight into.

Run: py -3 tools/disasm/test_imm_ref_fall_through.py

Turok: Evolution's D3D writes the push-buffer method word 0x001809E0, which
lands on an instruction inside a loop of a constructor that was still
undetected when the pass ran. The start split the loop, its back edge became a
call to a stub, and the loop body never ran.
"""

import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402
from tools.disasm.test_alias_tail_jumps import _Img  # noqa: E402
from tools.disasm.test_decode_at import _Section  # noqa: E402

BASE = 0x00010000
# add ebx, 0x10; cmp edi, eax; jl back; pop edi; pop ebx; ret
TAIL = b"\x83\xc3\x10" + b"\x3b\xf8" + b"\x7c\xf0" + b"\x5f\x5b\xc3"


def _detector(before, body=TAIL):
    """
    BASE+0x00  the installer: mov dword [esi], BASE+0x40; ret
    BASE+0x40 - len(before)   `before`
    BASE+0x40  `body`, which returns: the returning-body probe accepts it
    """
    code = bytearray(b"\xcc" * 0x100)
    target = BASE + 0x40
    code[0x00:0x06] = b"\xc7\x06" + target.to_bytes(4, "little")
    code[0x06] = 0xc3
    code[0x40 - len(before):0x40] = before
    code[0x40:0x40 + len(body)] = body
    code = bytes(code)
    sec = _Section(BASE, code)
    sec.data = code
    engine = DisasmEngine(_Img(sec))
    engine.linear_sweep(sec)
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = engine
    det.image = engine.image
    det._candidates = {}
    det._alias_entries = {}
    det.functions = {BASE: SimpleNamespace(start=BASE, end=BASE + 7)}
    return det, sec, target


def test_a_target_after_padding_is_found():
    det, sec, target = _detector(b"\xc3" + b"\xcc" * 3)
    det._pass_imm_ref_targets([sec])
    assert target in det._candidates, det._candidates


def test_a_target_code_runs_into_is_not():
    det, sec, target = _detector(b"\xc3\xcc\x47")         # ...; inc edi
    det._pass_imm_ref_targets([sec])
    assert target not in det._candidates, det._candidates


def test_nor_across_alignment_filler():
    det, sec, target = _detector(b"\xc3\x47\x8d\x49\x00")  # inc edi; lea ecx,[ecx+0]
    det._pass_imm_ref_targets([sec])
    assert target not in det._candidates, det._candidates


def test_a_padded_entry_after_data_is_found():
    # BLiNX: a switch's byte index table, which decodes as one-byte
    # instructions that never end, then nops up to an aligned function.
    entry = b"\x8b\x44\x24\x04" + b"\x83\xc0\x10" + b"\xc3"  # mov eax,[esp+4]; add eax,16; ret
    det, sec, target = _detector(b"\xc3" + b"\x9d\x9d\x9e\x9e" + b"\x90" * 4, entry)
    det._pass_imm_ref_targets([sec])
    assert target in det._candidates, det._candidates


def test_but_not_a_loop_head_whose_body_is_no_function():
    # The same padding in front of TAIL, which pops what it never pushed: the
    # middle of a function, as in Turok, and still refused.
    det, sec, target = _detector(b"\xc3" + b"\x9d\x9d\x9e\x9e" + b"\x90" * 4)
    det._pass_imm_ref_targets([sec])
    assert target not in det._candidates, det._candidates


def test_a_target_after_a_call_is_left_alone():
    # The call may not return; a function can follow it directly.
    det, sec, target = _detector(b"\xe8\x00\x00\x00\x00")
    det._pass_imm_ref_targets([sec])
    assert target in det._candidates, det._candidates


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
