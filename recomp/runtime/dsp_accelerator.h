// The audio processor's accelerator: it walks a sample buffer in main memory (MEM2 on a
// Wii), decodes ADPCM or PCM one sample at a time, and raises an exception when it reaches
// the buffer's end. Ported from Dolphin's Core/DSP/DSPAccelerator.cpp (GPL-2.0-or-later).
//
// It stands alone, so the interpreter (dsp_lle.c) and C versions of audio programs can share
// it. Memory and the three end exceptions are hooks, as Dolphin's virtual ReadMemory,
// WriteMemory and On*Exception are. Addresses are in the accelerator's own units, which
// depend on the sample size (nibbles, bytes or 16-bit words).
#pragma once
#include <stdint.h>

// The sample format register (FORMAT, 0xffd1).
#define DSP_FORMAT_SIZE(format) ((format) & 3u)            // 0 4-bit, 1 8-bit, 2 16-bit, 3 invalid
#define DSP_FORMAT_DECODE(format) ((format) >> 2 & 3u)     // See below.
#define DSP_FORMAT_GAIN_SCALE(format) ((format) >> 4 & 3u)  // 0 /2048, 1 /1, 2 /65536, 3 invalid
#define DSP_FORMAT_UNKNOWN(format) ((format) >> 6)
#define DSP_DECODE_ADPCM 0u            // ADPCM from memory, the address moves on.
#define DSP_DECODE_MMIO_PCM_NO_INC 1u  // PCM from ACIN, the address stays.
#define DSP_DECODE_PCM 2u              // PCM from memory, the address moves on.
#define DSP_DECODE_MMIO_PCM_INC 3u     // PCM from ACIN, the address moves on.

typedef struct DspAccelerator DspAccelerator;
struct DspAccelerator {
  uint32_t start_address, end_address, current_address;
  uint16_t sample_format;
  int16_t gain, yn1, yn2;
  uint16_t pred_scale, input;
  // After the end exception the accelerator reads back nothing and stops moving until YN2 is
  // written. Hardware keeps this internally; no register shows it.
  int reads_stopped;

  // Hooks. The memory ones are required; an exception hook left 0 does nothing.
  uint8_t (*read_memory)(DspAccelerator *accelerator, uint32_t address);
  void (*write_memory)(DspAccelerator *accelerator, uint32_t address, uint8_t value);
  void (*raw_read_end)(DspAccelerator *accelerator);     // Exception 3, ACCOV on raw reads.
  void (*raw_write_end)(DspAccelerator *accelerator);    // Exception 4, after every raw write.
  void (*sample_read_end)(DspAccelerator *accelerator);  // Exception 5, end of the sample.
  void *user;
};

// ACDSAMP (0xffdd): the next decoded sample. coefs are the 16 ADPCM coefficients, which the
// interpreter keeps in its registers 0xffa0-0xffaf.
uint16_t dsp_accelerator_read_sample(DspAccelerator *accelerator, const int16_t *coefs);
// ACDRAW (0xffd3): raw reads and writes. Zelda's program reads main memory this way.
uint16_t dsp_accelerator_read_raw(DspAccelerator *accelerator);
void dsp_accelerator_write_raw(DspAccelerator *accelerator, uint16_t value);

// The registers. Getters are the fields; these setters mask as the hardware does.
void dsp_accelerator_set_start_address(DspAccelerator *accelerator, uint32_t address);
void dsp_accelerator_set_end_address(DspAccelerator *accelerator, uint32_t address);
void dsp_accelerator_set_current_address(DspAccelerator *accelerator, uint32_t address);
void dsp_accelerator_set_sample_format(DspAccelerator *accelerator, uint16_t format);
void dsp_accelerator_set_gain(DspAccelerator *accelerator, int16_t gain);
void dsp_accelerator_set_yn1(DspAccelerator *accelerator, int16_t yn1);
void dsp_accelerator_set_yn2(DspAccelerator *accelerator, int16_t yn2);  // Also restarts reads.
void dsp_accelerator_set_pred_scale(DspAccelerator *accelerator, uint16_t pred_scale);
void dsp_accelerator_set_input(DspAccelerator *accelerator, uint16_t input);

// Wii memory as Dolphin's DSP.cpp (ReadARAM, WriteARAM) routes it. Reads with bit 0x10000000
// set come from MEM2, others from MEM1. Writes always go to MEM2, as Dolphin's do (it notes
// this isn't checked on a Wii). Bytes outside memory read as 0 and are reported.
// The physical address a byte comes from (or goes to, with write set), as host() takes it.
uint32_t dsp_accelerator_wii_physical(uint32_t address, int write);
uint8_t dsp_accelerator_wii_read(DspAccelerator *accelerator, uint32_t address);
void dsp_accelerator_wii_write(DspAccelerator *accelerator, uint32_t address, uint8_t value);
