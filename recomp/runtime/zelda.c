// The Zelda family of audio programs, in C. Nintendo's "DAC" sound program came in many
// versions, from the GameCube's start-up screen to Super Mario Galaxy 2. The game fills a
// parameter block for each voice, and the program reads them, makes samples from guest
// memory or from its own wave tables, mixes them, and writes stereo back to the game's
// memory, where the audio DMA plays it from.
//
// This follows Dolphin's HLE version line for line (HW/DSPHLE/UCodes/Zelda.cpp, and
// GBA.cpp for the GameCube link's key exchange), including its odd corners, because it
// is checked sample for sample against the real program. Where Dolphin doesn't know what
// the program does, or would crash, we report a gap instead.
#include <stddef.h>
#include <string.h>

#include "cpu.h"
#include "debug.h"
#include "dsp.h"
#include "gaps.h"
#include "zelda.h"

// What changes from one version of the program to the next (Zelda.cpp, ZeldaUCodeFlag).
enum {
  NO_ARAM = 0x001,              // Wii: samples come from main memory instead of ARAM.
  MAKE_DOLBY_LOUDER = 0x002,    // Positional volumes doubled.
  LIGHT_PROTOCOL = 0x004,       // Early, smaller program: other mail rules, no positional mixing.
  FOUR_MIXING_DESTS = 0x008,    // Four mixing outputs per voice instead of six.
  TINY_VPB = 0x010,             // Voice blocks of 0x80 words instead of 0xc0.
  VOLUME_EXPLICIT_STEP = 0x020, // Mixing gives a step, not a target volume.
  SYNC_PER_FRAME = 0x040,       // The game unlocks voices once a frame, not 16 at a time.
  NO_CMD_0D = 0x080,            // Command 0D does nothing.
  SUPPORTS_GBA_CRYPTO = 0x100,  // Command 0C works out the GameCube link's key.
  WEIRD_CMD_0C = 0x200,         // Command 0C does something nobody has worked out.
  COMBINED_CMD_0D = 0x400,      // Command 02 carries command 0D's two values.
};

static const struct { uint32_t crc, flags; } ucode_flags[] = {
  {0x24b22038u, LIGHT_PROTOCOL | FOUR_MIXING_DESTS | TINY_VPB | VOLUME_EXPLICIT_STEP | NO_CMD_0D | WEIRD_CMD_0C},  // GameCube menu, NTSC.
  {0x6ba3b3eau, LIGHT_PROTOCOL | FOUR_MIXING_DESTS | NO_CMD_0D},           // GameCube menu, PAL.
  {0xdf059f68u, LIGHT_PROTOCOL | NO_CMD_0D | SUPPORTS_GBA_CRYPTO},         // Pikmin demo, NTSC.
  {0x4be6a5cbu, LIGHT_PROTOCOL | NO_CMD_0D | SUPPORTS_GBA_CRYPTO},         // Pikmin NTSC, Animal Crossing.
  {0x42f64ac4u, LIGHT_PROTOCOL | NO_CMD_0D | WEIRD_CMD_0C},                // Luigi's Mansion.
  {0x267fd05au, SYNC_PER_FRAME | NO_CMD_0D},                               // Pikmin PAL.
  {0x56d36052u, SYNC_PER_FRAME | NO_CMD_0D},                               // Super Mario Sunshine.
  {0x86840740u, 0},                                                        // The Wind Waker.
  {0x2fcdf1ecu, MAKE_DOLBY_LOUDER},                                        // Collector's Edition, Four Swords, Double Dash, Pikmin 2.
  {0x6ca33a6du, MAKE_DOLBY_LOUDER | COMBINED_CMD_0D},                      // Twilight Princess and Jungle Beat, GameCube.
  {0x6c3f6f94u, NO_ARAM | MAKE_DOLBY_LOUDER | COMBINED_CMD_0D},            // Twilight Princess Wii, Link's Crossbow Training.
  {0xd643001fu, NO_ARAM | MAKE_DOLBY_LOUDER | COMBINED_CMD_0D},            // Super Mario Galaxy 1 and 2, Jungle Beat Wii.
  {0xb7eb9a9cu, NO_ARAM | MAKE_DOLBY_LOUDER | COMBINED_CMD_0D},            // Pikmin, New Play Control.
  {0xeaeb38ccu, NO_ARAM | MAKE_DOLBY_LOUDER | COMBINED_CMD_0D},            // Pikmin 2, New Play Control.
};

#define GAP_ZELDA "audio.zelda_galaxy_twilight_princess_pikmin"
#define GAP_MEMORY "audio.zelda_memory_range"
#define GAP_MAIL "audio.zelda_unexpected_mail"
#define GAP_COMMAND "audio.zelda_unknown_command"
#define GAP_BUFFER "audio.zelda_unknown_buffer"

#define SAMPLES 0x50  // In a frame, for each output.
#define PATTERN_SIZE 0x40

// Shifts done in Dolphin on signed values, without C's rules on shifting negatives.
static inline int32_t shl16(int32_t value) { return (int32_t)((uint32_t)value << 16); }
static inline int32_t clamp16(int32_t value) { return value < -0x8000 ? -0x8000 : value > 0x7fff ? 0x7fff : value; }
static inline int64_t clamp16_64(int64_t value) { return value < -0x8000 ? -0x8000 : value > 0x7fff ? 0x7fff : value; }

// Main memory as Dolphin's GetPointerForRange sees it: both ends must be memory, and the
// range smaller than MEM2 so it can't span two banks. Gaps name the MiB, to stay few.
static uint8_t *memory_range(uint32_t address, uint32_t bytes) {
  if (bytes >= MEM2_SIZE) return 0;
  uint8_t *first = host(address), *last = host(address + bytes - 1);
  if (!first || !last || last != first + (bytes - 1)) {
    gap_hit(GAP_MEMORY, address >> 20);
    return 0;
  }
  return first;
}

// CopyFromEmuSwapped and CopyToEmuSwapped for 16-bit values. Out of range, nothing moves.
static int copy_from_guest(int16_t *to, uint32_t address, uint32_t bytes) {
  if (!bytes) return 1;
  const uint8_t *from = memory_range(address, bytes);
  if (!from) return 0;
  for (uint32_t i = 0; i < bytes / 2; i++) to[i] = (int16_t)(from[i * 2] << 8 | from[i * 2 + 1]);
  return 1;
}

static void copy_to_guest(uint32_t address, const int16_t *from, uint32_t bytes) {
  if (!bytes) return;
  uint8_t *to = memory_range(address, bytes);
  if (!to) return;
  for (uint32_t i = 0; i < bytes / 2; i++) {
    to[i * 2] = (uint8_t)((uint16_t)from[i] >> 8);
    to[i * 2 + 1] = (uint8_t)from[i];
  }
}

// A voice's parameter block, word for word as the game lays it out (Zelda.cpp, VPB).
// The first 0x80 words go back to the game after each frame; the rest it only writes.
#pragma pack(push, 1)
typedef struct {
  uint16_t id;  // Really the buffer's address in the processor's own memory.
  int16_t target_volume, current_volume;
  uint16_t unk;
} Channel;

typedef struct {
  uint16_t enabled, done, resampling_ratio, unk_03, reset_vpb, end_reached, use_constant_sample, samples_to_keep_count;
  Channel channels[6];
  uint16_t unk_20_28[8];
  uint16_t dolby_voice_position;  // 0XXXXXXX0YYYYYYY: left-right, front-back.
  int16_t dolby_reverb_factor, dolby_volume_current, dolby_volume_target;
  uint16_t use_dolby_volume, unk_2d, unk_2e, unk_2f;
  uint16_t current_pos_frac, unk_31, afc_remaining_decoded_samples;
  int16_t constant_sample;
  uint16_t current_position_h, current_position_l, samples_before_loop, unk_37;
  uint16_t current_aram_addr_h, current_aram_addr_l, remaining_length_h, remaining_length_l;
  int16_t resample_buffer[4];
  int16_t variable_fir_history[20];
  int16_t biquad_xn1, biquad_xn2, biquad_yn1, biquad_yn2;
  int16_t afc_remaining_samples[16];  // The last two are the decoder's history.
  int16_t low_pass_yn1, low_pass_xn1;
  uint16_t unk_6a_80[0x80 - 0x6a];
  uint16_t samples_source_type, is_looping;
  int16_t loop_yn1, loop_yn2;
  uint16_t filter_flags;  // Bits 0-4: variable FIR size. Bit 5: biquad on.
  uint16_t end_requested, unk_86, unk_87;
  uint16_t loop_address_h, loop_address_l, loop_start_position_h, loop_start_position_l;
  uint16_t base_address_h, base_address_l, unk_8e, unk_8f;
  uint16_t variable_fir_coeffs[20];
  int16_t biquad_bn1, biquad_bn2, biquad_an1, biquad_an2;
  uint16_t low_pass_coeff;
  uint16_t padding[0xc0];
} Vpb;

