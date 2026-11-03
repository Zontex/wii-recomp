#!/usr/bin/env python3
"""Find the functions in a Wii program by following its code from known starts.

Starts are the entry point, every direct call target, and any address in data
that points at code. Each function is followed through its branches.

Usage: recomp/translate/functions.py extracted/sys/main.dol
"""
import struct
import sys
from pathlib import Path

from decode import Program, name_of

BLR = 0x4e800020
BCTR = 0x4e800420
RFI = 0x4c000064


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def branch_target(address, word):
    """Returns (target, is_call) for b and bc, or None for anything else."""
    primary = word >> 26
    absolute = word & 2
    if primary == 18:
        offset = signed(word & 0x3fffffc, 26)
    elif primary == 16:
        offset = signed(word & 0xfffc, 16)
    else:
        return None
    return (offset if absolute else address + offset) & 0xffffffff, bool(word & 1)


def is_conditional(word):
    return word >> 26 == 16 and (word >> 21) & 0x14 != 0x14


def walk(program, start):
    """Returns the addresses of a function, and the calls and unknowns within it."""
    seen, calls, unknown, tails = set(), set(), [], set()
    pending = [start]
    while pending:
        address = pending.pop()
        while address not in seen and program.is_code(address):
            seen.add(address)
            word = program.word(address)
            if name_of(word) is None:
                unknown.append(address)
                break
            target = branch_target(address, word)
            if target:
                destination, is_call = target
                if is_call:
                    calls.add(destination)
                elif word >> 26 == 18:
                    # A jump far away is a tail call; a nearby one stays in this function.
                    if abs(destination - address) > 0x4000 or destination < start:
                        tails.add(destination)
                        break
                    address = destination
                    continue
                else:
                    pending.append(destination)
            elif word in (BLR, BCTR, RFI):
                break
            address += 4
    return seen, calls, unknown, tails


def data_pointers(program):
    """Every word in a data section that holds an address inside the code.

    A program with no data sections keeps its data among its code, as the Wii Menu does.
    """
    found = set()
    mixed = all(code for code, _, _, _ in program.sections)
    for code, _, offset, size in program.sections:
        if code and not mixed:
            continue
        words = struct.unpack_from(f">{size // 4}I", program.data, offset)
        found.update(word for word in words if program.is_code(word) and not word & 3)
    return found


def built_pointers(program):
    """Code addresses made by a lis followed soon after by an addi or ori."""
    found, high = set(), {}
    for address, word in program.code():
        primary = word >> 26
        target, source = (word >> 21) & 31, (word >> 16) & 31
        if primary == 15 and source == 0:
            high[target] = (word & 0xffff) << 16
        elif primary == 14 and source in high:
            value = (high[source] + signed(word & 0xffff, 16)) & 0xffffffff
            if program.is_code(value) and not value & 3:
                found.add(value)
        elif primary == 24 and target in high:
            value = high[target] | (word & 0xffff)
            if program.is_code(value) and not value & 3:
                found.add(value)
    return found


def find_functions(program):
    starts = {program.entry} | data_pointers(program) | built_pointers(program)
    pending = list(starts)
    functions, unknown = {}, []
    while pending:
        start = pending.pop()
        if start in functions or not program.is_code(start):
            continue
        body, calls, bad, tails = walk(program, start)
        functions[start] = body
        unknown += bad
        pending += calls | tails
    return functions, sorted(set(unknown))


def add_starts(program, functions, starts):
    """Adds functions from known starts, such as addresses seen running under the interpreter."""
    pending = [start for start in starts if program.is_code(start) and not start & 3]
    while pending:
        start = pending.pop()
        if start in functions or not program.is_code(start):
            continue
        body, calls, _, tails = walk(program, start)
        functions[start] = body
        pending += calls | tails


def read_starts(path):
    """Addresses from a file, one in hex per line. A missing file gives none."""
    if not path.exists():
        return set()
    return {int(line, 16) for line in path.read_text().split() if line}


def add_unreached_code(program, functions):
    """Adds code nothing visibly points at, such as methods in tables built at run time.

    Anything that decodes becomes a function. One that is really data is never run, so
    it costs nothing, and the code found this way can't be reached by mistake.
    """
    covered = set().union(*functions.values())
    for address, word in program.code():
        if address in covered or not word or name_of(word) is None:
            continue
        pending = [address]
        while pending:
            start = pending.pop()
            if start in functions or not program.is_code(start):
                continue
            body, calls, _, tails = walk(program, start)
            functions[start] = body
            covered |= body
            pending += calls | tails


def main():
    program = Program(Path(sys.argv[1]).read_bytes())
    functions, unknown = find_functions(program)
    covered = set().union(*functions.values())
    total = sum(1 for _ in program.code())
    print(f"{len(functions)} functions, covering {len(covered)} of {total} instructions")
    print(f"{len(unknown)} unknown instructions reached", [hex(a) for a in unknown[:12]])


if __name__ == "__main__":
    main()
