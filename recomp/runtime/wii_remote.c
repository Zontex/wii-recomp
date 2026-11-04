// The simulated Wii Remotes and Balance Board: each answers the game's output reports
// and sends its state. A board is a remote inside, with no buttons, motion or camera
// to speak of, and its weight sensors where a remote's attachment would be.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bluetooth.h"
#include "gaps.h"
#include "input.h"
#include "wii_attachment.h"
#include "wii_board.h"
#include "wii_camera.h"
#include "wii_remote.h"

enum {
  OUTPUT_RUMBLE = 0x10, OUTPUT_LEDS = 0x11, OUTPUT_DATA_MODE = 0x12, OUTPUT_STATUS = 0x15, OUTPUT_WRITE = 0x16, OUTPUT_READ = 0x17,
  REPORT_STATUS = 0x20, REPORT_MEMORY = 0x21, REPORT_ACK = 0x22, REPORT_BYTES = 22, MEMORY_PIECE = 16, MEMORY_HEADER = 6,
  ADDRESS_SPACE_SHIFT = 24, ATTACHMENT_SPACE = 0x04a4, CAMERA_SPACE = 0x04b0, READ_NO_DEVICE = 7, ACK_WANTED = 2, CONTINUOUS = 4,
  STATUS_ATTACHMENT = 2, BATTERY_FULL = 0xc8, CALIBRATION_START = 0x16, CALIBRATION_COPY = 0x20, REPORT_TICKS = 607500,  // 100 a second.
};

// Where each data mode puts things: motion, camera and attachment bytes, by position.
typedef struct {
  uint8_t mode, motion, attachment_at, attachment_bytes, bytes, camera_at, camera_bytes;
} Mode;

static const Mode modes[] = {
  {0x30, 0, 0, 0, 3, 0, 0}, {0x31, 1, 0, 0, 6, 0, 0}, {0x32, 0, 3, 8, 11, 0, 0}, {0x33, 1, 0, 0, 18, 6, 12},
  {0x34, 0, 3, 19, 22, 0, 0}, {0x35, 1, 6, 16, 22, 0, 0}, {0x36, 0, 13, 9, 22, 3, 10}, {0x37, 1, 16, 6, 22, 6, 10},
  {0x3d, 0, 1, 21, 22, 0, 0},  // Attachment only: its 21 bytes take the buttons' place too.
};

static struct {
  const Mode *mode;
  int continuous;
  uint64_t next_report;
  uint8_t last_report[REPORT_BYTES];
} remotes[BLUETOOTH_DEVICES];

static int is_board(int device) { return device == BLUETOOTH_BOARD; }

// The keyboard, pipe and controllers press the first remote's buttons. The board has a
// single button, its power switch, which nothing presses.
static void buttons_into(int device, uint8_t *to) {
  uint32_t buttons = device == 0 ? input_buttons() : 0;
  to[0] = (uint8_t)(buttons & 0x1f);
  to[1] = (uint8_t)((buttons >> 8) & 0x9f);
}

static void report_status(int device) {
  uint8_t report[7] = {REPORT_STATUS};
  buttons_into(device, report + 1);
  report[3] = STATUS_ATTACHMENT;
  report[6] = BATTERY_FULL;
  bluetooth_send_report(device, report, sizeof report);
}

static void acknowledge(int device, int output) {
  uint8_t report[5] = {REPORT_ACK};
  buttons_into(device, report + 1);
  report[3] = (uint8_t)output;
  bluetooth_send_report(device, report, sizeof report);
}

// Calibration sits at the start of memory: zero and gravity for the three axes, which is
// what makes a still remote read as 1 g, then a check byte the library verifies.
static void read_memory(uint32_t address, uint8_t *out, uint32_t bytes) {
  uint8_t calibration[] = {0x80, 0x80, 0x80, 0, 0x9a, 0x9a, 0x9a, 0, 0x40, 0x55};
  for (uint32_t i = 0; i + 1 < sizeof calibration; i++) calibration[sizeof calibration - 1] += calibration[i];
  for (uint32_t i = 0; i < bytes; i++) {
    uint32_t at = address + i;
    out[i] = 0;
    if (at >= CALIBRATION_START && at < CALIBRATION_START + sizeof calibration) out[i] = calibration[at - CALIBRATION_START];
    else if (at >= CALIBRATION_COPY && at < CALIBRATION_COPY + sizeof calibration) out[i] = calibration[at - CALIBRATION_COPY];
  }
}

static void attachment_bytes(int device, uint32_t offset, uint8_t *out, uint32_t bytes) {
  if (is_board(device)) board_read(offset, out, bytes);
  else attachment_read(offset, out, bytes);
}