typedef struct {
  uint16_t enabled, circular_buffer_size, circular_buffer_base_h, circular_buffer_base_l;
  struct { uint16_t buffer_id, volume; } dest[2];
  int16_t filter_coeffs[8];
} ReverbPb;
#pragma pack(pop)

_Static_assert(offsetof(Vpb, current_pos_frac) == 0x30 * 2, "voice block layout");
_Static_assert(offsetof(Vpb, afc_remaining_samples) == 0x58 * 2, "voice block layout");
_Static_assert(offsetof(Vpb, samples_source_type) == 0x80 * 2, "voice block layout");
_Static_assert(offsetof(Vpb, low_pass_coeff) == 0xa8 * 2, "voice block layout");
_Static_assert(sizeof(ReverbPb) == 0x20, "reverb block layout");

#define AFC_SAMPLES_WORD 0x58
#define AFC_YN2 14
#define AFC_YN1 15

enum {
  SRC_SQUARE_WAVE = 0, SRC_SAW_WAVE = 1, SRC_SQUARE_WAVE_25PCT = 3, SRC_CONST_PATTERN_1 = 4,
  SRC_AFC_LQ_FROM_ARAM = 5, SRC_CONST_PATTERN_0 = 7, SRC_PCM8_FROM_ARAM = 8, SRC_AFC_HQ_FROM_ARAM = 9,
  SRC_CONST_PATTERN_0_VARIABLE_STEP = 10, SRC_CONST_PATTERN_2 = 11, SRC_CONST_PATTERN_3 = 12,
  SRC_PCM16_FROM_ARAM = 16, SRC_PCM16_FROM_MRAM = 33,
};

#define PAIR(field) ((uint32_t)vpb->field##_h << 16 | vpb->field##_l)
#define SET_PAIR(field, value) do { uint32_t v_ = (value); vpb->field##_h = (uint16_t)(v_ >> 16); vpb->field##_l = (uint16_t)v_; } while (0)

// The renderer (ZeldaAudioRenderer). One program runs at a time, so its state is static.
typedef int16_t MixingBuffer[SAMPLES];
static struct {
  uint32_t flags;
  int prepared;
  uint32_t output_lbuf_addr, output_rbuf_addr;
  uint16_t output_volume;
  MixingBuffer front_left, front_right, back_left, back_right, front_left_reverb, front_right_reverb,
      back_left_reverb, back_right_reverb, unk0_reverb, unk1_reverb, unk0, unk1, unk2;
  uint32_t vpb_base_addr;
  int16_t sine_table[0x80];        // sin(x) on [0, pi/4], 1.15.
  int16_t const_patterns[0x100];   // Four wave patterns of 0x40 samples.
  int16_t resampling_coeffs[0x100];
  uint32_t aram_base_addr;         // Wii: where in main memory "ARAM" starts.
  int16_t afc_coeffs[0x20];
  uint16_t reverb_pb_frames_count[4];
  int16_t reverb_last8[4][8];      // unk0, unk1, front left, front right.
  uint32_t reverb_pb_base_addr;
} r;

// Raw samples before resampling: four kept from last frame, at most 0x500 more, and 0x10
// for a stray whole part in the position that AFC rounds up (Zelda.cpp, LoadInputSamples).
static int16_t raw_input_samples[4 + 0x500 + 0x10];
static Vpb vpb_scratch;

// Volume in 1.15 (B = 1) or 4.12 (B = 4).
static void apply_volume_in_place(int16_t *buf, size_t count, uint16_t vol, int b) {
  for (size_t i = 0; i < count; i++) {
    int32_t tmp = (int32_t)((uint32_t)buf[i] * (uint32_t)vol);
    tmp >>= 16 - b;
    buf[i] = (int16_t)clamp16(tmp);
  }
}

// Ramps from vol by step for each sample, both 16.16 of a 1.15 volume. Dolphin ramps every
// sample; the real program does it in 32 steps.
static int32_t add_buffers_with_volume_ramp(int16_t *dst, const int16_t *src, int32_t vol, int32_t step) {
  if (!vol && !step) return vol;
  for (size_t i = 0; i < SAMPLES; i++) {
    dst[i] = (int16_t)(dst[i] + (((vol >> 16) * src[i]) >> 16));
    vol += step;
  }
  return vol;
}

static void add_buffers_with_volume(int16_t *dst, const int16_t *src, size_t count, uint16_t vol) {
  while (count--) {
    int32_t vol_src = ((int32_t)*src++ * (int32_t)vol) >> 15;
    *dst = (int16_t)(*dst + clamp16(vol_src));
    dst++;
  }
}

static int16_t *buffer_for_id(uint16_t buffer_id) {
  switch (buffer_id) {
  case 0x0d00: return r.front_left;
  case 0x0d60: return r.front_right;
  case 0x0f40: return r.back_left;
  case 0x0ca0: return r.back_right;
  case 0x0e80: return r.front_left_reverb;
  case 0x0ee0: return r.front_right_reverb;
  case 0x0c00: return r.back_left_reverb;
  case 0x0c50: return r.back_right_reverb;
  case 0x0dc0: return r.unk0_reverb;
  case 0x0e20: return r.unk1_reverb;
  case 0x09a0: return r.unk0;  // The GameCube menu's reverb output.
  case 0x0fa0: return r.unk1;  // The GameCube menu's mixing output.
  case 0x0b00: return r.unk2;  // Pikmin's mixing output.
  default: return 0;
  }
}

// An 8-tap filter over the frame and the eight samples before it, in place.
static void reverb_filter(int16_t *buffer, const int16_t *coeffs) {
  for (int i = 0; i < SAMPLES; ++i) {
    int32_t sample = 0;
    for (int j = 0; j < 8; ++j) sample += (int32_t)buffer[i + j] * coeffs[j];
    sample >>= 15;
    buffer[i] = (int16_t)clamp16(sample);
  }
}

// Each of the four reverb blocks feeds one buffer through a ring of frames in main
// memory: before the frame it reads one back, filters and spreads it, and after the frame
// it writes the buffer out (ApplyReverb).
static void apply_reverb(int post_rendering) {
  if (!r.reverb_pb_base_addr) return;
  int16_t *reverb_buffers[4] = {r.unk0_reverb, r.unk1_reverb, r.front_left_reverb, r.front_right_reverb};
  for (uint16_t rpb_idx = 0; rpb_idx < 4; ++rpb_idx) {
    ReverbPb rpb;
    memset(&rpb, 0, sizeof rpb);
    copy_from_guest((int16_t *)&rpb, r.reverb_pb_base_addr + rpb_idx * (uint32_t)sizeof(ReverbPb), sizeof(ReverbPb));
    if (!rpb.enabled) continue;

    uint16_t mram_buffer_idx = r.reverb_pb_frames_count[rpb_idx];
    uint32_t base = (uint32_t)rpb.circular_buffer_base_h << 16 | rpb.circular_buffer_base_l;
    uint32_t mram_addr = base + mram_buffer_idx * SAMPLES * (uint32_t)sizeof(int16_t);

    if (!post_rendering) {
      // Eight more samples for the filter: the last eight of the previous frame.
      int16_t buffer[SAMPLES + 8];
      for (int i = 0; i < 8; ++i) buffer[i] = r.reverb_last8[rpb_idx][i];
      if (!copy_from_guest(&buffer[8], mram_addr, SAMPLES * sizeof(int16_t))) memset(&buffer[8], 0, SAMPLES * sizeof(int16_t));
      for (int i = 0; i < 8; ++i) r.reverb_last8[rpb_idx][i] = buffer[SAMPLES + i];

      if (rpb.enabled & 1) reverb_filter(buffer, rpb.filter_coeffs);  // Bit 0: filter before mixing.
      for (int d = 0; d < 2; d++) {
        if (rpb.dest[d].buffer_id == 0) continue;
        int16_t *dest_buffer = buffer_for_id(rpb.dest[d].buffer_id);
        if (!dest_buffer) {
          gap_hit(GAP_BUFFER, rpb.dest[d].buffer_id);
          continue;
        }
        add_buffers_with_volume(dest_buffer, buffer, SAMPLES, rpb.dest[d].volume);
      }
      if (rpb.enabled & 2) reverb_filter(buffer, rpb.filter_coeffs);  // Bit 1: after.
      for (int i = 0; i < SAMPLES; ++i) reverb_buffers[rpb_idx][i] = buffer[i];
    } else {
      copy_to_guest(mram_addr, reverb_buffers[rpb_idx], SAMPLES * sizeof(int16_t));
      if (!rpb.circular_buffer_size) {  // Dolphin divides by zero here.
        gap_hit(GAP_ZELDA, 0x0e000000u | rpb_idx);
        mram_buffer_idx = 0;
      } else {
        mram_buffer_idx = (uint16_t)((mram_buffer_idx + 1) % rpb.circular_buffer_size);
      }
      r.reverb_pb_frames_count[rpb_idx] = mram_buffer_idx;
    }
  }
}

