import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "recomp/translate"))

from signatures import BOARD_LIBRARY, Finder, idle_loop, instruction_hashes, masked_words, resolve, small_data_references

BLR = 0x4e800020
ADD = 0x7c632214
NOP = 0x60000000


def call(offset):
    return 0x48000001 | (offset & 0x3fffffc)


def load_high(register, value):
    return 0x3c000000 | register << 21 | value


def load_small(register, offset):
    return 0x80000000 | register << 21 | 13 << 16 | (offset & 0xffff)


class FakeProgram:
    """Functions as lists of words, placed at the addresses given."""

    def __init__(self, placed, data=b""):
        self.words, self.functions, self.data, self.entry = {}, {}, data, min(placed)
        for start, words in placed.items():
            self.functions[start] = {start + i * 4 for i in range(len(words))}
            self.words.update({start + i * 4: word for i, word in enumerate(words)})

    def word(self, address):
        return self.words[address]


def body_of(program, start):
    return program.functions[start]


def entry(name, words, group="required", **more):
    source = FakeProgram({0x1000: words})
    return {"name": name, "group": group, "reason": name, "hashes": instruction_hashes(source, source.functions[0x1000]), **more}


class Masking(unittest.TestCase):
    def test_call_targets_are_blanked(self):
        first = FakeProgram({0x1000: [call(0x100), BLR]})
        second = FakeProgram({0x2000: [call(0x900), BLR]})
        self.assertEqual(masked_words(first, first.functions[0x1000]), masked_words(second, second.functions[0x2000]))

    def test_addresses_built_from_two_parts_are_blanked(self):
        addi = 0x38630000
        first = FakeProgram({0x1000: [load_high(3, 0x8012), addi | 0x10, BLR]})
        second = FakeProgram({0x1000: [load_high(3, 0x8070), addi | 0x44, BLR]})
        self.assertEqual(masked_words(first, first.functions[0x1000]), masked_words(second, second.functions[0x1000]))

    def test_small_data_offsets_are_blanked(self):
        first = FakeProgram({0x1000: [load_small(3, -0x10), BLR]})
        second = FakeProgram({0x1000: [load_small(3, -0x80), BLR]})
        self.assertEqual(masked_words(first, first.functions[0x1000]), masked_words(second, second.functions[0x1000]))

    def test_real_instructions_stay_different(self):
        first = FakeProgram({0x1000: [ADD, BLR]})
        second = FakeProgram({0x1000: [NOP, BLR]})
        self.assertNotEqual(masked_words(first, first.functions[0x1000]), masked_words(second, second.functions[0x1000]))


class Resolving(unittest.TestCase):
    TARGET = [ADD, NOP, ADD, BLR]

    def resolve(self, placed, entries, data=b""):
        program = FakeProgram(placed, data)
        return resolve(program, program.functions, entries)

    def test_same_function_at_another_address_is_found(self):
        entries = [entry("panic", self.TARGET, body="guest_panic();")]
        replaced, _, _ = self.resolve({0x9000: [BLR], 0x5000: self.TARGET}, entries)
        self.assertEqual(replaced, {0x5000: ("panic", "guest_panic();")})

    def test_a_missing_function_stops_and_names_it(self):
        entries = [entry("panic", self.TARGET, body="")]
        with self.assertRaises(SystemExit) as caught:
            self.resolve({0x9000: [BLR]}, entries)
        self.assertIn("panic", str(caught.exception))

    def test_two_matches_stop_rather_than_guess(self):
        entries = [entry("panic", self.TARGET, body="")]
        with self.assertRaises(SystemExit):
            self.resolve({0x5000: self.TARGET, 0x6000: self.TARGET}, entries)

    def test_board_functions_are_skipped_without_the_board_library(self):
        entries = [entry("board", self.TARGET, group="board", body="")]
        replaced, _, _ = self.resolve({0x9000: [BLR]}, entries)
        self.assertEqual(replaced, {})

    def test_board_functions_are_required_with_the_board_library(self):
        entries = [entry("board", self.TARGET, group="board", body="")]
        with self.assertRaises(SystemExit):
            self.resolve({0x9000: [BLR]}, entries, BOARD_LIBRARY)

    def test_function_beside_a_found_one(self):
        entries = [entry("check", [BLR], beside={"name": "send", "offset": -0x40}, body="x();"), entry("send", self.TARGET, body="")]
        replaced, _, _ = self.resolve({0x4fc0: [BLR], 0x5000: self.TARGET}, entries)
        self.assertEqual(replaced[0x4fc0], ("check", "x();"))

    def test_function_counted_back_from_an_anchor(self):
        anchor = [ADD, ADD, ADD, BLR]
        hashes = entry("anchor", anchor)["hashes"]
        entries = [entry("read", [BLR], before={"anchor": hashes, "count": 2}, body="")]
        placed = {0x1000: [NOP, BLR], 0x2000: [ADD, BLR], 0x3000: [NOP, ADD, BLR], 0x4000: anchor}
        replaced, _, _ = self.resolve(placed, entries)
        self.assertEqual(list(replaced), [0x2000])

    def test_idle_loop_is_the_wait_near_its_offset(self):
        scheduler = [ADD, NOP, load_small(0, -25312), 0x2c000000, 0x4182fff8, BLR]  # The wait is 8 bytes in.
        entries = [entry("scheduler", scheduler, idle_offset=12)]
        _, idle_loops, _ = self.resolve({0x5000: scheduler}, entries)
        self.assertEqual(list(idle_loops), [0x5008])

    def test_no_wait_means_no_idle_loop(self):
        entries = [entry("scheduler", self.TARGET, idle_offset=8)]
        _, idle_loops, _ = self.resolve({0x5000: self.TARGET}, entries)
        self.assertEqual(idle_loops, {})


