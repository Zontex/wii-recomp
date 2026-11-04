// The work of the audio processor's AX program, in C. The game's own code decides
// what plays: it fills a parameter block for each voice and writes a command list,
// and this reads them, decodes samples from guest memory, and mixes them to stereo.
#include <stdio.h>
#include <stdlib.h>

#include "audio.h"
#include "cpu.h"
#include "gaps.h"

enum { COMMAND_SETUP, COMMAND_ADD, COMMAND_SUBTRACT, COMMAND_ADD_SUBTRACT, COMMAND_VOICES, COMMAND_AUX_A, COMMAND_AUX_B,
       COMMAND_AUX_C, COMMAND_SURROUND_AUX_A, COMMAND_SURROUND_AUX_B, COMMAND_COMPRESSOR, COMMAND_OUTPUT,
       COMMAND_OUTPUT_SURROUND, COMMAND_REMOTES, COMMAND_END };

// A voice's parameter block.
enum {
  PB_NEXT = 0, PB_SOURCE_TYPE = 8, PB_MIXER_CONTROL = 12, PB_RUNNING = 16, PB_STREAM = 18, PB_MIXER = 20,
  PB_DEPOP = 82, PB_VOLUME = 106, PB_VOLUME_DELTA = 108, PB_LOOPING = 110, PB_FORMAT = 112, PB_LOOP_ADDRESS = 114,
  PB_END_ADDRESS = 118, PB_CURRENT_ADDRESS = 122, PB_COEFFICIENTS = 126, PB_PREDICTOR = 160, PB_HISTORY_1 = 162,
  PB_HISTORY_2 = 164, PB_RATIO = 166, PB_FRACTION = 170, PB_PAST_SAMPLES = 172, PB_LOOP_PREDICTOR = 180,
  PB_LOOP_HISTORY_1 = 182, PB_LOOP_HISTORY_2 = 184, PB_FILTER_ON = 186, PB_FILTER_HISTORY = 188, PB_FILTER_A = 190,
  PB_FILTER_B = 192,
};
enum { SOURCE_FILTERED, SOURCE_LINEAR, SOURCE_DIRECT };
enum { FORMAT_ADPCM = 0, FORMAT_PCM16 = 0x0a, FORMAT_PCM8 = 0x19 };

enum { BUS_MAIN, BUS_AUX_A, BUS_AUX_B, BUS_AUX_C, BUS_COUNT };
enum { LEFT, RIGHT, SURROUND, CHANNEL_COUNT };
#define PAST_SAMPLES 4
#define FULL_SCALE 0x10000u  // 1.0 for positions in a block.
#define VOLUME_SHIFT 15
#define SAMPLE_MAX 32767

// The order of the block's 12 volume and step pairs, and of its 12 depop values.
static const struct { int bus, channel, depop; } mix_slots[] = {
    {BUS_MAIN, LEFT, 0}, {BUS_MAIN, RIGHT, 4}, {BUS_AUX_A, LEFT, 1}, {BUS_AUX_A, RIGHT, 5},
    {BUS_AUX_B, LEFT, 2}, {BUS_AUX_B, RIGHT, 6}, {BUS_AUX_C, LEFT, 3}, {BUS_AUX_C, RIGHT, 7},
    {BUS_MAIN, SURROUND, 8}, {BUS_AUX_A, SURROUND, 9}, {BUS_AUX_B, SURROUND, 10}, {BUS_AUX_C, SURROUND, 11},
};
// Mixer control holds five bits per bus: left, right, ramp both, surround, ramp it.
static const int control_shift[BUS_COUNT] = {0, 16, 21, 26};
enum { CONTROL_LEFT = 1, CONTROL_RIGHT = 2, CONTROL_RAMP = 4, CONTROL_SURROUND = 8, CONTROL_SURROUND_RAMP = 16 };

static int32_t buses[BUS_COUNT][CHANNEL_COUNT][AUDIO_FRAME_SAMPLES];
static uint32_t last_output_volume = 0x8000, last_aux_volume[BUS_COUNT];
static uint32_t compressor_position;

typedef struct {
  uint32_t address, loop, end;
  int format, looping, stream, running;
  int predictor, history_1, history_2;
  int16_t coefficients[16];
} Voice;

static inline int clamp16(int value) { return value < -SAMPLE_MAX ? -SAMPLE_MAX : value > SAMPLE_MAX ? SAMPLE_MAX : value; }
static inline uint32_t pair(uint32_t block, uint32_t offset) { return rd16(block + offset) << 16 | rd16(block + offset + 2); }
static inline void set_pair(uint32_t block, uint32_t offset, uint32_t value) {
  wr16(block + offset, value >> 16);
  wr16(block + offset + 2, value);
}

