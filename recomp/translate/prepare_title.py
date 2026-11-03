#!/usr/bin/env python3
"""Turn an installed title on a Wii storage folder into the layout the translator reads.

System titles such as the Wii Menu aren't on a disc. Their program is one of the title's
contents, and the menu's is an unusual one: its entry is a physical address, and its code
sits in a section marked as data. This writes a copy with those fixed, plus an empty disc
for the runtime. The storage folder is only read.

Usage: recomp/translate/prepare_title.py <Wii storage folder> <title high> <title low> <output folder>
Example: recomp/translate/prepare_title.py /path/to/NAND 00000001 00000002 recomp/build-menu/disc
"""
import struct
import sys
from pathlib import Path

BOOT_INDEX_OFFSET = 0x1e0
CONTENT_COUNT_OFFSET = 0x1de
CONTENTS_OFFSET = 0x1e4
CONTENT_BYTES = 36
CONTENT_INDEX_OFFSET = 4

HEADER_WORDS = 64
TEXT_SECTIONS = 7
ALL_SECTIONS = 18
OFFSETS, ADDRESSES, SIZES = 0, 18, 36
ENTRY = 56
CACHED_MEMORY = 0x80000000
BOOT_STUB_TARGET = 0x800037fc  # The stub ends by jumping to the address stored here.

EMPTY_BOOT_BYTES = 0x440
EMPTY_FILE_TABLE = struct.pack(">BBHII", 1, 0, 0, 0, 1) + b"\0"


def boot_content_name(tmd):
    """The boot content's file name, from the title's metadata."""
    boot_index = struct.unpack_from(">H", tmd, BOOT_INDEX_OFFSET)[0]
    count = struct.unpack_from(">H", tmd, CONTENT_COUNT_OFFSET)[0]
    for number in range(count):
        record = CONTENTS_OFFSET + number * CONTENT_BYTES
        if struct.unpack_from(">H", tmd, record + CONTENT_INDEX_OFFSET)[0] == boot_index:
            return f"{struct.unpack_from('>I', tmd, record)[0]:08x}.app"
    raise SystemExit("The title has no boot content")


def next_free_text_section(header):
    for section in range(TEXT_SECTIONS):
        if not header[SIZES + section]:
            return section
    raise SystemExit("No free text section")


def word_at(program, header, address):
    """Reads a word from the program by the address it loads at."""
    for section in range(ALL_SECTIONS):
        start, size = header[ADDRESSES + section], header[SIZES + section]
        if start <= address < start + size:
            return struct.unpack_from(">I", program, header[OFFSETS + section] + address - start)[0]
    raise SystemExit(f"Nothing loads at {address:08x}")


def program_with_code_marked(program):
    """Lists every section holding code as text, and enters at the program's real start."""
    header = list(struct.unpack_from(f">{HEADER_WORDS}I", program))
    for section in range(TEXT_SECTIONS, ALL_SECTIONS):
        if header[SIZES + section]:
            text = next_free_text_section(header)
            for field in (OFFSETS, ADDRESSES, SIZES):
                header[field + text] = header[field + section]
                header[field + section] = 0
    if header[ENTRY] < CACHED_MEMORY:  # A boot stub: start where it would go, as it only sets up memory mapping.
        header[ENTRY] = word_at(program, header, BOOT_STUB_TARGET)
    return struct.pack(f">{HEADER_WORDS}I", *header) + program[HEADER_WORDS * 4:]


def main():
    storage, high, low, output = Path(sys.argv[1]), sys.argv[2], sys.argv[3], Path(sys.argv[4])
    content = storage / "title" / high / low / "content"
    tmd = (content / "title.tmd").read_bytes()
    system = output / "sys"
    system.mkdir(parents=True, exist_ok=True)
    (output / "tmd.bin").write_bytes(tmd)
    (system / "main.dol").write_bytes(program_with_code_marked((content / boot_content_name(tmd)).read_bytes()))
    (system / "boot.bin").write_bytes(bytes(EMPTY_BOOT_BYTES))
    (system / "fst.bin").write_bytes(EMPTY_FILE_TABLE)
    (output / "files").mkdir(exist_ok=True)


if __name__ == "__main__":
    main()
