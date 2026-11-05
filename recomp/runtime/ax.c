// The AX audio programs, in C: the GameCube's and the Wii's, which most games use. The
// game fills a parameter block for each voice and writes a command list; the program reads
// them, decodes samples through the accelerator, converts their rate with the filter table
// in the processor's ROM, mixes buses and writes the result to the game's memory, where the
// audio DMA plays it. Ported from Dolphin's HW/DSPHLE/UCodes/AX.cpp, AXWii.cpp, AX.h,
// AXWii.h and AXStructs.h (GPL-2.0-or-later), and checked against the interpreter running
// the real program on sound recordings (recomp/tools/sound_bench.py).
//
// The versions differ by checksum: the GameCube's 5 ms frames and command set (with one
// older layout and mixer), the Wii's 3 ms frames, its older command numbering, and its
// three parameter block layouts.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "debug.h"
#include "dsp.h"
#include "dsp_accelerator.h"
#include "gaps.h"

#define HILO(name) ((uint32_t)(name##_hi) << 16 | (name##_lo))

// --- The parameter blocks, as AXStructs.h lays them out, every field 16 bits. ---

typedef struct { uint16_t volume, volume_delta; } AxVolume;
typedef struct { uint16_t on, addr_mem_high, addr_mem_low, offset_left, offset_right, target_left, target_right; } AxTimeDelay;
typedef struct { uint16_t pb_offset, new_value; } AxUpdate;
#define AX_UPDATES 32
typedef struct { AxUpdate entry[AX_UPDATES]; } AxUpdates;
typedef struct { int16_t cur_volume, cur_volume_delta; } AxEnvelope;
typedef struct { uint16_t looping, sample_format, loop_addr_hi, loop_addr_lo, end_addr_hi, end_addr_lo, cur_addr_hi, cur_addr_lo; } AxAddress;
typedef struct { int16_t coefs[16]; uint16_t gain, pred_scale; int16_t yn1, yn2; } AxAdpcm;
typedef struct { uint16_t ratio_hi, ratio_lo, cur_addr_frac; int16_t last_samples[4]; } AxConverter;
typedef struct { uint16_t cur_addr_frac; int16_t last_samples[4]; } AxRemoteConverter;
typedef struct { uint16_t pred_scale, yn1, yn2; } AxLoop;
typedef struct { uint16_t on; int16_t yn1; uint16_t a0; int16_t b0; } AxLowPass;
typedef struct { uint16_t on, unknown[3]; } AxHighPass;
typedef struct { uint16_t on; int16_t xn1, xn2, yn1, yn2, b0, b1, b2, a1, a2; } AxBiquad;

typedef struct {
  uint16_t next_pb_hi, next_pb_lo, this_pb_hi, this_pb_lo;
  uint16_t src_type, coef_select, mixer_control, running, is_stream;
  struct { AxVolume main_left, main_right, aux_a_left, aux_a_right, aux_b_left, aux_b_right, aux_b_surround, main_surround, aux_a_surround; } mixer;
  AxTimeDelay initial_time_delay;
  struct { uint16_t num_updates[5], data_hi, data_lo; } updates;
  struct { int16_t main_left, aux_a_left, aux_b_left, main_right, aux_a_right, aux_b_right, main_surround, aux_a_surround, aux_b_surround; } dpop;
  AxEnvelope vol_env;
  uint16_t unknown3[3];
  AxAddress audio_addr;
  AxAdpcm adpcm;
  AxConverter src;
  AxLoop adpcm_loop_info;
  AxLowPass lpf;  // Missing from one version's layout.
  uint16_t loop_counter;
  uint16_t padding[24];
} AxPbGc;

typedef struct {
  uint16_t next_pb_hi, next_pb_lo, this_pb_hi, this_pb_lo;
  uint16_t src_type, coef_select, mixer_control_hi, mixer_control_lo, running, is_stream;
  struct {
    AxVolume main_left, main_right, aux_a_left, aux_a_right, aux_b_left, aux_b_right, aux_c_left, aux_c_right;
    AxVolume main_surround, aux_a_surround, aux_b_surround, aux_c_surround;
  } mixer;
  AxTimeDelay initial_time_delay;
  struct {
    int16_t main_left, aux_a_left, aux_b_left, aux_c_left, main_right, aux_a_right, aux_b_right, aux_c_right;
    int16_t main_surround, aux_a_surround, aux_b_surround, aux_c_surround;
  } dpop;
  struct { uint16_t num_updates[3], data_hi, data_lo; } updates;  // Only in the oldest layouts.
  AxEnvelope vol_env;
  AxAddress audio_addr;
  AxAdpcm adpcm;
  AxConverter src;
  AxLoop adpcm_loop_info;
  AxLowPass lpf;
  union { AxHighPass hpf; AxBiquad biquad; };
  uint16_t remote, remote_mixer_control;
  struct { AxVolume main0, aux0, main1, aux1, main2, aux2, main3, aux3; } remote_mixer;
  struct { int16_t main0, main1, main2, main3, aux0, aux1, aux2, aux3; } remote_dpop;
  AxRemoteConverter remote_src;
  union { uint16_t on; AxLowPass lpf; AxBiquad biquad; } remote_iir;
  uint16_t pad[2];
} AxPbWii;

_Static_assert(sizeof(AxPbGc) == 244 && sizeof(AxPbWii) == 310, "the blocks as Dolphin lays them out");

