// What's plugged into the simulated Wii Remote: a Classic Controller. An attachment is a
// small block of registers the remote reads over a wire. Games can turn on encryption of
// everything read from it, using a key they choose: see wii_attachment_key.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "wii_attachment.h"
#include "wii_attachment_key.h"

enum {
  REGISTERS = 256, CALIBRATION = 0x20, CALIBRATION_COPY = 0x30, CALIBRATION_BYTES = 14, CHECK_ONE = 0x55, CHECK_TWO = 0xaa,
  KEY_FIRST = 0x40, KEY_LAST = 0x4f, ENABLE = 0xf0, ENABLE_ENCRYPTED = 0xaa, IDENTITY = 0xfa,
  STICK_LEFT_RANGE = 63, STICK_RIGHT_RANGE = 31, CLASSIC_BYTES = 6,
};

static uint8_t registers[REGISTERS];
static int started, keyed;
static uint8_t key_add[8], key_exclusive[8];  // Made from the key the game wrote.

static void start(void) {
  static const uint8_t calibration[CALIBRATION_BYTES] = {0xff, 0, 0x80, 0xff, 0, 0x80, 0xff, 0, 0x80, 0xff, 0, 0x80, 0, 0};
  static const uint8_t identity[] = {0, 0, 0xa4, 0x20, 1, 1};
  uint8_t sum = 0;
  for (int i = 0; i < CALIBRATION_BYTES; i++) sum = (uint8_t)(sum + calibration[i]);
  for (int base = CALIBRATION; base <= CALIBRATION_COPY; base += 16) {
    memcpy(registers + base, calibration, sizeof calibration);
    registers[base + CALIBRATION_BYTES] = (uint8_t)(sum + CHECK_ONE);
    registers[base + CALIBRATION_BYTES + 1] = (uint8_t)(sum + CHECK_TWO);
  }
  memcpy(registers + IDENTITY, identity, sizeof identity);
  started = 1;
}

void attachment_encrypt(uint8_t *bytes, uint32_t count, uint32_t address, const uint8_t add[8], const uint8_t exclusive[8]) {
  for (uint32_t i = 0; i < count; i++) {
    uint32_t at = (address + i) % 8;
    bytes[i] = (uint8_t)((uint8_t)(bytes[i] - add[at]) ^ exclusive[at]);
  }
}

// The game writes a key, and both ends make the same two tables from it: the game's
// library to decrypt what it reads, and the attachment to encrypt it.
static void encrypt_if_on(uint8_t *bytes, uint32_t count, uint32_t address) {
  if (registers[ENABLE] != ENABLE_ENCRYPTED || !keyed) return;
  attachment_encrypt(bytes, count, address, key_add, key_exclusive);
}

void attachment_read(uint32_t address, uint8_t *out, uint32_t bytes) {
  if (!started) start();
  for (uint32_t i = 0; i < bytes; i++) out[i] = registers[(address + i) % REGISTERS];
  encrypt_if_on(out, bytes, address);
}

void attachment_write(uint32_t address, const uint8_t *data, uint32_t bytes) {
  if (!started) start();
  for (uint32_t i = 0; i < bytes; i++) {
    uint32_t at = (address + i) % REGISTERS;
    registers[at] = data[i];
    if (at == ENABLE) keyed = 0;
    if (at == KEY_LAST) {
      if (!attachment_key_tables(registers + KEY_FIRST, key_add, key_exclusive)) fprintf(stderr, "attachment: the game's key isn't one the Wii makes, so fallback tables are used\n");
      keyed = 1;
    }
  }
}

// Six bytes: both sticks and the triggers packed together, then buttons that read 0 when
// pressed. Anything past those bytes reads as all ones, as on the real thing.
void attachment_classic_state(uint8_t *to, uint32_t buttons, const float sticks[4]) {
  static const struct { uint32_t remote; int byte, bit; } map[] = {
    {BUTTON_A, 5, 4}, {BUTTON_B, 5, 6}, {BUTTON_ONE, 5, 3}, {BUTTON_TWO, 5, 5}, {BUTTON_UP, 5, 0}, {BUTTON_LEFT, 5, 1},
    {BUTTON_DOWN, 4, 6}, {BUTTON_RIGHT, 4, 7}, {BUTTON_PLUS, 4, 2}, {BUTTON_MINUS, 4, 4}, {BUTTON_HOME, 4, 3},
  };
  uint32_t left_x = (uint32_t)((sticks[0] + 1) / 2 * STICK_LEFT_RANGE + 0.5f), left_y = (uint32_t)((sticks[1] + 1) / 2 * STICK_LEFT_RANGE + 0.5f);
  uint32_t right_x = (uint32_t)((sticks[2] + 1) / 2 * STICK_RIGHT_RANGE + 0.5f), right_y = (uint32_t)((sticks[3] + 1) / 2 * STICK_RIGHT_RANGE + 0.5f);
  to[0] = (uint8_t)((right_x >> 3) << 6 | left_x);
  to[1] = (uint8_t)(((right_x >> 1) & 3) << 6 | left_y);
  to[2] = (uint8_t)((right_x & 1) << 7 | right_y);
  to[3] = 0;
  to[4] = to[5] = 0xff;
  for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
    if (buttons & map[i].remote) to[map[i].byte] &= (uint8_t)~(1u << map[i].bit);
  }
}

void attachment_fill(uint8_t *out, uint32_t bytes) {
  float sticks[4];
  uint8_t state[CLASSIC_BYTES];
  input_sticks(sticks);
  attachment_classic_state(state, input_buttons(), sticks);
  for (uint32_t i = 0; i < bytes; i++) out[i] = i < CLASSIC_BYTES ? state[i] : 0xff;
  encrypt_if_on(out, bytes, 0);
}
