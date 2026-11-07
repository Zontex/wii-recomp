// Runs guest code with no translation, one instruction at a time. It shares the processor
// state, memory and runtime hooks with translated code, and the two call each other
// through dispatch.
//
// It follows the PowerPC definition and Dolphin's interpreter, which is GPL-2.0-or-later
// like this project: Source/Core/Core/PowerPC/Interpreter. Results Dolphin measured on
// hardware are used here and named where they appear. Interpreter_Integer.cpp gives
// divide by zero, Interpreter_LoadStore.cpp gives lswx with a zero count, and cpu.h names
// its own.
//
// Written separately from translate/emit.py on purpose, as test_recomp_differential.py
// runs each against the other. Where translated code is a deliberate approximation, this
// is the same approximation, so a game behaves alike either way:
// - A trap that fires is reported to trap_taken, not delivered, and stwcx. always succeeds.
// - Arithmetic leaves FPSCR's status bits alone and ignores its rounding mode. Only
//   fctiw reads the mode.
// - fres, frsqrte, ps_res and ps_rsqrte give exact results, not the hardware's estimates.
// - Multiply-adds aren't fused, and single-precision multiplies skip Dolphin's Force25Bit.
// - cpu.h's conversions take the quick path on rare values: stfs rounds a double nothing
//   rounded to single, and fctiw leaves out the mark for a negative input giving zero.
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "interp.h"

#define OE_BIT 0x400u
#define SIGN_BIT 0x8000000000000000ull
#define CACHE_LINE_BYTES 32u
#define SPR_XER 1
#define SPR_LR 8
#define SPR_CTR 9
#define SPR_DEC 22
#define SPR_SRR0 26
#define SPR_SRR1 27
#define SPR_TBL_READ 268
#define SPR_TBU_READ 269
#define SPR_TBL_WRITE 284
#define SPR_TBU_WRITE 285
#define SPR_DMAL 923
#define EXTENDED_MASK_WITHOUT_OE 0x1ff

static inline int rd_field(uint32_t word) { return (word >> 21) & 31; }
static inline int ra_field(uint32_t word) { return (word >> 16) & 31; }
static inline int rb_field(uint32_t word) { return (word >> 11) & 31; }
static inline int rc_field(uint32_t word) { return (word >> 6) & 31; }
static inline int extended_of(uint32_t word) { return (word >> 1) & 0x3ff; }
static inline int short_extended_of(uint32_t word) { return (word >> 1) & 31; }
static inline uint32_t simm(uint32_t word) { return (uint32_t)(int32_t)(int16_t)word; }
static inline uint32_t uimm(uint32_t word) { return word & 0xffff; }
static inline uint32_t base_or_zero(int a) { return a ? cpu.r[a] : 0; }
static inline uint32_t address_d(uint32_t word) { return base_or_zero(ra_field(word)) + simm(word); }
static inline uint32_t address_x(uint32_t word) { return base_or_zero(ra_field(word)) + cpu.r[rb_field(word)]; }
static inline int spr_of(uint32_t word) { return ra_field(word) | rb_field(word) << 5; }

// tw and twi: each set bit of TO names a comparison that fires the trap.
static void trap(uint32_t address, uint32_t word, uint32_t other) {
  uint32_t value = cpu.r[ra_field(word)], to = (uint32_t)rd_field(word);
  int32_t x = (int32_t)value, y = (int32_t)other;
  if (((to & 16) && x < y) || ((to & 8) && x > y) || ((to & 4) && value == other) || ((to & 2) && value < other) ||
      ((to & 1) && value > other)) {
    trap_taken(address);
  }
}

static void unknown(uint32_t address, uint32_t word) {
  static char text[40];
  snprintf(text, sizeof text, "unknown instruction %08x", word);
  unimplemented(address, text);
}

static inline void record_int(uint32_t word, uint32_t value) {
  if (word & 1) cpu.cr[0] = cr_compare((int32_t)value, 0);
}
static inline void record_float(uint32_t word) {
  if (word & 1) cpu.cr[1] = (uint8_t)(cpu.fpscr >> 28);
}

// Every add and subtract is one adder. x and y already carry any inversion.
static void sum_into(uint32_t word, uint32_t x, uint32_t y, uint32_t carry_in, int sets_carry) {
  uint64_t wide = (uint64_t)x + y + carry_in;
  uint32_t result = (uint32_t)wide;
  if (sets_carry) cpu.ca = (uint32_t)(wide >> 32);
  if (word & OE_BIT) set_overflow((int)(((x ^ result) & (y ^ result)) >> 31));
  cpu.r[rd_field(word)] = result;
  record_int(word, result);
}

