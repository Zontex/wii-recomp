import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).parent.parent
sys.path.insert(0, str(ROOT / "recomp/translate"))

from decode import Program, name_of
import emit as emit_module
from emit import REPLACED, check_replaced, emit, emit_function, function_name, picture_scale
from functions import find_functions


def candidates():
    yield ROOT / "extracted/sys/main.dol"


DOL = next((path for path in candidates() if path.exists()), None)


REPLACED.update({0x80139830: ("KPADRead", "input_read();"), 0x801455e0: ("OSPanic", "guest_panic();")})


class Instructions(unittest.TestCase):
    def case(self, word, name, text):
        self.assertEqual(name_of(word), name)
        self.assertEqual(emit(0, word, name), text)

    def test_add_immediate_shifted(self):
        self.case(0x3c608000, "addis", "cpu.r[3] = 0x80000000u;")

    def test_load_uses_register_zero_as_zero(self):
        self.case(0x80600010, "lwz", "cpu.r[3] = rd32((uint32_t)(16));")

    def test_store_with_update_writes_base_last(self):
        # Through r1, the stack: always memory, so the store skips the hardware check.
        self.case(0x9421ffe0, "stwu", "{ uint32_t e = (cpu.r[1] + -32); wr32s(e, cpu.r[1]); cpu.r[1] = e; }")
        self.case(0x9423ffe0, "stwu", "{ uint32_t e = (cpu.r[3] + -32); wr32(e, cpu.r[1]); cpu.r[3] = e; }")

    def test_rotate_mask(self):
        self.case(0x5464043e, "rlwinm", "cpu.r[4] = rotl(cpu.r[3], 0) & 0xffffu;")

    def test_record_form_sets_condition(self):
        self.case(0x7c632215, "add", "cpu.r[3] = cpu.r[3] + cpu.r[4];cpu.cr[0] = cr_compare((int32_t)cpu.r[3], 0);")

    def test_time_base_reads_are_runtime_calls(self):
        self.case(0x7c6d42e6, "mftb", "cpu.r[3] = (uint32_t)(guest_time_base() >> 32);")
        self.case(0x7c8c42e6, "mftb", "cpu.r[4] = (uint32_t)guest_time_base();")

    def test_setting_the_clock_reaches_the_runtime(self):
        self.case(0x7c7c43a6, "mtspr", "cpu.spr[284] = cpu.r[3]; guest_time_base_set(0, cpu.r[3]);")
        self.case(0x7c7d43a6, "mtspr", "cpu.spr[285] = cpu.r[3]; guest_time_base_set(1, cpu.r[3]);")

    def test_a_cache_hint_does_nothing(self):
        self.case(0x7c0435ec, "dcba", ";")

    def test_overflow_form_writes_overflow(self):
        self.case(0x7c632614, "add", "cpu.r[3] = add_overflow(cpu.r[3], cpu.r[4], 0);")

    def test_float_record_form_copies_status(self):
        self.case(0xfc20102b, "fadd", "cpu.f[1] = cpu.f[0] + cpu.f[2]; cpu.cr[1] = (uint8_t)(cpu.fpscr >> 28);")

    def test_fctiwz_doesnt_read_the_rounding_mode(self):
        self.case(0xfc20101e, "fctiwz", "cpu.f[1] = float_to_int(cpu.f[2], ROUND_TOWARD_ZERO);")
        self.case(0xfc20101c, "fctiw", "cpu.f[1] = float_to_int(cpu.f[2], cpu.fpscr & FPSCR_ROUNDING);")

    def test_a_call_to_the_next_line_only_sets_the_link_register(self):
        start = 0x80100000
        words = {start: 0x48000005, start + 4: 0x7c6802a6, start + 8: 0x4e800020}

        class Words:
            word = staticmethod(words.__getitem__)

        text, _, _ = emit_function(Words, start, set(words), {start})
        self.assertIn(f"cpu.lr = 0x{start + 4:x}u;", text)
        self.assertNotIn("dispatch", text)

    def test_replaced_function_keeps_only_its_body(self):
        start = next(iter(a for a, v in REPLACED.items() if v[1] == "guest_panic();"))
        text, _, _ = emit_function(None, start, set(), {start})
        self.assertIn(function_name(start), text)
        self.assertIn("guest_panic();", text)