// What a block's mixer control asks for, the same bits whatever the version (AX.h).
enum {
  MIX_MAIN_L = 0x000001, MIX_MAIN_L_RAMP = 0x000002, MIX_MAIN_R = 0x000004, MIX_MAIN_R_RAMP = 0x000008,
  MIX_MAIN_S = 0x000010, MIX_MAIN_S_RAMP = 0x000020, MIX_AUXA_L = 0x000040, MIX_AUXA_L_RAMP = 0x000080,
  MIX_AUXA_R = 0x000100, MIX_AUXA_R_RAMP = 0x000200, MIX_AUXA_S = 0x000400, MIX_AUXA_S_RAMP = 0x000800,
  MIX_AUXB_L = 0x001000, MIX_AUXB_L_RAMP = 0x002000, MIX_AUXB_R = 0x004000, MIX_AUXB_R_RAMP = 0x008000,
  MIX_AUXB_S = 0x010000, MIX_AUXB_S_RAMP = 0x020000, MIX_AUXC_L = 0x040000, MIX_AUXC_L_RAMP = 0x080000,
  MIX_AUXC_R = 0x100000, MIX_AUXC_R_RAMP = 0x200000, MIX_AUXC_S = 0x400000, MIX_AUXC_S_RAMP = 0x800000,
  MIX_ALL_RAMPS = 0xaaaaaa,
};
enum { SRC_POLYPHASE = 0, SRC_LINEAR = 1, SRC_NEAREST = 2 };

// --- What both versions share. ---

static inline int16_t clamp16(int64_t sample) { return sample < -0x8000 ? -0x8000 : sample > 0x7fff ? 0x7fff : (int16_t)sample; }

typedef int16_t (*SampleSource)(void *from, uint32_t index);

static int16_t ax_array_sample(void *from, uint32_t index) { return ((const int16_t *)from)[index]; }

// Makes count samples at the block's rate from the source's, with the ROM's four-tap filters,
// or straight lines, or neither. Positions are 16.16; the last four inputs carry over.
static uint32_t ax_resample(SampleSource source, void *from, int16_t *output, uint32_t count, int16_t *last_samples,
                            uint32_t position, uint32_t ratio, int type, const int16_t *coefficients) {
  uint32_t read = 0;
  if (coefficients && type == SRC_POLYPHASE) {
    int16_t temp[4];
    uint32_t index = 0;
    for (int i = 0; i < 4; i++) temp[index++ & 3] = last_samples[i];
    for (uint32_t i = 0; i < count; i++) {
      position += ratio;
      for (; position >= 0x10000; position -= 0x10000) temp[index++ & 3] = source(from, read++);
      const int16_t *c = &coefficients[((position & 0xffff) >> 9) << 2];
      int64_t t0 = temp[index++ & 3], t1 = temp[index++ & 3], t2 = temp[index++ & 3], t3 = temp[index++ & 3];
      output[i] = clamp16((t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15);
    }
    for (int i = 3; i >= 0; i--) last_samples[i] = temp[--index & 3];
  } else if (type == SRC_LINEAR || type == SRC_POLYPHASE) {
    int16_t temp[4];
    uint32_t index = 0;
    for (int i = 0; i < 4; i++) temp[index++ & 3] = last_samples[i];
    for (uint32_t i = 0; i < count; i++) {
      position += ratio;
      for (; position >= 0x10000; position -= 0x10000) temp[index++ & 3] = source(from, read++);
      uint16_t fraction = (uint16_t)position, inverse = (uint16_t)-fraction;
      if (fraction) {
        int32_t s0 = temp[index++ & 3], s1 = temp[index++ & 3];
        output[i] = (int16_t)((s0 * inverse + s1 * fraction) >> 16);
        index += 2;
      } else {
        output[i] = temp[index++ & 3];
        index += 3;
      }
    }
    for (int i = 3; i >= 0; i--) last_samples[i] = temp[--index & 3];
  } else {
    for (uint32_t i = 0; i < count; i++) output[i] = source(from, i);
    memcpy(last_samples, output + count - 4, 4 * sizeof(int16_t));
  }
  return position;
}

static void ax_mix_add(int32_t *out, const int16_t *input, uint32_t count, AxVolume *volume, int16_t *depop, int ramp) {
  uint16_t delta = ramp ? volume->volume_delta : 0;
  for (uint32_t i = 0; i < count; i++) {
    int16_t sample = clamp16((int32_t)(((int64_t)input[i] * volume->volume) >> 15));
    out[i] += sample;
    volume->volume += delta;
    *depop = sample;  // What the voice last added: faded out by the game if it stops.
  }
}

static void ax_low_pass(int16_t *samples, uint32_t count, AxLowPass *f) {
  for (uint32_t i = 0; i < count; i++) f->yn1 = samples[i] = clamp16((f->a0 * (int32_t)samples[i] + f->b0 * (int32_t)f->yn1) >> 15);
}

static void ax_biquad(int16_t *samples, uint32_t count, AxBiquad *f) {
  for (uint32_t i = 0; i < count; i++) {
    int16_t xn0 = samples[i];
    int64_t sum = (int64_t)f->b0 * xn0 + (int64_t)f->b1 * f->xn1 + (int64_t)f->b2 * f->xn2 + (int64_t)f->a1 * f->yn1 + (int64_t)f->a2 * f->yn2;
    sum <<= 2;
    sum += sum & 0x10000 ? 0x8000 : 0x7fff;  // The processor's round to nearest even.
    int16_t yn0 = clamp16(sum >> 16);
    f->xn2 = f->xn1;
    f->yn2 = f->yn1;
    f->xn1 = xn0;
    f->yn1 = yn0;
    samples[i] = yn0;
  }
}

#define AX_GC
#include "ax_voice.h"
#undef AX_GC
#define AX_WII
#include "ax_voice.h"
#undef AX_WII

// --- The program's state: its buses, the command list, and what it remembers between frames. ---

extern const unsigned char data_dsp_coef[];  // The ROM's filter table, embedded by build_file.py.
static int16_t coefficients[0x800];

static struct {
  uint32_t crc;
  int wii, old_wii, new_filter;
  int32_t main_left[160], main_right[160], main_surround[160];
  int32_t aux_a_left[160], aux_a_right[160], aux_a_surround[160];
  int32_t aux_b_left[160], aux_b_right[160], aux_b_surround[160];
  int32_t aux_c_left[96], aux_c_right[96], aux_c_surround[96];
  int32_t remote_main[4][18], remote_aux[4][18];
  uint16_t list[512];
  uint16_t list_size;
  uint16_t compressor_position;
  uint16_t last_main_volume, last_aux_volumes[3];
  DspAccelerator accelerator;
} ax;

static void load_coefficients(void) {
  for (int i = 0; i < 0x800; i++) coefficients[i] = (int16_t)(data_dsp_coef[2 * i] << 8 | data_dsp_coef[2 * i + 1]);
}

static void copy_list(uint32_t address, uint16_t size) {
  if (size >= 512) {
    gap_hit("limit.ax_command_list", size);
    return;
  }
  for (uint32_t i = 0; i < size; i++) ax.list[i] = (uint16_t)rd16(address + i * 2);
}

static void read_words(uint16_t *to, uint32_t address, uint32_t bytes) {
  for (uint32_t i = 0; i < bytes / 2; i++) to[i] = (uint16_t)rd16(address + i * 2);
}

static void write_words(uint32_t address, const uint16_t *from, uint32_t bytes) {
  for (uint32_t i = 0; i < bytes / 2; i++) wr16(address + i * 2, from[i]);
}

static void upload(uint32_t address, const int32_t *from, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) wr32(address + i * 4, (uint32_t)from[i]);
}

