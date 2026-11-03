import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "recomp/translate"))

from decode import name_of
from emit import condition, emit, mask_value
from functions import branch_target


class Names(unittest.TestCase):
    def test_common_instructions(self):
        self.assertEqual(name_of(0x38600001), "addi")    # li r3, 1
        self.assertEqual(name_of(0x4e800020), "bclr")    # blr
        self.assertEqual(name_of(0x7c0802a6), "mfspr")   # mflr r0
        self.assertEqual(name_of(0x4c000064), "rfi")

    def test_string_and_external_control_instructions(self):
        self.assertEqual(name_of(0x7ca4342a), "lswx")
        self.assertEqual(name_of(0x7ca434aa), "lswi")
        self.assertEqual(name_of(0x7ca4352a), "stswx")
        self.assertEqual(name_of(0x7ca435aa), "stswi")
        self.assertEqual(name_of(0x7ca4336c), "ecowx")
        self.assertEqual(name_of(0x7c0435ec), "dcba")

    def test_padding_is_not_an_instruction(self):
        self.assertIsNone(name_of(0))


class Branches(unittest.TestCase):
    def test_relative_call_forwards(self):
        self.assertEqual(branch_target(0x80001000, 0x48000101), (0x80001100, True))

    def test_relative_jump_backwards(self):
        self.assertEqual(branch_target(0x80001000, 0x4bfffffc), (0x80000ffc, False))

    def test_other_instructions_have_no_target(self):
        self.assertIsNone(branch_target(0x80001000, 0x38600001))


class Translation(unittest.TestCase):
    def test_load_immediate_ignores_register_zero(self):
        self.assertEqual(emit(0, 0x38600001, "addi"), "cpu.r[3] = (uint32_t)(1);")

    def test_mask_in_order_and_wrapped(self):
        self.assertEqual(mask_value(24, 31), 0xff)
        self.assertEqual(mask_value(31, 0), 0x80000001)

    def test_unconditional_branch_has_no_test(self):
        self.assertEqual(condition(0x4e800020), ("", "1"))

    def test_decrement_branch_counts_down(self):
        before, test = condition(0x42000000)  # bdnz
        self.assertEqual(before, "cpu.ctr--;")
        self.assertEqual(test, "cpu.ctr != 0")


if __name__ == "__main__":
    unittest.main()
