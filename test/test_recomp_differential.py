"""Runs each instruction as emit.py translates it and under the interpreter, then compares.

Both start from the same pseudo-random registers and memory, with a fixed seed, so a
failure repeats. Instruction words are made up here. Nothing comes from a disc.

Run this file directly to see every disagreement, not only whether there are any.
"""
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE.parent / "recomp/translate"))

from decode import (EXTENDED_4, EXTENDED_4_QUANTIZED, EXTENDED_4_SHORT, EXTENDED_19, EXTENDED_31, EXTENDED_59, EXTENDED_63,
                    EXTENDED_63_SHORT, PRIMARY, name_of)
from emit import emit, emit_function, function_name

RUNTIME = HERE.parent / "recomp/runtime"
# The same meaning as the game's own build. Only the optimization differs, to compile quickly.
FLAGS = ["-w", "-O1", "-fno-strict-aliasing", "-fwrapv", "-fsigned-char", "-ffp-contract=off",
         f"-I{RUNTIME}", f"-I{HERE / 'c'}"]
SEED = 20261102
FORMS_PER_NAME = 10
STATES = 200
MEMORY = 1
ANY_NAN = 2
# Branches run inside small programs below, as they need somewhere to go.
BRANCHES = {"b", "bc", "bclr", "bcctr", "sc", "rfi"}
# Which of two NaNs comes out of an operation depends on the order the C compiler chose.
TWO_INPUT_MATH = {"fadd", "fadds", "fsub", "fsubs", "fmul", "fmuls", "fdiv", "fdivs", "fmadd", "fmadds", "fmsub", "fmsubs",
                  "fnmadd", "fnmadds", "fnmsub", "fnmsubs", "ps_add", "ps_sub", "ps_mul", "ps_div", "ps_madd", "ps_msub",
                  "ps_nmadd", "ps_nmsub", "ps_sum0", "ps_sum1", "ps_muls0", "ps_muls1", "ps_madds0", "ps_madds1"}

OPCODE_31 = {name: extended for extended, name in EXTENDED_31.items()}
OPCODE_19 = {name: extended for extended, name in EXTENDED_19.items()}
OPCODE_4 = {name: extended for extended, name in {**EXTENDED_4, **EXTENDED_4_SHORT, **EXTENDED_4_QUANTIZED}.items()}
OPCODE_59 = {name: extended for extended, name in EXTENDED_59.items()}
OPCODE_63 = {name: extended for extended, name in {**EXTENDED_63, **EXTENDED_63_SHORT}.items()}
PRIMARY_OPCODE = {name: primary for primary, name in PRIMARY.items()}

CAN_OVERFLOW = {"add", "addc", "adde", "addze", "addme", "subf", "subfc", "subfe", "subfze", "subfme", "neg", "mullw",
                "divw", "divwu"}
ONE_INPUT = {"addze", "addme", "subfze", "subfme", "neg"}
INT_RECORD = {"mulhw", "mulhwu"}
LOGIC = {"and", "or", "xor", "andc", "orc", "nor", "nand", "eqv", "slw", "srw", "sraw"}
ONE_REGISTER = {"cntlzw", "extsb", "extsh"}
LOAD_NAMES = {"lwz", "lbz", "lhz", "lha", "lfs", "lfd", "lmw"}
STORE_NAMES = {"stw", "stb", "sth", "stfs", "stfd", "stmw"}
UPDATE_D = {"lwzu", "lbzu", "lhzu", "lhau", "lfsu", "lfdu", "stwu", "stbu", "sthu", "stfsu", "stfdu"}
INDEXED_MEMORY = {"lwzx", "lbzx", "lhzx", "lhax", "lfsx", "lfdx", "stwx", "stbx", "sthx", "stfsx", "stfdx", "lwbrx", "lhbrx",
                  "stwbrx", "sthbrx", "lwarx", "stwcx.", "stfiwx", "eciwx", "ecowx", "dcbz", "lswx", "stswx",
                  "dcbf", "dcbi", "dcbst", "dcbt", "dcbtst", "dcba", "icbi"}