static int32_t download(uint32_t address, uint32_t index) { return (int32_t)rd32(address + index * 4); }

typedef struct { int32_t *buffer; int samples_per_ms; } BufferDescription;

// Each buffer starts the frame from a value and a step, the leftovers of voices that stopped.
static void init_mixing_buffers(uint32_t address, int ms, const BufferDescription *buffers, int count) {
  for (int i = 0; i < count; i++) {
    int32_t value = (int32_t)(rd16(address + i * 6) << 16 | rd16(address + i * 6 + 2));
    int16_t delta = (int16_t)rd16(address + i * 6 + 4);
    int samples = ms * buffers[i].samples_per_ms;
    for (int j = 0; j < samples; j++) buffers[i].buffer[j] = value ? value + j * delta : 0;
  }
}

static void run_compressor(uint16_t threshold, uint16_t release_frames, uint32_t table, uint32_t ms) {
  int triggered = 0;
  for (uint32_t i = 0; i < 32 * ms && !triggered; i++) {
    triggered = abs(ax.main_left[i]) > (int)threshold || abs(ax.main_right[i]) > (int)threshold;
  }
  uint32_t frame_bytes = 32 * ms * 2, offset;
  if (triggered) {
    offset = ax.compressor_position * frame_bytes;  // One attack frame, from the frame before.
    ax.compressor_position = release_frames;
  } else if (ax.compressor_position) {
    --ax.compressor_position;
    offset = (11 + ax.compressor_position) * frame_bytes;  // The releases follow the 11 attacks.
  } else {
    return;
  }
  for (uint32_t i = 0; i < 32 * ms; i++) {
    uint16_t coefficient = (uint16_t)rd16(table + offset + i * 2);
    ax.main_left[i] = (int32_t)(((int64_t)ax.main_left[i] * coefficient) >> 15);
    ax.main_right[i] = (int32_t)(((int64_t)ax.main_right[i] * coefficient) >> 15);
  }
}

// --- The GameCube version (AX.cpp): 5 ms frames. ---

// One version has no low-pass filter in its blocks, and an older mixer control.
static int gc_has_low_pass(void) { return ax.crc != 0x4e8a8b21u; }

static uint32_t gc_mixer_control(uint32_t control) {
  uint32_t mix = 0;
  if (ax.crc == 0x4e8a8b21u) {
    mix |= MIX_MAIN_L | MIX_MAIN_R;
    if (control & 0x10) {  // Dolby Pro Logic II.
      if ((control & 6) == 0) mix |= MIX_AUXB_L | MIX_AUXB_R;
      if ((control & 7) == 1) mix |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_S;
    } else {
      if (control & 1) mix |= MIX_AUXA_L | MIX_AUXA_R;
      if (control & 2) mix |= MIX_AUXB_L | MIX_AUXB_R;
      if (control & 4) {
        mix |= MIX_MAIN_S;
        if (mix & MIX_AUXA_L) mix |= MIX_AUXA_S;
        if (mix & MIX_AUXB_L) mix |= MIX_AUXB_S;
      }
    }
    if (control & 8) mix |= MIX_ALL_RAMPS;
    return mix;
  }
  if (control & 0x0001) mix |= MIX_MAIN_L;
  if (control & 0x0002) mix |= MIX_MAIN_R;
  if (control & 0x0004) mix |= MIX_MAIN_S;
  if (control & 0x0008) mix |= MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP | MIX_MAIN_S_RAMP;
  if (control & 0x0010) mix |= MIX_AUXA_L;
  if (control & 0x0020) mix |= MIX_AUXA_R;
  if (control & 0x0040) mix |= MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
  if (control & 0x0080) mix |= MIX_AUXA_S;
  if (control & 0x0100) mix |= MIX_AUXA_S_RAMP;
  if (control & 0x0200) mix |= MIX_AUXB_L;
  if (control & 0x0400) mix |= MIX_AUXB_R;
  if (control & 0x0800) mix |= MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
  if (control & 0x1000) mix |= MIX_AUXB_S;
  if (control & 0x2000) mix |= MIX_AUXB_S_RAMP;
  if (control & 0x4000) gap_hit("audio.ax_gamecube_dolby_aux_b", control);  // Dolphin leaves this out too.
  return mix;
}

