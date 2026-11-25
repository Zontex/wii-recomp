"""Recompiled disc images: packing a made-up extracted disc, and the drive reading it."""
import platform
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

import discs  # noqa: E402
import emit  # noqa: E402
import make_disc  # noqa: E402
from decode import Program  # noqa: E402

PROGRAM_AT, TABLE_AT, A_AT, B_AT = 0x3000, 0x4000, 0x5000, 0x6000
A, B = b"hello", b"world, again"


def this_platform():
    """As drive.c names it: Linux calls 64-bit Arm aarch64, the Mac arm64."""
    return f"{sys.platform}-{platform.machine()}"


def made_up_disc(root, game_id=b"TEST01", gamecube=False):
    """An extracted disc: a header pointing at a program and a file table with a file and a folder.
    A GameCube disc gives its offsets in bytes rather than words, and has no ticket, TMD or
    unencrypted area of its own."""
    system, files, disc = root / "sys", root / "files", root / "disc"
    for folder in (system, files / "d") + (() if gamecube else (disc,)):
        folder.mkdir(parents=True)
    shift = 0 if gamecube else 2
    names = b"a.bin\0d\0b.bin\0"
    table = struct.pack(">BBHII", 1, 0, 0, 0, 4)                 # Root, four entries.
    table += struct.pack(">I", 0) + struct.pack(">II", A_AT >> shift, len(A))           # a.bin
    table += struct.pack(">I", 1 << 24 | 6) + struct.pack(">II", 0, 4)                # d/, ends at 4
    table += struct.pack(">I", 8) + struct.pack(">II", B_AT >> shift, len(B))           # d/b.bin
    table += names
    boot = bytearray(0x440)
    boot[:6] = game_id
    struct.pack_into(">I", boot, *((0x1c, 0xc2339f3d) if gamecube else (0x18, 0x5d1c9ea3)))
    struct.pack_into(">III", boot, 0x420, PROGRAM_AT >> shift, TABLE_AT >> shift, len(table) >> shift)
    (system / "boot.bin").write_bytes(boot)
    (system / "bi2.bin").write_bytes(bytes(range(256)) * 32)
    (system / "apploader.img").write_bytes(b"\x11" * 0x100)
    program = bytearray(0x120)
    struct.pack_into(">I", program, 0x90, 0x20)  # One section of 0x20 bytes at offset 0.
    (system / "main.dol").write_bytes(program)
    (system / "fst.bin").write_bytes(table)
    (files / "a.bin").write_bytes(A)
    (files / "d/b.bin").write_bytes(B)
    if gamecube:
        return B_AT + len(B)
    (disc / "header.bin").write_bytes(boot[:0x100])
    (disc / "region.bin").write_bytes(bytes(0x20))
    (root / "ticket.bin").write_bytes(bytes(range(0x10)) + bytes(0x294))
    (root / "tmd.bin").write_bytes(b"\x7d" + bytes(0x207))
    (root / "cert.bin").write_bytes(bytes(0xa00))
    return B_AT + len(B)


class Disc(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)
        self.extracted = self.folder / "extracted"
        self.disc_bytes = made_up_disc(self.extracted)
        self.library = self.folder / "title.lib"
        self.library.write_text(this_platform())
        self.image = self.folder / "TEST01.wrd"
        make_disc.write_image(self.extracted, self.image, [(this_platform(), self.library), ("linux-x86_64", self.library)])

    def test_table(self):
        rows = make_disc.read_table(self.image)
        partition = sorted((disc_offset, name) for kind, disc_offset, _, _, name in rows if kind == make_disc.PARTITION)
        self.assertEqual(partition, [(0, "boot.bin"), (0x440, "bi2.bin"), (0x2440, "apploader.img"), (PROGRAM_AT, "main.dol"),
                                     (TABLE_AT, "fst.bin"), (A_AT, "a.bin"), (B_AT, "b.bin")])
        natives = [name for kind, _, _, _, name in rows if kind == make_disc.NATIVE]
        self.assertEqual(natives, [this_platform(), "linux-x86_64"])
        for kind, _, at, size, name in rows:
            self.assertEqual(at % make_disc.ALIGN, 0, name)
        data = self.image.read_bytes()
        a = next(row for row in rows if row[4] == "a.bin")
        self.assertEqual(data[a[2]:a[2] + a[3]], A)

    def test_overlapping_files_stop(self):
        table = bytearray((self.extracted / "sys/fst.bin").read_bytes())
        struct.pack_into(">I", table, 12 * 3 + 4, (A_AT + 4) >> 2)  # b.bin starts inside a.bin.
        (self.extracted / "sys/fst.bin").write_bytes(table)
        with self.assertRaises(SystemExit):
            make_disc.write_image(self.extracted, self.folder / "bad.wrd", [])
        self.assertFalse((self.folder / "bad.wrd").exists())

    @unittest.skipUnless(shutil.which("clang"), "needs clang")
    def test_drive_reads_image_like_folder(self):
        program = self.folder / "test_drive"
        sources = [HERE / "c/test_drive.c", RUNTIME / "drive.c", RUNTIME / "memory_watch.c", RUNTIME / "title.c"]
        subprocess.run(["clang", *map(str, sources), "-w", "-O1", f"-I{RUNTIME}", f"-I{HERE}", "-o", str(program)],
                       check=True, capture_output=True, text=True)
        system_title = self.folder / "menu"
        made_up_disc(system_title, game_id=bytes(6))
        result = subprocess.run([str(program), str(self.image), str(self.extracted / "sys"), str(self.folder / "cache"),
                                 hex(self.disc_bytes), str(system_title / "sys")], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.endswith("ok\n"), result.stdout)