static void prepare_frame(void) {
  if (r.prepared) return;

  memset(r.front_left, 0, sizeof r.front_left);
  memset(r.front_right, 0, sizeof r.front_right);
  apply_volume_in_place(r.back_left, SAMPLES, 0x6784, 1);
  apply_volume_in_place(r.back_right, SAMPLES, 0x6784, 1);

  // Back left and right should be filtered and folded into the front. Dolphin doesn't,
  // so sound mixed there is lost (its STRICT_ZELDA_HLE check, over the whole buffer).
  if (!(r.flags & LIGHT_PROTOCOL)) {
    for (int i = 0; i < SAMPLES; i++) {
      if (r.back_left[i] || r.back_right[i]) {
        gap_hit("audio.zelda_dolby_back_channels", 0);
        break;
      }
    }
  }

  // Reverb from the previous frame.
  apply_reverb(0);
  add_buffers_with_volume(r.front_left_reverb, r.back_left_reverb, SAMPLES, 0x7fff);
  add_buffers_with_volume(r.front_right_reverb, r.back_left_reverb, SAMPLES, 0xb820);
  add_buffers_with_volume(r.front_left_reverb, r.back_right_reverb + 0x28, 0x28, 0xb820);
  add_buffers_with_volume(r.front_right_reverb, r.back_right_reverb + 0x28, 0x28, 0x7fff);
  memset(r.back_left_reverb, 0, sizeof r.back_left_reverb);
  memset(r.back_right_reverb, 0, sizeof r.back_right_reverb);

  // Patterns 2 and 3 change every frame, unlike 0 and 1.
  int16_t *pattern2 = r.const_patterns + 2 * PATTERN_SIZE;
  int32_t yn2 = pattern2[PATTERN_SIZE - 2], yn1 = pattern2[PATTERN_SIZE - 1], v;
  for (int i = 0; i < PATTERN_SIZE; i += 2) {
    v = yn2 * yn1 - shl16(pattern2[i]);
    yn2 = yn1;
    yn1 = pattern2[i];
    pattern2[i] = (int16_t)(v >> 16);

    v = 2 * (yn2 * yn1 + shl16(pattern2[i + 1]));
    yn2 = yn1;
    yn1 = pattern2[i + 1];
    pattern2[i + 1] = (int16_t)(v >> 16);
  }
  int16_t *pattern3 = r.const_patterns + 3 * PATTERN_SIZE;
  yn2 = pattern3[PATTERN_SIZE - 2];
  yn1 = pattern3[PATTERN_SIZE - 1];
  int16_t acc = (int16_t)yn1;
  int16_t step = (int16_t)(pattern3[0] + ((yn1 * yn2 + (shl16(yn2) + yn1)) >> 16));
  step = (int16_t)((step & 0x1ff) | 0x2000);
  for (int32_t i = 0; i < PATTERN_SIZE; ++i) pattern3[i] = (int16_t)(acc + (i + 1) * step);

  r.prepared = 1;
}

static void apply_low_pass_filter(int16_t *buf, Vpb *vpb) {
  int32_t yn1 = vpb->reset_vpb ? 0 : vpb->low_pass_yn1;
  int32_t xn1 = vpb->reset_vpb ? 0 : vpb->low_pass_xn1;
  int32_t coeff = vpb->low_pass_coeff;  // 9.7, Dolphin thinks.
  for (int i = 0; i < SAMPLES; ++i) {
    int32_t xn0 = buf[i];
    int64_t tmp = xn0 - xn1;
    tmp *= coeff;
    tmp >>= 7;
    tmp += yn1;
    int16_t yn0 = (int16_t)clamp16_64(tmp);
    buf[i] = yn0;
    yn1 = yn0;
    xn1 = xn0;
  }
  vpb->low_pass_yn1 = (int16_t)yn1;
  vpb->low_pass_xn1 = (int16_t)xn1;
}

// As Dolphin has it, the current input sample takes no part.
static void apply_biquad_filter(int16_t *buf, Vpb *vpb) {
  int32_t xn1 = vpb->biquad_xn1, xn2 = vpb->biquad_xn2, yn1 = vpb->biquad_yn1, yn2 = vpb->biquad_yn2;
  for (int i = 0; i < SAMPLES; ++i) {
    int32_t xn0 = buf[i];
    int64_t tmp = 0;
    tmp += vpb->biquad_bn1 * xn1;
    tmp += vpb->biquad_bn2 * xn2;
    tmp += vpb->biquad_an1 * yn1;
    tmp += vpb->biquad_an2 * yn2;
    int16_t yn0 = (int16_t)clamp16_64(tmp >> 15);
    buf[i] = yn0;
    xn2 = xn1;
    xn1 = xn0;
    yn2 = yn1;
    yn1 = yn0;
  }
  vpb->biquad_xn1 = (int16_t)xn1;
  vpb->biquad_xn2 = (int16_t)xn2;
  vpb->biquad_yn1 = (int16_t)yn1;
  vpb->biquad_yn2 = (int16_t)yn2;
}

// The earliest programs' 0x80-word blocks lack words 0x40-0x7f and positional mixing.
// These move the words to and from where the full layout keeps them.
static void vpb_uncompress(Vpb *vpb) {
  uint16_t *words = (uint16_t *)vpb;
  for (int i = 0; i < 0x40; ++i) { words[0x80 + i] = words[0x40 + i]; words[0x40 + i] = 0; }
  for (int i = 0; i < 0x10; ++i) { words[0x58 + i] = words[0x30 + i]; words[0x30 + i] = 0; }
  for (int i = 0; i < 0x18; ++i) { words[0x30 + i] = words[0x18 + i]; words[0x18 + i] = 0; }
}

static void vpb_compress(Vpb *vpb) {
  uint16_t *words = (uint16_t *)vpb;
  for (int i = 0; i < 0x18; ++i) { words[0x18 + i] = words[0x30 + i]; words[0x30 + i] = 0; }
  for (int i = 0; i < 0x10; ++i) { words[0x30 + i] = words[0x58 + i]; words[0x58 + i] = 0; }
  for (int i = 0; i < 0x40; ++i) { words[0x40 + i] = words[0x80 + i]; words[0x80 + i] = 0; }
}

static uint32_t vpb_bytes(void) { return ((r.flags & TINY_VPB) ? 0x80u : 0xc0u) * (uint32_t)sizeof(uint16_t); }

static void fetch_vpb(uint16_t voice_id, Vpb *vpb) {
  uint32_t size = vpb_bytes();
  if (!copy_from_guest((int16_t *)vpb, r.vpb_base_addr + voice_id * size, size)) memset(vpb, 0, sizeof *vpb);  // Skipped.
  if (r.flags & TINY_VPB) vpb_uncompress(vpb);
}