static void gc_read_pb(uint32_t address, AxPbGc *pb) {
  if (gc_has_low_pass()) {
    read_words((uint16_t *)pb, address, sizeof *pb);
    return;
  }
  size_t lpf = offsetof(AxPbGc, lpf), counter = offsetof(AxPbGc, loop_counter);
  read_words((uint16_t *)pb, address, (uint32_t)lpf);
  memset((char *)pb + lpf, 0, counter - lpf);
  read_words((uint16_t *)((char *)pb + counter), address + (uint32_t)lpf, (uint32_t)(sizeof *pb - counter));
}

static void gc_write_pb(uint32_t address, const AxPbGc *pb) {
  if (gc_has_low_pass()) {
    write_words(address, (const uint16_t *)pb, sizeof *pb);
    return;
  }
  size_t lpf = offsetof(AxPbGc, lpf), counter = offsetof(AxPbGc, loop_counter);
  write_words(address, (const uint16_t *)pb, (uint32_t)lpf);
  write_words(address + (uint32_t)lpf, (const uint16_t *)((const char *)pb + counter), (uint32_t)(sizeof *pb - counter));
}

static void gc_process_pb_list(uint32_t address) {
  AxPbGc pb;
  while (address) {
    AxBuffers_gc buffers = {{ax.main_left, ax.main_right, ax.main_surround, ax.aux_a_left, ax.aux_a_right, ax.aux_a_surround,
                             ax.aux_b_left, ax.aux_b_right, ax.aux_b_surround}};
    gc_read_pb(address, &pb);
    AxUpdates updates = load_updates_gc(&pb);
    for (int ms = 0; ms < 5; ms++) {
      apply_updates_gc(ms, &pb, pb.updates.num_updates, &updates);
      process_voice_gc(&ax.accelerator, &pb, &buffers, 32, gc_mixer_control(pb.mixer_control), coefficients, 0);
      for (int i = 0; i < 9; i++) buffers.all[i] += 32;
    }
    gc_write_pb(address, &pb);
    address = HILO(pb.next_pb);
  }
}

static void gc_mix_aux(int aux, uint32_t write_address, uint32_t read_address) {
  int32_t *buffers[3] = {aux ? ax.aux_b_left : ax.aux_a_left, aux ? ax.aux_b_right : ax.aux_a_right,
                         aux ? ax.aux_b_surround : ax.aux_a_surround};
  if (write_address) {
    for (int i = 0; i < 3; i++, write_address += 160 * 4) upload(write_address, buffers[i], 160);
  }
  int32_t *mains[3] = {ax.main_left, ax.main_right, ax.main_surround};
  for (int i = 0; i < 3; i++) {
    for (uint32_t k = 0; k < 160; k++) mains[i][k] += download(read_address, i * 160 + k);
  }
}

static void gc_output(uint32_t lr_address, uint32_t surround_address) {
  upload(surround_address, ax.main_surround, 160);
  for (uint32_t i = 0; i < 160; i++) {  // Right, then left.
    wr16(lr_address + i * 4, (uint16_t)clamp16(ax.main_right[i]));
    wr16(lr_address + i * 4 + 2, (uint16_t)clamp16(ax.main_left[i]));
  }
}