class GameCubeDisc(unittest.TestCase):
    """A GameCube disc packs as one partition at its own byte offsets, with nothing else."""
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)
        self.extracted = self.folder / "extracted"
        made_up_disc(self.extracted, game_id=b"GTST01", gamecube=True)

    def test_table(self):
        image = self.folder / "GTST01.wrd"
        library = self.folder / "title.lib"
        library.write_text(this_platform())
        make_disc.write_image(self.extracted, image, [(this_platform(), library)])
        rows = make_disc.read_table(image)
        self.assertEqual(sorted((disc_offset, name) for kind, disc_offset, _, _, name in rows if kind == make_disc.PARTITION),
                         [(0, "boot.bin"), (0x440, "bi2.bin"), (0x2440, "apploader.img"), (PROGRAM_AT, "main.dol"),
                          (TABLE_AT, "fst.bin"), (A_AT, "a.bin"), (B_AT, "b.bin")])
        self.assertEqual(sorted(kind for kind, *_ in rows if kind != make_disc.PARTITION), [make_disc.NATIVE])

    def test_title_from_the_game_id(self):
        self.assertTrue(discs.is_gamecube_folder(self.extracted))
        self.assertEqual(discs.title(self.extracted), (0x00010000, 0x47545354, 0))  # "GTST", and no IOS.

    @unittest.skipUnless(shutil.which("clang"), "needs clang")
    def test_drive_reads_it_at_byte_offsets(self):
        image, library = self.folder / "GTST01.wrd", self.folder / "title.lib"
        library.write_text(this_platform())
        make_disc.write_image(self.extracted, image, [(this_platform(), library)])
        made_up_disc(self.folder / "wii")
        program = self.folder / "test_drive_gamecube"
        sources = [HERE / "c/test_drive_gamecube.c", RUNTIME / "drive.c", RUNTIME / "memory_watch.c", RUNTIME / "title.c"]
        subprocess.run(["clang", *map(str, sources), "-w", "-O1", f"-I{RUNTIME}", f"-I{HERE}", "-o", str(program)],
                       check=True, capture_output=True, text=True)
        result = subprocess.run([str(program), str(image), str(self.extracted / "sys"), hex(B_AT + len(B)), hex(A_AT), A.decode(),
                                 str(self.folder / "wii/sys")], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.endswith("ok\n"), result.stdout)

    def test_a_wii_disc_is_not_a_gamecube_one(self):
        made_up_disc(self.folder / "wii")
        self.assertFalse(discs.is_gamecube_folder(self.folder / "wii"))
        (self.folder / "wii/tmd.bin").unlink()
        with self.assertRaises(SystemExit):
            discs.title(self.folder / "wii")


class Apploader(unittest.TestCase):
    def test_translated_with_the_game(self):
        """An entry that hands back its main function's address, as an apploader's does."""
        with tempfile.TemporaryDirectory() as folder:
            system, out = Path(folder) / "sys", Path(folder) / "out"
            system.mkdir()
            out.mkdir()
            main = emit.APPLOADER_AT + 0x10
            code = struct.pack(">6I", 0x3c000000 | main >> 16, 0x60000000 | main & 0xffff,  # lis r0, ori r0
                               0x90030000, 0x4e800020,                                          # stw r0,0(r3); blr
                               0x38600001, 0x4e800020)                                          # main: li r3,1; blr
            (system / "apploader.img").write_bytes(b"2009/02/27".ljust(16, b"\0") + struct.pack(">III", emit.APPLOADER_AT, len(code), 0)
                                                   + bytes(4) + code)
            game = bytearray(0x200)
            struct.pack_into(">I", game, 0, 0x100)
            struct.pack_into(">I", game, 0x48, 0x80004000)
            struct.pack_into(">I", game, 0x90, 0x100)
            size, check, count = emit.emit_apploader(system, out, Program(bytes(game)))
            self.assertEqual((size, check, count), (len(code), emit.fnv1a64(code), 2))
            text = (out / "apploader.c").read_text()
            self.assertIn(f"{{0x{main:x}u,f_{main:08x}}}", text)


if __name__ == "__main__":
    unittest.main()
