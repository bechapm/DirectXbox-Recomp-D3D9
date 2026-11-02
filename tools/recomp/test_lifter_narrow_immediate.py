"""A narrow add/sub/adc/sbb reads its immediate at the destination's width.

Capstone sign-extends the imm8 of the 83 /r forms to the operand size and the
disassembler stores it as 32 bits, so `add ax, -1` (66 83 C0 FF) reaches the
lifter as 0xFFFFFFFF. The store truncates and the value written was right. The
carry was not: add computed it as bit 16 of (uint64_t)ax + 0xFFFFFFFF, which
is 0 for every ax but 0 (x86 says 1 for every ax but 0), and sub compared ax
against 0xFFFFFFFF, so it borrowed for every value. adc and sbb carried out
of bit 32 the same way.

Upstream found it with fpdiff (sp00nznet/xboxrecomp#278, NoRain211):
`int_add_m116_jb` failed on all 1,344 of its vectors. The checks below compile
the lifter's own output and sweep it over every 8-bit value and the edges of
the 16-bit range, with the immediates Capstone actually produces, against the
SDM's carry, zero and sign. The negative control runs the same sweep with the
source read as it was before, and requires it to fail -- otherwise the sweep
would not be testing the thing it is named for. OF and PF after these are
test_lifter_overflow_parity.py's.
"""
import unittest
from unittest import mock

from . import lifter as lifter_mod
from .flag_sweep import (REG, case, edge_values, find_cc, imm, lift_with_jcc,
                         reference, reg, sweep)
from .lifter import _fmt_operand_read

# Carry, zero and sign: the conditions this fix changes or must keep.
CONDS = ("jb", "jae", "jbe", "ja", "je", "jne", "js", "jns")

# (immediate as the disassembler stores it, what it means at width 1 and 2)
IMMS = (0xFFFFFFFF,   # `add ax, -1`: Capstone's -1, masked to 32 bits
        0xFFFFFF80,   # `sub ax, -0x80`
        0x00000001)


def _vectors(m, w):
    alist = range(256) if w == 1 else edge_values(2) + [0x1234, 0x8001, 0xFFFE]
    cfs = (0, 1) if m in ("adc", "sbb") else (0,)
    return [(a, 0, cf) for a in alist for cf in cfs]


def _cases(prefix=""):
    out = []
    for m in ("add", "sub", "adc", "sbb"):
        for w in (1, 2):
            for k, value in enumerate(IMMS):
                masked = value & ((1 << (w * 8)) - 1)
                out.append(case(
                    f"{prefix}{m}{w}_{k}", m, [reg(REG[w][0]), imm(value)],
                    w, _vectors(m, w),
                    lambda a, b, cf, m=m, w=w, v=masked: reference(m, w, a, v, cf),
                    jccs=CONDS))
    return out


class NarrowImmediateTest(unittest.TestCase):
    def test_immediate_is_folded_to_the_width(self):
        """The text: a 16-bit -1 is 0xFFFF in the carry, not 0xFFFFFFFF."""
        dst = reg("ax")
        for m in ("add", "sub", "adc", "sbb"):
            with self.subTest(m=m):
                text = "\n".join(lift_with_jcc(m, [dst, imm(0xFFFFFFFF)], "jb"))
                self.assertIn("0xFFFF", text)
                self.assertNotIn("0xFFFFFFFFu", text)
        # A 32-bit destination keeps its immediate exactly.
        text = "\n".join(lift_with_jcc(
            "add", [reg("eax"), imm(0xFFFFFFFF)], "jb"))
        self.assertIn("0xFFFFFFFFu", text)

    def test_carry_zero_sign_match_x86(self):
        if not find_cc():
            self.skipTest("no C compiler on PATH")
        rc, out = sweep(_cases())
        self.assertEqual(rc, 0, out)

    def test_unmasked_immediate_is_rejected(self):
        """Negative control: the pre-fix source fails the same sweep."""
        if not find_cc():
            self.skipTest("no C compiler on PATH")
        with mock.patch.object(lifter_mod, "_narrow_source",
                               lambda ops: _fmt_operand_read(ops[1])):
            cases = _cases("old_")
        # `add ax, -1; jb` is upstream's measured case; each of the four
        # instructions must fail on its own, not only in aggregate.
        for m in ("add", "sub", "adc", "sbb"):
            with self.subTest(m=m):
                rc, out = sweep([c for c in cases
                                 if c["name"] == f"old_{m}2_0"])
                self.assertNotEqual(rc, 0, f"{m}: the sweep passed the "
                                           f"unmasked immediate")


if __name__ == "__main__":
    unittest.main()