static void gc_run_list(void) {
  uint32_t pb_address = 0;
  for (uint32_t at = 0;;) {
    if (at >= 512 - 16) {
      gap_hit("limit.ax_command_list", at);
      return;
    }
    uint16_t command = ax.list[at++];
#define ARG32() (at += 2, (uint32_t)ax.list[at - 2] << 16 | ax.list[at - 1])
    switch (command) {
    case 0x00: {  // Set up the buses.
      BufferDescription buffers[9] = {{ax.main_left, 32}, {ax.main_right, 32}, {ax.main_surround, 32},
                                      {ax.aux_a_left, 32}, {ax.aux_a_right, 32}, {ax.aux_a_surround, 32},
                                      {ax.aux_b_left, 32}, {ax.aux_b_right, 32}, {ax.aux_b_surround, 32}};
      init_mixing_buffers(ARG32(), 5, buffers, 9);
      break;
    }
    case 0x01: {  // Download three buses and mix them in, each at a volume.
      uint32_t address = ARG32();
      uint16_t volumes[3] = {ax.list[at], ax.list[at + 1], ax.list[at + 2]};
      at += 3;
      int32_t *buses[3][3] = {{ax.main_left, ax.main_right, ax.main_surround},
                              {ax.aux_a_left, ax.aux_a_right, ax.aux_a_surround},
                              {ax.aux_b_left, ax.aux_b_right, ax.aux_b_surround}};
      for (int i = 0; i < 3; i++) {  // Each from the same address, as Dolphin reads it.
        for (int j = 0; j < 3; j++) {
          for (uint32_t k = 0; k < 160; k++) buses[i][j][k] += (int32_t)(((int64_t)download(address, j * 160 + k) * volumes[i]) >> 15);
        }
      }
      break;
    }
    case 0x02: pb_address = ARG32(); break;
    case 0x03: gc_process_pb_list(pb_address); break;
    case 0x04: case 0x05: {
      uint32_t write_address = ARG32(), read_address = ARG32();
      gc_mix_aux(command - 0x04, write_address, read_address);
      break;
    }
    case 0x06: {  // Upload the main buses.
      uint32_t address = ARG32();
      upload(address, ax.main_left, 160);
      upload(address + 640, ax.main_right, 160);
      upload(address + 1280, ax.main_surround, 160);
      break;
    }
    case 0x07: case 0x11: {  // Set left and right from one buffer, the same or opposite.
      uint32_t address = ARG32();
      for (uint32_t i = 0; i < 160; i++) {
        int32_t sample = download(address, i);
        ax.main_left[i] = command == 0x07 ? sample : -sample;
        ax.main_right[i] = sample;
        ax.main_surround[i] = 0;
      }
      break;
    }
    case 0x08:
      gap_hit("audio.ax_gamecube_command_08", command);  // Unknown to Dolphin as well.
      at += 10;
      break;
    case 0x09: gc_mix_aux(1, 0, ARG32()); break;
    case 0x0a: case 0x0b: case 0x0c: break;  // Nothing, in every version.
    case 0x0d: {  // The list goes on elsewhere.
      uint32_t address = ARG32();
      uint16_t size = ax.list[at];
      copy_list(address, size);
      at = 0;
      break;
    }
    case 0x0e: {
      uint32_t surround = ARG32(), lr = ARG32();
      gc_output(lr, surround);
      break;
    }
    case 0x0f: return;
    case 0x10: {  // Upload aux B's left and right, and mix them back from the game's copy.
      uint32_t up = ARG32(), down = ARG32();
      upload(up, ax.aux_b_left, 160);
      upload(up + 640, ax.aux_b_right, 160);
      for (uint32_t i = 0; i < 160; i++) {
        ax.aux_b_left[i] = download(down, i);
        ax.main_left[i] += ax.aux_b_left[i];
      }
      for (uint32_t i = 0; i < 160; i++) {
        ax.aux_b_right[i] = download(down, 160 + i);
        ax.main_right[i] += ax.aux_b_right[i];
      }
      break;
    }
    case 0x12: {
      if (ax.crc == 0x4e8a8b21u) gap_hit("audio.ax_command_not_in_version", command);
      uint16_t threshold = ax.list[at], frames = ax.list[at + 1];
      at += 2;
      run_compressor(threshold, frames, ARG32(), 5);
      break;
    }
    case 0x13: {  // Upload aux A and aux B's surround, then mix in four buffers from the game.
      uint32_t aux_a = ARG32(), aux_b_surround = ARG32();
      uint32_t from[4];
      for (int i = 0; i < 4; i++) from[i] = ARG32();
      upload(aux_a, ax.aux_a_left, 160);
      upload(aux_a + 640, ax.aux_a_right, 160);
      upload(aux_a + 1280, ax.aux_a_surround, 160);
      upload(aux_b_surround, ax.aux_b_surround, 160);
      int32_t *into[4] = {ax.main_left, ax.main_right, ax.aux_b_left, ax.aux_b_right};
      for (int i = 0; i < 4; i++) {
        for (uint32_t j = 0; j < 160; j++) into[i][j] += download(from[i], j);
      }
      break;
    }
    default:
      gap_note("unlisted.audio.ax_command", command);
      return;
    }
#undef ARG32
  }
}

// --- The Wii version (AXWii.cpp): 3 ms frames, aux C, Wii Remote speakers. ---

static uint32_t wii_mixer_control(uint32_t control) {
  uint32_t mix = 0;
  static const struct { uint32_t bit, mix; } bits[] = {
    {0x00000001, MIX_MAIN_L}, {0x00000002, MIX_MAIN_R},
    {0x00000004, MIX_MAIN_L | MIX_MAIN_R | MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP},
    {0x00000008, MIX_MAIN_S}, {0x00000010, MIX_MAIN_S | MIX_MAIN_S_RAMP},
    {0x00010000, MIX_AUXA_L}, {0x00020000, MIX_AUXA_R},
    {0x00040000, MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP},
    {0x00080000, MIX_AUXA_S}, {0x00100000, MIX_AUXA_S | MIX_AUXA_S_RAMP},
    {0x00200000, MIX_AUXB_L}, {0x00400000, MIX_AUXB_R},
    {0x00800000, MIX_AUXB_L | MIX_AUXB_R | MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP},
    {0x01000000, MIX_AUXB_S}, {0x02000000, MIX_AUXB_S | MIX_AUXB_S_RAMP},
    {0x04000000, MIX_AUXC_L}, {0x08000000, MIX_AUXC_R},
    {0x10000000, MIX_AUXC_L | MIX_AUXC_R | MIX_AUXC_L_RAMP | MIX_AUXC_R_RAMP},
    {0x20000000, MIX_AUXC_S}, {0x40000000, MIX_AUXC_S | MIX_AUXC_S_RAMP},
  };
  for (size_t i = 0; i < sizeof bits / sizeof bits[0]; i++) {
    if (control & bits[i].bit) mix |= bits[i].mix;
  }
  return mix;
}

// The three layouts in memory: the oldest has the update counts but a high-pass filter
// shorter than the biquad; the middle has neither updates nor the biquad; the newest has
// the biquad but no updates. Everything is read into the largest, as Dolphin does.
enum { LAYOUT_OLDEST, LAYOUT_MIDDLE, LAYOUT_NEWEST };
static int wii_layout(void) {
  switch (ax.crc) {
  case 0x7699af32u: case 0xfa450138u: return LAYOUT_OLDEST;
  case 0xd9c4bf34u: case 0xadbc06bdu: return LAYOUT_MIDDLE;
  default: return LAYOUT_NEWEST;  // 0x347112ba, 0x4cc52064: dsp.c sends no others here.
  }
}

