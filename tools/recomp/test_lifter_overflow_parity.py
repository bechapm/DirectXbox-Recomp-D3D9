"""OF, PF and the signed conditions after the arithmetic setters match x86.

Before this, three kinds of consumer were wrong after integer arithmetic:

  * jo/jno had no rule after cmp, sub, add, adc, sbb, neg or a shift, and
    jp/jnp none after anything but cmp, test, inc and dec. Each fell back to
    `_flags`, which nothing assigns, so the branch was never taken.
  * jl/jge/jle/jg after add, adc, sbb, neg and the logical shifts tested the
    result's sign alone. That is SF, and the condition is SF != OF, so they
    were wrong exactly when the arithmetic overflowed: `add al, bl; jl` with
    0x70 + 0x70 wraps to 0xE0 and x86 does not branch.
  * jl after an 8- or 16-bit sub rebuilt the minuend at 32 bits, so it was
    never negative: `sub al, bl; jl` with al = 0x80 did not branch.

Upstream found these with fpdiff (sp00nznet/xboxrecomp#276, NoRain211), e.g.
`int_sub8_jl` failing 587 of 1,344 vectors and `int_adc8_jo` 180. The sweep
below runs every condition after each setter, at 8, 16 and 32 bits, through
the lifter's own output compiled on this host, against the SDM. The negative
controls run the pre-fix expressions through the same sweep and require them to
fail, and the translator checks pin the one thing a new rule costs: shl's OF
reads the carry, so a function that branches on it must declare _cf.
"""
import unittest

from .disasm import Instruction
from .flag_sweep import (JCCS, REG, case, edge_values, find_cc, imm, reference,
                         reg, sweep, value_pairs)
from .lifter import _make_condition
from .translator import FunctionTranslator


def _cases(m, widths=(1, 2, 4)):
    out = []
    for w in widths:
        d, s, c = REG[w]
        cfs = (0, 1)
        if m == "neg":
            vecs = [(a, 0, cf) for a in edge_values(w) for cf in cfs]
            out.append(case(f"neg{w}", "neg", [reg(d)], w, vecs,
                            lambda a, b, cf, w=w: reference("neg", w, a, b, cf)))
        elif m in ("shl", "shr", "sar"):
            # By one, where OF is defined, and by cl over every count.
            vecs = [(a, 1, cf) for a in edge_values(w) for cf in cfs]
            out.append(case(f"{m}{w}_1", m, [reg(d), imm(1)], w, vecs,
                            lambda a, b, cf, m=m, w=w: reference(m, w, a, 1, cf)))
            counts = [n for n in range(32) if m == "sar" or n < w * 8]
            vecs = [(a, n, cf) for a in edge_values(w) for n in counts
                    for cf in cfs]
            out.append(case(f"{m}{w}_cl", m, [reg(d), reg("cl")], w, vecs,
                            lambda a, b, cf, m=m, w=w: reference(m, w, a, b, cf)))
        else:
            cf_in = (0, 1) if m in ("adc", "sbb") else (0,)
            vecs = [(a, b, cf) for a, b in value_pairs(w, extra=64)
                    for cf in cf_in]
            out.append(case(f"{m}{w}", m, [reg(d), reg(s)], w, vecs,
                            lambda a, b, cf, m=m, w=w: reference(m, w, a, b, cf)))
    return out


class OverflowParitySweep(unittest.TestCase):
    def _sweep(self, cases):
        if not find_cc():
            self.skipTest("no C compiler on PATH")
        rc, out = sweep(cases)
        self.assertEqual(rc, 0, out)

    def test_add_sub(self):
        self._sweep(_cases("add") + _cases("sub"))

    def test_adc_sbb(self):
        self._sweep(_cases("adc") + _cases("sbb"))

    def test_neg(self):
        self._sweep(_cases("neg"))

    def test_shifts(self):
        self._sweep(_cases("shl") + _cases("shr") + _cases("sar"))

    def test_logic(self):
        self._sweep(_cases("and") + _cases("or") + _cases("xor"))

    def test_cmp(self):
        # Fused with its jcc, so this is the operand-reading form; the
        # snapshot form shares the expression.
        self._sweep(_cases("cmp"))


class NoFallbackTest(unittest.TestCase):
    """Every condition after these setters has a rule now."""

    def test_no_condition_reads_the_fallback(self):
        ops2 = [reg("al"), reg("bl")]
        for m in ("add", "sub", "adc", "sbb", "and", "or", "xor", "cmp"):
            for jcc in JCCS:
                with self.subTest(m=m, jcc=jcc):
                    self.assertIsNotNone(_make_condition(jcc, m, ops2))
        for m in ("neg",):
            for jcc in JCCS:
                with self.subTest(m=m, jcc=jcc):
                    self.assertIsNotNone(_make_condition(jcc, m, [reg("al")]))


class PreFixControls(unittest.TestCase):
    """The expressions these rules replaced must fail the same sweep."""

    def _expect_failure(self, name, m, w, jcc, expr):
        if not find_cc():
            self.skipTest("no C compiler on PATH")
        d, s, _ = REG[w]
        c = case(name, m, [reg(d), reg(s)], w,
                 [(a, b, 0) for a, b in value_pairs(w)],
                 lambda a, b, cf: reference(m, w, a, b, cf), jccs=(jcc,))
        stmts = c["stmts"][jcc]
        stmts[-1] = f"if ({expr}) goto loc_00010100;"
        rc, out = sweep([c])
        self.assertNotEqual(rc, 0, f"{name}: the old expression passed")

    def test_sub8_jl_rebuilt_at_32_bits(self):
        self._expect_failure("old_sub8_jl", "sub", 1, "jl",
                             "CMP_L((uint32_t)_fa + (uint32_t)_fb, (uint32_t)_fb)")

    def test_add_jl_read_the_sign_alone(self):
        self._expect_failure("old_add32_jl", "add", 4, "jl", "(_fas < 0)")

    def test_adc_jo_fell_back(self):
        self._expect_failure("old_adc8_jo", "adc", 1, "jo", "_flags")


class CarryDeclarationTest(unittest.TestCase):
    """New rules that read _cf must make the function declare it."""

    @staticmethod
    def _insns(*mnemonics):
        return [Instruction(i, 1, m, "", "") for i, m in enumerate(mnemonics)]

    def test_signed_condition_after_shl_needs_cf(self):
        for jcc in ("jo", "jl", "jg", "setle", "cmovno"):
            with self.subTest(jcc=jcc):
                self.assertTrue(FunctionTranslator._function_needs_cf(
                    self._insns("shl", jcc)))
        # shr's OF is the old top bit, which is in the result: no carry.
        self.assertFalse(FunctionTranslator._function_needs_cf(
            self._insns("shr", "jl")))

    def test_lahf_after_a_carry_setter_needs_cf(self):
        # lahf copies CF into AH bit 0. Now that jp has a rule after add, lahf
        # after it is no longer refused, and its AH reads _cf.
        self.assertTrue(FunctionTranslator._function_needs_cf(
            self._insns("add", "lahf")))
        self.assertFalse(FunctionTranslator._function_needs_cf(
            self._insns("and", "lahf")))


if __name__ == "__main__":
    unittest.main()