// Only the first 0x80 bytes short of the block go back: the rest is the game's.
static void store_vpb(uint16_t voice_id, Vpb *vpb) {
  uint32_t size = vpb_bytes();
  if (r.flags & TINY_VPB) vpb_compress(vpb);
  copy_to_guest(r.vpb_base_addr + voice_id * size, (const int16_t *)vpb, size - 0x80);
}

// The "ARAM" sample sources. On a Wii, Dolphin reads them from main memory at the base
// that command 0E set, whatever the program's version, and so do we.
static uint8_t *aram_range(uint32_t offset, uint32_t bytes) {
  if (!bytes) return 0;
  return memory_range(r.aram_base_addr + offset, bytes);
}

static uint16_t needed_raw_samples_count(const Vpb *vpb) {
  return (uint16_t)((vpb->current_pos_frac + SAMPLES * vpb->resampling_ratio) >> 12);  // Both 4.12.
}

static void resample(Vpb *vpb, const int16_t *src, int16_t *dst) {
  uint32_t ratio = vpb->resampling_ratio;  // 20.12, as is the position.
  uint32_t pos = vpb->current_pos_frac;
  if ((ratio >> 12) >= 4) {  // Over 4 to 1, no interpolation.
    for (int i = 0; i < SAMPLES; i++) {
      pos += ratio;
      dst[i] = src[pos >> 12];
    }
  } else {
    for (int i = 0; i < SAMPLES; i++) {
      // Four taps from 0x40 sets, picked by the top six bits of the fraction.
      uint32_t coeffs_idx = ((pos & 0xfff) >> 6) * 4;
      const int16_t *coeffs = &r.resampling_coeffs[coeffs_idx];
      const int16_t *input = &src[pos >> 12];
      int64_t dst_sample_unclamped = 0;
      for (int k = 0; k < 4; ++k) dst_sample_unclamped += (int64_t)2 * coeffs[k] * input[k];
      dst_sample_unclamped >>= 16;
      dst[i] = (int16_t)clamp16_64(dst_sample_unclamped);
      pos += ratio;
    }
  }
  for (uint32_t i = 0; i < 4; ++i) vpb->resample_buffer[i] = src[(pos >> 12) + i];
  vpb->constant_sample = dst[SAMPLES - 1];
  vpb->current_pos_frac = (uint16_t)(pos & 0xfff);
}

// PCM8 or PCM16 from ARAM, following loops (DownloadPCMSamplesFromARAM).
static void download_pcm_samples_from_aram(int16_t *dst, Vpb *vpb, uint16_t requested_samples_count, uint32_t size) {
  if (vpb->done) {
    for (uint16_t i = 0; i < requested_samples_count; ++i) dst[i] = 0;
    return;
  }
  if (vpb->reset_vpb) {
    SET_PAIR(remaining_length, PAIR(loop_start_position) - PAIR(current_position));
    SET_PAIR(current_aram_addr, PAIR(base_address) + PAIR(current_position) * size);
  }
  vpb->end_reached = 0;
  while (requested_samples_count) {
    if (vpb->end_reached) {
      vpb->end_reached = 0;
      if (!vpb->is_looping) {
        for (uint16_t i = 0; i < requested_samples_count; ++i) dst[i] = 0;
        vpb->done = 1;
        break;
      }
      SET_PAIR(current_position, PAIR(loop_address));
      SET_PAIR(remaining_length, PAIR(loop_start_position) - PAIR(current_position));
      SET_PAIR(current_aram_addr, PAIR(base_address) + PAIR(current_position) * size);
      if (!PAIR(remaining_length)) {  // Dolphin spins here for ever.
        gap_hit("limit.zelda_empty_loop", vpb->samples_source_type);
        for (uint16_t i = 0; i < requested_samples_count; ++i) dst[i] = 0;
        break;
      }
    }
    uint32_t remaining = PAIR(remaining_length);
    uint16_t samples_to_download = (uint16_t)(remaining < requested_samples_count ? remaining : requested_samples_count);
    const uint8_t *src = aram_range(PAIR(current_aram_addr), samples_to_download * size);
    for (uint16_t i = 0; i < samples_to_download; ++i) {
      if (!src) *dst++ = 0;  // Dolphin would crash.
      else if (size == 1) *dst++ = (int16_t)((int8_t)src[i] << 8);
      else *dst++ = (int16_t)(src[i * 2] << 8 | src[i * 2 + 1]);
    }
    SET_PAIR(remaining_length, PAIR(remaining_length) - samples_to_download);
    SET_PAIR(current_aram_addr, PAIR(current_aram_addr) + samples_to_download * size);
    requested_samples_count -= samples_to_download;
    if (!PAIR(remaining_length)) vpb->end_reached = 1;
  }
}

// AFC: blocks of 16 samples, a header byte of scale and filter, then 4-bit samples in 8
// bytes (source 9) or 2-bit in 4 bytes (source 5). The source number is the block size.
static void decode_afc(Vpb *vpb, int16_t *dst, size_t block_count) {
  uint32_t addr = PAIR(current_aram_addr);
  const uint8_t *src = aram_range(addr, (uint32_t)block_count * vpb->samples_source_type);
  SET_PAIR(current_aram_addr, addr + (uint32_t)block_count * vpb->samples_source_type);
  if (!src) {  // Dolphin would crash.
    for (size_t i = 0; i < block_count * 16; i++) dst[i] = 0;
    return;
  }
  for (size_t b = 0; b < block_count; ++b) {
    int16_t nibbles[16];
    int16_t delta = (int16_t)(1 << ((*src >> 4) & 0xf));
    int16_t idx = (int16_t)(*src & 0xf);
    src++;
    if (vpb->samples_source_type == SRC_AFC_HQ_FROM_ARAM) {
      for (size_t i = 0; i < 16; i += 2) {
        nibbles[i + 0] = (int16_t)(*src >> 4);
        nibbles[i + 1] = (int16_t)(*src & 0xf);
        src++;
      }
      for (int i = 0; i < 16; i++) nibbles[i] = (int16_t)((int16_t)(nibbles[i] << 12) >> 1);
    } else {
      for (size_t i = 0; i < 16; i += 4) {
        nibbles[i + 0] = (int16_t)((*src >> 6) & 3);
        nibbles[i + 1] = (int16_t)((*src >> 4) & 3);
        nibbles[i + 2] = (int16_t)((*src >> 2) & 3);
        nibbles[i + 3] = (int16_t)((*src >> 0) & 3);
        src++;
      }
      for (int i = 0; i < 16; i++) nibbles[i] = (int16_t)((int16_t)(nibbles[i] << 14) >> 1);
    }
    int32_t yn1 = vpb->afc_remaining_samples[AFC_YN1], yn2 = vpb->afc_remaining_samples[AFC_YN2];
    for (int i = 0; i < 16; i++) {
      int32_t sample = delta * nibbles[i] + yn1 * r.afc_coeffs[idx * 2] + yn2 * r.afc_coeffs[idx * 2 + 1];
      sample >>= 11;
      sample = clamp16(sample);
      *dst++ = (int16_t)sample;
      yn2 = yn1;
      yn1 = sample;
    }
    vpb->afc_remaining_samples[AFC_YN2] = (int16_t)yn2;
    vpb->afc_remaining_samples[AFC_YN1] = (int16_t)yn1;
  }
}

