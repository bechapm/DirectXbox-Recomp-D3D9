"""
One-operand mul/imul at the operand's width.

Run: py -3 tools/recomp/test_narrow_multiply.py

x86 multiplies AL by a byte into AX, AX by a word into DX:AX and EAX by a
dword into EDX:EAX. Every form was lifted as the dword one, so `imul bl`
overwrote edx. Pro Evolution Soccer 5's libpng keeps the image height in edx
across `imul bl` (pixel depth = channels * bit depth), and png_set_IHDR got 0.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000


def _translate(code):
    config._install(
        [config.Section(".text", BASE, len(code), 0x0000, len(code), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="narrow-multiply-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(code),
                 "_addr": BASE, "size": len(code)}}
    return FunctionTranslator(code, db).translate_function(BASE, db[BASE])


def _body(c):
    return "\n".join(line for line in c.splitlines()
                     if not line.lstrip().startswith(("/*", "*", "//")))


def test_byte_imul_leaves_edx():
    # imul bl ; ret
    c = _body(_translate(bytes.fromhex("F6EB" "C3")))
    assert "SET_LO16(eax," in c, c
    assert "(int8_t)" in c, c
    assert "edx =" not in c, c


def test_byte_mul_is_unsigned_and_leaves_edx():
    # mul cl ; ret
    c = _body(_translate(bytes.fromhex("F6E1" "C3")))
    assert "SET_LO16(eax," in c, c
    assert "(int8_t)" not in c, c
    assert "edx =" not in c, c


def test_word_imul_writes_dx_ax_only():
    # imul cx ; ret
    c = _body(_translate(bytes.fromhex("66F7E9" "C3")))
    assert "SET_LO16(eax, _r)" in c and "SET_LO16(edx, _r >> 16)" in c, c


def test_dword_forms_unchanged():
    # imul ecx ; mul ecx ; ret
    c = _body(_translate(bytes.fromhex("F7E9" "F7E1" "C3")))
    assert "int64_t _r = (int64_t)(int32_t)eax" in c, c
    assert "uint64_t _r = (uint64_t)eax" in c, c
    assert c.count("edx = (uint32_t)(_r >> 32)") == 2, c


if __name__ == "__main__":
    test_byte_imul_leaves_edx()
    test_byte_mul_is_unsigned_and_leaves_edx()
    test_word_imul_writes_dx_ax_only()
    test_dword_forms_unchanged()
    print("ok")
