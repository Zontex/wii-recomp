// Stands in for the Wii Remotes and Balance Board at the point the game reads them, in
// games built without --virtual-bluetooth, so no Bluetooth is needed. Channel n is
// player n+1's remote, except that a game built for the board finds it on channel 3.
#include <stdint.h>

#include "cpu.h"
#include "input.h"
#include "title.h"

enum {
  CHANNEL_BOARD = 3, CHANNELS = 4,
  DEVICE_REMOTE = 0, DEVICE_BOARD = 3, NO_DEVICE = 253, NOT_CONNECTED = -1,
  STATUS_BYTES = 0xb0, HOLD = 0, TRIGGER = 4, RELEASE = 8, ACCELERATION = 0x0c, ACCELERATION_SIZE = 0x18,
  POINTER = 0x20, HORIZON = 0x34, DISTANCE = 0x48, DEVICE_TYPE = 0x5c, ERROR = 0x5d, POINTER_VALID = 0x5e,
  DATA_FORMAT = 0x5f, BOARD_TOTAL = 0x60, BOARD_SENSORS = 0x68, BOARD_AVERAGES = 0x88,
  POINTER_SEEN = 2, CORE_ACCELERATION_POINTER = 3,
  SENSOR_STATUS_BYTES = 0x34, SENSOR_DEVICE = 0x28, SENSOR_RAW = 0x2a, SENSOR_TEMPERATURE = 0x32, SENSOR_BATTERY = 0x33,
  RAW_PER_KILOGRAM = 100, ROOM_TEMPERATURE = 25, BATTERY_FULL = 200,
};

static const float DISTANCE_METERS = 1.5f;
static uint32_t previous[CHANNELS];  // Only the emulation thread reads statuses.

static void write_float(uint32_t address, float value) {
  union { float f; uint32_t bits; } word = {.f = value};
  wr32(address, word.bits);
}

static void write_double(uint32_t address, double value) {
  union { double d; uint64_t bits; } word = {.d = value};
  wr32(address, (uint32_t)(word.bits >> 32));
  wr32(address + 4, (uint32_t)word.bits);
}

static void fill_common(uint32_t out, int channel, uint32_t buttons) {
  for (uint32_t i = 0; i < STATUS_BYTES; i += 4) wr32(out + i, 0);
  wr32(out + HOLD, buttons);
  wr32(out + TRIGGER, buttons & ~previous[channel]);
  wr32(out + RELEASE, previous[channel] & ~buttons);
  previous[channel] = buttons;
}

static void fill_remote(uint32_t out, int channel) {
  PlayerState player;
  input_snapshot(channel, &player);
  fill_common(out, channel, player.buttons);
  write_float(out + ACCELERATION + 4, -1);  // Held still, pointing at the screen.
  write_float(out + ACCELERATION_SIZE, 1);
  write_float(out + POINTER, player.pointer_x);
  write_float(out + POINTER + 4, player.pointer_y);
  write_float(out + HORIZON, 1);
  write_float(out + DISTANCE, DISTANCE_METERS);
  wr8(out + DEVICE_TYPE, DEVICE_REMOTE);
  wr8(out + ERROR, 0);
  wr8(out + POINTER_VALID, POINTER_SEEN);
  wr8(out + DATA_FORMAT, CORE_ACCELERATION_POINTER);
}

static void fill_board(uint32_t out) {
  fill_common(out, CHANNEL_BOARD, 0);
  wr8(out + DEVICE_TYPE, DEVICE_BOARD);
  double sensors[4];
  input_board_now(sensors);
  write_double(out + BOARD_TOTAL, input_board_total());
  for (uint32_t i = 0; i < 4; i++) {
    write_double(out + BOARD_SENSORS + i * 8, sensors[i]);
    write_double(out + BOARD_AVERAGES + i * 8, sensors[i]);
  }
}

// Only a game built for the Balance Board has this flag. Others scan every channel
// and shouldn't find a device they know nothing about.
static int has_board(void) { return title->calibration_flag != 0; }
static int is_board(int channel) { return channel == CHANNEL_BOARD && has_board(); }
static int is_connected(int channel) {
  return channel >= 0 && channel < CHANNELS && (is_board(channel) || input_connected(channel));
}

// KPADRead(channel, statuses, count): newest status first, returns how many.
void input_read(void) {
  int channel = (int)cpu.r[3];
  uint32_t out = cpu.r[4];
  int connected = is_connected(channel);
  if (connected && is_board(channel)) fill_board(out);
  else if (connected) fill_remote(out, channel);
  cpu.r[3] = (uint32_t)connected;
}

// WPADProbe(channel, type): 0 and a device type if something is connected.
void input_probe(void) {
  int channel = (int)cpu.r[3];
  uint32_t type = cpu.r[4];
  int connected = is_connected(channel);
  if (type) wr32(type, !connected ? NO_DEVICE : is_board(channel) ? DEVICE_BOARD : DEVICE_REMOTE);
  cpu.r[3] = connected ? 0 : (uint32_t)NOT_CONNECTED;
}

// WPADRead(channel, status). Only the game's board code calls this directly.
void input_board_sensors(void) {
  uint32_t out = cpu.r[4];
  double sensors[4];
  input_board_now(sensors);
  for (uint32_t i = 0; i < SENSOR_STATUS_BYTES; i++) wr8(out + i, 0);
  wr8(out + SENSOR_DEVICE, DEVICE_BOARD);
  for (uint32_t i = 0; i < 4; i++) wr16(out + SENSOR_RAW + i * 2, (uint16_t)(sensors[i] * RAW_PER_KILOGRAM));
  wr8(out + SENSOR_TEMPERATURE, ROOM_TEMPERATURE);
  wr8(out + SENSOR_BATTERY, BATTERY_FULL);
}

// Asks for calibration data with a callback that only raises this flag.
void input_board_calibration(void) {
  uint32_t buffer = cpu.r[4], bytes = cpu.r[6];
  for (uint32_t i = 0; i < bytes; i++) wr8(buffer + i, 0);
  wr32(title->calibration_flag, 1);
  cpu.r[3] = 0;
}

// WBCRead(status, kilograms, count): the four sensors, as doubles.
void input_board_weights(void) {
  uint32_t out = cpu.r[4], count = cpu.r[5];
  double sensors[4];
  input_board_now(sensors);
  for (uint32_t i = 0; i < count && i < 4; i++) write_double(out + i * 8, sensors[i]);
  cpu.r[3] = 0;
}
