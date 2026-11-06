// Sound timing and delivery. The audio DMA reads the game's sound from memory, 32 bytes
// for every 8 samples' time, as Dolphin's DSP.cpp does (UpdateAudioDMA), and interrupts
// when its blocks run out. The game answers by having the audio processor (dsp.c) mix
// the next buffer, so whatever program made the sound, it's heard the same way. The
// samples then wait in a ring for the output thread, and neither side ever waits for
// the other: a full ring drops samples, and an empty one plays silence.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "cpu.h"
#include "debug.h"
#include "gaps.h"
#include "hardware.h"

#define TIME_BASE_HZ 60750000ull
#define BLOCK_BYTES 32u
#define BLOCK_SAMPLES 8u  // Stereo 16-bit samples in a block.
// Blocks only move between the game's slow steps, such as waiting on the graphics
// card, so they're made up right after. Past this, that time is let go.
#define MAX_BLOCKS_BEHIND 1200u  // 300 ms.
// Dolphin waits 200 processor cycles before the first interrupt of a new DMA: some
// games crash if it comes sooner (DSP.cpp, AUDIO_DMA_CONTROL_LEN).
#define START_INTERRUPT_TICKS 17u
// The registers, as Dolphin's DSP.cpp lays them out. The start address keeps 29 bits on
// a Wii, 32-byte aligned; the control register has the enable bit over the block count.
#define DMA_START_HIGH 0xcc005030u
#define DMA_START_LOW 0xcc005032u
#define DMA_CONTROL 0xcc005036u
#define DMA_BLOCKS_LEFT 0xcc00503au
#define DMA_ENABLE 0x8000u
#define DMA_BLOCKS 0x7fffu
#define AI_CONTROL 0xcc006c00u
#define AI_DMA_32KHZ 0x40u  // AIDFR: clear means 48 kHz.

#define RING_SAMPLES 16384u  // A power of two, so positions can wrap freely.
#define START_TARGET_SAMPLES 2400u  // 75 ms: what the ring holds to ride out a slow step.
#define TARGET_STEP_SAMPLES 800u  // Each time the ring runs dry, the target rises by 25 ms...
#define MAX_TARGET_SAMPLES 6400u  // ...up to 200 ms, to stay close to the picture.
#define DRIFT_MARGIN 480u  // Past this distance from the target, nudge the position.

#define WAVE_HEADER_BYTES 44

static uint32_t dma_start, dma_control, dma_address, dma_left;
static uint64_t dma_clock_start, dma_blocks_done, start_interrupt_at;
static int16_t ring[RING_SAMPLES * AUDIO_CHANNELS];
static _Atomic uint32_t written, played;
static uint32_t target = START_TARGET_SAMPLES;  // These two are the output side's.
static int refilling = 1;
static FILE *wave;
static uint32_t wave_bytes;

static void put_le(uint8_t *to, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; i++) to[i] = (uint8_t)(value >> (i * 8));
}

static void write_wave_header(void) {
  uint8_t h[WAVE_HEADER_BYTES] = "RIFF....WAVEfmt ";
  put_le(h + 4, wave_bytes + WAVE_HEADER_BYTES - 8, 4);
  put_le(h + 16, 16, 4);
  put_le(h + 20, 1, 2);
  put_le(h + 22, AUDIO_CHANNELS, 2);
  put_le(h + 24, AUDIO_SAMPLE_RATE, 4);
  put_le(h + 28, AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * sizeof(int16_t), 4);
  put_le(h + 32, AUDIO_CHANNELS * sizeof(int16_t), 2);
  put_le(h + 34, 16, 2);
  memcpy(h + 36, "data", 4);
  put_le(h + 40, wave_bytes, 4);
  rewind(wave);
  fwrite(h, 1, sizeof h, wave);
  fseek(wave, 0, SEEK_END);
}

