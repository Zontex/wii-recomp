// The guest processor and memory, shared by the translated game and the runtime.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  uint32_t r[32];
  double f[32];   // Paired singles: first value.
  double p[32];   // Paired singles: second value.
  uint8_t cr[8];  // Each field: 8 less, 4 greater, 2 equal, 1 summary overflow.
  uint32_t lr, ctr, ca, so, msr, fpscr;
  uint32_t spr[1024];
  uint32_t ov, xer_string;  // XER's overflow bit, and its byte count and compare fields.
  uint32_t sr[16];          // Segment registers. Stored only, as nothing maps through them.
} Cpu;

extern Cpu cpu;
extern uint8_t *mem1;
extern uint8_t *mem2;

typedef void (*GuestFunction)(void);
void dispatch(uint32_t address);
GuestFunction translated_at(uint32_t address);  // 0 if nothing is translated there.
void unimplemented(uint32_t address, const char *name);
void guest_panic(void);
void input_read(void);
void input_probe(void);
void input_board_sensors(void);
void input_board_calibration(void);
void input_board_weights(void);
void guest_fatal(void);
void audio_dsp_mail(uint32_t mail);
void system_call(uint32_t address);
uint32_t hw_read(uint32_t address, int bytes);
void hw_write(uint32_t address, uint32_t value, int bytes);
uint64_t time_base(void);        // The runtime's own clock, which deadlines use.
uint64_t guest_time_base(void);  // What the game reads. It can set this one.
void guest_time_base_set(int high_half, uint32_t value);
void time_start(void);
uint64_t wii_ticks_at(int64_t unix_seconds, long utc_offset_seconds);
void ps_load(uint32_t address, int w, int gqr, int rd);
void ps_store(uint32_t address, int w, int gqr, int rs);

// Threads (threads.c): the game's context functions, replaced at translation.
void save_context(void);
void load_context(void);
void enter_exception(uint32_t kind, uint32_t handler);

// Interrupts (interrupts.c). Translated code counts down and checks in here.
extern int interrupt_budget;
void poll_interrupts(void);
void decrementer_set(uint32_t value);
uint32_t decrementer_get(void);
void locked_cache_transfer(void);
#define POLL() do { if (--interrupt_budget < 0) poll_interrupts(); } while (0)

#define MEM1_SIZE (24u << 20)
#define MEM2_SIZE (64u << 20)

static inline uint8_t *host(uint32_t address) {
  uint32_t physical = address & 0x3fffffff;
  if (physical < MEM1_SIZE) return mem1 + physical;
  if (physical >= 0x10000000 && physical < 0x10000000 + MEM2_SIZE) return mem2 + physical - 0x10000000;
  return 0;
}

