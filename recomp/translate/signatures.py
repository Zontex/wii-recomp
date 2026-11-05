#!/usr/bin/env python3
"""Find a game's system functions by what they contain, not where they sit.

Every game links Nintendo's libraries at its own addresses. Each function we replace
is described in system_functions.json by a short hash of each instruction, with the
parts that change between games blanked. Only hashes are kept, so the repo holds no
game code. A guess is never accepted: a function is found by one clear rule, or the
build stops and names it.

Usage: recomp/translate/signatures.py extracted/sys/main.dol
"""
import difflib
import hashlib
import json
import sys
from pathlib import Path

from decode import Program
from functions import find_functions

SYSTEM_FUNCTIONS = Path(__file__).with_name("system_functions.json")
BRANCH = 18
ADD_IMMEDIATE = 14
ADD_IMMEDIATE_SHIFTED = 15
OR_IMMEDIATE = 24
LOADS_AND_STORES = {32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55}
SMALL_DATA_REGISTERS = {2, 13}
SMALL_DATA = 13
MIN_SIMILAR_INSTRUCTIONS = 8
MIN_SIMILARITY = 0.75
MIN_LEAD = 0.2
BOARD_LIBRARY = b"RVL_SDK - WBC"


def fields(word):
    return word >> 26, (word >> 21) & 31, (word >> 16) & 31


def signed16(word):
    value = word & 0xffff
    return value - 0x10000 if value & 0x8000 else value


def masked_words(program, body):
    """The function's instructions, with whatever depends on where things landed blanked."""
    words, addressed = [], set()
    for address in sorted(body):
        word = program.word(address)
        primary, target, source = fields(word)
        if primary == BRANCH:
            word &= 0xfc000003
        elif primary == ADD_IMMEDIATE_SHIFTED and source == 0:
            addressed.add(target)
            word &= 0xffff0000
        elif primary == OR_IMMEDIATE and target in addressed:
            word &= 0xffff0000
        elif primary == ADD_IMMEDIATE and source in addressed | SMALL_DATA_REGISTERS:
            word &= 0xffff0000
        elif primary in LOADS_AND_STORES and source in addressed | SMALL_DATA_REGISTERS:
            word &= 0xffff0000
        words.append(word)
    return words


def instruction_hashes(program, body):
    return [int.from_bytes(hashlib.sha256(word.to_bytes(4, "big")).digest()[:2], "big")
            for word in masked_words(program, body)]


def small_data_base(program, functions):
    """Start-up sets r13 once, and small variables are addressed from it from then on."""
    found = high = None
    for body in functions.values():
        for address in sorted(body):
            word = program.word(address)
            primary, target, source = fields(word)
            if primary == ADD_IMMEDIATE_SHIFTED and source == 0 and target == SMALL_DATA:
                high = (word & 0xffff) << 16
            elif primary == OR_IMMEDIATE and target == SMALL_DATA and source == SMALL_DATA and high is not None:
                found = high | (word & 0xffff)
    return found


def small_data_references(program, body, base):
    """The addresses a function reaches through r13, in order."""
    found = []
    for address in sorted(body):
        word = program.word(address)
        primary, _, source = fields(word)
        if source == SMALL_DATA and (primary in LOADS_AND_STORES or primary == ADD_IMMEDIATE):
            found.append((base + signed16(word)) & 0xffffffff)
    return found