// Set WII_RECOMP_AUDIO_WAV=<path> to keep what the speakers get, to listen to or measure.
// It's what Dolphin's DumpAudio keeps as dspdump.wav, silence included.
static void save_samples(const int16_t *samples, uint32_t count) {
  uint32_t bytes = count * AUDIO_CHANNELS * sizeof(int16_t);
  fwrite(samples, 1, bytes, wave);  // Both this Mac and the Pi are little-endian.
  wave_bytes += bytes;
  write_wave_header();
}

void audio_start(void) {
  const char *path = getenv("WII_RECOMP_AUDIO_WAV");
  if (path) wave = fopen(path, "wb");
  if (wave) write_wave_header();
  if (getenv("WII_RECOMP_MUTE")) return;
  if (!audio_output_open()) fprintf(stderr, "no sound output: the game carries on silently\n");
}

static void push_samples(const int16_t *samples, uint32_t count) {
  uint32_t position = atomic_load_explicit(&written, memory_order_relaxed);
  if (position - atomic_load_explicit(&played, memory_order_acquire) + count > RING_SAMPLES) return;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t at = (position + i) % RING_SAMPLES * AUDIO_CHANNELS;
    ring[at] = samples[i * AUDIO_CHANNELS];
    ring[at + 1] = samples[i * AUDIO_CHANNELS + 1];
  }
  atomic_store_explicit(&written, position + count, memory_order_release);
}

// The output device's clock drifts from ours. Skipping or repeating one sample per
// request keeps the ring near its target, too slightly to hear.
void audio_pull(int16_t *output, uint32_t samples) {
  uint32_t position = atomic_load_explicit(&played, memory_order_relaxed);
  uint32_t waiting = atomic_load_explicit(&written, memory_order_acquire) - position;
  uint32_t done = 0;
  if (refilling && waiting >= target) refilling = 0;
  if (!refilling) {
    if (waiting > target + DRIFT_MARGIN) { position++; waiting--; }
    else if (waiting < target - DRIFT_MARGIN && waiting > 0) { position--; waiting++; }
    for (; done < samples && waiting; done++, waiting--, position++) {
      uint32_t at = position % RING_SAMPLES * AUDIO_CHANNELS;
      output[done * AUDIO_CHANNELS] = ring[at];
      output[done * AUDIO_CHANNELS + 1] = ring[at + 1];
    }
    atomic_store_explicit(&played, position, memory_order_release);
  }
  if (done < samples) {
    if (!refilling && target < MAX_TARGET_SAMPLES) target += TARGET_STEP_SAMPLES;
    refilling = 1;
    memset(output + done * AUDIO_CHANNELS, 0, (samples - done) * AUDIO_CHANNELS * sizeof(int16_t));
  }
}

static uint32_t blocks_per_second(void) {
  return (hw_peek(AI_CONTROL, 4) & AI_DMA_32KHZ ? 32000u : 48000u) / BLOCK_SAMPLES;
}

static uint64_t block_time(uint64_t block) { return dma_clock_start + block * TIME_BASE_HZ / blocks_per_second(); }

// A new pass over the buffer waits for the game to answer the last interrupt, which is
// when it mixes the next buffer. On a Wii the game always answers in time, but here a
// slow step can leave it behind, and making up the time would play the same few
// milliseconds over and over, which sounds like a buzz. Silence is better.
static int waiting_for_game(void) {
  return (dma_control & DMA_ENABLE) && dma_left == (dma_control & DMA_BLOCKS) && audio_dma_interrupt_unanswered();
}