static void divide_signed_into(uint32_t word) {
  int32_t a = (int32_t)cpu.r[ra_field(word)], b = (int32_t)cpu.r[rb_field(word)];
  int overflowed = b == 0 || ((uint32_t)a == 0x80000000u && b == -1);
  uint32_t result = overflowed ? (a < 0 ? 0xffffffffu : 0) : (uint32_t)(a / b);
  if (word & OE_BIT) set_overflow(overflowed);
  cpu.r[rd_field(word)] = result;
  record_int(word, result);
}

static void divide_unsigned_into(uint32_t word) {
  uint32_t a = cpu.r[ra_field(word)], b = cpu.r[rb_field(word)];
  uint32_t result = b ? a / b : 0;
  if (word & OE_BIT) set_overflow(!b);
  cpu.r[rd_field(word)] = result;
  record_int(word, result);
}

static void multiply_into(uint32_t word) {
  int64_t product = (int64_t)(int32_t)cpu.r[ra_field(word)] * (int32_t)cpu.r[rb_field(word)];
  if (word & OE_BIT) set_overflow(product != (int32_t)product);
  cpu.r[rd_field(word)] = (uint32_t)product;
  record_int(word, (uint32_t)product);
}

static uint32_t read_spr(int n) {
  switch (n) {
    case SPR_XER: return xer_get();
    case SPR_LR: return cpu.lr;
    case SPR_CTR: return cpu.ctr;
    case SPR_DEC: return decrementer_get();
    case SPR_TBL_READ: return (uint32_t)guest_time_base();
    case SPR_TBU_READ: return (uint32_t)(guest_time_base() >> 32);
    default: return cpu.spr[n];
  }
}

static void write_spr(int n, uint32_t value) {
  switch (n) {
    case SPR_XER: xer_set(value); break;
    case SPR_LR: cpu.lr = value; break;
    case SPR_CTR: cpu.ctr = value; break;
    case SPR_DEC: decrementer_set(value); break;
    case SPR_TBL_WRITE: case SPR_TBU_WRITE:
      cpu.spr[n] = value;
      guest_time_base_set(n == SPR_TBU_WRITE, value);
      break;
    case SPR_DMAL:
      cpu.spr[n] = value;
      locked_cache_transfer();
      break;
    default: cpu.spr[n] = value; break;
  }
}

static inline uint32_t string_register(int first, uint32_t byte) { return (first + (byte >> 2)) & 31; }
static inline int string_shift(uint32_t byte) { return 24 - (int)(byte & 3) * 8; }

static void load_string(uint32_t address, uint32_t bytes, int first) {
  for (uint32_t i = 0; i < bytes; i++) {
    uint32_t r = string_register(first, i);
    if (!(i & 3)) cpu.r[r] = 0;
    cpu.r[r] |= rd8(address + i) << string_shift(i);
  }
}

static void store_string(uint32_t address, uint32_t bytes, int first) {
  for (uint32_t i = 0; i < bytes; i++) {
    wr8(address + i, cpu.r[string_register(first, i)] >> string_shift(i));
  }
}

static void zero_line(uint32_t address) {
  address &= ~(CACHE_LINE_BYTES - 1);
  for (uint32_t i = 0; i < CACHE_LINE_BYTES; i += 4) wr32(address + i, 0);
}

static inline void set_float_single(int d, uint32_t bits) { cpu.f[d] = cpu.p[d] = single_to_double(bits); }

static uint32_t condition_register(void) {
  uint32_t value = 0;
  for (int i = 0; i < 8; i++) value = value << 4 | cpu.cr[i];
  return value;
}

