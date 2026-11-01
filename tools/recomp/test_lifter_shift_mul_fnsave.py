"""Shift counts, multiply flags and fnsave/frstor, run against x86 on this host.

Three gaps in the same family -- what the CPU defines and the C did not:

  * shl/shr used their count as it came. x86 masks it to five bits; C leaves a
    shift by the type's width or more undefined, and the carry's bit index
    `w - cnt` went negative for `shl al, cl` with cl = 9..31.
  * mul and imul set CF = OF = "the product did not fit", and nothing read
    it: mul cleared flag tracking, imul was tracked with no rule, so jo, jc,
    seto and setc after either fell back to the never-assigned _flags.
  * fnsave and frstor were not lifted at all (a comment), so a routine that
    saved the FPU around its own use of it saved and restored nothing.

Upstream measured all three against the CPU through fpdiff
(sp00nznet/xboxrecomp#277, NoRain211): `shl al, 32` gave 0 where the CPU
leaves the value, and after fnsave the stack depth was 2 where the CPU's is 0.
The narrow multiplies themselves (mul bl into AX only) were already right
here; their flags were not. Each group below compiles the lifter's output.
"""
import math
import unittest

from .disasm import Instruction, Operand
from .flag_sweep import (PRELUDE_FOR_TESTS, REG, case, compile_and_run,
                         edge_values, find_cc, imm, lift_with_jcc, reference,
                         reg, sweep, value_pairs)
from .lifter import Lifter, _make_condition


class ShiftCountTest(unittest.TestCase):
    def test_count_in_a_register_is_masked(self):
        text = "\n".join(lift_with_jcc("shl", [reg("eax"), reg("cl")], "jb"))
        self.assertIn("& 31u", text)
        self.assertNotIn("32 - (", text)

    def test_immediate_count_is_folded(self):
        # `shl eax, 1` keeps its old shape: a literal count, no mask.
        text = "\n".join(lift_with_jcc("shl", [reg("eax"), imm(1)], "jb"))
        self.assertIn("eax = eax << 1;", text)
        self.assertIn("_cf = (int)(((eax) >> 31) & 1);", text)
        # A count that masks to zero writes nothing at all.
        text = "\n".join(Lifter().lift_instruction(Instruction(
            0, 3, "shl", "", "", operands=[reg("eax"), imm(32)])))
        self.assertNotIn("eax =", text)

    def test_value_and_carry_match_x86_for_every_count(self):
        if not find_cc():
            self.skipTest("no C compiler on PATH")
        cases = []
        for m in ("shl", "shr"):
            for w in (1, 2, 4):
                vecs = [(a, n, cf) for a in edge_values(w)
                        for n in range(64) for cf in (0, 1)]
                cases.append(case(
                    f"{m}{w}", m, [reg(REG[w][0]), reg("cl")], w, vecs,
                    lambda a, b, cf, m=m, w=w: reference(m, w, a, b, cf),
                    jccs=("jb", "jae")))
                # Immediate counts, including ones past the width and 32.
                for n in (1, 7, 8, 9, 15, 16, 17, 31, 32, 33):
                    vecs = [(a, 0, cf) for a in edge_values(w) for cf in (0, 1)]
                    cases.append(case(
                        f"{m}{w}_i{n}", m, [reg(REG[w][0]), imm(n)], w, vecs,
                        lambda a, b, cf, m=m, w=w, n=n: reference(m, w, a, n, cf),
                        jccs=("jb", "jae")))
        rc, out = sweep(cases)
        self.assertEqual(rc, 0, out)


