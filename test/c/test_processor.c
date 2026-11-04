// System calls and FPSCR modes, through the real processor.c.
#define HARNESS_WITH_PROCESSOR
#include "../harness.h"
#include <string.h>

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

uint32_t hw_read(uint32_t address, int bytes) { (void)address; (void)bytes; return 0; }
void hw_write(uint32_t address, uint32_t value, int bytes) { (void)address; (void)value; (void)bytes; }

static int gaps;
static char last_gap[64];
void gap_hit(const char *id, uint32_t detail) { gaps++; strncpy(last_gap, id, sizeof last_gap - 1); (void)detail; }

// A handler that only waits for memory, as Nintendo's SDK installs.
static const uint32_t waits_for_memory[] = {
  0x7d30faa6,  // mfspr r9, HID0
  0x612a0008,  // ori r10, r9, 8
  0x7d50fba6,  // mtspr HID0, r10
  0x4c00012c,  // isync
  0x7c0004ac,  // sync
  0x60000000,  // nop
  0x7d30fba6,  // mtspr HID0, r9
  0x4c000064,  // rfi
};

static void put_handler(const uint32_t *words, int count) {
  for (int i = 0; i < count; i++) wr32(0x80000c00u + (uint32_t)i * 4, words[i]);
}

static void test_system_call(void) {
  put_handler(waits_for_memory, sizeof waits_for_memory / 4);
  system_call(0x80001000);
  CHECK_EQ(gaps, 0);
  const uint32_t stores[] = {0x90610008, 0x4c000064};  // stw r3, 8(r1); rfi
  put_handler(stores, 2);
  system_call(0x80001000);
  CHECK_EQ(gaps, 1);
  CHECK(!strcmp(last_gap, "cpu.sc"));
  const uint32_t no_return[16] = {0x60000000};  // Sixteen nops and no rfi.
  put_handler(no_return, 16);
  system_call(0x80001000);
  CHECK_EQ(gaps, 2);
}

static void test_modes(void) {
#if defined(__x86_64__)
  fpscr_mode_changed(1 | 4);  // Toward zero, non-IEEE.
  CHECK_EQ(_mm_getcsr() & 0xe000, 0x6000 | 0x8000);
  volatile double one = 1.0, three = 3.0;
  double toward_zero = one / three;
  fpscr_mode_changed(2);  // Up: the same division now lands one step higher.
  CHECK(one / three > toward_zero);
  CHECK_EQ(_mm_getcsr() & 0xe000, 0x4000);
  fpscr_mode_changed(3);  // Down.
  CHECK_EQ(_mm_getcsr() & 0xe000, 0x2000);
  fpscr_mode_changed(0);  // Back to nearest.
  CHECK_EQ(_mm_getcsr() & 0xe000, 0);
#elif defined(__aarch64__)
  uint64_t control;
  fpscr_mode_changed(1 | 4);
  __asm__ volatile("mrs %0, fpcr" : "=r"(control));
  CHECK_EQ(control & (7ull << 22), (3ull << 22) | (1ull << 24));
  fpscr_mode_changed(0);
  __asm__ volatile("mrs %0, fpcr" : "=r"(control));
  CHECK_EQ(control & (7ull << 22), 0);
#endif
  // fpscr_write applies a change of mode, and setting it back too.
  cpu.fpscr = 0;
  fpscr_write(4);
#if defined(__x86_64__)
  CHECK_EQ(_mm_getcsr() & 0x8000, 0x8000);
  fpscr_write(0);
  CHECK_EQ(_mm_getcsr() & 0x8000, 0);
#endif
}

int main(void) {
  memory_start();
  test_system_call();
  test_modes();
  return finish();
}
