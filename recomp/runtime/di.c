// The disc interface as a GameCube game reaches it: its registers at 0xcc006000, one
// command at a time, the drive's answer, and the interrupt when it's done. A Wii game asks
// IOS's /dev/di instead (dvd.c), so this is only for GameCube mode. Registers, commands,
// drive states and error codes from Dolphin's DVDInterface.cpp (GPL-2.0+).
//
// Commands finish at once: the data is in memory and the interrupt raised before the
// write that started them returns. The drive's motor isn't modelled, so a stopped motor
// still reads. Audio streamed from the disc isn't played: a game that asks for it is
// told no stream is playing, and it's reported.
#include <stdint.h>

#include "cpu.h"
#include "drive.h"
#include "gaps.h"
#include "hardware.h"

enum {
  BLOCK_MASK = 0xffffff,
  STATUS = 0x6000, COVER = 0x6004, COMMAND_0 = 0x6008, COMMAND_1 = 0x600c, COMMAND_2 = 0x6010,
  DMA_ADDRESS = 0x6014, DMA_LENGTH = 0x6018, DMA_CONTROL = 0x601c, IMMEDIATE = 0x6020, CONFIG = 0x6024,
  // The status register: an interrupt for errors, one for finished transfers and one for
  // breaks, each with its mask just below it. Writing an interrupt bit clears it.
  BREAK = 1u << 0, ERROR_MASK = 1u << 1, ERROR_DONE = 1u << 2, TRANSFER_MASK = 1u << 3, TRANSFER_DONE = 1u << 4,
  BREAK_MASK = 1u << 5, BREAK_DONE = 1u << 6, STATUS_MASKS = ERROR_MASK | TRANSFER_MASK | BREAK_MASK | BREAK,
  STATUS_INTERRUPTS = ERROR_DONE | TRANSFER_DONE | BREAK_DONE,
  // The cover register: open, then its interrupt's mask and the interrupt.
  COVER_OPEN = 1u << 0, COVER_MASK = 1u << 1, COVER_CHANGED = 1u << 2,
  // The control register: start, DMA rather than an answer in IMMEDIATE, and writing.
  START = 1u << 0, CONTROL_BITS = 7,
  // A GameCube's DMA address has 26 bits and 32-byte steps, and so has the length.
  ADDRESS_BITS = 0x03ffffe0, LENGTH_BITS = ~0x1fu,
  // Commands, in the top byte of COMMAND_0.
  INQUIRY = 0x12, READ = 0xa8, SEEK = 0xab, REQUEST_ERROR = 0xe0, AUDIO_STREAM = 0xe1, AUDIO_STATUS = 0xe2,
  STOP_MOTOR = 0xe3, AUDIO_BUFFER_CONFIG = 0xe4,
  READ_SECTOR = 0x00, READ_DISC_ID = 0x40, DISC_ID_BYTES = 0x20,
  // What REQUEST_ERROR reports: the drive's state in the top byte, then the last error.
  STATE_READY = 0, NO_ERROR = 0, MEDIUM_NOT_PRESENT = 0x23a00, INVALID_COMMAND = 0x52000, NO_AUDIO_BUFFER = 0x52001,
  BLOCK_OUT_OF_BOUNDS = 0x52100, INVALID_AUDIO_COMMAND = 0x52401,
  // Dolphin's DICFG: the boot ROM's descrambler is off.
  CONFIG_VALUE = 1,
  // The disc header's byte saying the game streams audio from the disc.
  HEADER_STREAMING = 8,
};

static uint32_t status, cover, commands[3], dma_address, dma_length, control, immediate;
static uint32_t error;          // The last command's, for REQUEST_ERROR.
static int streaming_allowed;   // The disc header asks for audio streaming, so the drive set it up.

static void update_interrupt(void) {
  int raised = ((status & ERROR_DONE) && (status & ERROR_MASK)) || ((status & TRANSFER_DONE) && (status & TRANSFER_MASK)) ||
               ((status & BREAK_DONE) && (status & BREAK_MASK)) || ((cover & COVER_CHANGED) && (cover & COVER_MASK));
  if (raised) raise_irq(IRQ_DI); else clear_irq(IRQ_DI);
}

void di_start(void) {
  uint8_t header[DISC_ID_BYTES];
  cover = drive_has_disc() ? 0 : COVER_OPEN;
  // The boot program sets the drive's audio buffer up when the header asks for streaming,
  // as Dolphin's GameCube boot does.
  streaming_allowed = drive_read(0, sizeof header, header) && header[HEADER_STREAMING];
}

// Copies disc bytes to memory at the DMA address. A read past the disc's end, or out of
// memory, fails as the drive would.
static int read_to_memory(uint64_t offset, uint32_t bytes) {
  uint32_t address = dma_address & ADDRESS_BITS;
  if (!bytes) return 1;
  uint8_t *start = host(0x80000000u | address), *last = host(0x80000000u | (address + bytes - 1));
  if (!start || last != start + bytes - 1) {
    gap_hit("hw.di_command", address);
    return 0;
  }
  return drive_read(offset, bytes, start);
}

