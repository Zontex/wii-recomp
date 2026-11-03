// What test_recomp_differential.py generates for differential.c.
#pragma once
#include <stdint.h>

#include "cpu.h"

enum { CASE_MEMORY = 1, CASE_ANY_NAN = 2 };

// One instruction, translated. base and index are registers the driver points at memory.
typedef struct {
  const char *name;
  uint32_t word;
  GuestFunction translated;
  int base, index, flags;
} Case;

typedef struct { uint32_t address; GuestFunction run; } Function;

// A small program. Its first function is the one that runs. direct calls its neighbours
// by name, and through_dispatch reaches them only through dispatch.
typedef struct {
  const char *name;
  uint32_t start;
  const uint32_t *words;
  int word_count;
  GuestFunction through_dispatch;
  const Function *functions;
  int function_count;
  int compare_polls;
} Program;

extern const Case cases[];
extern const int case_count;
extern const Program programs[];
extern const int program_count;
extern const uint32_t known_words[], unknown_words[];
extern const int known_word_count, unknown_word_count;
