// The audio processor's accelerator, ported from Dolphin's Core/DSP/DSPAccelerator.cpp
// (GPL-2.0-or-later). See dsp_accelerator.h.
#include "dsp_accelerator.h"

#include "cpu.h"
#include "gaps.h"

#define START_END_ADDRESS_MASK 0x3fffffffu
#define CURRENT_ADDRESS_MASK 0xbfffffffu
#define MEM1_MASK 0x01ffffffu  // Dolphin's RAM mask: 24 MB rounded up to a power of two.
#define MEM2_MASK 0x03ffffffu

static uint16_t current_sample(DspAccelerator *a) {
  uint16_t value = 0;
  switch (DSP_FORMAT_SIZE(a->sample_format)) {
  case 0:  // 4-bit: the high nibble first.
    value = a->read_memory(a, a->current_address >> 1);
    if (a->current_address & 1) value &= 0xf;
    else value >>= 4;
    break;
  case 1:
    value = a->read_memory(a, a->current_address);
    break;
  case 2:
    value = (uint16_t)(a->read_memory(a, a->current_address * 2) << 8 | a->read_memory(a, a->current_address * 2 + 1));
    break;
  default:  // Gives garbage on hardware, but still moves the address.
    gap_hit("limit.dsp_accelerator_format", a->sample_format);
    break;
  }
  return value;
}

uint16_t dsp_accelerator_read_raw(DspAccelerator *a) {
  uint16_t value = current_sample(a);
  if (DSP_FORMAT_SIZE(a->sample_format) != 3) a->current_address++;
  else a->current_address = (a->current_address & ~3u) | ((a->current_address + 1) & 3u);

  // Hardware has edge cases in 4- and 8-bit modes when the address passes the end by a little
  // (a few more reads of the last word, then the reset). Like Dolphin, only reset on reading
  // the end address itself; an address already past the end ignores it.
  if (a->current_address - 1 == a->end_address) {
    a->current_address = a->start_address;  // Back to the start, as hardware does.
    if (a->raw_read_end) a->raw_read_end(a);
  }
  dsp_accelerator_set_current_address(a, a->current_address);
  return value;
}

void dsp_accelerator_write_raw(DspAccelerator *a, uint16_t value) {
  // Writes only take with the address's top bit set. They're always 16 bits, whatever the format.
  if (a->current_address & 0x80000000u) {
    a->write_memory(a, a->current_address * 2, (uint8_t)(value >> 8));
    a->write_memory(a, a->current_address * 2 + 1, (uint8_t)value);
    a->current_address++;
    if (a->raw_write_end) a->raw_write_end(a);
  } else {
    gap_hit("limit.dsp_accelerator_raw_write", a->current_address);
  }
}

static int16_t saturate16(int32_t value) {
  return value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value;
}

