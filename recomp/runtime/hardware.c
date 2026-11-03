// Hardware registers. Plain registers keep what was written; blocks with real
// behaviour claim their own addresses first. Each address is logged once.
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "hardware.h"

#define REGISTER_SPACE 0x01000000u
#define LOCKED_CACHE_START 0xe0000000u
#define LOCKED_CACHE_SIZE 0x4000u
#define SPR_CACHE_DMA_HIGH 922
#define SPR_CACHE_DMA_LOW 923
#define CACHE_DMA_TRIGGER 2u
#define CACHE_DMA_LOAD 0x10u
#define CACHE_DMA_MOST_LINES 128u
#define CACHE_LINE_BYTES 32u
#define PI_CAUSE 0xcc003000u
#define PI_MASK 0xcc003004u
#define VI_DISPLAY_INTERRUPT 0xcc002030u
#define VI_INTERRUPT_COUNT 4
#define VI_INTERRUPT_FLAG 0x80000000u
#define VI_INTERRUPT_ENABLE 0x10000000u
#define EXI_CONTROL_A 0xcd00680cu
#define EXI_CONTROL_B 0xcd006820u
#define EXI_CONTROL_C 0xcd006834u
#define DSP_CONTROL 0xcc00500au
#define DSP_RESET_BITS 0x0801u
#define DSP_AUDIO_FLAG 0x0008u
#define DSP_AUDIO_ENABLE 0x0010u
#define DSP_ACCELERATOR_FLAG 0x0020u
#define DSP_MAIL_FLAG 0x0080u
#define DSP_FLAGS (DSP_AUDIO_FLAG | DSP_ACCELERATOR_FLAG | DSP_MAIL_FLAG)
#define AUDIO_DMA_CONTROL 0xcc005036u
#define AUDIO_DMA_ENABLE 0x8000u
#define AUDIO_SAMPLE_COUNTER 0xcd006c08u
#define TIME_BASE_TICKS_PER_AUDIO_SAMPLE 1266u  // 48 kHz.

static uint8_t *registers;  // Big-endian, as the game sees them, so any width works.
static uint8_t *seen;
static uint32_t cause;

static uint32_t offset_of(uint32_t address) { return address & (REGISTER_SPACE - 1); }

static uint32_t get(uint32_t address, int bytes) {
  uint32_t value = 0;
  for (int i = 0; i < bytes; i++) value = (value << 8) | registers[offset_of(address + i)];
  return value;
}

static void put(uint32_t address, uint32_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; i--, value >>= 8) registers[offset_of(address + i)] = (uint8_t)value;
}

static void first_touch(const char *kind, uint32_t address, uint32_t value) {
  uint32_t index = offset_of(address);
  if (seen[index]) return;
  seen[index] = 1;
  fprintf(stderr, "hardware %s %08x = %08x from %08x\n", kind, address, value, cpu.lr);
}

uint32_t hw_peek(uint32_t address, int bytes) { return get(address, bytes); }
void hw_poke(uint32_t address, uint32_t value, int bytes) { put(address, value, bytes); }

void hardware_start(void) {
  registers = calloc(REGISTER_SPACE, 1);
  seen = calloc(REGISTER_SPACE, 1);
}

void raise_irq(int bit) { cause |= 1u << bit; }
void clear_irq(int bit) { cause &= ~(1u << bit); }
int irq_pending(void) { return (cause & get(PI_MASK, 4)) != 0; }

// The screen finished a frame: flag each enabled display interrupt.
void video_retrace(void) {
  graphics_retrace();
  for (int i = 0; i < VI_INTERRUPT_COUNT; i++) {
    uint32_t address = VI_DISPLAY_INTERRUPT + (uint32_t)i * 4;
    uint32_t value = get(address, 4);
    if (value & VI_INTERRUPT_ENABLE) {
      put(address, value | VI_INTERRUPT_FLAG, 4);
      raise_irq(IRQ_VI);
    }
  }
}

static void update_video_interrupt(void) {
  for (int i = 0; i < VI_INTERRUPT_COUNT; i++) {
    if (get(VI_DISPLAY_INTERRUPT + (uint32_t)i * 4, 4) & VI_INTERRUPT_FLAG) return;
  }
  clear_irq(IRQ_VI);
}

// The audio DMA shares the DSP's interrupt line, and only its flag is ever set.
static void update_dsp_interrupt(void) {
  uint32_t control = get(DSP_CONTROL, 2);
  if ((control & DSP_AUDIO_FLAG) && (control & DSP_AUDIO_ENABLE)) raise_irq(IRQ_DSP);
  else clear_irq(IRQ_DSP);
}

