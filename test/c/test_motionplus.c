// A simulated Wii MotionPlus through the virtual Bluetooth chip, as a game uses one: found
// at its own address, switched on with a Nunchuk passing through, the attachment pin
// dropping and coming back with a status report each time, the check's answers, gyro
// readings taking turns with the Nunchuk's, and switched off again. Compiled with
// bluetooth.c, bluetooth_channels.c, wii_remote.c, wii_motionplus.c, remote_link.c,
// wii_attachment.c, wii_attachment_key.c, wii_board.c, wii_camera.c, input.c and gaps.c.
// Prints its calibration block to stderr, for the test to check its CRC with zlib's.
#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "chip.h"
#include "input.h"
#include "remote_link.h"
#include "title.h"
#include "wii_motionplus.h"
#include "wii_motionplus_challenge.h"
#include "wii_remote.h"

const Title *title = &(Title){.virtual_bluetooth = 1};
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
static uint64_t now;
uint64_t time_base(void) { return now; }
void link_backend_start(int mode) {}
void link_backend_search(void) {}
void link_backend_handover(void) {}
void link_loopback_start(void) {}

enum { TICKS_PER_MS = 60750 };

// The attachment bit of every status report, in order, however they were collected.
static int pins[64], pin_count;

// Time passes; the reports that came, in order, into `reports`.
static int pass(int milliseconds, uint8_t reports[][24], int most) {
  int count = 0;
  for (int i = 0; i < milliseconds; i++) {
    now += TICKS_PER_MS;
    bluetooth_update(now);
    while (chip_collect(0x82)) {
      if (chip_packet_device() != 0 || rd8(CHIP_EVENT + 8) != 0xa1) continue;
      if (rd8(CHIP_EVENT + 9) == 0x20 && pin_count < 64) pins[pin_count++] = rd8(CHIP_EVENT + 12) & 2;
      if (count < most) memcpy(reports[count], host(CHIP_EVENT + 9), 24);
      count++;
    }
  }
  return count < most ? count : most;
}

static int count_kind(uint8_t reports[][24], int count, int kind) {
  int found = 0;
  for (int i = 0; i < count; i++) found += reports[i][0] == kind;
  return found;
}