uint16_t dsp_accelerator_read_sample(DspAccelerator *a, const int16_t *coefs) {
  if (a->reads_stopped) return 0;
  uint16_t format = a->sample_format;
  if (DSP_FORMAT_UNKNOWN(format)) gap_hit("limit.dsp_accelerator_format", format);

  uint16_t value = 0;
  uint8_t step_size = 0;
  int16_t raw_sample;
  uint16_t decode = DSP_FORMAT_DECODE(format);
  if (decode == DSP_DECODE_MMIO_PCM_NO_INC || decode == DSP_DECODE_MMIO_PCM_INC) {
    raw_sample = (int16_t)a->input;  // The addresses can be nonsense in these modes.
  } else {
    raw_sample = (int16_t)current_sample(a);
  }

  int coef_index = (a->pred_scale >> 4) & 7;
  int32_t coef1 = coefs[coef_index * 2], coef2 = coefs[coef_index * 2 + 1];

  if (decode == DSP_DECODE_ADPCM) {
    // Only 4-bit ADPCM exists; with larger sizes hardware ignores the upper 12 bits.
    raw_sample &= 0xf;
    int32_t scale = 1 << (a->pred_scale & 0xf);
    if (raw_sample >= 8) raw_sample -= 16;
    int32_t value32 = scale * raw_sample + ((0x400 + coef1 * a->yn1 + coef2 * a->yn2) >> 11);
    value = (uint16_t)saturate16(value32);
    step_size = 2;
    a->yn2 = a->yn1;
    a->yn1 = (int16_t)value;
    a->current_address += 1;

    // These two ends are special: no exception, no new predictor and scale, and a 16-byte
    // aligned end loops to start + 1 rather than start. (Dolphin: probably different with
    // 8- or 16-bit reads.)
    if ((a->end_address & 0xf) == 0x0 && a->current_address == a->end_address) {
      a->current_address = a->start_address + 1;
    } else if ((a->end_address & 0xf) == 0x1 && a->current_address == a->end_address - 1) {
      a->current_address = a->start_address;
    } else if ((a->current_address & 15) == 0) {
      // A new frame: its header byte is the next predictor and scale.
      a->pred_scale = a->read_memory(a, (a->current_address & ~15u) >> 1);
      a->current_address += 2;
      step_size += 2;
    }
  } else {
    // PCM. Gain only applies here.
    uint8_t gain_shift = 0;
    switch (DSP_FORMAT_GAIN_SCALE(format)) {
    case 0: gain_shift = 11; break;
    case 1: gain_shift = 0; break;
    case 2: gain_shift = 16; break;
    default: gap_hit("limit.dsp_accelerator_format", format); break;
    }
    int32_t value32 = (((int32_t)a->gain * raw_sample) >> gain_shift) +
                      (((coef1 * a->yn1) >> gain_shift) + ((coef2 * a->yn2) >> gain_shift));
    value = (uint16_t)(int16_t)value32;
    a->yn2 = a->yn1;
    a->yn1 = (int16_t)value;
    step_size = 2;
    if (decode != DSP_DECODE_MMIO_PCM_NO_INC) a->current_address += 1;
  }

  // The end of the sample. The program sets YN1 and YN2 to their loop values, usually in the
  // exception, and writing YN2 lets reads go on.
  if (a->current_address == a->end_address + step_size - 1) {
    a->current_address = a->start_address;
    a->reads_stopped = 1;
    if (a->sample_read_end) a->sample_read_end(a);
  }
  dsp_accelerator_set_current_address(a, a->current_address);
  return value;
}

void dsp_accelerator_set_start_address(DspAccelerator *a, uint32_t address) { a->start_address = address & START_END_ADDRESS_MASK; }
void dsp_accelerator_set_end_address(DspAccelerator *a, uint32_t address) { a->end_address = address & START_END_ADDRESS_MASK; }
void dsp_accelerator_set_current_address(DspAccelerator *a, uint32_t address) { a->current_address = address & CURRENT_ADDRESS_MASK; }
void dsp_accelerator_set_sample_format(DspAccelerator *a, uint16_t format) { a->sample_format = format; }
void dsp_accelerator_set_gain(DspAccelerator *a, int16_t gain) { a->gain = gain; }
void dsp_accelerator_set_yn1(DspAccelerator *a, int16_t yn1) { a->yn1 = yn1; }
void dsp_accelerator_set_yn2(DspAccelerator *a, int16_t yn2) {
  a->yn2 = yn2;
  a->reads_stopped = 0;
}
void dsp_accelerator_set_pred_scale(DspAccelerator *a, uint16_t pred_scale) { a->pred_scale = pred_scale & 0x7f; }
void dsp_accelerator_set_input(DspAccelerator *a, uint16_t input) { a->input = input; }

uint32_t dsp_accelerator_wii_physical(uint32_t address, int write) {
  return write || (address & 0x10000000u) ? MEM2_START | (address & MEM2_MASK) : address & MEM1_MASK;
}

uint8_t dsp_accelerator_wii_read(DspAccelerator *a, uint32_t address) {
  (void)a;
  uint8_t *byte = host(dsp_accelerator_wii_physical(address, 0));
  if (!byte) {
    gap_hit("limit.dsp_accelerator_memory", address);
    return 0;
  }
  return *byte;
}

void dsp_accelerator_wii_write(DspAccelerator *a, uint32_t address, uint8_t value) {
  (void)a;
  uint8_t *byte = host(dsp_accelerator_wii_physical(address, 1));
  if (!byte) {
    gap_hit("limit.dsp_accelerator_memory", address);
    return;
  }
  *byte = value;
}
