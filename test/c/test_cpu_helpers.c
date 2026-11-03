// What translated code and the interpreter share, so comparing them can't check it.
// Expected values are worked out by hand from the PowerPC definition, and from Dolphin
// where hardware behaviour is measured rather than defined.
#include "../harness.h"
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}

#define NEAREST 0
#define ZERO 1
#define UP 2
#define DOWN 3
#define MARK 0xfff8000000000000ull

static uint64_t convert(double value, int mode) { return double_bits(float_to_int(value, (uint32_t)mode)); }

static void test_float_to_int(void) {
  CHECK_EQ(convert(2.5, NEAREST), MARK | 2);   // Ties go to even.
  CHECK_EQ(convert(3.5, NEAREST), MARK | 4);
  CHECK_EQ(convert(-2.5, NEAREST), MARK | 0xfffffffeu);
  CHECK_EQ(convert(2.9, ZERO), MARK | 2);
  CHECK_EQ(convert(-2.9, ZERO), MARK | 0xfffffffeu);
  CHECK_EQ(convert(2.1, UP), MARK | 3);
  CHECK_EQ(convert(-2.9, UP), MARK | 0xfffffffeu);
  CHECK_EQ(convert(2.9, DOWN), MARK | 2);
  CHECK_EQ(convert(-2.1, DOWN), MARK | 0xfffffffdu);
  CHECK_EQ(convert(3e9, NEAREST), MARK | 0x7fffffffu);   // Saturates.
  CHECK_EQ(convert(-3e9, DOWN), MARK | 0x80000000u);
  CHECK_EQ(convert(2147483647.5, ZERO), MARK | 0x7fffffffu);
  CHECK_EQ(convert(as_double(0x7ff8000000000000ull), UP), MARK | 0x80000000u);  // NaN.
  CHECK_EQ(convert(as_double(0x7ff0000000000000ull), UP), MARK | 0x7fffffffu);  // Infinity.
  CHECK_EQ(convert(0.3, ZERO), MARK);
}

static void test_floats_move_as_bits(void) {
  CHECK_EQ(double_bits(single_to_double(0x3f800000u)), 0x3ff0000000000000ull);
  CHECK_EQ(double_bits(single_to_double(0x7f800000u)), 0x7ff0000000000000ull);
  CHECK_EQ(double_bits(single_to_double(0xff800001u)), 0xfff0000020000000ull);  // Stays signaling.
  CHECK_EQ(double_bits(single_to_double(0x7fc00000u)), 0x7ff8000000000000ull);
  CHECK_EQ(double_bits(single_to_double(1)), 0x36a0000000000000ull);  // Smallest denormal.

  CHECK_EQ(double_to_single(1.0), 0x3f800000u);
  CHECK_EQ(double_to_single(as_double(0x7ff0000020000000ull)), 0x7f800001u);  // Stays signaling.
  CHECK_EQ(double_to_single(as_double(0x3800000000000000ull)), 0x00400000u);  // 2^-127, a denormal.
  CHECK_EQ(double_to_single(as_double(0x36a0000000000000ull)), 1);
  CHECK_EQ(double_to_single(-0.0), 0x80000000u);
  CHECK_EQ(double_to_single(as_double(0xfff0000020000000ull)), 0xff800001u);
  CHECK_EQ(double_to_single(as_double(0x7ff8000012345678ull)), 0x7fc00000u | (0x12345678u >> 29));

  // Any word survives a trip through a register, which is how games copy data.
  static const uint32_t patterns[] = {0xffa040ffu, 0x7f800001u, 0xffbfffffu, 0, 0x80000000u, 0x00000001u, 0x807fffffu, 0x7fffffffu, 0xdeadbeefu};
  for (unsigned i = 0; i < sizeof patterns / 4; i++) CHECK_EQ(double_to_single(single_to_double(patterns[i])), patterns[i]);

  CHECK_EQ(double_to_single_flushed(as_double(0x3800000000000000ull)), 0);
  CHECK_EQ(double_to_single_flushed(as_double(0xb800000000000000ull)), 0x80000000u);
  CHECK_EQ(double_to_single_flushed(1.0), 0x3f800000u);
  CHECK_EQ(double_to_single_flushed(as_double(0xfff0000020000000ull)), 0xff800001u);
}

