"""Compiles small C harnesses around the hand-written runtime and runs them."""
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).parent
RUNTIME = HERE.parent / "recomp/runtime"
FLAGS = ["-w", "-O1", "-fwrapv", f"-I{RUNTIME}", f"-I{HERE}", "-lm"]
INTERPRETER = [HERE / "c/test_interp.c", RUNTIME / "dispatch.c", RUNTIME / "interp.c", RUNTIME / "quantized.c"]


def tree(folder):
    """Every path under a folder, with the hash and time of each file."""
    found = {}
    for path in sorted(Path(folder).rglob("*")):
        info = path.stat()
        data = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else "folder"
        found[str(path)] = (data, info.st_mtime_ns)
    return found


@unittest.skipUnless(shutil.which("clang"), "needs clang")
class Runtime(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def compile(self, sources, flags=()):
        program = self.folder / "program"
        subprocess.run(["clang", *map(str, sources), *flags, *FLAGS, "-o", str(program)], check=True,
                       capture_output=True, text=True)
        return program

    def build_and_run(self, sources, *arguments, flags=()):
        program = self.compile(sources, flags)
        result = subprocess.run([str(program), *map(str, arguments)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.endswith("ok\n"), result.stdout)

    def test_interpreter(self):
        self.build_and_run(INTERPRETER)

    def test_interpreter_logs_each_address_once(self):
        program = self.compile(INTERPRETER)
        result = subprocess.run([str(program), "logs"], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(re.findall(r"interpreting (\w+)", result.stderr), ["80100000", "80100008"])

    def test_nonsense_addresses_stop_the_program(self):
        program = self.compile(INTERPRETER)
        for mode, status, message in (("null-call", 4, "no code at 00000000"), ("hardware-call", 4, "no code at cc003000"),
                                      ("unknown", 3, "unknown instruction 00000000")):
            result = subprocess.run([str(program), mode], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, status, result.stderr)
            self.assertIn(message, result.stderr)

    def test_helpers_shared_with_translated_code(self):
        self.build_and_run([HERE / "c/test_cpu_helpers.c", RUNTIME / "quantized.c"])

    def test_clock(self):
        self.build_and_run([HERE / "c/test_clock.c"])

    def test_fifo_ring_and_bursts(self):
        self.build_and_run([HERE / "c/test_fifo.c"])

    def test_attachment_encryption_and_layout(self):
        self.build_and_run([HERE / "c/test_attachment.c"])

    def test_storage_is_never_written(self):
        storage, saves = self.folder / "storage", self.folder / "saves"
        (storage / "shared2").mkdir(parents=True)
        (storage / "shared2/only_storage").write_text("old")
        (storage / "shared2/read_only").write_text("keep")
        saves.mkdir()
        before = tree(storage)
        os.chmod(storage / "shared2/read_only", 0o444)
        self.build_and_run([HERE / "c/test_storage.c", RUNTIME / "fs.c", RUNTIME / "files.c"], storage, saves)
        os.chmod(storage / "shared2/read_only", 0o644)
        self.assertEqual({k: v[0] for k, v in tree(storage).items()}, {k: v[0] for k, v in before.items()})
        self.assertEqual((saves / "shared2/only_storage").read_text()[:3], "new")


if __name__ == "__main__":
    unittest.main()
