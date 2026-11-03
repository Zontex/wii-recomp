// Finds the code for a guest address. Translated code runs directly. Anything else, such
// as a module loaded at run time, runs under the interpreter.
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "apploader.h"
#include "console.h"
#include "dispatch.h"
#include "interp.h"
#include "modules.h"
#include "title.h"

void unimplemented(uint32_t address, const char *name) {
  fprintf(stderr, "stop: %s at %08x, return address %08x\n", name, address, cpu.lr);
  exit(3);
}

// Indirect calls are frequent, so every code address gets a direct slot.
#define CODE_START 0x80000000u
#define CODE_WORDS 0x600000u
static void (*code_slots[CODE_WORDS])(void);

void fill_code_slots(void) {
  for (int i = 0; i < title->function_count; i++) {
    uint32_t word = (title->functions[i].address - CODE_START) >> 2;
    if (word < CODE_WORDS) code_slots[word] = title->functions[i].run;
  }
}

static GuestFunction search_table(uint32_t address) {
  int low = 0, high = title->function_count - 1;
  while (low <= high) {
    int middle = (low + high) / 2;
    uint32_t found = title->functions[middle].address;
    if (found == address) return title->functions[middle].run;
    if (found < address) low = middle + 1; else high = middle - 1;
  }
  return 0;
}

GuestFunction translated_at(uint32_t address) {
  uint32_t word = (address - CODE_START) >> 2;
  if (word < CODE_WORDS && code_slots[word]) return code_slots[word];
  return search_table(address);
}

// Addresses seen so far. If this ever fills, logging stops and the game runs on.
#define LOG_SLOTS (1u << 16)
static uint32_t logged[LOG_SLOTS];
static uint32_t logged_count;

static void log_once(uint32_t address) {
  uint32_t at = (address >> 2) * 2654435761u >> 16;
  while (logged[at]) {
    if (logged[at] == address) return;
    at = (at + 1) & (LOG_SLOTS - 1);
  }
  if (logged_count >= LOG_SLOTS - 1) return;
  logged_count++;
  logged[at] = address;
  fprintf(stderr, "interpreting %08x (return address %08x)\n", address, cpu.lr);
  // Kept for the translator, which treats each one as a function start next time.
  static FILE *discovered;
  if (!discovered && getenv("WII_RECOMP_DISCOVERED")) discovered = fopen(getenv("WII_RECOMP_DISCOVERED"), "a");
  if (discovered) {
    fprintf(discovered, "%08x\n", address);
    fflush(discovered);
  }
}

void dispatch_register(uint32_t address, GuestFunction run) {
  uint32_t word = (address - CODE_START) >> 2;
  if (word >= CODE_WORDS) {
    fprintf(stderr, "stop: a module's code at %08x is outside main memory, which isn't supported yet\n", address);
    exit(4);
  }
  code_slots[word] = run;
}

static __attribute__((noinline)) void run_untranslated(uint32_t address) {
  // A module the program just linked, or a disc's apploader, translated ahead of time.
  if (modules_bind(address) || apploader_bind(address)) {
    GuestFunction run = translated_at(address);
    if (run) {
      run();
      return;
    }
  }
  console_hand_over(address);  // The Wii Menu starting the disc's game.
  if (getenv("WII_RECOMP_NATIVE_ONLY")) {
    fprintf(stderr, "stop: no native code at %08x (return address %08x), and the interpreter is off\n", address, cpu.lr);
    exit(4);
  }
  log_once(address);
  interpret(address);
}

void dispatch(uint32_t address) {
  uint32_t word = (address - CODE_START) >> 2;
  if (word < CODE_WORDS && code_slots[word]) {
    code_slots[word]();
    return;
  }
  GuestFunction run = search_table(address);
  if (run) {
    run();
    return;
  }
  run_untranslated(address);
}
