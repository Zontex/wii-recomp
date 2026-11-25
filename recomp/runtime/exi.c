// The expansion interface: three channels of a small serial bus. Channel 0's second
// device is the chip with the real-time clock and SRAM, which the system reads as it
// starts. In GameCube mode a memory card is in slot A, channel 0's first device. Nothing
// else is plugged in, so a transfer to any other device finishes at once and reads zeros.
// Register and command layouts are from Dolphin's EXI_Channel.h and EXI_DeviceIPL.h, the
// memory card's from its EXI_DeviceMemoryCard.cpp, and SRAM's from its Sram.h (GPL-2.0+).
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "gaps.h"
#include "hardware.h"
#include "ios.h"

enum {
  FIRST = 0x6800, CHANNEL_BYTES = 0x14, CHANNELS = 3, BLOCK_MASK = 0xffffff,
  STATUS = 0x00, DMA_ADDRESS = 0x04, DMA_LENGTH = 0x08, CONTROL = 0x0c, DATA = 0x10,
  // Status: each interrupt sits beside its mask, and writing the interrupt's bit clears it.
  EXI_MASK = 1u << 0, EXI_INTERRUPT = 1u << 1, DONE_MASK = 1u << 2, DONE = 1u << 3,
  SELECT_SHIFT = 7, SELECT_BITS = 7, INSERT_MASK = 1u << 10, INSERT_INTERRUPT = 1u << 11, DEVICE_PRESENT = 1u << 12,
  INTERRUPTS = EXI_INTERRUPT | DONE | INSERT_INTERRUPT,
  // Control.
  START = 1u << 0, USES_DMA = 1u << 1, DIRECTION_SHIFT = 2, LENGTH_SHIFT = 4,
  READ = 0, WRITE = 1, READ_WRITE = 2,
  SELECT_CARD = 1, SELECT_CLOCK_CHIP = 2,  // On channel 0: memory card slot A, then the clock chip.
  // The clock chip's command: bit 31 writes, and the address is bits 6 to 30.
  COMMAND_WRITE = 1u << 31, ADDRESS_SHIFT = 6, ADDRESS_MASK = 0x1ffffff, COMMAND_BYTES = 4,
  ROM_END = 0x200000, FONT_JAPANESE_AT = 0x1aff00, FONT_WESTERN_AT = 0x1fcf00, SRAM_BASE = 0x800000, UART_BASE = 0x800400, WII_RTC_FLAGS = 0x840020,
  // SRAM: the clock, then the settings, whose checksum covers bytes 16 to 23.
  SRAM_BYTES = 0x44, SRAM_CLOCK_BYTES = 4, SRAM_CHECKSUM = 4, SUMMED_FIRST = 16, SUMMED_END = 24,
  SRAM_FLAGS = 23, SRAM_CARD_CHECKSUMS = 62,
  FLAG_STEREO = 1 << 2, FLAG_SET_UP = 1 << 3, FLAG_ALWAYS_SET = 1 << 5,
  SRAM_FLASH_IDS = 0x18, FLASH_ID_BYTES = 12,  // Each slot's card serial, in settings_ex.
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

// Seconds since 2000 in local time, the same clock the time base starts from. A Wii adds
// the settings' counter bias (IPL.CB) to the chip's count, and its date and time setting
// changes only the bias, so the count here leaves the bias out, as Dolphin's does: the
// console shows the computer's own time, whatever the bias says.
static uint32_t clock_now(void) {
  uint8_t bias[4];
  int32_t counter_bias = 0;
  if (files_system_setting("IPL.CB", bias, sizeof bias) == 4) {
    counter_bias = (int32_t)((uint32_t)bias[0] << 24 | bias[1] << 16 | bias[2] << 8 | bias[3]);
  }
  return (uint32_t)((int64_t)(time_base() / TICKS_PER_SECOND) + chip.clock_offset - counter_bias);
}

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
  if (address < ROM_END && !write) {
    // The boot ROM, of which programs read only the two fonts (OSInitFont), each a Yay0
    // file at the place Dolphin's EXI_DeviceIPL.cpp gives. These are Dolphin's free
    // fonts, not Nintendo's. A read goes on from the command's address, byte by byte.
    extern const unsigned char data_font_western[], data_font_japanese[];
    extern const unsigned data_font_western_bytes, data_font_japanese_bytes;
    uint32_t at = address + chip.cursor++;
    if (at >= FONT_WESTERN_AT && at - FONT_WESTERN_AT < data_font_western_bytes) return data_font_western[at - FONT_WESTERN_AT];
    if (at >= FONT_JAPANESE_AT && at - FONT_JAPANESE_AT < data_font_japanese_bytes) return data_font_japanese[at - FONT_JAPANESE_AT];
    if (at < FONT_JAPANESE_AT) gap_hit("hw.ipl_rom_code", address);  // The boot code itself, which isn't here.
  }
  return 0;  // The debug UART, whose output nothing needs.
}

