#!/usr/bin/env python3
"""Translate each function of a Wii program into C.

Usage: recomp/translate/emit.py extracted/sys/main.dol recomp/build/generated [--all-code] [--fifo-breakpoints] [--virtual-bluetooth]

--all-code also translates code that nothing visibly points at, which games with C++
method tables built at run time need. --fifo-breakpoints makes the graphics chip honour
FIFO breakpoints, for games that keep several frames in the FIFO. Wii Fit sets one but
needs it ignored, as it sends two tokens a frame. --virtual-bluetooth runs the game's own
Wii Remote code against the virtual Bluetooth chip in place of direct input.
"""
import struct
import sys
from collections import Counter
from pathlib import Path

from decode import Program, name_of
import modules
from functions import add_starts, add_unreached_code, branch_target, find_functions, read_starts, signed
from remote_tables import find_remote_tables
from signatures import load_entries, resolve

FUNCTIONS_PER_FILE = 150
# Filled in for each game from signatures.py. REPLACED holds the system functions we
# replace rather than translate, as address -> (reason, C body). IDLE_LOOPS holds loops
# where the game waits for an interrupt with nothing to do. Spinning them at full speed
# keeps a core busy for no work, so they sleep instead.
REPLACED = {}
IDLE_LOOPS = {}
# Replacements that skip the game's Wii Remote code, for games not on virtual Bluetooth.
DIRECT_INPUT = {"input_read();", "input_probe();"}
IOS_OFFSET = 0x184  # The IOS the title asks for, in its TMD.
TITLE_ID_OFFSET = 0x18c  # In the disc's tmd.bin.
DECREMENTER = 22
CACHE_DMA_LOW = 923
LINK = 8
COUNT = 9
XER = 1
TIME_LOW = 268
TIME_HIGH = 269
TIME_LOW_WRITE = 284
TIME_HIGH_WRITE = 285
OVERFLOW_ENABLE = 0x400


def s16(word):
    return signed(word & 0xffff, 16)


def s12(word):
    return signed(word & 0xfff, 12)


def rt(word):
    return (word >> 21) & 31


def ra(word):
    return (word >> 16) & 31


def rb(word):
    return (word >> 11) & 31


def rc(word):
    return (word >> 6) & 31


def base(word):
    return f"(cpu.r[{ra(word)}] + {s16(word)})" if ra(word) else f"(uint32_t)({s16(word)})"


def base_u(word):
    return f"(cpu.r[{ra(word)}] + {s16(word)})"


def indexed(word):
    return f"(cpu.r[{ra(word)}] + cpu.r[{rb(word)}])" if ra(word) else f"cpu.r[{rb(word)}]"


def record(word, value):
    return f"cpu.cr[0] = cr_compare((int32_t){value}, 0);" if word & 1 else ""


def record_float(word):
    return " cpu.cr[1] = (uint8_t)(cpu.fpscr >> 28);" if word & 1 else ""


def spr_number(word):
    return ((word >> 16) & 31) | (((word >> 11) & 31) << 5)


def condition(word):
    """A C test for a conditional branch's BO and BI fields."""
    bo, bi = rt(word), ra(word)
    parts, before = [], ""
    if not bo & 4:
        before = "cpu.ctr--;"
        parts.append("cpu.ctr == 0" if bo & 2 else "cpu.ctr != 0")
    if not bo & 16:
        parts.append(f"cr_bit({bi})" if bo & 8 else f"!cr_bit({bi})")
    return before, " && ".join(parts) or "1"


LOADS = {"lwz": "rd32({a})", "lbz": "rd8({a})", "lhz": "rd16({a})", "lha": "(uint32_t)(int16_t)rd16({a})"}
STORES = {"stw": "wr32({a}, {v})", "stb": "wr8({a}, {v})", "sth": "wr16({a}, {v})"}
INDEXED_LOADS = {"lwzx": "lwz", "lbzx": "lbz", "lhzx": "lhz", "lhax": "lha"}
INDEXED_UPDATE_LOADS = {"lwzux": "lwz", "lbzux": "lbz", "lhzux": "lhz", "lhaux": "lha"}
INDEXED_STORES = {"stwx": "stw", "stbx": "stb", "sthx": "sth"}
INDEXED_UPDATE_STORES = {"stwux": "stw", "stbux": "stb", "sthux": "sth"}
NO_OPERATIONS = {"sync", "isync", "eieio", "dcba", "dcbf", "dcbi", "dcbst", "dcbt", "dcbtst", "icbi", "tw", "twi",
                 "tlbie", "tlbsync"}
ROUND_SINGLE = "(double)(float)"


def fload(address, double):
    return f"as_double(rd64({address}))" if double else f"single_to_double(rd32({address}))"


def load_float(word, address, double):
    d = rt(word)
    value = fload(address, double)
    if double:
        return f"cpu.f[{d}] = {value};"
    return f"cpu.f[{d}] = cpu.p[{d}] = {value};"


def store_float(word, address, double):
    d = rt(word)
    if double:
        return f"wr64({address}, double_bits(cpu.f[{d}]));"
    return f"wr32({address}, double_to_single(cpu.f[{d}]));"


def math(word, name):
    """Floating-point arithmetic, for scalar and paired forms."""
    d, a, b, c = rt(word), ra(word), rb(word), rc(word)
    single = name.endswith("s") and name != "fres"
    kind = name[:-1] if single else name
    wrap = (lambda e: f"{ROUND_SINGLE}({e})") if single else (lambda e: e)
    expressions = {
        "fadd": f"cpu.f[{a}] + cpu.f[{b}]", "fsub": f"cpu.f[{a}] - cpu.f[{b}]",
        "fmul": f"cpu.f[{a}] * cpu.f[{c}]", "fdiv": f"cpu.f[{a}] / cpu.f[{b}]",
        "fmadd": f"cpu.f[{a}] * cpu.f[{c}] + cpu.f[{b}]", "fmsub": f"cpu.f[{a}] * cpu.f[{c}] - cpu.f[{b}]",
        "fnmadd": f"-(cpu.f[{a}] * cpu.f[{c}] + cpu.f[{b}])", "fnmsub": f"-(cpu.f[{a}] * cpu.f[{c}] - cpu.f[{b}])",
    }
    text = f"cpu.f[{d}] = {wrap(expressions[kind])};"
    if single:
        text += f" cpu.p[{d}] = cpu.f[{d}];"
    return text + record_float(word)


