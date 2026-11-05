#!/usr/bin/env python3
"""Pack an extracted Wii disc and its native code into one recompiled disc image.

make_disc.py <extracted folder> <image.wrd> --native <platform>=<title library> [--native ...]

The image holds the disc as the game and the Wii Menu read it, at its original
offsets, plus the title's translated code for each platform it was built for, such as
darwin-x86_64 or linux-aarch64. The console's drive reads it like a disc; a folder
holding one stands in for a drive with that disc inserted, and a burned disc in a real
drive works the same way. Nothing in it is encrypted, and it needs no keys.

Layout, little-endian:
  header, 64 bytes: "WIIRDISC", format version (u32), entry count (u32), table offset (u64)
  data, each entry's bytes aligned to 32
  table, 64 bytes an entry: kind (u32), 0 (u32), disc offset (u64), offset (u64), size (u64), name (32 bytes)
Kinds: 1 partition data at a disc offset, 2 the disc's unencrypted area at a raw offset,
3 ticket, 4 TMD, 5 certificates, 6 native code (name: platform), 7 shader recipes for
any platform (recomp/runtime/gx_recipes.c). Native code and recipes come last.
"""
import argparse
import os
import struct
import sys
from pathlib import Path

MAGIC, VERSION = b"WIIRDISC", 1
HEADER = struct.Struct("<8sIIQ40x")
ENTRY = struct.Struct("<IIQQQ32s")
PARTITION, RAW, TICKET, TMD, CERTIFICATES, NATIVE, SHADERS = 1, 2, 3, 4, 5, 6, 7
TAIL = {NATIVE, SHADERS}  # Written last, as they change with each build.
ALIGN = 32
BOOT_DOL_OFFSET, BOOT_FST_OFFSET = 0x420, 0x424
BI2_AT, APPLOADER_AT = 0x440, 0x2440
REGION_AT = 0x4e000  # The region setting in the disc's unencrypted area.
PLATFORMS = {"darwin-x86_64", "darwin-arm64", "linux-aarch64", "linux-x86_64"}


def word(data, at):
    return struct.unpack_from(">I", data, at)[0]


def partition_files(extracted):
    """(disc offset, path) for everything in the data partition, from its own file table."""
    system = extracted / "sys"
    boot = (system / "boot.bin").read_bytes()
    table = (system / "fst.bin").read_bytes()
    found = [(0, system / "boot.bin"), (BI2_AT, system / "bi2.bin"), (APPLOADER_AT, system / "apploader.img"),
             (word(boot, BOOT_DOL_OFFSET) << 2, system / "main.dol"), (word(boot, BOOT_FST_OFFSET) << 2, system / "fst.bin")]
    total = word(table, 8)
    names = table[total * 12:]

    def walk(index, end, folder):
        while index < end:
            entry = table[index * 12:index * 12 + 12]
            name_at = word(entry, 0) & 0xffffff
            name = names[name_at:names.index(b"\0", name_at)].decode("latin-1")
            if entry[0]:
                index = walk(index + 1, word(entry, 8), folder / name)
            else:
                found.append((word(entry, 4) << 2, folder / name))
                index += 1
        return index

    walk(1, total, extracted / "files")
    return sorted(found)


def check_no_overlap(entries):
    last_end, last_name = 0, None
    for offset, path in entries:
        if offset < last_end:
            raise SystemExit(f"{path.name} at {offset:#x} overlaps {last_name}: the extracted disc isn't what its file table says")
        last_end, last_name = offset + path.stat().st_size, path.name


