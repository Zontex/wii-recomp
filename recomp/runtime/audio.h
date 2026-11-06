// Sound: the game's mixer output on its way to the speakers.
#pragma once
#include <stdint.h>

#define AUDIO_SAMPLE_RATE 32000
#define AUDIO_FRAME_SAMPLES 96  // 3 ms, the game's mixing step.
#define AUDIO_CHANNELS 2

void audio_start(void);
void audio_update(uint64_t now);
uint64_t audio_next_due(void);  // When the next audio interrupt is due, or 0 if none is.

// The audio DMA's registers, which hardware.c passes on, and a reset of the processor.
int audio_dma_read(uint32_t address, int bytes, uint32_t *value);
int audio_dma_write(uint32_t address, uint32_t value, int bytes);
void audio_dma_reset(void);

// audio_output_*.c: one per platform. Called once, from audio_start. The output
// thread asks for samples through audio_pull, which never waits.
int audio_output_open(void);
void audio_pull(int16_t *output, uint32_t samples);
