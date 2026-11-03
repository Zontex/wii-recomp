// The simulated Wii Remote: answers the game's output reports and sends its state.
#include <string.h>

#include "bluetooth.h"
#include "input.h"
#include "wii_attachment.h"
#include "wii_remote.h"

enum {
  OUTPUT_RUMBLE = 0x10, OUTPUT_LEDS = 0x11, OUTPUT_DATA_MODE = 0x12, OUTPUT_STATUS = 0x15, OUTPUT_WRITE = 0x16, OUTPUT_READ = 0x17,
  REPORT_STATUS = 0x20, REPORT_MEMORY = 0x21, REPORT_ACK = 0x22, REPORT_BYTES = 22, MEMORY_PIECE = 16, MEMORY_HEADER = 6,
  ADDRESS_SPACE_SHIFT = 24, ATTACHMENT_SPACE = 0x04a4, READ_NO_DEVICE = 7, ACK_WANTED = 2, CONTINUOUS = 4,
  STATUS_ATTACHMENT = 2, BATTERY_FULL = 0xc8, CALIBRATION_START = 0x16, CALIBRATION_COPY = 0x20, REPORT_TICKS = 607500,  // 100 a second.
};

// Where each data mode puts things: motion and attachment bytes, by position.
typedef struct {
  uint8_t mode, motion, attachment_at, attachment_bytes, bytes;
} Mode;

static const Mode modes[] = {
  {0x30, 0, 0, 0, 3}, {0x31, 1, 0, 0, 6}, {0x32, 0, 3, 8, 11}, {0x33, 1, 0, 0, 18}, {0x34, 0, 3, 19, 22}, {0x35, 1, 6, 16, 22}, {0x36, 0, 13, 9, 22}, {0x37, 1, 16, 6, 22},
};

static const Mode *mode_now;
static int continuous;
static uint32_t last_buttons = 0xffffffffu;
static uint64_t next_report;

static void buttons_into(uint8_t *to) {
  uint32_t buttons = input_buttons();
  to[0] = (uint8_t)(buttons & 0x1f);
  to[1] = (uint8_t)((buttons >> 8) & 0x9f);
}

static void report_status(void) {
  uint8_t report[7] = {REPORT_STATUS};
  buttons_into(report + 1);
  report[3] = STATUS_ATTACHMENT;
  report[6] = BATTERY_FULL;
  bluetooth_send_report(report, sizeof report);
}

static void acknowledge(int output) {
  uint8_t report[5] = {REPORT_ACK};
  buttons_into(report + 1);
  report[3] = (uint8_t)output;
  bluetooth_send_report(report, sizeof report);
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

// Reads come back in pieces of at most 16 bytes.
static void report_read(uint32_t address, uint32_t bytes) {
  int from_attachment = address >> 16 == ATTACHMENT_SPACE;
  uint32_t space = address >> ADDRESS_SPACE_SHIFT, offset = address & 0xffff, done = 0;
  do {
    uint32_t part = bytes - done > MEMORY_PIECE ? MEMORY_PIECE : bytes - done;
    uint8_t report[REPORT_BYTES] = {REPORT_MEMORY};
    buttons_into(report + 1);
    report[3] = (uint8_t)((part - 1) << 4);
    report[4] = (uint8_t)((offset + done) >> 8);
    report[5] = (uint8_t)(offset + done);
    if (space && !from_attachment) {
      // Other registers belong to devices that aren't there, such as Wii MotionPlus.
      report[3] |= READ_NO_DEVICE;
      bluetooth_send_report(report, sizeof report);
      return;
    }
    if (from_attachment) attachment_read(offset + done, report + MEMORY_HEADER, part);
    else read_memory(offset + done, report + MEMORY_HEADER, part);
    bluetooth_send_report(report, sizeof report);
    done += part;
  } while (done < bytes);
}

static uint32_t big32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void wii_remote_connected(void) { report_status(); }

void wii_remote_output(const uint8_t *data, uint32_t bytes) {
  int report = data[0];
  (void)bytes;
  if (report == OUTPUT_DATA_MODE) {
    continuous = data[1] & CONTINUOUS;
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
      if (modes[i].mode == data[2]) mode_now = &modes[i];
    }
    acknowledge(report);
  } else if (report == OUTPUT_STATUS) {
    report_status();
  } else if (report == OUTPUT_READ) {
    report_read(big32(data + 1), (uint32_t)data[5] << 8 | data[6]);
  } else if (report == OUTPUT_WRITE) {
    uint32_t address = big32(data + 1);
    if (address >> 16 == ATTACHMENT_SPACE) attachment_write(address & 0xffff, data + 6, data[5] > MEMORY_PIECE ? MEMORY_PIECE : data[5]);
    acknowledge(report);
  } else if (report == OUTPUT_LEDS || report == OUTPUT_RUMBLE) {
    if (data[1] & ACK_WANTED) acknowledge(report);
  } else {
    acknowledge(report);
  }
}

// Sends the remote's current state as often as a real one does.
void wii_remote_update(uint64_t now) {
  if (!mode_now || now < next_report) return;
  next_report = now + REPORT_TICKS;
  uint32_t buttons = input_buttons();
  if (!continuous && buttons == last_buttons) return;
  last_buttons = buttons;
  uint8_t report[REPORT_BYTES] = {mode_now->mode};
  buttons_into(report + 1);
  if (mode_now->motion) {
    report[3] = 0x80;  // Held still and flat: one g on the Z axis.
    report[4] = 0x80;
    report[5] = 0x9a;
  }
  if (mode_now->attachment_bytes) attachment_fill(report + mode_now->attachment_at, mode_now->attachment_bytes);
  bluetooth_send_report(report, mode_now->bytes);
}
