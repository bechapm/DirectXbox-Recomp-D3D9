"""A function that begins right after a ret, with no padding between.

_pass_cc_boundaries only sees a boundary when the compiler left int3 padding
to the next alignment. When the next function already starts on the boundary
there is no padding, and a clean prologue sits immediately after the previous
function's ret with nothing marking it. It is not a call target if it is only
reached through a vtable, and the prologue pass looks for "push ebp; mov ebp,
esp", which an FPO function like "sub esp, 0x18" does not have.

MSVC parks out-of-line tails after a ret too, and from here they look
identical. The difference is that a tail belongs to the function above it, so
_find_function_end has already covered it -- it is not in a gap. Restricting
to gaps separates the two by construction. Half-Life 2 has 20 of these: 12 in
gaps, 8 tails.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402


class _Insn:
    def __init__(self, addr, size, is_ret=False, jump_target=None):
        self.address = addr
        self.size = size
        self.end_address = addr + size
        self.is_ret = is_ret
        self.jump_target = jump_target
        self.call_target = None


class _Section:
    executable = True


class _Func:
    def __init__(self, start, end):
        self.start = start
        self.end = end


class _Image:
    def __init__(self, memory=None):
        self.memory = memory or {}     # va -> bytes, for read_bytes_at_va

    def get_section_at_va(self, addr):
        return _Section()

    def read_bytes_at_va(self, addr, n):
        for base, data in self.memory.items():
            if base <= addr and addr + n <= base + len(data):
                return data[addr - base:addr - base + n]
        return None


class _Engine:
    def __init__(self, insns, prologues, jump_tables=None, bodies=()):
        self.instructions = {i.address: i for i in insns}
        self._prologues = set(prologues)
        self._stub = False
        self.jump_tables = dict(jump_tables or {})
        self._bodies = set(bodies)

    def probes_as_prologue(self, addr):
        return addr in self._prologues

    def probes_as_constant_stub(self, addr):
        return self._stub

    def probes_as_function_body(self, addr):
        return addr in self._bodies

    def decode_at(self, addr):
        self.decoded = getattr(self, "decoded", []) + [addr]
        return 0


def _detector(insns, functions, prologues, jump_tables=None, memory=None,
              bodies=()):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns, prologues, jump_tables, bodies)
    det.image = _Image(memory)
    det.functions = {f.start: f for f in functions}
    det._candidates = {}
    det._alias_entries = {}
    det.added = []
    det._add_candidate = lambda addr, conf, why: det.added.append((addr, why))
    return det


class GapPrologueTest(unittest.TestCase):
    def test_prologue_in_a_gap_after_a_ret_is_a_function(self):
        # sub_00476EA0 ends with a ret at 0x00476EAF; 0x00476EB0 is unclaimed.
        insns = [_Insn(0x00476EAF, 1, is_ret=True)]
        funcs = [_Func(0x00476EA0, 0x00476EB0), _Func(0x004771C0, 0x004771D0)]
        det = _detector(insns, funcs, prologues={0x00476EB0})
        self.assertTrue(det._pass_gap_prologues([]))
        self.assertIn((0x00476EB0, "gap_prologue"), det.added)

    def test_an_out_of_line_tail_is_not_split(self):
        # Same shape, but the enclosing function's body already covers it.
        insns = [_Insn(0x00476EAF, 1, is_ret=True)]
        funcs = [_Func(0x00476EA0, 0x00476F00)]
        det = _detector(insns, funcs, prologues={0x00476EB0})
        self.assertFalse(det._pass_gap_prologues([]))
        self.assertEqual(det.added, [])

    def test_bytes_that_are_neither_a_prologue_nor_a_stub_are_ignored(self):
        insns = [_Insn(0x00476EAF, 1, is_ret=True)]
        funcs = [_Func(0x00476EA0, 0x00476EB0), _Func(0x004771C0, 0x004771D0)]
        det = _detector(insns, funcs, prologues=set())
        det.engine._stub = False
        self.assertFalse(det._pass_gap_prologues([]))

    def test_a_constant_stub_counts_even_without_a_prologue(self):
        """"mov eax, <imm32>; ret" has no frame to recognise."""
        insns = [_Insn(0x00476EAF, 1, is_ret=True)]
        funcs = [_Func(0x00476EA0, 0x00476EB0), _Func(0x004771C0, 0x004771D0)]
        det = _detector(insns, funcs, prologues=set())
        det.engine._stub = True
        self.assertTrue(det._pass_gap_prologues([]))
        self.assertIn((0x00476EB0, "gap_prologue"), det.added)

    def test_a_constant_stub_inside_a_function_is_still_not_split(self):
        # The same two instructions occur as a return path in larger
        # functions; the gap test is what keeps them intact.
        insns = [_Insn(0x00476EAF, 1, is_ret=True)]
        funcs = [_Func(0x00476EA0, 0x00476F00)]
        det = _detector(insns, funcs, prologues=set())
        det.engine._stub = True
        self.assertFalse(det._pass_gap_prologues([]))

    def test_a_non_ret_instruction_starts_nothing(self):
        insns = [_Insn(0x00476EAF, 1, is_ret=False)]
        funcs = [_Func(0x00476EA0, 0x00476EB0), _Func(0x004771C0, 0x004771D0)]
        det = _detector(insns, funcs, prologues={0x00476EB0})
        self.assertFalse(det._pass_gap_prologues([]))


class SwitchTableTest(unittest.TestCase):
    """The bytes after a switch's function, from Future Perfect at 0x001C4D8A.

    The function above ends with a ret at 0x001C4D89. Then: 8B FF, which
    aligns the dword table and is also the hot-patch prologue; six case
    pointers; a byte table of indices into them, every one below six; int3
    padding; and the next function, a level callback reached only through a
    pointer built at run time, at 0x001C4DC0.
    """
    RET = 0x001C4D89
    TABLE = (0x001C4D8C, 0x001C4DA4)
    NEXT = 0x001C4DC0
    BYTES = bytes.fromhex(
        "8bff"
        "f74c1c00844d1c00354d1c00254d1c004a4d1c00224d1c00"
        "0005050501050502050505050505050503"
        "04" "cccccccccccccccccccc"
        "8b442404")

    def _det(self, prologues=(), bodies=()):
        insns = [_Insn(self.RET, 1, is_ret=True)]
        funcs = [_Func(0x001C4CE0, self.RET + 1), _Func(0x001C4E50, 0x001C4F35)]
        return _detector(insns, funcs, prologues=set(prologues),
                         jump_tables=dict([self.TABLE]),
                         memory={self.RET + 1: self.BYTES}, bodies=bodies)

    def test_the_tables_alignment_is_not_a_prologue(self):
        # 8B FF probes as a prologue -- it is one, when it is not padding.
        det = self._det(prologues={self.RET + 1})
        det._pass_gap_prologues([])
        self.assertNotIn((self.RET + 1, "gap_prologue"), det.added)

    def test_the_function_after_the_tables_is_found(self):
        det = self._det(bodies={self.NEXT})
        self.assertTrue(det._pass_gap_prologues([]))
        self.assertEqual(det.added, [(self.NEXT, "after_switch_table")])
        # Decoded from its own start: the sweep came through the index bytes
        # as code and need not be in step at the function.
        self.assertEqual(det.engine.decoded, [self.NEXT])

    def test_nothing_is_added_where_the_bytes_do_not_decode(self):
        det = self._det(bodies=set())
        self.assertFalse(det._pass_gap_prologues([]))

    def test_bytes_no_function_starts_with_are_refused(self):
        # After the table, bytes that are no index table (0x0D >= 6 entries)
        # but are `or eax, imm32` and `add` opcodes all the same: data of some
        # other kind, or the tail of an index table, not a function -- even
        # though they decode far enough to probe as a body.
        junk_at = self.TABLE[1]
        memory = {self.RET + 1: bytes.fromhex("8bff") + self.BYTES[2:26]
                  + bytes.fromhex("0d0d0d0d0d05c3")}
        det = _detector([_Insn(self.RET, 1, is_ret=True)],
                        [_Func(0x001C4CE0, self.RET + 1)], prologues=set(),
                        jump_tables=dict([self.TABLE]), memory=memory,
                        bodies={junk_at})
        self.assertFalse(det._pass_gap_prologues([]))
        self.assertEqual(det.added, [])

    def test_a_second_switchs_table_is_not_a_function(self):
        # Future Perfect 0x001C53FE: the index bytes run straight into the
        # next table's 8B FF alignment.
        tail = bytes.fromhex("0005050501050505050505050502030505" "04")             + bytes.fromhex("8bff") + bytes(24)
        start = 0x001C5410
        det = _detector([], [], prologues=set(),
                        jump_tables={0x001C53F8: start,
                                     start + len(tail) - 24: start + len(tail)},
                        memory={start: tail}, bodies={start + len(tail) - 26})
        det._pass_gap_prologues([])
        self.assertEqual(det.added, [])


class _Code:
    """.text, for the code-pointer test beside a data word."""
    name = ".text"
    virtual_addr = 0x00011000
    virtual_size = 0x00300000


class BranchTargetAfterTableTest(unittest.TestCase):
    """Constantine 0x0004E490: an index table ends straight into a function.

    The switch has more cases than "56" and "8B" are worth, so the bytes of
    "push esi; mov esi, ecx" read as index bytes by value alone. 0x0004E490 is
    a tail-jump target, which no index table can be.
    """
    TABLE = (0x0004E220, 0x0004E220 + 4 * 160)
    INDEX = TABLE[1]
    FUNC = INDEX + 4
    BYTES = bytes.fromhex("01020304" "56" "8bf1" "8b4604")

    def _det(self, insns, aliases=None):
        det = _detector(insns, [_Func(0x0004E100, 0x0004E21D)], prologues=set(),
                        jump_tables=dict([self.TABLE]),
                        memory={self.INDEX: self.BYTES},
                        bodies={self.FUNC, self.FUNC + 2})
        det._alias_entries = dict(aliases or {})
        return det

    def test_a_jump_target_ends_the_index_bytes(self):
        det = self._det([_Insn(0x0004E110, 5, jump_target=self.FUNC)])
        det._pass_gap_prologues([])
        # The function itself, not the F1 in the middle of its mov.
        self.assertEqual(det.added, [(self.FUNC, "after_switch_table")])

    def test_an_alias_entry_does_not_end_the_index_bytes(self):
        # Aliases can be the sweep's own junk (Constantine 0x00140010).
        det = self._det([], aliases={self.FUNC: self.FUNC + 0x1C})
        det._pass_gap_prologues([])
        self.assertEqual(det.added, [(self.FUNC + 2, "after_switch_table")])

    def test_a_vtable_slot_ends_the_index_bytes(self):
        # How 0x0004E490 is actually reached: only through a vtable in .rdata.
        import struct

        class _Rdata:
            name = ".rdata"
            executable = True           # as an XBE flags it

        det = self._det([])
        det.image.sections = [_Rdata()]
        det.image.get_section_data = lambda sec: struct.pack("<3I", 0x0004E000,
                                                              self.FUNC, 0x0004E600)
        det._pass_gap_prologues([_Code()])
        self.assertEqual(det.added, [(self.FUNC, "after_switch_table")])

    def test_a_lone_data_word_does_not(self):
        # A word that names an index byte with no code pointer beside it is a
        # coincidence (Constantine 0x00140010): the walk goes on.
        import struct

        class _Rdata:
            name = ".rdata"
            executable = True

        det = self._det([])
        det.image.sections = [_Rdata()]
        det.image.get_section_data = lambda sec: struct.pack("<3I", 7, self.FUNC, 9)
        det._pass_gap_prologues([_Code()])
        self.assertEqual(det.added, [(self.FUNC + 2, "after_switch_table")])

    def test_a_jump_decoded_from_the_index_bytes_does_not(self):
        # The sweep decodes index bytes as code; a jump from there is junk.
        det = self._det([_Insn(self.INDEX, 2, jump_target=self.FUNC)])
        det._pass_gap_prologues([])
        self.assertEqual(det.added, [(self.FUNC + 2, "after_switch_table")])

    def test_without_either_the_walk_runs_on(self):
        # What happened before: the walk takes 56 and 8B as indices and
        # starts a function at the F1 inside the mov.
        det = self._det([])
        det._pass_gap_prologues([])
        self.assertEqual(det.added, [(self.FUNC + 2, "after_switch_table")])


class SehPrologueShapeTest(unittest.TestCase):
    """A function whose frame __SEH_prolog builds has no prologue to find.

        push <frame size>
        push <scope table>
        call __SEH_prolog

    None of the usual shapes match, because the helper does "push ebp; mov
    ebp, esp" on the caller's behalf. Half-Life 2 has one at 0x001F572B
    reached only through a vtable: unresolved, the call was skipped rather
    than made, and the arguments already pushed for it stayed on the stack --
    which shifted the caller's frame so its "pop ebx" restored the wrong slot.
    """
    import os as _os
    import sys as _sys

    def _probe(self, code):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        from tools.disasm.engine import DisasmEngine

        class _Sec:
            virtual_addr = 0x001F5000
            virtual_size = 0x1000
            executable = True

        class _Img:
            def get_section_at_va(self, addr):
                return _Sec()

            def get_section_data(self, sec):
                return bytes([0x90]) * 0x72B + code

        eng = DisasmEngine.__new__(DisasmEngine)
        eng.image = _Img()
        eng._cs = Cs(CS_ARCH_X86, CS_MODE_32)
        eng._cs.detail = True
        return eng.probes_as_prologue(0x001F572B)

    def test_seh_prologue_is_a_prologue(self):
        # push 0x18 ; push 0x00772760 ; call rel32
        code = bytes.fromhex("6a18") + bytes.fromhex("6860277700")              + bytes.fromhex("e800000000")
        self.assertTrue(self._probe(code))

    def test_two_pushes_without_a_call_are_not(self):
        code = bytes.fromhex("6a18") + bytes.fromhex("6860277700")              + bytes.fromhex("90909090")
        self.assertFalse(self._probe(code))

    def test_a_single_immediate_push_is_not(self):
        code = bytes.fromhex("6a18") + bytes.fromhex("c3") + bytes([0x90]) * 8
        self.assertFalse(self._probe(code))

