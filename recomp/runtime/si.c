// The serial interface: the four GameCube controller ports. On the Wii side nothing is
// ever plugged in, so every transfer finishes at once with "no response" for its port, as
// a real Wii's does with empty ports: the Wii Menu probes each port before it starts a
// disc and waits for every transfer to finish. In GameCube mode a standard controller is
// in the first port, fed from player 1's controls (input.h): the game polls it once a
// field and talks to it through the transfer buffer. The others stay empty. Registers,
// commands and the controller's answers from Dolphin's SI.cpp, SI_Device.cpp and
// SI_DeviceGCController.cpp (GPL-2.0+).
#include <stdint.h>

#include "gaps.h"
#include "hardware.h"
#include "input.h"

enum {
  BLOCK_MASK = 0xffffff, CHANNELS = 4,
  // A channel's registers, 12 bytes apart: the command polling sends, then the last answer.
  CHANNEL_OUT = 0x6400, CHANNEL_IN_HIGH = 0x6404, CHANNEL_IN_LOW = 0x6408, CHANNEL_STRIDE = 12,
  POLL = 0x6430, COMMUNICATION = 0x6434, STATUS = 0x6438, EXI_CLOCK = 0x643c,
  BUFFER = 0xcc006480u, BUFFER_BYTES = 0x80,
  // The communication register.
  TRANSFER_START = 1u << 0, CHANNEL_SHIFT = 1, CHANNEL_BITS = 3, IN_LENGTH_SHIFT = 8, OUT_LENGTH_SHIFT = 16,
  LENGTH_BITS = 0x7f, READ_STATUS_MASK = 1u << 27, READ_STATUS = 1u << 28, COMMUNICATION_ERROR = 1u << 29,
  TRANSFER_MASK = 1u << 30, TRANSFER_DONE = 1u << 31,
  // The status register, a byte for each channel from the top: errors in its low four bits
  // (no response is bit 3), then new data in bit 5. Bit 31 sends every channel's command.
  NO_RESPONSE = 1u << 27, NEW_DATA = 1u << 29, PORT_ERRORS = 0x0f0f0f0fu, SEND_COMMANDS = 1u << 31,
  WRITE_PENDING = 0x10101010u,
  // The poll register: which channels are polled, bit 7 for the first.
  POLL_CHANNEL_0 = 1u << 7,
  // An answer's error bits, at the top of its high word.
  ANSWER_ERROR = 1u << 31, ANSWER_ERROR_LATCHED = 1u << 30,
  // The controller's commands.
  COMMAND_STATUS = 0x00, COMMAND_DIRECT = 0x40, COMMAND_ORIGIN = 0x41, COMMAND_RECALIBRATE = 0x42, COMMAND_RESET = 0xff,
  RUMBLE_ON = 1, DEFAULT_MODE = 3,
  // Its buttons, as the game reads them (Dolphin's GCPadStatus.h).
  GC_LEFT = 0x0001, GC_RIGHT = 0x0002, GC_DOWN = 0x0004, GC_UP = 0x0008, GC_Z = 0x0010, GC_R = 0x0020, GC_L = 0x0040,
  GC_USE_ORIGIN = 0x0080, GC_A = 0x0100, GC_B = 0x0200, GC_X = 0x0400, GC_Y = 0x0800, GC_START = 0x1000,
  STICK_CENTRE = 0x80, STICK_RADIUS = 0x7f, TRIGGER_FULL = 0xff,
};

static uint32_t out[CHANNELS], in_high[CHANNELS], in_low[CHANNELS], poll, communication, status, exi_clock;
static int mode = DEFAULT_MODE;  // How the controller packs its triggers and C stick into the answer.

static int has_controller(uint32_t channel) { return gamecube_mode && channel == 0; }

static void update_interrupt(void) {
  if (status & (NEW_DATA | NEW_DATA >> 8 | NEW_DATA >> 16 | NEW_DATA >> 24)) communication |= READ_STATUS;
  else communication &= ~READ_STATUS;
  int raised = ((communication & TRANSFER_DONE) && (communication & TRANSFER_MASK)) ||
               ((communication & READ_STATUS) && (communication & READ_STATUS_MASK));
  if (raised) raise_irq(IRQ_SI); else clear_irq(IRQ_SI);
}

