"""Compiles small C harnesses around the hand-written runtime and runs them."""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).parent
RUNTIME = HERE.parent / "recomp/runtime"
FLAGS = ["-w", "-O1", "-fwrapv", f"-I{RUNTIME}", f"-I{HERE}", "-lm"]
# dispatch.c keeps discovered code in the title's data folder, from profile.c.
INTERPRETER = [HERE / "c/test_interp.c", RUNTIME / "dispatch.c", RUNTIME / "interp.c", RUNTIME / "quantized.c", RUNTIME / "modules.c", RUNTIME / "profile.c"]


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

    def test_guest_memory_window(self):
        self.build_and_run([HERE / "c/test_guest_memory.c", RUNTIME / "guest_memory.c", RUNTIME / "memory_watch.c"])

    def test_copy_encoding_and_efb_values(self):
        self.build_and_run([HERE / "c/test_encode.c", RUNTIME / "gx_encode.c"])

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

    def run_gaps(self, **environment):
        program = self.compile([HERE / "c/test_gaps.c", RUNTIME / "gaps.c"])
        listing = self.folder / "gaps.txt"
        result = subprocess.run([str(program)], capture_output=True, text=True, timeout=20,
                                env={**os.environ, "WII_RECOMP_GAPS": str(listing), **environment})
        return result, listing

    def test_gaps_log_once_and_count_in_the_file(self):
        result, listing = self.run_gaps()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(re.findall(r"^gap: .*$", result.stderr, re.M), ["gap: ios.setattr 0x0", "gap: ios.setattr 0x2"])
        lines = listing.read_text().splitlines()
        self.assertEqual(lines[0], "# id detail count first_lr")
        self.assertEqual(sorted(lines[1:]), ["cpu.sc 0x5 2 0x80001234", "ios.setattr 0x0 2 0x80001234",
                                             "ios.setattr 0x2 1 0x80001234"])

    def test_gaps_strict_mode_stops_at_the_first(self):
        result, listing = self.run_gaps(WII_RECOMP_GAPS_STRICT="1")
        self.assertEqual(result.returncode, 12, result.stderr)
        self.assertIn("stop: ios.setattr isn't done yet", result.stderr)
        self.assertEqual(listing.read_text().splitlines()[1:], ["ios.setattr 0x0 1 0x80001234"])

    def test_clock_chip_on_the_expansion_bus(self):
        self.build_and_run([HERE / "c/test_exi.c"])

    def test_system_calls_and_floating_point_modes(self):
        self.build_and_run([HERE / "c/test_processor.c", RUNTIME / "processor.c"])

    def test_helpers_shared_with_translated_code(self):
        self.build_and_run([HERE / "c/test_cpu_helpers.c", RUNTIME / "quantized.c"])

    def test_clock(self):
        self.build_and_run([HERE / "c/test_clock.c"])

    def test_fifo_ring_and_bursts(self):
        self.build_and_run([HERE / "c/test_fifo.c"])

    def test_attachment_encryption_and_layout(self):
        self.build_and_run([HERE / "c/test_attachment.c"])

    def test_camera_dots_and_formats(self):
        self.build_and_run([HERE / "c/test_camera.c"])

    def test_board_calibration_and_readings(self):
        self.build_and_run([HERE / "c/test_board.c"])

    def test_storage_is_never_written(self):
        storage, saves = self.folder / "storage", self.folder / "saves"
        (storage / "shared2").mkdir(parents=True)
        (storage / "shared2/only_storage").write_text("old")
        (storage / "shared2/read_only").write_text("keep")
        saves.mkdir()
        before = tree(storage)
        os.chmod(storage / "shared2/read_only", 0o444)
        self.build_and_run([HERE / "c/test_storage.c", RUNTIME / "fs.c", RUNTIME / "files.c", RUNTIME / "gaps.c"], storage, saves)
        os.chmod(storage / "shared2/read_only", 0o644)
        self.assertEqual({k: v[0] for k, v in tree(storage).items()}, {k: v[0] for k, v in before.items()})
        self.assertEqual((saves / "shared2/only_storage").read_text()[:3], "new")

    def test_system_files_made_on_a_new_wii(self):
        storage, saves = self.folder / "storage", self.folder / "saves"
        (storage / "shared2/wc24").mkdir(parents=True)
        (storage / "shared2/wc24/nwc24msg.cfg").write_bytes(b"WcCf" + bytes(0x3fc))
        saves.mkdir()
        before = tree(storage)
        self.build_and_run([HERE / "c/test_system_files.c", RUNTIME / "system_files.c", RUNTIME / "fs.c",
                            RUNTIME / "files.c", RUNTIME / "gaps.c"], storage, saves)
        self.assertEqual(tree(storage), before)

    def test_modules_loaded_at_run_time(self):
        sources = [HERE / "c/test_modules.c", RUNTIME / "dispatch.c", RUNTIME / "interp.c", RUNTIME / "quantized.c", RUNTIME / "profile.c"]
        self.build_and_run(sources, "match")
        program = self.compile(sources)
        self.build_and_run(sources, "rel")
        for mode, message in (("changed", "isn't what was translated"), ("no-native-import", "has no native code"),
                              ("rel-elsewhere", "where it was translated to call")):
            result = subprocess.run([str(program), mode], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
            self.assertIn(message, result.stderr)

    def test_installed_titles(self):
        storage, saves = self.folder / "storage", self.folder / "saves"
        make_storage(storage)
        saves.mkdir()
        self.build_and_run([HERE / "c/test_titles.c", RUNTIME / "titles.c", RUNTIME / "files.c"], storage, saves)

    def test_es_requests(self):
        storage, saves, disc = self.folder / "storage", self.folder / "saves", self.folder / "disc"
        make_storage(storage)
        saves.mkdir()
        (disc / "sys").mkdir(parents=True)
        (disc / "files").mkdir()
        (disc / "sys/boot.bin").write_bytes(b"SAAA01".ljust(0x440, b"\0"))  # A disc is in.
        (disc / "sys/fst.bin").write_bytes(struct.pack(">BBHII", 1, 0, 0, 0, 1) + b"\0")
        (disc / "sys/main.dol").write_bytes(bytes(0x100))
        disc_ticket = bytearray(ticket(0x0001000053414141))
        struct.pack_into(">I", disc_ticket, 0x1d0, 0x99887766)
        (disc / "ticket.bin").write_bytes(disc_ticket)
        sources = ["test_es.c", "es.c", "es_titles.c", "es_content.c", "titles.c", "files.c", "drive.c", "memory_watch.c", "title.c"]
        self.build_and_run([HERE / "c" / sources[0]] + [RUNTIME / name for name in sources[1:]], storage, saves, disc / "sys")


def tmd(title, contents, ios=0x0000000100000050, version=0x1234, boot=1):
    """A made-up TMD: header fields only, no real signature. contents: (id, index, type, data)."""
    header = bytearray(0x1e4)
    header[0x180] = 1
    struct.pack_into(">QQ", header, 0x184, ios, title)
    struct.pack_into(">HHH", header, 0x1dc, version, len(contents), boot)
    for content_id, index, kind, data in contents:
        header += struct.pack(">IHHQ", content_id, index, kind, len(data)) + hashlib.sha1(data).digest()
    return bytes(header)


def ticket(title):
    data = bytearray(0x2a4)
    struct.pack_into(">QIQ", data, 0x1d0, 0x1122334455667788, 0, title)
    return bytes(data)


def make_storage(root):
    """A made-up console storage: a menu with a private and a shared content and a
    ticket, a channel with no ticket, and a channel whose TMD is cut short."""
    private, shared = b"private", b"share"
    menu = root / "title/00000001/00000002/content"
    menu.mkdir(parents=True)
    (menu / "title.tmd").write_bytes(tmd(0x0000000100000002, [(0x1f, 0, 1, private), (0x20, 1, 0x8001, shared)]))
    (menu / "0000001f.app").write_bytes(private)
    (root / "shared1").mkdir()
    (root / "shared1/00000007.app").write_bytes(shared)
    (root / "shared1/content.map").write_bytes(b"00000006" + bytes(20) + b"00000007" + hashlib.sha1(shared).digest())
    (root / "ticket/00000001").mkdir(parents=True)
    (root / "ticket/00000001/00000002.tik").write_bytes(ticket(0x0000000100000002))
    channel = root / "title/00010001/48414141/content"
    channel.mkdir(parents=True)
    (channel / "title.tmd").write_bytes(tmd(0x0001000148414141, []))
    broken = root / "title/00010001/48424242/content"
    broken.mkdir(parents=True)
    (broken / "title.tmd").write_bytes(tmd(0x0001000148424242, [(1, 0, 1, b"x")])[:-10])
    (root / "title/00010001/48434343").mkdir(parents=True)  # A save with no title installed.


if __name__ == "__main__":
    unittest.main()
