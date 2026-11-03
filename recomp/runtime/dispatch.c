// Finds the code for a guest address. Translated code runs directly. Anything else, such
// as a module loaded at run time, runs under the interpreter.
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "dispatch.h"
#include "interp.h"

typedef struct { uint32_t address; GuestFunction run; } Entry;
extern const Entry function_table[];
extern const int function_count;

void unimplemented(uint32_t address, const char *name) {
  fprintf(stderr, "stop: %s at %08x, return address %08x\n", name, address, cpu.lr);
  exit(3);
}

// Indirect calls are frequent, so every code address gets a direct slot.
#define CODE_START 0x80000000u
#define CODE_WORDS 0x600000u
static void (*code_slots[CODE_WORDS])(void);

void fill_code_slots(void) {
  for (int i = 0; i < function_count; i++) {
    uint32_t word = (function_table[i].address - CODE_START) >> 2;
    if (word < CODE_WORDS) code_slots[word] = function_table[i].run;
  }
}

static GuestFunction search_table(uint32_t address) {
  int low = 0, high = function_count - 1;
  while (low <= high) {
    int middle = (low + high) / 2;
    uint32_t found = function_table[middle].address;
    if (found == address) return function_table[middle].run;
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
}

static __attribute__((noinline)) void run_untranslated(uint32_t address) {
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
