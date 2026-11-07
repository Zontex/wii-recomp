#!/usr/bin/env python3
"""Code modules a program loads at run time, read and prepared for translation.

Two kinds: the Wii Menu's RSO modules, which name what they import, and the SDK's REL
modules (OSLink), which most games load the bulk of their code in and which address the
program directly. Both are relocatable: the program's own linker places one in memory and
patches its code. This reads a module's sections, relocations, exports and imports, so the
translator can turn all of its code into native code ahead of time. Nothing about where
it will be loaded is guessed: every address that depends on that is expressed in terms of
the section addresses and import addresses the program's linker settles on, which the
runtime reads once the module is linked. RSO layout per Dolphin's RSO.h and WiiBrew; REL
layout per WiiBrew and the SDK's OSModule.h as the decompilation projects describe it.

Usage: recomp/translate/modules.py <decompressed module> prints what it contains.
"""
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

REL24, ADDR32, ADDR16, LO, HI, HA = 10, 1, 3, 4, 5, 6
REL_NOP, REL_SECTION, REL_END = 201, 202, 203  # Steer a REL's relocation list.
HEADER_BYTES = 0x58
REL_HEADER_BYTES = {1: 0x40, 2: 0x48, 3: 0x4c}  # By version.
U8_MAGIC = b"\x55\xaa\x38\x2d"


class Unsupported(SystemExit):
    """A module the translator can't prepare. The build carries on, leaving it to the interpreter."""


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


def yaz0_decompress(data):
    """Nintendo's Yaz0. Returns None for anything else."""
    if data[:4] != b"Yaz0" or len(data) < 16:
        return None
    size, at, out = big32(data, 4), 16, bytearray()
    while len(out) < size:
        flags = data[at]
        at += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                out.append(data[at])
                at += 1
                continue
            distance = ((data[at] & 15) << 8 | data[at + 1]) + 1
            count = data[at] >> 4
            if count:
                count += 2
                at += 2
            else:
                count = data[at + 2] + 0x12
                at += 3
            for _ in range(count):
                out.append(out[-distance])
    return bytes(out)


def decompress(data):
    """The plain bytes of a file stored with any of the compressions modules are found in."""
    if data[:4] == b"Yaz0":
        return yaz0_decompress(data)
    if data[:1] in (b"\x10", b"\x11"):
        return lz_decompress(data) or data
    return data


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
    kind: str = "rso"
    module_id: int = 0                            # A REL's number, which its relocations refer to.
    fixed: list = field(default_factory=list)     # A REL's relocations against the program, at fixed addresses.
    executable: set = field(default_factory=set)  # A REL's sections marked as code.
    bss_bytes: int = 0

    def code_section(self):
        """The one section holding code: for a REL, the one marked so; for an RSO, the one
        branches are relocated in."""
        if self.kind == "rel":
            found = set(self.executable)
        else:
            found = {self.section_of(r.offset) for r in self.internal + self.external if r.kind == REL24}
        if len(found) != 1:
            raise Unsupported(f"{self.name}: code is in {len(found)} sections, expected one")
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