class ReplacedCheck(unittest.TestCase):
    def test_merged_address_fails_loudly(self):
        with self.assertRaises(SystemExit) as caught:
            check_replaced(set(REPLACED) - {0x80139830})
        self.assertIn("0x80139830", str(caught.exception))

    def test_all_found_passes(self):
        check_replaced(set(REPLACED))

    @unittest.skipUnless(DOL, "needs extracted/sys/main.dol")
    def test_real_program(self):
        functions, _ = find_functions(Program(DOL.read_bytes()))
        check_replaced(set(functions))


class PictureScale(unittest.TestCase):
    def scale(self, *arguments):
        saved = sys.argv
        sys.argv = ["emit.py", *arguments]
        try:
            return picture_scale()
        finally:
            sys.argv = saved

    def test_scale_from_the_command_line(self):
        self.assertEqual(self.scale(), (0, 0))  # The console's own.
        self.assertEqual(self.scale("--scale", "3/2"), (3, 2))
        self.assertEqual(self.scale("--all-code", "--scale", "1"), (1, 1))
        for wrong in ("1/2", "5", "0/1", "9/2"):
            with self.assertRaises(SystemExit):
                self.scale("--scale", wrong)


class Charges(unittest.TestCase):
    """A GameCube title's code counts the cycles it runs; a Wii title's is as it was."""

    START = 0x80100000
    # addi r3,r3,1; mullw r4,r3,r3; cmpwi r3,10; blt start; divw r5,r4,r3; blr
    WORDS = dict(zip(range(START, START + 24, 4), (0x38630001, 0x7c8319d6, 0x2c03000a, 0x4180fff4, 0x7ca41bd6, 0x4e800020)))

    def text(self, charged):
        class Words:
            word = staticmethod(self.WORDS.__getitem__)

        saved = emit_module.CHARGED
        emit_module.CHARGED = charged
        try:
            return emit_function(Words, self.START, set(self.WORDS), {self.START})[0]
        finally:
            emit_module.CHARGED = saved

    def test_each_run_of_instructions_is_charged_before_control_leaves_it(self):
        lines = self.text(True).split("\n")
        # The loop's four instructions, a multiply costing 5: charged before the branch back,
        # whose check then only looks at the count.
        back = next(i for i, line in enumerate(lines) if "POLL(); goto" in line)
        self.assertEqual(lines[back - 1], "interrupt_budget -= 8;")
        # The divide costs 40, and the return 1.
        self.assertEqual(lines[-3], "interrupt_budget -= 41;")
        self.assertIn("return;", lines[-2])
        self.assertEqual(sum("interrupt_budget" in line for line in lines), 2)

    def test_a_title_that_isnt_charged_is_written_as_before(self):
        plain = self.text(False)
        self.assertNotIn("interrupt_budget", plain)
        charged = "\n".join(line for line in self.text(True).split("\n") if not line.startswith("interrupt_budget -= "))
        self.assertEqual(plain, charged)

    def test_a_replaced_function_is_charged_one_pass_over_the_programs_own(self):
        start = self.START
        REPLACED[start] = ("made up", "stand_in();")
        saved = emit_module.CHARGED
        try:
            class Words:
                word = staticmethod(self.WORDS.__getitem__)

            emit_module.CHARGED = True
            text, _, _ = emit_function(Words, start, set(self.WORDS), {start})
            self.assertIn("interrupt_budget -= 49; stand_in();", text)
            emit_module.CHARGED = False
            text, _, _ = emit_function(Words, start, set(self.WORDS), {start})
            self.assertNotIn("interrupt_budget", text)
        finally:
            emit_module.CHARGED = saved
            del REPLACED[start]

    def test_costs_are_dolphins(self):
        self.assertEqual([emit_module.cycles(name) for name in ("addi", "mulli", "mullw", "lmw", "fdivs", "fdiv", "divwu", None)],
                         [1, 3, 5, 11, 17, 31, 40, 1])


if __name__ == "__main__":
    unittest.main()
