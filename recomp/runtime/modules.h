// Code modules the program loads at run time, translated ahead of time by emit.py. The
// program's own linker places each one; this finds it in memory, checks it is exactly the
// module that was translated, and hands its functions to dispatch.
#pragma once
#include <stdint.h>

#include "cpu.h"

typedef struct {
  uint32_t offset;  // From the start of the module's code.
  GuestFunction run;
} ModuleFunction;

// Where to read the address an import was linked to: a patched branch (kind 1, at `first`),
// or a high and low half (kind 2, at `first` and `second`, less `addend`). Kind 3 is a REL's
// branch to a program function at a fixed address, `expected`, checked once linked. Kind 0:
// unused. For kinds 1 and 2, `expected` is where the build placed it, from the program's
// export list, or 0.
typedef struct {
  uint32_t kind, first, second, addend, expected;
} ImportSite;

typedef struct {
  const char *name;
  uint32_t code_at, code_bytes;  // Where the code sits in the module file, and its size.
  uint32_t section_count, code_section;
  // Where the linked header holds the section count and the section table's address, how
  // far the table is from the header, and the module's number (a REL's; 0 for an RSO).
  uint32_t count_field, table_field, table_offset, id;
  const ModuleFunction *functions;
  int function_count;
  const uint32_t *exports;  // Exported code offsets: likely first calls into it.
  int export_count;
  const uint32_t *relocated;  // Code offsets the linker writes, sorted, left out of the check.
  int relocated_count;
  uint64_t check;  // FNV-1a 64 over every other code word.
  const ImportSite *imports;
  int import_count;
  uint32_t *code, *sections, *import_addresses;  // Filled in once the module is found.
  uint32_t *counts;  // Runs of each function built to count them, or 0.
} Module;

// Called with an address nothing is translated for. Returns 1 if it lies in a linked
// module, which is then checked and its functions made ready, and 0 if not.
int modules_bind(uint32_t address);

// Lets dispatch run a module function at an address only known at run time.
void dispatch_register(uint32_t address, GuestFunction run);
