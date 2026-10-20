"""
A switch longer than the table reader's cap is read to its bounds check.

Run: py -3 tools/recomp/test_switch_bound.py

Pro Evolution Soccer 5's sub_003C78B0 is `cmp eax, 0x3EE; ja; jmp [eax*4 +
table]` over 1007 entries. The lifter read 256 of them, so a code above 255
jumped to an arm nothing had lifted ("[ICALL] unresolved jump target
0x003C796E"). The `cmp` says how long the table is.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.switch_bound import case_count  # noqa: E402
from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000


def _reader(image, base=BASE):
    def read(va, n):
        o = va - base
        if o < 0 or o + n > len(image):
            return None
        return image[o:o + n]
    return read


def test_case_count_forms():
    jmp_eax = bytes.fromhex("FF2485") + struct.pack("<I", 0x1000)
    jmp_ecx = bytes.fromhex("FF248D") + struct.pack("<I", 0x1000)
    cases = [
        (bytes.fromhex("3DEE030000") + bytes.fromhex("0F87") + b"\0" * 4 + jmp_eax, 1007),
        (bytes.fromhex("83F82A") + bytes.fromhex("772A") + jmp_eax, 43),
        (bytes.fromhex("81F92B010000") + bytes.fromhex("772A") + jmp_ecx, 300),
        # A test on another register bounds something else.
        (bytes.fromhex("83F92A") + bytes.fromhex("772A") + jmp_eax, None),
        # No ja: not a bounds check.
        (bytes.fromhex("83F82A") + bytes.fromhex("742A") + jmp_eax, None),
        # imm8 >= 0x80 is sign-extended, so not a count.
        (bytes.fromhex("83F890") + bytes.fromhex("772A") + jmp_eax, None),
    ]
    for code, want in cases:
        at = BASE + len(code) - 7
        got = case_count(_reader(code), at)
        assert got == want, (code.hex(), got, want)


def _translate(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="switch-bound-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    ft = FunctionTranslator(image, db)
    return ft.translate_function(BASE, db[BASE])


def test_a_table_past_the_cap_is_read_whole():
    """
    0x00  8B 44 24 04          mov eax, [esp+4]
    0x04  3D 2B 01 00 00       cmp eax, 299
    0x09  0F 87 <rel32>        ja default
    0x0F  FF 24 85 <table>     jmp [eax*4 + table]
    0x16  300 arms             mov eax, imm32; ret
    ....  default              xor eax, eax; ret
    ....  table                300 entries
    """
    n = 300
    head = bytes.fromhex("8B442404") + bytes.fromhex("3D") + struct.pack("<I", n - 1)
    arms_at = 0x16
    default_at = arms_at + 6 * n
    table_at = default_at + 3
    ja = bytes.fromhex("0F87") + struct.pack("<i", default_at - (len(head) + 6))
    jmp = bytes.fromhex("FF2485") + struct.pack("<I", BASE + table_at)
    code = head + ja + jmp
    assert len(code) == arms_at
    for k in range(n):
        code += bytes.fromhex("B8") + struct.pack("<I", 0x100 + k) + bytes.fromhex("C3")
    code += bytes.fromhex("33C0C3")
    arms = [BASE + arms_at + 6 * k for k in range(n)]
    code += b"".join(struct.pack("<I", a) for a in arms)
    c = _translate(code)
    assert f"switch: {n} entries" in c, c[:2000]
    for arm in (arms[0], arms[255], arms[256], arms[-1]):
        assert f"if (_jt == 0x{arm:08X}u)" in c, hex(arm)


if __name__ == "__main__":
    test_case_count_forms()
    test_a_table_past_the_cap_is_read_whole()
    print("ok")