def write_image(extracted, output, natives, shaders=None):
    files = partition_files(extracted)
    check_no_overlap(files)
    disc = extracted / "disc"
    pieces = [(PARTITION, offset, path, path.name) for offset, path in files]
    pieces += [(RAW, 0, disc / "header.bin", "header.bin"), (RAW, REGION_AT, disc / "region.bin", "region.bin")]
    pieces += [(TICKET, 0, extracted / "ticket.bin", "ticket.bin"), (TMD, 0, extracted / "tmd.bin", "tmd.bin"),
               (CERTIFICATES, 0, extracted / "cert.bin", "cert.bin")]
    pieces += [(NATIVE, 0, Path(library), platform) for platform, library in natives]
    pieces += [(SHADERS, 0, Path(shaders), "shaders")] if shaders else []
    missing = [str(path) for _, _, path, _ in pieces if not path.is_file()]
    if missing:
        raise SystemExit("missing: " + ", ".join(missing))
    partial = output.with_name(output.name + ".partial")
    table = []
    with open(partial, "wb") as out:
        out.write(bytes(HEADER.size))
        for kind, disc_offset, path, name in pieces:
            out.write(bytes(-out.tell() % ALIGN))
            at = out.tell()
            with open(path, "rb") as source:
                while chunk := source.read(1 << 20):
                    out.write(chunk)
            table.append(ENTRY.pack(kind, 0, disc_offset, at, out.tell() - at, name.encode("utf-8")[:31]))
        out.write(bytes(-out.tell() % ALIGN))
        table_at = out.tell()
        out.write(b"".join(table))
        out.seek(0)
        out.write(HEADER.pack(MAGIC, VERSION, len(table), table_at))
    partial.replace(output)  # Only a finished image gets the real name.
    return len(table)


def replace_native(image, natives, shaders=None):
    """Swaps the image's native code for `natives`, and its shader recipes for the file
    `shaders` if given, without copying the disc again. Recipes not replaced are kept.

    These are written last, so everything from the first of them on is cut off and
    written again, then the table, then the header pointing at it. Returns False,
    changing nothing, if the image isn't laid out that way."""
    rows = read_table(image)
    first = min((at for kind, _, at, _, _ in rows if kind in TAIL), default=None)
    if first is None or any(at >= first for kind, _, at, _, _ in rows if kind not in TAIL):
        return False
    kept = [row for row in rows if row[0] not in TAIL]
    missing = [library for _, library in natives if not Path(library).is_file()]
    missing += [shaders] if shaders and not Path(shaders).is_file() else []
    if missing:
        raise SystemExit("missing: " + ", ".join(map(str, missing)))
    tail = [(NATIVE, platform, Path(library).read_bytes()) for platform, library in natives]
    recipes = Path(shaders).read_bytes() if shaders else read_shaders(image)
    tail += [(SHADERS, "shaders", recipes)] if recipes else []
    with open(image, "r+b") as out:
        out.truncate(first)
        out.seek(first)
        table = [ENTRY.pack(kind, 0, disc_offset, at, size, name.encode("utf-8")[:31]) for kind, disc_offset, at, size, name in kept]
        for kind, name, data in tail:
            out.write(bytes(-out.tell() % ALIGN))
            at = out.tell()
            out.write(data)
            table.append(ENTRY.pack(kind, 0, 0, at, out.tell() - at, name.encode("utf-8")[:31]))
        out.write(bytes(-out.tell() % ALIGN))
        table_at = out.tell()
        out.write(b"".join(table))
        out.flush()
        os.fsync(out.fileno())
        out.seek(0)
        out.write(HEADER.pack(MAGIC, VERSION, len(table), table_at))
    return True


def read_shaders(image):
    """The shader recipes the image carries, or None."""
    for kind, _, at, size, _ in read_table(image):
        if kind == SHADERS:
            with open(image, "rb") as source:
                source.seek(at)
                return source.read(size)
    return None


def read_table(path):
    """The image's entries, as (kind, disc offset, offset, size, name)."""
    with open(path, "rb") as image:
        magic, version, count, table_at = HEADER.unpack(image.read(HEADER.size))
        if magic != MAGIC or version != VERSION:
            raise SystemExit(f"{path} isn't a recompiled disc image of version {VERSION}")
        image.seek(table_at)
        rows = [ENTRY.unpack(image.read(ENTRY.size)) for _ in range(count)]
    return [(kind, disc_offset, at, size, name.rstrip(b"\0").decode("utf-8")) for kind, _, disc_offset, at, size, name in rows]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("extracted", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--native", action="append", default=[], metavar="PLATFORM=LIBRARY")
    options = parser.parse_args()
    natives = []
    for item in options.native:
        platform, _, library = item.partition("=")
        if platform not in PLATFORMS or not library:
            raise SystemExit(f"--native wants one of {', '.join(sorted(PLATFORMS))}=<library>, not {item}")
        natives.append((platform, library))
    if not natives:
        print("note: no native code given, so the image can be shown but not played", file=sys.stderr)
    count = write_image(options.extracted, options.output, natives)
    print(f"{options.output}: {count} entries, {options.output.stat().st_size / 1e9:.2f} GB")


if __name__ == "__main__":
    main()