def parse_rel(name, data):
    """Returns an Rso of kind "rel" for an SDK REL module, or None if this isn't one.

    A REL's relocations come in one list per module they point into: its own number for
    its own sections, 0 for the program at fixed addresses, and any other number for
    another REL. Each list steers through the module with R_DOLPHIN_SECTION (start of a
    section) and offsets from the entry before. Relocations into other RELs are kept in
    `external`, by module number, for the translator to turn down.
    """
    if len(data) < 0x40:
        return None
    module_id, count, section_info, version = big32(data, 0), big32(data, 0xc), big32(data, 0x10), big32(data, 0x1c)
    if big32(data, 4) or big32(data, 8) or REL_HEADER_BYTES.get(version) != section_info:
        return None
    relocations_at, imports_at, imports_size = big32(data, 0x24), big32(data, 0x28), big32(data, 0x2c)
    if not 2 <= count <= 64 or section_info + count * 8 > len(data) or imports_at + imports_size > len(data) \
            or imports_size % 8 or relocations_at > len(data):
        return None
    sections, executable = [], set()
    for i in range(count):
        offset, size = big32(data, section_info + i * 8), big32(data, section_info + i * 8 + 4)
        if offset & 1 and size:
            executable.add(i)
        if (offset & ~1) + size > len(data) and offset & ~1:
            return None
        sections.append((offset & ~1, size))
    module = Rso(name, data, sections, kind="rel", module_id=module_id, executable=executable, bss_bytes=big32(data, 0x20))
    for at in range(imports_at, imports_at + imports_size, 8):
        target_module, start = big32(data, at), big32(data, at + 4)
        section, place, entry = None, 0, start
        while entry + 8 <= len(data):
            step, kind, symbol, addend = struct.unpack_from(">HBBI", data, entry)
            entry += 8
            place += step
            if kind == REL_END:
                break
            if kind == REL_SECTION:
                section, place = symbol, 0
                continue
            if kind == REL_NOP:
                continue
            if section is None or not sections[section][0]:
                return None  # Patching nothing, or the zeroed bss: not a REL as described.
            relocation = Relocation(sections[section][0] + place, symbol, kind, addend)
            if target_module == module_id:
                module.internal.append(relocation)
            elif target_module == 0:
                module.fixed.append(relocation)
            else:
                relocation.symbol = (target_module, symbol)
                module.external.append(relocation)
        else:
            return None
    for name_, section_at, offset_at in (("_prolog", 0x30, 0x34), ("_epilog", 0x31, 0x38), ("_unresolved", 0x32, 0x3c)):
        if data[section_at]:
            module.exports.append((name_, data[section_at], big32(data, offset_at)))
    return module


def parse_module(name, data):
    """An RSO or a REL module, or None."""
    return parse_rso(name, data) or parse_rel(name, data)


def find_modules(files):
    """RSO and REL modules in a title's files, (name, bytes) pairs, looking inside archives
    and through compression."""
    found = []
    for name, data in files:
        if data[:4] == U8_MAGIC:
            found += find_modules(list(u8_files(data)))
            continue
        plain = decompress(data)
        module = parse_module(Path(name).stem, plain) if plain else None
        if module and (module.internal or module.fixed or (name.endswith(".sel") and module.exports)):
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
    ("lo", "hi" or "ha", section number or None, import number or None, addend). A REL's
    halves of fixed program addresses are written straight into `code`, and each program
    function it calls gets an import stand-in of its own, `fixed_calls` giving the address.
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
    fixed_calls: dict = field(default_factory=dict)  # import number -> the program address it calls


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
        raise Unsupported(f"{name}: a branch at {at:#x} can't reach {target:#x}")
    word = struct.unpack_from(">I", code, at)[0]
    struct.pack_into(">I", code, at, (word & 0xfc000003) | (distance & 0x3fffffc))


