"""Every gap the runtime can report names a row of docs/hardware_inventory.md."""
import importlib.util
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "recomp/runtime"
REPORT = ROOT / "recomp/tools/gap_report.py"
spec = importlib.util.spec_from_file_location("gap_report", REPORT)
gap_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gap_report)

# A string in the runtime that is an ID: an area, a dot, and at least one more character.
ID = re.compile(r'"((?:cpu|gx|pixel|vtx|hw|audio|ios|ctl|limit|unlisted)\.[a-z0-9_.]+)"')
HEADER = re.compile(r"\.[ch]$")  # "cpu.h" in an include is a file, not an ID.


class Gaps(unittest.TestCase):
    def test_rows_have_unique_ids(self):
        text = gap_report.INVENTORY.read_text()
        ids = re.findall(r"^\| `([a-z0-9_.]+)` \|", text, re.M)
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(len(ids), len(gap_report.inventory_rows()))

    def test_every_id_in_the_runtime_is_a_row(self):
        rows = gap_report.inventory_rows()
        found = {}
        for source in sorted(RUNTIME.glob("*.[ch]")):
            for ident in (i for i in ID.findall(source.read_text()) if not HEADER.search(i)):
                found.setdefault(ident, source.name)
        self.assertGreater(len(found), 40)
        strays = {i: f for i, f in found.items() if i not in rows and not i.startswith(gap_report.OWN_PREFIXES)}
        self.assertEqual(strays, {})

    def test_report_ranks_by_runs_and_separates_strays(self):
        with tempfile.TemporaryDirectory() as folder:
            first, second = Path(folder) / "a.txt", Path(folder) / "b.txt"
            first.write_text("# id detail count first_lr\ncpu.dsi_isi 0x0 3 0x0\ncpu.program_trap_illegal 0x0 1 0x0\nunlisted.ios.file 0x6 1 0x0\n")
            second.write_text("cpu.program_trap_illegal 0x4 2 0x0\ncpu.divwu 0x0 1 0x0\n")
            out = subprocess.run([sys.executable, str(REPORT), f"menu={first}", f"game={second}"], capture_output=True, text=True,
                                 check=True).stdout
        close = out.split("## Gaps to close")[1].split("##")[0]
        self.assertLess(close.index("`cpu.program_trap_illegal`"), close.index("`cpu.dsi_isi`"))  # Two runs beat one.
        self.assertIn("| `cpu.program_trap_illegal` | missing | game, menu | 3 | 0x0, 0x4 |", close)
        self.assertIn("`cpu.divwu`", out.split("## Marked done or not needed, but hit")[1])
        self.assertIn("`unlisted.ios.file`", out.split("## No inventory row")[1])


if __name__ == "__main__":
    unittest.main()