// One block: 32 bytes from memory, or silence while the DMA is off. In memory each
// sample is big-endian, right channel first. The count runs down, and at zero the
// DMA starts over from the start address with the count written last, and interrupts.
static void play_block(void) {
  int16_t samples[BLOCK_SAMPLES * AUDIO_CHANNELS] = {0};
  if (dma_control & DMA_ENABLE) {
    uint8_t *from = host(dma_address), *last = host(dma_address + BLOCK_BYTES - 1);
    if (from && last == from + BLOCK_BYTES - 1) {
      for (uint32_t i = 0; i < BLOCK_SAMPLES; i++) {
        samples[i * 2] = (int16_t)(from[i * 4 + 2] << 8 | from[i * 4 + 3]);
        samples[i * 2 + 1] = (int16_t)(from[i * 4] << 8 | from[i * 4 + 1]);
      }
    } else {
      gap_hit("hw.audio_dma_start_length", dma_address);
    }
    if (dma_left) {
      dma_left--;
      dma_address += BLOCK_BYTES;
    }
    if (!dma_left) {
      dma_address = dma_start;
      dma_left = dma_control & DMA_BLOCKS;
      audio_dma_interrupt();
    }
  }
  push_samples(samples, BLOCK_SAMPLES);
  if (wave) save_samples(samples, BLOCK_SAMPLES);
}

// Real time sets the pace. Blocks are worked out from a fixed start, so their times
// don't drift, and the DMA's first interrupt waits its moment.
void audio_update(uint64_t now) {
  if (!dma_clock_start) dma_clock_start = now;
  if (start_interrupt_at && now >= start_interrupt_at) {
    start_interrupt_at = 0;
    audio_dma_interrupt();
  }
  if ((dma_control & DMA_ENABLE) && !(hw_peek(AI_CONTROL, 4) & AI_DMA_32KHZ)) gap_hit("hw.control", 48000);  // Played as 32 kHz.
  if (block_time(dma_blocks_done + MAX_BLOCKS_BEHIND) < now) {
    dma_clock_start = now;
    dma_blocks_done = 0;
  }
  while (block_time(dma_blocks_done + 1) <= now) {
    if (waiting_for_game()) {  // The clock goes on without it.
      dma_clock_start = now;
      dma_blocks_done = 0;
      break;
    }
    dma_blocks_done++;
    play_block();
  }
}

// When the next interrupt is due, for the game's idle sleep, or 0 if none is.
uint64_t audio_next_due(void) {
  if (start_interrupt_at) return start_interrupt_at;
  if (!(dma_control & DMA_ENABLE)) return 0;
  return block_time(dma_blocks_done + (dma_left ? dma_left : 1));
}

void audio_dma_reset(void) {
  dma_control = 0;
  start_interrupt_at = 0;
}

// Turning the DMA on loads its address and count at once; while it runs, new values
// wait for the current blocks to run out.
int audio_dma_write(uint32_t address, uint32_t value, int bytes) {
  if (address == DMA_START_HIGH && bytes == 4) {
    dma_start = value & 0x1fffffe0u;
  } else if (address == DMA_START_HIGH) {
    dma_start = (dma_start & 0xffffu) | (value & 0x1fffu) << 16;
  } else if (address == DMA_START_LOW) {
    dma_start = (dma_start & 0xffff0000u) | (value & 0xffe0u);
  } else if (address == DMA_CONTROL) {
    int was_on = (dma_control & DMA_ENABLE) != 0;
    dma_control = value & 0xffffu;
    if (!was_on && (dma_control & DMA_ENABLE)) {
      dma_address = dma_start;
      dma_left = dma_control & DMA_BLOCKS;
      start_interrupt_at = time_base() + START_INTERRUPT_TICKS;
    }
  } else if (address == DMA_BLOCKS_LEFT) {
    gap_hit("hw.audio_dma_blocks_left", value);  // Read only.
  } else {
    return 0;
  }
  return 1;
}

// The count left is zero-based, as Dolphin reads it: some games wait for it to reach 0.
int audio_dma_read(uint32_t address, int bytes, uint32_t *value) {
  if (address == DMA_START_HIGH) *value = bytes == 4 ? dma_start : dma_start >> 16;
  else if (address == DMA_START_LOW) *value = dma_start & 0xffffu;
  else if (address == DMA_CONTROL) *value = dma_control;
  else if (address == DMA_BLOCKS_LEFT) *value = dma_left ? dma_left - 1 : 0;
  else return 0;
  return 1;
}