static void execute_31(uint32_t address, uint32_t word, int extended) {
  int d = rd_field(word), a = ra_field(word), b = rb_field(word);
  uint32_t ra = cpu.r[a], rb = cpu.r[b], rs = cpu.r[d], result;
  switch (extended) {
    case 0: cpu.cr[d >> 2] = cr_compare((int32_t)ra, (int32_t)rb); return;  // cmp
    case 32: cpu.cr[d >> 2] = cr_compare_unsigned(ra, rb); return;          // cmpl
    case 266: sum_into(word, ra, rb, 0, 0); return;                 // add
    case 10: sum_into(word, ra, rb, 0, 1); return;                  // addc
    case 138: sum_into(word, ra, rb, cpu.ca, 1); return;            // adde
    case 202: sum_into(word, ra, 0, cpu.ca, 1); return;             // addze
    case 234: sum_into(word, ra, 0xffffffffu, cpu.ca, 1); return;   // addme
    case 40: sum_into(word, ~ra, rb, 1, 0); return;                 // subf
    case 8: sum_into(word, ~ra, rb, 1, 1); return;                  // subfc
    case 136: sum_into(word, ~ra, rb, cpu.ca, 1); return;           // subfe
    case 200: sum_into(word, ~ra, 0, cpu.ca, 1); return;            // subfze
    case 232: sum_into(word, ~ra, 0xffffffffu, cpu.ca, 1); return;  // subfme
    case 104: sum_into(word, ~ra, 0, 1, 0); return;                 // neg
    case 235: multiply_into(word); return;                          // mullw
    case 491: divide_signed_into(word); return;                     // divw
    case 459: divide_unsigned_into(word); return;                   // divwu
    case 75:  // mulhw
      result = (uint32_t)(((int64_t)(int32_t)ra * (int32_t)rb) >> 32);
      cpu.r[d] = result;
      record_int(word, result);
      return;
    case 11:  // mulhwu
      result = (uint32_t)(((uint64_t)ra * rb) >> 32);
      cpu.r[d] = result;
      record_int(word, result);
      return;

    case 28: result = rs & rb; break;     // and
    case 60: result = rs & ~rb; break;    // andc
    case 124: result = ~(rs | rb); break; // nor
    case 284: result = ~(rs ^ rb); break; // eqv
    case 316: result = rs ^ rb; break;    // xor
    case 412: result = rs | ~rb; break;   // orc
    case 444: result = rs | rb; break;    // or
    case 476: result = ~(rs & rb); break; // nand
    case 26: result = rs ? (uint32_t)__builtin_clz(rs) : 32; break;  // cntlzw
    case 922: result = (uint32_t)(int32_t)(int16_t)rs; break;        // extsh
    case 954: result = (uint32_t)(int32_t)(int8_t)rs; break;         // extsb
    case 24: result = (rb & 32) ? 0 : rs << (rb & 31); break;        // slw
    case 536: result = (rb & 32) ? 0 : rs >> (rb & 31); break;       // srw
    case 792: case 824: {  // sraw, srawi
      uint32_t amount = extended == 824 ? (uint32_t)b : rb & 63;
      if (amount > 31) {
        result = (int32_t)rs < 0 ? 0xffffffffu : 0;
        cpu.ca = (int32_t)rs < 0;
      } else {
        result = (uint32_t)((int32_t)rs >> amount);
        cpu.ca = (int32_t)rs < 0 && (rs & ((1u << amount) - 1)) != 0;
      }
      break;
    }

    case 23: cpu.r[d] = rd32(address_x(word)); return;   // lwzx
    case 87: cpu.r[d] = rd8(address_x(word)); return;    // lbzx
    case 279: cpu.r[d] = rd16(address_x(word)); return;  // lhzx
    case 343: cpu.r[d] = (uint32_t)(int32_t)(int16_t)rd16(address_x(word)); return;  // lhax
    case 55: cpu.r[d] = rd32(ra + rb); cpu.r[a] = ra + rb; return;   // lwzux
    case 119: cpu.r[d] = rd8(ra + rb); cpu.r[a] = ra + rb; return;   // lbzux
    case 311: cpu.r[d] = rd16(ra + rb); cpu.r[a] = ra + rb; return;  // lhzux
    case 375: cpu.r[d] = (uint32_t)(int32_t)(int16_t)rd16(ra + rb); cpu.r[a] = ra + rb; return;  // lhaux
    case 151: wr32(address_x(word), rs); return;  // stwx
    case 215: wr8(address_x(word), rs); return;   // stbx
    case 407: wr16(address_x(word), rs); return;  // sthx
    case 183: wr32(ra + rb, rs); cpu.r[a] = ra + rb; return;  // stwux
    case 247: wr8(ra + rb, rs); cpu.r[a] = ra + rb; return;   // stbux
    case 439: wr16(ra + rb, rs); cpu.r[a] = ra + rb; return;  // sthux
    case 534: cpu.r[d] = __builtin_bswap32(rd32(address_x(word))); return;  // lwbrx
    case 790: cpu.r[d] = __builtin_bswap16((uint16_t)rd16(address_x(word))); return;  // lhbrx
    case 662: wr32(address_x(word), __builtin_bswap32(rs)); return;  // stwbrx
    case 918: wr16(address_x(word), __builtin_bswap16((uint16_t)rs)); return;  // sthbrx
    case 20: cpu.r[d] = rd32(address_x(word)); return;  // lwarx
    case 150:  // stwcx.
      wr32(address_x(word), rs);
      cpu.cr[0] = 2 | (uint8_t)cpu.so;
      return;
    case 310: cpu.r[d] = rd32(address_x(word)); return;  // eciwx
    case 438: wr32(address_x(word), rs); return;         // ecowx

    // lswi and stswi: the address has no offset, and a count of zero means 32.
    case 597: load_string(base_or_zero(a), b ? (uint32_t)b : 32, d); return;
    case 725: store_string(base_or_zero(a), b ? (uint32_t)b : 32, d); return;
    case 533: load_string(address_x(word), cpu.xer_string & 0xff, d); return;   // lswx
    case 661: store_string(address_x(word), cpu.xer_string & 0xff, d); return;  // stswx

    case 535: set_float_single(d, rd32(address_x(word))); return;  // lfsx
    case 567: set_float_single(d, rd32(ra + rb)); cpu.r[a] = ra + rb; return;  // lfsux
    case 599: cpu.f[d] = as_double(rd64(address_x(word))); return;  // lfdx
    case 631: cpu.f[d] = as_double(rd64(ra + rb)); cpu.r[a] = ra + rb; return;  // lfdux
    case 663: wr32(address_x(word), double_to_single(cpu.f[d])); return;  // stfsx
    case 695: wr32(ra + rb, double_to_single(cpu.f[d])); cpu.r[a] = ra + rb; return;  // stfsux
    case 727: wr64(address_x(word), double_bits(cpu.f[d])); return;  // stfdx
    case 759: wr64(ra + rb, double_bits(cpu.f[d])); cpu.r[a] = ra + rb; return;  // stfdux
    case 983: wr32(address_x(word), (uint32_t)double_bits(cpu.f[d])); return;  // stfiwx

    case 1014: zero_line(address_x(word)); return;  // dcbz
    case 54: case 86: case 246: case 278: case 470: case 758: case 982:  // dcbst dcbf dcbtst dcbt dcbi dcba icbi
    case 306: case 566: case 598: case 854:  // tlbie tlbsync sync eieio
      return;
    case 4: trap(address, word, cpu.r[b]); return;  // tw

    case 19: cpu.r[d] = condition_register(); return;  // mfcr
    case 144:  // mtcrf
      for (int i = 0; i < 8; i++) {
        if (((word >> 12) & 0xff) & (0x80u >> i)) cpu.cr[i] = (rs >> (28 - i * 4)) & 15;
      }
      return;
    case 83: cpu.r[d] = cpu.msr; return;  // mfmsr
    case 146: cpu.msr = rs; if (rs & 0x8000u) interrupts_turned_on(); return;  // mtmsr
    case 339: cpu.r[d] = read_spr(spr_of(word)); return;  // mfspr
    case 467: write_spr(spr_of(word), rs); return;        // mtspr
    case 371: cpu.r[d] = spr_of(word) == SPR_TBU_READ ? read_spr(SPR_TBU_READ) : read_spr(SPR_TBL_READ); return;  // mftb
    case 512:  // mcrxr
      cpu.cr[d >> 2] = (uint8_t)(xer_get() >> 28);
      cpu.so = cpu.ov = cpu.ca = 0;
      return;
    case 595: cpu.r[d] = cpu.sr[a & 15]; return;   // mfsr
    case 210: cpu.sr[a & 15] = rs; return;         // mtsr
    case 659: cpu.r[d] = cpu.sr[rb >> 28]; return;  // mfsrin
    case 242: cpu.sr[rb >> 28] = rs; return;        // mtsrin

    default:
      // As decode.py does: a form it doesn't list by itself may be a listed one with OE set.
      if (extended > EXTENDED_MASK_WITHOUT_OE) execute_31(address, word, extended & EXTENDED_MASK_WITHOUT_OE);
      else unknown(address, word);
      return;
  }
  cpu.r[a] = result;
  record_int(word, result);
}

