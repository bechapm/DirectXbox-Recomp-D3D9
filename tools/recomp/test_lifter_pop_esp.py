"""`pop esp` leaves esp at the value popped.

x86 reads the top of the stack, adds 4 to esp, and then writes the value it
read into the destination -- which, for `pop esp`, is esp itself, so the
increment is lost and esp holds the popped value. The lifter emitted
`POP32(esp, esp)`, and POP32 assigned the destination first and added 4
after it, so esp came out 4 too high. Upstream found it in Spartan: Total
Warrior's round-to-int helper, which does `push esp ... pop esp` and returned
with esp off by 4 (sp00nznet/xboxrecomp#186, sp00nznet/apexJCL).

Both halves are fixed: the lifter spells `pop esp` out, and POP32 itself now
reads, bumps and only then assigns, so C generated before the lifter change is
right with the new header too. Each is compiled and run here against the real
recomp_types.h, with the old macro as the negative control.
"""
import unittest

from .disasm import Instruction, Operand
from .flag_sweep import PRELUDE_FOR_TESTS, compile_and_run, find_cc
from .lifter import Lifter


def _lift_pop(reg):
    insn = Instruction(0x10000, 1, "pop", reg, "",
                       operands=[Operand(type="reg", reg=reg)])
    return "\n".join(Lifter().lift_instruction(insn))


_MAIN = r'''
OLD_MACRO
static void pop_esp(void) { LIFTED_ESP }
static void pop_ebx(void) { LIFTED_EBX }
static void macro_pop_esp(void) { POP32(esp, esp); }
static void old_macro_pop_esp(void) { OLD_POP32(esp, esp); }

int main(void) {
    int failures = 0;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;
    void (*const fns[])(void) = { pop_esp, macro_pop_esp, old_macro_pop_esp };
    const char *names[] = { "lifted", "macro", "old macro" };
    for (int k = 0; k < 3; k++) {
        esp = 0x200;
        *(uint32_t *)(guest + 0x200) = 0x345;
        fns[k]();
        printf("%s %X\n", names[k], esp);
        if (k < 2 && esp != 0x345) failures++;
    }
    /* Any other register: the value popped, esp up by 4. */
    esp = 0x200; ebx = 0;
    *(uint32_t *)(guest + 0x200) = 0xCAFEF00Du;
    pop_ebx();
    if (ebx != 0xCAFEF00Du || esp != 0x204) { printf("pop ebx\n"); failures++; }
    return failures != 0;
}
'''


class PopEspTest(unittest.TestCase):
    def test_lifted_pop_esp_is_a_plain_load(self):
        text = _lift_pop("esp")
        self.assertIn("esp = MEM32(esp);", text)
        self.assertNotIn("POP32(esp, esp)", text)
        self.assertIn("POP32(esp, ebx);", _lift_pop("ebx"))

    def test_runs_as_x86(self):
        if not find_cc():
            self.skipTest("no C compiler on PATH")
        src = PRELUDE_FOR_TESTS + (
            _MAIN.replace("LIFTED_ESP", _lift_pop("esp"))
            .replace("LIFTED_EBX", _lift_pop("ebx"))
            .replace("OLD_MACRO",
                     "#define OLD_POP32(sp, dst) do { (dst) = MEM32(sp);"
                     " (sp) += 4; } while(0)"))
        rc, out = compile_and_run(src)
        self.assertEqual(rc, 0, out)
        # Negative control: the macro as it was leaves esp 4 high.
        self.assertIn("old macro 349", out)


if __name__ == "__main__":
    unittest.main()