static void test_fpscr(void) {
  cpu.fpscr = 0;
  fpscr_set_bit(30);  // RN, not an exception bit.
  CHECK_EQ(cpu.fpscr, 2);
  fpscr_set_bit(5);   // ZX: an exception bit, so FX too.
  CHECK_EQ(cpu.fpscr, 0x84000002u);
  cpu.fpscr &= ~FPSCR_FX;
  fpscr_set_bit(5);   // Already set, so FX stays clear.
  CHECK_EQ(cpu.fpscr, 0x04000002u);
  fpscr_clear_bit(5);
  CHECK_EQ(cpu.fpscr, 2);

  cpu.fpscr = 0;
  fpscr_set_bit(7);   // VXSNAN sets the invalid summary, VX.
  CHECK_EQ(cpu.fpscr, 0xa1000000u);
  fpscr_clear_bit(7);
  CHECK_EQ(cpu.fpscr, 0x80000000u);

  cpu.fpscr = 0;
  fpscr_write(0x04000010u);  // ZX with ZE on gives the enabled summary, FEX.
  CHECK_EQ(cpu.fpscr, 0x44000010u);

  cpu.fpscr = 0;
  fpscr_write_field(7, 3);  // RN is the low two bits of the last field.
  CHECK_EQ(cpu.fpscr & FPSCR_ROUNDING, 3);
  fpscr_write_field(0, 0);
  fpscr_write_fields(0x01, 0xffffffffu);  // Only field 7.
  CHECK_EQ(cpu.fpscr, 0x0000000fu);
  fpscr_write_fields(0x80, 0x9fffffffu);  // Only field 0. FEX and VX come from the rest.
  CHECK_EQ(cpu.fpscr, 0x9000000fu);
  fpscr_write(0xffffffffu);
  CHECK_EQ(cpu.fpscr & FPSCR_RESERVED, 0);  // Reads as zero.

  cpu.fpscr = 0x9e000000u;  // FX, OX, UX, ZX, XX.
  CHECK_EQ(fpscr_take_field(0), 9);
  CHECK_EQ(cpu.fpscr, 0x0e000000u);
  CHECK_EQ(fpscr_take_field(1), 14);
  CHECK_EQ(cpu.fpscr, 0);
  cpu.fpscr = 0x000000f3u;
  CHECK_EQ(fpscr_take_field(7), 3);  // Enable and rounding bits aren't cleared.
  CHECK_EQ(cpu.fpscr, 0x000000f3u);

  cpu.fpscr = 0x12345678u;
  CHECK_EQ(double_bits(fpscr_as_double()), 0xfff8000012345678ull);
}

static void test_xer(void) {
  xer_set(0xe000ffffu);
  CHECK_EQ(cpu.so, 1);
  CHECK_EQ(cpu.ov, 1);
  CHECK_EQ(cpu.ca, 1);
  CHECK_EQ(cpu.xer_string, 0xff7f);  // Bit 7 doesn't exist.
  CHECK_EQ(xer_get(), 0xe000ff7fu);
  xer_set(0x40000000u);
  CHECK_EQ(xer_get(), 0x40000000u);
  cpu.so = cpu.ov = 0;
  set_overflow(1);
  set_overflow(0);
  CHECK_EQ(cpu.ov, 0);
  CHECK_EQ(cpu.so, 1);  // The summary sticks.
}

static void set_gqr(int index, int store_type, int store_scale, int load_type, int load_scale) {
  cpu.spr[912 + index] = (uint32_t)store_type | (uint32_t)store_scale << 8 | (uint32_t)load_type << 16 | (uint32_t)load_scale << 24;
}

