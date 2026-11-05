// Finds run-time-loaded modules after the program links them, and makes their
// ahead-of-time translations ready. A module is only used if its code in memory matches
// what was translated, word for word, apart from the fields its linker fills in.
#include <stdio.h>
#include <stdlib.h>

#include "modules.h"
#include "title.h"

static int is_guest(uint32_t address) { return address >= 0x80000000u && address < 0x81800000u; }

static uint64_t code_check(const Module *module, uint32_t code) {
  uint64_t value = 0xcbf29ce484222325ull;
  int next = 0;
  for (uint32_t at = 0; at + 4 <= module->code_bytes; at += 4) {
    while (next < module->relocated_count && module->relocated[next] < at) next++;
    if (next < module->relocated_count && module->relocated[next] == at) continue;
    uint32_t word = rd32(code + at);
    for (int shift = 24; shift >= 0; shift -= 8) value = (value ^ ((word >> shift) & 0xff)) * 0x100000001b3ull;
  }
  return value;
}

// The module's header, if `code` is where its code would be once linked: the linker
// turns the section table into addresses, so its own entry points back at `code`.
static int looks_linked(const Module *module, uint32_t code, uint32_t *header) {
  *header = code - module->code_at;
  if (!is_guest(*header) || !is_guest(code + module->code_bytes - 1)) return 0;
  if (module->id && rd32(*header) != module->id) return 0;
  if (rd32(*header + module->count_field) != module->section_count) return 0;
  uint32_t table = rd32(*header + module->table_field);
  if (table != *header + module->table_offset) return 0;
  return (rd32(table + module->code_section * 8) & ~1u) == code;
}

static uint32_t branch_target(uint32_t at) {
  uint32_t word = rd32(at), offset = word & 0x3fffffcu;
  if (offset & 0x2000000u) offset -= 0x4000000u;
  return word & 2 ? offset : at + offset;
}

static void bind(const Module *module, uint32_t code, uint32_t header) {
  *module->code = code;
  uint32_t table = rd32(header + module->table_field);
  for (uint32_t i = 0; i < module->section_count; i++) module->sections[i] = rd32(table + i * 8) & ~1u;
  for (int i = 0; i < module->import_count; i++) {
    const ImportSite *site = &module->imports[i];
    if (site->kind == 1 || site->kind == 3) module->import_addresses[i] = branch_target(code + site->first);
    else if (site->kind == 2) {
      uint32_t high = rd32(code + site->first) & 0xffff, low = rd32(code + site->second) & 0xffff;
      module->import_addresses[i] = (high << 16) + (uint32_t)(int32_t)(int16_t)low - site->addend;
    }
    if (site->kind == 3 && module->import_addresses[i] != site->expected) {
      fprintf(stderr, "stop: module %s branches to %08x at %x, where it was translated to call %08x\n", module->name,
              module->import_addresses[i], site->first, site->expected);
      exit(4);
    }
    if (site->kind == 1 && !translated_at(module->import_addresses[i])) {
      fprintf(stderr, "stop: module %s calls %08x, which has no native code\n", module->name, module->import_addresses[i]);
      exit(4);
    }
    if (site->kind == 1 && site->expected && module->import_addresses[i] != site->expected) {
      fprintf(stderr, "stop: module %s's import %d is linked to %08x, but the build placed it at %08x\n", module->name, i,
              module->import_addresses[i], site->expected);
      exit(4);
    }
  }
  for (int i = 0; i < module->function_count; i++) dispatch_register(code + module->functions[i].offset, module->functions[i].run);
  fprintf(stderr, "module %s: linked at %08x, %d native functions ready\n", module->name, header, module->function_count);
}

static int try_code(const Module *module, uint32_t code) {
  uint32_t header;
  if (!looks_linked(module, code, &header)) return 0;
  if (code_check(module, code) != module->check) {
    fprintf(stderr, "stop: module %s is linked at %08x, but its code isn't what was translated\n", module->name, header);
    exit(4);
  }
  bind(module, code, header);
  return 1;
}

int modules_bind(uint32_t address) {
  for (int m = 0; m < title->module_count; m++) {
    const Module *module = title->modules[m];
    for (int i = 0; i < module->export_count; i++) {
      if (try_code(module, address - module->exports[i])) return 1;
    }
    for (int i = 0; i < module->function_count; i++) {  // Reached first through a pointer, not an export.
      if (try_code(module, address - module->functions[i].offset)) return 1;
    }
  }
  return 0;
}
