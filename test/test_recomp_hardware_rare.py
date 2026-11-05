"""Rarely used hardware: small C harnesses around the runtime, and translated traps."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).parent
RUNTIME = HERE.parent / "recomp/runtime"
FLAGS = ["-w", "-O1", "-fwrapv", f"-I{RUNTIME}", f"-I{HERE}", "-lm"]
sys.path.insert(0, str(HERE.parent / "recomp/translate"))

from emit import emit  # noqa: E402


def tw(to, a, b):
    return 31 << 26 | to << 21 | a << 16 | b << 11 | 4 << 1


def twi(to, a, immediate):
    return 3 << 26 | to << 21 | a << 16 | immediate & 0xffff


# (name, word, r3, r4, traps): r3 and r4 are the registers compared.
TRAPS = [
    ("tw", tw(31, 3, 4), 1, 2, True),  # Always, as asserts use it.
    ("tw", tw(0, 3, 4), 1, 1, False),  # Never.
    ("tw", tw(4, 3, 4), 5, 5, True),  # Equal.
    ("tw", tw(4, 3, 4), 5, 6, False),
    ("tw", tw(16, 3, 4), 0xffffffff, 1, True),  # Less, signed.
    ("tw", tw(2, 3, 4), 0xffffffff, 1, False),  # Less, unsigned.
    ("twi", twi(8, 3, -3), 5, 0, True),  # Greater, signed.
    ("twi", twi(1, 3, -3), 5, 0, False),  # Greater, unsigned.
    ("twi", twi(12, 3, 0), 0, 0, True),  # Greater or equal.
]


@unittest.skipUnless(shutil.which("clang"), "needs clang")
class RareHardware(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def build_and_run(self, sources):
        program = self.folder / "program"
        subprocess.run(["clang", *map(str, sources), *FLAGS, "-o", str(program)], check=True,
                       capture_output=True, text=True)
        result = subprocess.run([str(program)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.endswith("ok\n"), result.stdout)

    def test_reset_button_audio_interrupt_and_accelerator_dma(self):
        self.build_and_run([HERE / "c/test_hardware_rare.c"])

    def test_audio_processor_mailboxes_boot_and_programs(self):
        self.build_and_run([HERE / "c/test_dsp.c"])

    def test_hollywood_registers_reset_event_and_eject(self):
        self.build_and_run([HERE / "c/test_hollywood.c"])

    def test_traps_and_floating_point_exceptions_are_reported(self):
        self.build_and_run([HERE / "c/test_processor_rare.c", RUNTIME / "processor.c"])

    def test_fifo_reset(self):
        self.build_and_run([HERE / "c/test_fifo_reset.c"])

    def test_translated_traps_fire_on_their_conditions(self):
        checks = []
        for index, (name, word, r3, r4, traps) in enumerate(TRAPS):
            address = 0x80100000 + index * 4
            checks.append(f"  traps = 0; cpu.r[3] = 0x{r3:x}u; cpu.r[4] = 0x{r4:x}u;\n"
                          f"  {emit(address, word, name)}\n"
                          f"  CHECK_EQ(traps, {int(traps)});\n"
                          + (f"  CHECK_EQ(trap_at, 0x{address:x}u);\n" if traps else ""))
        source = self.folder / "traps.c"
        source.write_text('#include "harness.h"\n'
                          "uint32_t hw_read(uint32_t a, int n) { return 0; }\n"
                          "void hw_write(uint32_t a, uint32_t v, int n) {}\n"
                          "static int traps; static uint32_t trap_at;\n"
                          "void trap_taken(uint32_t address) { traps++; trap_at = address; }\n"
                          "int main(void) {\n  memory_start();\n" + "".join(checks) + "  return finish();\n}\n")
        self.build_and_run([source])


if __name__ == "__main__":
    unittest.main()