static inline uint32_t rd8(uint32_t a) {
  uint8_t *p = host(a);
  return p ? *p : hw_read(a, 1);
}
static inline uint32_t rd16(uint32_t a) {
  uint8_t *p = host(a);
  if (!p) return hw_read(a, 2);
  uint16_t v;
  memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
static inline uint32_t rd32(uint32_t a) {
  uint8_t *p = host(a);
  if (!p) return hw_read(a, 4);
  uint32_t v;
  memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
static inline uint64_t rd64(uint32_t a) { return ((uint64_t)rd32(a) << 32) | rd32(a + 4); }
static inline void wr8(uint32_t a, uint32_t v) {
  uint8_t *p = host(a);
  if (p) *p = (uint8_t)v; else hw_write(a, v & 0xff, 1);
}
static inline void wr16(uint32_t a, uint32_t v) {
  uint8_t *p = host(a);
  if (!p) { hw_write(a, v & 0xffff, 2); return; }
  uint16_t s = __builtin_bswap16((uint16_t)v);
  memcpy(p, &s, 2);
}
static inline void wr32(uint32_t a, uint32_t v) {
  uint8_t *p = host(a);
  if (!p) { hw_write(a, v, 4); return; }
  uint32_t s = __builtin_bswap32(v);
  memcpy(p, &s, 4);
}
static inline void wr64(uint32_t a, uint64_t v) { wr32(a, (uint32_t)(v >> 32)); wr32(a + 4, (uint32_t)v); }

static inline float as_float(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t float_bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static inline double as_double(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static inline uint64_t double_bits(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }

static inline uint8_t cr_compare(int32_t a, int32_t b) {
  return (a < b ? 8 : a > b ? 4 : 2) | (uint8_t)cpu.so;
}
static inline uint8_t cr_compare_unsigned(uint32_t a, uint32_t b) {
  return (a < b ? 8 : a > b ? 4 : 2) | (uint8_t)cpu.so;
}
static inline uint8_t cr_float(double a, double b) {
  return a < b ? 8 : a > b ? 4 : a == b ? 2 : 1;
}
static inline int cr_bit(int bit) { return (cpu.cr[bit >> 2] >> (3 - (bit & 3))) & 1; }
static inline void cr_set(int bit, int value) {
  uint8_t mask = (uint8_t)(1 << (3 - (bit & 3)));
  cpu.cr[bit >> 2] = (uint8_t)((cpu.cr[bit >> 2] & ~mask) | (value ? mask : 0));
}
static inline uint32_t rotl(uint32_t v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline uint32_t mask(int mb, int me) {
  uint32_t a = 0xffffffffu >> mb, b = me == 31 ? 0xffffffffu : ~(0xffffffffu >> (me + 1));
  return mb <= me ? a & b : a | b;
}
static inline uint32_t add_carry(uint32_t a, uint32_t b, uint32_t in) {
  uint64_t sum = (uint64_t)a + b + in;
  cpu.ca = (uint32_t)(sum >> 32);
  return (uint32_t)sum;
}
static inline int32_t to_int(double v) {
  if (!(v == v)) return INT32_MIN;
  if (v >= 2147483647.0) return INT32_MAX;
  if (v <= -2147483648.0) return INT32_MIN;
  return (int32_t)v;
}

// XER is kept in pieces, as carry and summary overflow are read far more than the whole.
// Which bits exist is from Dolphin's UReg_XER (Gekko.h).
#define XER_STRING_BITS 0xff7fu
static inline uint32_t xer_get(void) { return cpu.so << 31 | cpu.ov << 30 | cpu.ca << 29 | cpu.xer_string; }
static inline void xer_set(uint32_t v) {
  cpu.so = v >> 31;
  cpu.ov = (v >> 30) & 1;
  cpu.ca = (v >> 29) & 1;
  cpu.xer_string = v & XER_STRING_BITS;
}
static inline void set_overflow(int overflowed) {
  cpu.ov = (uint32_t)overflowed;
  cpu.so |= (uint32_t)overflowed;
}
static inline uint32_t add_overflow(uint32_t a, uint32_t b, uint32_t in) {
  uint32_t sum = a + b + in;
  set_overflow((int)(((a ^ sum) & (b ^ sum)) >> 31));
  return sum;
}
static inline uint32_t add_carry_overflow(uint32_t a, uint32_t b, uint32_t in) {
  uint32_t sum = add_overflow(a, b, in);
  cpu.ca = (uint32_t)(((uint64_t)a + b + in) >> 32);
  return sum;
}
static inline uint32_t multiply_overflow(uint32_t a, uint32_t b) {
  int64_t product = (int64_t)(int32_t)a * (int32_t)b;
  set_overflow(product != (int32_t)product);
  return (uint32_t)product;
}
static inline int divide_overflows(uint32_t a, uint32_t b) { return !b || (a == 0x80000000u && b == 0xffffffffu); }
// Dolphin's results for a quotient that doesn't exist (Interpreter_Integer.cpp).
static inline uint32_t divide_signed(uint32_t a, uint32_t b) {
  if (divide_overflows(a, b)) return (int32_t)a < 0 ? 0xffffffffu : 0;
  return (uint32_t)((int32_t)a / (int32_t)b);
}
static inline uint32_t divide_signed_overflow(uint32_t a, uint32_t b) {
  set_overflow(divide_overflows(a, b));
  return divide_signed(a, b);
}
static inline uint32_t divide_unsigned_overflow(uint32_t a, uint32_t b) {
  set_overflow(!b);
  return b ? a / b : 0;
}

// FPSCR. Status from arithmetic isn't computed, but what the game writes behaves. The
// bit groups and the FX, VX and FEX rules are Dolphin's: Gekko.h, Interpreter_FPUtils.h
// (SetFPException, UpdateFPExceptionSummary) and Interpreter_SystemRegisters.cpp.
#define FPSCR_RESERVED 0x00000800u
#define FPSCR_FX 0x80000000u
#define FPSCR_FEX 0x40000000u
#define FPSCR_VX 0x20000000u
#define FPSCR_INVALID_BITS 0x01f80700u  // VXSNAN to VXVC, VXSOFT to VXCVI.
#define FPSCR_EXCEPTION_BITS (0x1e000000u | FPSCR_INVALID_BITS)  // OX, UX, ZX, XX and the above.
#define FPSCR_ENABLE_BITS 0x000000f8u
#define FPSCR_ROUNDING 3u
static inline void fpscr_write(uint32_t value) {
  value &= ~(uint32_t)FPSCR_RESERVED;
  value = (value & FPSCR_INVALID_BITS) ? value | FPSCR_VX : value & ~FPSCR_VX;
  cpu.fpscr = (value >> 22) & value & FPSCR_ENABLE_BITS ? value | FPSCR_FEX : value & ~FPSCR_FEX;
}
// Setting an exception bit that was clear also sets FX.
static inline void fpscr_set_bit(int bit) {
  uint32_t mask = 0x80000000u >> bit;
  fpscr_write(cpu.fpscr | mask | (mask & FPSCR_EXCEPTION_BITS & ~cpu.fpscr ? FPSCR_FX : 0));
}
static inline void fpscr_clear_bit(int bit) { fpscr_write(cpu.fpscr & ~(0x80000000u >> bit)); }
static inline void fpscr_write_field(int field, uint32_t value) {
  int shift = 28 - field * 4;
  fpscr_write((cpu.fpscr & ~(15u << shift)) | value << shift);
}
static inline void fpscr_write_fields(uint32_t fields, uint32_t value) {
  uint32_t bits = 0;
  for (int i = 0; i < 8; i++) if (fields & (1u << i)) bits |= 15u << (i * 4);
  fpscr_write((cpu.fpscr & ~bits) | (value & bits));
}
// Reading a field into CR clears any exception bits it held.
static inline uint32_t fpscr_take_field(int field) {
  int shift = 28 - field * 4;
  uint32_t value = (cpu.fpscr >> shift) & 15;
  fpscr_write(cpu.fpscr & ~((15u << shift) & (FPSCR_FX | FPSCR_EXCEPTION_BITS)));
  return value;
}
#define FLOAT_RESULT_MARK 0xfff8000000000000ull  // Hardware's top bits on an integer result.
static inline double fpscr_as_double(void) { return as_double(FLOAT_RESULT_MARK | cpu.fpscr); }

// fctiw and fctiwz. The top word is what Dolphin measured on hardware
// (Interpreter_FloatingPoint.cpp, ConvertToInteger). Only stfd can see it. Hardware also
// marks a zero result from a negative input there. That isn't copied: fctiwz is common,
// and the mark cost more than the conversion's other work put together.
#define ROUND_NEAREST 0
#define ROUND_TOWARD_ZERO 1
#define ROUND_UP 2
#define ROUND_DOWN 3
static inline double float_to_int(double v, uint32_t rounding) {
  int32_t value;
  switch (rounding) {
    case ROUND_NEAREST: value = to_int(rint(v)); break;
    case ROUND_UP: value = to_int(ceil(v)); break;
    case ROUND_DOWN: value = to_int(floor(v)); break;
    default: value = to_int(v); break;
  }
  return as_double(FLOAT_RESULT_MARK | (uint32_t)value);
}

// The processor moves a float into a register by rearranging its bits. A C cast does
// the same, except that it quiets a signaling NaN, which is any data with an exponent
// of all ones and the top fraction bit clear: the color ff a0 40 ff, say.
static inline double single_to_double(uint32_t v) {
  if (__builtin_expect((v & 0x7f800000u) == 0x7f800000u, 0)) {
    return as_double((uint64_t)(v & 0x80000000u) << 32 | 0x7ff0000000000000ull | (uint64_t)(v & 0x007fffffu) << 29);
  }
  return (double)as_float(v);
}

// Storing a NaN rearranges its bits, as loading does. Anything else goes through a cast,
// which is far quicker here than rearranging, and stfs is common. The cast rounds where
// hardware truncates (Dolphin's ConvertToSingle, Interpreter_FPUtils.h), and gives
// infinity for a double too big for a float. Both need a value no one rounded to single.
#define DOUBLE_SIGN 0x8000000000000000ull
static inline uint32_t double_top_bits(uint64_t x) { return (uint32_t)((x >> 32) & 0xc0000000u) | (uint32_t)((x >> 29) & 0x3fffffffu); }
static inline uint32_t double_to_single(double d) {
  if (__builtin_expect(d != d, 0)) return double_top_bits(double_bits(d));
  return float_bits((float)d);
}
// psq_st also flushes denormals, unlike stfs (ConvertToSingleFTZ, the same file).
static inline uint32_t double_to_single_flushed(double d) {
  uint32_t bits = double_to_single(d);
  return bits & 0x7f800000u ? bits : bits & 0x80000000u;
}