static void download_afc_samples_from_aram(int16_t *dst, Vpb *vpb, uint16_t requested_samples_count) {
  if (vpb->reset_vpb) {
    vpb->afc_remaining_samples[AFC_YN1] = 0;
    vpb->afc_remaining_samples[AFC_YN2] = 0;
    vpb->afc_remaining_decoded_samples = 0;
    SET_PAIR(remaining_length, PAIR(loop_start_position));
    SET_PAIR(current_aram_addr, PAIR(base_address));
  }
  if (vpb->done) {
    for (uint16_t i = 0; i < requested_samples_count; ++i) dst[i] = 0;
    return;
  }
  if (vpb->afc_remaining_decoded_samples > 0x10) vpb->afc_remaining_decoded_samples = 0x10;

  int16_t *words = (int16_t *)vpb;
  for (;;) {
    // What's already decoded goes first.
    uint16_t remaining_to_output = vpb->afc_remaining_decoded_samples < requested_samples_count
                                       ? vpb->afc_remaining_decoded_samples : requested_samples_count;
    const int16_t *base = &vpb->afc_remaining_samples[0x10 - vpb->afc_remaining_decoded_samples];
    for (size_t i = 0; i < remaining_to_output; ++i) *dst++ = base[i];
    vpb->afc_remaining_decoded_samples -= remaining_to_output;
    requested_samples_count -= remaining_to_output;

    if (requested_samples_count == 0) return;
    if (requested_samples_count <= PAIR(remaining_length)) {
      uint16_t requested_blocks_count = (uint16_t)((requested_samples_count + 0xf) >> 4);
      uint16_t decoded_samples_count = (uint16_t)(requested_blocks_count << 4);
      if (decoded_samples_count < PAIR(remaining_length)) {
        vpb->afc_remaining_decoded_samples = (uint16_t)(decoded_samples_count - requested_samples_count);
        SET_PAIR(remaining_length, PAIR(remaining_length) - decoded_samples_count);
      } else {
        vpb->afc_remaining_decoded_samples = (uint16_t)(PAIR(remaining_length) - requested_samples_count);
        SET_PAIR(remaining_length, 0);
      }
      decode_afc(vpb, dst, requested_blocks_count);
      if (vpb->afc_remaining_decoded_samples) {
        for (size_t i = 0; i < 0x10; ++i) vpb->afc_remaining_samples[i] = dst[decoded_samples_count - 0x10 + i];
        if (!PAIR(remaining_length) && PAIR(loop_start_position)) {
          // Line the kept samples up for the next time round the loop. As in Dolphin, this
          // can reach back past the kept samples into the words before them.
          int at = AFC_SAMPLES_WORD + (int)((PAIR(loop_start_position) + 0xf) & 0xf);
          for (size_t i = 0; i < vpb->afc_remaining_decoded_samples; ++i) words[AFC_SAMPLES_WORD + 0x10 - i - 1] = words[at--];
        }
      }
      return;
    }
    // More asked for than is left: end the sound, or loop.
    if (PAIR(remaining_length)) {
      requested_samples_count -= (uint16_t)PAIR(remaining_length);
      uint16_t requested_blocks_count = (uint16_t)((PAIR(remaining_length) + 0xf) >> 4);
      decode_afc(vpb, dst, requested_blocks_count);
      dst += PAIR(remaining_length);
    }
    if (!vpb->is_looping) {
      vpb->done = 1;
      for (size_t i = 0; i < requested_samples_count; ++i) *dst++ = 0;
      return;
    }
    uint32_t loop_off_in_bytes = (PAIR(loop_address) >> 4) * vpb->samples_source_type;
    SET_PAIR(current_aram_addr, PAIR(base_address) + loop_off_in_bytes);
    vpb->afc_remaining_samples[AFC_YN2] = vpb->loop_yn2;
    vpb->afc_remaining_samples[AFC_YN1] = vpb->loop_yn1;
    decode_afc(vpb, vpb->afc_remaining_samples, 1);
    vpb->afc_remaining_decoded_samples = (uint16_t)(0x10 - (PAIR(loop_address) & 0xf));
    uint32_t remaining_length = PAIR(loop_start_position);
    remaining_length -= vpb->afc_remaining_decoded_samples;
    remaining_length -= PAIR(loop_address);
    SET_PAIR(remaining_length, remaining_length);
  }
}

// PCM16 straight from main memory (DownloadRawSamplesFromMRAM). At the end, Dolphin
// fills with the last sample from the start of the buffer, over what it just copied;
// that is kept.
static void download_raw_samples_from_mram(int16_t *dst, Vpb *vpb, uint16_t requested_samples_count) {
  uint32_t addr = PAIR(base_address) + vpb->current_position_h * (uint32_t)sizeof(uint16_t);
  uint32_t remaining_length = PAIR(remaining_length);
  if (requested_samples_count > remaining_length) {
    int16_t last_sample = 0;
    if (remaining_length != 0) {
      copy_from_guest(dst, addr, remaining_length * sizeof(int16_t));
      last_sample = dst[remaining_length - 1];
    }
    for (uint16_t i = (uint16_t)remaining_length; i < requested_samples_count; ++i) *dst++ = last_sample;
    vpb->current_position_h = (uint16_t)(vpb->current_position_h + remaining_length);
    SET_PAIR(remaining_length, 0);
    vpb->done = 1;
  } else {
    SET_PAIR(remaining_length, remaining_length - requested_samples_count);
    vpb->samples_before_loop = (uint16_t)(vpb->loop_start_position_h - vpb->current_position_h);
    if (requested_samples_count <= vpb->samples_before_loop) {
      copy_from_guest(dst, addr, requested_samples_count * (uint32_t)sizeof(int16_t));
      vpb->current_position_h = (uint16_t)(vpb->current_position_h + requested_samples_count);
    } else {
      copy_from_guest(dst, addr, vpb->samples_before_loop * (uint32_t)sizeof(int16_t));
      SET_PAIR(base_address, PAIR(loop_address));
      vpb->current_position_h = (uint16_t)(requested_samples_count - vpb->samples_before_loop);
      copy_from_guest(dst + vpb->samples_before_loop, PAIR(loop_address), vpb->current_position_h * (uint32_t)sizeof(int16_t));
    }
  }
}

static void load_input_samples(int16_t *buffer, Vpb *vpb) {
  for (size_t i = 0; i < 4; ++i) raw_input_samples[i] = vpb->resample_buffer[i];
  if (vpb->use_constant_sample) {
    for (int i = 0; i < SAMPLES; i++) buffer[i] = vpb->constant_sample;
    return;
  }
  switch (vpb->samples_source_type) {
  case SRC_SQUARE_WAVE:
  case SRC_SQUARE_WAVE_25PCT: {
    uint32_t shift = vpb->samples_source_type == SRC_SQUARE_WAVE ? 1 : 2;
    uint32_t mask = (1u << shift) - 1;
    uint32_t ratio = (uint32_t)vpb->resampling_ratio << (shift - 1);
    uint32_t pos = (uint32_t)vpb->current_pos_frac << shift;
    for (int i = 0; i < SAMPLES; i++) {
      buffer[i] = ((pos >> 16) & mask) ? (int16_t)0xc000 : 0x4000;
      pos += ratio;
    }
    vpb->current_pos_frac = (uint16_t)((pos >> shift) & 0xffff);
    break;
  }
  case SRC_SAW_WAVE: {
    uint32_t pos = vpb->current_pos_frac;
    for (int i = 0; i < SAMPLES; i++) {
      buffer[i] = (int16_t)(pos & 0xffff);
      pos += vpb->resampling_ratio >> 1;
    }
    vpb->current_pos_frac = (uint16_t)(pos & 0xffff);
    break;
  }
  case SRC_CONST_PATTERN_0:
  case SRC_CONST_PATTERN_0_VARIABLE_STEP:
  case SRC_CONST_PATTERN_1:
  case SRC_CONST_PATTERN_2:
  case SRC_CONST_PATTERN_3: {
    uint16_t idx = vpb->samples_source_type == SRC_CONST_PATTERN_1 ? 1
                   : vpb->samples_source_type == SRC_CONST_PATTERN_2 ? 2
                   : vpb->samples_source_type == SRC_CONST_PATTERN_3 ? 3 : 0;
    int variable_step = vpb->samples_source_type == SRC_CONST_PATTERN_0_VARIABLE_STEP;
    const int16_t *pattern = r.const_patterns + idx * PATTERN_SIZE;
    uint32_t pos = (uint32_t)vpb->current_pos_frac << 6;   // log2 of the pattern size.
    uint32_t step = (uint32_t)vpb->resampling_ratio << 5;  // Dolphin: the NTSC menu's program may shift by 6.
    for (int i = 0; i < SAMPLES; ++i) {
      buffer[i] = pattern[pos >> 16];
      pos = (pos + step) % (PATTERN_SIZE << 16);
      if (variable_step) pos = ((pos << 10) + (uint32_t)(r.back_right[i] * vpb->resampling_ratio)) >> 10;
    }
    vpb->current_pos_frac = (uint16_t)(pos >> 6);
    break;
  }
  case SRC_PCM8_FROM_ARAM:
    download_pcm_samples_from_aram(raw_input_samples + 4, vpb, needed_raw_samples_count(vpb), 1);
    resample(vpb, raw_input_samples, buffer);
    break;
  case SRC_AFC_HQ_FROM_ARAM:
  case SRC_AFC_LQ_FROM_ARAM:
    download_afc_samples_from_aram(raw_input_samples + 4, vpb, needed_raw_samples_count(vpb));
    resample(vpb, raw_input_samples, buffer);
    break;
  case SRC_PCM16_FROM_ARAM:
    download_pcm_samples_from_aram(raw_input_samples + 4, vpb, needed_raw_samples_count(vpb), 2);
    resample(vpb, raw_input_samples, buffer);
    break;
  case SRC_PCM16_FROM_MRAM:
    download_raw_samples_from_mram(raw_input_samples + 4, vpb, needed_raw_samples_count(vpb));
    resample(vpb, raw_input_samples, buffer);
    break;
  default:  // Including 2 and 6, which nobody has worked out.
    gap_hit("audio.zelda_sample_source", vpb->samples_source_type);
    memset(buffer, 0, SAMPLES * sizeof(int16_t));
    return;
  }
}