static void write_word(uint32_t at, uint32_t value) { wr32(0x80000000u | (at & 0x03fffffc), value); }

// Runs the command in the command registers and answers with an interrupt.
static void execute(void) {
  uint32_t command = commands[0] >> 24, done = TRANSFER_DONE;
  if (command != REQUEST_ERROR) error = NO_ERROR;
  if (!drive_has_disc() && command != REQUEST_ERROR && command != INQUIRY) {
    error = MEDIUM_NOT_PRESENT;
    done = ERROR_DONE;
  } else switch (command) {
    case INQUIRY:  // Dolphin's answer, from a Wii's drive.
      write_word(dma_address, 0x00000002);       // Revision, device code.
      write_word(dma_address + 4, 0x20060526);  // Release date.
      write_word(dma_address + 8, 0x41000000);  // Version.
      break;
    case READ: {
      uint32_t kind = commands[0] & 0xff, bytes = kind == READ_DISC_ID ? DISC_ID_BYTES : commands[2];
      uint64_t offset = kind == READ_DISC_ID ? 0 : (uint64_t)commands[1] << 2;
      if (bytes > dma_length) bytes = dma_length;  // Dolphin clamps a read to the buffer.
      if (kind != READ_SECTOR && kind != READ_DISC_ID) {
        gap_hit("hw.di_command", commands[0]);
        error = INVALID_COMMAND;
        done = ERROR_DONE;
      } else if (!read_to_memory(offset, bytes)) {
        error = BLOCK_OUT_OF_BOUNDS;
        done = ERROR_DONE;
      }
      break;
    }
    case SEEK: break;  // Dolphin ignores it too.
    case REQUEST_ERROR:
      immediate = (uint32_t)STATE_READY << 24 | error;
      error = NO_ERROR;
      break;
    case AUDIO_STREAM:
    case AUDIO_STATUS:
      if (!streaming_allowed) {
        error = NO_AUDIO_BUFFER;
        done = ERROR_DONE;
      } else if (command == AUDIO_STREAM && (commands[0] >> 16 & 0xff) > 1) {
        error = INVALID_AUDIO_COMMAND;
        done = ERROR_DONE;
      } else {
        if (command == AUDIO_STREAM && !(commands[0] >> 16 & 0xff) && (commands[1] || commands[2])) {
          gap_hit("hw.disc_audio_streaming", commands[1]);
        }
        immediate = 0;  // No stream, at offset, start and length 0.
      }
      break;
    case STOP_MOTOR: break;
    case AUDIO_BUFFER_CONFIG: streaming_allowed = commands[0] >> 16 & 1; break;
    default:
      gap_hit("hw.di_command", commands[0]);
      error = INVALID_COMMAND;
      done = ERROR_DONE;
  }
  if (done == TRANSFER_DONE) {  // As Dolphin's: the whole length counts as moved.
    dma_address += dma_length;
    dma_length = 0;
  }
  control &= ~START;
  status |= done;
  update_interrupt();
}

int di_read(uint32_t address, uint32_t *value) {
  if (!gamecube_mode) return 0;
  switch (address & BLOCK_MASK) {
    case STATUS: *value = status; break;
    case COVER: *value = cover; break;
    case COMMAND_0: *value = commands[0]; break;
    case COMMAND_1: *value = commands[1]; break;
    case COMMAND_2: *value = commands[2]; break;
    case DMA_ADDRESS: *value = dma_address; break;
    case DMA_LENGTH: *value = dma_length; break;
    case DMA_CONTROL: *value = control; break;
    case IMMEDIATE: *value = immediate; break;
    case CONFIG: *value = CONFIG_VALUE; break;
    default: return 0;
  }
  return 1;
}

int di_write(uint32_t address, uint32_t value) {
  if (!gamecube_mode) return 0;
  switch (address & BLOCK_MASK) {
    case STATUS:
      if (value & BREAK) gap_hit("hw.di_command", value);  // Stopping a command midway.
      status = (status & STATUS_INTERRUPTS & ~value) | (value & STATUS_MASKS & ~BREAK);
      break;
    case COVER: cover = (cover & COVER_OPEN) | (cover & COVER_CHANGED & ~value) | (value & COVER_MASK); break;
    case COMMAND_0: commands[0] = value; return 1;
    case COMMAND_1: commands[1] = value; return 1;
    case COMMAND_2: commands[2] = value; return 1;
    case DMA_ADDRESS: dma_address = value & ADDRESS_BITS; return 1;
    case DMA_LENGTH: dma_length = value & LENGTH_BITS; return 1;
    case DMA_CONTROL:
      control = value & CONTROL_BITS;
      if (control & START) execute();
      return 1;
    case IMMEDIATE: immediate = value; return 1;
    case CONFIG: return 1;  // Read only.
    default: return 0;
  }
  update_interrupt();
  return 1;
}