INDEXED_UPDATE = {"lwzux", "lbzux", "lhzux", "lhaux", "lfsux", "lfdux", "stwux", "stbux", "sthux", "stfsux", "stfdux"}
SPECIAL_REGISTERS = [1, 8, 9, 22, 26, 27, 272, 273, 274, 275, 282, 287, 912, 913, 915, 919, 920, 921, 922, 1008, 1011]
READ_ONLY_SPECIAL = [268, 269]
WRITE_ONLY_SPECIAL = [284, 285, 923]
FLOAT_THREE = {"fmadd", "fmadds", "fmsub", "fmsubs", "fnmadd", "fnmadds", "fnmsub", "fnmsubs", "fsel"}
FLOAT_TWO = {"fadd", "fadds", "fsub", "fsubs", "fdiv", "fdivs", "fmul", "fmuls"}
FLOAT_ONE = {"fres", "frsqrte", "frsp", "fctiw", "fctiwz", "fneg", "fmr", "fnabs", "fabs"}
CR_LOGIC = {"crand", "crandc", "creqv", "crnand", "crnor", "cror", "crorc", "crxor"}
FIELDLESS = {"sync", "eieio", "isync", "tlbsync"}


def spr_bits(number):
    return (number & 31) << 16 | (number >> 5) << 11


def primary_word(name, d, a, immediate):
    return PRIMARY_OPCODE[name] << 26 | d << 21 | a << 16 | immediate & 0xffff


def extended_word(primary, extended, d, a, b, rc=0):
    return primary << 26 | d << 21 | a << 16 | b << 11 | extended << 1 | rc


def form_31(name, d, a, b, rc=0):
    return extended_word(31, OPCODE_31[name], d, a, b, rc)


def form_a(primary, extended, d, a, b, c, rc):
    return primary << 26 | d << 21 | a << 16 | b << 11 | c << 6 | extended << 1 | rc