// Reads come back in pieces of at most 16 bytes.
static void report_read(int device, uint32_t address, uint32_t bytes) {
  int from_attachment = address >> 16 == ATTACHMENT_SPACE, from_camera = address >> 16 == CAMERA_SPACE && !is_board(device);
  uint32_t space = address >> ADDRESS_SPACE_SHIFT, offset = address & 0xffff, done = 0;
  do {
    uint32_t part = bytes - done > MEMORY_PIECE ? MEMORY_PIECE : bytes - done;
    uint8_t report[REPORT_BYTES] = {REPORT_MEMORY};
    buttons_into(device, report + 1);
    report[3] = (uint8_t)((part - 1) << 4);
    report[4] = (uint8_t)((offset + done) >> 8);
    report[5] = (uint8_t)(offset + done);
    if (space && !from_attachment && !from_camera) {
      // Other registers belong to devices that aren't there, such as Wii MotionPlus.
      report[3] |= READ_NO_DEVICE;
      bluetooth_send_report(device, report, sizeof report);
      return;
    }
    if (from_attachment) attachment_bytes(device, offset + done, report + MEMORY_HEADER, part);
    else if (from_camera) camera_read(offset + done, report + MEMORY_HEADER, part);
    else read_memory(offset + done, report + MEMORY_HEADER, part);
    bluetooth_send_report(device, report, sizeof report);
    done += part;
  } while (done < bytes);
}

static uint32_t big32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void wii_remote_connected(int device) { report_status(device); }

void wii_remote_output(int device, const uint8_t *data, uint32_t bytes) {
  int report = data[0];
  (void)bytes;
  if (report == OUTPUT_DATA_MODE) {
    remotes[device].continuous = data[1] & CONTINUOUS;
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
      if (modes[i].mode == data[2]) remotes[device].mode = &modes[i];
    }
    if (data[2] == 0x3e || data[2] == 0x3f) gap_hit("ctl.0x3e_0x3f_interleaved", data[2]);
    memset(remotes[device].last_report, 0, sizeof remotes[device].last_report);
    acknowledge(device, report);
  } else if (report == OUTPUT_STATUS) {
    report_status(device);
  } else if (report == OUTPUT_READ) {
    report_read(device, big32(data + 1), (uint32_t)data[5] << 8 | data[6]);
  } else if (report == OUTPUT_WRITE) {
    uint32_t address = big32(data + 1);
    uint32_t bytes_written = data[5] > MEMORY_PIECE ? MEMORY_PIECE : data[5];
    if (address >> 16 == ATTACHMENT_SPACE) {
      if (is_board(device)) board_write(address & 0xffff, data + 6, bytes_written);
      else attachment_write(address & 0xffff, data + 6, bytes_written);
    } else if (address >> 16 == CAMERA_SPACE && !is_board(device)) {
      camera_write(address & 0xffff, data + 6, bytes_written);
    }
    acknowledge(device, report);
  } else if (report == OUTPUT_LEDS || report == OUTPUT_RUMBLE) {
    if (data[1] & ACK_WANTED) acknowledge(device, report);
  } else {
    if (report == 0x14 && (data[1] & 4)) gap_hit("ctl.0x14_speaker_enable", data[1]);
    else if (report == 0x18) gap_hit("ctl.0x18_speaker_data", 0);
    acknowledge(device, report);
  }
}

// Sends a device's current state as often as a real one does. Out of continuous mode,
// only when something in the report changed, such as a button, where it points, or the
// weight on the board.
void wii_remote_update(int device, uint64_t now) {
  const Mode *mode = remotes[device].mode;
  if (!mode || now < remotes[device].next_report) return;
  remotes[device].next_report = now + REPORT_TICKS;
  uint8_t report[REPORT_BYTES] = {mode->mode};
  buttons_into(device, report + 1);
  if (mode->motion) {
    report[3] = 0x80;  // Held still and flat: one g on the Z axis.
    report[4] = 0x80;
    report[5] = 0x9a;
  }
  if (mode->camera_bytes && !is_board(device)) {
    float x, y;
    input_pointer_now(&x, &y);
    camera_fill(x, y, report + mode->camera_at, mode->camera_bytes);
  } else if (mode->camera_bytes) {
    memset(report + mode->camera_at, 0xff, mode->camera_bytes);
  }
  if (mode->attachment_bytes) {
    if (is_board(device)) board_fill(report + mode->attachment_at, mode->attachment_bytes);
    else attachment_fill(report + mode->attachment_at, mode->attachment_bytes);
  }
  if (!remotes[device].continuous && !memcmp(report, remotes[device].last_report, mode->bytes)) return;
  if (getenv("WII_RECOMP_BLUETOOTH_LOG") && (report[1] != remotes[device].last_report[1] || report[2] != remotes[device].last_report[2])) {
    fprintf(stderr, "bluetooth: device %d sends buttons %02x%02x at %llu\n", device, report[2], report[1], (unsigned long long)now);
  }
  memcpy(remotes[device].last_report, report, sizeof report);
  bluetooth_send_report(device, report, mode->bytes);
}
