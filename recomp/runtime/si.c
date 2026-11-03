// The serial interface: the four GameCube controller ports. Nothing is ever plugged in,
// so every transfer finishes at once with "no response" for its port, as a real Wii's
// does with empty ports. The Wii Menu probes each port before it starts a disc and waits
// for every transfer to finish. Register layout from Dolphin's SerialInterface (GPL-2.0+).
#include <stdint.h>

#include "hardware.h"

enum {
  COMMUNICATION = 0x6434, STATUS = 0x6438, BLOCK_MASK = 0xffffff,
  // The communication register.
  TRANSFER_START = 1u << 0, CHANNEL_SHIFT = 1, CHANNEL_BITS = 3,
  READ_STATUS_MASK = 1u << 27, READ_STATUS = 1u << 28, COMMUNICATION_ERROR = 1u << 29,
  TRANSFER_MASK = 1u << 30, TRANSFER_DONE = 1u << 31,
  // The status register, a byte for each port from the top: no response is bit 3 of it.
  NO_RESPONSE = 1u << 27, PORT_ERRORS = 0x0f0f0f0fu,
};

static uint32_t communication, status;

static void update_interrupt(void) {
  int raised = ((communication & TRANSFER_DONE) && (communication & TRANSFER_MASK)) ||
               ((communication & READ_STATUS) && (communication & READ_STATUS_MASK));
  if (raised) raise_irq(IRQ_SI); else clear_irq(IRQ_SI);
}

int si_read(uint32_t address, uint32_t *value) {
  uint32_t offset = address & BLOCK_MASK;
  if (offset == COMMUNICATION) *value = communication;
  else if (offset == STATUS) *value = status;
  else return 0;
  return 1;
}

int si_write(uint32_t address, uint32_t value) {
  uint32_t offset = address & BLOCK_MASK;
  if (offset == STATUS) {
    status &= ~(value & PORT_ERRORS);  // Writing an error bit clears it.
    return 1;
  }
  if (offset != COMMUNICATION) return 0;
  uint32_t kept = communication & (TRANSFER_DONE | READ_STATUS | COMMUNICATION_ERROR);
  if (value & TRANSFER_DONE) kept &= ~TRANSFER_DONE;  // Writing the done bit acknowledges it.
  communication = (value & ~(TRANSFER_DONE | READ_STATUS | COMMUNICATION_ERROR | TRANSFER_START)) | kept;
  if (value & TRANSFER_START) {
    uint32_t port = value >> CHANNEL_SHIFT & CHANNEL_BITS;
    status |= NO_RESPONSE >> (port * 8);
    communication |= TRANSFER_DONE | COMMUNICATION_ERROR;
  }
  update_interrupt();
  return 1;
}