static uint8_t stick(float value) {
  int scaled = STICK_CENTRE + (int)(value * STICK_RADIUS + (value < 0 ? -0.5f : 0.5f));
  return (uint8_t)(scaled < 0 ? 0 : scaled > 0xff ? 0xff : scaled);
}

// Player 1's controls as a GameCube controller. The pad maps as a Classic Controller
// (main.c attaches one in GameCube mode), so its buttons sit where a GameCube's do: the
// right shoulder is Z, and the triggers are L and R, fully pressed when held.
static void controller_now(uint16_t *buttons, uint8_t sticks[4], uint8_t triggers[2]) {
  static const struct { uint32_t from; uint16_t to; } map[] = {
    {BUTTON_A, GC_A}, {BUTTON_B, GC_B}, {BUTTON_ONE, GC_X}, {BUTTON_TWO, GC_Y}, {BUTTON_PLUS, GC_START},
    {BUTTON_CLASSIC_R, GC_Z}, {BUTTON_CLASSIC_ZL, GC_L}, {BUTTON_CLASSIC_L, GC_L}, {BUTTON_CLASSIC_ZR, GC_R},
    {BUTTON_UP, GC_UP}, {BUTTON_DOWN, GC_DOWN}, {BUTTON_LEFT, GC_LEFT}, {BUTTON_RIGHT, GC_RIGHT},
  };
  PlayerState player;
  input_snapshot(0, &player);
  *buttons = 0;
  for (unsigned i = 0; i < sizeof map / sizeof *map; i++) {
    if (player.buttons & map[i].from) *buttons |= map[i].to;
  }
  for (int i = 0; i < 4; i++) sticks[i] = stick(player.sticks[i]);
  triggers[0] = *buttons & GC_L ? TRIGGER_FULL : 0;
  triggers[1] = *buttons & GC_R ? TRIGGER_FULL : 0;
}

// What a poll reads: buttons and the main stick, then the rest packed by mode.
static void controller_answer(uint32_t *high, uint32_t *low) {
  uint16_t buttons;
  uint8_t sticks[4], triggers[2];
  controller_now(&buttons, sticks, triggers);
  *high = (uint32_t)(buttons | GC_USE_ORIGIN) << 16 | (uint32_t)sticks[0] << 8 | sticks[1];
  uint32_t c_x = sticks[2], c_y = sticks[3], l = triggers[0], r = triggers[1];  // No analog A and B: 0.
  switch (mode) {
    case 1: *low = r << 8 | l << 16 | (c_y >> 4) << 24 | (c_x >> 4) << 28; break;
    case 2: *low = (r >> 4) << 16 | (l >> 4) << 20 | (c_y >> 4) << 24 | (c_x >> 4) << 28; break;
    case 3: *low = r | l << 8 | c_y << 16 | c_x << 24; break;
    case 4: *low = c_y << 16 | c_x << 24; break;
    default: *low = (r >> 4) << 8 | (l >> 4) << 12 | c_y << 16 | c_x << 24; break;
  }
}

// A transfer through the buffer: the command in its first byte, the answer written over
// it. Returns the answer's length, or -1 for no answer.
static int controller_transfer(void) {
  uint8_t command = (uint8_t)hw_peek(BUFFER, 1), answer[10] = {0};
  int length;
  switch (command) {
    case COMMAND_STATUS:
    case COMMAND_RESET:
      answer[0] = 0x09;  // A standard GameCube controller.
      length = 3;
      break;
    case COMMAND_DIRECT: {
      uint32_t high, low;
      controller_answer(&high, &low);
      for (int i = 0; i < 4; i++) answer[i] = (uint8_t)(high >> (24 - 8 * i)), answer[4 + i] = (uint8_t)(low >> (24 - 8 * i));
      length = 8;
      break;
    }
    case COMMAND_ORIGIN:
    case COMMAND_RECALIBRATE:  // Centred sticks and released triggers, as Dolphin's origin is.
      answer[2] = answer[3] = answer[4] = answer[5] = STICK_CENTRE;
      length = 10;
      break;
    default:
      gap_hit("hw.gc_controller_wheel_keyboard_dance_mat", command);
      return -1;
  }
  for (int i = 0; i < length; i++) hw_poke(BUFFER + (uint32_t)i, answer[i], 1);
  return length;
}