PAIRED_MATH = {
    "ps_add": "{x}[{a}] + {x}[{b}]", "ps_sub": "{x}[{a}] - {x}[{b}]", "ps_mul": "{x}[{a}] * {x}[{c}]",
    "ps_div": "{x}[{a}] / {x}[{b}]", "ps_madd": "{x}[{a}] * {x}[{c}] + {x}[{b}]",
    "ps_msub": "{x}[{a}] * {x}[{c}] - {x}[{b}]", "ps_nmadd": "-({x}[{a}] * {x}[{c}] + {x}[{b}])",
    "ps_nmsub": "-({x}[{a}] * {x}[{c}] - {x}[{b}])", "ps_res": "1.0 / {x}[{b}]",
    "ps_rsqrte": "1.0 / sqrt({x}[{b}])",
}


def paired(word, name):
    d, a, b, c = rt(word), ra(word), rb(word), rc(word)
    if name in PAIRED_MATH:
        first = PAIRED_MATH[name].format(x="f0", a=a, b=b, c=c)
        second = PAIRED_MATH[name].format(x="f1", a=a, b=b, c=c)
        return (f"{{ double *f0 = cpu.f, *f1 = cpu.p; double x = (float)({first}), y = (float)({second});"
                f" cpu.f[{d}] = x; cpu.p[{d}] = y; }}")
    table = {
        "ps_mr": f"cpu.f[{d}] = cpu.f[{b}]; cpu.p[{d}] = cpu.p[{b}];",
        "ps_neg": f"cpu.f[{d}] = -cpu.f[{b}]; cpu.p[{d}] = -cpu.p[{b}];",
        "ps_abs": f"cpu.f[{d}] = fabs(cpu.f[{b}]); cpu.p[{d}] = fabs(cpu.p[{b}]);",
        "ps_nabs": f"cpu.f[{d}] = -fabs(cpu.f[{b}]); cpu.p[{d}] = -fabs(cpu.p[{b}]);",
        "ps_merge00": f"{{ double x = cpu.f[{a}], y = cpu.f[{b}]; cpu.f[{d}] = x; cpu.p[{d}] = y; }}",
        "ps_merge01": f"{{ double x = cpu.f[{a}], y = cpu.p[{b}]; cpu.f[{d}] = x; cpu.p[{d}] = y; }}",
        "ps_merge10": f"{{ double x = cpu.p[{a}], y = cpu.f[{b}]; cpu.f[{d}] = x; cpu.p[{d}] = y; }}",
        "ps_merge11": f"{{ double x = cpu.p[{a}], y = cpu.p[{b}]; cpu.f[{d}] = x; cpu.p[{d}] = y; }}",
        "ps_sum0": f"{{ double x = (float)(cpu.f[{a}] + cpu.p[{b}]), y = (float)cpu.p[{c}]; cpu.f[{d}] = x; cpu.p[{d}] = y; }}",
        "ps_sum1": f"{{ double x = (float)cpu.f[{c}], y = (float)(cpu.f[{a}] + cpu.p[{b}]); cpu.f[{d}] = x; cpu.p[{d}] = y; }}",
        "ps_muls0": f"{{ double x = (float)(cpu.f[{a}] * cpu.f[{c}]), y = (float)(cpu.p[{a}] * cpu.f[{c}]); cpu.f[{d}] = x; cpu.p[{d}] = y; }}",
        "ps_muls1": f"{{ double x = (float)(cpu.f[{a}] * cpu.p[{c}]), y = (float)(cpu.p[{a}] * cpu.p[{c}]); cpu.f[{d}] = x; cpu.p[{d}] = y; }}",
        "ps_madds0": f"{{ double x = (float)(cpu.f[{a}] * cpu.f[{c}] + cpu.f[{b}]), y = (float)(cpu.p[{a}] * cpu.f[{c}] + cpu.p[{b}]); cpu.f[{d}] = x; cpu.p[{d}] = y; }}",
        "ps_madds1": f"{{ double x = (float)(cpu.f[{a}] * cpu.p[{c}] + cpu.f[{b}]), y = (float)(cpu.p[{a}] * cpu.p[{c}] + cpu.p[{b}]); cpu.f[{d}] = x; cpu.p[{d}] = y; }}",
        "ps_sel": f"{{ double x = cpu.f[{a}] >= 0 ? cpu.f[{c}] : cpu.f[{b}], y = cpu.p[{a}] >= 0 ? cpu.p[{c}] : cpu.p[{b}]; cpu.f[{d}] = x; cpu.p[{d}] = y; }}",
    }
    if name in table:
        return table[name]
    field = rt(word) >> 2
    compare = {"ps_cmpu0": ("f", "f"), "ps_cmpo0": ("f", "f"), "ps_cmpu1": ("p", "p"), "ps_cmpo1": ("p", "p")}
    if name in compare:
        return f"cpu.cr[{field}] = cr_float(cpu.{compare[name][0]}[{a}], cpu.{compare[name][1]}[{b}]);"
    return None


def quantized(word, name, address):
    """psq forms carry W at bit 16 (or 21 when indexed) and I in the next bits."""
    if name in ("psq_l", "psq_lu", "psq_st", "psq_stu"):
        w, gqr = (word >> 15) & 1, (word >> 12) & 7
    else:
        w, gqr = (word >> 10) & 1, (word >> 7) & 7
    call = "ps_load" if "_l" in name else "ps_store"
    return f"{call}({address}, {w}, {gqr}, {rt(word)});"


def string_load(first, count, e):
    return (f"{{ uint32_t e = {e}; for (uint32_t i = 0; i < {count}; i++) {{ uint32_t r = ({first} + (i >> 2)) & 31;"
            f" if (!(i & 3)) cpu.r[r] = 0; cpu.r[r] |= rd8(e + i) << (24 - (i & 3) * 8); }} }}")


def string_store(first, count, e):
    return (f"{{ uint32_t e = {e}; for (uint32_t i = 0; i < {count}; i++)"
            f" wr8(e + i, cpu.r[({first} + (i >> 2)) & 31] >> (24 - (i & 3) * 8)); }}")