class Forms:
    """Builds valid-looking instruction words, one family at a time."""

    def __init__(self, rng):
        self.rng = rng

    def register(self, avoid=()):
        while True:
            value = self.rng.randrange(32)
            if value not in avoid:
                return value

    def offset(self):
        return self.rng.randrange(-0x300, 0x300)

    def build(self, name):
        """Returns (word, base register, index register, flags)."""
        r, bit = self.register, lambda: self.rng.randrange(2)
        d, a, b, c, rc, plain = r(), r(), r(), r(), bit(), lambda word: (word, -1, -1, 0)
        immediate = self.rng.choice([0, 1, -1, 0x7fff, -0x8000, self.rng.randrange(-0x8000, 0x8000)])
        if name in ("addi", "addis", "addic", "addic.", "subfic", "mulli"):
            return plain(primary_word(name, d, a, immediate))
        if name in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
            return plain(primary_word(name, d, a, immediate))
        if name in ("cmpi", "cmpli"):
            return plain(primary_word(name, self.rng.randrange(8) << 2, a, immediate))
        if name == "twi":
            return plain(primary_word(name, d, a, immediate))
        if name in ("rlwimi", "rlwinm", "rlwnm"):
            return plain(PRIMARY_OPCODE[name] << 26 | d << 21 | a << 16 | b << 11 | c << 6 | r() << 1 | rc)
        if name in CAN_OVERFLOW:
            return plain(form_31(name, d, a, 0 if name in ONE_INPUT else b, rc) | bit() << 10)
        if name in INT_RECORD or name in LOGIC:
            return plain(form_31(name, d, a, b, rc))
        if name in ONE_REGISTER:
            return plain(form_31(name, d, a, 0, rc))
        if name == "srawi":
            return plain(form_31(name, d, a, b, rc))
        if name in ("cmp", "cmpl"):
            return plain(form_31(name, self.rng.randrange(8) << 2, a, b))
        if name in LOAD_NAMES or name in STORE_NAMES:
            base = self.rng.choice([0, a or 1, a or 1, a or 1])
            return primary_word(name, d, base, self.offset()), base, -1, MEMORY
        if name in UPDATE_D:
            base = r(avoid=(0, d) if name[0] == "l" and name[1] != "f" else (0,))
            return primary_word(name, d, base, self.offset()), base, -1, MEMORY
        if name in INDEXED_MEMORY:
            base = self.rng.choice([0, r(avoid=(0, b)), r(avoid=(0, b))])
            if name == "stwcx.":
                rc = 1
            return form_31(name, d, base, b, rc if name == "stwcx." else 0), base, b, MEMORY
        if name in INDEXED_UPDATE:
            base = r(avoid=(0, b, d) if name[0] == "l" and name[1] != "f" else (0, b))
            return form_31(name, d, base, b), base, b, MEMORY
        if name in ("lswi", "stswi"):
            base = r(avoid=(0,))
            return form_31(name, d, base, self.rng.choice([0, 1, 3, 4, 5, 8, 31])), base, -1, MEMORY
        if name in ("tw",):
            return plain(form_31(name, d, a, b))
        if name in ("mfmsr", "mfcr"):
            return plain(form_31(name, d, 0, 0))
        if name == "mtmsr":
            return plain(form_31(name, d, 0, 0))
        if name == "mtcrf":
            return plain(31 << 26 | d << 21 | self.rng.randrange(256) << 12 | OPCODE_31[name] << 1)
        if name == "mcrxr":
            return plain(form_31(name, self.rng.randrange(8) << 2, 0, 0))
        if name in ("mfspr", "mtspr"):
            pool = SPECIAL_REGISTERS + (READ_ONLY_SPECIAL if name == "mfspr" else WRITE_ONLY_SPECIAL)
            return plain(31 << 26 | d << 21 | spr_bits(self.rng.choice(pool)) | OPCODE_31[name] << 1)
        if name == "mftb":
            return plain(31 << 26 | d << 21 | spr_bits(self.rng.choice(READ_ONLY_SPECIAL)) | OPCODE_31[name] << 1)
        if name in ("mtsr", "mfsr"):
            return plain(form_31(name, d, self.rng.randrange(16), 0))
        if name in ("mtsrin", "mfsrin"):
            return plain(form_31(name, d, 0, b))
        if name == "tlbie":
            return plain(form_31(name, 0, 0, b))
        if name in FIELDLESS:
            return plain(extended_word(19 if name == "isync" else 31, (OPCODE_19 if name == "isync" else OPCODE_31)[name], 0, 0, 0))
        if name == "mcrf":
            return plain(extended_word(19, 0, self.rng.randrange(8) << 2, self.rng.randrange(8) << 2, 0))
        if name in CR_LOGIC:
            return plain(extended_word(19, OPCODE_19[name], d, a, b))
        if name in OPCODE_59 and name in FLOAT_THREE | FLOAT_TWO | FLOAT_ONE:
            return self.float_form(59, OPCODE_59[name], name, d, a, b, c, rc)
        if name in FLOAT_THREE | FLOAT_TWO | FLOAT_ONE:
            return self.float_form(63, OPCODE_63[name], name, d, a, b, c, rc)
        if name in ("fcmpu", "fcmpo"):
            return plain(extended_word(63, OPCODE_63[name], self.rng.randrange(8) << 2, a, b))
        if name in ("mtfsb0", "mtfsb1"):
            return plain(extended_word(63, OPCODE_63[name], d, 0, 0, rc))
        if name == "mtfsfi":
            return plain(63 << 26 | self.rng.randrange(8) << 23 | self.rng.randrange(16) << 12 | OPCODE_63[name] << 1 | rc)
        if name == "mcrfs":
            return plain(63 << 26 | self.rng.randrange(8) << 23 | self.rng.randrange(8) << 18 | OPCODE_63[name] << 1)
        if name == "mffs":
            return plain(extended_word(63, OPCODE_63[name], d, 0, 0, rc))
        if name == "mtfsf":
            return plain(63 << 26 | self.rng.randrange(256) << 17 | b << 11 | OPCODE_63[name] << 1 | rc)
        if name in ("psq_l", "psq_st", "psq_lu", "psq_stu"):
            update = name in ("psq_lu", "psq_stu")
            base = r(avoid=(0,)) if update else self.rng.choice([0, a or 1, a or 1])
            word = PRIMARY_OPCODE[name] << 26 | d << 21 | base << 16 | bit() << 15 | self.rng.randrange(8) << 12 | self.offset() & 0xfff
            return word, base, -1, MEMORY
        if name in ("psq_lx", "psq_stx", "psq_lux", "psq_stux"):
            update = name in ("psq_lux", "psq_stux")
            base = r(avoid=(0, b)) if update else self.rng.choice([0, r(avoid=(0, b))])
            word = 4 << 26 | d << 21 | base << 16 | b << 11 | bit() << 10 | self.rng.randrange(8) << 7 | OPCODE_4[name] << 1
            return word, base, b, MEMORY
        if name == "dcbz_l":
            base = r(avoid=(0, b))
            return extended_word(4, OPCODE_4[name], 0, base, b), base, b, MEMORY
        if name in ("ps_cmpu0", "ps_cmpo0", "ps_cmpu1", "ps_cmpo1"):
            return plain(extended_word(4, OPCODE_4[name], self.rng.randrange(8) << 2, a, b))
        if name in EXTENDED_4_SHORT.values():
            flags = ANY_NAN if name in TWO_INPUT_MATH else 0
            return form_a(4, OPCODE_4[name], d, a, b, c, rc), -1, -1, flags
        if name in EXTENDED_4.values():
            return plain(extended_word(4, OPCODE_4[name], d, a, b, rc))
        raise ValueError(f"no form for {name}")

    def every_special(self, name):
        """Forms where a few field values mean something quite different, so chance won't do."""
        if name in ("mfspr", "mtspr", "mftb"):
            pool = READ_ONLY_SPECIAL if name == "mftb" else SPECIAL_REGISTERS + (READ_ONLY_SPECIAL if name == "mfspr" else WRITE_ONLY_SPECIAL)
            return [(31 << 26 | self.register() << 21 | spr_bits(n) | OPCODE_31[name] << 1, -1, -1, 0) for n in pool]
        if name in ("mtfsb0", "mtfsb1"):
            return [(extended_word(63, OPCODE_63[name], bit, 0, 0, rc), -1, -1, 0) for bit in range(32) for rc in (0, 1)]
        if name == "mtfsfi":
            return [(63 << 26 | field << 23 | self.rng.randrange(16) << 12 | OPCODE_63[name] << 1, -1, -1, 0) for field in range(8)]
        if name == "mcrfs":
            return [(63 << 26 | self.rng.randrange(8) << 23 | field << 18 | OPCODE_63[name] << 1, -1, -1, 0) for field in range(8)]
        if name == "mtfsf":
            return [(63 << 26 | 1 << field << 17 | self.register() << 11 | OPCODE_63[name] << 1, -1, -1, 0) for field in range(8)]
        if name in ("lswi", "stswi"):
            return [(form_31(name, self.register(), 3, count), 3, -1, MEMORY) for count in (0, 1, 2, 3, 4, 7)]
        if name == "srawi":
            return [(form_31(name, self.register(), self.register(), amount, rc), -1, -1, 0) for amount in (0, 1, 31) for rc in (0, 1)]
        return []

    def float_form(self, primary, extended, name, d, a, b, c, rc):
        flags = ANY_NAN if name in TWO_INPUT_MATH else 0
        if name in FLOAT_ONE:
            a = c = 0
        if extended >= 32:
            return extended_word(primary, extended, d, 0, b, rc), -1, -1, flags
        return form_a(primary, extended, d, a, b, c, rc), -1, -1, flags


