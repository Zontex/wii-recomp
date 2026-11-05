"""The audio processor interpreter and its accelerator, built with clang around made-up
programs, Dolphin's hand-assembled test programs and Dolphin's free boot ROM."""
import shutil
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).parent
RUNTIME = HERE.parent / "recomp/runtime"
DATA = RUNTIME / "data"
SOURCES = [RUNTIME / "dsp_lle.c", RUNTIME / "dsp_lle_ops.c", RUNTIME / "dsp_accelerator.c"]
FLAGS = ["-w", "-O1", "-fwrapv", "-fno-strict-aliasing", "-fsigned-char", "-ffp-contract=off",
         f"-I{RUNTIME}", f"-I{HERE}", "-lm"]


def swapped_adler32(data):
    """Dolphin hashes the ROMs as words in the host's little-endian order."""
    swapped = bytearray(len(data))
    swapped[0::2], swapped[1::2] = data[1::2], data[0::2]
    return zlib.adler32(bytes(swapped))


class FreeRoms(unittest.TestCase):
    def test_roms_are_dolphins_free_v0_4(self):
        rom, coef = (DATA / "dsp_rom.bin").read_bytes(), (DATA / "dsp_coef.bin").read_bytes()
        self.assertEqual((len(rom), len(coef)), (8192, 4096))
        self.assertEqual((swapped_adler32(rom), swapped_adler32(coef)), (0xe789b5a5, 0xa4a575f5))


@unittest.skipUnless(shutil.which("clang"), "needs clang")
class DspInterpreter(unittest.TestCase):
    def test_opcodes_accelerator_rom_boot_and_dolphin_programs(self):
        with tempfile.TemporaryDirectory() as folder:
            program = Path(folder) / "program"
            subprocess.run(["clang", str(HERE / "c/test_dsp_lle.c"), *map(str, SOURCES), *FLAGS,
                            "-o", str(program)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(program), str(DATA)], capture_output=True, text=True,
                                    timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.endswith("ok\n"), result.stdout)


if __name__ == "__main__":
    unittest.main()
