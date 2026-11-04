// The expansion interface: three channels of a small serial bus. Channel 0's second
// device is the chip with the real-time clock and SRAM, which the system reads as it
// starts. Nothing else is plugged in, so a transfer to any other device finishes at once
// and reads zeros. Register and command layouts are from Dolphin's EXI_Channel.h and
// EXI_DeviceIPL.h, and SRAM's from its Sram.h (GPL-2.0+).
#include <string.h>

#include "cpu.h"
#include "gaps.h"
#include "hardware.h"

enum {
  FIRST = 0x6800, CHANNEL_BYTES = 0x14, CHANNELS = 3, BLOCK_MASK = 0xffffff,
  STATUS = 0x00, DMA_ADDRESS = 0x04, DMA_LENGTH = 0x08, CONTROL = 0x0c, DATA = 0x10,
  // Status: each interrupt sits beside its mask, and writing the interrupt's bit clears it.
  EXI_MASK = 1u << 0, EXI_INTERRUPT = 1u << 1, DONE_MASK = 1u << 2, DONE = 1u << 3,
  SELECT_SHIFT = 7, SELECT_BITS = 7, INSERT_MASK = 1u << 10, INSERT_INTERRUPT = 1u << 11,
  INTERRUPTS = EXI_INTERRUPT | DONE | INSERT_INTERRUPT,
  // Control.
  START = 1u << 0, USES_DMA = 1u << 1, DIRECTION_SHIFT = 2, LENGTH_SHIFT = 4,
  READ = 0, WRITE = 1, READ_WRITE = 2,
  SELECT_CLOCK_CHIP = 2,  // On channel 0. 1 is memory card slot A there.
  // The clock chip's command: bit 31 writes, and the address is bits 6 to 30.
  COMMAND_WRITE = 1u << 31, ADDRESS_SHIFT = 6, ADDRESS_MASK = 0x1ffffff, COMMAND_BYTES = 4,
  ROM_END = 0x200000, SRAM_BASE = 0x800000, UART_BASE = 0x800400, WII_RTC_FLAGS = 0x840020,
  // SRAM: the clock, then the settings, whose checksum covers bytes 16 to 23.
  SRAM_BYTES = 0x44, SRAM_CLOCK_BYTES = 4, SRAM_CHECKSUM = 4, SUMMED_FIRST = 16, SUMMED_END = 24,
  SRAM_FLAGS = 23, SRAM_CARD_CHECKSUMS = 62,
  FLAG_STEREO = 1 << 2, FLAG_SET_UP = 1 << 3, FLAG_ALWAYS_SET = 1 << 5,
};
#define TICKS_PER_SECOND 60750000ull

typedef struct {
  uint32_t status, dma_address, dma_length, control, data;
} Channel;

static Channel channels[CHANNELS];

static struct {
  uint32_t command, received, cursor, rtc_flags, written_clock;
  int64_t clock_offset;  // Seconds the game moved the clock by.
  uint8_t sram[SRAM_BYTES];
  int ready;
} chip;

// Settings as a new Wii's: stereo, set up, English, no offset, and no memory card IDs.
static void sram_start(void) {
  memset(chip.sram, 0, sizeof chip.sram);
  chip.sram[SRAM_FLAGS] = FLAG_ALWAYS_SET | FLAG_SET_UP | FLAG_STEREO;
  chip.sram[SRAM_CARD_CHECKSUMS] = chip.sram[SRAM_CARD_CHECKSUMS + 1] = 0xff;  // Of IDs that are all zero.
  chip.ready = 1;
}

// A 16-bit sum and a sum of complements, each over the big-endian words that count.
static void sram_checksum(void) {
  uint16_t sum = 0, complement = 0;
  for (int i = SUMMED_FIRST; i < SUMMED_END; i += 2) {
    uint16_t word = (uint16_t)(chip.sram[i] << 8 | chip.sram[i + 1]);
    sum = (uint16_t)(sum + word);
    complement = (uint16_t)(complement + (uint16_t)~word);
  }
  chip.sram[SRAM_CHECKSUM] = (uint8_t)(sum >> 8);
  chip.sram[SRAM_CHECKSUM + 1] = (uint8_t)sum;
  chip.sram[SRAM_CHECKSUM + 2] = (uint8_t)(complement >> 8);
  chip.sram[SRAM_CHECKSUM + 3] = (uint8_t)complement;
}

// Seconds since 2000 in local time, the same clock the time base starts from.
static uint32_t clock_now(void) { return (uint32_t)((int64_t)(time_base() / TICKS_PER_SECOND) + chip.clock_offset); }

