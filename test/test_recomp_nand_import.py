"""import_nand.py on a made-up NAND backup: a few files encrypted with a made-up key, in
BootMii's layout, decrypted back into a storage folder."""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).parent
TOOL = HERE.parent / "recomp/tools/import_nand.py"
sys.path.insert(0, str(TOOL.parent))
import import_nand as nand  # noqa: E402

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:
    Cipher = None

KEY = bytes(range(16))


def encrypt(data):
    worker = Cipher(algorithms.AES(KEY), modes.CBC(bytes(16))).encryptor()
    return worker.update(data) + worker.finalize()


def build_backup(path, files, keys_at_end=True, spare=True):
    """A backup holding files ({path: bytes}) under the root, with sys/cert.sys first."""
    clusters = {}
    fat = [0xfffe] * nand.CLUSTERS
    tree = [(b"/", nand.FOLDER, 0xffff, 0xffff, 0)]
    next_cluster = [16]

    def add_file(name, data):
        first = previous = None
        for start in range(0, len(data), nand.CLUSTER):
            number = next_cluster[0]
            next_cluster[0] += 1
            clusters[number] = encrypt(data[start:start + nand.CLUSTER].ljust(nand.CLUSTER, b"\0"))
            if previous is not None:
                fat[previous] = number
            first = number if first is None else first
            previous = number
        fat[previous] = nand.LAST_CLUSTER
        tree.append((name.encode(), nand.FILE, first, 0xffff, len(data)))
        return len(tree) - 1

    def add_folder(name):
        tree.append((name.encode(), nand.FOLDER, 0xffff, 0xffff, 0))
        return len(tree) - 1

    def link(parent, children):
        for index, child in enumerate(children):
            if index == 0:
                tree[parent] = tree[parent][:2] + (child,) + tree[parent][3:]
            else:
                before = children[index - 1]
                tree[before] = tree[before][:3] + (child,) + tree[before][4:]

    folders = {}
    for full, data in files.items():
        parts = full.split("/")
        parent = 0
        for depth in range(len(parts) - 1):
            key = "/".join(parts[:depth + 1])
            if key not in folders:
                folders[key] = (add_folder(parts[depth]), [])
                (folders["/".join(parts[:depth])][1] if depth else root_children).append(folders[key][0])
            parent = folders[key][0]
        index = add_file(parts[-1], data)
        (folders["/".join(parts[:-1])][1] if len(parts) > 1 else root_children).append(index)
    for number, children in folders.values():
        link(number, children)
    link(0, root_children)

    block = bytearray(nand.SUPERBLOCK_CLUSTERS * nand.CLUSTER)
    block[0:12] = b"SFFS" + struct.pack(">II", 7, 0)
    block[nand.FAT_AT:nand.FAT_AT + 2 * nand.CLUSTERS] = struct.pack(f">{nand.CLUSTERS}H", *fat)
    for i, (name, kind, sub, sibling, size) in enumerate(tree):
        at = nand.FST_AT + i * nand.FST_ENTRY
        block[at:at + 22] = name.ljust(12, b"\0") + struct.pack(">BBHHI", kind, 0, sub, sibling, size)
    for i in range(nand.SUPERBLOCK_CLUSTERS):
        clusters[nand.SUPERBLOCK_FIRST_CLUSTER + i] = bytes(block[i * nand.CLUSTER:(i + 1) * nand.CLUSTER])

    with open(path, "wb") as out:
        page = nand.PAGE_DATA + (nand.PAGE_SPARE if spare else 0)
        out.truncate(nand.CLUSTERS * nand.PAGES_PER_CLUSTER * page)
        for number, data in clusters.items():
            out.seek(number * nand.PAGES_PER_CLUSTER * page)
            for p in range(nand.PAGES_PER_CLUSTER):
                out.write(data[p * nand.PAGE_DATA:(p + 1) * nand.PAGE_DATA])
                if spare:
                    out.write(bytes(nand.PAGE_SPARE))
        if keys_at_end:
            out.seek(0, os.SEEK_END)
            keys = bytearray(nand.KEYS_BYTES)
            keys[nand.NAND_KEY_AT:nand.NAND_KEY_AT + 16] = KEY
            out.write(bytes(keys))


root_children = []


@unittest.skipUnless(Cipher, "needs Python's cryptography package")
class NandImport(unittest.TestCase):
    def setUp(self):
        root_children.clear()
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def run_tool(self, *extra):
        return subprocess.run([sys.executable, str(TOOL), str(self.folder), *extra], capture_output=True, text=True)

    def test_files_come_back_decrypted(self):
        long = bytes(i % 251 for i in range(40000))  # Three clusters, through the chain.
        files = {"sys/cert.sys": b"\x00\x01\x00\x01certificates", "title/00000001/data/setting.txt": b"AREA=EUR", "shared2/long": long}
        build_backup(self.folder / "nand.bin", files)
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        storage = self.folder / "nand"
        for name, data in files.items():
            self.assertEqual((storage / name).read_bytes(), data, name)
        self.assertIn("already up to date", self.run_tool().stdout)

    def test_keys_beside_the_backup(self):
        build_backup(self.folder / "nand.bin", {"sys/cert.sys": b"\x00\x01\x00\x01"}, keys_at_end=False, spare=False)
        self.assertIn("no keys", self.run_tool().stderr)
        keys = bytearray(nand.KEYS_BYTES)
        keys[nand.NAND_KEY_AT:nand.NAND_KEY_AT + 16] = KEY
        (self.folder / "keys.bin").write_bytes(bytes(keys))
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_wrong_keys_are_refused_and_leave_nothing(self):
        build_backup(self.folder / "nand.bin", {"sys/cert.sys": b"\x00\x01\x00\x01"})
        (self.folder / "keys.bin").write_bytes(bytes(nand.KEYS_BYTES))
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("same Wii", result.stderr)
        self.assertFalse((self.folder / "nand").exists())


if __name__ == "__main__":
    unittest.main()
