#!/usr/bin/env python3
"""Find where a game's Wii Remote library keeps its attachment decryption tables.

The library works out two 8-byte tables from the key it sends to an attachment, and keeps
them in a block for each remote. Its decrypt routine reads them at two fixed offsets, 8
bytes apart, from a block it finds through a table of pointers. That shape is what's
searched for here, as the addresses and offsets differ between SDK versions.

Usage: recomp/translate/remote_tables.py extracted/sys/main.dol
"""
import sys
from collections import Counter
from pathlib import Path

from decode import Program, name_of
from functions import find_functions

LOAD_BYTE = 34
ADD_IMMEDIATE = 14
ADD_IMMEDIATE_SHIFTED = 15
TABLE_BYTES = 8
MIN_XORS = 8
MIN_BLOCK_OFFSET = 0x100


def signed16(word):
    value = word & 0xffff
    return value - 0x10000 if value & 0x8000 else value


def built_addresses(program, body):
    """Addresses made by a lis and an addi."""
    high, found = {}, []
    for address in sorted(body):
        word = program.word(address)
        primary, target, source = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        if primary == ADD_IMMEDIATE_SHIFTED and source == 0:
            high[target] = (word & 0xffff) << 16
        elif primary == ADD_IMMEDIATE and source in high:
            found.append((high[source] + signed16(word)) & 0xffffffff)
    return found


def tables_in(program, body):
    """Returns (add offset, xor offset) if this function reads two tables 8 bytes apart."""
    names = Counter(name_of(program.word(address)) for address in body)
    if names["xor"] < MIN_XORS or not names["lwzx"]:
        return None
    offsets = Counter(program.word(address) & 0xffff for address in body if program.word(address) >> 26 == LOAD_BYTE)
    for add_offset in sorted(offsets):
        # The routine adds each byte's position to its base, so one offset is read eight times.
        if add_offset >= MIN_BLOCK_OFFSET and offsets[add_offset] >= TABLE_BYTES and offsets[add_offset + TABLE_BYTES] >= TABLE_BYTES:
            return add_offset, add_offset + TABLE_BYTES
    return None


def find_remote_tables(program, functions):
    """Returns (pointer table, add offset, xor offset), or None if the game has no such code."""
    matches = []
    for body in functions.values():
        tables = tables_in(program, body)
        if tables:
            built = [address for address in built_addresses(program, body) if address >= 0x80000000]
            if len(built) >= 1:
                matches.append((built[0], *tables))
    found = set(matches)
    if len(found) > 1:
        raise SystemExit("Found more than one set of attachment tables: " + ", ".join(hex(m[0]) for m in found))
    return found.pop() if found else None


def main():
    program = Program(Path(sys.argv[1]).read_bytes())
    functions, _ = find_functions(program)
    found = find_remote_tables(program, functions)
    print("not found" if not found else f"blocks 0x{found[0]:08x}, add +0x{found[1]:x}, xor +0x{found[2]:x}")


if __name__ == "__main__":
    main()