static inline uint32_t sample_byte(uint32_t physical) {
  uint8_t *from = host(physical);
  return from ? *from : 0;
}

// The real chip jumps back to the loop point, restoring the decoder there unless the
// voice is a stream, where the data simply carries on.
static void reach_end(Voice *v, uint32_t block) {
  v->address = v->loop;
  if (!v->looping) {
    v->running = 0;
    return;
  }
  v->predictor = rd16(block + PB_LOOP_PREDICTOR);
  if (!v->stream) {
    v->history_1 = (int16_t)rd16(block + PB_LOOP_HISTORY_1);
    v->history_2 = (int16_t)rd16(block + PB_LOOP_HISTORY_2);
  }
}

static int decode_adpcm(Voice *v) {
  if ((v->address & 15) == 0) {
    v->predictor = sample_byte(v->address >> 1);
    v->address += 2;
  }
  int coefficient = (v->predictor >> 4) & 7;
  int nibble = sample_byte(v->address >> 1);
  nibble = v->address & 1 ? nibble & 15 : nibble >> 4;
  if (nibble >= 8) nibble -= 16;
  int predicted = v->coefficients[coefficient * 2] * v->history_1 + v->coefficients[coefficient * 2 + 1] * v->history_2;
  int value = clamp16((nibble << (v->predictor & 15)) + ((predicted + 0x400) >> 11));
  v->history_2 = v->history_1;
  v->history_1 = value;
  v->address++;
  return value;
}

static int next_sample(Voice *v, uint32_t block) {
  if (!v->running) return 0;
  int value = 0;
  if (v->format == FORMAT_ADPCM) {
    value = decode_adpcm(v);
  } else if (v->format == FORMAT_PCM16) {
    value = (int16_t)(sample_byte(v->address * 2) << 8 | sample_byte(v->address * 2 + 1));
    v->address++;
  } else if (v->format == FORMAT_PCM8) {
    value = (int8_t)sample_byte(v->address) * 256;
    v->address++;
  }
  if (v->address > v->end) reach_end(v, block);
  return value;
}

// Four-point interpolation, standing in for the filter tables in the chip's ROM.
static inline int interpolate(const int16_t *s, uint32_t fraction, int source) {
  float t = (float)fraction / FULL_SCALE;
  if (source == SOURCE_LINEAR) return (int)(s[1] + (s[2] - s[1]) * t);
  float a = 1.5f * (s[1] - s[2]) + 0.5f * (s[3] - s[0]);
  float b = s[0] - 2.5f * s[1] + 2 * s[2] - 0.5f * s[3];
  float c = 0.5f * (s[2] - s[0]);
  return clamp16((int)(((a * t + b) * t + c) * t + s[1]));
}

static void read_source(Voice *v, uint32_t block, int16_t *samples) {
  int source = rd16(block + PB_SOURCE_TYPE);
  if (source == SOURCE_DIRECT) {
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) samples[i] = (int16_t)next_sample(v, block);
    return;
  }
  uint32_t ratio = pair(block, PB_RATIO), fraction = rd16(block + PB_FRACTION);
  int16_t past[PAST_SAMPLES];
  for (int i = 0; i < PAST_SAMPLES; i++) past[i] = (int16_t)rd16(block + PB_PAST_SAMPLES + i * 2);
  for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
    samples[i] = (int16_t)interpolate(past, fraction, source);
    fraction += ratio;
    for (; fraction >= FULL_SCALE; fraction -= FULL_SCALE) {
      past[0] = past[1];
      past[1] = past[2];
      past[2] = past[3];
      past[3] = (int16_t)next_sample(v, block);
    }
  }
  wr16(block + PB_FRACTION, fraction);
  for (int i = 0; i < PAST_SAMPLES; i++) wr16(block + PB_PAST_SAMPLES + i * 2, (uint16_t)past[i]);
}

static void apply_envelope(uint32_t block, int16_t *samples) {
  uint32_t volume = rd16(block + PB_VOLUME);
  int delta = (int16_t)rd16(block + PB_VOLUME_DELTA);
  for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
    samples[i] = (int16_t)clamp16(samples[i] * (int)volume >> VOLUME_SHIFT);
    volume = (volume + delta) & 0xffff;
  }
  wr16(block + PB_VOLUME, volume);
}