def all_names():
    names = set(PRIMARY.values()) | set(EXTENDED_19.values()) | set(EXTENDED_31.values()) | set(EXTENDED_59.values())
    names |= set(EXTENDED_63.values()) | set(EXTENDED_63_SHORT.values()) | set(EXTENDED_4.values())
    names |= set(EXTENDED_4_SHORT.values()) | set(EXTENDED_4_QUANTIZED.values())
    return names - BRANCHES


def generate_cases(only=None):
    """Every instruction name, in several forms. Returns (name, word, base, index, flags)."""
    rng = random.Random(SEED)
    forms = Forms(rng)
    cases = []
    for name in sorted(all_names()):
        if only and name not in only:
            continue
        seen = set()
        for word, base, index, flags in [forms.build(name) for _ in range(FORMS_PER_NAME)] + forms.every_special(name):
            if word in seen:
                continue
            seen.add(word)
            if name_of(word) != name:
                raise ValueError(f"{word:08x} was built as {name} but decodes as {name_of(word)}")
            cases.append((name, word, base, index, flags))
    return cases


def decode_sweep():
    """Every primary and extended opcode, split by whether decode.py knows them."""
    known, unknown = [], []
    words = [primary << 26 for primary in range(64)]
    words += [primary << 26 | extended << 1 for primary in (4, 19, 31, 59, 63) for extended in range(1024)]
    for word in words:
        name = name_of(word)
        if name in BRANCHES:
            continue
        (known if name else unknown).append(word)
    return known, unknown


