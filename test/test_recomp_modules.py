"""Run-time-loaded modules: reading a made-up RSO and REL, preparing their code, and the C they become."""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).parent.parent
sys.path.insert(0, str(ROOT / "recomp/translate"))

import emit
import modules
from decode import Program
from functions import add_starts

LIS_R3, ADDI_R3, BL, BLR, NOP = 0x3c600000, 0x38630000, 0x48000001, 0x4e800020, 0x60000000
CODE_AT, CODE_WORDS = 0x100, 6


def made_up_rso():
    """Two functions: the first calls the second and the program's `imported`, and loads
    the address of something in section 2. Branches and fields are left for the linker."""
    code = [BL, BL, LIS_R3, ADDI_R3, BLR, BLR]  # Function at 0 ends at 0x10; function at 0x14.
    data = bytearray(0x400)
    struct.pack_into(f">{CODE_WORDS}I", data, CODE_AT, *code)
    struct.pack_into(">I", data, 0x8, 3)        # Sections.
    struct.pack_into(">I", data, 0xc, 0x58)     # Section table.
    sections = [(0, 0), (CODE_AT, CODE_WORDS * 4), (0x200, 0x20)]
    for number, (offset, size) in enumerate(sections):
        struct.pack_into(">II", data, 0x58 + number * 8, offset, size)

    def table(at, entries):
        for index, (offset, symbol, kind, addend) in enumerate(entries):
            struct.pack_into(">III", data, at + index * 12, offset, symbol << 8 | kind, addend)
        return at, len(entries) * 12

    internal = table(0x280, [(CODE_AT, 1, modules.REL24, 0x14), (CODE_AT + 10, 2, modules.HA, 0x10),
                             (CODE_AT + 14, 2, modules.LO, 0x10)])
    external = table(0x2c0, [(CODE_AT + 4, 0, modules.REL24, 0)])
    struct.pack_into(">IIII", data, 0x300, 0, 0x14, 1, 0)  # Export: the second function.
    data[0x310:0x318] = b"second\0\0"
    struct.pack_into(">I", data, 0x320, 0)  # Import 0's name.
    data[0x330:0x33a] = b"imported\0\0"
    struct.pack_into(">10I", data, 0x30, *internal, *external, 0x300, 16, 0x310, 0x320, 12, 0x330)
    return bytes(data)


class Modules(unittest.TestCase):
    def setUp(self):
        self.module = modules.parse_rso("test", made_up_rso())
        self.prepared = modules.prepare(self.module, 0xc0000000, 0xc1000000)

    def test_reads_tables(self):
        self.assertEqual(self.module.imports, ["imported"])
        self.assertEqual(self.module.exports, [("second", 1, 0x14)])
        self.assertEqual(self.module.code_section(), 1)

    def test_branches_reach_their_targets(self):
        code = self.prepared.code
        first, second = struct.unpack_from(">II", code, 0)
        self.assertEqual((0xc0000000 + (first & 0x3fffffc)) & 0xffffffff, 0xc0000014)
        self.assertEqual(0xc0000004 + (second & 0x3fffffc), 0xc1000000)  # The import's stand-in.
        self.assertIn(0xc0000014, self.prepared.starts)
        self.assertEqual(self.prepared.import_sites, {0: ("branch", 4)})
        self.assertEqual(self.prepared.relocated_words, {0, 4, 8, 12})

    def test_import_out_of_reach_stops(self):
        with self.assertRaises(SystemExit):
            modules.prepare(self.module, 0xc0000000, 0xb0000000)

    def test_generated_c(self):
        prepared = self.prepared
        header = bytearray(0x100)
        struct.pack_into(">I", header, 0, 0x100)
        struct.pack_into(">I", header, 0x48, 0xc0000000)
        struct.pack_into(">I", header, 0x90, len(prepared.code))
        program = Program(bytes(header) + prepared.code)
        functions = {}
        add_starts(program, functions, {0xc0000000, 0xc0000014})
        emit.MODULE = {"prefix": "m0", "base": 0xc0000000, "size": len(prepared.code), "imports": 0xc1000000,
                       "import_count": 1, "fields": prepared.fields}
        try:
            text, _, unknown = emit.emit_function(program, 0xc0000000, functions[0xc0000000], set(functions))
        finally:
            emit.MODULE = None
        self.assertFalse(unknown)
        self.assertIn("cpu.lr = (m0_code + 0x4u); f_c0000014();", text)
        self.assertIn("cpu.lr = (m0_code + 0x8u); dispatch(m0_import[0]);", text)
        self.assertIn("cpu.r[3] = (((((m0_section[2] + 0x10u)) + 0x8000u) >> 16) << 16);", text)
        self.assertIn("cpu.r[3] = cpu.r[3] + (uint32_t)((int32_t)(int16_t)((m0_section[2] + 0x10u) & 0xffffu));", text)


PROGRAM_FUNCTION, PROGRAM_DATA = 0x80004000, 0x80451234
REL_CODE_AT = 0x100


