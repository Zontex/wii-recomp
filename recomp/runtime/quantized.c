// Paired-single loads and stores, which scale values by the GQR registers.
#include <string.h>

#include "cpu.h"

// Written directly because this runs for every value. The exponent is always in range,
// as a scale is six bits.
static inline double power_of_two(int exponent) {
  uint64_t bits = (uint64_t)(1023 + exponent) << 52;
  double value;
  memcpy(&value, &bits, sizeof value);
  return value;
}

static double quantized_load(uint32_t address, int type, int scale, int *size) {
  double factor = power_of_two(-scale);
  switch (type) {
    case 4: *size = 1; return (double)(uint8_t)rd8(address) * factor;
    case 5: *size = 2; return (double)(uint16_t)rd16(address) * factor;
    case 6: *size = 1; return (double)(int8_t)rd8(address) * factor;
    case 7: *size = 2; return (double)(int16_t)rd16(address) * factor;
    default: *size = 4; return single_to_double(rd32(address));
  }
}

static void quantized_store(uint32_t address, int type, int scale, double value, int *size) {
  double scaled = value * power_of_two(scale);
  switch (type) {
    case 4: *size = 1; wr8(address, scaled <= 0 ? 0 : scaled >= 255 ? 255 : (uint32_t)scaled); break;
    case 5: *size = 2; wr16(address, scaled <= 0 ? 0 : scaled >= 65535 ? 65535 : (uint32_t)scaled); break;
    case 6: *size = 1; wr8(address, (uint32_t)(scaled <= -128 ? -128 : scaled >= 127 ? 127 : (int32_t)scaled)); break;
    case 7: *size = 2; wr16(address, (uint32_t)(scaled <= -32768 ? -32768 : scaled >= 32767 ? 32767 : (int32_t)scaled)); break;
    default: *size = 4; wr32(address, double_to_single_flushed(value)); break;
  }
}

static int sign_six(uint32_t value) { return value & 32 ? (int)value - 64 : (int)value; }

void ps_load(uint32_t address, int w, int gqr, int rd) {
  uint32_t q = cpu.spr[912 + gqr];
  int type = (q >> 16) & 7, scale = sign_six((q >> 24) & 63), size;
  cpu.f[rd] = quantized_load(address, type, scale, &size);
  cpu.p[rd] = w ? 1.0 : quantized_load(address + size, type, scale, &size);
}

void ps_store(uint32_t address, int w, int gqr, int rs) {
  uint32_t q = cpu.spr[912 + gqr];
  int type = q & 7, scale = sign_six((q >> 8) & 63), size;
  quantized_store(address, type, scale, cpu.f[rs], &size);
  if (!w) quantized_store(address + size, type, scale, cpu.p[rs], &size);
}
