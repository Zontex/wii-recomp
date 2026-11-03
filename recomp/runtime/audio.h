// Sound: the game's mixer output on its way to the speakers.
#pragma once
#include <stdint.h>

#define AUDIO_SAMPLE_RATE 32000
#define AUDIO_FRAME_SAMPLES 96  // 3 ms, the game's mixing step.
#define AUDIO_CHANNELS 2

void audio_start(void);
void audio_update(uint64_t now);
uint64_t audio_next_due(void);  // When the next frame is due, or 0 if none is.

// ax.c: runs one frame's command list, leaving interleaved left and right samples.
void ax_run_list(uint32_t list, int16_t *output);

// audio_output_*.c: one per platform. Called once, from audio_start. The output
// thread asks for samples through audio_pull, which never waits.
int audio_output_open(void);
void audio_pull(int16_t *output, uint32_t samples);