def made_up_rel():
    """A REL, number 5, as the SDK's makerel leaves it. Its code calls a program function,
    loads the address of its own data, then of program data; its data points at its second
    function. A prolog is the first function."""
    code = [BL, LIS_R3, ADDI_R3, LIS_R3, ADDI_R3, BLR, BLR]  # Functions at 0 and 0x18.
    data = bytearray(0x400)
    struct.pack_into(">IIIIIIII", data, 0, 5, 0, 0, 4, 0x4c, 0, 0, 3)  # Number, links, sections, table, name, version.
    struct.pack_into(">IIII", data, 0x20, 0x40, 0x300, 0x2e0, 16)       # Bss, relocations, imports.
    data[0x30:0x34] = bytes([1, 1, 1, 0])
    struct.pack_into(">III", data, 0x34, 0, 0x14, 0x14)               # Prolog, epilog, unresolved.
    struct.pack_into(">III", data, 0x40, 32, 32, 0x300)
    for number, (offset, size) in enumerate([(0, 0), (REL_CODE_AT | 1, len(code) * 4), (0x200, 0x20), (0, 0x40)]):
        struct.pack_into(">II", data, 0x4c + number * 8, offset, size)
    struct.pack_into(f">{len(code)}I", data, REL_CODE_AT, *code)
    struct.pack_into(">IIII", data, 0x2e0, 5, 0x300, 0, 0x340)  # Its own relocations, then the program's.

    def relocations(at, entries):
        for index, (step, kind, section, addend) in enumerate(entries):
            struct.pack_into(">HBBI", data, at + index * 8, step, kind, section, addend)

    relocations(0x300, [(0, modules.REL_SECTION, 1, 0), (6, modules.HA, 2, 0x10), (4, modules.LO, 2, 0x10),
                        (0, modules.REL_SECTION, 2, 0), (4, modules.ADDR32, 1, 0x18), (0, modules.REL_END, 0, 0)])
    relocations(0x340, [(0, modules.REL_SECTION, 1, 0), (0, modules.REL24, 0, PROGRAM_FUNCTION),
                        (14, modules.HA, 0, PROGRAM_DATA), (4, modules.LO, 0, PROGRAM_DATA), (0, modules.REL_END, 0, 0)])
    return bytes(data)


class RelModules(unittest.TestCase):
    def setUp(self):
        self.module = modules.parse_module("rel", made_up_rel())
        self.prepared = modules.prepare(self.module, 0xc0000000, 0xc1000000)

    def test_reads_lists(self):
        module = self.module
        self.assertEqual((module.kind, module.module_id, module.code_section(), module.bss_bytes), ("rel", 5, 1, 0x40))
        self.assertIsNone(modules.parse_rso("rel", made_up_rel()))
        self.assertEqual([(r.offset, r.kind, r.symbol) for r in module.internal],
                         [(REL_CODE_AT + 6, modules.HA, 2), (REL_CODE_AT + 10, modules.LO, 2), (0x204, modules.ADDR32, 1)])
        self.assertEqual([(r.offset, r.kind, r.addend) for r in module.fixed],
                         [(REL_CODE_AT, modules.REL24, PROGRAM_FUNCTION), (REL_CODE_AT + 14, modules.HA, PROGRAM_DATA),
                          (REL_CODE_AT + 18, modules.LO, PROGRAM_DATA)])
        self.assertEqual(module.exports, [("_prolog", 1, 0), ("_epilog", 1, 0x14), ("_unresolved", 1, 0x14)])

    def test_program_addresses_are_settled_now(self):
        prepared = self.prepared
        words = struct.unpack_from(">7I", prepared.code)
        self.assertEqual(words[3] & 0xffff, 0x8045)  # The high half, rounded for the signed low half.
        self.assertEqual(words[4] & 0xffff, 0x1234)
        self.assertEqual(0xc0000000 + (words[0] & 0x3fffffc), 0xc1000000)  # The program call's stand-in.
        self.assertEqual(prepared.fixed_calls, {0: PROGRAM_FUNCTION})
        self.assertEqual(prepared.import_sites, {0: ("fixed", 0, PROGRAM_FUNCTION)})
        self.assertEqual(prepared.relocated_words, {0, 4, 8, 12, 16})
        self.assertIn(0xc0000018, prepared.starts)  # From the pointer in its data.

    def test_generated_c_calls_the_program_directly(self):
        prepared = self.prepared
        header = bytearray(0x100)
        struct.pack_into(">I", header, 0, 0x100)
        struct.pack_into(">I", header, 0x48, 0xc0000000)
        struct.pack_into(">I", header, 0x90, len(prepared.code))
        program = Program(bytes(header) + prepared.code)
        functions = {}
        add_starts(program, functions, {0xc0000000})
        emit.MODULE = {"prefix": "m1", "base": 0xc0000000, "size": len(prepared.code), "imports": 0xc1000000,
                       "import_count": 1, "fields": prepared.fields, "fixed": {0xc1000000: PROGRAM_FUNCTION},
                       "program": {PROGRAM_FUNCTION}}
        try:
            text, _, unknown = emit.emit_function(program, 0xc0000000, functions[0xc0000000], set(functions))
        finally:
            emit.MODULE = None
        self.assertFalse(unknown)
        self.assertIn("cpu.lr = (m1_code + 0x4u); f_80004000();", text)
        self.assertIn("m1_section[2] + 0x10u", text)
        self.assertIn("0x80450000", text)

    def test_links_to_other_modules_are_left_to_the_interpreter(self):
        data = bytearray(made_up_rel())
        struct.pack_into(">I", data, 0x2e8, 9)  # The program's list now points into module 9.
        with self.assertRaises(modules.Unsupported):
            modules.prepare(modules.parse_module("rel", bytes(data)), 0xc0000000, 0xc1000000)

    def test_yaz0(self):
        # "abc" as literals, then a copy of 6 from 3 back: "abcabcabc".
        packed = b"Yaz0" + struct.pack(">I", 9) + bytes(8) + bytes([0xe0]) + b"abc" + bytes([0x40, 0x02])
        self.assertEqual(modules.yaz0_decompress(packed), b"abcabcabc")
        self.assertEqual(modules.decompress(b"plain"), b"plain")


if __name__ == "__main__":
    unittest.main()