static inline double rounded(double value) { return (double)(float)value; }
static inline void set_both(int d, double value) { cpu.f[d] = cpu.p[d] = rounded(value); }
static inline void set_pair(int d, double first, double second) { cpu.f[d] = first; cpu.p[d] = second; }

static void execute_59(uint32_t address, uint32_t word) {
  int d = rd_field(word);
  double a = cpu.f[ra_field(word)], b = cpu.f[rb_field(word)], c = cpu.f[rc_field(word)];
  switch (short_extended_of(word)) {
    case 18: set_both(d, a / b); break;  // fdivs
    case 20: set_both(d, a - b); break;  // fsubs
    case 21: set_both(d, a + b); break;  // fadds
    case 24: set_both(d, 1.0 / b); break;  // fres
    case 25: set_both(d, a * c); break;  // fmuls
    case 28: set_both(d, a * c - b); break;  // fmsubs
    case 29: set_both(d, a * c + b); break;  // fmadds
    case 30: set_both(d, -(a * c - b)); break;  // fnmsubs
    case 31: set_both(d, -(a * c + b)); break;  // fnmadds
    default: unknown(address, word); return;
  }
  record_float(word);
}

static void execute_63(uint32_t address, uint32_t word) {
  int d = rd_field(word), fa = ra_field(word), fb = rb_field(word);
  double a = cpu.f[fa], b = cpu.f[fb], c = cpu.f[rc_field(word)];
  switch (short_extended_of(word)) {
    case 18: cpu.f[d] = a / b; record_float(word); return;  // fdiv
    case 20: cpu.f[d] = a - b; record_float(word); return;  // fsub
    case 21: cpu.f[d] = a + b; record_float(word); return;  // fadd
    case 23: cpu.f[d] = a >= 0 ? c : b; record_float(word); return;  // fsel
    case 25: cpu.f[d] = a * c; record_float(word); return;  // fmul
    case 26: cpu.f[d] = 1.0 / sqrt(b); record_float(word); return;  // frsqrte
    case 28: cpu.f[d] = a * c - b; record_float(word); return;  // fmsub
    case 29: cpu.f[d] = a * c + b; record_float(word); return;  // fmadd
    case 30: cpu.f[d] = -(a * c - b); record_float(word); return;  // fnmsub
    case 31: cpu.f[d] = -(a * c + b); record_float(word); return;  // fnmadd
    default: break;
  }
  switch (extended_of(word)) {
    case 0: case 32: cpu.cr[d >> 2] = cr_float(a, b); return;  // fcmpu, fcmpo
    case 12: set_both(d, b); break;  // frsp
    case 14: cpu.f[d] = float_to_int(b, cpu.fpscr & FPSCR_ROUNDING); break;  // fctiw
    case 15: cpu.f[d] = float_to_int(b, ROUND_TOWARD_ZERO); break;  // fctiwz
    case 38: fpscr_set_bit(d); break;  // mtfsb1
    case 70: fpscr_clear_bit(d); break;  // mtfsb0
    case 40: cpu.f[d] = as_double(double_bits(b) ^ SIGN_BIT); break;  // fneg
    case 72: cpu.f[d] = b; break;  // fmr
    case 136: cpu.f[d] = as_double(double_bits(b) | SIGN_BIT); break;  // fnabs
    case 264: cpu.f[d] = as_double(double_bits(b) & ~SIGN_BIT); break;  // fabs
    case 64: cpu.cr[d >> 2] = (uint8_t)fpscr_take_field(fa >> 2); return;  // mcrfs
    case 134: fpscr_write_field(d >> 2, (word >> 12) & 15); break;  // mtfsfi
    case 583: cpu.f[d] = fpscr_as_double(); break;  // mffs
    case 711: fpscr_write_fields((word >> 17) & 0xff, (uint32_t)double_bits(b)); break;  // mtfsf
    default: unknown(address, word); return;
  }
  record_float(word);
}

