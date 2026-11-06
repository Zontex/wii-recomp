// Loopback (WII_RECOMP_REMOTES=loopback) through the virtual Bluetooth chip: the simulated
// remote and board, on their own thread behind the link, call the console, answer status
// and memory reads, send data reports with the buttons held, go off when the game hangs
// up, and come back with a press. The bytes checked are what a game reads. Compiled with
// bluetooth.c, bluetooth_channels.c, wii_remote.c, remote_link.c, remote_link_loopback.c,
// wii_attachment.c, wii_attachment_key.c, wii_board.c, wii_camera.c, input.c and gaps.c. The
// argument is a file for remembered slots.
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "harness.h"
#include "chip.h"
#include "input.h"
#include "remote_link.h"
#include "title.h"
#include "wii_board.h"
#include "wii_remote.h"

const Title *title = &(Title){.virtual_bluetooth = 1, .calibration_flag = 1};
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
static _Atomic uint64_t now;
uint64_t time_base(void) { return now; }
void link_backend_start(int mode) {}
void link_backend_search(void) {}
void link_backend_handover(void) {}

enum { TICKS_PER_MILLISECOND = 60750, TRIES = 3000 };

// Time passes for both sides: the devices' thread reports by it, and the chip polls.
static void pass_time(void) {
  now += TICKS_PER_MILLISECOND;
  bluetooth_update(now);
  nanosleep(&(struct timespec){0, 200000}, 0);
}

static int wait_event(int code) {
  for (int i = 0; i < TRIES; i++, pass_time()) {
    int got = chip_event();
    if (got == code) return 1;
  }
  return 0;
}

// The next HID input report of this kind from this device, whose bytes from the report
// ID on start with `prefix`. Copied to `out`.
static int wait_report(int device, const uint8_t *prefix, uint32_t bytes, uint8_t out[24]) {
  for (int i = 0; i < TRIES; i++, pass_time()) {
    while (chip_collect(0x82)) {
      if (chip_packet_device() != device || rd8(CHIP_EVENT + 8) != 0xa1) continue;
      if (!memcmp(host(CHIP_EVENT + 9), prefix, bytes)) {
        memcpy(out, host(CHIP_EVENT + 9), 24);
        return 1;
      }
    }
  }
  return 0;
}

int main(int count, char **arguments) {
  memory_start();
  setenv("WII_RECOMP_REMOTES", "loopback", 1);
  setenv("WII_RECOMP_REMOTE_SLOTS", arguments[1], 1);
  uint8_t report[24];

  chip_command(0x0c03, 0, 0);  // Reset: the devices plug in behind the link.
  CHECK_EQ(chip_event(), 0x0e);
  pass_time();  // The chip hears they linked, as it does between the game's commands.
  const uint8_t scan[] = {0x02};
  chip_command(0x0c1a, scan, sizeof scan);
  CHECK_EQ(chip_event(), 0x0e);

  // The remote calls first, from slot 0, and gives its status once its channels are up:
  // no buttons, a Classic Controller plugged in, the battery full.
  CHECK(wait_event(0x04));
  uint8_t address[6];
  chip_address(0, address);
  CHECK(!memcmp(host(CHIP_EVENT + 2), address, 6));
  chip_answer_call(0);
  const uint8_t status[] = {0x20, 0x00, 0x00, 0x02, 0x00, 0x00, 0xc8};
  CHECK(wait_report(0, status, sizeof status, report));

  // Then the board, from slot 4.
  CHECK(wait_event(0x04));
  chip_address(BLUETOOTH_BOARD, address);
  CHECK(!memcmp(host(CHIP_EVENT + 2), address, 6));
  chip_answer_call(BLUETOOTH_BOARD);
  CHECK(wait_report(BLUETOOTH_BOARD, status, sizeof status, report));

  // Each answers a read of its attachment's identity.
  const uint8_t read_identity[] = {0x17, 0x04, 0xa4, 0x00, 0xfa, 0x00, 0x06};
  chip_output(0, read_identity, sizeof read_identity);
  const uint8_t classic[] = {0x21, 0x00, 0x00, 0x50, 0x00, 0xfa, 0x00, 0x00, 0xa4, 0x20, 0x01, 0x01};
  CHECK(wait_report(0, classic, sizeof classic, report));
  chip_output(BLUETOOTH_BOARD, read_identity, sizeof read_identity);
  const uint8_t board[] = {0x21, 0x00, 0x00, 0x50, 0x00, 0xfa, 0x00, 0x00, 0xa4, 0x20, 0x04, 0x02};
  CHECK(wait_report(BLUETOOTH_BOARD, board, sizeof board, report));

  // A read of the remote's calibration: zero 0x80 and 1 g at 0x9a on each axis.
  const uint8_t read_calibration[] = {0x17, 0x00, 0x00, 0x00, 0x16, 0x00, 0x0a};
  chip_output(0, read_calibration, sizeof read_calibration);
  const uint8_t calibration[] = {0x21, 0x00, 0x00, 0x90, 0x00, 0x16, 0x80, 0x80, 0x80, 0x00, 0x9a, 0x9a, 0x9a, 0x00, 0x40};
  CHECK(wait_report(0, calibration, sizeof calibration, report));

  // Buttons and motion, continuously: A held, the remote still and flat.
  const uint8_t motion_mode[] = {0x12, 0x04, 0x31};
  chip_output(0, motion_mode, sizeof motion_mode);
  const uint8_t acknowledged[] = {0x22, 0x00, 0x00, 0x12, 0x00};
  CHECK(wait_report(0, acknowledged, sizeof acknowledged, report));
  input_hold(0, BUTTON_A, 1);
  const uint8_t pressed[] = {0x31, 0x00, 0x08, 0x80, 0x80, 0x9a};
  CHECK(wait_report(0, pressed, sizeof pressed, report));
  input_hold(0, BUTTON_A, 0);

  // The board's weights where a remote's attachment would be.
  const uint8_t board_mode[] = {0x12, 0x04, 0x32};
  chip_output(BLUETOOTH_BOARD, board_mode, sizeof board_mode);
  CHECK(wait_report(BLUETOOTH_BOARD, acknowledged, sizeof acknowledged, report));
  input_board(1, 100, 0.2f, -0.4f);
  double kilograms[4];
  input_board_now(kilograms);
  uint8_t weights[11], expected[11] = {0x32, 0x00, 0x00};
  board_reading(kilograms, weights);
  memcpy(expected + 3, weights, 8);
  CHECK(wait_report(BLUETOOTH_BOARD, expected, sizeof expected, report));

  // The game hangs up on the remote: it turns off, and a press brings it back calling.
  const uint8_t hang_up[] = {0x00, 0x01, 0x13};
  chip_command(0x0406, hang_up, sizeof hang_up);
  CHECK_EQ(chip_event(), 0x0f);
  CHECK(wait_event(0x05));
  CHECK(!link_linked(0));
  input_hold(0, BUTTON_A, 1);
  CHECK(wait_event(0x04));
  chip_address(0, address);
  CHECK(!memcmp(host(CHIP_EVENT + 2), address, 6));
  input_hold(0, BUTTON_A, 0);
  return finish();
}
