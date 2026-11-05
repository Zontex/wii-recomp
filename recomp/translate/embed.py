#!/usr/bin/env python3
"""Turns a data file into C, so the console carries it inside: the audio processor's ROM
and its filter table, from recomp/runtime/data. build_file.py runs it for each file there.

Usage: recomp/translate/embed.py <file> <out.c> <symbol>
Writes `const unsigned char <symbol>[]` and `const unsigned <symbol>_bytes`.
"""
import sys
from pathlib import Path


def main():
    source, out, symbol = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    data = source.read_bytes()
    rows = [", ".join(f"0x{byte:02x}" for byte in data[at:at + 16]) for at in range(0, len(data), 16)]
    text = (f"// Made from {source.name} by embed.py.\n"
            f"const unsigned char {symbol}[{len(data)}] = {{\n  " + ",\n  ".join(rows) + "\n};\n"
            f"const unsigned {symbol}_bytes = {len(data)};\n")
    if not out.exists() or out.read_text() != text:
        out.write_text(text)


if __name__ == "__main__":
    main()