def overflowing(name, a, b):
    """The arithmetic forms with OE set, which also write OV and SO."""
    x, y = f"cpu.r[{a}]", f"cpu.r[{b}]"
    return {
        "add": f"add_overflow({x}, {y}, 0)", "subf": f"add_overflow(~{x}, {y}, 1)", "neg": f"add_overflow(~{x}, 0, 1)",
        "mullw": f"multiply_overflow({x}, {y})",
        "divw": f"divide_signed_overflow({x}, {y})", "divwu": f"divide_unsigned_overflow({x}, {y})",
        "addc": f"add_carry_overflow({x}, {y}, 0)", "adde": f"add_carry_overflow({x}, {y}, cpu.ca)",
        "subfc": f"add_carry_overflow(~{x}, {y}, 1)", "subfe": f"add_carry_overflow(~{x}, {y}, cpu.ca)",
        "addze": f"add_carry_overflow({x}, 0, cpu.ca)", "addme": f"add_carry_overflow({x}, 0xffffffffu, cpu.ca)",
        "subfze": f"add_carry_overflow(~{x}, 0, cpu.ca)", "subfme": f"add_carry_overflow(~{x}, 0xffffffffu, cpu.ca)",
    }.get(name)


def emit(address, word, name):
    """Returns C for one instruction that isn't a branch, or None if not handled."""
    d, a, b = rt(word), ra(word), rb(word)
    imm, uimm = s16(word), word & 0xffff
    if name == "addi":
        return f"cpu.r[{d}] = {f'cpu.r[{a}] + ' if a else ''}(uint32_t)({imm});"
    if name == "addis":
        return f"cpu.r[{d}] = {f'cpu.r[{a}] + ' if a else ''}0x{uimm << 16:x}u;"
    if name == "addic":
        return f"cpu.r[{d}] = add_carry(cpu.r[{a}], (uint32_t)({imm}), 0);"
    if name == "addic.":
        return f"cpu.r[{d}] = add_carry(cpu.r[{a}], (uint32_t)({imm}), 0); cpu.cr[0] = cr_compare((int32_t)cpu.r[{d}], 0);"
    if name == "subfic":
        return f"cpu.r[{d}] = add_carry(~cpu.r[{a}], (uint32_t)({imm}), 1);"
    if name == "mulli":
        return f"cpu.r[{d}] = cpu.r[{a}] * (uint32_t)({imm});"
    if name == "cmpi":
        return f"cpu.cr[{d >> 2}] = cr_compare((int32_t)cpu.r[{a}], {imm});"
    if name == "cmpli":
        return f"cpu.cr[{d >> 2}] = cr_compare_unsigned(cpu.r[{a}], {uimm}u);"
    if name == "cmp":
        return f"cpu.cr[{d >> 2}] = cr_compare((int32_t)cpu.r[{a}], (int32_t)cpu.r[{b}]);"
    if name == "cmpl":
        return f"cpu.cr[{d >> 2}] = cr_compare_unsigned(cpu.r[{a}], cpu.r[{b}]);"
    logic = {"ori": "|", "oris": "|", "xori": "^", "xoris": "^", "andi.": "&", "andis.": "&"}
    if name in logic:
        value = uimm << 16 if name.endswith(("s", "s.")) else uimm
        text = f"cpu.r[{a}] = cpu.r[{d}] {logic[name]} 0x{value:x}u;"
        return text + (f" cpu.cr[0] = cr_compare((int32_t)cpu.r[{a}], 0);" if name.endswith(".") else "")
    if name == "rlwinm":
        text = f"cpu.r[{a}] = rotl(cpu.r[{d}], {b}) & 0x{mask_value(rc(word), (word >> 1) & 31):x}u;"
        return text + record(word, f"cpu.r[{a}]")
    if name == "rlwnm":
        text = f"cpu.r[{a}] = rotl(cpu.r[{d}], cpu.r[{b}]) & 0x{mask_value(rc(word), (word >> 1) & 31):x}u;"
        return text + record(word, f"cpu.r[{a}]")
    if name == "rlwimi":
        m = mask_value(rc(word), (word >> 1) & 31)
        text = f"cpu.r[{a}] = (rotl(cpu.r[{d}], {b}) & 0x{m:x}u) | (cpu.r[{a}] & 0x{~m & 0xffffffff:x}u);"
        return text + record(word, f"cpu.r[{a}]")
    if name in LOADS:
        return f"cpu.r[{d}] = {LOADS[name].format(a=base(word))};"
    if name[:-1] in LOADS and name.endswith("u"):
        return f"{{ uint32_t e = {base_u(word)}; cpu.r[{d}] = {LOADS[name[:-1]].format(a='e')}; cpu.r[{a}] = e; }}"
    if name in STORES:
        return STORES[name].format(a=base(word), v=f"cpu.r[{d}]") + ";"
    if name[:-1] in STORES and name.endswith("u"):
        return f"{{ uint32_t e = {base_u(word)}; {STORES[name[:-1]].format(a='e', v=f'cpu.r[{d}]')}; cpu.r[{a}] = e; }}"
    if name in INDEXED_LOADS:
        return f"cpu.r[{d}] = {LOADS[INDEXED_LOADS[name]].format(a=indexed(word))};"
    if name in INDEXED_UPDATE_LOADS:
        return f"{{ uint32_t e = cpu.r[{a}] + cpu.r[{b}]; cpu.r[{d}] = {LOADS[INDEXED_UPDATE_LOADS[name]].format(a='e')}; cpu.r[{a}] = e; }}"
    if name in INDEXED_STORES:
        return STORES[INDEXED_STORES[name]].format(a=indexed(word), v=f"cpu.r[{d}]") + ";"
    if name in INDEXED_UPDATE_STORES:
        return f"{{ uint32_t e = cpu.r[{a}] + cpu.r[{b}]; {STORES[INDEXED_UPDATE_STORES[name]].format(a='e', v=f'cpu.r[{d}]')}; cpu.r[{a}] = e; }}"
    if name == "lmw":
        return f"{{ uint32_t e = {base(word)}; for (int i = {d}; i < 32; i++, e += 4) cpu.r[i] = rd32(e); }}"
    if name == "stmw":
        return f"{{ uint32_t e = {base(word)}; for (int i = {d}; i < 32; i++, e += 4) wr32(e, cpu.r[i]); }}"
    if name == "lswi":
        return string_load(d, b or 32, f"cpu.r[{a}]" if a else "0")
    if name == "stswi":
        return string_store(d, b or 32, f"cpu.r[{a}]" if a else "0")
    if name == "lswx":
        return string_load(d, "(cpu.xer_string & 0xff)", indexed(word))
    if name == "stswx":
        return string_store(d, "(cpu.xer_string & 0xff)", indexed(word))
    if name == "eciwx":
        return f"cpu.r[{d}] = rd32({indexed(word)});"
    if name == "ecowx":
        return f"wr32({indexed(word)}, cpu.r[{d}]);"
    if name == "lwbrx":
        return f"cpu.r[{d}] = __builtin_bswap32(rd32({indexed(word)}));"
    if name == "lhbrx":
        return f"cpu.r[{d}] = __builtin_bswap16((uint16_t)rd16({indexed(word)}));"
    if name == "stwbrx":
        return f"wr32({indexed(word)}, __builtin_bswap32(cpu.r[{d}]));"
    if name == "sthbrx":
        return f"wr16({indexed(word)}, __builtin_bswap16((uint16_t)cpu.r[{d}]));"
    if name == "lwarx":
        return f"cpu.r[{d}] = rd32({indexed(word)});"
    if name == "stwcx.":
        return f"wr32({indexed(word)}, cpu.r[{d}]); cpu.cr[0] = 2 | (uint8_t)cpu.so;"
    if name in NO_OPERATIONS:
        return ";"
    arithmetic = {
        "add": f"cpu.r[{a}] + cpu.r[{b}]", "subf": f"cpu.r[{b}] - cpu.r[{a}]",
        "mullw": f"cpu.r[{a}] * cpu.r[{b}]", "neg": f"0u - cpu.r[{a}]",
        "mulhw": f"(uint32_t)(((int64_t)(int32_t)cpu.r[{a}] * (int32_t)cpu.r[{b}]) >> 32)",
        "mulhwu": f"(uint32_t)(((uint64_t)cpu.r[{a}] * cpu.r[{b}]) >> 32)",
        "divw": f"divide_signed(cpu.r[{a}], cpu.r[{b}])",
        "divwu": f"cpu.r[{b}] ? cpu.r[{a}] / cpu.r[{b}] : 0u",
        "addc": f"add_carry(cpu.r[{a}], cpu.r[{b}], 0)", "adde": f"add_carry(cpu.r[{a}], cpu.r[{b}], cpu.ca)",
        "subfc": f"add_carry(~cpu.r[{a}], cpu.r[{b}], 1)", "subfe": f"add_carry(~cpu.r[{a}], cpu.r[{b}], cpu.ca)",
        "addze": f"add_carry(cpu.r[{a}], 0, cpu.ca)", "addme": f"add_carry(cpu.r[{a}], 0xffffffffu, cpu.ca)",
        "subfze": f"add_carry(~cpu.r[{a}], 0, cpu.ca)", "subfme": f"add_carry(~cpu.r[{a}], 0xffffffffu, cpu.ca)",
    }
    if name in arithmetic:
        value = overflowing(name, a, b) if word & OVERFLOW_ENABLE else None
        return f"cpu.r[{d}] = {value or arithmetic[name]};" + record(word, f"cpu.r[{d}]")
    bitwise = {
        "and": f"cpu.r[{d}] & cpu.r[{b}]", "or": f"cpu.r[{d}] | cpu.r[{b}]", "xor": f"cpu.r[{d}] ^ cpu.r[{b}]",
        "andc": f"cpu.r[{d}] & ~cpu.r[{b}]", "orc": f"cpu.r[{d}] | ~cpu.r[{b}]",
        "nor": f"~(cpu.r[{d}] | cpu.r[{b}])", "nand": f"~(cpu.r[{d}] & cpu.r[{b}])", "eqv": f"~(cpu.r[{d}] ^ cpu.r[{b}])",
        "slw": f"(cpu.r[{b}] & 32) ? 0u : cpu.r[{d}] << (cpu.r[{b}] & 31)",
        "srw": f"(cpu.r[{b}] & 32) ? 0u : cpu.r[{d}] >> (cpu.r[{b}] & 31)",
        "extsb": f"(uint32_t)(int8_t)cpu.r[{d}]", "extsh": f"(uint32_t)(int16_t)cpu.r[{d}]",
        "cntlzw": f"cpu.r[{d}] ? (uint32_t)__builtin_clz(cpu.r[{d}]) : 32u",
    }
    if name in bitwise:
        return f"cpu.r[{a}] = {bitwise[name]};" + record(word, f"cpu.r[{a}]")
    if name == "srawi":
        text = (f"{{ uint32_t v = cpu.r[{d}]; cpu.ca = (v >> 31) && (v & ((1u << {b}) - 1u)) ? 1 : 0;"
                f" cpu.r[{a}] = (uint32_t)((int32_t)v >> {b}); }}")
        return text + record(word, f"cpu.r[{a}]")
    if name == "sraw":
        text = (f"{{ uint32_t v = cpu.r[{d}], n = cpu.r[{b}] & 63; uint32_t s = n > 31 ? 31 : n;"
                f" cpu.ca = (v >> 31) && (n > 31 || (v & ((1u << s) - 1u))) ? 1 : 0;"
                f" cpu.r[{a}] = (uint32_t)((int32_t)v >> s); }}")
        return text + record(word, f"cpu.r[{a}]")
    if name == "mfspr":
        n = spr_number(word)
        special = {DECREMENTER: "decrementer_get()", LINK: "cpu.lr", COUNT: "cpu.ctr", XER: "xer_get()",
                   TIME_LOW: "(uint32_t)guest_time_base()", TIME_HIGH: "(uint32_t)(guest_time_base() >> 32)"}
        return f"cpu.r[{d}] = {special.get(n, f'cpu.spr[{n}]')};"
    if name == "mtspr":
        n = spr_number(word)
        if n == DECREMENTER:
            return f"decrementer_set(cpu.r[{d}]);"
        if n == LINK:
            return f"cpu.lr = cpu.r[{d}];"
        if n == COUNT:
            return f"cpu.ctr = cpu.r[{d}];"
        if n == XER:
            return f"xer_set(cpu.r[{d}]);"
        if n == CACHE_DMA_LOW:
            return f"cpu.spr[{n}] = cpu.r[{d}]; locked_cache_transfer();"
        if n in (TIME_LOW_WRITE, TIME_HIGH_WRITE):
            return f"cpu.spr[{n}] = cpu.r[{d}]; guest_time_base_set({int(n == TIME_HIGH_WRITE)}, cpu.r[{d}]);"
        return f"cpu.spr[{n}] = cpu.r[{d}];"
    if name == "mftb":
        return f"cpu.r[{d}] = {'(uint32_t)(guest_time_base() >> 32)' if spr_number(word) == TIME_HIGH else '(uint32_t)guest_time_base()'};"
    if name == "mfmsr":
        return f"cpu.r[{d}] = cpu.msr;"
    if name == "mtmsr":
        return f"cpu.msr = cpu.r[{d}];"
    if name == "mfcr":
        return f"{{ uint32_t v = 0; for (int i = 0; i < 8; i++) v = (v << 4) | cpu.cr[i]; cpu.r[{d}] = v; }}"
    if name == "mtcrf":
        fields = (word >> 12) & 0xff
        return f"for (int i = 0; i < 8; i++) if ({fields} & (0x80 >> i)) cpu.cr[i] = (cpu.r[{d}] >> (28 - i * 4)) & 15;"
    if name == "mcrf":
        return f"cpu.cr[{d >> 2}] = cpu.cr[{a >> 2}];"
    if name == "mcrxr":
        return f"cpu.cr[{d >> 2}] = (uint8_t)(xer_get() >> 28); cpu.so = cpu.ov = cpu.ca = 0;"
    condition_logic = {"crxor": "x ^ y", "cror": "x | y", "crand": "x & y", "crnor": "!(x | y)", "crnand": "!(x & y)",
                       "creqv": "!(x ^ y)", "crandc": "x & !y", "crorc": "x | !y"}
    if name in condition_logic:
        return f"{{ int x = cr_bit({a}), y = cr_bit({b}); cr_set({d}, {condition_logic[name]}); }}"
    if name == "dcbz" or name == "dcbz_l":
        return f"{{ uint32_t e = {indexed(word)} & ~31u; for (int i = 0; i < 32; i += 4) wr32(e + i, 0); }}"
    if name in ("lfs", "lfd"):
        return load_float(word, base(word), name == "lfd")
    if name in ("lfsu", "lfdu"):
        return f"{{ uint32_t e = {base_u(word)}; {load_float(word, 'e', name == 'lfdu')} cpu.r[{a}] = e; }}"
    if name in ("lfsx", "lfdx"):
        return load_float(word, indexed(word), name == "lfdx")
    if name in ("lfsux", "lfdux"):
        return f"{{ uint32_t e = cpu.r[{a}] + cpu.r[{b}]; {load_float(word, 'e', name == 'lfdux')} cpu.r[{a}] = e; }}"
    if name in ("stfs", "stfd"):
        return store_float(word, base(word), name == "stfd")
    if name in ("stfsu", "stfdu"):
        return f"{{ uint32_t e = {base_u(word)}; {store_float(word, 'e', name == 'stfdu')} cpu.r[{a}] = e; }}"
    if name in ("stfsx", "stfdx"):
        return store_float(word, indexed(word), name == "stfdx")
    if name in ("stfsux", "stfdux"):
        return f"{{ uint32_t e = cpu.r[{a}] + cpu.r[{b}]; {store_float(word, 'e', name == 'stfdux')} cpu.r[{a}] = e; }}"
    if name == "stfiwx":
        return f"wr32({indexed(word)}, (uint32_t)double_bits(cpu.f[{d}]));"
    if name in ("fadd", "fadds", "fsub", "fsubs", "fmul", "fmuls", "fdiv", "fdivs", "fmadd", "fmadds", "fmsub",
                "fmsubs", "fnmadd", "fnmadds", "fnmsub", "fnmsubs"):
        return math(word, name)
    simple = {
        "fmr": f"cpu.f[{b}]", "fneg": f"-cpu.f[{b}]", "fabs": f"fabs(cpu.f[{b}])", "fnabs": f"-fabs(cpu.f[{b}])",
        "frsqrte": f"1.0 / sqrt(cpu.f[{b}])",
        "fsel": f"cpu.f[{a}] >= 0 ? cpu.f[{rc(word)}] : cpu.f[{b}]",
    }
    if name in simple:
        return f"cpu.f[{d}] = {simple[name]};" + record_float(word)
    if name == "frsp":
        return f"cpu.f[{d}] = cpu.p[{d}] = (double)(float)cpu.f[{b}];" + record_float(word)
    if name == "fres":
        return f"cpu.f[{d}] = cpu.p[{d}] = (double)(float)(1.0 / cpu.f[{b}]);" + record_float(word)
    if name in ("fctiw", "fctiwz"):
        rounding = "ROUND_TOWARD_ZERO" if name == "fctiwz" else "cpu.fpscr & FPSCR_ROUNDING"
        return f"cpu.f[{d}] = float_to_int(cpu.f[{b}], {rounding});" + record_float(word)
    if name in ("fcmpu", "fcmpo"):
        return f"cpu.cr[{d >> 2}] = cr_float(cpu.f[{a}], cpu.f[{b}]);"
    if name == "mffs":
        return f"cpu.f[{d}] = fpscr_as_double();" + record_float(word)
    if name == "mtfsf":
        return f"fpscr_write_fields({(word >> 17) & 0xff}, (uint32_t)double_bits(cpu.f[{b}]));" + record_float(word)
    if name == "mtfsfi":
        return f"fpscr_write_field({d >> 2}, {(word >> 12) & 15});" + record_float(word)
    if name == "mtfsb0":
        return f"fpscr_clear_bit({d});" + record_float(word)
    if name == "mtfsb1":
        return f"fpscr_set_bit({d});" + record_float(word)
    if name == "mcrfs":
        return f"cpu.cr[{d >> 2}] = (uint8_t)fpscr_take_field({a >> 2});"
    if name == "mfsr":
        return f"cpu.r[{d}] = cpu.sr[{a & 15}];"
    if name == "mtsr":
        return f"cpu.sr[{a & 15}] = cpu.r[{d}];"
    if name == "mfsrin":
        return f"cpu.r[{d}] = cpu.sr[cpu.r[{b}] >> 28];"
    if name == "mtsrin":
        return f"cpu.sr[cpu.r[{b}] >> 28] = cpu.r[{d}];"
    if name.startswith("psq_"):
        update = name in ("psq_lu", "psq_stu", "psq_lux", "psq_stux")
        if name in ("psq_l", "psq_st", "psq_lu", "psq_stu"):
            offset = s12(word)
            e = f"cpu.r[{a}] + {offset}" if a or update else f"(uint32_t)({offset})"
        else:
            e = f"cpu.r[{a}] + cpu.r[{b}]" if a or update else f"cpu.r[{b}]"
        text = quantized(word, name, "e")
        return f"{{ uint32_t e = {e}; {text}{f' cpu.r[{a}] = e;' if update else ''} }}"
    if name.startswith("ps_"):
        text = paired(word, name)
        return text if "cmp" in name else text + record_float(word)
    return None