// --- The memory card in slot A, in GameCube mode ---
// A Nintendo Memory Card 251 (16 Mbit), kept in a raw image file as Dolphin's raw cards
// are: WII_RECOMP_MEMORY_CARD_A, beside the console. A new one is blank, and the game
// offers to format it. Every change reaches the file at once. It reports itself unlocked,
// as Dolphin's does, so the SDK skips the unlocking handshake.
enum {
  CARD_DEFAULT_MEGABITS = 16, CARD_MOST_MEGABITS = 128, BYTES_PER_MEGABIT = 1024 * 1024 / 8,
  CARD_SECTOR = 0x2000, CARD_PAGE = 0x80, CARD_IN_PAGE = 0x1ff, CARD_ID = 0xc221,  // A Nintendo card.
  CARD_BUSY = 0x80, CARD_UNLOCKED = 0x40, CARD_ERASE_ERROR = 0x10, CARD_PROGRAM_ERROR = 0x08, CARD_READY = 0x01,
  CARD_NINTENDO_ID = 0x00, CARD_READ_ARRAY = 0x52, CARD_ARRAY_TO_BUFFER = 0x53, CARD_SET_INTERRUPT = 0x81,
  CARD_WRITE_BUFFER = 0x82, CARD_READ_STATUS = 0x83, CARD_READ_ID = 0x85, CARD_READ_ERROR_BUFFER = 0x86,
  CARD_WAKE_UP = 0x87, CARD_SLEEP = 0x88, CARD_CLEAR_STATUS = 0x89, CARD_SECTOR_ERASE = 0xf1, CARD_PAGE_PROGRAM = 0xf2,
  CARD_EXTRA_BYTE_PROGRAM = 0xf3, CARD_CHIP_ERASE = 0xf4,
  CARD_SERIAL_AT = 12,  // In the card's header: the format time, which seeds the serial's scrambling.
};

static struct {
  uint8_t *data;
  uint32_t bytes, megabits, address, position;
  uint8_t command, status, interrupt_on, interrupt_set, programming[CARD_PAGE];
  FILE *file;
  char path[1024];
} card;

// Writes changed bytes to the card's file at once. A failure is said, and play goes on
// with the card in memory.
static void card_save(uint32_t at, uint32_t bytes) {
  if (!card.file) return;
  if (fseek(card.file, (long)at, SEEK_SET) || fwrite(card.data + at, 1, bytes, card.file) != bytes || fflush(card.file)) {
    fprintf(stderr, "memory card: can't write %s: %s; the game's saves stay in memory only\n", card.path, strerror(errno));
    fclose(card.file);
    card.file = 0;
  }
}

// The serial the card's header was formatted with goes into SRAM, where the SDK checks a
// card against it, as Dolphin's SetCardFlashID works it out.
static void card_serial_to_sram(void) {
  uint64_t seed = 0;
  for (int i = 0; i < 8; i++) seed = seed << 8 | card.data[CARD_SERIAL_AT + i];
  uint8_t sum = 0;
  for (int i = 0; i < FLASH_ID_BYTES; i++) {
    seed = (seed * 0x41c64e6dull + 0x3039) >> 16;
    sum = (uint8_t)(sum + (chip.sram[SRAM_FLASH_IDS + i] = (uint8_t)(card.data[i] - (uint8_t)seed)));
    seed = ((seed * 0x41c64e6dull + 0x3039) >> 16) & 0x7fff;
  }
  chip.sram[SRAM_CARD_CHECKSUMS] = sum ^ 0xff;
}

