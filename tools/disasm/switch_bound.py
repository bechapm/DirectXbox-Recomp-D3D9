"""How many cases a switch dispatch has, from the bounds check before it.

MSVC guards `jmp dword ptr [reg*4 + table]` with the range test it was
compiled from, straight before the jump:

    cmp  eax, 0x3EE          3D EE 03 00 00     (or 83 F8 ib / 81 F8+r id)
    ja   default             0F 87 rel32        (or 77 rel8)
    jmp  [eax*4 + 0x3C79E0]  FF 24 85 E0 79 3C 00

so the table holds exactly imm + 1 entries. The table readers stop at a fixed
cap (256 in the lifter, 512 in the disassembler) because without this they
have nothing to bound a table but the words themselves; Pro Evolution Soccer
5's sub_003C78B0 maps 1007 codes through one table, and the arms past the cap
were left out, so a code above 255 jumped to an address nothing had lifted.

Only the adjacent form is recognised. A two-level switch (`movzx eax, byte
ptr [eax + B]` between the test and the jump) bounds the byte table, not the
dword one, and is left alone. Returns None when the form is not there.
"""

# Big enough for any real switch, small enough that a misread immediate
# cannot send a reader across the image.
MAX_CASES = 4096


def _jump_index_reg(code):
    """The index register of `FF 24 <SIB> disp32` with scale 4 and no base,
    or None."""
    if len(code) < 3 or code[0] != 0xFF or code[1] != 0x24:
        return None
    sib = code[2]
    if sib >> 6 != 2 or sib & 7 != 5:
        return None
    return (sib >> 3) & 7


def case_count(read, dispatch_va):
    """Entries in the table `jmp [reg*4 + table]` at dispatch_va indexes, from
    the `cmp reg, imm; ja` straight before it. read(va, n) returns n bytes of
    the image at va, or None."""
    code = read(dispatch_va, 3)
    reg = _jump_index_reg(code) if code else None
    if reg is None:
        return None
    for ja_len in (2, 6):
        ja = read(dispatch_va - ja_len, ja_len)
        if not ja:
            continue
        if not ((ja_len == 2 and ja[0] == 0x77) or
                (ja_len == 6 and ja[0] == 0x0F and ja[1] == 0x87)):
            continue
        at = dispatch_va - ja_len
        bound = None
        cmp3 = read(at - 3, 3)
        cmp5 = read(at - 5, 5)
        cmp6 = read(at - 6, 6)
        if cmp3 and cmp3[0] == 0x83 and cmp3[1] == 0xF8 + reg:
            bound = cmp3[2]
            if bound >= 0x80:
                return None          # sign-extended: not a table bound
        elif reg == 0 and cmp5 and cmp5[0] == 0x3D:
            bound = int.from_bytes(cmp5[1:5], "little")
        elif cmp6 and cmp6[0] == 0x81 and cmp6[1] == 0xF8 + reg:
            bound = int.from_bytes(cmp6[2:6], "little")
        if bound is None:
            return None
        count = bound + 1
        return count if 0 < count <= MAX_CASES else None
    return None
