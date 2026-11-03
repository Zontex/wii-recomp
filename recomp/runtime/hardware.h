// The Wii's hardware blocks, as seen by the game through register addresses.
#pragma once
#include <stdint.h>

enum {
  IRQ_EXI = 4, IRQ_AI = 5, IRQ_DSP = 6, IRQ_VI = 8, IRQ_PE_TOKEN = 9, IRQ_PE_FINISH = 10, IRQ_CP = 11, IRQ_IPC = 14,
};

void hardware_start(void);
void raise_irq(int bit);
void clear_irq(int bit);
int irq_pending(void);
void video_retrace(void);
void audio_dma_interrupt(void);
int audio_dma_running(void);
int audio_dma_interrupt_waiting(void);
uint32_t hw_peek(uint32_t address, int bytes);
void hw_poke(uint32_t address, uint32_t value, int bytes);

// Blocks claim the registers they handle, and return 1 if they did.
int ipc_read(uint32_t address, uint32_t *value);
int ipc_write(uint32_t address, uint32_t value);
void ipc_update(void);
int graphics_write(uint32_t address, uint32_t value, int bytes);
int graphics_read(uint32_t address, uint32_t *value);
void graphics_update(void);
void graphics_retrace(void);
int graphics_busy(void);  // Commands are still waiting to be run.
uint64_t graphics_next_due(void);  // When the next delayed signal is due, or 0 if none is.
