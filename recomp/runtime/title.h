// A title's native code: the translated program, as a library the console loads. The
// translator writes one Title into each library, and the console reads everything that
// differs between titles from it.
#pragma once
#include <stdint.h>

#include "cpu.h"
#include "modules.h"

// Changes whenever anything the library and the console share changes: the processor
// state, the functions translated code calls, or this description. build_file.py hashes
// the headers that define them and passes the same value to both, so a library built
// against another console is refused rather than run.
#ifndef TITLE_INTERFACE
#define TITLE_INTERFACE 0ull
#endif

typedef struct {
  uint32_t address;
  GuestFunction run;
} TitleFunction;

typedef struct {
  uint64_t interface;      // TITLE_INTERFACE when built.
  uint64_t program_check;  // FNV-1a 64 of the program file it was translated from.
  uint32_t title_high, title_low;
  uint32_t ios_version;           // The IOS the title asks for, such as 53.
  uint32_t calibration_flag;      // 0 in a game with no Balance Board.
  int virtual_bluetooth;          // Whether the game talks to bluetooth.c rather than direct input.
  int fifo_breakpoints;           // Whether the chip honours FIFO breakpoints: see gx_fifo.c.
  const TitleFunction *functions;  // Sorted by address.
  int function_count;
  const Module *const *modules;  // Modules the program loads at run time.
  int module_count;
  // A disc's apploader, which the Wii Menu runs to load the game: where its code sits,
  // how much of it there is, and the check its bytes must pass. 0 bytes if none.
  uint32_t apploader_at, apploader_bytes;
  uint64_t apploader_check;
  const TitleFunction *apploader_functions;
  int apploader_function_count;
  // How often each function has run, indexed like `functions`, for the functions built to
  // count (emit.py --profile). 0 if none do. profile.c keeps them between runs.
  uint32_t *counts;
} Title;

#define TITLE_SYMBOL "wii_recomp_title"

extern const Title *title;  // The running title.

// Loads the library at `path` and checks it was built for this console. Stops with a
// clear message if not.
void title_load(const char *path);

// The same, for a library used beside the running title, such as a disc's apploader.
const Title *title_open(const char *path);

// Checks the program about to run is the one the library was translated from.
void title_verify_program(const uint8_t *data, unsigned long length);

uint64_t title_check(const uint8_t *data, unsigned long length);  // FNV-1a 64.