// The command a channel's out register holds, sent when the game sets the status
// register's top bit: rumble, and while the channel isn't polled, the answer's mode.
static void controller_command(uint32_t command) {
  if ((command >> 16 & 0xff) != COMMAND_DIRECT) {
    if (command >> 16 & 0xff) gap_hit("hw.gc_controller_wheel_keyboard_dance_mat", command);
    return;
  }
  input_set_outputs(0, (command & 0xff) == RUMBLE_ON, input_outputs(0).lights);
  if (!(poll & POLL_CHANNEL_0)) mode = command >> 8 & 0xff;
}

void si_poll(void) {
  if (!gamecube_mode) return;
  for (uint32_t channel = 0; channel < CHANNELS; channel++) {
    uint32_t latched = in_high[channel] & ANSWER_ERROR_LATCHED;
    if (has_controller(channel)) {
      controller_answer(&in_high[channel], &in_low[channel]);
      in_high[channel] |= latched;
      status |= NEW_DATA >> (channel * 8);
    } else {
      in_high[channel] |= ANSWER_ERROR | ANSWER_ERROR_LATCHED;
      status |= NO_RESPONSE >> (channel * 8);
    }
  }
  update_interrupt();
}

int si_read(uint32_t address, uint32_t *value) {
  uint32_t offset = address & BLOCK_MASK;
  if (offset >= CHANNEL_OUT && offset < POLL) {
    uint32_t channel = (offset - CHANNEL_OUT) / CHANNEL_STRIDE, which = (offset - CHANNEL_OUT) % CHANNEL_STRIDE;
    if (which == 0) *value = out[channel];
    else {
      *value = which == 4 ? in_high[channel] : in_low[channel];
      status &= ~(NEW_DATA >> (channel * 8));  // Read, so no longer new.
      update_interrupt();
    }
  } else if (offset == POLL) *value = poll;
  else if (offset == COMMUNICATION) *value = communication;
  else if (offset == STATUS) *value = status;
  else if (offset == EXI_CLOCK) *value = exi_clock;
  else return 0;
  return 1;
}

int si_write(uint32_t address, uint32_t value) {
  uint32_t offset = address & BLOCK_MASK;
  if (offset >= CHANNEL_OUT && offset < POLL) {
    uint32_t channel = (offset - CHANNEL_OUT) / CHANNEL_STRIDE, which = (offset - CHANNEL_OUT) % CHANNEL_STRIDE;
    if (which == 0) out[channel] = value;
    else if (which == 4) in_high[channel] = value;
    else in_low[channel] = value;
    return 1;
  }
  if (offset == POLL || offset == EXI_CLOCK) {
    *(offset == POLL ? &poll : &exi_clock) = value;
    return 1;
  }
  if (offset == STATUS) {
    status &= ~(value & PORT_ERRORS);  // Writing an error bit clears it.
    if (value & SEND_COMMANDS) {
      if (gamecube_mode) controller_command(out[0]);
      status &= ~(SEND_COMMANDS | WRITE_PENDING);
    }
    return 1;
  }
  if (offset != COMMUNICATION) return 0;
  uint32_t kept = communication & (TRANSFER_DONE | READ_STATUS | COMMUNICATION_ERROR);
  if (value & TRANSFER_DONE) kept &= ~TRANSFER_DONE;  // Writing the done bit acknowledges it.
  communication = (value & ~(TRANSFER_DONE | READ_STATUS | COMMUNICATION_ERROR | TRANSFER_START)) | kept;
  if (value & TRANSFER_START) {
    uint32_t channel = value >> CHANNEL_SHIFT & CHANNEL_BITS;
    int answered = has_controller(channel) ? controller_transfer() : -1;
    communication &= ~COMMUNICATION_ERROR;
    communication |= TRANSFER_DONE | (answered < 0 ? COMMUNICATION_ERROR : 0);
    if (answered < 0) status |= NO_RESPONSE >> (channel * 8);
  }
  update_interrupt();
  return 1;
}