#define UPDATES_BEGIN offsetof(AxPbWii, updates)
#define UPDATES_END (offsetof(AxPbWii, updates) + sizeof(((AxPbWii *)0)->updates))
#define GAP_BEGIN (offsetof(AxPbWii, hpf) + sizeof(AxHighPass))
#define GAP_END (offsetof(AxPbWii, biquad) + sizeof(AxBiquad))

static void wii_read_pb(uint32_t address, AxPbWii *pb) {
  char *to = (char *)pb;
  switch (wii_layout()) {
  case LAYOUT_OLDEST:
    read_words((uint16_t *)to, address, GAP_BEGIN);
    memset(to + GAP_BEGIN, 0, GAP_END - GAP_BEGIN);
    read_words((uint16_t *)(to + GAP_END), address + GAP_BEGIN, sizeof *pb - GAP_END);
    break;
  case LAYOUT_MIDDLE:
    read_words((uint16_t *)to, address, UPDATES_BEGIN);
    memset(to + UPDATES_BEGIN, 0, UPDATES_END - UPDATES_BEGIN);
    read_words((uint16_t *)(to + UPDATES_END), address + UPDATES_BEGIN, GAP_BEGIN - UPDATES_END);
    memset(to + GAP_BEGIN, 0, GAP_END - GAP_BEGIN);
    read_words((uint16_t *)(to + GAP_END), address + UPDATES_BEGIN + (GAP_BEGIN - UPDATES_END), sizeof *pb - GAP_END);
    break;
  default:
    read_words((uint16_t *)to, address, UPDATES_BEGIN);
    memset(to + UPDATES_BEGIN, 0, UPDATES_END - UPDATES_BEGIN);
    read_words((uint16_t *)(to + UPDATES_END), address + UPDATES_BEGIN, sizeof *pb - UPDATES_END);
    break;
  }
}

static void wii_write_pb(uint32_t address, const AxPbWii *pb) {
  const char *from = (const char *)pb;
  switch (wii_layout()) {
  case LAYOUT_OLDEST:
    write_words(address, (const uint16_t *)from, GAP_BEGIN);
    write_words(address + GAP_BEGIN, (const uint16_t *)(from + GAP_END), sizeof *pb - GAP_END);
    break;
  case LAYOUT_MIDDLE:
    write_words(address, (const uint16_t *)from, UPDATES_BEGIN);
    write_words(address + UPDATES_BEGIN, (const uint16_t *)(from + UPDATES_END), GAP_BEGIN - UPDATES_END);
    write_words(address + UPDATES_BEGIN + (GAP_BEGIN - UPDATES_END), (const uint16_t *)(from + GAP_END), sizeof *pb - GAP_END);
    break;
  default:
    write_words(address, (const uint16_t *)from, UPDATES_BEGIN);
    write_words(address + UPDATES_BEGIN, (const uint16_t *)(from + UPDATES_END), sizeof *pb - UPDATES_END);
    break;
  }
}

static void wii_process_pb_list(uint32_t address) {
  AxPbWii pb;
  while (address) {
    AxBuffers_wii buffers = {{ax.main_left, ax.main_right, ax.main_surround, ax.aux_a_left, ax.aux_a_right, ax.aux_a_surround,
                              ax.aux_b_left, ax.aux_b_right, ax.aux_b_surround, ax.aux_c_left, ax.aux_c_right, ax.aux_c_surround,
                              ax.remote_main[0], ax.remote_aux[0], ax.remote_main[1], ax.remote_aux[1],
                              ax.remote_main[2], ax.remote_aux[2], ax.remote_main[3], ax.remote_aux[3]}};
    wii_read_pb(address, &pb);
    // The middle layout's high-pass filter, which Dolphin doesn't run either.
    if (wii_layout() == LAYOUT_MIDDLE && pb.running == 1 && pb.hpf.on) gap_hit("audio.axwii_high_pass_filter", pb.hpf.on);
    uint32_t mix = wii_mixer_control(HILO(pb.mixer_control));
    if (ax.old_wii && (pb.updates.num_updates[0] | pb.updates.num_updates[1] | pb.updates.num_updates[2])) {
      AxUpdates updates = load_updates_wii(&pb);
      for (int ms = 0; ms < 3; ms++) {
        apply_updates_wii(ms, &pb, pb.updates.num_updates, &updates);
        process_voice_wii(&ax.accelerator, &pb, &buffers, 32, wii_mixer_control(HILO(pb.mixer_control)), coefficients,
                          ax.new_filter);
        for (int i = 0; i < 12; i++) buffers.regular[i] += 32;
        for (int i = 0; i < 8; i++) buffers.remotes[i] += 6;
      }
    } else {
      process_voice_wii(&ax.accelerator, &pb, &buffers, 96, mix, coefficients, ax.new_filter);
    }
    wii_write_pb(address, &pb);
    address = HILO(pb.next_pb);
  }
}

// Dolphin's volume ramp, in floating point, which Dolphin notes isn't quite the program's.
static void volume_ramp(uint16_t *output, uint16_t from, uint16_t to, size_t count) {
  float current = from;
  for (size_t i = 0; i < count; i++) {
    current += (to - from) / (float)count;
    output[i] = (uint16_t)current;
  }
}