static void execute_quantized(uint32_t address, uint32_t word) {
  int a = ra_field(word), b = rb_field(word), d = rd_field(word);
  int w = (word >> 10) & 1, gqr = (word >> 7) & 7;
  uint32_t effective = base_or_zero(a) + cpu.r[b], updated = cpu.r[a] + cpu.r[b];
  switch (extended_of(word) & 0x3f) {
    case 6: ps_load(effective, w, gqr, d); break;  // psq_lx
    case 7: ps_store(effective, w, gqr, d); break;  // psq_stx
    case 38: ps_load(updated, w, gqr, d); cpu.r[a] = updated; break;  // psq_lux
    case 39: ps_store(updated, w, gqr, d); cpu.r[a] = updated; break;  // psq_stux
    default: unknown(address, word); break;
  }
}

static void execute_paired(uint32_t address, uint32_t word) {
  int d = rd_field(word), fa = ra_field(word), fb = rb_field(word), fc = rc_field(word);
  double a0 = cpu.f[fa], a1 = cpu.p[fa], b0 = cpu.f[fb], b1 = cpu.p[fb], c0 = cpu.f[fc], c1 = cpu.p[fc];
  switch (short_extended_of(word)) {
    case 10: set_pair(d, rounded(a0 + b1), rounded(c1)); break;  // ps_sum0
    case 11: set_pair(d, rounded(c0), rounded(a0 + b1)); break;  // ps_sum1
    case 12: set_pair(d, rounded(a0 * c0), rounded(a1 * c0)); break;  // ps_muls0
    case 13: set_pair(d, rounded(a0 * c1), rounded(a1 * c1)); break;  // ps_muls1
    case 14: set_pair(d, rounded(a0 * c0 + b0), rounded(a1 * c0 + b1)); break;  // ps_madds0
    case 15: set_pair(d, rounded(a0 * c1 + b0), rounded(a1 * c1 + b1)); break;  // ps_madds1
    case 18: set_pair(d, rounded(a0 / b0), rounded(a1 / b1)); break;  // ps_div
    case 20: set_pair(d, rounded(a0 - b0), rounded(a1 - b1)); break;  // ps_sub
    case 21: set_pair(d, rounded(a0 + b0), rounded(a1 + b1)); break;  // ps_add
    case 23: set_pair(d, a0 >= 0 ? c0 : b0, a1 >= 0 ? c1 : b1); break;  // ps_sel
    case 24: set_pair(d, rounded(1.0 / b0), rounded(1.0 / b1)); break;  // ps_res
    case 25: set_pair(d, rounded(a0 * c0), rounded(a1 * c1)); break;  // ps_mul
    case 26: set_pair(d, rounded(1.0 / sqrt(b0)), rounded(1.0 / sqrt(b1))); break;  // ps_rsqrte
    case 28: set_pair(d, rounded(a0 * c0 - b0), rounded(a1 * c1 - b1)); break;  // ps_msub
    case 29: set_pair(d, rounded(a0 * c0 + b0), rounded(a1 * c1 + b1)); break;  // ps_madd
    case 30: set_pair(d, rounded(-(a0 * c0 - b0)), rounded(-(a1 * c1 - b1))); break;  // ps_nmsub
    case 31: set_pair(d, rounded(-(a0 * c0 + b0)), rounded(-(a1 * c1 + b1))); break;  // ps_nmadd
    default:
      switch (extended_of(word)) {
        case 0: case 32: cpu.cr[d >> 2] = cr_float(a0, b0); return;  // ps_cmpu0, ps_cmpo0
        case 64: case 96: cpu.cr[d >> 2] = cr_float(a1, b1); return;  // ps_cmpu1, ps_cmpo1
        case 40: set_pair(d, as_double(double_bits(b0) ^ SIGN_BIT), as_double(double_bits(b1) ^ SIGN_BIT)); break;  // ps_neg
        case 72: set_pair(d, b0, b1); break;  // ps_mr
        case 136: set_pair(d, as_double(double_bits(b0) | SIGN_BIT), as_double(double_bits(b1) | SIGN_BIT)); break;  // ps_nabs
        case 264: set_pair(d, as_double(double_bits(b0) & ~SIGN_BIT), as_double(double_bits(b1) & ~SIGN_BIT)); break;  // ps_abs
        case 528: set_pair(d, a0, b0); break;  // ps_merge00
        case 560: set_pair(d, a0, b1); break;  // ps_merge01
        case 592: set_pair(d, a1, b0); break;  // ps_merge10
        case 624: set_pair(d, a1, b1); break;  // ps_merge11
        case 1014: zero_line(address_x(word)); return;  // dcbz_l
        default: execute_quantized(address, word); return;
      }
  }
  record_float(word);
}

