"""Capstone x86 decoding without the binding's per-instruction copies.

Capstone's Python binding wraps every instruction in a CsInsn, which copies
the whole cs_insn and the whole cs_detail (a union over every architecture,
about 1.5 KB), and reading .operands then builds a 24-field tuple of x86
detail and copies each operand again. Both pipeline stages read five fields
and the operands and nothing else, and on TimeSplitters 2 that wrapping was
4.4 s of every full pass over its 1.19 million instructions, against 0.95 s
for Capstone's own decoder.

disasm() takes the same arguments as Cs.disasm and yields objects with the
attributes the stages read -- address, size, mnemonic, op_str, bytes,
operands, and on each operand type, imm, reg, size and mem (segment, base,
index, scale, disp) -- read straight from the array cs_disasm returns, with
the same values the binding gives. It uses the binding's own ctypes
definitions, so the layout always matches the installed library; if those
internal names are ever missing, it falls back to Cs.disasm itself.
"""

import ctypes

from capstone import CS_ERR_OK, CS_OP_IMM, CS_OP_MEM, CsError

def _binding_has_layout():
    """Whether the binding still has the internals disasm() reads."""
    try:
        from capstone import _cs, _cs_insn
        detail = dict(_cs_insn._fields_)["detail"]._type_
        arch = dict(detail._fields_)["arch"]
        return ("x86" in dict(arch._fields_)
                and all(hasattr(_cs, f) for f in ("cs_disasm", "cs_free", "cs_errno")))
    except (ImportError, AttributeError, KeyError, TypeError):
        return False


_FAST = _binding_has_layout()
if _FAST:
    from capstone import _cs, _cs_insn


class Mem:
    __slots__ = ("segment", "base", "index", "scale", "disp")

    def __init__(self, segment, base, index, scale, disp):
        self.segment = segment
        self.base = base
        self.index = index
        self.scale = scale
        self.disp = disp


class Op:
    """One operand. imm and reg are the union read both ways, as the
    binding's are; mem is None unless the operand is a memory one."""
    __slots__ = ("type", "imm", "reg", "size", "mem")

    def __init__(self, type, imm, reg, size, mem):
        self.type = type
        self.imm = imm
        self.reg = reg
        self.size = size
        self.mem = mem


class Insn:
    __slots__ = ("address", "size", "mnemonic", "op_str", "bytes", "operands")

    def __init__(self, address, size, mnemonic, op_str, code, operands):
        self.address = address
        self.size = size
        self.mnemonic = mnemonic
        self.op_str = op_str
        self.bytes = code
        self.operands = operands


def _operands(raw):
    x86 = raw.detail.contents.arch.x86
    ops = []
    for op in x86.operands[:x86.op_count]:
        value = op.value
        mem = None
        if op.type == CS_OP_MEM:
            m = value.mem
            mem = Mem(m.segment, m.base, m.index, m.scale, m.disp)
        ops.append(Op(op.type, value.imm, value.reg, op.size, mem))
    return ops


def disasm(cs, code, offset, count=0):
    """What cs.disasm(code, offset, count) yields, as Insn objects."""
    if not _FAST:
        yield from cs.disasm(code, offset, count)
        return

    all_insn = ctypes.POINTER(_cs_insn)()
    size = len(code)
    view = memoryview(code)
    if not view.readonly:
        code = ctypes.byref(ctypes.c_char.from_buffer(view))
    elif not isinstance(code, bytes):
        code = view.tobytes()
    res = _cs.cs_disasm(cs.csh, code, size, offset, count, ctypes.byref(all_insn))
    if res <= 0:
        status = _cs.cs_errno(cs.csh)
        if status != CS_ERR_OK:
            raise CsError(status)
        return

    detail = cs._detail
    try:
        for i in range(res):
            raw = all_insn[i]          # a view into the array, not a copy
            n = raw.size
            yield Insn(raw.address, n,
                       raw.mnemonic.decode("ascii"), raw.op_str.decode("ascii"),
                       bytes(raw.bytes[:n]),
                       _operands(raw) if detail and raw.id else [])
    finally:
        _cs.cs_free(all_insn, res)