static void add_voice(uint16_t voice_id) {
  Vpb *vpb = &vpb_scratch;
  fetch_vpb(voice_id, vpb);
  if (!vpb->enabled || vpb->done) return;

  int16_t input_samples[SAMPLES];
  load_input_samples(input_samples, vpb);

  if (vpb->low_pass_coeff != 0) apply_low_pass_filter(input_samples, vpb);
  if (vpb->filter_flags & 0x1f) gap_hit("audio.zelda_variable_fir", vpb->filter_flags & 0x1f);
  if ((vpb->filter_flags >> 5 & 1) &&
      (vpb->biquad_an2 != 0 || vpb->biquad_an1 != 0 || vpb->biquad_bn2 != 0 || vpb->biquad_bn1 != 0x7fff)) {
    apply_biquad_filter(input_samples, vpb);
  }

  if (vpb->use_dolby_volume) {
    if (vpb->end_requested) {
      vpb->dolby_volume_target = (int16_t)(vpb->dolby_volume_current / 2);
      if (vpb->dolby_volume_target == 0) vpb->done = 1;
    }
    // Each in 1.15.
    uint8_t x = (vpb->dolby_voice_position >> 8) & 0x7f, y = vpb->dolby_voice_position & 0x7f;
    int16_t right_volume = r.sine_table[x];
    int16_t back_volume = r.sine_table[y];
    int16_t left_volume = r.sine_table[x ^ 0x7f];
    int16_t front_volume = r.sine_table[y ^ 0x7f];

    int shift_factor = (r.flags & MAKE_DOLBY_LOUDER) ? 15 : 16;
    int16_t quadrant_volumes[4] = {
        (int16_t)((left_volume * front_volume) >> shift_factor),
        (int16_t)((left_volume * back_volume) >> shift_factor),
        (int16_t)((right_volume * front_volume) >> shift_factor),
        (int16_t)((right_volume * back_volume) >> shift_factor),
    };
    int16_t delta = (int16_t)(vpb->dolby_volume_target - vpb->dolby_volume_current);
    int16_t volume_deltas[4];
    for (int i = 0; i < 4; ++i) volume_deltas[i] = (int16_t)(((uint16_t)quadrant_volumes[i] * delta) >> shift_factor);
    for (int i = 0; i < 4; ++i) quadrant_volumes[i] = (int16_t)((quadrant_volumes[i] * vpb->dolby_volume_current) >> shift_factor);
    int16_t reverb_volumes[4], reverb_volume_deltas[4];
    for (int i = 0; i < 4; ++i) {
      reverb_volumes[i] = (int16_t)((quadrant_volumes[i] * vpb->dolby_reverb_factor) >> shift_factor);
      reverb_volume_deltas[i] = (int16_t)((volume_deltas[i] * vpb->dolby_reverb_factor) >> shift_factor);
    }
    struct { int16_t *buffer; int16_t volume, volume_delta; } buffers[8] = {
        {r.front_left, quadrant_volumes[0], volume_deltas[0]},
        {r.back_left, quadrant_volumes[1], volume_deltas[1]},
        {r.front_right, quadrant_volumes[2], volume_deltas[2]},
        {r.back_right, quadrant_volumes[3], volume_deltas[3]},
        {r.front_left_reverb, reverb_volumes[0], reverb_volume_deltas[0]},
        {r.back_left_reverb, reverb_volumes[1], reverb_volume_deltas[1]},
        {r.front_right_reverb, reverb_volumes[2], reverb_volume_deltas[2]},
        {r.back_right_reverb, reverb_volumes[3], reverb_volume_deltas[3]},
    };
    for (int i = 0; i < 8; i++) {
      add_buffers_with_volume_ramp(buffers[i].buffer, input_samples, shl16(buffers[i].volume),
                                   shl16(buffers[i].volume_delta) / (int32_t)SAMPLES);
    }
    vpb->dolby_volume_current = vpb->dolby_volume_target;
  } else {
    // Dolphin's TODO, storing input samples when the block asks, has no known flag to see.
    int num_channels = (r.flags & FOUR_MIXING_DESTS) ? 4 : 6;
    if (vpb->end_requested) {
      int all_mute = 1;
      for (int i = 0; i < num_channels; ++i) {
        vpb->channels[i].target_volume = (int16_t)(vpb->channels[i].current_volume / 2);
        all_mute &= vpb->channels[i].target_volume == 0;
      }
      if (all_mute) vpb->done = 1;
    }
    for (int i = 0; i < num_channels; ++i) {
      if (!vpb->channels[i].id) continue;
      int16_t volume_delta = (r.flags & VOLUME_EXPLICIT_STEP)
                                 ? vpb->channels[i].target_volume
                                 : (int16_t)(vpb->channels[i].target_volume - vpb->channels[i].current_volume);
      int32_t volume_step = shl16(volume_delta) / (int32_t)SAMPLES;  // 1.31.
      // The channel's last word decides whether it's skipped; nobody knows how.
      if (vpb->channels[i].unk) gap_hit("audio.zelda_channel_skip_word", 0);
      if (!vpb->channels[i].current_volume && !volume_step) continue;
      int16_t *dst_buffer = buffer_for_id(vpb->channels[i].id);
      if (!dst_buffer) {
        gap_hit(GAP_BUFFER, vpb->channels[i].id);
        continue;
      }
      int32_t new_volume = add_buffers_with_volume_ramp(dst_buffer, input_samples, shl16(vpb->channels[i].current_volume), volume_step);
      vpb->channels[i].current_volume = (int16_t)(new_volume >> 16);
    }
  }
  // The block has been reset by now, unless it plays silence.
  if (!vpb->use_constant_sample) vpb->reset_vpb = 0;
  store_vpb(voice_id, vpb);
}

// The frame goes to the game's two buffers, left and right, and they move on a frame.
static void finalize_frame(void) {
  apply_volume_in_place(r.front_left, SAMPLES, r.output_volume, 4);
  apply_volume_in_place(r.front_right, SAMPLES, r.output_volume, 4);
  copy_to_guest(r.output_lbuf_addr, r.front_left, sizeof r.front_left);
  copy_to_guest(r.output_rbuf_addr, r.front_right, sizeof r.front_right);
  r.output_lbuf_addr += sizeof r.front_left;
  r.output_rbuf_addr += sizeof r.front_right;
  apply_reverb(1);
  r.prepared = 0;
}