def mask_value(mb, me):
    first = 0xffffffff >> mb
    second = 0xffffffff if me == 31 else ~(0xffffffff >> (me + 1)) & 0xffffffff
    return first & second if mb <= me else first | second


def function_name(address):
    return f"f_{address:08x}"


# Set while a run-time-loaded module is translated: where its code and imports stand in,
# and the C names of the addresses its linker settles on. See modules.py.
MODULE = None
FIELD_STAND_IN = 0x7b5d  # Put in place of a linker-filled immediate, then replaced by C.


def address_text(address):
    """An address as C. A module's own addresses depend on where it was loaded."""
    if MODULE and MODULE["base"] <= address < MODULE["base"] + MODULE["size"]:
        return f"({MODULE['prefix']}_code + 0x{address - MODULE['base']:x}u)"
    if MODULE and MODULE["imports"] <= address < MODULE["imports"] + 4 * MODULE["import_count"]:
        return f"{MODULE['prefix']}_import[{(address - MODULE['imports']) // 4}]"
    return f"0x{address:x}u"


def emit_field(address, word, name):
    """An instruction whose immediate the module's linker fills in, as C that works it out."""
    kind, section, symbol, addend = MODULE["fields"][address]
    prefix = MODULE["prefix"]
    target = f"{prefix}_section[{section}]" if symbol is None else f"{prefix}_import[{symbol}]"
    target = f"({target} + 0x{addend:x}u)"
    value = f"({target} & 0xffffu)" if kind == "lo" else f"((({target}) + 0x8000u) >> 16)"
    text = emit(address, (word & 0xffff0000) | FIELD_STAND_IN, name)
    forms = [(f"0x{FIELD_STAND_IN << 16:x}u", f"({value} << 16)"), (f"0x{FIELD_STAND_IN:x}u", value),
             (str(FIELD_STAND_IN), f"(int32_t)(int16_t){value}")]
    found = [(literal, c) for literal, c in forms if text and literal in text]
    if len(found) != 1 or text.count(found[0][0]) != 1:
        raise SystemExit(f"{prefix}: can't place the linker's {kind} field in {name} at 0x{address:x}: {text}")
    return text.replace(found[0][0], found[0][1])


