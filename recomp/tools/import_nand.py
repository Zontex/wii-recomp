#!/usr/bin/env python3
"""Decrypts a Wii's NAND backup into the storage folder Wii Recomp reads.

    python3 recomp/tools/import_nand.py [folder]

The folder is wii/ unless given. Put two files there, both made by BootMii on your own
Wii (Backup NAND):

    wii/nand.bin   the NAND backup
    wii/keys.bin   that console's keys. Not needed if they're at the end of nand.bin,
                   as BootMii's backups have them.

The decrypted storage goes to wii/nand/, the folder wii-recomp uses when none is named.
Running it again does nothing unless nand.bin changed; --force decrypts anyway. Both the
backup and its keys belong to one console and hold its owner's data, so wii/ is
ignored by git and must never be shared.

The layout follows Dolphin's NANDImporter.cpp (GPL-2.0-or-later): each 16 KB cluster is
AES-128-CBC with the console's NAND key and a zero IV, and the newest SFFS superblock
holds the cluster chain (FAT) and the file tree (FST).
"""
import argparse
import os
import shutil
import struct
import sys

PAGE_DATA, PAGE_SPARE, PAGES_PER_CLUSTER = 0x800, 0x40, 8
CLUSTER = PAGE_DATA * PAGES_PER_CLUSTER
CLUSTERS = 0x8000
SIZE_WITHOUT_SPARE = CLUSTERS * CLUSTER                            # 512 MB
SIZE_WITH_SPARE = CLUSTERS * PAGES_PER_CLUSTER * (PAGE_DATA + PAGE_SPARE)
KEYS_BYTES, NAND_KEY_AT = 0x400, 0x158                             # BootMii's keys.bin: the NAND key is in its OTP copy.
SUPERBLOCK_FIRST_CLUSTER, SUPERBLOCK_CLUSTERS, SUPERBLOCKS = 0x7f00, 16, 16
FAT_AT, FST_AT, FST_ENTRIES, FST_ENTRY = 12, 12 + 2 * CLUSTERS, 0x17ff, 0x20
LAST_CLUSTER, NO_ENTRY = 0xfffb, 0xffff
FILE, FOLDER = 1, 2


def stop(message):
    print(f"import_nand: {message}", file=sys.stderr)
    sys.exit(1)


def decryptor(key):
    """Decrypts one cluster: AES-128-CBC from a zero IV."""
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    except ImportError:
        stop("this needs Python's cryptography package: python3 -m pip install cryptography")

    def decrypt(data):
        worker = Cipher(algorithms.AES(key), modes.CBC(bytes(16))).decryptor()
        return worker.update(data) + worker.finalize()
    return decrypt


class Nand:
    def __init__(self, path):
        self.file = open(path, "rb")
        self.size = os.path.getsize(path)
        if self.size in (SIZE_WITH_SPARE, SIZE_WITH_SPARE + KEYS_BYTES):
            self.spare = True
        elif self.size in (SIZE_WITHOUT_SPARE, SIZE_WITHOUT_SPARE + KEYS_BYTES):
            self.spare = False
        else:
            stop(f"{path} is {self.size} bytes, which isn't a BootMii NAND backup")

    def keys_at_end(self):
        if self.size not in (SIZE_WITH_SPARE + KEYS_BYTES, SIZE_WITHOUT_SPARE + KEYS_BYTES):
            return None
        self.file.seek(self.size - KEYS_BYTES)
        return self.file.read(KEYS_BYTES)

    def cluster(self, number):
        """One cluster's 16 KB, without the spare bytes each page carries."""
        if not self.spare:
            self.file.seek(number * CLUSTER)
            return self.file.read(CLUSTER)
        page_bytes = PAGE_DATA + PAGE_SPARE
        self.file.seek(number * PAGES_PER_CLUSTER * page_bytes)
        raw = self.file.read(PAGES_PER_CLUSTER * page_bytes)
        return b"".join(raw[i * page_bytes:i * page_bytes + PAGE_DATA] for i in range(PAGES_PER_CLUSTER))


def newest_superblock(nand):
    best, best_version = None, -1
    for index in range(SUPERBLOCKS):
        first = SUPERBLOCK_FIRST_CLUSTER + index * SUPERBLOCK_CLUSTERS
        block = b"".join(nand.cluster(first + i) for i in range(SUPERBLOCK_CLUSTERS))
        if block[:4] == b"SFFS" and struct.unpack(">I", block[4:8])[0] > best_version:
            best, best_version = block, struct.unpack(">I", block[4:8])[0]
    if best is None:
        stop("no file system found in the backup: is it a Wii NAND?")
    return best