// The GameCube link's key exchange, which early programs did as command 0C (GBA.cpp).
static void process_gba_crypto(uint32_t address) {
  uint32_t challenge = __builtin_bswap32(rd32(address));  // Read from the GBA, so little-endian.
  uint32_t logo_palette = rd32(address + 4);
  uint32_t logo_speed_32 = rd32(address + 8);
  uint32_t length = rd32(address + 12);
  uint32_t dest_addr = rd32(address + 16);
  uint32_t key = challenge ^ 0x6f646573u;
  wr32(dest_addr, key);

  uint16_t palette_speed_coded;
  int16_t logo_speed = (int8_t)logo_speed_32;
  if (logo_speed < 0) palette_speed_coded = (uint16_t)(((-logo_speed + 2) * 2) | (logo_palette << 4));
  else if (logo_speed == 0) palette_speed_coded = (uint16_t)((logo_palette * 2) | 0x70);
  else palette_speed_coded = (uint16_t)(((logo_speed - 1) * 2) | (logo_palette << 4));

  int32_t length_no_header = (int32_t)(((length + 7) & ~7u) - 0x200);
  uint16_t packet_pair_count = length_no_header < 0 ? 0 : (uint16_t)(length_no_header / 8);
  palette_speed_coded |= (uint16_t)((packet_pair_count & 0x4000) >> 14);

  uint32_t t1 = ((((uint32_t)packet_pair_count << 16) | 0x3f80) & 0x3f80ffffu) * 2;
  t1 += (uint32_t)((int16_t)(int8_t)(t1 >> 8) & packet_pair_count) << 16;
  uint32_t t2 = ((uint32_t)(palette_speed_coded & 0xff) << 16) + (t1 & 0xff0000) + ((t1 >> 8) & 0xffff00);
  uint32_t t3 = (uint32_t)palette_speed_coded << 16 | ((t2 << 8) & 0xff000000u) | (t1 >> 16) | 0x80808080u;
  t3 ^= (t3 & 0x200) != 0 ? 0x6f646573u : 0x6177614bu;  // "sedo" or "Kawa".
  wr32(dest_addr + 4, t3);
}

// The program (ZeldaUCode). Its mail comes in as commands, which it queues, and sync
// mails during a frame, which let it render more voices.
enum { MAIL_WAITING, MAIL_RENDERING, MAIL_WRITING_CMD, MAIL_HALTED };
enum { ACK_STANDARD, ACK_DONE_RENDERING };

static struct {
  uint32_t flags;
  int mail_state;
  uint32_t expected_cmd_mails;
  uint32_t sync_max_voice_id;
  uint16_t sync_voice_skip_flags[256];
  int sync_flags_second_half;
  uint32_t cmd_buffer[64];
  uint32_t read_offset, write_offset, pending_commands_count;
  int cmd_can_execute;
  uint32_t rendering_requested_frames;
  uint16_t rendering_voices_per_frame;
  uint32_t rendering_curr_frame, rendering_curr_voice;
} u;

#define CMD_BUFFER_WORDS (sizeof u.cmd_buffer / sizeof u.cmd_buffer[0])

static int rendering_in_progress(void) { return u.rendering_curr_frame != u.rendering_requested_frames; }

static uint32_t read32(void) {
  if (u.read_offset == u.write_offset) {
    gap_hit(GAP_MAIL, 0x01);  // A command with fewer words than it needs.
    return 0;
  }
  uint32_t res = u.cmd_buffer[u.read_offset];
  u.read_offset = (u.read_offset + 1) % CMD_BUFFER_WORDS;
  return res;
}

static void write32(uint32_t value) {
  u.cmd_buffer[u.write_offset] = value;
  u.write_offset = (u.write_offset + 1) % CMD_BUFFER_WORDS;
}

static void send_command_ack(int ack_type, uint16_t sync_value) {
  if (u.flags & LIGHT_PROTOCOL) {
    // The light programs send their command handler's address instead. Dolphin notes
    // the real one answers differently.
    sync_value = (uint16_t)(2 * ((sync_value >> 8) & 0x7f) + 0x62);
    dsp_push_mail(0x80000000u | sync_value, 0, 0);
    return;
  }
  dsp_push_mail(ack_type == ACK_STANDARD ? DSP_MAIL_SYNC : DSP_MAIL_FRAME_END, 1, 0);
  if (ack_type == ACK_STANDARD) dsp_push_mail(0xf3550000u | sync_value, 0, 0);
}

// Renders voices up to the last the game has unlocked, and the frames asked for.
static void render_frames(void) {
  if (!rendering_in_progress()) {
    gap_hit(GAP_MAIL, 0x02);  // Asked to render with no frame asked for.
    return;
  }
  while (u.rendering_curr_frame < u.rendering_requested_frames) {
    if (u.rendering_curr_voice == 0) prepare_frame();
    uint16_t limit = (uint16_t)(256 << 4);
    uint16_t number_of_voices = u.rendering_voices_per_frame < limit ? u.rendering_voices_per_frame : limit;
    while (u.rendering_curr_voice < number_of_voices) {
      if (u.rendering_curr_voice >= u.sync_max_voice_id) return;  // Until the next sync mail.
      uint16_t flags = u.sync_voice_skip_flags[u.rendering_curr_voice >> 4];
      uint8_t bit = (uint8_t)(0xf - (u.rendering_curr_voice & 0xf));
      if (flags & (1 << bit)) add_voice((uint16_t)u.rendering_curr_voice);
      u.rendering_curr_voice++;
    }
    if (!(u.flags & LIGHT_PROTOCOL)) send_command_ack(ACK_STANDARD, (uint16_t)(0xff00 | u.rendering_curr_frame));
    finalize_frame();
    u.rendering_curr_voice = 0;
    u.sync_max_voice_id = 0;
    u.rendering_curr_frame++;
  }
  if (!(u.flags & LIGHT_PROTOCOL)) {
    send_command_ack(ACK_DONE_RENDERING, 0);
    u.cmd_can_execute = 0;  // Until the game answers.
  } else {
    u.mail_state = MAIL_WAITING;
  }
}

// Timed as the audio stage, as AX is.
static void render_audio(void) {
  uint64_t started = time_base();
  render_frames();
  stage_add(STAGE_AUDIO, time_base() - started);
}

static void run_pending_commands(void) {
  if (rendering_in_progress() || !u.cmd_can_execute) return;
  while (u.pending_commands_count) {
    if (u.read_offset == u.write_offset) {  // Dolphin reads zeros here for ever.
      gap_hit(GAP_MAIL, 0x03);
      u.pending_commands_count = 0;
      return;
    }
    uint32_t cmd_mail = read32();
    if ((cmd_mail & 0x80000000u) == 0) continue;
    uint32_t command = (cmd_mail >> 24) & 0x7f;
    uint32_t sync = cmd_mail >> 16;
    uint32_t extra_data = cmd_mail & 0xffff;
    u.pending_commands_count--;

    switch (command) {
    case 0x00:
    case 0x0a:  // Not nothing in the NTSC menu's program, but unused.
    case 0x0b:  // Not nothing in either menu's program, but unused.
    case 0x0f:
      if ((command == 0x0a && dsp_program_crc() == 0x24b22038u) ||
          (command == 0x0b && (dsp_program_crc() == 0x24b22038u || dsp_program_crc() == 0x6ba3b3eau))) {
        gap_hit(GAP_COMMAND, command);
      }
      send_command_ack(ACK_STANDARD, (uint16_t)sync);
      break;

    case 0x03:  // Nothing, except that the light programs never queue it.
      if (u.flags & LIGHT_PROTOCOL) {
        gap_hit(GAP_COMMAND, command);
        break;
      }
      send_command_ack(ACK_STANDARD, (uint16_t)sync);
      break;

    case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09:
      // These crash the real program, which Dolphin takes as halting.
      gap_hit(GAP_COMMAND, command);
      u.mail_state = MAIL_HALTED;
      return;

    // Set-up: where the voice blocks are, and the program's tables.
    case 0x01: {
      u.rendering_voices_per_frame = (uint16_t)extra_data;
      r.vpb_base_addr = read32();
      uint32_t address = read32();
      memset(r.resampling_coeffs, 0, sizeof r.resampling_coeffs);
      copy_from_guest(r.resampling_coeffs, address, sizeof r.resampling_coeffs);
      memset(r.const_patterns, 0, sizeof r.const_patterns);
      copy_from_guest(r.const_patterns, address + 0x200, sizeof r.const_patterns);
      if (!(u.flags & LIGHT_PROTOCOL)) {  // Only positional mixing uses it.
        memset(r.sine_table, 0, sizeof r.sine_table);
        copy_from_guest(r.sine_table, address + 0x400, sizeof r.sine_table);
      }
      memset(r.afc_coeffs, 0, sizeof r.afc_coeffs);
      copy_from_guest(r.afc_coeffs, read32(), sizeof r.afc_coeffs);
      r.reverb_pb_base_addr = read32();
      send_command_ack(ACK_STANDARD, (uint16_t)sync);
      break;
    }

    // Render: holds the mail until the frames are done, so nothing after it runs yet.
    case 0x02:
      u.rendering_requested_frames = (cmd_mail >> 16) & 0xff;
      r.output_volume = (uint16_t)(cmd_mail & 0xffff);
      r.output_lbuf_addr = read32();
      r.output_rbuf_addr = read32();
      if (u.flags & COMBINED_CMD_0D) {
        // Command 0D's two values, which Dolphin skips.
        read32();
        read32();
        gap_hit("audio.zelda_command_0d", 0x02);
      }
      u.rendering_curr_frame = 0;
      u.rendering_curr_voice = 0;
      if (u.flags & LIGHT_PROTOCOL) {
        send_command_ack(ACK_STANDARD, (uint16_t)u.rendering_requested_frames);
        u.mail_state = MAIL_RENDERING;
      } else {
        render_audio();
      }
      return;

    case 0x0c:
      if (u.flags & SUPPORTS_GBA_CRYPTO) {
        process_gba_crypto(read32());
      } else if (u.flags & WEIRD_CMD_0C) {
        read32();
        read32();
        gap_hit("audio.zelda_command_0c", dsp_program_crc());
      }
      send_command_ack(ACK_STANDARD, (uint16_t)sync);
      break;

    case 0x0d:  // Used by The Wind Waker; nobody knows what for.
      if (u.flags & NO_CMD_0D) {
        send_command_ack(ACK_STANDARD, (uint16_t)sync);
        break;
      }
      read32();
      gap_hit("audio.zelda_command_0d", 0x0d);
      send_command_ack(ACK_STANDARD, (uint16_t)sync);
      break;

    // Wii: where in main memory the "ARAM" samples are.
    case 0x0e:
      if (!(u.flags & NO_ARAM)) gap_hit(GAP_COMMAND, command);
      r.aram_base_addr = read32();
      send_command_ack(ACK_STANDARD, (uint16_t)sync);
      break;

    default:
      gap_hit(GAP_COMMAND, command);
      u.mail_state = MAIL_HALTED;
      return;
    }
  }
}

