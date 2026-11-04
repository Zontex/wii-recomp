#!/usr/bin/env python3
"""Read a Wii program (DOL) and name each PowerPC instruction in it.

Usage: recomp/translate/decode.py extracted/sys/main.dol
"""
import struct
import sys
from collections import Counter
from pathlib import Path

TEXT_SECTIONS = 7
ALL_SECTIONS = 18
HEADER_OFFSETS = 0x00
HEADER_ADDRESSES = 0x48
HEADER_SIZES = 0x90
HEADER_ENTRY = 0xe0

PRIMARY = {
    3: "twi", 7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.",
    14: "addi", 15: "addis", 16: "bc", 17: "sc", 18: "b", 20: "rlwimi", 21: "rlwinm", 23: "rlwnm",
    24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis.",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
    40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
    56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu",
}

EXTENDED_19 = {
    0: "mcrf", 16: "bclr", 33: "crnor", 50: "rfi", 129: "crandc", 150: "isync", 193: "crxor",
    225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr",
}

EXTENDED_31 = {
    0: "cmp", 4: "tw", 8: "subfc", 10: "addc", 11: "mulhwu", 19: "mfcr", 20: "lwarx", 23: "lwzx",
    24: "slw", 26: "cntlzw", 28: "and", 32: "cmpl", 40: "subf", 54: "dcbst", 55: "lwzux", 60: "andc",
    75: "mulhw", 83: "mfmsr", 86: "dcbf", 87: "lbzx", 104: "neg", 119: "lbzux", 124: "nor", 136: "subfe",
    138: "adde", 144: "mtcrf", 146: "mtmsr", 150: "stwcx.", 151: "stwx", 183: "stwux", 200: "subfze",
    202: "addze", 210: "mtsr", 215: "stbx", 232: "subfme", 234: "addme", 235: "mullw", 242: "mtsrin",
    246: "dcbtst", 247: "stbux", 266: "add", 278: "dcbt", 279: "lhzx", 284: "eqv", 306: "tlbie",
    310: "eciwx", 311: "lhzux", 316: "xor", 339: "mfspr", 343: "lhax", 371: "mftb", 375: "lhaux",
    407: "sthx", 412: "orc", 438: "ecowx", 439: "sthux", 444: "or", 459: "divwu", 467: "mtspr", 470: "dcbi",
    476: "nand", 491: "divw", 512: "mcrxr", 533: "lswx", 534: "lwbrx", 535: "lfsx", 536: "srw", 566: "tlbsync",
    567: "lfsux", 595: "mfsr", 597: "lswi", 598: "sync", 599: "lfdx", 631: "lfdux", 659: "mfsrin",
    661: "stswx", 662: "stwbrx", 663: "stfsx", 695: "stfsux", 725: "stswi", 727: "stfdx", 758: "dcba", 759: "stfdux",
    790: "lhbrx", 792: "sraw", 824: "srawi", 854: "eieio", 918: "sthbrx", 922: "extsh", 954: "extsb",
    982: "icbi", 983: "stfiwx", 1014: "dcbz",
}

EXTENDED_59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 24: "fres", 25: "fmuls", 28: "fmsubs",
               29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}

EXTENDED_63_SHORT = {18: "fdiv", 20: "fsub", 21: "fadd", 23: "fsel", 25: "fmul", 26: "frsqrte",
                     28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}

EXTENDED_63 = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1", 40: "fneg",
               64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs", 264: "fabs",
               583: "mffs", 711: "mtfsf"}

EXTENDED_4_SHORT = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0",
                    15: "ps_madds1", 18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res",
                    25: "ps_mul", 26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub",
                    31: "ps_nmadd"}

EXTENDED_4 = {0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1", 72: "ps_mr", 96: "ps_cmpo1",
              136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10",
              624: "ps_merge11", 1014: "dcbz_l"}

EXTENDED_4_QUANTIZED = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}


def name_of(word):
    """Returns an instruction's name, or None if it isn't one we recognise."""
    primary = word >> 26
    if primary in PRIMARY:
        return PRIMARY[primary]
    extended = (word >> 1) & 0x3ff
    if primary == 19:
        return EXTENDED_19.get(extended)
    if primary == 31:
        return EXTENDED_31.get(extended) or EXTENDED_31.get(extended & 0x1ff)
    if primary == 59:
        return EXTENDED_59.get(extended & 0x1f)
    if primary == 63:
        return EXTENDED_63_SHORT.get(extended & 0x1f) or EXTENDED_63.get(extended)
    if primary == 4:
        return (EXTENDED_4_SHORT.get(extended & 0x1f) or EXTENDED_4.get(extended)
                or EXTENDED_4_QUANTIZED.get(extended & 0x3f))
    return None


class Program:
    def __init__(self, data):
        self.data = data
        read = lambda start: struct.unpack_from(f">{ALL_SECTIONS}I", data, start)
        offsets, addresses, sizes = read(HEADER_OFFSETS), read(HEADER_ADDRESSES), read(HEADER_SIZES)
        self.entry = struct.unpack_from(">I", data, HEADER_ENTRY)[0]
        self.sections = [(index < TEXT_SECTIONS, addresses[index], offsets[index], sizes[index])
                         for index in range(ALL_SECTIONS) if sizes[index]]
        # Each section's whole words, read once: the translator asks millions of times, and
        # mostly about the section it asked about last.
        self.spans = [(start, start + size // 4 * 4, struct.unpack_from(f">{size // 4}I", data, offset))
                      for _, start, offset, size in self.sections]
        self.code_ranges = [(start, start + size) for code, start, _, size in self.sections if code]
        self.last_span = self.spans[0] if self.spans else (0, 0, ())
        self.last_code = self.code_ranges[0] if self.code_ranges else (0, 0)

    def is_code(self, address):
        start, end = self.last_code
        if start <= address < end:
            return True
        for start, end in self.code_ranges:
            if start <= address < end:
                self.last_code = (start, end)
                return True
        return False

    def word(self, address):
        start, end, words = self.last_span
        if start <= address < end and not (address - start) & 3:
            return words[(address - start) >> 2]
        for span, (_, start, offset, size) in zip(self.spans, self.sections):
            if start <= address < start + size:
                if span[0] <= address < span[1] and not (address - start) & 3:
                    self.last_span = span
                return struct.unpack_from(">I", self.data, offset + address - start)[0]
        raise ValueError(f"{address:#x} is outside the program")

    def code(self):
        """Yields every (address, word) in the code sections."""
        for code, start, offset, size in self.sections:
            if code:
                words = struct.unpack_from(f">{size // 4}I", self.data, offset)
                yield from ((start + index * 4, word) for index, word in enumerate(words))


def main():
    program = Program(Path(sys.argv[1]).read_bytes())
    names, unknown = Counter(), Counter()
    for _, word in program.code():
        name = name_of(word)
        if name:
            names[name] += 1
        else:
            unknown[word >> 26] += 1
    total = sum(names.values()) + sum(unknown.values())
    print(f"{total} words, {sum(names.values())} recognised, {len(names)} kinds")
    print("unrecognised by primary opcode:", unknown.most_common(12))
    print("most common:", names.most_common(12))


if __name__ == "__main__":
    main()
