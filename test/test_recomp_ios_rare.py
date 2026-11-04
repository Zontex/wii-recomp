"""Rarely used IOS and Bluetooth requests, each answered as a Wii would, in small C harnesses."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).parent
RUNTIME = HERE.parent / "recomp/runtime"
FLAGS = ["-w", "-O1", "-fwrapv", f"-I{RUNTIME}", f"-I{HERE}", "-lm"]


@unittest.skipUnless(shutil.which("clang"), "needs clang")
class RareRequests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def build_and_run(self, sources, *arguments):
        program = self.folder / "program"
        subprocess.run(["clang", *map(str, sources), *FLAGS, "-o", str(program)], check=True, capture_output=True, text=True)
        result = subprocess.run([str(program), *map(str, arguments)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.endswith("ok\n"), result.stdout)
        return result

    def test_replies_crypto_usb_and_cover_status(self):
        self.build_and_run([HERE / "c/test_ios_rare.c"] + [RUNTIME / name for name in ("ios.c", "usb.c", "crypto.c", "dvd.c", "gaps.c")])

    def test_attributes_open_modes_and_shutdown(self):
        saves = self.folder / "saves"
        saves.mkdir()
        self.build_and_run([HERE / "c/test_storage_rare.c", RUNTIME / "fs.c", RUNTIME / "files.c"], saves)
        self.assertEqual((saves / "title/b").read_bytes(), b"yes!")

    def test_bluetooth_rare_commands_and_channels(self):
        sources = ["bluetooth.c", "bluetooth_channels.c", "wii_remote.c", "remote_link.c", "gaps.c"]
        result = self.build_and_run([HERE / "c/test_bluetooth_rare.c"] + [RUNTIME / name for name in sources])
        self.assertIn("gap: ctl.sdp_queries 0x1", result.stderr)  # Refused service discovery is reported.

    def test_real_devices_through_the_link(self):
        sources = ["bluetooth.c", "bluetooth_channels.c", "wii_remote.c", "remote_link.c", "gaps.c"]
        result = self.build_and_run([HERE / "c/test_remote_link.c"] + [RUNTIME / name for name in sources], self.folder / "remotes.txt")
        self.assertIn("remotes: the board 00:23:31:00:00:09 is player 5", result.stderr)
        self.assertIn("00:23:31:00:00:09 4", (self.folder / "remotes.txt").read_text())  # Its slot is remembered.

    @unittest.skipUnless(sys.platform == "linux", "Linux's Bluetooth backend")
    def test_linux_remote_sockets(self):
        sources = [HERE / "c/test_remote_link_linux.c", RUNTIME / "remote_link_linux.c", RUNTIME / "remote_link.c"]
        self.build_and_run(sources, self.folder / "remotes.txt")


if __name__ == "__main__":
    unittest.main()