def prepare(module, base, import_base):
    section = module.code_section()
    start, size = module.sections[section]
    if import_base < base + size:  # Each branch checks it can reach them.
        raise Unsupported(f"{module.name}: imports must stand in after its {size:#x} bytes of code")
    code = bytearray(module.data[start:start + size])
    fields, starts, import_sites, relocated = {}, set(), {}, set()
    halves = {LO: "lo", ADDR16: "lo", HI: "hi", HA: "ha"}

    def address_in_code(target_section, addend):
        return base + addend if target_section == section else None

    for reloc in module.internal:
        inside = start <= reloc.offset < start + size
        target = address_in_code(reloc.symbol, reloc.addend)
        if reloc.kind == ADDR32:
            if target is not None:
                starts.add(target)  # A pointer to code in the module's data, such as a method table.
            if inside:
                relocated.add((reloc.offset - start) & ~3)
            continue
        if not inside:
            raise Unsupported(f"{module.name}: relocation type {reloc.kind} outside the code at {reloc.offset:#x}")
        at = reloc.offset - start
        relocated.add(at & ~3)
        if reloc.kind == REL24:
            if target is None:
                raise Unsupported(f"{module.name}: a branch at {at:#x} leaves the code section")
            word = struct.unpack_from(">I", code, at)[0]
            patch_branch(code, at, base + at, target, module.name)
            starts.add(target) if word & 1 else None
        elif reloc.kind in halves:
            fields[base + (at & ~3)] = (halves[reloc.kind], reloc.symbol, None, reloc.addend)
            if target is not None:
                starts.add(target)  # An address taken, such as a callback.
        else:
            raise Unsupported(f"{module.name}: relocation type {reloc.kind} in code isn't handled")

    if module.kind == "rel" and module.external:
        others = sorted({r.symbol[0] for r in module.external})
        raise Unsupported(f"{module.name}: links against other modules ({', '.join(map(str, others))}), not handled yet")
    pending_ha, pending_lo = {}, {}
    for reloc in module.external:
        at = reloc.offset - start
        if not 0 <= at < size:
            raise Unsupported(f"{module.name}: an import is used outside the code at {reloc.offset:#x}")
        relocated.add(at & ~3)
        if reloc.kind == REL24:
            patch_branch(code, at, base + at, import_base + reloc.symbol * 4, module.name)
            import_sites.setdefault(reloc.symbol, ("branch", at))
        elif reloc.kind in (LO, HA):
            fields[base + (at & ~3)] = ("lo" if reloc.kind == LO else "ha", None, reloc.symbol, reloc.addend)
            # The halves of an address can come in either order in the list.
            (pending_ha if reloc.kind == HA else pending_lo).setdefault((reloc.symbol, reloc.addend), at)
        else:
            raise Unsupported(f"{module.name}: import relocation type {reloc.kind} isn't handled")
    for (symbol, addend), high in pending_ha.items():
        if symbol not in import_sites and (symbol, addend) in pending_lo:
            import_sites[symbol] = ("pair", high & ~3, pending_lo[(symbol, addend)] & ~3, addend)
    fixed_calls = prepare_fixed(module, base, import_base, code, start, size, import_sites, relocated)
    missing = [module.imports[i] for i in {r.symbol for r in module.external} if i not in import_sites]
    if missing:
        raise Unsupported(f"{module.name}: no way to read where these imports went: {', '.join(missing)}")

    for name, export_section, offset in module.exports:
        if export_section == section:
            starts.add(base + offset)
    original = struct.unpack(f">{size // 4}I", module.data[start:start + (size & ~3)])
    check = fnv64(word for index, word in enumerate(original) if index * 4 not in relocated)
    return Prepared(module, section, base, bytes(code), import_base, fields, starts, import_sites, relocated, check,
                    fixed_calls)


def prepare_fixed(module, base, import_base, code, start, size, import_sites, relocated):
    """A REL's relocations against the program, whose addresses never move. Halves of an
    address are written into the code now. Each program function it branches to gets an
    import stand-in after the module's own imports, so the translator can call it by name
    and the runtime can check the linked branch went there. Returns {import number: address}."""
    calls, numbers = {}, {}
    for reloc in module.fixed:
        at = reloc.offset - start
        if not 0 <= at < size:
            continue  # In the module's data: read at run time, like any other data.
        relocated.add(at & ~3)
        address = reloc.addend
        if reloc.kind == REL24:
            number = numbers.setdefault(address, len(module.imports) + len(numbers))
            calls[number] = address
            patch_branch(code, at, base + at, import_base + number * 4, module.name)
            import_sites.setdefault(number, ("fixed", at, address))
        elif reloc.kind in (LO, ADDR16, HI, HA):
            value = {LO: address, ADDR16: address, HI: address >> 16, HA: (address + 0x8000) >> 16}[reloc.kind]
            struct.pack_into(">H", code, at, value & 0xffff)
        elif reloc.kind == ADDR32:
            struct.pack_into(">I", code, at, address)
        else:
            raise Unsupported(f"{module.name}: relocation type {reloc.kind} against the program isn't handled")
    return calls


def main():
    data = Path(sys.argv[1]).read_bytes()
    module = parse_module(Path(sys.argv[1]).stem, decompress(data))
    if not module:
        raise SystemExit("not an RSO or REL module")
    prepared = prepare(module, 0xc0000000, 0xc1000000)
    print(f"{module.name}: {len(module.sections)} sections, code in section {prepared.section}, "
          f"{len(module.internal)} internal and {len(module.external)} external relocations, "
          f"{len(module.fixed)} fixed, {len(module.exports)} exports, {len(module.imports)} imports, "
          f"{len(prepared.fixed_calls)} program functions called, {len(prepared.starts)} known starts, "
          f"{len(prepared.fields)} filled-in fields, check {prepared.check:016x}")


if __name__ == "__main__":
    main()
