// Stands in for the Wii Remote and Balance Board at the point the game reads them,
// so no Bluetooth is needed. Channel 0 is the remote, channel 3 the board.
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "title.h"
#include "cpu.h"
#include "gx_inspect.h"
#ifdef GX_WITH_VULKAN
#include "gx_shot.h"
#endif
#include "input.h"

enum {
  CHANNEL_REMOTE = 0, CHANNEL_BOARD = 3, CHANNELS = 4,
  DEVICE_REMOTE = 0, DEVICE_BOARD = 3, NO_DEVICE = 253, NOT_CONNECTED = -1,
  STATUS_BYTES = 0xb0, HOLD = 0, TRIGGER = 4, RELEASE = 8, ACCELERATION = 0x0c, ACCELERATION_SIZE = 0x18,
  POINTER = 0x20, HORIZON = 0x34, DISTANCE = 0x48, DEVICE_TYPE = 0x5c, ERROR = 0x5d, POINTER_VALID = 0x5e,
  DATA_FORMAT = 0x5f, BOARD_TOTAL = 0x60, BOARD_SENSORS = 0x68, BOARD_AVERAGES = 0x88,
  POINTER_SEEN = 2, CORE_ACCELERATION_POINTER = 3, PRESS_MS = 100, TICKS_PER_MS = 60750,
  SENSOR_STATUS_BYTES = 0x34, SENSOR_DEVICE = 0x28, SENSOR_RAW = 0x2a, SENSOR_TEMPERATURE = 0x32, SENSOR_BATTERY = 0x33,
  RAW_PER_KILOGRAM = 100, ROOM_TEMPERATURE = 25, BATTERY_FULL = 200,
};

static const float DISTANCE_METERS = 1.5f;

static struct {
  uint32_t held;
  uint32_t pulsed;
  uint64_t pulse_until;
  float pointer_x, pointer_y;
  int standing;
  float kilograms, lean_x, lean_y;
} state = {.kilograms = 60};
static uint32_t previous[CHANNELS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void input_hold(uint32_t buttons, int down) {
  pthread_mutex_lock(&lock);
  state.held = down ? state.held | buttons : state.held & ~buttons;
  pthread_mutex_unlock(&lock);
}

void input_press(uint32_t buttons) {
  pthread_mutex_lock(&lock);
  state.pulsed |= buttons;
  state.pulse_until = time_base() + PRESS_MS * TICKS_PER_MS;
  pthread_mutex_unlock(&lock);
}

void input_pointer(float x, float y) {
  pthread_mutex_lock(&lock);
  state.pointer_x = x;
  state.pointer_y = y;
  pthread_mutex_unlock(&lock);
}

void input_pointer_now(float *x, float *y) {
  pthread_mutex_lock(&lock);
  *x = state.pointer_x;
  *y = state.pointer_y;
  pthread_mutex_unlock(&lock);
}

void input_board(int standing, float kilograms, float lean_x, float lean_y) {
  pthread_mutex_lock(&lock);
  state.standing = standing;
  state.kilograms = kilograms;
  state.lean_x = lean_x;
  state.lean_y = lean_y;
  pthread_mutex_unlock(&lock);
}

void input_board_nudge(float lean_x, float lean_y) {
  pthread_mutex_lock(&lock);
  state.lean_x = lean_x;
  state.lean_y = lean_y;
  pthread_mutex_unlock(&lock);
}

void input_board_toggle(void) {
  pthread_mutex_lock(&lock);
  state.standing = !state.standing;
  pthread_mutex_unlock(&lock);
}

static void write_float(uint32_t address, float value) {
  union { float f; uint32_t bits; } word = {.f = value};
  wr32(address, word.bits);
}

static void write_double(uint32_t address, double value) {
  union { double d; uint64_t bits; } word = {.d = value};
  wr32(address, (uint32_t)(word.bits >> 32));
  wr32(address + 4, (uint32_t)word.bits);
}

static uint32_t current_buttons(void) {
  if (state.pulsed && time_base() >= state.pulse_until) state.pulsed = 0;
  return state.held | state.pulsed;
}

// No source moves the sticks yet, so they sit centred.
void input_sticks(float out[4]) { memset(out, 0, sizeof(float) * 4); }

uint32_t input_buttons(void) {
  pthread_mutex_lock(&lock);
  uint32_t buttons = current_buttons();
  pthread_mutex_unlock(&lock);
  return buttons;
}

static void fill_common(uint32_t out, int channel, uint32_t buttons) {
  for (uint32_t i = 0; i < STATUS_BYTES; i += 4) wr32(out + i, 0);
  wr32(out + HOLD, buttons);
  wr32(out + TRIGGER, buttons & ~previous[channel]);
  wr32(out + RELEASE, previous[channel] & ~buttons);
  previous[channel] = buttons;
}

static void fill_remote(uint32_t out) {
  fill_common(out, CHANNEL_REMOTE, current_buttons());
  write_float(out + ACCELERATION + 4, -1);  // Held still, pointing at the screen.
  write_float(out + ACCELERATION_SIZE, 1);
  write_float(out + POINTER, state.pointer_x);
  write_float(out + POINTER + 4, state.pointer_y);
  write_float(out + HORIZON, 1);
  write_float(out + DISTANCE, DISTANCE_METERS);
  wr8(out + DEVICE_TYPE, DEVICE_REMOTE);
  wr8(out + ERROR, 0);
  wr8(out + POINTER_VALID, POINTER_SEEN);
  wr8(out + DATA_FORMAT, CORE_ACCELERATION_POINTER);
}

// Sensors sit at front right, back right, front left, back left.
static void read_sensors(double sensors[4]) {
  double total = state.standing ? state.kilograms : 0;
  double right = (1 + state.lean_x) / 2, front = (1 + state.lean_y) / 2;
  sensors[0] = total * right * front;
  sensors[1] = total * right * (1 - front);
  sensors[2] = total * (1 - right) * front;
  sensors[3] = total * (1 - right) * (1 - front);
}

void input_board_now(double kilograms[4]) {
  pthread_mutex_lock(&lock);
  read_sensors(kilograms);
  pthread_mutex_unlock(&lock);
}

static void fill_board(uint32_t out) {
  fill_common(out, CHANNEL_BOARD, 0);
  wr8(out + DEVICE_TYPE, DEVICE_BOARD);
  double sensors[4], total = state.standing ? state.kilograms : 0;
  read_sensors(sensors);
  write_double(out + BOARD_TOTAL, total);
  for (uint32_t i = 0; i < 4; i++) {
    write_double(out + BOARD_SENSORS + i * 8, sensors[i]);
    write_double(out + BOARD_AVERAGES + i * 8, sensors[i]);
  }
}

// KPADRead(channel, statuses, count): newest status first, returns how many.
// Only a game built for the Balance Board has this flag. Others scan every channel
// and shouldn't find a device they know nothing about.
static int has_board(void) { return title->calibration_flag != 0; }

static int is_connected(int channel) { return channel == CHANNEL_REMOTE || (channel == CHANNEL_BOARD && has_board()); }

void input_read(void) {
  int channel = (int)cpu.r[3];
  uint32_t out = cpu.r[4];
  pthread_mutex_lock(&lock);
  if (channel == CHANNEL_REMOTE) fill_remote(out);
  else if (channel == CHANNEL_BOARD && has_board()) fill_board(out);
  pthread_mutex_unlock(&lock);
  cpu.r[3] = is_connected(channel);
}

// WPADProbe(channel, type): 0 and a device type if something is connected.
void input_probe(void) {
  int channel = (int)cpu.r[3];
  uint32_t type = cpu.r[4];
  int connected = is_connected(channel);
  if (type) wr32(type, !connected ? NO_DEVICE : channel == CHANNEL_BOARD ? DEVICE_BOARD : DEVICE_REMOTE);
  cpu.r[3] = connected ? 0 : (uint32_t)NOT_CONNECTED;
}

// WPADRead(channel, status). Only the game's board code calls this directly.
void input_board_sensors(void) {
  uint32_t out = cpu.r[4];
  double sensors[4];
  pthread_mutex_lock(&lock);
  read_sensors(sensors);
  pthread_mutex_unlock(&lock);
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
  pthread_mutex_lock(&lock);
  read_sensors(sensors);
  pthread_mutex_unlock(&lock);
  for (uint32_t i = 0; i < count && i < 4; i++) write_double(out + i * 8, sensors[i]);
  cpu.r[3] = 0;
}

static uint32_t button_named(const char *name) {
  static const struct { const char *name; uint32_t bit; } names[] = {
    {"left", BUTTON_LEFT}, {"right", BUTTON_RIGHT}, {"down", BUTTON_DOWN}, {"up", BUTTON_UP}, {"plus", BUTTON_PLUS},
    {"two", BUTTON_TWO}, {"one", BUTTON_ONE}, {"b", BUTTON_B}, {"a", BUTTON_A}, {"minus", BUTTON_MINUS}, {"home", BUTTON_HOME},
  };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (!strcmp(name, names[i].name)) return names[i].bit;
  }
  return 0;
}