def write_if_changed(path, text):
    """Keeps file times, so only changed parts are compiled again."""
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def emit_function(program, start, body, starts):
    if start in REPLACED:
        reason, body_text = REPLACED[start]
        return f"void {function_name(start)}(void) {{ /* {reason} */ {body_text} }}", set(), Counter()
    lines, labels = [f"void {function_name(start)}(void) {{ POLL();"], set()
    ordered = sorted(body)
    for address in ordered:
        word = program.word(address)
        target = branch_target(address, word)
        if target and not target[1] and target[0] in body:
            labels.add(target[0])

    # Code is listed by address, and a function can start in the middle of that.
    if ordered and ordered[0] != start:
        labels.add(start)
        lines.append(f"goto L_{start:x};")

    # A line that falls through to code listed elsewhere needs a jump to it.
    for position, address in enumerate(ordered):
        following = address + 4
        next_listed = ordered[position + 1] if position + 1 < len(ordered) else None
        if following in body and next_listed != following:
            labels.add(following)

    unknown = Counter()
    for position, address in enumerate(ordered):
        word = program.word(address)
        name = name_of(word)
        if address in labels:
            lines.append(f"L_{address:x}:;")
        following = address + 4
        # Control leaves this line unless the next address in the listing follows on.
        next_listed = ordered[position + 1] if position + 1 < len(ordered) else None

        if name in ("b", "bc"):
            destination, is_call = branch_target(address, word)
            before, test = ("", "1") if name == "b" else condition(word)
            if is_call and destination == following:  # Only reads its own address, and isn't a call.
                lines.append(f"{before} if ({test}) {{ cpu.lr = {address_text(following)}; }}")
            elif is_call:
                lines.append(f"{before} if ({test}) {{ cpu.lr = {address_text(following)}; {call_text(destination, starts)} }}")
            elif destination in body:
                check = "POLL(); " if destination <= address else ""
                if destination in IDLE_LOOPS and destination <= address:
                    check = "{ extern void idle_wait(void); idle_wait(); } "
                lines.append(f"{before} if ({test}) {{ {check}goto L_{destination:x}; }}")
            else:
                lines.append(f"{before} if ({test}) {{ {tail_text(destination, starts)} }}")
        elif name == "bclr":
            before, test = condition(word)
            if word & 1:  # blrl calls the address in the link register, then carries on here.
                lines.append(f"{before} if ({test}) {{ uint32_t target = cpu.lr; cpu.lr = {address_text(following)}; dispatch(target); }}")
            else:
                lines.append(f"{before} if ({test}) {{ return; }}")
        elif name == "bcctr":
            before, test = condition(word)
            if word & 1:
                lines.append(f"{before} if ({test}) {{ cpu.lr = {address_text(following)}; dispatch(cpu.ctr); }}")
            else:
                lines.append(f"{before} if ({test}) {{ dispatch(cpu.ctr); return; }}")
        elif name == "sc":
            lines.append(f"system_call(0x{address:x}u);")
        elif name == "rfi":
            lines.append("cpu.msr = cpu.spr[27]; dispatch(cpu.spr[26]); return;")
        else:
            if MODULE and address in MODULE["fields"]:
                text = emit_field(address, word, name)
            else:
                text = emit(address, word, name) if name else None
            if text is None:
                unknown[name] += 1
                lines.append(f'unimplemented(0x{address:x}u, "{name}");')
            else:
                lines.append(text)

        ended = word in (0x4e800020, 0x4e800420, 0x4c000064) or (name == "b" and not word & 1)
        if not ended and next_listed != following and following not in body:
            lines.append(tail_text(following, starts))
        elif not ended and next_listed != following:
            lines.append(f"goto L_{following:x};")
    lines.append("}")
    return "\n".join(lines), labels, unknown