class Assembler:
    """Just enough to write test programs, with labels."""

    def __init__(self, start):
        self.start = start
        self.words = {}
        self.at = start
        self.labels = {}
        self.fixups = []

    def word(self, value):
        self.words[self.at] = value
        self.at += 4

    def label(self, name):
        self.labels[name] = self.at

    def branch(self, target, link=0):
        self.fixups.append((self.at, target, 18 << 26 | link, 0x3fffffc))
        self.word(0)

    def conditional(self, bo, bi, target, link=0):
        self.fixups.append((self.at, target, 16 << 26 | bo << 21 | bi << 16 | link, 0xfffc))
        self.word(0)

    def li(self, d, value):
        self.word(primary_word("addi", d, 0, value))

    def addi(self, d, a, value):
        self.word(primary_word("addi", d, a, value))

    def load_address(self, d, label):
        self.fixups.append((self.at, label, primary_word("addis", d, 0, 0), None))
        self.word(0)
        self.fixups.append((self.at, label, primary_word("ori", d, d, 0), "low"))
        self.word(0)

    def move_to_special(self, number, s):
        self.word(31 << 26 | s << 21 | spr_bits(number) | OPCODE_31["mtspr"] << 1)

    def enter(self):
        """A frame that keeps the link register, as real functions do."""
        self.word(31 << 26 | spr_bits(8) | OPCODE_31["mfspr"] << 1)  # mflr r0
        self.word(primary_word("stw", 0, 1, 4))
        self.word(primary_word("stwu", 1, 1, -16))

    def leave(self):
        self.word(primary_word("addi", 1, 1, 16))
        self.word(primary_word("lwz", 0, 1, 4))
        self.move_to_special(8, 0)
        self.blr()

    def blr(self):
        self.word(0x4e800020)

    def finish(self):
        for at, target, base, field in self.fixups:
            destination = self.labels[target]
            if field is None:
                high = destination >> 16
                self.words[at] = base | high
            elif field == "low":
                self.words[at] = base | destination & 0xffff
            else:
                self.words[at] = base | (destination - at) & field
        return self.words


class FakeProgram:
    def __init__(self, words):
        self.words = words

    def word(self, address):
        return self.words[address]


def conditional_branch(a, bo, bi, link):
    a.enter()
    a.conditional(bo, bi, "taken", link)
    a.li(3, 1)
    a.leave()
    a.label("taken")
    a.addi(3, 3, 2)
    a.leave()


def conditional_return(a, bo, bi):
    a.word(extended_word(19, 16, bo, bi, 0))
    a.li(3, 1)
    a.blr()


def conditional_count_jump(a, bo, bi, link):
    a.enter()
    a.load_address(4, "other")
    a.move_to_special(9, 4)
    a.word(extended_word(19, 528, bo, bi, 0, link))
    a.li(3, 1)
    a.leave()
    a.mark_end = a.at
    a.label("other")
    a.addi(3, 3, 7)
    a.blr()


def link_register_call(a):
    a.enter()
    a.load_address(4, "other")
    a.move_to_special(8, 4)
    a.word(extended_word(19, 16, 20, 0, 0, 1))  # blrl
    a.addi(3, 3, 1)
    a.leave()
    a.mark_end = a.at
    a.label("other")
    a.addi(3, 3, 7)
    a.blr()


def count_loop(a):
    a.li(3, 0)
    a.li(4, 9)
    a.move_to_special(9, 4)
    a.label("loop")
    a.addi(3, 3, 3)
    a.conditional(16, 0, "loop")
    a.blr()


def compare_loop(a):
    a.li(3, 0)
    a.label("loop")
    a.addi(3, 3, 1)
    a.word(primary_word("cmpi", 0, 3, 7))
    a.conditional(4, 2, "loop")  # bne
    a.blr()


def nested_calls(a):
    a.enter()
    a.branch("first", 1)
    a.addi(3, 3, 1)
    a.branch("second", 1)
    a.leave()
    a.mark_end = a.at
    a.label("first")
    a.enter()
    a.branch("second", 1)
    a.addi(3, 3, 16)
    a.leave()
    a.mark_second = a.at
    a.label("second")
    a.addi(3, 3, 256)
    a.blr()


