// Hardware registers. Plain registers keep what was written; blocks with real
// behaviour claim their own addresses first. Each address is logged once.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "cpu.h"
#include "gaps.h"
#include "hardware.h"
#include "ios.h"

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
#define DSP_CONTROL 0xcc00500au
#define DSP_RESET_BITS 0x0801u
#define DSP_AUDIO_FLAG 0x0008u
#define DSP_ACCELERATOR_FLAG 0x0020u  // The accelerator DMA (ARAM DMA) finished.
#define DSP_MAIL_FLAG 0x0080u
#define DSP_FLAGS (DSP_AUDIO_FLAG | DSP_ACCELERATOR_FLAG | DSP_MAIL_FLAG)
// The accelerator DMA, as Dolphin's DSP.cpp lays it out. On a Wii its far side is MEM2.
// Writing the count's low half starts it, and the top bit of the count is the direction.
#define AR_DMA_MAIN_ADDRESS 0xcc005020u
#define AR_DMA_FAR_ADDRESS 0xcc005024u
#define AR_DMA_COUNT 0xcc005028u
#define AR_DMA_COUNT_LOW 0xcc00502au
#define AR_DMA_TO_MAIN 0x80000000u
#define AR_DMA_ADDRESS_BITS 0x03ffffe0u  // 32-byte aligned, mirrored every 64 MB.
#define MEM2_START 0x90000000u
#define AUDIO_DMA_CONTROL 0xcc005036u
#define AUDIO_DMA_ENABLE 0x8000u
#define AUDIO_DMA_BLOCKS_LEFT 0xcc00503au
#define AUDIO_DMA_BLOCKS 0x7fffu
#define AUDIO_BLOCKS_PER_SECOND 4000u  // 32 bytes each: 8 stereo samples at 32 kHz.
#define TIME_BASE_TICKS_PER_SECOND 60750000ull
#define PI_HARDWARE_REVISION 0xcc00302cu
#define HARDWARE_REVISION 0x246500b1u  // Flipper revision C, as Dolphin answers.
#define TIME_BASE_TICKS_PER_AUDIO_SAMPLE 1266u  // 48 kHz.
// The audio interface, as Dolphin's AudioInterface.cpp reads it. The interrupt is raised
// when the sample counter, while playing, reaches the interrupt timing register.
#define AI_CONTROL 0xcc006c00u
#define AI_SAMPLE_COUNT 0xcc006c08u
#define AI_INTERRUPT_TIMING 0xcc006c0cu
#define AI_PLAYING 0x01u
#define AI_INTERRUPT_ENABLE 0x04u
#define AI_INTERRUPT_FLAG 0x08u
#define AI_COUNTER_RESET 0x20u
// The reset button, as Dolphin's ProcessorInterface.cpp models it: pressing it raises the
// reset switch interrupt, and the cause register holds its state (set while it's up).
// A press lasts half a second.
#define PI_RESET_BUTTON_UP 0x00010000u
#define RESET_PRESS_TICKS (TIME_BASE_TICKS_PER_SECOND / 2)
// VI clock select and DTV status. Clock select only times the video signal, which the
// host window replaces. DTV status says which cable is in: bit 0 a component cable, which
// is what lets a game offer progressive scan, and bit 1 a Japanese console, as Dolphin
// sets them (VideoInterface.cpp).
#define VI_DTV_STATUS 0xcc00206eu
#define DTV_NTSC_J 0x0002u

static uint8_t *registers;  // Big-endian, as the game sees them, so any width works.
static uint8_t *seen;
static uint32_t cause;
static uint32_t audio_counter_start, audio_counter_last;  // In 48 kHz samples since the time base began.
static atomic_int reset_pressed;  // From the control pipe's thread.
static uint64_t reset_up_at;

static uint32_t offset_of(uint32_t address) { return address & (REGISTER_SPACE - 1); }

// Registers seen at both 0xCC and 0xCD.
static int is_register(uint32_t address, uint32_t wanted) { return (address | 0x01000000u) == (wanted | 0x01000000u); }

static uint32_t get(uint32_t address, int bytes) {
  uint32_t value = 0;
  for (int i = 0; i < bytes; i++) value = (value << 8) | registers[offset_of(address + i)];
  return value;
}

static void put(uint32_t address, uint32_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; i--, value >>= 8) registers[offset_of(address + i)] = (uint8_t)value;
}

static int first_touch(const char *kind, uint32_t address, uint32_t value) {
  uint32_t index = offset_of(address);
  if (seen[index]) return 0;
  seen[index] = 1;
  fprintf(stderr, "hardware %s %08x = %08x from %08x\n", kind, address, value, cpu.lr);
  return 1;
}

