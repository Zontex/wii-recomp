// Tiny test helpers: guest memory, and a check that reports its line.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"

Cpu cpu;
uint8_t *mem1, *mem2;
static int failures;
// FPSCR modes reach the host through processor.c, which most tests leave out.
#ifndef HARNESS_WITH_PROCESSOR
void fpscr_mode_changed(uint32_t fpscr) { (void)fpscr; }
#endif
#define CHECK(test) do { if (!(test)) { printf("FAILED line %d: %s\n", __LINE__, #test); failures++; } } while (0)
#define CHECK_EQ(got, want) do { unsigned long long g = (got), w = (want); \
  if (g != w) { printf("FAILED line %d: %s = %llx, wanted %llx\n", __LINE__, #got, g, w); failures++; } } while (0)
static void memory_start(void) { mem1 = calloc(1, MEM1_SIZE); mem2 = calloc(1, MEM2_SIZE); }
static int finish(void) { if (!failures) printf("ok\n"); return failures != 0; }