static void execute_19(uint32_t address, uint32_t word) {
  int d = rd_field(word), a = ra_field(word), b = rb_field(word);
  int x = cr_bit(a), y = cr_bit(b), value;
  switch (extended_of(word)) {
    case 0: cpu.cr[d >> 2] = cpu.cr[a >> 2]; return;  // mcrf
    case 150: return;  // isync
    case 33: value = !(x | y); break;  // crnor
    case 129: value = x & !y; break;   // crandc
    case 193: value = x ^ y; break;    // crxor
    case 225: value = !(x & y); break; // crnand
    case 257: value = x & y; break;    // crand
    case 289: value = !(x ^ y); break; // creqv
    case 417: value = x | !y; break;   // crorc
    case 449: value = x | y; break;    // cror
    default: unknown(address, word); return;
  }
  cr_set(d, value);
}

static inline void load_multiple(uint32_t address, int first) {
  for (int i = first; i < 32; i++, address += 4) cpu.r[i] = rd32(address);
}
static inline void store_multiple(uint32_t address, int first) {
  for (int i = first; i < 32; i++, address += 4) wr32(address, cpu.r[i]);
}

void interpret_instruction(uint32_t address, uint32_t word) {
  int d = rd_field(word), a = ra_field(word), b = rb_field(word);
  uint32_t ra = cpu.r[a], rs = cpu.r[d], updated = ra + simm(word), mask_bits;
  switch (word >> 26) {
    case 3: trap(address, word, simm(word)); return;  // twi
    case 4: execute_paired(address, word); return;
    case 7: cpu.r[d] = ra * simm(word); return;  // mulli
    case 8: {  // subfic
      uint64_t wide = (uint64_t)(uint32_t)~ra + simm(word) + 1;
      cpu.ca = (uint32_t)(wide >> 32);
      cpu.r[d] = (uint32_t)wide;
      return;
    }
    case 10: cpu.cr[d >> 2] = cr_compare_unsigned(ra, uimm(word)); return;  // cmpli
    case 11: cpu.cr[d >> 2] = cr_compare((int32_t)ra, (int32_t)simm(word)); return;  // cmpi
    case 12: case 13: {  // addic, addic.
      uint64_t wide = (uint64_t)ra + simm(word);
      cpu.ca = (uint32_t)(wide >> 32);
      cpu.r[d] = (uint32_t)wide;
      if (word >> 26 == 13) cpu.cr[0] = cr_compare((int32_t)wide, 0);
      return;
    }
    case 14: cpu.r[d] = base_or_zero(a) + simm(word); return;  // addi
    case 15: cpu.r[d] = base_or_zero(a) + (word << 16); return;  // addis
    case 19: execute_19(address, word); return;
    case 20: {  // rlwimi
      mask_bits = mask(rc_field(word), (word >> 1) & 31);
      cpu.r[a] = (rotl(rs, b) & mask_bits) | (ra & ~mask_bits);
      record_int(word, cpu.r[a]);
      return;
    }
    case 21:  // rlwinm
      cpu.r[a] = rotl(rs, b) & mask(rc_field(word), (word >> 1) & 31);
      record_int(word, cpu.r[a]);
      return;
    case 23:  // rlwnm
      cpu.r[a] = rotl(rs, (int)(cpu.r[b] & 31)) & mask(rc_field(word), (word >> 1) & 31);
      record_int(word, cpu.r[a]);
      return;
    case 24: cpu.r[a] = rs | uimm(word); return;  // ori
    case 25: cpu.r[a] = rs | word << 16; return;  // oris
    case 26: cpu.r[a] = rs ^ uimm(word); return;  // xori
    case 27: cpu.r[a] = rs ^ word << 16; return;  // xoris
    case 28: cpu.r[a] = rs & uimm(word); cpu.cr[0] = cr_compare((int32_t)cpu.r[a], 0); return;  // andi.
    case 29: cpu.r[a] = rs & word << 16; cpu.cr[0] = cr_compare((int32_t)cpu.r[a], 0); return;  // andis.
    case 31: execute_31(address, word, extended_of(word)); return;
    case 32: cpu.r[d] = rd32(address_d(word)); return;  // lwz
    case 33: cpu.r[d] = rd32(updated); cpu.r[a] = updated; return;  // lwzu
    case 34: cpu.r[d] = rd8(address_d(word)); return;  // lbz
    case 35: cpu.r[d] = rd8(updated); cpu.r[a] = updated; return;  // lbzu
    case 36: wr32(address_d(word), rs); return;  // stw
    case 37: wr32(updated, rs); cpu.r[a] = updated; return;  // stwu
    case 38: wr8(address_d(word), rs); return;  // stb
    case 39: wr8(updated, rs); cpu.r[a] = updated; return;  // stbu
    case 40: cpu.r[d] = rd16(address_d(word)); return;  // lhz
    case 41: cpu.r[d] = rd16(updated); cpu.r[a] = updated; return;  // lhzu
    case 42: cpu.r[d] = (uint32_t)(int32_t)(int16_t)rd16(address_d(word)); return;  // lha
    case 43: cpu.r[d] = (uint32_t)(int32_t)(int16_t)rd16(updated); cpu.r[a] = updated; return;  // lhau
    case 44: wr16(address_d(word), rs); return;  // sth
    case 45: wr16(updated, rs); cpu.r[a] = updated; return;  // sthu
    case 46: load_multiple(address_d(word), d); return;  // lmw
    case 47: store_multiple(address_d(word), d); return;  // stmw
    case 48: set_float_single(d, rd32(address_d(word))); return;  // lfs
    case 49: set_float_single(d, rd32(updated)); cpu.r[a] = updated; return;  // lfsu
    case 50: cpu.f[d] = as_double(rd64(address_d(word))); return;  // lfd
    case 51: cpu.f[d] = as_double(rd64(updated)); cpu.r[a] = updated; return;  // lfdu
    case 52: wr32(address_d(word), double_to_single(cpu.f[d])); return;  // stfs
    case 53: wr32(updated, double_to_single(cpu.f[d])); cpu.r[a] = updated; return;  // stfsu
    case 54: wr64(address_d(word), double_bits(cpu.f[d])); return;  // stfd
    case 55: wr64(updated, double_bits(cpu.f[d])); cpu.r[a] = updated; return;  // stfdu
    case 56: case 57: case 60: case 61: {  // psq_l, psq_lu, psq_st, psq_stu
      int w = (word >> 15) & 1, gqr = (word >> 12) & 7, update = word & (1u << 26);
      uint32_t offset = (uint32_t)((int32_t)(word << 20) >> 20);
      uint32_t effective = (update ? ra : base_or_zero(a)) + offset;
      if (word & (1u << 28)) ps_store(effective, w, gqr, d); else ps_load(effective, w, gqr, d);
      if (update) cpu.r[a] = effective;
      return;
    }
    case 59: execute_59(address, word); return;
    case 63: execute_63(address, word); return;
    default: unknown(address, word); return;
  }
}