def tail_text(destination, starts):
    """A jump to other code. It must not grow the stack, as loops can span functions."""
    if destination in starts:
        return f"__attribute__((musttail)) return {function_name(destination)}();"
    return f"dispatch({address_text(destination)}); return;"


def call_text(destination, starts):
    return f"{function_name(destination)}();" if destination in starts else f"dispatch({address_text(destination)});"


def check_replaced(starts):
    """A replaced address that isn't a function start would silently never be used."""
    missing = sorted(a for a in REPLACED if a not in starts)
    if missing:
        raise SystemExit("REPLACED addresses are not function starts: " + ", ".join(f"0x{a:08x}" for a in missing))


def game_settings(tmd, values, tables):
    """What the runtime needs to know about this game, as C."""
    blocks, add, exclusive = tables or (0, 0, 0)
    high = int.from_bytes(tmd[TITLE_ID_OFFSET:TITLE_ID_OFFSET + 4], "big")
    low = int.from_bytes(tmd[TITLE_ID_OFFSET + 4:TITLE_ID_OFFSET + 8], "big")
    return ('#include "../../runtime/game.h"\n'
            f"const uint32_t game_ax_frame_done = 0x{values['ax_frame_done']:x}u;\n"
            f"const uint32_t game_calibration_flag = 0x{values.get('calibration_flag', 0):x}u;\n"
            f"const uint32_t game_title_high = 0x{high:08x}u, game_title_low = 0x{low:08x}u;\n"
            f'const char game_data_folder[] = "/title/{high:08x}/{low:08x}/data";\n'
            f"const uint32_t game_ios_version = {int.from_bytes(tmd[IOS_OFFSET + 4:IOS_OFFSET + 8], 'big')};\n"
            f"const uint32_t game_remote_blocks = 0x{blocks:x}u, game_attachment_add = 0x{add:x}u, game_attachment_xor = 0x{exclusive:x}u;\n"
            f"const int game_fifo_breakpoints = {int('--fifo-breakpoints' in sys.argv)};\n"
            f"const int game_virtual_bluetooth = {int('--virtual-bluetooth' in sys.argv)};\n")