def entries(superblock):
    fat = struct.unpack(f">{CLUSTERS}H", superblock[FAT_AT:FAT_AT + 2 * CLUSTERS])
    tree = []
    for i in range(FST_ENTRIES):
        raw = superblock[FST_AT + i * FST_ENTRY:FST_AT + (i + 1) * FST_ENTRY]
        name = raw[:12].split(b"\0")[0].decode("latin-1")
        mode, _, sub, sibling, size = struct.unpack(">BBHHI", raw[12:22])
        tree.append((name, mode & 3, sub, sibling, size))
    return fat, tree


def safe_name(name):
    # Names come from the backup: keep them inside the folder.
    if not name or name in (".", "..") or "/" in name or "\\" in name or "\0" in name:
        return None
    return name


def extract(nand, decrypt, fat, tree, entry, folder, counts, seen):
    while entry != NO_ENTRY:
        if entry >= len(tree) or entry in seen:
            stop(f"the file tree is damaged at entry {entry}")
        seen.add(entry)
        name, kind, sub, sibling, size = tree[entry]
        target = safe_name(name)
        if target is None:
            print(f"import_nand: skipped an entry named {name!r}", file=sys.stderr)
        elif kind == FOLDER:
            path = os.path.join(folder, target)
            os.makedirs(path, exist_ok=True)
            extract(nand, decrypt, fat, tree, sub, path, counts, seen)
            counts["folders"] += 1
        elif kind == FILE:
            with open(os.path.join(folder, target), "wb") as out:
                cluster, left = sub, size
                while left > 0:
                    if cluster >= CLUSTERS:
                        stop(f"{name} runs off the end of the backup")
                    data = decrypt(nand.cluster(cluster))
                    out.write(data[:min(left, CLUSTER)])
                    left -= CLUSTER
                    cluster = fat[cluster]
            counts["files"] += 1
            counts["bytes"] += size
        entry = sibling


def main():
    parser = argparse.ArgumentParser(usage=__doc__)
    parser.add_argument("folder", nargs="?", default="wii")
    parser.add_argument("--force", action="store_true", help="decrypt even if the storage is up to date")
    options = parser.parse_args()
    backup, keys_path = os.path.join(options.folder, "nand.bin"), os.path.join(options.folder, "keys.bin")
    storage, stamp = os.path.join(options.folder, "nand"), os.path.join(options.folder, "nand", ".imported")
    if not os.path.exists(backup):
        stop(f"put your BootMii backup at {backup} (and keys.bin beside it if it has no keys at the end)")
    marker = f"{os.path.getsize(backup)} {int(os.path.getmtime(backup))}\n"
    if not options.force and os.path.exists(stamp) and open(stamp).read() == marker:
        print(f"{storage} is already up to date")
        return
    nand = Nand(backup)
    keys = open(keys_path, "rb").read() if os.path.exists(keys_path) else nand.keys_at_end()
    if keys is None:
        stop(f"no keys: put BootMii's keys.bin at {keys_path}")
    if len(keys) != KEYS_BYTES:
        stop(f"{keys_path} is {len(keys)} bytes; BootMii's keys.bin is {KEYS_BYTES}")
    decrypt = decryptor(keys[NAND_KEY_AT:NAND_KEY_AT + 16])
    fat, tree = entries(newest_superblock(nand))
    if tree[0][1] != FOLDER:
        stop("the file tree has no root folder")
    partial = storage + ".partial"
    shutil.rmtree(partial, ignore_errors=True)
    os.makedirs(partial)
    counts = {"files": 0, "folders": 0, "bytes": 0}
    extract(nand, decrypt, fat, tree, tree[0][2], partial, counts, set())
    # Every Wii has its certificate chain, which starts with a signature type of 0x10001.
    # The right key gives that back; a wrong one gives noise.
    certificates = os.path.join(partial, "sys", "cert.sys")
    if not os.path.exists(certificates) or open(certificates, "rb").read(4) != b"\x00\x01\x00\x01":
        shutil.rmtree(partial)
        stop("the keys don't decrypt this backup: are they from the same Wii?")
    with open(os.path.join(partial, ".imported"), "w") as out:
        out.write(marker)
    shutil.rmtree(storage, ignore_errors=True)
    os.rename(partial, storage)
    print(f"{storage}: {counts['files']} files in {counts['folders']} folders, {counts['bytes'] / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