// Whether a conditional branch goes, after any counting it does.
static inline int branch_taken(uint32_t word) {
  int bo = rd_field(word);
  int counter = 1, condition = 1;
  if (!(bo & 4)) {
    cpu.ctr--;
    counter = (cpu.ctr == 0) == ((bo & 2) != 0);
  }
  if (!(bo & 16)) condition = cr_bit(ra_field(word)) == ((bo & 8) != 0);
  return counter && condition;
}

static inline int can_hold_code(uint32_t address) {
  uint32_t top = address >> 28;
  return (top == 8 || top == 9) && host(address);
}

static void bad_address(uint32_t address) {
  fprintf(stderr, "stop: no code at %08x (return address %08x)\n", address, cpu.lr);
  // What a crash needs to be traced back: the registers, and the return addresses
  // saved up the stack.
  for (int i = 0; i < 32; i += 4)
    fprintf(stderr, "  r%-2d %08x %08x %08x %08x\n", i, cpu.r[i], cpu.r[i + 1], cpu.r[i + 2], cpu.r[i + 3]);
  // A bad jump is mostly a method call on a broken object, which arrives in r3.
  for (uint32_t at = cpu.r[3], n = 0; n < 2 && host(at) && host(at + 63); n++, at = rd32(at)) {
    fprintf(stderr, "  at %08x:", at);
    for (int i = 0; i < 64; i += 4) fprintf(stderr, " %08x", rd32(at + i));
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "  ctr %08x\n  called from", cpu.ctr);
  uint32_t frame = cpu.r[1];
  for (int depth = 0; depth < 24 && host(frame) && host(frame + 4); depth++) {
    frame = rd32(frame);
    if (!frame || !host(frame + 4)) break;
    fprintf(stderr, " %08x", rd32(frame + 4));
  }
  fprintf(stderr, "\n");
  exit(4);
}