class Finder:
    """Looks up functions by their hashes in one game."""

    def __init__(self, program, functions):
        self.program, self.functions = program, functions
        self.starts = sorted(functions)
        self.hashes = {start: instruction_hashes(program, body) for start, body in functions.items()}
        self.by_hashes = {}
        for start, hashes in self.hashes.items():
            self.by_hashes.setdefault(tuple(hashes), []).append(start)
        self.closest = {}  # similar()'s answers, as it compares against every function.

    def exact(self, hashes):
        return self.by_hashes.get(tuple(hashes), [])

    def similar(self, hashes):
        """The one function clearly closest to these hashes, allowing for a changed library."""
        key = tuple(hashes)
        if key not in self.closest:
            self.closest[key] = self.find_similar(hashes)
        return self.closest[key]

    def find_similar(self, hashes):
        if len(hashes) < MIN_SIMILAR_INSTRUCTIONS:
            return None
        scores = []
        for start, other in self.hashes.items():
            if len(other) < MIN_SIMILAR_INSTRUCTIONS or not 0.6 <= len(other) / len(hashes) <= 1.6:
                continue
            matcher = difflib.SequenceMatcher(None, hashes, other, autojunk=False)
            if matcher.real_quick_ratio() < MIN_SIMILARITY or matcher.quick_ratio() < MIN_SIMILARITY:
                continue
            scores.append((matcher.ratio(), start))
        scores.sort(reverse=True)
        if not scores or scores[0][0] < MIN_SIMILARITY:
            return None
        if len(scores) > 1 and scores[0][0] - scores[1][0] < MIN_LEAD:
            return None
        return scores[0][1]

    def before(self, anchor_hashes, count):
        """The function `count` places before an anchor. Libraries keep their order."""
        anchors = self.exact(anchor_hashes)
        if len(anchors) != 1:
            return None
        position = self.starts.index(anchors[0]) - count
        return self.starts[position] if position >= 0 else None


def find_one(finder, entry, found):
    """Returns an entry's address, or None if no rule gives one answer."""
    if "before" in entry:
        address = finder.before(entry["before"]["anchor"], entry["before"]["count"])
        if address is not None:
            return address
    if "beside" in entry:
        anchor = found.get(entry["beside"]["name"])
        if anchor is None:
            return None
        if "places" in entry["beside"]:  # Neighbours keep their order when their sizes change.
            position = finder.starts.index(anchor) + entry["beside"]["places"]
            return finder.starts[position] if position >= 0 else None
        address = anchor + entry["beside"]["offset"]
        return address if address in finder.functions else None
    for hashes in entry.get("variants", []):  # Versions of the function in other SDKs.
        matches = finder.exact(hashes)
        if len(matches) == 1:
            return matches[0]
    matches = finder.exact(entry["hashes"])
    if len(matches) == 1:
        return matches[0]
    return None if matches else finder.similar(entry["hashes"])


def resolve(program, functions, entries):
    """Returns (replaced, idle loops, values) for a game, or stops if anything is unclear."""
    finder = Finder(program, functions)
    has_board = BOARD_LIBRARY in program.data
    wanted = [e for e in entries if e["group"] != "board" or has_board]
    found = {}
    progress = True
    while progress:  # A function placed beside another waits for that one.
        progress = False
        for entry in wanted:
            if entry["name"] not in found:
                address = find_one(finder, entry, found)
                if address is not None:
                    found[entry["name"]] = address
                    progress = True
    missing = [entry["name"] for entry in wanted
               if entry["name"] not in found and entry["group"] != "optional" and entry.get("or") not in found]
    if missing:
        raise SystemExit("System functions not found: " + ", ".join(missing))
    wanted = [entry for entry in wanted if entry["name"] in found]

    base = small_data_base(program, functions)
    replaced, idle_loops, values = {}, {}, {}
    for entry in wanted:  # Replacement bodies below can name these values.
        address = found[entry["name"]]
        for name, position in entry.get("small_data", {}).items():
            values[name] = small_data_references(program, functions[address], base)[position]
    for entry in wanted:
        address = found[entry["name"]]
        if "idle_offset" in entry:
            idle_loops[address + entry["idle_offset"]] = entry["reason"]
        if "body" in entry:
            replaced[address] = (entry["reason"], entry["body"].format(**values))
    return replaced, idle_loops, values


def load_entries():
    return json.loads(SYSTEM_FUNCTIONS.read_text())


def main():
    program = Program(Path(sys.argv[1]).read_bytes())
    functions, _ = find_functions(program)
    replaced, idle_loops, values = resolve(program, functions, load_entries())
    for address, (reason, _) in sorted(replaced.items()):
        print(f"0x{address:08x} {reason}")
    print("idle loops:", [hex(a) for a in idle_loops])
    print({name: hex(value) for name, value in values.items()})


if __name__ == "__main__":
    main()
