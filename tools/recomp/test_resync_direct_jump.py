"""
Self-check: a direct jump into bytes a linear decode swallowed is re-synced.

Run: py -3 tools/recomp/test_resync_direct_jump.py

The translator re-decoded only at switch arms. A direct `jmp` (or jcc) to the
code just past an inline table lands on bytes the table's linear decode ran
over, so its target never became a block: no label, and the goto was dropped
as dead code. Upstream met it in Dead or Alive 3 (Japan 3.1), whose function
at 0x72F50 lost its whole phase-5 case that way, so character select never
started a match (sp00nznet/xboxrecomp#275, NoRain211). The two images below
are upstream's: the smallest shapes that show it, and the second also shows
that the restart evicts the stale decodes inside the real instruction.

The third is the guard: a branch decoded from data nothing reaches must not
force a re-decode, or table bytes could tear up real code.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000


def _translator(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="resync-direct-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db), db


def _translate(image):
    ft, db = _translator(image)
    seen = {}
    real = ft.disasm.disassemble_function

    def spy(raw, start, end, resync=None):
        insns = real(raw, start, end, resync=resync)
        seen["insns"], seen["resync"] = insns, set(resync or ())
        return insns

    ft.disasm.disassemble_function = spy
    code = ft.translate_function(BASE, db[BASE])
    return code, seen["insns"], seen["resync"]


def test_direct_jump_past_inline_data_is_resynced():
    # jmp +4, then data that decodes as `mov eax, imm32` and swallows the ret
    # the jump lands on (DOA3 JP 0x72FB8 -> 0x72FE0, past a jump table).
    image = bytes.fromhex("e904000000b8112233c3")
    target = BASE + 9
    code, insns, resync = _translate(image)
    assert target in resync, resync
    assert insns[-1].address == target and insns[-1].mnemonic == "ret", insns
    assert f"loc_{target:08X}:" in code, code
    assert f"goto loc_{target:08X};" in code, code
    print("ok  direct_jump_past_inline_data_is_resynced")


def test_resync_evicts_stale_decode_inside_real_instruction():
    # jmp to BASE+7; the data `c1 e0` decodes as shl eax, imm8 and swallows the
    # a1 of `mov eax, [imm32]` at BASE+7, leaving stale sbb/lds decodes inside
    # it (DOA3 JP 0x18A210, where the leftover lds ran after the real mov).
    image = bytes.fromhex("e902000000c1e0a11c8ec500c3")
    code, insns, _ = _translate(image)
    assert [(i.address, i.mnemonic) for i in insns] == [
        (BASE, "jmp"), (BASE + 7, "mov"), (BASE + 12, "ret")], insns
    assert "lds" not in code and "sbb" not in code, code
    print("ok  resync_evicts_stale_decode_inside_real_instruction")


def test_unreachable_branch_in_data_forces_nothing():
    # ret, then data that decodes as a jmp into the middle of the data after
    # it. Nothing reaches that jmp, so its target must not be resynced.
    #   +0  c3            ret
    #   +1  eb 01         jmp +1  -> +4, inside the next instruction
    #   +3  b8 c3 c3 c3 c3  mov eax, imm32
    image = bytes.fromhex("c3eb01b8c3c3c3c3")
    _, insns, resync = _translate(image)
    assert BASE + 4 not in resync, resync
    assert (BASE + 3) in {i.address for i in insns}, insns
    print("ok  unreachable_branch_in_data_forces_nothing")


def test_jump_into_a_reachable_instruction_forces_nothing():
    # The target is inside an instruction the function does execute. That is
    # not swallowed table data, and re-decoding there would tear up code that
    # runs; it is what data mistaken for code looks like (TimeSplitters 2 has
    # a stretch of it, and chasing its targets only exposed more).
    #   +0  74 01           je +1  -> +3, inside the mov below
    #   +2  b8 c3 00 00 00  mov eax, 0xC3   (reached by falling through)
    #   +7  c3              ret
    image = bytes.fromhex("7401b8c3000000c3")
    _, insns, resync = _translate(image)
    assert BASE + 3 not in resync, resync
    assert [(i.address, i.mnemonic) for i in insns] == [
        (BASE, "je"), (BASE + 2, "mov"), (BASE + 7, "ret")], insns
    print("ok  jump_into_a_reachable_instruction_forces_nothing")


if __name__ == "__main__":
    test_direct_jump_past_inline_data_is_resynced()
    test_resync_evicts_stale_decode_inside_real_instruction()
    test_unreachable_branch_in_data_forces_nothing()
    test_jump_into_a_reachable_instruction_forces_nothing()
    print("\nall passed")