class MultiplyFlagsTest(unittest.TestCase):
    def test_mul_no_longer_clears_tracking(self):
        for m in ("mul", "imul"):
            for jcc in ("jo", "jno", "jb", "jae"):
                with self.subTest(m=m, jcc=jcc):
                    cond = _make_condition(jcc, m, [reg("ebx")])
                    self.assertIsNotNone(cond)
                    self.assertIn("_fa", cond[0])
            # SF, ZF and PF are undefined after a multiply: still no rule.
            for jcc in ("je", "js", "jp", "jl", "ja"):
                with self.subTest(m=m, jcc=jcc):
                    self.assertIsNone(_make_condition(jcc, m, [reg("ebx")]))
        text = "\n".join(lift_with_jcc("mul", [reg("ebx")], "jo"))
        self.assertNotIn("_flags", text)

    def test_flag_is_only_computed_where_something_can_read_it(self):
        # Thousands of multiplies, almost no readers: without a reader in the
        # function the multiply lifts exactly as it did before.
        from .translator import FunctionTranslator
        lifter = Lifter()
        lifter.needs_mul_flags = False
        for ops in ([reg("bl")], [reg("bx")], [reg("ebx")]):
            for m in ("mul", "imul"):
                text = "\n".join(lifter.lift_instruction(Instruction(
                    0, 2, m, "", "", operands=ops)))
                self.assertNotIn("_fa", text)
        text = "\n".join(lifter.lift_instruction(Instruction(
            0, 3, "imul", "", "", operands=[reg("eax"), reg("ebx")])))
        self.assertNotIn("_fa", text)
        # A carry reader turns it back on: _cf comes from _fa.
        lifter.needs_cf = True
        text = "\n".join(lifter.lift_instruction(Instruction(
            0, 2, "mul", "", "", operands=[reg("ebx")])))
        self.assertIn("_cf = (int)_fa;", text)

        def reads(*mnemonics):
            return FunctionTranslator._function_reads_mul_flags(
                [Instruction(i, 1, m, "", "") for i, m in enumerate(mnemonics)])
        for consumer in ("jo", "jno", "jb", "jae", "setc", "cmovo", "sahf"):
            self.assertTrue(reads("imul", consumer), consumer)
        for consumer in ("je", "jl", "js", "setg", "ret"):
            self.assertFalse(reads("imul", consumer), consumer)

    def test_products_and_overflow_match_x86(self):
        if not find_cc():
            self.skipTest("no C compiler on PATH")
        conds = ("jo", "jno", "jb", "jae")
        cases = []
        for w in (1, 2, 4):
            d, s, _ = REG[w]
            pairs = value_pairs(w, extra=200)
            for m in ("mul", "imul"):
                cases.append(case(
                    f"{m}{w}", m, [reg(s)], w, [(a, b, 0) for a, b in pairs],
                    lambda a, b, cf, m=m, w=w: reference(m, w, a, b, cf),
                    wide=True, jccs=conds))
            if w == 1:
                continue      # imul has no 8-bit two-operand form
            cases.append(case(
                f"imul2_{w}", "imul", [reg(d), reg(s)], w,
                [(a, b, 0) for a, b in pairs],
                lambda a, b, cf, w=w: reference("imul2", w, a, b, cf),
                jccs=conds))
            for k, v in enumerate((0xFFFFFFFF, 3, 0x7FFF, 0xFFFF8000)):
                cases.append(case(
                    f"imul3_{w}_{k}", "imul", [reg(d), reg(s), imm(v)], w,
                    [(a, b, 0) for a, b in pairs],
                    lambda a, b, cf, w=w, v=v: reference("imul2", w, b, v, cf),
                    jccs=conds))
        rc, out = sweep(cases)
        self.assertEqual(rc, 0, out)


def _f80(v):
    """(mantissa, sign_exponent) of a finite double as the x87 stores it."""
    if v == 0:
        return 0, (0x8000 if math.copysign(1, v) < 0 else 0)
    m, e = math.frexp(abs(v))
    mant = int(m * (1 << 64))
    return mant, (0x8000 if v < 0 else 0) | (e - 1 + 16383)