static void test_quantized(void) {
  uint32_t at = 0x80100000u;
  set_gqr(0, 0, 0, 0, 0);
  wr32(at, 0xffa040ffu);
  wr32(at + 4, 0x3f800000u);
  ps_load(at, 0, 0, 1);
  CHECK_EQ(double_bits(cpu.f[1]), double_bits(single_to_double(0xffa040ffu)));  // Not quieted.
  CHECK(cpu.p[1] == 1.0);
  ps_store(at + 8, 0, 0, 1);
  CHECK_EQ(rd32(at + 8), 0xffa040ffu);  // Copies data unchanged.
  CHECK_EQ(rd32(at + 12), 0x3f800000u);

  set_gqr(1, 0, 0, 7, 8);  // Loads s16, divided by 256.
  wr32(at, 0x0100ff00u);
  ps_load(at, 0, 1, 2);
  CHECK(cpu.f[2] == 1.0);
  CHECK(cpu.p[2] == -1.0);
  ps_load(at, 1, 1, 2);  // One value: the second reads as 1.
  CHECK(cpu.p[2] == 1.0);

  set_gqr(2, 0, 0, 4, 1);  // Loads u8, divided by 2.
  wr32(at, 0xff010000u);
  ps_load(at, 0, 2, 3);
  CHECK(cpu.f[3] == 127.5);
  CHECK(cpu.p[3] == 0.5);

  set_gqr(3, 0, 0, 6, 63);  // Loads s8. A scale of 63 is -1, which multiplies by 2.
  wr32(at, 0xff020000u);
  ps_load(at, 0, 3, 4);
  CHECK(cpu.f[4] == -2.0);
  CHECK(cpu.p[4] == 4.0);

  set_gqr(4, 4, 0, 0, 0);  // Stores u8.
  cpu.f[5] = 300.7;
  cpu.p[5] = -5.0;
  ps_store(at, 0, 4, 5);
  CHECK_EQ(rd8(at), 255);  // Clamped.
  CHECK_EQ(rd8(at + 1), 0);
  cpu.f[5] = 3.9;
  wr8(at + 1, 77);
  ps_store(at, 1, 4, 5);
  CHECK_EQ(rd8(at), 3);    // Truncated.
  CHECK_EQ(rd8(at + 1), 77);  // One value: the next byte is left alone.

  set_gqr(5, 7, 4, 0, 0);  // Stores s16, multiplied by 16.
  cpu.f[6] = -1.5;
  cpu.p[6] = 1e9;
  ps_store(at, 0, 5, 6);
  CHECK_EQ(rd16(at), 0xffe8);
  CHECK_EQ(rd16(at + 2), 0x7fff);

  set_gqr(6, 6, 0, 0, 0);  // Stores s8.
  cpu.f[7] = -1000.0;
  cpu.p[7] = 1000.0;
  ps_store(at, 0, 6, 7);
  CHECK_EQ(rd8(at), 0x80);
  CHECK_EQ(rd8(at + 1), 0x7f);

  set_gqr(7, 5, 0, 0, 0);  // Stores u16. NaN comes out as zero.
  cpu.f[8] = as_double(0x7ff8000000000000ull);
  cpu.p[8] = 70000.0;
  ps_store(at, 0, 7, 8);
  CHECK_EQ(rd16(at), 0);
  CHECK_EQ(rd16(at + 2), 0xffff);

  set_gqr(0, 0, 0, 0, 0);  // Float stores flush denormals, unlike stfs.
  cpu.f[9] = as_double(0x3800000000000000ull);
  cpu.p[9] = 1.0;
  ps_store(at, 0, 0, 9);
  CHECK_EQ(rd32(at), 0);
  CHECK_EQ(rd32(at + 4), 0x3f800000u);
}

static void test_comparisons(void) {
  CHECK_EQ(cr_float(1.0, 2.0), 8);
  CHECK_EQ(cr_float(2.0, 1.0), 4);
  CHECK_EQ(cr_float(-0.0, 0.0), 2);
  CHECK_EQ(cr_float(as_double(0x7ff8000000000000ull), 1.0), 1);
  CHECK_EQ(cr_float(1.0, as_double(0x7ff0000000000001ull)), 1);
  cpu.so = 1;
  CHECK_EQ(cr_compare(-1, 0), 9);
  CHECK_EQ(cr_compare_unsigned(0xffffffffu, 0), 5);
  cpu.so = 0;
  CHECK_EQ(cr_compare(0, 0), 2);
}

int main(void) {
  memory_start();
  test_comparisons();
  test_float_to_int();
  test_floats_move_as_bits();
  test_fpscr();
  test_xer();
  test_quantized();
  return finish();
}
