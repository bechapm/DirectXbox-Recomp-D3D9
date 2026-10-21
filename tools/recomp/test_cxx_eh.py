"""
C++ catch support: the lifter finds a function's try blocks, catch blocks and
unwind actions in MSVC's FuncInfo, lifts the funclets, and gives the owning
function a catch point that lands on the continuation.

Run: py -3 tools/recomp/test_cxx_eh.py

Max Payne's sub_0003CD80 is the shape built here: `catch (...)` whose block is
`mov eax, <continuation>; ret`, the continuation inside the function, and a
destructor in the unwind map. Lifted without this, a throw it should have
caught returned into the next function in memory and crashed.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.lifter import scan_cxx_eh  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000


def _image():
    """
    0x000 func    push ebp; mov ebp, esp; push -1; push thunk;
                  mov eax, fs:[0]; push eax; mov fs:[0], esp
                  ...; mov eax, 1; jmp epilogue
    0x020 catch   mov eax, cont; ret                  (outside the body's flow)
    0x026 cont    xor eax, eax
    0x028 epi     mov esp, ebp; pop ebp; ret
    0x040 unwind  lea ecx, [ebp - 0x10]; jmp dtor
    0x050 dtor    ret
    0x060 thunk   mov eax, FuncInfo; jmp <anywhere>
    0x080 FuncInfo, unwind map, try map, handler
    """
    img = bytearray(0x100)
    thunk, fi, umap, tmap, hand = 0x60, 0x80, 0xA0, 0xB0, 0xD0
    catch, cont, epi, unwind, dtor = 0x20, 0x26, 0x28, 0x40, 0x50

    code = (bytes.fromhex("558BEC6AFF68") + struct.pack("<I", BASE + thunk)
            + bytes.fromhex("64A10000000050648925000000")
            + bytes.fromhex("B801000000"))
    code += bytes.fromhex("E9") + struct.pack("<i", epi - (len(code) + 5))
    img[0:len(code)] = code
    img[catch:catch + 6] = bytes.fromhex("B8") + struct.pack("<I", BASE + cont) + b"\xC3"
    img[cont:cont + 2] = bytes.fromhex("33C0")
    img[epi:epi + 4] = bytes.fromhex("8BE55DC3")
    jmp_dtor = struct.pack("<i", dtor - (unwind + 8))
    img[unwind:unwind + 8] = bytes.fromhex("8D4DF0E9") + jmp_dtor
    img[dtor] = 0xC3
    img[thunk:thunk + 10] = (bytes.fromhex("B8") + struct.pack("<I", BASE + fi)
                             + bytes.fromhex("E9") + struct.pack("<i", 0))
    img[fi:fi + 20] = struct.pack("<5I", 0x19930520, 1, BASE + umap, 1, BASE + tmap)
    img[umap:umap + 8] = struct.pack("<iI", -1, BASE + unwind)
    img[tmap:tmap + 20] = struct.pack("<iiiiI", 0, 0, 1, 1, BASE + hand)
    img[hand:hand + 16] = struct.pack("<4I", 0, 0, 0, BASE + catch)
    return bytes(img), {"cont": BASE + cont, "catch": BASE + catch,
                        "unwind": BASE + unwind, "func_end": BASE + 0x2C}


def _install(img):
    config._install(
        [config.Section(".text", BASE, len(img), 0x0000, len(img), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="cxx-eh-test")


def test_scan_finds_the_frame():
    img, at = _image()
    _install(img)
    db = {BASE: {"start": f"0x{BASE:08X}", "end": at["func_end"], "_addr": BASE,
                 "size": at["func_end"] - BASE}}
    found = scan_cxx_eh(db, img)
    assert found["owners"] == {BASE: (at["cont"],)}, found["owners"]
    assert found["funclets"].get(at["catch"]) == at["catch"] + 6, found["funclets"]
    assert found["funclets"].get(at["unwind"]) == at["unwind"] + 8, found["funclets"]


def test_owner_lands_on_its_continuation():
    img, at = _image()
    _install(img)
    db = {BASE: {"start": f"0x{BASE:08X}", "end": at["func_end"], "_addr": BASE,
                 "size": at["func_end"] - BASE}}
    ft = FunctionTranslator(img, db)
    ft.cxx_owners = {BASE: (at["cont"],)}
    c = ft.translate_function(BASE, db[BASE])
    name = f"sub_{BASE:08X}"
    assert f"static void {name}_cxx(recomp_cxx_frame *_cxf)" in c, c
    assert "if (RECOMP_CXX_SETJMP(_cxf->jb)) {" in c, c
    assert f"case 0x{at['cont']:08X}u: goto loc_{at['cont']:08X};" in c, c
    assert f"loc_{at['cont']:08X}: ;" in c, c
    assert f"void {name}(void)\n{{\n    recomp_cxx_frame _cxf;" in c, c
    assert f"recomp_cxx_enter(&_cxf, 0x{BASE:08X}u);" in c, c


def test_a_function_without_try_blocks_is_unchanged():
    img, at = _image()
    _install(img)
    db = {BASE: {"start": f"0x{BASE:08X}", "end": at["func_end"], "_addr": BASE,
                 "size": at["func_end"] - BASE}}
    c = FunctionTranslator(img, db).translate_function(BASE, db[BASE])
    assert "recomp_cxx" not in c and f"void sub_{BASE:08X}(void)" in c, c


if __name__ == "__main__":
    test_scan_finds_the_frame()
    test_owner_lands_on_its_continuation()
    test_a_function_without_try_blocks_is_unchanged()
    print("ok")