static void handle_mail_default(uint32_t mail) {
  switch (u.mail_state) {
  case MAIL_WAITING:
    if (mail & 0x80000000u) {
      // The program doesn't check for the CDD1 prefix.
      mail = TASK_MAIL_TO_DSP | (mail & ~TASK_MAIL_MASK);
      switch (mail) {
      case TASK_NEW_PROGRAM:
        u.cmd_can_execute = 1;
        run_pending_commands();
        dsp_expect_new_program();
        u.mail_state = MAIL_WAITING;
        break;
      case TASK_RESET:
        u.mail_state = MAIL_HALTED;
        dsp_set_program(PROGRAM_ROM);
        break;
      case TASK_CONTINUE:
        u.cmd_can_execute = 1;
        run_pending_commands();
        break;
      case TASK_RESUME:  // The program halts.
        u.mail_state = MAIL_HALTED;
        break;
      default:  // Halts, as for TASK_RESUME.
        gap_hit(GAP_MAIL, 0x04);
        u.mail_state = MAIL_HALTED;
        break;
      }
    } else if (!(mail & 0xffff)) {
      if (rendering_in_progress()) {
        u.mail_state = MAIL_RENDERING;
      } else {
        gap_hit(GAP_MAIL, 0x05);  // A sync mail with no frame being rendered: halts.
        u.mail_state = MAIL_HALTED;
      }
    } else {
      u.mail_state = MAIL_WRITING_CMD;
      u.expected_cmd_mails = mail & 0xffff;
    }
    break;

  case MAIL_RENDERING:
    if (u.flags & SYNC_PER_FRAME) {
      int base = u.sync_flags_second_half ? 2 : 0;
      u.sync_voice_skip_flags[base] = (uint16_t)(mail >> 16);
      u.sync_voice_skip_flags[base + 1] = (uint16_t)(mail & 0xffff);
      if (u.sync_flags_second_half) u.sync_max_voice_id = 0xffff;
      render_audio();
      if (u.sync_flags_second_half) u.mail_state = MAIL_WAITING;
      u.sync_flags_second_half = !u.sync_flags_second_half;
    } else {
      u.sync_max_voice_id = (((mail >> 16) & 0xf) + 1) << 4;
      u.sync_voice_skip_flags[(mail >> 16) & 0xff] = (uint16_t)(mail & 0xffff);
      render_audio();
      u.mail_state = MAIL_WAITING;
    }
    break;

  case MAIL_WRITING_CMD:
    write32(mail);
    if (--u.expected_cmd_mails == 0) {
      u.pending_commands_count += 1;
      u.mail_state = MAIL_WAITING;
      run_pending_commands();
    }
    break;

  case MAIL_HALTED:
    gap_hit(GAP_MAIL, 0x06);
    break;
  }
}

static void handle_mail_light(uint32_t mail) {
  int add_command = 1;
  switch (u.mail_state) {
  case MAIL_WAITING:
    if ((mail & 0x80000000u) == 0) gap_hit(GAP_MAIL, 0x07);
    // The light programs' commands have a fixed length each.
    write32(mail);
    switch ((mail >> 24) & 0x7f) {
    case 0x00: u.expected_cmd_mails = 0; break;
    case 0x01: u.expected_cmd_mails = 4; break;
    case 0x02: u.expected_cmd_mails = 2; break;
    case 0x03: add_command = 0; break;  // Straight back to waiting, but left in the queue, as in Dolphin.
    case 0x0c:
      if (u.flags & SUPPORTS_GBA_CRYPTO) u.expected_cmd_mails = 1;
      else if (u.flags & WEIRD_CMD_0C) u.expected_cmd_mails = 2;
      else u.expected_cmd_mails = 0;
      break;
    default: gap_hit(GAP_COMMAND, 0x100 | ((mail >> 24) & 0x7f)); break;
    }
    if (u.expected_cmd_mails) {
      u.mail_state = MAIL_WRITING_CMD;
    } else if (add_command) {
      u.pending_commands_count += 1;
      run_pending_commands();
    }
    break;

  case MAIL_WRITING_CMD:
    write32(mail);
    if (--u.expected_cmd_mails == 0) {
      u.pending_commands_count += 1;
      u.mail_state = MAIL_WAITING;
      run_pending_commands();
    }
    break;

  case MAIL_RENDERING:
    if (mail != 0) gap_hit(GAP_MAIL, 0x08);
    // No per-voice syncing in the light protocol.
    u.sync_max_voice_id = 0xffffffffu;
    for (int i = 0; i < 256; i++) u.sync_voice_skip_flags[i] = 0xffff;
    render_audio();
    dsp_mail_interrupt(0);
    break;

  case MAIL_HALTED:
    gap_hit(GAP_MAIL, 0x06);
    break;
  }
}

static void zelda_start(void) {
  uint32_t crc = dsp_program_crc();
  memset(&u, 0, sizeof u);
  memset(&r, 0, sizeof r);
  u.cmd_can_execute = 1;
  int found = 0;
  for (size_t i = 0; i < sizeof ucode_flags / sizeof ucode_flags[0]; i++) {
    if (ucode_flags[i].crc == crc) {
      u.flags = ucode_flags[i].flags;
      found = 1;
    }
  }
  if (!found) gap_hit(GAP_ZELDA, crc);  // Dolphin stops here; we carry on with no flags.
  r.flags = u.flags;
  if (crc == 0x6ca33a6du) gap_hit("audio.zelda_frame_filter", crc);  // Extra filtering Dolphin doesn't do.

  if (u.flags & LIGHT_PROTOCOL) {
    dsp_push_mail(0x88881111u, 0, 0);
  } else {
    dsp_push_mail(DSP_MAIL_INIT, 1, 0);
    dsp_push_mail(0xf3551111u, 0, 0);  // Handshake.
  }
}

static void zelda_mail(uint32_t mail) {
  if (u.flags & LIGHT_PROTOCOL) handle_mail_light(mail);
  else handle_mail_default(mail);
}

static void zelda_update(void) {
  if (dsp_needs_resume_mail()) dsp_push_mail(DSP_MAIL_RESUME, 1, 0);
}

const DspProgram zelda_program = {"Zelda family", zelda_start, zelda_mail, zelda_update};