static void card_start(void) {
  const char *path = getenv("WII_RECOMP_MEMORY_CARD_A");
  snprintf(card.path, sizeof card.path, "%s", path ? path : "memory-card-a.raw");
  card.file = fopen(card.path, "r+b");
  long size = 0;
  if (card.file && !fseek(card.file, 0, SEEK_END)) size = ftell(card.file);
  int usable = size > 0 && size % (BYTES_PER_MEGABIT * 4) == 0 && size <= CARD_MOST_MEGABITS * BYTES_PER_MEGABIT;
  if (card.file && !usable) {
    fprintf(stderr, "memory card: %s isn't a card image (%ld bytes), so slot A is empty\n", card.path, size);
    fclose(card.file);
    card.file = 0;
    return;
  }
  card.bytes = card.file ? (uint32_t)size : CARD_DEFAULT_MEGABITS * BYTES_PER_MEGABIT;
  card.megabits = card.bytes / BYTES_PER_MEGABIT;
  card.data = malloc(card.bytes);
  memset(card.data, 0xff, card.bytes);  // A blank card, until the game formats it.
  if (card.file) {
    rewind(card.file);
    if (fread(card.data, 1, card.bytes, card.file) != card.bytes) fprintf(stderr, "memory card: %s reads short\n", card.path);
  } else if ((card.file = fopen(card.path, "w+b"))) {
    card_save(0, card.bytes);
    fprintf(stderr, "memory card: a blank one in %s\n", card.path);
  } else {
    fprintf(stderr, "memory card: can't make %s: %s; saves stay in memory only\n", card.path, strerror(errno));
  }
  card.status = CARD_BUSY | CARD_UNLOCKED | CARD_READY;
  card_serial_to_sram();
}

// A command is done: ready, and its interrupt raised if the game turned it on.
static void card_done(void) {
  card.status = (uint8_t)((card.status | CARD_READY) & ~CARD_BUSY);
  card.interrupt_set = 1;
}

// One byte each way. The first is the command; then come its address, then its data.
static uint8_t card_byte(uint8_t in) {
  uint8_t out = 0xff;
  if (card.position == 0) {
    card.command = in;
    switch (in) {
      case CARD_NINTENDO_ID: case CARD_READ_ARRAY: case CARD_ARRAY_TO_BUFFER: case CARD_SET_INTERRUPT: case CARD_WRITE_BUFFER:
      case CARD_READ_STATUS: case CARD_READ_ID: case CARD_READ_ERROR_BUFFER: case CARD_WAKE_UP: case CARD_SLEEP:
      case CARD_SECTOR_ERASE: case CARD_PAGE_PROGRAM: case CARD_EXTRA_BYTE_PROGRAM: case CARD_CHIP_ERASE: break;
      case CARD_CLEAR_STATUS:
        card.status = (uint8_t)((card.status & ~(CARD_PROGRAM_ERROR | CARD_ERASE_ERROR)) | CARD_READY);
        card.interrupt_set = 0;
        break;
      default: gap_hit("hw.gc_memory_card", in);
    }
  } else {
    uint32_t at = card.position;
    int addressed = card.command == CARD_READ_ARRAY || card.command == CARD_PAGE_PROGRAM || card.command == CARD_SECTOR_ERASE;
    if (addressed && at == 1) card.address = (uint32_t)in << 17;
    else if (addressed && at == 2) card.address |= (uint32_t)in << 9;
    else if (addressed && card.command != CARD_SECTOR_ERASE && at == 3) card.address |= (uint32_t)(in & 3) << 7;
    else if (addressed && card.command != CARD_SECTOR_ERASE && at == 4) card.address |= in & 0x7f;
    switch (card.command) {
      case CARD_NINTENDO_ID: out = at == 1 ? 0x80 : (uint8_t)(card.megabits >> (24 - ((at - 2) & 3) * 8)); break;
      case CARD_READ_ARRAY:
        if (at > 1) {
          out = card.data[card.address & (card.bytes - 1)];
          if (at >= 9) card.address = (card.address & ~(uint32_t)CARD_IN_PAGE) | ((card.address + 1) & CARD_IN_PAGE);
        }
        break;
      case CARD_READ_STATUS: out = card.status; break;
      case CARD_READ_ID: out = (uint8_t)(at == 1 || !(at & 1) ? CARD_ID >> 8 : CARD_ID); break;
      case CARD_SET_INTERRUPT: if (at == 1) card.interrupt_on = in; break;
      case CARD_PAGE_PROGRAM: if (at >= 5) card.programming[(at - 5) & (CARD_PAGE - 1)] = in; break;
      default: break;
    }
  }
  card.position++;
  return out;
}