def recursion(a):
    a.label("self")
    a.enter()
    a.word(primary_word("andi.", 5, 5, 7))  # At most seven levels, whatever r5 holds.
    a.word(primary_word("cmpi", 0, 5, 0))
    a.conditional(12, 2, "done")  # beq
    a.addi(5, 5, -1)
    a.addi(3, 3, 2)
    a.branch("self", 1)
    a.label("done")
    a.addi(3, 3, 1)
    a.leave()


def tail_jump(a):
    a.addi(3, 3, 1)
    a.branch("other")
    a.mark_end = a.at
    a.label("other")
    a.addi(3, 3, 7)
    a.blr()


def fall_into_next_function(a):
    a.addi(3, 3, 1)
    a.mark_end = a.at
    a.label("other")
    a.addi(3, 3, 7)
    a.blr()


def system_call_and_return(a):
    a.li(3, 5)
    a.word(0x44000002)  # sc
    a.addi(3, 3, 1)
    a.blr()


def return_from_interrupt(a):
    a.load_address(4, "other")
    a.move_to_special(26, 4)
    a.move_to_special(27, 5)
    a.word(0x4c000064)  # rfi
    a.mark_end = a.at
    a.label("other")
    a.addi(3, 3, 7)
    a.blr()


def read_own_address(a):
    a.enter()
    a.branch("here", 1)
    a.label("here")
    a.word(31 << 26 | 3 << 21 | spr_bits(8) | OPCODE_31["mfspr"] << 1)  # mflr r3
    a.addi(3, 3, 1)
    a.leave()


def programs():
    """(name, builder, whether interrupt checks must match)."""
    made = []
    for bo in range(32):
        for link in (0, 1):
            made.append((f"bc_{bo}_{link}", lambda a, bo=bo, link=link: conditional_branch(a, bo, 2, link), False))
    for bo in (0, 2, 4, 8, 10, 12, 16, 18, 20):
        made.append((f"bclr_{bo}", lambda a, bo=bo: conditional_return(a, bo, 5), False))
    for bo in (4, 12, 20):
        for link in (0, 1):
            made.append((f"bcctr_{bo}_{link}", lambda a, bo=bo, link=link: conditional_count_jump(a, bo, 6, link), False))
    made += [("blrl", link_register_call, False), ("count_loop", count_loop, True), ("compare_loop", compare_loop, True),
             ("nested_calls", nested_calls, True), ("recursion", recursion, True), ("tail_jump", tail_jump, False),
             ("fall_through", fall_into_next_function, False), ("sc", system_call_and_return, False),
             ("rfi", return_from_interrupt, False), ("bl_next", read_own_address, False)]
    return made


def bodies_of(assembler, start):
    """The functions in an assembled program, as first address -> addresses."""
    marks = [mark for mark in (getattr(assembler, "mark_end", None), getattr(assembler, "mark_second", None)) if mark]
    edges = [start] + marks + [assembler.at]
    return {edges[i]: {a for a in range(edges[i], edges[i + 1], 4)} for i in range(len(edges) - 1)}


def generate_programs():
    """Returns (C for each program, C for the program table)."""
    code, rows, tables = [], [], []
    for index, (name, build, compare_polls) in enumerate(programs()):
        start = 0x80200000 + index * 0x1000
        assembler = Assembler(start)
        build(assembler)
        words = assembler.finish()
        program = FakeProgram(words)
        bodies = bodies_of(assembler, start)
        all_starts = set(bodies)
        ordered = sorted(words)
        code += [f"void {function_name(a)}(void);" for a in sorted(all_starts)]
        for address, body in bodies.items():
            code.append(emit_function(program, address, body, all_starts)[0])
        # The same first function again, with the others reachable only through dispatch.
        alone = emit_function(program, start, bodies[start], {start})[0]
        code.append(alone.replace(function_name(start), f"alone_{index}"))
        code.append(f"static const uint32_t words_{index}[] = {{{', '.join(f'0x{words[a]:08x}u' for a in ordered)}}};")
        table = ", ".join(f"{{0x{a:x}u, {function_name(a)}}}" for a in sorted(all_starts))
        code.append(f"static const Function functions_{index}[] = {{{table}}};")
        rows.append(f'{{"{name}", 0x{start:x}u, words_{index}, {len(ordered)}, alone_{index}, functions_{index}, {len(all_starts)}, {int(compare_polls)}}}')
    return code, rows


