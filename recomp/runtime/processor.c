// Processor behaviour the translated code calls out for: system calls, traps, and the
// floating-point modes in FPSCR.
#include <stdint.h>

#include "cpu.h"
#include "gaps.h"

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

enum {
  MSR_FLOAT_EXCEPTION_MODES = 0x900,  // FE0 and FE1: either one lets FPSCR's enabled exceptions interrupt.
  SYSTEM_CALL_VECTOR = 0x80000c00u, MOST_HANDLER_WORDS = 16,
  INSTRUCTION_SYNC = 0x7c0004acu, INSTRUCTION_ISYNC = 0x4c00012cu, INSTRUCTION_RFI = 0x4c000064u,
  OPCODE_ORI = 24, OPCODE_EXTENDED = 31, EXTENDED_MFSPR = 339, EXTENDED_MTSPR = 467,
  SPR_FIELD_HID0 = 0x21f,  // SPR 1008, as the instruction holds it, with its halves swapped.
};

// Nintendo's SDK puts a handler at 0xC00 whose only work is to wait for memory: it sets
// a bit in HID0, syncs, puts HID0 back and returns. Memory here is always in step, so
// that handler has nothing to do. Anything else in a handler is a gap.
static int handler_has_no_effect(void) {
  for (uint32_t i = 0; i < MOST_HANDLER_WORDS; i++) {
    uint32_t word = rd32(SYSTEM_CALL_VECTOR + i * 4), opcode = word >> 26, extended = (word >> 1) & 0x3ff;
    if (word == INSTRUCTION_RFI) return 1;
    if (word == INSTRUCTION_SYNC || word == INSTRUCTION_ISYNC || opcode == OPCODE_ORI) continue;
    if (opcode == OPCODE_EXTENDED && (extended == EXTENDED_MFSPR || extended == EXTENDED_MTSPR) &&
        ((word >> 11) & 0x3ff) == SPR_FIELD_HID0) continue;
    return 0;
  }
  return 0;
}

void system_call(uint32_t address) {
  if (!handler_has_no_effect()) gap_hit("cpu.sc", rd32(SYSTEM_CALL_VECTOR));
  (void)address;
}

// A trap fired. A Wii would enter the game's program exception handler, which in a
// retail game reports an assert and halts. Translated code can't resume a handler at an
// instruction, so it's reported and the game carries on past it.
void trap_taken(uint32_t address) { gap_hit("cpu.tw_twi", address); }

// Arithmetic doesn't work out FPSCR's exception bits, so an exception the game has
// turned on, in both MSR and FPSCR, never comes. Checked with interrupts, rather than in
// translated code, as no retail game is known to do it.
void floating_point_exceptions_check(void) {
  static int said;
  if (said || !(cpu.msr & MSR_FLOAT_EXCEPTION_MODES) || !(cpu.fpscr & FPSCR_ENABLE_BITS)) return;
  said = 1;
  gap_hit("cpu.floating_point_program", cpu.fpscr & FPSCR_ENABLE_BITS);
}

// The game's rounding mode and non-IEEE mode, applied to the host's floating point, as
// Dolphin does (Common/x64FPURoundMode.cpp, SetSIMDMode). Every thread of the game runs
// on this one host thread, so setting it here covers them all. Non-IEEE mode flushes
// results too small to be normal to zero.
void fpscr_mode_changed(uint32_t fpscr) {
  uint32_t rounding = fpscr & FPSCR_ROUNDING;
  int non_ieee = (fpscr & FPSCR_NON_IEEE) != 0;
#if defined(__x86_64__)
  enum { ROUNDING_BITS = 0x6000, FLUSH_TO_ZERO = 0x8000 };
  static const uint32_t modes[4] = {0x0000, 0x6000, 0x4000, 0x2000};  // Nearest, toward zero, up, down.
  _mm_setcsr((_mm_getcsr() & ~(uint32_t)(ROUNDING_BITS | FLUSH_TO_ZERO)) | modes[rounding] | (non_ieee ? FLUSH_TO_ZERO : 0));
#elif defined(__aarch64__)
  static const uint64_t modes[4] = {0ull << 22, 3ull << 22, 1ull << 22, 2ull << 22};  // FPCR.RMode.
  const uint64_t rounding_bits = 3ull << 22, flush_to_zero = 1ull << 24;
  uint64_t control;
  __asm__ volatile("mrs %0, fpcr" : "=r"(control));
  control = (control & ~(rounding_bits | flush_to_zero)) | modes[rounding] | (non_ieee ? flush_to_zero : 0);
  __asm__ volatile("msr fpcr, %0" : : "r"(control));
#else
  if (rounding) gap_hit("cpu.rounding_mode_rn", rounding);
  if (non_ieee) gap_hit("cpu.non_ieee_mode_ni", 1);
#endif
}
