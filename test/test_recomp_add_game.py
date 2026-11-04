"""Disc in, game out: the two build tiers, the profile that picks them, swapping an image's
native code, and add_game.py's own steps. Made-up data only."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).parent
RUNTIME = HERE.parent / "recomp/runtime"
sys.path.insert(0, str(HERE.parent / "recomp/tools"))
sys.path.insert(0, str(HERE.parent / "recomp/translate"))

import add_game  # noqa: E402
import emit  # noqa: E402
import make_disc  # noqa: E402
from decode import Program  # noqa: E402
from test_recomp_disc import made_up_disc  # noqa: E402

BLR = 0x4e800020
CODE_AT = 0x80004000


def returning_program(count=2):
    """A program of `count` functions that each return at once, from CODE_AT on."""
    header = bytearray(0x100)
    struct.pack_into(">I", header, 0x00, 0x100)
    struct.pack_into(">I", header, 0x48, CODE_AT)
    struct.pack_into(">I", header, 0x90, 4 * count)
    return Program(bytes(header) + struct.pack(f">{count}I", *[BLR] * count))


class Folder(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)


class Tiers(Folder):
    def test_fast_tier_covers_the_share_of_runs_most_run_first(self):
        counts = {0x10: 900, 0x20: 90, 0x30: 9, 0x40: 1}
        self.assertEqual(emit.choose_fast(counts, share=0.9), {0x10})
        self.assertEqual(emit.choose_fast(counts, share=0.99), {0x10, 0x20})
        self.assertEqual(emit.choose_fast(counts, share=1.0), {0x10, 0x20, 0x30, 0x40})
        self.assertEqual(emit.choose_fast({}), set())

    def test_profile_skips_its_header_and_adds_repeats(self):
        path = self.folder / "profile.txt"
        path.write_text("# program 0000000000001234\n80004000 5\n80004100 2\n80004000 3\n")
        self.assertEqual(emit.read_profile(path), {0x80004000: 8, 0x80004100: 2})
        self.assertEqual(emit.read_profile(self.folder / "missing.txt"), {})

    def test_groups_split_by_tier_and_only_the_quick_tier_counts(self):
        program = returning_program()
        functions = {CODE_AT: {CODE_AT}, CODE_AT + 4: {CODE_AT + 4}}
        addresses = sorted(functions)
        (self.folder / "part0007.c").write_text("stale")
        written, _ = emit.write_groups(program, functions, addresses, {CODE_AT}, self.folder)
        self.assertEqual(written, {"part0000.c", "cold0000.c"})
        fast, quick = (self.folder / "part0000.c").read_text(), (self.folder / "cold0000.c").read_text()
        self.assertIn(f"f_{CODE_AT:x}", fast)
        self.assertNotIn("POLL_COUNTED", fast)
        self.assertIn(f"f_{CODE_AT + 4:x}", quick)
        self.assertIn("POLL_COUNTED(1);", quick)  # Its index in the function table.
        self.assertFalse((self.folder / "part0007.c").exists())

        # Everything fast, as without --profile: the quick group goes.
        written, _ = emit.write_groups(program, functions, addresses, set(addresses), self.folder)
        self.assertEqual(written, {"part0000.c"})
        self.assertFalse((self.folder / "cold0000.c").exists())

    def test_groups_come_out_the_same_on_every_core(self):
        count = emit.FUNCTIONS_PER_FILE * 2 + 7
        program = returning_program(count)
        functions = {CODE_AT + 4 * i: {CODE_AT + 4 * i} for i in range(count)}
        addresses = sorted(functions)
        fast = set(addresses[::3])
        outputs = []
        for jobs in (1, 3):
            out = self.folder / f"jobs{jobs}"
            out.mkdir()
            written, _ = emit.write_groups(program, functions, addresses, fast, out, jobs=jobs)
            outputs.append({name: (out / name).read_text() for name in written})
        self.assertEqual(len(outputs[0]), 6)  # Three groups, each with both tiers.
        self.assertEqual(outputs[0], outputs[1])

    def test_only_the_quick_tier_is_built_quickly(self):
        build = HERE.parent / "recomp" / f"build-test-{os.getpid()}"
        self.addCleanup(shutil.rmtree, build, ignore_errors=True)
        (build / "generated").mkdir(parents=True)
        for name in ("part0000.c", "cold0000.c", "table.c"):
            (build / "generated" / name).write_text("")
        subprocess.run([sys.executable, str(HERE.parent / "recomp/translate/build_file.py"), str(build)],
                       check=True, capture_output=True)
        rules = (build / "build.ninja").read_text()
        flags = {line.split(": cc generated/")[1]: following.split()[2]
                 for line, following in zip(rules.splitlines(), rules.splitlines()[1:]) if ": cc generated/" in line}
        self.assertEqual(flags, {"cold0000.c": "-O0", "part0000.c": "-O2", "table.c": "-O2"})


@unittest.skipUnless(shutil.which("clang"), "needs clang")
class Profile(Folder):
    def test_counts_add_up_across_runs_and_restart_for_another_program(self):
        program = self.folder / "program"
        subprocess.run(["clang", str(HERE / "c/test_profile.c"), str(RUNTIME / "profile.c"), "-w", "-O1", f"-I{RUNTIME}",
                        f"-I{HERE}", "-lpthread", "-o", str(program)], check=True, capture_output=True)
        data = self.folder / "data"
        saved = data / "00010004-54455354/profile.txt"

        def run(stage):
            result = subprocess.run([str(program), stage], capture_output=True, text=True, timeout=20,
                                    env={**os.environ, "WII_RECOMP_TITLE_DATA": str(data)})
            self.assertEqual(result.stdout, "ok\n", result.stdout + result.stderr)
            return saved.read_text().splitlines()

        self.assertEqual(run("first"), ["# program 0000000000001234", "80004000 5", "80004200 7"])
        self.assertEqual(run("second"), ["# program 0000000000001234", "80004000 6", "80004100 2", "80004200 7"])
        self.assertEqual(run("other"), ["# program 0000000000005678", "80004200 3"])


class SwapNative(Folder):
    def pack(self, library):
        disc = self.folder / "disc"
        if not disc.exists():
            made_up_disc(disc)
        image = self.folder / "game.wrd"
        make_disc.write_image(disc, image, [("linux-aarch64", library)])
        return image

    def native(self, image):
        rows = make_disc.read_table(image)
        (kind, _, at, size, name), = [row for row in rows if row[0] == make_disc.NATIVE]
        return name, image.read_bytes()[at:at + size]

    def test_native_code_is_swapped_and_the_disc_kept(self):
        first, second = self.folder / "first.so", self.folder / "second.so"
        first.write_bytes(b"A" * 100)
        second.write_bytes(b"B" * 5000)
        image = self.pack(first)
        disc_rows = [row for row in make_disc.read_table(image) if row[0] != make_disc.NATIVE]
        self.assertTrue(make_disc.replace_native(image, [("linux-aarch64", second)]))
        self.assertEqual(self.native(image), ("linux-aarch64", b"B" * 5000))
        self.assertEqual([row for row in make_disc.read_table(image) if row[0] != make_disc.NATIVE], disc_rows)
        self.assertTrue(make_disc.replace_native(image, [("linux-aarch64", first)]))  # Smaller again.
        self.assertEqual(self.native(image), ("linux-aarch64", b"A" * 100))
        self.assertEqual(image.stat().st_size % 32, 0)

    def test_an_image_without_native_code_is_left_alone(self):
        image = self.folder / "game.wrd"
        made_up_disc(self.folder / "disc")
        make_disc.write_image(self.folder / "disc", image, [])
        before = image.read_bytes()
        library = self.folder / "x.so"
        library.write_bytes(b"x")
        self.assertFalse(make_disc.replace_native(image, [("linux-aarch64", library)]))
        self.assertEqual(image.read_bytes(), before)


class AddGame(Folder):
    def test_this_computer_has_a_native_code_name(self):
        self.assertIn(add_game.native_platform(), make_disc.PLATFORMS)

    def test_game_and_title_ids_come_from_the_disc(self):
        disc = self.folder / "disc"
        made_up_disc(disc, game_id=b"TEST01")
        tmd = bytearray((disc / "tmd.bin").read_bytes())
        struct.pack_into(">II", tmd, add_game.TITLE_ID_OFFSET, 0x00010000, 0x54455354)
        (disc / "tmd.bin").write_bytes(tmd)
        add_game.check_disc(disc)
        self.assertEqual(add_game.game_id(disc), "TEST01")
        self.assertEqual(add_game.title_key(disc), "00010000-54455354")
        (disc / "sys/boot.bin").write_bytes(b"\0" * 0x440)
        with self.assertRaises(SystemExit):
            add_game.game_id(disc)
        with self.assertRaises(SystemExit):
            add_game.check_disc(self.folder)

    def test_profiles_from_every_console_add_up(self):
        first, second = self.folder / "a.txt", self.folder / "b.txt"
        first.write_text("# program 01\n80004000 5\n80004100 1\n")
        second.write_text("# program 01\n80004000 2\n")
        merged = self.folder / "merged.txt"
        self.assertEqual(add_game.merge_profiles([first, second], merged), 2)
        self.assertEqual(emit.read_profile(merged), {0x80004000: 7, 0x80004100: 1})

    def test_the_drive_holds_one_disc_and_the_other_goes_to_the_shelf(self):
        console = self.folder / "console"
        (console / "drive").mkdir(parents=True)
        (console / "drive/OLD001.wrd").write_bytes(b"old")
        image = self.folder / "NEW001.wrd"
        image.write_bytes(b"new")
        add_game.insert(image, console)
        self.assertEqual(sorted(path.name for path in (console / "drive").iterdir()), ["NEW001.wrd"])
        self.assertEqual((console / "drive/NEW001.wrd").read_bytes(), b"new")
        self.assertEqual((console / "shelf/OLD001.wrd").read_bytes(), b"old")
        add_game.insert(image, console)  # Already in: nothing changes.
        self.assertEqual((console / "drive/NEW001.wrd").read_bytes(), b"new")
        with self.assertRaises(SystemExit):
            add_game.insert(image, self.folder / "no-console")


if __name__ == "__main__":
    unittest.main()