int audio_dma_running(void) { return (get(AUDIO_DMA_CONTROL, 2) & AUDIO_DMA_ENABLE) != 0; }
int audio_dma_interrupt_waiting(void) { return (get(DSP_CONTROL, 2) & DSP_AUDIO_FLAG) != 0; }

void audio_dma_interrupt(void) {
  put(DSP_CONTROL, get(DSP_CONTROL, 2) | DSP_AUDIO_FLAG, 2);
  update_dsp_interrupt();
}

// The processor's locked cache, a small fast memory that Wii Fit never uses. It isn't
// in normal memory, so reaching it through here keeps every other access as quick.
static uint8_t locked_cache[LOCKED_CACHE_SIZE];

static int in_locked_cache(uint32_t address) { return address - LOCKED_CACHE_START < LOCKED_CACHE_SIZE; }

static uint32_t locked_cache_read(uint32_t address, int bytes) {
  uint32_t value = 0;
  for (int i = 0; i < bytes; i++) value = value << 8 | locked_cache[(address + (uint32_t)i - LOCKED_CACHE_START) % LOCKED_CACHE_SIZE];
  return value;
}

static void locked_cache_write(uint32_t address, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; i++) locked_cache[(address + (uint32_t)i - LOCKED_CACHE_START) % LOCKED_CACHE_SIZE] = (uint8_t)(value >> ((bytes - 1 - i) * 8));
}

// Moves whole cache lines between main memory and the locked cache, which video decoding
// relies on. The transfer starts when its low register is written with the trigger bit.
void locked_cache_transfer(void) {
  uint32_t high = cpu.spr[SPR_CACHE_DMA_HIGH], low = cpu.spr[SPR_CACHE_DMA_LOW];
  if (!(low & CACHE_DMA_TRIGGER)) return;
  uint32_t lines = (high & 0x1f) << 2 | (low >> 2) & 3;
  uint32_t memory = (high & ~0x1fu) | 0x80000000u, cache = low & ~0x1fu;
  if (!lines) lines = CACHE_DMA_MOST_LINES;
  for (uint32_t i = 0; i < lines * CACHE_LINE_BYTES; i++) {
    uint32_t at = (cache + i - LOCKED_CACHE_START) % LOCKED_CACHE_SIZE;
    if (low & CACHE_DMA_LOAD) locked_cache[at] = (uint8_t)rd8(memory + i);
    else wr8(memory + i, locked_cache[at]);
  }
  cpu.spr[SPR_CACHE_DMA_LOW] = low & ~(uint32_t)CACHE_DMA_TRIGGER;
}

uint32_t hw_read(uint32_t address, int bytes) {
  if (in_locked_cache(address)) return locked_cache_read(address, bytes);
  uint32_t value = get(address, bytes);
  if (address == PI_CAUSE) value = cause;
  else if (address == AUDIO_SAMPLE_COUNTER) value = (uint32_t)(time_base() / TIME_BASE_TICKS_PER_AUDIO_SAMPLE);
  else if (!si_read(address, &value) && !ipc_read(address, &value)) graphics_read(address, &value);
  first_touch("read ", address, value);
  return value;
}

void hw_write(uint32_t address, uint32_t value, int bytes) {
  if (in_locked_cache(address)) {
    locked_cache_write(address, value, bytes);
    return;
  }
  first_touch("write", address, value);
  if (address == PI_CAUSE) {
    cause &= ~value;
    return;
  }
  if (si_write(address, value) || ipc_write(address, value) || graphics_write(address, value, bytes)) return;
  // These finish at once, so their busy bits clear themselves.
  if (address == EXI_CONTROL_A || address == EXI_CONTROL_B || address == EXI_CONTROL_C) value &= ~1u;
  if (address == DSP_CONTROL) {
    // Writing a flag bit acknowledges it, and other bits are kept as they were.
    value = (value & ~(DSP_RESET_BITS | DSP_FLAGS)) | (get(address, 2) & DSP_FLAGS & ~value);
  }
  put(address, value, bytes);
  if (address == DSP_CONTROL) update_dsp_interrupt();
  if (address >= VI_DISPLAY_INTERRUPT && address < VI_DISPLAY_INTERRUPT + VI_INTERRUPT_COUNT * 4) {
    update_video_interrupt();
  }
}
