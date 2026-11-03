#!/usr/bin/env python3
"""Code modules a program loads at run time, read and prepared for translation.

Nintendo's RSO modules are relocatable: the program's own linker places one in memory
and patches its code. This reads a module's sections, relocations, exports and imports,
so the translator can turn all of its code into native code ahead of time. Nothing about
where it will be loaded is guessed: every address that depends on that is expressed in
terms of the section addresses and import addresses the program's linker settles on,
which the runtime reads once the module is linked. RSO layout per Dolphin's RSO.h and
WiiBrew.

Usage: recomp/translate/modules.py <decompressed module> prints what it contains.
"""
import hashlib
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

REL24, ADDR32, LO, HA = 10, 1, 4, 6
HEADER_BYTES = 0x58
U8_MAGIC = b"\x55\xaa\x38\x2d"


def big32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


def lz_decompress(data):
    """Nintendo's LZ77, types 0x10 and 0x11. Returns None for anything else."""
    kind = data[0]
    if kind not in (0x10, 0x11) or len(data) < 4:
        return None
    size, at = data[1] | data[2] << 8 | data[3] << 16, 4
    if size == 0:
        size, at = struct.unpack_from("<I", data, 4)[0], 8
    out = bytearray()
    while len(out) < size:
        flags = data[at]
        at += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(data[at])
                at += 1
                continue
            if kind == 0x10:
                count, distance = (data[at] >> 4) + 3, ((data[at] & 15) << 8 | data[at + 1]) + 1
                at += 2
            else:
                indicator = data[at] >> 4
                if indicator == 0:
                    count = ((data[at] & 15) << 4 | data[at + 1] >> 4) + 0x11
                    distance = ((data[at + 1] & 15) << 8 | data[at + 2]) + 1
                    at += 3
                elif indicator == 1:
                    count = ((data[at] & 15) << 12 | data[at + 1] << 4 | data[at + 2] >> 4) + 0x111
                    distance = ((data[at + 2] & 15) << 8 | data[at + 3]) + 1
                    at += 4
                else:
                    count, distance = indicator + 1, ((data[at] & 15) << 8 | data[at + 1]) + 1
                    at += 2
            for _ in range(count):
                out.append(out[-distance])
    return bytes(out)


def u8_files(data):
    """(name, bytes) for each file in a U8 archive."""
    root = big32(data, 4)
    count = big32(data, root + 8)
    names = root + count * 12
    for index in range(1, count):
        kind_name, offset, size = struct.unpack_from(">III", data, root + index * 12)
        if kind_name >> 24 == 0:
            start = names + (kind_name & 0xffffff)
            yield data[start:data.index(b"\0", start)].decode(errors="replace"), data[offset:offset + size]


@dataclass
class Relocation:
    offset: int     # Where in the module file the field is.
    symbol: int     # A section number for internal ones, an import number for external ones.
    kind: int
    addend: int


@dataclass
class Rso:
    name: str
    data: bytes
    sections: list            # (file offset, size) per section; offset 0 with a size is bss.
    internal: list = field(default_factory=list)
    external: list = field(default_factory=list)
    exports: list = field(default_factory=list)   # (name, section, offset)
    imports: list = field(default_factory=list)   # names

    def code_section(self):
        """The one section holding code: the one branches are relocated in."""
        found = {self.section_of(r.offset) for r in self.internal + self.external if r.kind == REL24}
        if len(found) != 1:
            raise SystemExit(f"{self.name}: code is in {len(found)} sections, expected one")
        return found.pop()

    def section_of(self, offset):
        for number, (start, size) in enumerate(self.sections):
            if start and start <= offset < start + size:
                return number
        return None


def parse_rso(name, data):
    """Returns an Rso, or None if this isn't one."""
    if len(data) < HEADER_BYTES:
        return None
    count, section_info = big32(data, 8), big32(data, 0xc)
    if not 1 <= count <= 64 or section_info != HEADER_BYTES or section_info + count * 8 > len(data):
        return None
    tables = [big32(data, at) for at in range(0x30, 0x58, 4)]
    internal_at, internal_size, external_at, external_size = tables[:4]
    export_at, export_size, export_names, import_at, import_size, import_names = tables[4:]
    if any(at + size > len(data) for at, size in ((internal_at, internal_size), (external_at, external_size),
                                                 (export_at, export_size), (import_at, import_size))):
        return None
    sections = [(big32(data, section_info + i * 8) & ~1, big32(data, section_info + i * 8 + 4)) for i in range(count)]
    module = Rso(name, data, sections)

    def relocations(at, size):
        for entry in range(at, at + size, 12):
            offset, info, addend = struct.unpack_from(">III", data, entry)
            yield Relocation(offset, info >> 8, info & 0xff, addend)

    def text(table, offset):
        start = table + offset
        return data[start:data.index(b"\0", start)].decode(errors="replace")

    module.internal = list(relocations(internal_at, internal_size))
    module.external = list(relocations(external_at, external_size))
    for entry in range(export_at, export_at + export_size, 16):
        name_offset, value, section, _ = struct.unpack_from(">IIII", data, entry)
        module.exports.append((text(export_names, name_offset), section, value))
    for entry in range(import_at, import_at + import_size, 12):
        module.imports.append(text(import_names, big32(data, entry)))
    return module


def find_modules(files):
    """RSO modules in a title's files, (name, bytes) pairs, looking inside archives and
    through compression."""
    found = []
    for name, data in files:
        if data[:4] == U8_MAGIC:
            found += find_modules(list(u8_files(data)))
            continue
        plain = lz_decompress(data) if data[:1] in (b"\x10", b"\x11") else data
        module = parse_rso(Path(name).stem, plain) if plain else None
        if module and (module.internal or (name.endswith(".sel") and module.exports)):
            found.append(module)
    return found