static void wii_mix_aux(int aux, uint32_t write_address, uint32_t read_address, uint16_t volume) {
  uint16_t ramp[96];
  volume_ramp(ramp, ax.last_aux_volumes[aux], volume, 96);
  ax.last_aux_volumes[aux] = volume;
  int32_t *buses[3][3] = {{ax.aux_a_left, ax.aux_a_right, ax.aux_a_surround},
                          {ax.aux_b_left, ax.aux_b_right, ax.aux_b_surround},
                          {ax.aux_c_left, ax.aux_c_right, ax.aux_c_surround}};
  if (write_address) {
    for (int i = 0; i < 3; i++, write_address += 96 * 4) upload(write_address, buses[aux][i], 96);
  }
  int32_t *mains[3] = {ax.main_left, ax.main_right, ax.main_surround};
  for (int i = 0; i < 3; i++) {
    for (uint32_t j = 0; j < 96; j++) mains[i][j] += (int32_t)(((int64_t)download(read_address, i * 96 + j) * ramp[j]) >> 15);
  }
}

// Commands 8 and 9: aux A or B goes to the game with one of aux C's channels, and four
// buffers come back for the main buses and aux C's left.
static void wii_upload_aux_mix_lrsc(int aux, const uint32_t *addresses, uint16_t volume) {
  int32_t *left = aux ? ax.aux_b_left : ax.aux_a_left, *right = aux ? ax.aux_b_right : ax.aux_a_right;
  int32_t *surround = aux ? ax.aux_b_surround : ax.aux_a_surround, *aux_c = aux ? ax.aux_c_surround : ax.aux_c_right;
  upload(addresses[0], left, 96);
  upload(addresses[0] + 96 * 4, right, 96);
  upload(addresses[0] + 2 * 96 * 4, surround, 96);
  upload(addresses[1], aux_c, 96);
  uint16_t ramp[96];
  volume_ramp(ramp, ax.last_aux_volumes[aux], volume, 96);
  ax.last_aux_volumes[aux] = volume;
  int32_t *into[4] = {ax.main_left, ax.main_right, ax.main_surround, ax.aux_c_left};
  for (int i = 0; i < 4; i++) {
    for (uint32_t j = 0; j < 96; j++) left[j] = download(addresses[2 + i], j);  // Through the aux's own left, as Dolphin does.
    for (uint32_t j = 0; j < 96; j++) into[i][j] += (int32_t)(((int64_t)left[j] * ramp[j]) >> 15);
  }
}

static void wii_output(uint32_t lr_address, uint32_t surround_address, uint16_t volume, int upload_aux_c) {
  uint16_t ramp[96];
  volume_ramp(ramp, ax.last_main_volume, volume, 96);
  ax.last_main_volume = volume;
  upload(surround_address, ax.main_surround, 96);
  if (upload_aux_c) upload(surround_address + 96 * 4, ax.aux_c_left, 96);
  for (uint32_t i = 0; i < 96; i++) {
    ax.main_left[i] = clamp16(((int64_t)ax.main_left[i] * ramp[i]) >> 15);
    ax.main_right[i] = clamp16(((int64_t)ax.main_right[i] * ramp[i]) >> 15);
  }
  for (uint32_t i = 0; i < 96; i++) {  // Right, then left.
    wr16(lr_address + i * 4, (uint16_t)ax.main_right[i]);
    wr16(lr_address + i * 4 + 2, (uint16_t)ax.main_left[i]);
  }
  dsp_push_mail(DSP_MAIL_SYNC, 1, 0);
}

// The four Wii Remote speakers' 18 samples each, for the game to send them.
static void wii_output_remotes(const uint32_t *addresses) {
  for (int i = 0; i < 4; i++) {
    for (uint32_t j = 0; j < 18; j++) wr16(addresses[i] + j * 2, (uint16_t)clamp16(ax.remote_main[i][j]));
  }
}

static void wii_run_list(void) {
  // The older programs number their commands one higher from 4 on, for a separate
  // command that names the voice list, and their output command has no volume.
  enum { SETUP, ADD, SUBTRACT, ADD_SUBTRACT, PB_ADDRESS, PROCESS, MIX_AUX_A, MIX_AUX_B, MIX_AUX_C, UPLOAD_AUX_A_LRSC,
         UPLOAD_AUX_B_LRSC, COMPRESSOR, OUTPUT, OUTPUT_DOLBY, REMOTES, END };
  uint32_t pb_address = 0;
  for (uint32_t at = 0;;) {
    if (at >= 512 - 16) {
      gap_hit("limit.ax_command_list", at);
      return;
    }
    uint16_t raw = ax.list[at++];
    int command = ax.old_wii ? raw : raw >= 4 ? raw + 1 : raw;
#define ARG32() (at += 2, (uint32_t)ax.list[at - 2] << 16 | ax.list[at - 1])
    switch (command) {
    case SETUP: {
      BufferDescription buffers[20] = {
        {ax.main_left, 32}, {ax.main_right, 32}, {ax.main_surround, 32}, {ax.aux_a_left, 32}, {ax.aux_a_right, 32},
        {ax.aux_a_surround, 32}, {ax.aux_b_left, 32}, {ax.aux_b_right, 32}, {ax.aux_b_surround, 32}, {ax.aux_c_left, 32},
        {ax.aux_c_right, 32}, {ax.aux_c_surround, 32}, {ax.remote_main[0], 6}, {ax.remote_aux[0], 6}, {ax.remote_main[1], 6},
        {ax.remote_aux[1], 6}, {ax.remote_main[2], 6}, {ax.remote_aux[2], 6}, {ax.remote_main[3], 6}, {ax.remote_aux[3], 6},
      };
      init_mixing_buffers(ARG32(), 3, buffers, 20);
      break;
    }
    case ADD: case SUBTRACT: {
      uint32_t address = ARG32();
      for (uint32_t i = 0; i < 96; i++) {
        int32_t value = download(address, i);
        if (command == SUBTRACT) value = -value;
        ax.main_left[i] += value;
        ax.main_right[i] += value;
      }
      break;
    }
    case ADD_SUBTRACT: {
      uint32_t address = ARG32();
      for (uint32_t i = 0; i < 96; i++) ax.main_left[i] += download(address, i);
      for (uint32_t i = 0; i < 96; i++) ax.main_right[i] -= download(address, 96 + i);
      break;
    }
    case PB_ADDRESS: pb_address = ARG32(); break;
    case PROCESS:
      if (!ax.old_wii) pb_address = ARG32();
      wii_process_pb_list(pb_address);
      break;
    case MIX_AUX_A: case MIX_AUX_B: case MIX_AUX_C: {
      uint16_t volume = ax.list[at++];
      uint32_t write_address = ARG32(), read_address = ARG32();
      wii_mix_aux(command - MIX_AUX_A, write_address, read_address, volume);
      break;
    }
    case UPLOAD_AUX_A_LRSC: case UPLOAD_AUX_B_LRSC: {
      uint16_t volume = ax.list[at++];
      uint32_t addresses[6];
      for (int i = 0; i < 6; i++) addresses[i] = ARG32();
      wii_upload_aux_mix_lrsc(command == UPLOAD_AUX_B_LRSC, addresses, volume);
      break;
    }
    case COMPRESSOR: {
      uint16_t threshold = ax.list[at], frames = ax.list[at + 1];
      at += 2;
      run_compressor(threshold, frames, ARG32(), 3);
      break;
    }
    case OUTPUT: case OUTPUT_DOLBY: {
      // The oldest and the System Menu 1.0's have no volume.
      uint16_t volume = ax.old_wii || ax.crc == 0xd9c4bf34u ? 0x8000 : ax.list[at++];
      uint32_t surround = ARG32(), lr = ARG32();
      wii_output(lr, surround, volume, command == OUTPUT_DOLBY);
      break;
    }
    case REMOTES: {
      uint32_t addresses[4];
      for (int i = 0; i < 4; i++) addresses[i] = ARG32();
      wii_output_remotes(addresses);
      break;
    }
    case END: return;
    default:
      gap_note("unlisted.audio.ax_command", raw);
      return;
    }
#undef ARG32
  }
}

