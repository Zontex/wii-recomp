// What's plugged into each simulated Wii Remote: a Nunchuk or a Classic Controller. An
// attachment is a small block of registers the remote reads over a wire. Games can turn
// on encryption of everything read from it, using a key they choose: see
// wii_attachment_key.c. Each remote has its own, so two players' games can use two keys.
// The Nunchuk's numbers are Dolphin's (Nunchuk.h, Nunchuk.cpp).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "wii_attachment.h"
#include "wii_attachment_key.h"

enum {
  REGISTERS = 256, CALIBRATION = 0x20, CALIBRATION_COPY = 0x30, CALIBRATION_BYTES = 14, CHECK_ONE = 0x55, CHECK_TWO = 0xaa,
  KEY_FIRST = 0x40, KEY_LAST = 0x4f, ENABLE = 0xf0, ENABLE_ENCRYPTED = 0xaa, IDENTITY = 0xfa,
  STICK_LEFT_RANGE = 63, STICK_RIGHT_RANGE = 31, CLASSIC_BYTES = 6, NUNCHUK_BYTES = 6,
  NUNCHUK_ZERO_G = 0x80 << 2, NUNCHUK_ONE_G = 0xb3 << 2, NUNCHUK_STICK_CENTRE = 0x80, NUNCHUK_STICK_REACH = 0x60,
  NUNCHUK_Z_UP = 1, NUNCHUK_C_UP = 2,
};

typedef struct {
  uint8_t registers[REGISTERS];
  int started, keyed;
  Attachment kind;
  uint8_t key_add[8], key_exclusive[8];  // Made from the key the game wrote.
} Port;

// Until the remote plugs one in, a port holds a Classic Controller.
static Port ports[PLAYERS] = {{.kind = ATTACHMENT_CLASSIC}, {.kind = ATTACHMENT_CLASSIC}, {.kind = ATTACHMENT_CLASSIC}, {.kind = ATTACHMENT_CLASSIC}};

static Port *start(int device) {
  Port *a = &ports[device >= 0 && device < PLAYERS ? device : 0];
  if (a->started) return a;
  uint8_t *registers = a->registers;
  static const uint8_t classic[CALIBRATION_BYTES] = {0xff, 0, 0x80, 0xff, 0, 0x80, 0xff, 0, 0x80, 0xff, 0, 0x80, 0, 0};
  // Zero, then one g, on each axis, a byte for the bits below them, then the stick's
  // right, left and middle across and its top, bottom and middle.
  static const uint8_t nunchuk[CALIBRATION_BYTES] = {
    NUNCHUK_ZERO_G >> 2, NUNCHUK_ZERO_G >> 2, NUNCHUK_ZERO_G >> 2, 0, NUNCHUK_ONE_G >> 2, NUNCHUK_ONE_G >> 2, NUNCHUK_ONE_G >> 2, 0,
    NUNCHUK_STICK_CENTRE + NUNCHUK_STICK_REACH, NUNCHUK_STICK_CENTRE - NUNCHUK_STICK_REACH, NUNCHUK_STICK_CENTRE,
    NUNCHUK_STICK_CENTRE + NUNCHUK_STICK_REACH, NUNCHUK_STICK_CENTRE - NUNCHUK_STICK_REACH, NUNCHUK_STICK_CENTRE,
  };
  static const uint8_t classic_identity[] = {0, 0, 0xa4, 0x20, 1, 1}, nunchuk_identity[] = {0, 0, 0xa4, 0x20, 0, 0};
  const uint8_t *calibration = a->kind == ATTACHMENT_NUNCHUK ? nunchuk : classic;
  const uint8_t *identity = a->kind == ATTACHMENT_NUNCHUK ? nunchuk_identity : classic_identity;
  uint8_t sum = 0;
  for (int i = 0; i < CALIBRATION_BYTES; i++) sum = (uint8_t)(sum + calibration[i]);
  for (int base = CALIBRATION; base <= CALIBRATION_COPY; base += 16) {
    memcpy(registers + base, calibration, CALIBRATION_BYTES);
    registers[base + CALIBRATION_BYTES] = (uint8_t)(sum + CHECK_ONE);
    registers[base + CALIBRATION_BYTES + 1] = (uint8_t)(sum + CHECK_TWO);
  }
  memcpy(registers + IDENTITY, identity, sizeof classic_identity);
  a->started = 1;
  return a;
}

// A new attachment starts from nothing: no encryption, no key, its own registers.
void attachment_plug(int device, Attachment kind) {
  if (device < 0 || device >= PLAYERS) return;
  memset(&ports[device], 0, sizeof ports[device]);
  ports[device].kind = kind;
}

void attachment_encrypt(uint8_t *bytes, uint32_t count, uint32_t address, const uint8_t add[8], const uint8_t exclusive[8]) {
  for (uint32_t i = 0; i < count; i++) {
    uint32_t at = (address + i) % 8;
    bytes[i] = (uint8_t)((uint8_t)(bytes[i] - add[at]) ^ exclusive[at]);
  }
}

// The game writes a key, and both ends make the same two tables from it: the game's
// library to decrypt what it reads, and the attachment to encrypt it.
static void encrypt_if_on(Port *a, uint8_t *bytes, uint32_t count, uint32_t address) {
  if (a->registers[ENABLE] != ENABLE_ENCRYPTED || !a->keyed) return;
  attachment_encrypt(bytes, count, address, a->key_add, a->key_exclusive);
}