_FPU_MAIN = r'''
static void save(void)    { esi = 0x100; FNSAVE }
static void restore(void) { esi = 0x100; FRSTOR }

int main(void) {
    static const double vals[8] = { VALUES };
    int failures = 0;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;
    g_fp_top = 5;
    for (int i = 0; i < 8; i++) g_fp_stack[(5 + i) & 7] = vals[i];
    g_fp_control_word = 0x027F; g_fp_cc = 0x4100; g_fp_cmp = 0;
    save();
    if (*(uint16_t *)(guest + 0x100) != 0x027F) { printf("cw\n"); failures++; }
    if (*(uint16_t *)(guest + 0x100 + STRIDE) != (uint16_t)((5 << 11) | 0x4100))
        { printf("sw %04X\n", *(uint16_t *)(guest + 0x100 + STRIDE)); failures++; }
    for (int i = 0; i < 8; i++) {
        uint64_t m; uint16_t se;
        memcpy(&m, guest + 0x100 + ENV + 10 * i, 8);
        memcpy(&se, guest + 0x100 + ENV + 10 * i + 8, 2);
        if (m != want_m[i] || se != want_se[i])
            { printf("st%d %016llX %04X\n", i, (unsigned long long)m, se); failures++; }
    }
    /* fnsave initialises the FPU, as fninit does. */
    if (g_fp_top != 0 || g_fp_control_word != 0x037F || g_fp_cc != 0)
        { printf("not reset: top %d cw %04X\n", g_fp_top, g_fp_control_word); failures++; }
    for (int i = 0; i < 8; i++) g_fp_stack[i] = -99.0;
    g_fp_top = 2;
    restore();
    if (g_fp_top != 5 || g_fp_control_word != 0x027F || g_fp_cc != 0x4100 || g_fp_cmp != 0)
        { printf("restore: top %d cw %04X cc %04X\n", g_fp_top, g_fp_control_word, g_fp_cc); failures++; }
    for (int i = 0; i < 8; i++)
        if (g_fp_stack[(g_fp_top + i) & 7] != vals[i])
            { printf("restored st%d = %g\n", i, g_fp_stack[(g_fp_top + i) & 7]); failures++; }
    printf(failures ? "FAIL\n" : "OK\n");
    return failures != 0;
}
'''


class FnsaveFrstorTest(unittest.TestCase):
    @staticmethod
    def _lift(m, prefix=""):
        insn = Instruction(0x10000, len(prefix) // 2 + 2, m, "[esi]",
                           prefix + ("dd36" if m == "fnsave" else "dd26"),
                           operands=[Operand(type="mem", mem_base="esi",
                                             mem_size=4)])
        return "\n".join(Lifter().lift_instruction(insn))

    def test_lifted_to_the_helpers(self):
        self.assertIn("recomp_fnsave(esi, 0);", self._lift("fnsave"))
        self.assertIn("recomp_frstor(esi, 0);", self._lift("frstor"))
        # The operand-size prefix selects the 14-byte environment.
        self.assertIn("recomp_fnsave(esi, 1);", self._lift("fnsave", "66"))
        self.assertIn("recomp_frstor(esi, 1);", self._lift("frstor", "66"))

    def test_image_and_round_trip(self):
        if not find_cc():
            self.skipTest("no C compiler on PATH")
        vals = (1.0, -2.5, 0.0, 3.141592653589793, -0.0, 1e-300, 65504.0, -1e300)
        enc = [_f80(v) for v in vals]
        for prefix, stride in (("", 4), ("66", 2)):
            with self.subTest(short=bool(prefix)):
                src = (PRELUDE_FOR_TESTS
                       + "static const uint64_t want_m[8] = {"
                       + ", ".join(f"0x{m:016X}ull" for m, _ in enc) + "};\n"
                       + "static const uint16_t want_se[8] = {"
                       + ", ".join(f"0x{se:04X}" for _, se in enc) + "};\n"
                       + _FPU_MAIN
                       .replace("FNSAVE", self._lift("fnsave", prefix))
                       .replace("FRSTOR", self._lift("frstor", prefix))
                       .replace("VALUES", ", ".join(repr(v) for v in vals))
                       .replace("STRIDE", str(stride))
                       .replace("ENV", str(7 * stride)))
                rc, out = compile_and_run(src)
                self.assertEqual(rc, 0, out)


if __name__ == "__main__":
    unittest.main()