# Each module's code stands in at MODULE_BASE + n * MODULE_SPACING, and its imports 16 MiB
# after that, within a branch's reach of all its code.
MODULE_BASE, MODULE_IMPORTS, MODULE_SPACING = 0xc0000000, 0xc1000000, 0x02000000


def export_addresses(system, functions):
    """What the program exports to its modules, by name, from its .sel export list.

    The list gives each export as an offset into a section of the program, but not where
    that section starts. A code section's start is the one that puts its exports on
    functions the translator found. It must be clear-cut, or the build stops. The runtime
    checks every address again once a module is linked.
    """
    starts = sorted(functions)
    found = {}
    for path in sorted((system / "modules").glob("*.sel")):
        exported = modules.parse_rso(path.stem, path.read_bytes())
        by_section = {}
        for name, section, value in exported.exports:
            by_section.setdefault(section, []).append((name, value))
        for section, entries in sorted(by_section.items()):
            votes = Counter(start - value for _, value in entries[:40] for start in starts)
            scored = sorted(((sum(base + value in functions for _, value in entries), base)
                             for base, _ in votes.most_common(20)), reverse=True)
            best, base = scored[0]
            runner_up = scored[1][0] if len(scored) > 1 else 0
            if best < max(3, len(entries) * 0.7) or runner_up * 2 > best:
                print(f"{path.name}: section {section} isn't code the translator found ({best} of {len(entries)} exports)")
                continue
            print(f"{path.name}: section {section} starts at {base:08x}, {best} of {len(entries)} exports on known functions")
            found.update({name: base + value for name, value in entries})
    return found


def module_imports(system):
    """Names each module calls in the program, which must be native functions."""
    names = set()
    for path in sorted((system / "modules").glob("*.rso")):
        module = modules.parse_rso(path.stem, path.read_bytes())
        names |= {module.imports[r.symbol] for r in module.external if r.kind == modules.REL24}
    return names