// The card's chip select going off ends a command: an erase or a program happens then.
static void card_deselected(void) {
  uint32_t at = card.address & (card.bytes - 1);
  if (card.command == CARD_SECTOR_ERASE && card.position > 2) {
    at &= ~(uint32_t)(CARD_SECTOR - 1);
    memset(card.data + at, 0xff, CARD_SECTOR);
    card_save(at, CARD_SECTOR);
    card_done();
  } else if (card.command == CARD_CHIP_ERASE && card.position > 2) {
    memset(card.data, 0xff, card.bytes);
    card_save(0, card.bytes);
    card.status &= (uint8_t)~CARD_BUSY;
  } else if (card.command == CARD_PAGE_PROGRAM && card.position >= 5) {
    for (uint32_t i = 0; i < card.position - 5; i++) {  // Bytes sent one at a time, wrapping in the page.
      card.data[card.address & (card.bytes - 1)] = card.programming[i & (CARD_PAGE - 1)];
      card_save(card.address & (card.bytes - 1), 1);
      card.address = (card.address & ~(uint32_t)CARD_IN_PAGE) | ((card.address + 1) & CARD_IN_PAGE);
    }
    card_done();
  }
}

// DMA moves a block between memory and the card at the address the command gave.
static void card_dma(int to_card, uint32_t memory_at, uint32_t bytes) {
  uint32_t at = card.address & (card.bytes - 1);
  uint8_t *memory = host(memory_at), *last = bytes ? host(memory_at + bytes - 1) : memory;
  if (!memory || last != memory + (bytes ? bytes - 1 : 0) || at + bytes > card.bytes) {
    gap_hit("hw.gc_memory_card", memory_at);
    return;
  }
  if (to_card) {
    memcpy(card.data + at, memory, bytes);
    card_save(at, bytes);
  } else {
    memcpy(memory, card.data + at, bytes);
  }
}

static int card_in_slot(int channel) { return gamecube_mode && channel == 0 && card.data; }

void exi_start(void) {
  if (!chip.ready) sram_start();
  card_start();
  // The insertion interrupt is up on both card channels when the console starts, as
  // Dolphin has it, so the SDK looks at what's there.
  channels[0].status |= INSERT_INTERRUPT;
  channels[1].status |= INSERT_INTERRUPT;
}

static int selected(int channel) { return (int)(channels[channel].status >> SELECT_SHIFT) & SELECT_BITS; }

static uint8_t transfer_byte(int channel, uint8_t in) {
  if (channel == 0 && selected(0) == SELECT_CLOCK_CHIP) return clock_chip_byte(in);
  if (card_in_slot(channel) && selected(0) == SELECT_CARD) return card_byte(in);
  return 0;
}

static void update_interrupt(void) {
  if (card_in_slot(0) && card.interrupt_on && card.interrupt_set) channels[0].status |= EXI_INTERRUPT;
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
  int to_card = card_in_slot(channel) && device == SELECT_CARD;
  if (((channel == 0 && device == SELECT_CARD) || (channel == 1 && device)) && !to_card) gap_hit("hw.gc_memory_card", (uint32_t)channel);
  if (to_card && (c->control & USES_DMA)) {
    card_dma(direction == WRITE, c->dma_address | 0x80000000u, c->dma_length);
  } else if (c->control & USES_DMA) {
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
  if (offset == STATUS) *value = c->status | (card_in_slot((int)(c - channels)) ? DEVICE_PRESENT : 0);
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
    // Selecting the clock chip or the card starts a new command, and letting the card
    // go ends one.
    if (channel == 0 && selected(0) == SELECT_CLOCK_CHIP && before != SELECT_CLOCK_CHIP) chip.received = chip.cursor = 0;
    if (card_in_slot(channel) && selected(0) == SELECT_CARD && before != SELECT_CARD) card.position = 0;
    if (card_in_slot(channel) && before == SELECT_CARD && selected(0) != SELECT_CARD) card_deselected();
    update_interrupt();
  } else if (offset == DMA_ADDRESS) {
    // A physical address in 32-byte steps. The GameCube's 26 bits don't reach the Wii's
    // second memory at 0x10000000, where a game's heap may hold the buffer: cut short,
    // a font read into MEM2 landed over the program's own data in MEM1.
    c->dma_address = value & 0x1fffffe0u;
  }
  else if (offset == DMA_LENGTH) c->dma_length = value & 0x03ffffe0u;
  else if (offset == CONTROL) {
    c->control = value;
    if (value & START) transfer(channel);
  } else c->data = value;
  return 1;
}