// A call to the very next address only reads the link register. It isn't a function.
static inline void call(uint32_t target, uint32_t next) {
  cpu.lr = next;
  if (target != next) dispatch(target);
}

// blr means "return" only if it goes back where this was called from. Hand-set link
// registers are also how a thunk jumps. Two things mark a return:
// - The link register holds what it held on the way in, the normal case.
// - The stack is higher than on the way in, so a frame this run didn't push has been
//   popped. Code entered by a jump from a function that saved its link register ends
//   this way, with a stale link register on entry.
// Translated code can't tell these apart and always returns, which is why setjmp and
// similar are a known gap there.
void interpret(uint32_t address) {
  POLL();
  uint32_t pc = address, entry_link = cpu.lr, entry_stack = cpu.r[1];
  for (;;) {
    if (!can_hold_code(pc) || pc & 3) bad_address(pc);
    uint32_t word = rd32(pc), next = pc + 4, target;
    switch (word >> 26) {
      case 17:  // sc
        system_call(pc);
        pc = next;
        continue;
      case 18:  // b
        target = (uint32_t)((int32_t)(word << 6) >> 6) & ~3u;
        if (!(word & 2)) target += pc;
        if (word & 1) {
          call(target, next);
          pc = next;
          continue;
        }
        break;
      case 16:  // bc
        target = (uint32_t)(int32_t)(int16_t)(word & 0xfffc);
        if (!(word & 2)) target += pc;
        if (!branch_taken(word)) {
          pc = next;
          continue;
        }
        if (word & 1) {
          call(target, next);
          pc = next;
          continue;
        }
        break;
      case 19:
        switch (extended_of(word)) {
          case 16:  // bclr
            if (!branch_taken(word)) {
              pc = next;
              continue;
            }
            target = cpu.lr & ~3u;
            if (word & 1) {
              cpu.lr = next;
              dispatch(target);
              pc = next;
              continue;
            }
            if (target == (entry_link & ~3u) || cpu.r[1] > entry_stack) return;
            break;
          case 528:  // bcctr
            if (!branch_taken(word)) {
              pc = next;
              continue;
            }
            target = cpu.ctr & ~3u;
            if (word & 1) {
              cpu.lr = next;
              dispatch(target);
              pc = next;
              continue;
            }
            break;
          case 50:  // rfi
            cpu.msr = cpu.spr[SPR_SRR1];
            target = cpu.spr[SPR_SRR0] & ~3u;
            break;
          default:
            interpret_instruction(pc, word);
            pc = next;
            continue;
        }
        break;
      default:
        interpret_instruction(pc, word);
        pc = next;
        continue;
    }
    // A jump. Into translated code it's a tail call, and otherwise it stays here. Going
    // back is how a loop goes round, so it checks for interrupts as translated code does.
    if (translated_at(target)) {
      __attribute__((musttail)) return dispatch(target);
    }
    if (target <= pc) POLL();
    pc = target;
  }
}