static void apply_filter(uint32_t block, int16_t *samples) {
  if (!rd16(block + PB_FILTER_ON)) return;
  int a = rd16(block + PB_FILTER_A), b = rd16(block + PB_FILTER_B);
  int last = (int16_t)rd16(block + PB_FILTER_HISTORY);
  for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
    last = clamp16((a * samples[i] + b * last) >> VOLUME_SHIFT);
    samples[i] = (int16_t)last;
  }
  wr16(block + PB_FILTER_HISTORY, (uint16_t)last);
}

static void mix_voice(uint32_t block, const int16_t *samples) {
  uint32_t control = pair(block, PB_MIXER_CONTROL);
  for (int slot = 0; slot < 12; slot++) {
    int bus = mix_slots[slot].bus, channel = mix_slots[slot].channel;
    uint32_t bits = control >> control_shift[bus];
    int on = channel == LEFT ? bits & CONTROL_LEFT : channel == RIGHT ? bits & CONTROL_RIGHT : bits & CONTROL_SURROUND;
    if (!on) continue;
    int ramp = bits & (channel == SURROUND ? CONTROL_SURROUND_RAMP : CONTROL_RAMP);
    uint32_t entry = block + PB_MIXER + slot * 4;
    uint32_t volume = rd16(entry);
    int delta = ramp ? (int16_t)rd16(entry + 2) : 0;
    int32_t *out = buses[bus][channel];
    int mixed = 0;
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
      mixed = clamp16(samples[i] * (int)volume >> VOLUME_SHIFT);
      out[i] += mixed;
      volume = (volume + delta) & 0xffff;
    }
    wr16(entry, volume);
    // What the voice last contributed: the game fades this out if it stops abruptly.
    wr16(block + PB_DEPOP + mix_slots[slot].depop * 2, (uint16_t)mixed);
  }
}

static void run_voice(uint32_t block) {
  Voice v = {
      .address = pair(block, PB_CURRENT_ADDRESS), .loop = pair(block, PB_LOOP_ADDRESS), .end = pair(block, PB_END_ADDRESS),
      .format = rd16(block + PB_FORMAT), .looping = rd16(block + PB_LOOPING), .stream = rd16(block + PB_STREAM),
      .running = 1, .predictor = rd16(block + PB_PREDICTOR), .history_1 = (int16_t)rd16(block + PB_HISTORY_1),
      .history_2 = (int16_t)rd16(block + PB_HISTORY_2),
  };
  for (int i = 0; i < 16; i++) v.coefficients[i] = (int16_t)rd16(block + PB_COEFFICIENTS + i * 2);
  int16_t samples[AUDIO_FRAME_SAMPLES];
  read_source(&v, block, samples);
  apply_envelope(block, samples);
  apply_filter(block, samples);
  mix_voice(block, samples);
  wr16(block + PB_RUNNING, (uint32_t)v.running);
  set_pair(block, PB_CURRENT_ADDRESS, v.address);
  wr16(block + PB_PREDICTOR, (uint32_t)v.predictor);
  wr16(block + PB_HISTORY_1, (uint16_t)v.history_1);
  wr16(block + PB_HISTORY_2, (uint16_t)v.history_2);
}

static void run_voices(uint32_t block) {
  for (; block; block = pair(block, PB_NEXT)) {
    if (rd16(block + PB_RUNNING) == 1) run_voice(block);
  }
}

// Starts a frame's mix from the leftovers of voices that stopped abruptly, as a
// value and a per-sample step for each of the 12 channels.
static void set_up_buses(uint32_t studio) {
  for (int bus = 0; bus < BUS_COUNT; bus++) {
    for (int channel = 0; channel < CHANNEL_COUNT; channel++, studio += 6) {
      int32_t value = (int32_t)pair(studio, 0);
      int delta = (int16_t)rd16(studio + 4);
      for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) buses[bus][channel][i] = value + i * delta;
    }
  }
}

static void add_to_stereo(uint32_t address, int command) {
  for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
    int32_t first = (int32_t)rd32(address + i * 4);
    int32_t second = command == COMMAND_ADD_SUBTRACT ? (int32_t)rd32(address + (i + AUDIO_FRAME_SAMPLES) * 4) : first;
    buses[BUS_MAIN][LEFT][i] += command == COMMAND_SUBTRACT ? -first : first;
    buses[BUS_MAIN][RIGHT][i] += command == COMMAND_ADD ? second : -second;
  }
}

static inline uint32_t ramped(uint32_t from, uint32_t to, int i) {
  return from + ((int)to - (int)from) * i / AUDIO_FRAME_SAMPLES;
}