// One byte each way. The command comes first, then bytes from where it points.
static uint8_t clock_chip_byte(uint8_t in) {
  if (!chip.ready) sram_start();
  if (chip.received < COMMAND_BYTES) {
    chip.command = chip.command << 8 | in;
    if (++chip.received == COMMAND_BYTES) {
      uint32_t now = clock_now();
      for (int i = 0; i < SRAM_CLOCK_BYTES; i++) chip.sram[i] = (uint8_t)(now >> (24 - i * 8));
      sram_checksum();
    }
    return 0xff;
  }
  uint32_t address = (chip.command >> ADDRESS_SHIFT) & ADDRESS_MASK;
  int write = (chip.command & COMMAND_WRITE) != 0;
  if (address >= SRAM_BASE && address < UART_BASE) {
    uint32_t at = (address - SRAM_BASE + chip.cursor++) % SRAM_BYTES;
    if (!write) return chip.sram[at];
    chip.sram[at] = in;
    if (at < SRAM_CLOCK_BYTES) {  // Setting the clock moves it from now on.
      chip.written_clock = chip.written_clock << 8 | in;
      if (at == SRAM_CLOCK_BYTES - 1) chip.clock_offset += (int64_t)chip.written_clock - (int64_t)clock_now();
    }
    return 0;
  }
  if (address == WII_RTC_FLAGS) {
    int shift = 24 - (int)(chip.cursor++ % 4) * 8;
    if (!write) return (uint8_t)(chip.rtc_flags >> shift);
    chip.rtc_flags = (chip.rtc_flags & ~(0xffu << shift)) | (uint32_t)in << shift;
    return 0;
  }
  if (address < ROM_END && !write) gap_hit("unlisted.hw.ipl_rom_fonts", address);
  return 0;  // The debug UART, whose output nothing needs.
}

static int selected(int channel) { return (int)(channels[channel].status >> SELECT_SHIFT) & SELECT_BITS; }

static uint8_t transfer_byte(int channel, uint8_t in) {
  if (channel == 0 && selected(0) == SELECT_CLOCK_CHIP) return clock_chip_byte(in);
  return 0;
}

static void update_interrupt(void) {
  int raised = 0;
  for (int i = 0; i < CHANNELS; i++) {
    uint32_t s = channels[i].status;
    raised |= ((s & EXI_INTERRUPT) && (s & EXI_MASK)) || ((s & DONE) && (s & DONE_MASK)) || ((s & INSERT_INTERRUPT) && (s & INSERT_MASK));
  }
  if (raised) raise_irq(IRQ_EXI); else clear_irq(IRQ_EXI);
}

static void transfer(int channel) {
  Channel *c = &channels[channel];
  int direction = (int)(c->control >> DIRECTION_SHIFT) & 3, device = selected(channel);
  // A game asking for a memory card wanted one. Channel 0's third device and channel 2's
  // are debugging hardware a retail Wii doesn't have, so finding nothing there is right.
  if ((channel == 0 && device == 1) || (channel == 1 && device)) gap_hit("hw.gc_memory_card", (uint32_t)channel);
  if (c->control & USES_DMA) {
    uint32_t at = c->dma_address | 0x80000000u;
    for (uint32_t i = 0; i < c->dma_length; i++) {
      if (direction == WRITE) transfer_byte(channel, (uint8_t)rd8(at + i));
      else wr8(at + i, transfer_byte(channel, 0));
    }
  } else {
    int bytes = (int)((c->control >> LENGTH_SHIFT) & 3) + 1;
    uint32_t received = 0;
    for (int i = 0; i < bytes; i++) {
      uint8_t out = direction == READ ? 0 : (uint8_t)(c->data >> (24 - i * 8));
      received |= (uint32_t)transfer_byte(channel, out) << (24 - i * 8);
    }
    if (direction != WRITE) c->data = received;
  }
  c->control &= ~START;
  c->status |= DONE;
  update_interrupt();
}

static Channel *channel_at(uint32_t address, uint32_t *offset) {
  uint32_t at = (address & BLOCK_MASK) - FIRST;
  if ((address & BLOCK_MASK) < FIRST || at >= CHANNELS * CHANNEL_BYTES) return 0;
  *offset = at % CHANNEL_BYTES;
  return &channels[at / CHANNEL_BYTES];
}

int exi_read(uint32_t address, uint32_t *value) {
  uint32_t offset;
  Channel *c = channel_at(address, &offset);
  if (!c) return 0;
  if (offset == STATUS) *value = c->status;
  else if (offset == DMA_ADDRESS) *value = c->dma_address;
  else if (offset == DMA_LENGTH) *value = c->dma_length;
  else if (offset == CONTROL) *value = c->control;
  else *value = c->data;
  return 1;
}

int exi_write(uint32_t address, uint32_t value) {
  uint32_t offset;
  Channel *c = channel_at(address, &offset);
  if (!c) return 0;
  int channel = (int)(c - channels);
  if (offset == STATUS) {
    int before = selected(channel);
    uint32_t kept = c->status & INTERRUPTS & ~(value & INTERRUPTS);
    c->status = (value & ~INTERRUPTS) | kept;
    // Selecting the clock chip starts a new command.
    if (channel == 0 && selected(0) == SELECT_CLOCK_CHIP && before != SELECT_CLOCK_CHIP) chip.received = chip.cursor = 0;
    update_interrupt();
  } else if (offset == DMA_ADDRESS) c->dma_address = value & 0x03ffffe0u;
  else if (offset == DMA_LENGTH) c->dma_length = value & 0x03ffffe0u;
  else if (offset == CONTROL) {
    c->control = value;
    if (value & START) transfer(channel);
  } else c->data = value;
  return 1;
}