void attachment_read(int device, uint32_t address, uint8_t *out, uint32_t bytes) {
  Port *a = start(device);
  for (uint32_t i = 0; i < bytes; i++) out[i] = a->registers[(address + i) % REGISTERS];
  encrypt_if_on(a, out, bytes, address);
}

void attachment_write(int device, uint32_t address, const uint8_t *data, uint32_t bytes) {
  Port *a = start(device);
  for (uint32_t i = 0; i < bytes; i++) {
    uint32_t at = (address + i) % REGISTERS;
    a->registers[at] = data[i];
    if (at == ENABLE) a->keyed = 0;
    if (at == KEY_LAST) {
      if (!attachment_key_tables(a->registers + KEY_FIRST, a->key_add, a->key_exclusive)) {
        fprintf(stderr, "attachment: the game's key isn't one the Wii makes, so fallback tables are used\n");
      }
      a->keyed = 1;
    }
  }
}

// Six bytes: both sticks and the triggers packed together, then buttons that read 0 when
// pressed. Anything past those bytes reads as all ones, as on the real thing.
void attachment_classic_state(uint8_t *to, uint32_t buttons, const float sticks[4]) {
  static const struct { uint32_t remote; int byte, bit; } map[] = {
    {BUTTON_A, 5, 4}, {BUTTON_B, 5, 6}, {BUTTON_ONE, 5, 3}, {BUTTON_TWO, 5, 5}, {BUTTON_UP, 5, 0}, {BUTTON_LEFT, 5, 1},
    {BUTTON_DOWN, 4, 6}, {BUTTON_RIGHT, 4, 7}, {BUTTON_PLUS, 4, 2}, {BUTTON_MINUS, 4, 4}, {BUTTON_HOME, 4, 3},
    {BUTTON_CLASSIC_L, 4, 5}, {BUTTON_CLASSIC_R, 4, 1}, {BUTTON_CLASSIC_ZL, 5, 7}, {BUTTON_CLASSIC_ZR, 5, 2},
  };
  uint32_t left_x = (uint32_t)((sticks[0] + 1) / 2 * STICK_LEFT_RANGE + 0.5f), left_y = (uint32_t)((sticks[1] + 1) / 2 * STICK_LEFT_RANGE + 0.5f);
  uint32_t right_x = (uint32_t)((sticks[2] + 1) / 2 * STICK_RIGHT_RANGE + 0.5f), right_y = (uint32_t)((sticks[3] + 1) / 2 * STICK_RIGHT_RANGE + 0.5f);
  to[0] = (uint8_t)((right_x >> 3) << 6 | left_x);
  to[1] = (uint8_t)(((right_x >> 1) & 3) << 6 | left_y);
  // A held shoulder button is also pressed all the way down.
  uint32_t left_trigger = buttons & BUTTON_CLASSIC_L ? 31 : 0, right_trigger = buttons & BUTTON_CLASSIC_R ? 31 : 0;
  to[2] = (uint8_t)((right_x & 1) << 7 | (left_trigger >> 3) << 5 | right_y);
  to[3] = (uint8_t)((left_trigger & 7) << 5 | right_trigger);
  to[4] = to[5] = 0xff;
  for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
    if (buttons & map[i].remote) to[map[i].byte] &= (uint8_t)~(1u << map[i].bit);
  }
}

static uint32_t clamp(float value, uint32_t top) { return value < 0 ? 0 : value > (float)top ? top : (uint32_t)(value + 0.5f); }

// The stick, then the motion's top eight bits on each axis, then C and Z, which read 0
// while held, under the motion's lowest two bits.
void attachment_nunchuk_state(uint8_t *to, uint32_t buttons, const float stick[2], const float accel[3]) {
  to[0] = (uint8_t)clamp(NUNCHUK_STICK_CENTRE + stick[0] * NUNCHUK_STICK_REACH, 0xff);
  to[1] = (uint8_t)clamp(NUNCHUK_STICK_CENTRE + stick[1] * NUNCHUK_STICK_REACH, 0xff);
  uint8_t last = (buttons & BUTTON_NUNCHUK_Z ? 0 : NUNCHUK_Z_UP) | (buttons & BUTTON_NUNCHUK_C ? 0 : NUNCHUK_C_UP);
  for (int axis = 0; axis < 3; axis++) {
    uint32_t value = clamp(NUNCHUK_ZERO_G + accel[axis] * (NUNCHUK_ONE_G - NUNCHUK_ZERO_G), 0x3ff);
    to[2 + axis] = (uint8_t)(value >> 2);
    last |= (uint8_t)((value & 3) << (2 + axis * 2));
  }
  to[5] = last;
}

void attachment_plain_state(int device, const PlayerState *player, uint8_t out[6]) {
  Port *a = start(device);
  if (a->kind == ATTACHMENT_NUNCHUK) attachment_nunchuk_state(out, player->buttons, player->nunchuk_stick, player->nunchuk_accel);
  else attachment_classic_state(out, player->buttons, player->sticks);
}

void attachment_fill(int device, const PlayerState *player, uint8_t *out, uint32_t bytes) {
  uint8_t state[CLASSIC_BYTES];
  attachment_plain_state(device, player, state);
  for (uint32_t i = 0; i < bytes; i++) out[i] = i < CLASSIC_BYTES ? state[i] : 0xff;
  encrypt_if_on(start(device), out, bytes, 0);
}