// Hands an aux bus to the game's effect, and mixes in what the effect made earlier.
static void run_aux(int bus, uint32_t volume, uint32_t upload, uint32_t download) {
  for (int channel = 0; channel < CHANNEL_COUNT; channel++) {
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
      uint32_t offset = (uint32_t)(channel * AUDIO_FRAME_SAMPLES + i) * 4;
      if (upload) wr32(upload + offset, (uint32_t)buses[bus][channel][i]);
      int64_t sample = (int32_t)rd32(download + offset);
      buses[BUS_MAIN][channel][i] += (int32_t)(sample * ramped(last_aux_volume[bus], volume, i) >> VOLUME_SHIFT);
    }
  }
  last_aux_volume[bus] = volume;
}

// Pulls the mix down over the next frames if it went over the limit, following the
// game's own table: first the attack ramps, one for each step, then the releases.
static void run_compressor(uint32_t threshold, uint32_t frames, uint32_t table) {
  int triggered = 0;
  for (int i = 0; i < AUDIO_FRAME_SAMPLES && !triggered; i++) {
    triggered = (uint32_t)abs(buses[BUS_MAIN][LEFT][i]) > threshold || (uint32_t)abs(buses[BUS_MAIN][RIGHT][i]) > threshold;
  }
  uint32_t ramp;
  if (triggered) {
    ramp = compressor_position;
    compressor_position = frames;
  } else if (compressor_position) {
    ramp = frames + compressor_position--;
  } else {
    return;
  }
  uint32_t from = table + ramp * AUDIO_FRAME_SAMPLES * sizeof(uint16_t);
  for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
    int64_t gain = rd16(from + i * sizeof(uint16_t));
    buses[BUS_MAIN][LEFT][i] = (int32_t)(buses[BUS_MAIN][LEFT][i] * gain >> VOLUME_SHIFT);
    buses[BUS_MAIN][RIGHT][i] = (int32_t)(buses[BUS_MAIN][RIGHT][i] * gain >> VOLUME_SHIFT);
  }
}

// The surround channel goes back to the game's memory, for next frame's mix to fold
// into left and right. The guest's output buffer has right first.
static void write_output(uint32_t volume, uint32_t surround, uint32_t stereo, int16_t *output) {
  for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
    wr32(surround + i * 4, (uint32_t)buses[BUS_MAIN][SURROUND][i]);
    int64_t gain = ramped(last_output_volume, volume, i);
    int left = clamp16((int)(buses[BUS_MAIN][LEFT][i] * gain >> VOLUME_SHIFT));
    int right = clamp16((int)(buses[BUS_MAIN][RIGHT][i] * gain >> VOLUME_SHIFT));
    output[i * AUDIO_CHANNELS] = (int16_t)left;
    output[i * AUDIO_CHANNELS + 1] = (int16_t)right;
    wr16(stereo + i * 4, (uint16_t)right);
    wr16(stereo + i * 4 + 2, (uint16_t)left);
  }
  last_output_volume = volume;
}

void ax_run_list(uint32_t list, int16_t *output) {
  memset(output, 0, AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t));
  for (uint32_t at = list;;) {
    uint32_t command = rd16(at);
    at += 2;
    if (command == COMMAND_END) return;
    switch (command) {
      case COMMAND_SETUP: set_up_buses(pair(at, 0)); at += 4; break;
      case COMMAND_ADD: case COMMAND_SUBTRACT: case COMMAND_ADD_SUBTRACT:
        add_to_stereo(pair(at, 0), (int)command);
        at += 4;
        break;
      case COMMAND_VOICES: run_voices(pair(at, 0)); at += 4; break;
      case COMMAND_AUX_A: case COMMAND_AUX_B: case COMMAND_AUX_C:
        run_aux((int)(command - COMMAND_AUX_A) + BUS_AUX_A, rd16(at), pair(at, 2), pair(at, 6));
        at += 10;
        break;
      case COMMAND_SURROUND_AUX_A: case COMMAND_SURROUND_AUX_B:
        gap_hit("audio.axwii_aux_surround_uploads_commands_8_9", command);
        at += 26;
        break;
      case COMMAND_COMPRESSOR: run_compressor(rd16(at), rd16(at + 2), pair(at, 4)); at += 8; break;
      case COMMAND_OUTPUT: case COMMAND_OUTPUT_SURROUND:
        write_output(rd16(at), pair(at, 2), pair(at, 6), output);
        at += 10;
        break;
      case COMMAND_REMOTES: gap_hit("audio.axwii_wii_remote_speaker_output", command); at += 16; break;
      default:  // Losing a frame of sound beats stopping the game.
        fprintf(stderr, "unknown audio command %x at %08x\n", command, at - 2);
        gap_note("unlisted.audio.ax_command", command);
        return;
    }
  }
}