def generate_source(cases, with_programs=True, translate=emit):
    parts = ['#include "differential.h"']
    rows, missing = [], []
    for index, (name, word, base, index_register, flags) in enumerate(cases):
        text = translate(0x80001000, word, name)
        if text is None:
            missing.append(name)
            continue
        parts.append(f"static void case_{index}(void) {{ {text} }}")
        rows.append(f'{{"{name}", 0x{word:08x}u, case_{index}, {base}, {index_register}, {flags}}}')
    parts.append(f"const Case cases[] = {{{', '.join(rows)}}};")
    parts.append(f"const int case_count = {len(rows)};")
    program_code, program_rows = generate_programs() if with_programs else ([], [])
    parts += program_code
    parts.append(f"const Program programs[] = {{{', '.join(program_rows) or '{0}'}}};")
    parts.append(f"const int program_count = {len(program_rows)};")
    known, unknown = decode_sweep() if with_programs else ([], [])
    parts.append(f"const uint32_t known_words[] = {{{', '.join(f'0x{w:08x}u' for w in known) or '0'}}};")
    parts.append(f"const uint32_t unknown_words[] = {{{', '.join(f'0x{w:08x}u' for w in unknown) or '0'}}};")
    parts.append(f"const int known_word_count = {len(known)}, unknown_word_count = {len(unknown)};")
    return "\n".join(parts) + "\n", missing


def run_differential(cases, folder, with_programs=True, translate=emit, states=STATES):
    """Returns (output of the C driver, names emit.py couldn't translate)."""
    source, missing = generate_source(cases, with_programs, translate)
    generated = Path(folder) / "cases.c"
    generated.write_text(source)
    program = Path(folder) / "differential"
    subprocess.run(["clang", str(generated), str(HERE / "c/differential.c"), str(RUNTIME / "interp.c"), str(RUNTIME / "quantized.c"),
                    *FLAGS, "-lm", "-o", str(program)], check=True, capture_output=True, text=True)
    result = subprocess.run([str(program), str(states)], capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f"the driver failed ({result.returncode}): {result.stdout[-2000:]}{result.stderr[-2000:]}")
    return result.stdout, missing


def summary_of(output):
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", output.splitlines()[-1])}


@unittest.skipUnless(shutil.which("clang"), "needs clang")
class Differential(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)

    def test_every_instruction_family_is_covered(self):
        covered = {case[0] for case in generate_cases()}
        self.assertEqual(all_names() - covered, set())

    def test_translated_code_and_interpreter_agree(self):
        output, missing = run_differential(generate_cases(), self.temporary.name)
        self.assertEqual(missing, [])
        summary = summary_of(output)
        problems = [line for line in output.splitlines() if not line.startswith("SUMMARY")]
        self.assertEqual(problems[:40], [], output[-1000:])
        self.assertGreater(summary["forms"], 1500)
        self.assertGreater(summary["programs"], 80)
        self.assertEqual(summary["bad_forms"] + summary["bad_programs"] + summary["bad_decoding"], 0)

    def test_a_wrong_translation_is_caught(self):
        wrong = {"add": lambda text: text.replace("+", "-", 1), "lwz": lambda text: text.replace("rd32", "rd16"),
                 "fadds": lambda text: text.replace("cpu.p[", "cpu.f[", 1), "ps_merge10": lambda text: text.replace("cpu.p", "cpu.f", 1),
                 "stwu": lambda text: re.sub(r" cpu\.r\[\d+\] = e;", "", text)}

        def translate(address, word, name):
            return wrong[name](emit(address, word, name))

        cases = generate_cases(only=set(wrong))
        output, _ = run_differential(cases, self.temporary.name, with_programs=False, translate=translate, states=50)
        caught = set(re.findall(r"^CASE (\S+)", output, re.MULTILINE))
        self.assertEqual(caught, set(wrong), output[-1000:])


def main():
    with tempfile.TemporaryDirectory() as folder:
        output, missing = run_differential(generate_cases(), folder)
    print("\n".join(line for line in output.splitlines() if not line.startswith("DIFF") or "--all" in sys.argv))
    print("not translated:", sorted(set(missing)))


if __name__ == "__main__":
    main()