def emit_module(path, number, out, exports):
    """Translates a run-time-loaded module's code into native functions, wherever it is
    loaded. Writes mod<number>_*.c and returns the C for its entry in the module table."""
    global MODULE
    module = modules.parse_rso(path.stem, path.read_bytes())
    if not module:
        raise SystemExit(f"{path} isn't an RSO module")
    base, imports = MODULE_BASE + number * MODULE_SPACING, MODULE_IMPORTS + number * MODULE_SPACING
    prepared = modules.prepare(module, base, imports)
    header = bytearray(0x100)
    struct.pack_into(">I", header, 0, 0x100)
    struct.pack_into(">I", header, 0x48, base)
    struct.pack_into(">I", header, 0x90, len(prepared.code))
    struct.pack_into(">I", header, 0xe0, base)
    program = Program(bytes(header) + prepared.code)
    functions = {}
    add_starts(program, functions, prepared.starts)
    prefix = f"m{number}"
    MODULE = {"prefix": prefix, "base": base, "size": len(prepared.code), "imports": imports,
              "import_count": len(module.imports), "fields": prepared.fields}
    try:
        addresses = sorted(functions)
        starts = set(addresses)
        declarations = "\n".join(f"void {function_name(a)}(void);" for a in addresses)
        write_if_changed(out / f"{prefix}_functions.h",
                         f'#include "../../runtime/cpu.h"\nextern uint32_t {prefix}_code, {prefix}_section[], {prefix}_import[];\n'
                         f"{declarations}\n")
        unknown, files = Counter(), 0
        for first in range(0, len(addresses), FUNCTIONS_PER_FILE):
            parts = [f'#include "{prefix}_functions.h"']
            for start in addresses[first:first + FUNCTIONS_PER_FILE]:
                text, _, missing = emit_function(program, start, functions[start], starts)
                unknown += missing
                parts.append(text)
            write_if_changed(out / f"{prefix}_part{files:04}.c", "\n".join(parts) + "\n")
            files += 1
    finally:
        MODULE = None
    for stale in out.glob(f"{prefix}_part*.c"):  # Left from a translation with more files.
        if int(stale.stem.rsplit("part", 1)[1]) >= files:
            stale.unlink()
    code_start = module.sections[prepared.section][0]
    export_offsets = sorted(offset for _, section, offset in module.exports if section == prepared.section)
    sites = []
    for index in range(len(module.imports)):
        site = prepared.import_sites.get(index)
        if site is None:
            sites.append("{0, 0, 0, 0, 0}")
        elif site[0] == "branch":
            sites.append(f"{{1, 0x{site[1]:x}u, 0, 0, 0x{exports.get(module.imports[index], 0):x}u}}")
        else:
            sites.append(f"{{2, 0x{site[1]:x}u, 0x{site[2]:x}u, 0x{site[3]:x}u, 0x{exports.get(module.imports[index], 0):x}u}}")
    lines = [f'#include "{prefix}_functions.h"', '#include "../../runtime/modules.h"',
             f"uint32_t {prefix}_code, {prefix}_section[{len(module.sections)}], {prefix}_import[{max(1, len(module.imports))}];",
             f"static const ModuleFunction {prefix}_functions[] = {{" + ",".join(
                 f"{{0x{a - base:x}u,{function_name(a)}}}" for a in addresses) + "};",
             f"static const uint32_t {prefix}_exports[] = {{" + ",".join(f"0x{e:x}u" for e in export_offsets or [0]) + "};",
             f"static const uint32_t {prefix}_relocated[] = {{" + ",".join(
                 f"0x{r:x}u" for r in sorted(prepared.relocated_words)) + "};",
             f"static const ImportSite {prefix}_sites[] = {{" + ",".join(sites or ["{0, 0, 0, 0, 0}"]) + "};"]
    entry = (f'{{"{module.name}", 0x{code_start:x}u, 0x{len(prepared.code):x}u, {len(module.sections)}, {prepared.section}, '
             f"{prefix}_functions, {len(addresses)}, {prefix}_exports, {len(export_offsets)}, "
             f"{prefix}_relocated, {len(prepared.relocated_words)}, 0x{prepared.check:x}ull, "
             f"{prefix}_sites, {len(module.imports)}, &{prefix}_code, {prefix}_section, {prefix}_import}}")
    lines.append(f"const Module {prefix}_module = {entry};")
    write_if_changed(out / f"{prefix}_table.c", "\n".join(lines) + "\n")
    print(f"{module.name}: {len(addresses)} functions in {files} files")
    if unknown:
        print(f"{module.name} not yet translated:", unknown.most_common(20))
    return prefix


def write_modules(system, out, exports):
    """Every module the title loads at run time, from sys/modules, as native code."""
    prefixes = [emit_module(path, number, out, exports)
                for number, path in enumerate(sorted((system / "modules").glob("*.rso")))]
    write_if_changed(out / "modules.c", '#include "../../runtime/modules.h"\n'
                     + "".join(f"extern const Module {prefix}_module;\n" for prefix in prefixes)
                     + f"const Module *const game_modules[] = {{{', '.join(f'&{p}_module' for p in prefixes) or '0'}}};\n"
                     f"const int game_module_count = {len(prefixes)};\n")


def main():
    dol = Path(sys.argv[1])
    program = Program(dol.read_bytes())
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    functions, _ = find_functions(program)
    replaced, idle_loops, values = resolve(program, functions, load_entries())
    if "--virtual-bluetooth" in sys.argv:
        # The program's own controller code runs, including an older KPADRead left as a stub.
        replaced = {address: entry for address, entry in replaced.items()
                    if entry[1] not in DIRECT_INPUT and "Bluetooth" not in entry[0] and not entry[0].startswith("KPADRead")}
    REPLACED.update(replaced)
    IDLE_LOOPS.update(idle_loops)
    write_if_changed(out / "game.c", game_settings((dol.parent.parent / "tmd.bin").read_bytes(), values, find_remote_tables(program, functions)))
    # After resolving, as finding a function can depend on counting its neighbours.
    if "--all-code" in sys.argv:
        add_unreached_code(program, functions)
    # Program functions only its run-time modules call, by name from its export list.
    exports = export_addresses(dol.parent, functions) if (dol.parent / "modules").is_dir() else {}
    called = module_imports(dol.parent) if exports else set()
    unplaced = sorted(name for name in called if name not in exports)
    if unplaced:  # The runtime checks each has native code once a module is linked.
        print("module calls placed only at run time:", ", ".join(unplaced))
    add_starts(program, functions, {exports[name] for name in called if name in exports})
    starts = set(functions)
    check_replaced(starts)
    addresses = sorted(starts)
    declarations = "\n".join(f"void {function_name(a)}(void);" for a in addresses)
    write_if_changed(out / "functions.h", f'#include "../../runtime/cpu.h"\n{declarations}\n')

    unknown = Counter()
    files = 0
    for first in range(0, len(addresses), FUNCTIONS_PER_FILE):
        group = addresses[first:first + FUNCTIONS_PER_FILE]
        parts = ['#include "functions.h"']
        for start in group:
            text, _, missing = emit_function(program, start, functions[start], starts)
            unknown += missing
            parts.append(text)
        write_if_changed(out / f"part{files:04}.c", "\n".join(parts) + "\n")
        files += 1

    table = ",\n".join(f"{{0x{a:x}u, {function_name(a)}}}" for a in addresses)
    write_if_changed(
        out / "table.c",
        '#include "functions.h"\n'
        "typedef struct { uint32_t address; GuestFunction run; } Entry;\n"
        f"const Entry function_table[] = {{\n{table}\n}};\n"
        f"const int function_count = {len(addresses)};\n",
    )
    print(f"{len(addresses)} functions in {files} files")
    print("not yet translated:", unknown.most_common(40))
    write_modules(dol.parent, out, exports)


if __name__ == "__main__":
    main()
