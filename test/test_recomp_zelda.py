"""The Zelda family's audio program (recomp/runtime/zelda.c), in a small C harness."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).parent
RUNTIME = HERE.parent / "recomp/runtime"
FLAGS = ["-w", "-O1", "-fno-strict-aliasing", "-fwrapv", "-fsigned-char", "-ffp-contract=off",
         f"-I{RUNTIME}", f"-I{HERE}", "-lm"]


@unittest.skipUnless(shutil.which("clang"), "needs clang")
class ZeldaProgram(unittest.TestCase):
    def test_mail_commands_rendering_and_afc(self):
        with tempfile.TemporaryDirectory() as folder:
            program = Path(folder) / "program"
            subprocess.run(["clang", str(HERE / "c/test_zelda.c"), *FLAGS, "-o", str(program)], check=True,
                           capture_output=True, text=True)
            result = subprocess.run([str(program)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.endswith("ok\n"), result.stdout)


if __name__ == "__main__":
    unittest.main()