// --- The program behind the mailbox (AX.cpp, HandleMail). The game sends the list's size,
// then its address; the list runs at once, and the program yields. The game then says what
// comes next: carry on, a new program, or the boot ROM again. ---

#define MAIL_LIST 0xbabe0000u
// Dolphin's AX_EMPTY_COMMAND_LIST_CYCLES: some games hang if the yield comes sooner.
#define LIST_CYCLES 2500u

enum { WAIT_LIST_SIZE, WAIT_LIST_ADDRESS, WAIT_NEXT_TASK };
static int mail_state;

// The Wii's versions (UCodes.cpp); dsp.c sends the GameCube's here too.
static int is_wii(uint32_t crc) {
  static const uint32_t wii[] = {0x347112bau, 0xfa450138u, 0xadbc06bdu, 0x4cc52064u, 0xd9c4bf34u, 0x7699af32u};
  for (size_t i = 0; i < sizeof wii / sizeof wii[0]; i++) {
    if (wii[i] == crc) return 1;
  }
  return 0;
}

static void ax_start(void) {
  memset(&ax, 0, sizeof ax);
  ax.crc = dsp_program_crc();
  ax.wii = is_wii(ax.crc);
  ax.old_wii = ax.crc == 0xfa450138u || ax.crc == 0x7699af32u;
  ax.new_filter = ax.crc == 0x347112bau || ax.crc == 0x4cc52064u;
  ax.last_main_volume = 0x8000;
  for (int i = 0; i < 3; i++) ax.last_aux_volumes[i] = 0x8000;
  load_coefficients();
  mail_state = WAIT_LIST_SIZE;
  dsp_push_mail(DSP_MAIL_INIT, 1, 0);
}

static void ax_mail(uint32_t mail) {
  switch (mail_state) {
  case WAIT_LIST_SIZE:
    if ((mail & TASK_MAIL_MASK) == MAIL_LIST) {
      ax.list_size = (uint16_t)mail;
      mail_state = WAIT_LIST_ADDRESS;
    } else {
      gap_note("unlisted.audio.ax_mail", mail);
    }
    break;
  case WAIT_LIST_ADDRESS: {
    uint64_t started = time_base();
    copy_list(mail, ax.list_size);
    if (ax.wii) wii_run_list();
    else gc_run_list();
    ax.list_size = 0;
    stage_add(STAGE_AUDIO, time_base() - started);
    dsp_push_mail(DSP_MAIL_YIELD, 1, LIST_CYCLES);
    mail_state = WAIT_NEXT_TASK;
    break;
  }
  case WAIT_NEXT_TASK:  // The program itself doesn't check the CDD1 prefix.
    switch (TASK_MAIL_TO_DSP | (mail & ~TASK_MAIL_MASK)) {
    case TASK_RESUME: dsp_push_mail(DSP_MAIL_RESUME, 1, 0); mail_state = WAIT_LIST_SIZE; break;
    case TASK_NEW_PROGRAM: dsp_expect_new_program(); mail_state = WAIT_LIST_SIZE; break;
    case TASK_RESET: dsp_set_program(PROGRAM_ROM); break;
    case TASK_CONTINUE: mail_state = WAIT_LIST_SIZE; break;
    default: gap_note("unlisted.audio.ax_mail", mail); break;
    }
    break;
  }
}

static void ax_update(void) {
  if (dsp_needs_resume_mail()) dsp_push_mail(DSP_MAIL_RESUME, 1, 0);
}

const DspProgram ax_program = {"AX", ax_start, ax_mail, ax_update};