class Similarity(unittest.TestCase):
    WORDS = [0x7c000214 + (i << 11) for i in range(20)]

    def finder(self, placed):
        program = FakeProgram(placed)
        return Finder(program, program.functions)

    def hashes(self):
        return entry("x", self.WORDS)["hashes"]

    def test_a_slightly_changed_function_is_found(self):
        changed = list(self.WORDS)
        changed[3] = NOP
        self.assertEqual(self.finder({0x5000: changed}).similar(self.hashes()), 0x5000)

    def test_a_close_runner_up_means_no_answer(self):
        one, two = list(self.WORDS), list(self.WORDS)
        one[3], two[4] = NOP, NOP
        self.assertIsNone(self.finder({0x5000: one, 0x6000: two}).similar(self.hashes()))

    def test_something_unlike_it_is_not_accepted(self):
        unlike = [0x7c000378 + (i << 11) for i in range(20)]
        self.assertIsNone(self.finder({0x5000: unlike}).similar(self.hashes()))


class SmallData(unittest.TestCase):
    def test_references_are_worked_out_from_the_base(self):
        program = FakeProgram({0x1000: [load_small(3, -0x10), ADD, BLR]})
        self.assertEqual(small_data_references(program, program.functions[0x1000], 0x8000), [0x7ff0])


if __name__ == "__main__":
    unittest.main()


COMPARE_ZERO = 0x2c000000  # cmpwi r0, 0
BEQ_BACK_8 = 0x4182fff8  # beq cr0 to two words back


class IdleLoops(unittest.TestCase):
    """The scheduler's wait for a ready thread, found by its shape, not one fixed place."""

    SPIN = [load_small(0, -25312), COMPARE_ZERO, BEQ_BACK_8]

    def test_found_where_the_hint_misses(self):
        program = FakeProgram({0x5000: [NOP, NOP, *self.SPIN, BLR]})
        self.assertEqual(idle_loop(program, program.functions[0x5000], 0x500c), 0x5008)

    def test_the_hint_picks_between_two(self):
        program = FakeProgram({0x5000: [*self.SPIN, NOP, NOP, NOP, *self.SPIN, BLR]})
        self.assertEqual(idle_loop(program, program.functions[0x5000], 0x5000), 0x5000)
        self.assertEqual(idle_loop(program, program.functions[0x5000], 0x5020), 0x5018)

    def test_other_loops_are_not_waits(self):
        not_small_data = 0x80000000 | 4 << 16  # lwz r0, 0(r4)
        program = FakeProgram({0x5000: [not_small_data, COMPARE_ZERO, BEQ_BACK_8, load_small(0, 8), 0x2c000001, BEQ_BACK_8, BLR]})
        self.assertIsNone(idle_loop(program, program.functions[0x5000], 0x5000))
