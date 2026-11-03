"""Run with: python3 -m unittest discover -s test -p 'test_*.py'"""
import hashlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "recomp" / "tools"))

from extract_disc import check_hash, nodtool_build


class ExtractDisc(unittest.TestCase):
    def test_each_computer_gets_its_own_nodtool(self):
        self.assertEqual(nodtool_build("Darwin", "x86_64")[0], "nodtool-macos-x86_64")
        self.assertEqual(nodtool_build("Darwin", "arm64")[0], "nodtool-macos-arm64")
        self.assertEqual(nodtool_build("Linux", "aarch64")[0], "nodtool-linux-aarch64")

    def test_unknown_computer_stops_with_a_message(self):
        with self.assertRaises(SystemExit):
            nodtool_build("Plan9", "mips")

    def test_a_changed_download_is_refused(self):
        check_hash(b"nodtool", hashlib.sha256(b"nodtool").hexdigest())
        with self.assertRaises(SystemExit):
            check_hash(b"tampered", hashlib.sha256(b"nodtool").hexdigest())


if __name__ == "__main__":
    unittest.main()