def is_export_list(module):
    """A program's own export list (a .sel file): exports, but no code to load."""
    return not module.internal and not module.external and bool(module.exports)


@dataclass
class Prepared:
    """A module's code ready for the translator, at a stand-in address.

    `code` has every relocated branch inside the module pointing at its real target, and
    every call to the program pointing at a stand-in address for that import. `fields`
    gives, for each instruction whose immediate the linker fills in, how to work it out:
    ("lo" or "ha", section number or None, import number or None, addend).
    """
    module: Rso
    section: int
    base: int
    code: bytes
    import_base: int
    fields: dict
    starts: set
    import_sites: dict        # import number -> ("branch", offset) or ("pair", ha offset, lo offset, addend)
    relocated_words: set      # code offsets the linker writes, left out of the identity check
    check: int                # FNV-1a 64 over every other code word


def fnv64(words):
    value = 0xcbf29ce484222325
    for word in words:
        for shift in (24, 16, 8, 0):
            value = ((value ^ ((word >> shift) & 0xff)) * 0x100000001b3) & 0xffffffffffffffff
    return value


BRANCH_REACH = 0x2000000  # A relative branch reaches 32 MiB either way.


def patch_branch(code, at, address, target, name):
    """Points the branch at `at` (loaded at `address`) at `target`, and checks it got there."""
    distance = target - address
    if not -BRANCH_REACH <= distance < BRANCH_REACH:
        raise SystemExit(f"{name}: a branch at {at:#x} can't reach {target:#x}")
    word = struct.unpack_from(">I", code, at)[0]
    struct.pack_into(">I", code, at, (word & 0xfc000003) | (distance & 0x3fffffc))


def prepare(module, base, import_base):
    section = module.code_section()
    start, size = module.sections[section]
    if import_base < base + size:  # Each branch checks it can reach them.
        raise SystemExit(f"{module.name}: imports must stand in after its {size:#x} bytes of code")
    code = bytearray(module.data[start:start + size])
    fields, starts, import_sites, relocated = {}, set(), {}, set()

    def address_in_code(target_section, addend):
        return base + addend if target_section == section else None

    for reloc in module.internal:
        inside = start <= reloc.offset < start + size
        target = address_in_code(reloc.symbol, reloc.addend)
        if reloc.kind == ADDR32:
            if target is not None:
                starts.add(target)  # A pointer to code in the module's data, such as a method table.
            continue
        if not inside:
            raise SystemExit(f"{module.name}: relocation type {reloc.kind} outside the code at {reloc.offset:#x}")
        at = reloc.offset - start
        relocated.add(at & ~3)
        if reloc.kind == REL24:
            if target is None:
                raise SystemExit(f"{module.name}: a branch at {at:#x} leaves the code section")
            word = struct.unpack_from(">I", code, at)[0]
            patch_branch(code, at, base + at, target, module.name)
            starts.add(target) if word & 1 else None
        elif reloc.kind in (LO, HA):
            fields[base + (at & ~3)] = ("lo" if reloc.kind == LO else "ha", reloc.symbol, None, reloc.addend)
            if target is not None:
                starts.add(target)  # An address taken, such as a callback.
        else:
            raise SystemExit(f"{module.name}: relocation type {reloc.kind} in code isn't handled")

    pending_ha = {}
    for reloc in module.external:
        at = reloc.offset - start
        if not 0 <= at < size:
            raise SystemExit(f"{module.name}: an import is used outside the code at {reloc.offset:#x}")
        relocated.add(at & ~3)
        if reloc.kind == REL24:
            patch_branch(code, at, base + at, import_base + reloc.symbol * 4, module.name)
            import_sites.setdefault(reloc.symbol, ("branch", at))
        elif reloc.kind in (LO, HA):
            fields[base + (at & ~3)] = ("lo" if reloc.kind == LO else "ha", None, reloc.symbol, reloc.addend)
            if reloc.kind == HA:
                pending_ha[reloc.symbol] = (at, reloc.addend)
            elif reloc.symbol in pending_ha and reloc.symbol not in import_sites:
                high, addend = pending_ha[reloc.symbol]
                if addend == reloc.addend:
                    import_sites[reloc.symbol] = ("pair", high & ~3, at & ~3, addend)
        else:
            raise SystemExit(f"{module.name}: import relocation type {reloc.kind} isn't handled")
    missing = [module.imports[i] for i in {r.symbol for r in module.external} if i not in import_sites]
    if missing:
        raise SystemExit(f"{module.name}: no way to read where these imports went: {', '.join(missing)}")

    for name, export_section, offset in module.exports:
        if export_section == section:
            starts.add(base + offset)
    words = struct.unpack(f">{size // 4}I", code[:size & ~3])
    original = struct.unpack(f">{size // 4}I", module.data[start:start + (size & ~3)])
    check = fnv64(word for index, word in enumerate(original) if index * 4 not in relocated)
    return Prepared(module, section, base, bytes(code), import_base, fields, starts, import_sites, relocated, check)


def main():
    data = Path(sys.argv[1]).read_bytes()
    data = lz_decompress(data) or data
    module = parse_rso(Path(sys.argv[1]).stem, data)
    if not module:
        raise SystemExit("not an RSO module")
    prepared = prepare(module, 0xc0000000, 0xc1000000)
    print(f"{module.name}: {len(module.sections)} sections, code in section {prepared.section}, "
          f"{len(module.internal)} internal and {len(module.external)} external relocations, "
          f"{len(module.exports)} exports, {len(module.imports)} imports, {len(prepared.starts)} known starts, "
          f"{len(prepared.fields)} filled-in fields, check {prepared.check:016x}")


if __name__ == "__main__":
    main()