// One command per line: press a, hold a, release a, pointer x y, board on kg x y, board off,
// shot <file> to save the next picture.
static void run_command(const char *line) {
  char word[16], name[16], path[1024];
  float x = 0, y = 0, kilograms = state.kilograms;
  int first, last;
  if (sscanf(line, "press %15s", name) == 1) input_press(button_named(name));
  else if (sscanf(line, "hold %15s", name) == 1) input_hold(button_named(name), 1);
  else if (sscanf(line, "release %15s", name) == 1) input_hold(button_named(name), 0);
  else if (sscanf(line, "pointer %f %f", &x, &y) == 2) input_pointer(x, y);
  else if (sscanf(line, "board %15s %f %f %f", word, &kilograms, &x, &y) >= 1) {
    input_board(!strcmp(word, "on"), kilograms, x, y);
  } else if (!strncmp(line, "dump", 4)) gx_inspect_dump();
  else if (sscanf(line, "skip %d %d", &first, &last) == 2) gx_inspect_skip(first, last);
  else if (sscanf(line, "shot %1023s", path) == 1) {
#ifdef GX_WITH_VULKAN
    gx_shot(path);
#else
    fprintf(stderr, "control: shot needs the Vulkan renderer\n");
#endif
  }
  else fprintf(stderr, "control: unknown command: %s", line);
}

static void *control_loop(void *path) {
  // Opened for writing too, so it stays open between senders.
  FILE *pipe_file = fdopen(open(path, O_RDWR), "r");
  char line[1200];  // Room for a shot command with a long path.
  while (pipe_file && fgets(line, sizeof line, pipe_file)) run_command(line);
  return 0;
}

void input_start_control_pipe(const char *path) {
  static pthread_t thread;
  unlink(path);
  mkfifo(path, 0600);
  pthread_create(&thread, 0, control_loop, (void *)path);
}