// Reads `bytes` from the wire at an address, and the error the remote gives.
static int read_wire(uint32_t address, uint8_t *out, int bytes) {
  const uint8_t ask[] = {0x17, 0x04, (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address, 0, (uint8_t)bytes};
  chip_output(0, ask, sizeof ask);
  uint8_t reports[16][24];
  int count = pass(1, reports, 16), got = 0, error = 0;
  for (int i = 0; i < count; i++) {
    if (reports[i][0] != 0x21) continue;
    error = reports[i][3] & 0x0f;
    int part = (reports[i][3] >> 4) + 1;
    if (!error) memcpy(out + got, reports[i] + 6, (size_t)part);
    got += part;
  }
  return error;
}

static int write_wire(uint32_t address, uint8_t value) {
  const uint8_t write[22] = {0x16, 0x04, (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address, 1, value};
  chip_output(0, write, sizeof write);
  uint8_t reports[16][24];
  int count = pass(1, reports, 16);
  for (int i = 0; i < count; i++) {
    if (reports[i][0] == 0x22 && reports[i][3] == 0x16) return reports[i][4];
  }
  return -1;
}

static uint32_t reading(const uint8_t *d, int axis) {  // pitch, roll, yaw
  return axis == 2 ? (uint32_t)(d[0] | (d[3] >> 2) << 8) : axis == 1 ? (uint32_t)(d[1] | (d[4] >> 2) << 8) : (uint32_t)(d[2] | (d[5] >> 2) << 8);
}

int main(void) {
  memory_start();
  setenv("WII_RECOMP_REMOTES", "simulated", 1);

  // Gyro readings on their own: still is 0x2000 on every axis, fine steps; 100 degrees a
  // second is fine still, 1000 coarse.
  uint8_t gyro[6];
  motionplus_gyro_bytes((float[3]){0, 0, 0}, 1, gyro);
  CHECK_EQ(reading(gyro, 0), 0x2000);
  CHECK_EQ(reading(gyro, 2), 0x2000);
  CHECK_EQ(gyro[3] & 3, 3);  // Pitch and yaw slow.
  CHECK_EQ(gyro[4] & 3, 3);  // An attachment behind it; roll slow.
  CHECK_EQ(gyro[5] & 3, 2);  // A MotionPlus reading.
  motionplus_gyro_bytes((float[3]){100, 0, 1000}, 0, gyro);
  CHECK_EQ(reading(gyro, 0), 0x2000 + 1612);
  CHECK_EQ(gyro[3] & 1, 1);
  CHECK_EQ(reading(gyro, 2), 0x2000 + 3627);
  CHECK_EQ(gyro[3] >> 1 & 1, 0);  // Yaw coarse.
  CHECK_EQ(gyro[4] & 1, 0);
  motionplus_gyro_bytes((float[3]){-9000, 0, 0}, 0, gyro);
  CHECK_EQ(reading(gyro, 0), 0);  // Clamped.
  // A Nunchuk passing through loses its motion's lowest bits to the flags.
  uint8_t chuk[6] = {0x80, 0x80, 0x80, 0x80, 0xb3, 0x01 | 0x80};  // Z up... C held; x's low bits 0, z's top bit set.
  motionplus_pass_through(ATTACHMENT_NUNCHUK, chuk);
  CHECK_EQ(chuk[5] >> 6 & 1, 1);  // Bit 7 moves to 6.
  CHECK_EQ(chuk[5] >> 7 & 1, 1);  // Byte 4's bit 0 moves to 7.
  CHECK_EQ(chuk[5] >> 2 & 1, 1);  // Z moves to bit 2.
  CHECK_EQ(chuk[5] >> 3 & 1, 0);  // C to bit 3.

  // Player 1's remote has a MotionPlus with a Nunchuk behind it.
  input_attach(0, ATTACHMENT_NUNCHUK);
  input_attach_motionplus(0, 1);
  chip_command(0x0c03, 0, 0);
  CHECK_EQ(chip_event(), 0x0e);
  const uint8_t scan[] = {0x02};
  chip_command(0x0c1a, scan, sizeof scan);
  CHECK_EQ(chip_event(), 0x0e);
  bluetooth_update(now);
  CHECK_EQ(chip_event(), 0x04);
  chip_answer_call(0);
  uint8_t reports[64][24];
  int count = pass(1, reports, 64);
  CHECK(count >= 1 && reports[0][0] == 0x20 && (reports[0][3] & 2));  // The Nunchuk shows through the inactive MotionPlus.

  // Found at its own address, with its calibration.
  uint8_t data[64];
  CHECK_EQ(read_wire(0xa600fa, data, 6), 0);
  const uint8_t inactive[6] = {0x00, 0x00, 0xa6, 0x20, 0x00, 0x05};
  CHECK(!memcmp(data, inactive, 6));
  CHECK_EQ(read_wire(0xa60020, data, 32), 0);
  fprintf(stderr, "calibration:");
  for (int i = 0; i < 32; i++) fprintf(stderr, " %02x", data[i]);
  fprintf(stderr, "\n");
  CHECK_EQ(data[0], 0x80);  // Yaw zero, 0x8000.
  CHECK_EQ(data[6], 0x3c);  // Yaw scale.
  CHECK_EQ(data[8], 0xc4);  // Roll scale.
  CHECK_EQ(data[12], 200);  // 1200 degrees a second, over six.
  CHECK_EQ(data[28], 45);   // 270.
  CHECK_EQ(read_wire(0xa400fa, data, 6), 0);
  CHECK_EQ(data[4], 0x00);  // At the attachment's address, the Nunchuk.

  // Switched on with the Nunchuk passing through: the pin drops, then comes back, each
  // with a status report; then it answers at the attachment's address and not its own.
  pin_count = 0;
  CHECK_EQ(write_wire(0xa600fe, 0x05), 0);
  pass(40, reports, 64);
  CHECK_EQ(pin_count, 2);
  CHECK_EQ(pins[0], 0);
  CHECK_EQ(pins[1], 2);
  CHECK_EQ(read_wire(0xa400fa, data, 6), 0);
  const uint8_t active[6] = {0x00, 0x00, 0xa4, 0x20, 0x05, 0x05};
  CHECK(!memcmp(data, active, 6));
  CHECK_EQ(read_wire(0xa600fa, data, 6), 7);
  // It copied the Nunchuk's identity and calibration where the game reads them.
  CHECK_EQ(read_wire(0xa400f6, data, 4), 0);
  CHECK_EQ(data[0], 0x00);  // Byte 4 of the Nunchuk's identity.
  CHECK_EQ(read_wire(0xa40040, data, 16), 0);
  CHECK_EQ(data[4], 0xb3);  // The Nunchuk's one g.

  // The check: x after a while, then y1 when asked for it.
  CHECK_EQ(read_wire(0xa400f7, data, 1), 0);
  CHECK_EQ(data[0], 0x02);
  pass(510, reports, 64);
  CHECK_EQ(read_wire(0xa400f7, data, 1), 0);
  CHECK_EQ(data[0], 0x0e);
  CHECK_EQ(read_wire(0xa40050, data, 64), 0);
  CHECK(!memcmp(data, MOTIONPLUS_X, 64));
  CHECK_EQ(write_wire(0xa400f1, 1), 0);
  CHECK_EQ(read_wire(0xa400f7, data, 1), 0);
  CHECK_EQ(data[0], 0x14);
  pass(510, reports, 64);
  CHECK_EQ(read_wire(0xa400f7, data, 1), 0);
  CHECK_EQ(data[0], 0x1a);
  CHECK_EQ(read_wire(0xa40050, data, 64), 0);
  CHECK(!memcmp(data, MOTIONPLUS_Y1, 64));

  // Readings: the gyro's and the Nunchuk's take turns.
  const uint8_t mode[] = {0x12, 0x04, 0x35};
  chip_output(0, mode, sizeof mode);
  input_gyro(0, (float[3]){0, 0, 90});
  input_hold(0, BUTTON_NUNCHUK_Z, 1);
  count = pass(40, reports, 64);
  int gyro_reports = 0, nunchuk_reports = 0;
  for (int i = 0; i < count; i++) {
    if (reports[i][0] != 0x35) continue;
    const uint8_t *d = reports[i] + 6;
    if (d[5] & 2) {
      gyro_reports++;
      CHECK_EQ(reading(d, 2), 0x2000 + 1451);  // 90 degrees a second: 1.5708 * 923.5.
      CHECK_EQ(d[4] & 1, 1);  // The Nunchuk is there.
    } else {
      nunchuk_reports++;
      CHECK_EQ(d[0], 0x80);  // The stick in the middle.
      CHECK_EQ(d[5] >> 2 & 1, 0);  // Z held, in its moved place.
    }
  }
  CHECK(gyro_reports >= 1 && nunchuk_reports >= 1);
  CHECK(gyro_reports - nunchuk_reports <= 1 && nunchuk_reports - gyro_reports <= 1);

  // Switched off by the game starting the attachment: the pin drops, and the Nunchuk is back.
  pin_count = 0;
  CHECK_EQ(write_wire(0xa400f0, 0x55), 0);
  pass(40, reports, 64);
  CHECK_EQ(pin_count, 2);
  CHECK_EQ(pins[0], 0);
  CHECK_EQ(pins[1], 2);
  CHECK_EQ(read_wire(0xa400fa, data, 6), 0);
  CHECK_EQ(data[2], 0xa4);
  CHECK_EQ(data[4], 0x00);
  CHECK_EQ(data[5], 0x00);  // The Nunchuk's own identity again.
  CHECK_EQ(read_wire(0xa600fa, data, 6), 0);
  CHECK_EQ(data[2], 0xa6);

  // Unplugged: a status report, and nothing at its address.
  pin_count = 0;
  input_attach_motionplus(0, 0);
  count = pass(5, reports, 64);
  CHECK_EQ(count_kind(reports, count, 0x20), 1);
  CHECK_EQ(pins[0], 2);  // The Nunchuk is still there.
  CHECK_EQ(read_wire(0xa600fa, data, 6), 7);
  CHECK_EQ(write_wire(0xa600fe, 0x04), 7);
  return finish();
}
