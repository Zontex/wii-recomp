"""Run-time-loaded modules: reading a made-up RSO, preparing its code, and the C it becomes."""
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


if __name__ == "__main__":
    unittest.main()