// Registers that only keep their values here, where a Wii would act, by inventory row.
// The PE's z and alpha settings are left out, as every game sets them up and only EFB
// peeks and pokes use them, which are reported themselves. AI volume only scales
// streamed disc audio, which Wii discs don't use.
enum { READS = 1, WRITES = 2 };
static const struct { uint32_t start, end; int on; const char *id; } unmodelled[] = {
  {0xcc001010u, 0xcc001018u, READS, "gx.pe_bounding_box_0x10_0x16"},
  {0xcc001018u, 0xcc001030u, READS, "gx.pe_performance_counters"},
  {0xcc00202cu, 0xcc002030u, READS, "hw.beam_position_0x2c_0x2e"},
  {0xcc005004u, 0xcc005008u, READS, "hw.mail_from_dsp"},
};

// The picture's own memory (EFB) is checked on every access, as its addresses share
// first_touch's map with the registers'. Others only the first time.
static void note_unmodelled(uint32_t address, int on, int first) {
  static int efb_said;
  if ((address & 0x0f000000u) == 0x08000000u) {
    if (!(efb_said & on)) {
      efb_said |= on;
      gap_hit(on == WRITES ? "pixel.efb_poke_cpu_writes" : "pixel.efb_peek_cpu_reads_color_depth", address);
    }
    return;
  }
  if (!first) return;
  for (size_t i = 0; i < sizeof unmodelled / sizeof *unmodelled; i++) {
    if (address >= unmodelled[i].start && address < unmodelled[i].end && (unmodelled[i].on & on)) gap_hit(unmodelled[i].id, address);
  }
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

// The audio DMA and accelerator DMA share the DSP's interrupt line. Each flag has its
// enable bit just above it, as Dolphin's DSP.cpp reads them. The DSP's own mail flag is
// never set, as no DSP runs.
static void update_dsp_interrupt(void) {
  uint32_t control = get(DSP_CONTROL, 2);
  if ((control >> 1) & control & DSP_FLAGS) raise_irq(IRQ_DSP);
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

// One audio DMA sends one frame, a block every quarter millisecond, and the next starts
// when audio.c raises the interrupt. The count is zero-based, as Dolphin reads it
// (HW/DSP.cpp), so it reaches zero before the next frame.
static uint32_t audio_dma_blocks_left(void) {
  uint32_t blocks = get(AUDIO_DMA_CONTROL, 2) & AUDIO_DMA_BLOCKS;
  uint64_t due = audio_next_due(), now = time_base();
  if (!audio_dma_running() || !due || due <= now) return 0;
  uint64_t left = ((due - now) * AUDIO_BLOCKS_PER_SECOND + TIME_BASE_TICKS_PER_SECOND - 1) / TIME_BASE_TICKS_PER_SECOND;
  if (left > blocks) left = blocks;
  return left ? (uint32_t)left - 1 : 0;
}

// Copies between main memory and MEM2 at once, then flags the accelerator interrupt.
static void accelerator_dma(void) {
  uint32_t count = get(AR_DMA_COUNT, 4), bytes = count & AR_DMA_ADDRESS_BITS;
  uint32_t main_address = get(AR_DMA_MAIN_ADDRESS, 4) & AR_DMA_ADDRESS_BITS;
  uint32_t far_address = get(AR_DMA_FAR_ADDRESS, 4) & AR_DMA_ADDRESS_BITS;
  if (bytes) {
    uint8_t *main_memory = host(0x80000000u | main_address), *main_last = host(0x80000000u | (main_address + bytes - 1));
    uint8_t *far = host(MEM2_START | far_address), *far_last = host(MEM2_START | (far_address + bytes - 1));
    if (!main_memory || !far || main_last != main_memory + bytes - 1 || far_last != far + bytes - 1) {
      gap_hit("hw.ar_dma_registers", main_address);  // Outside memory, or across its end.
    } else if (count & AR_DMA_TO_MAIN) {
      memmove(main_memory, far, bytes);
    } else {
      memmove(far, main_memory, bytes);
    }
  }
  put(AR_DMA_MAIN_ADDRESS, main_address + bytes, 4);
  put(AR_DMA_FAR_ADDRESS, far_address + bytes, 4);
  put(AR_DMA_COUNT, count & AR_DMA_TO_MAIN, 4);
  put(DSP_CONTROL, get(DSP_CONTROL, 2) | DSP_ACCELERATOR_FLAG, 2);
  update_dsp_interrupt();
}

static uint32_t audio_samples(void) {
  return (uint32_t)(time_base() / TIME_BASE_TICKS_PER_AUDIO_SAMPLE) - audio_counter_start;
}

static void update_audio_interrupt(void) {
  uint32_t control = get(AI_CONTROL, 4);
  if ((control & AI_INTERRUPT_FLAG) && (control & AI_INTERRUPT_ENABLE)) raise_irq(IRQ_AI);
  else clear_irq(IRQ_AI);
}

// Writing the flag clears it, and the counter reset bit starts the count again.
static void audio_control_write(uint32_t value) {
  uint32_t kept = get(AI_CONTROL, 4) & AI_INTERRUPT_FLAG & ~value;
  if (value & AI_COUNTER_RESET) {
    audio_counter_start += audio_samples();
    audio_counter_last = 0;
  }
  put(AI_CONTROL, (value & ~(AI_INTERRUPT_FLAG | AI_COUNTER_RESET)) | kept, 4);
  update_audio_interrupt();
}

// The counter only moves on while playing, and the interrupt comes as it passes the
// timing register. The test is Dolphin's, so it holds across the counter wrapping.
static void audio_interface_update(void) {
  uint32_t now = audio_samples(), from = audio_counter_last + 1, timing = get(AI_INTERRUPT_TIMING, 4);
  int passed = now != audio_counter_last && timing - from <= now - from;
  audio_counter_last = now;
  if (!passed || !(get(AI_CONTROL, 4) & AI_PLAYING)) return;
  put(AI_CONTROL, get(AI_CONTROL, 4) | AI_INTERRUPT_FLAG, 4);
  update_audio_interrupt();
}

void reset_button_press(void) { atomic_store(&reset_pressed, 1); }

void hardware_update(uint64_t now) {
  audio_interface_update();
  if (atomic_exchange(&reset_pressed, 0)) {
    reset_up_at = now + RESET_PRESS_TICKS;
    raise_irq(IRQ_RESET_SWITCH);
    stm_reset_button();  // The other half of a press on a Wii, as Dolphin sends both.
  }
}

static uint32_t interrupt_cause(void) { return cause | (time_base() >= reset_up_at ? PI_RESET_BUTTON_UP : 0); }

static uint32_t dtv_status(void) {
  static int looked;
  static uint32_t status;
  char region[8];
  if (!looked) {
    looked = 1;
    if (console_setting("GAME", region, sizeof region) && !strcmp(region, "JP")) status |= DTV_NTSC_J;
  }
  return status;
}

uint32_t hw_read(uint32_t address, int bytes) {
  if (in_locked_cache(address)) return locked_cache_read(address, bytes);
  uint32_t value = get(address, bytes);
  if (address == PI_CAUSE) value = interrupt_cause();
  else if (is_register(address, AI_SAMPLE_COUNT)) value = audio_samples();
  else if (is_register(address, VI_DTV_STATUS)) value = dtv_status();
  else if (address == AUDIO_DMA_BLOCKS_LEFT) value = audio_dma_blocks_left();
  else if (address == PI_HARDWARE_REVISION) value = HARDWARE_REVISION;
  else if (!si_read(address, &value) && !exi_read(address, &value) && !ipc_read(address, &value)) graphics_read(address, &value);
  note_unmodelled(address, READS, first_touch("read ", address, value));
  return value;
}

void hw_write(uint32_t address, uint32_t value, int bytes) {
  if (in_locked_cache(address)) {
    locked_cache_write(address, value, bytes);
    return;
  }
  note_unmodelled(address, WRITES, first_touch("write", address, value));
  if (address == PI_CAUSE) {
    cause &= ~value;
    return;
  }
  if (si_write(address, value) || exi_write(address, value) || ipc_write(address, value) || graphics_write(address, value, bytes)) return;
  if (is_register(address, AI_CONTROL)) {
    audio_control_write(value);
    return;
  }
  if (is_register(address, AI_SAMPLE_COUNT)) {
    audio_counter_start = (uint32_t)(time_base() / TIME_BASE_TICKS_PER_AUDIO_SAMPLE) - value;
    audio_counter_last = value;
    return;
  }
  if (address == DSP_CONTROL) {
    // Writing a flag bit acknowledges it, and other bits are kept as they were.
    value = (value & ~(DSP_RESET_BITS | DSP_FLAGS)) | (get(address, 2) & DSP_FLAGS & ~value);
  }
  put(address, value, bytes);
  if (address == DSP_CONTROL) update_dsp_interrupt();
  if (address == AR_DMA_COUNT_LOW || (address == AR_DMA_COUNT && bytes == 4)) accelerator_dma();
  if (address >= VI_DISPLAY_INTERRUPT && address < VI_DISPLAY_INTERRUPT + VI_INTERRUPT_COUNT * 4) {
    update_video_interrupt();
  }
}
