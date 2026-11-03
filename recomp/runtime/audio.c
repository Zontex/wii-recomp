// Sound timing and delivery. The game's own AX library builds a command list every
// 3 ms when the audio DMA interrupt fires, and mails it to the audio processor. That
// mail runs ax.c here, on the game's thread, because voices are only stable between
// frames. Samples then wait in a ring for the output thread, and neither side ever
// waits for the other: a full ring drops a frame, and an empty one plays silence.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio.h"
#include "title.h"
#include "cpu.h"
#include "debug.h"
#include "hardware.h"

#define TIME_BASE_HZ 60750000ull
#define FRAME_TICKS (TIME_BASE_HZ * AUDIO_FRAME_SAMPLES / AUDIO_SAMPLE_RATE)
// Interrupts only reach the game between its slow steps, such as waiting on the
// graphics card, so they're made up right after. Past this, that time is let go.
#define MAX_FRAMES_BEHIND 100

#define MAIL_LIST_FOLLOWS 0xbabeu

#define RING_SAMPLES 16384u  // A power of two, so positions can wrap freely.
#define START_TARGET_SAMPLES 2400u  // 75 ms: what the ring holds to ride out a slow step.
#define TARGET_STEP_SAMPLES 800u  // Each time the ring runs dry, the target rises by 25 ms...
#define MAX_TARGET_SAMPLES 6400u  // ...up to 200 ms, to stay close to the picture.
#define DRIFT_MARGIN 480u  // Past this distance from the target, nudge the position.

#define WAVE_HEADER_BYTES 44

static uint64_t next_frame;
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

// Set WII_RECOMP_AUDIO_WAV=<path> to keep what the mixer made, to listen to or measure.
static void save_frame(const int16_t *frame) {
  uint32_t bytes = AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t);
  fwrite(frame, 1, bytes, wave);  // Both this Mac and the Pi are little-endian.
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

static void push_frame(const int16_t *frame) {
  uint32_t position = atomic_load_explicit(&written, memory_order_relaxed);
  if (position - atomic_load_explicit(&played, memory_order_acquire) + AUDIO_FRAME_SAMPLES > RING_SAMPLES) return;
  for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
    uint32_t at = (position + i) % RING_SAMPLES * AUDIO_CHANNELS;
    ring[at] = frame[i * AUDIO_CHANNELS];
    ring[at + 1] = frame[i * AUDIO_CHANNELS + 1];
  }
  atomic_store_explicit(&written, position + AUDIO_FRAME_SAMPLES, memory_order_release);
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

uint64_t audio_next_due(void) { return next_frame; }

// Real time sets the pace, from the moment the game starts the audio DMA.
void audio_update(uint64_t now) {
  if (!audio_dma_running()) {
    next_frame = 0;
    return;
  }
  if (!next_frame) next_frame = now + FRAME_TICKS;
  if (now < next_frame || audio_dma_interrupt_waiting()) return;
  next_frame += FRAME_TICKS;
  if (now > next_frame + MAX_FRAMES_BEHIND * FRAME_TICKS) next_frame = now + FRAME_TICKS;
  audio_dma_interrupt();
}

// The game mails a header, then the command list's address.
void audio_dsp_mail(uint32_t mail) {
  static int list_follows;
  if (mail >> 16 == MAIL_LIST_FOLLOWS) {
    list_follows = 1;
    return;
  }
  if (!list_follows) return;
  list_follows = 0;
  static int16_t frame[AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS];
  uint64_t started = time_base();
  ax_run_list(mail, frame);
  stage_add(STAGE_AUDIO, time_base() - started);
  push_frame(frame);
  if (wave) save_frame(frame);
  dispatch(title->ax_frame_done);
}
