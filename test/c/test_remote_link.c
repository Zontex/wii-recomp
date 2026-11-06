// Real devices through the link (remote_link.c) and the virtual Bluetooth chip: a fake
// device links, calls the console, passes reports both ways, drops, wakes, is hung up
// on, and is asked what it is. Compiled with bluetooth.c, bluetooth_channels.c,
// wii_remote.c, remote_link.c and gaps.c. The argument is a file for remembered slots.
#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "chip.h"
#include "bluetooth.h"
#include "remote_link.h"
#include "input.h"
#include "title.h"
#include "wii_remote.h"

const Title *title = &(Title){.virtual_bluetooth = 1, .calibration_flag = 1};
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
uint32_t input_buttons(int player) { return 0; }
int input_connected(int player) { return player == 0; }
void input_snapshot(int player, PlayerState *out) { memset(out, 0, sizeof *out); }
Attachment input_attachment(int player) { return ATTACHMENT_CLASSIC; }
const char *input_attachment_name(Attachment attachment) { return "classic"; }
float input_battery_level(int player) { return 1; }
int input_board_enabled(void) { return 1; }
int input_motionplus(int player) { return 0; }
void attachment_plain_state(int device, const PlayerState *player, uint8_t out[6]) { memset(out, 0, 6); }
void input_set_outputs(int player, int rumble, uint8_t lights) {}
void attachment_plug(int device, Attachment kind) {}
void attachment_read(int device, uint32_t address, uint8_t *out, uint32_t bytes) { memset(out, 0, bytes); }
void attachment_write(int device, uint32_t address, const uint8_t *data, uint32_t bytes) {}
void attachment_fill(int device, const PlayerState *player, uint8_t *out, uint32_t bytes) {}
void board_read(uint32_t address, uint8_t *out, uint32_t bytes) {}
void board_write(uint32_t address, const uint8_t *data, uint32_t bytes) {}
void board_fill(uint8_t *out, uint32_t bytes) {}
void camera_read(int device, uint32_t address, uint8_t *out, uint32_t bytes) {}
void camera_write(int device, uint32_t address, const uint8_t *data, uint32_t bytes) {}
void camera_fill(int device, float x, float y, uint8_t *out, uint32_t bytes) {}
static uint64_t now;
uint64_t time_base(void) { return now; }
static int backend_started, searches;
void link_backend_start(int mode) { backend_started = 1; }
void link_backend_search(void) { searches++; }
void link_backend_handover(void) {}
void link_loopback_start(void) {}

// The fake device: it keeps the last report the game sent, and whether it was turned off.
typedef struct { uint8_t last[32]; uint32_t last_bytes; int writes, closed, powered_off; } Fake;
static int fake_write(void *device, const uint8_t *report, uint32_t bytes) {
  Fake *fake = device;
  memcpy(fake->last, report, bytes);
  fake->last_bytes = bytes;
  fake->writes++;
  return (int)bytes;
}
static int fake_close(void *device, int power_off) {
  Fake *fake = device;
  fake->closed++;
  fake->powered_off = power_off;
  return 1;
}
static const LinkOps fake_ops = {fake_write, fake_close};

static const uint8_t slot0[6] = {0x01, 0x00, 0x1f, 0x19, 0x00, 0x00};
static void answer_call(void) { chip_answer_call(0); }

static void test_slots_and_probing(void) {
  // A remote's slot is remembered by its address.
  const uint8_t remembered[6] = {0x00, 0x19, 0x1d, 0x11, 0x22, 0x33};
  CHECK_EQ(link_choose_slot(remembered, LINK_REMOTE), 0);
  FILE *file = fopen(getenv("WII_RECOMP_REMOTE_SLOTS"), "w");
  fprintf(file, "00:19:1d:11:22:33 2\n");
  fclose(file);
  CHECK_EQ(link_choose_slot(remembered, LINK_REMOTE), 2);
  CHECK_EQ(link_choose_slot(remembered, LINK_BOARD), BLUETOOTH_BOARD);

  // A device with no name is asked: the board answers 04 02 from 0xa400fe.
  static Fake board;
  const uint8_t board_address[6] = {0x00, 0x23, 0x31, 0x00, 0x00, 0x09};
  int id = link_attach(board_address, 0, &fake_ops, &board);
  CHECK(id >= 0);
  CHECK_EQ(board.last[0], 0x16);
  CHECK_EQ(board.last[4], 0xf0);
  const uint8_t written[] = {0x22, 0, 0, 0x16, 0};
  link_input(id, written, sizeof written);
  CHECK_EQ(board.last[4], 0xfb);
  link_input(id, written, sizeof written);
  CHECK_EQ(board.last[0], 0x17);
  CHECK(!link_linked(BLUETOOTH_BOARD));
  const uint8_t answer[] = {0x21, 0, 0, 0x10, 0x00, 0xfe, 0x04, 0x02};
  link_input(id, answer, sizeof answer);
  CHECK(link_linked(BLUETOOTH_BOARD));
  CHECK_EQ(link_kind(BLUETOOTH_BOARD), LINK_BOARD);
  CHECK(link_take_wake(BLUETOOTH_BOARD));

  // Data reports give way when the game falls behind; a status still gets in.
  uint8_t report[LINK_REPORT_BYTES] = {0x32};
  for (int i = 0; i < 60; i++) link_input(id, report, 11);
  report[0] = 0x20;
  link_input(id, report, 7);
  int data = 0, status = 0;
  for (int bytes; (bytes = link_poll(BLUETOOTH_BOARD, report)) > 0;) {
    if (report[0] == 0x32) data++;
    if (report[0] == 0x20) status++;
  }
  CHECK_EQ(data, 48);
  CHECK_EQ(status, 1);
  link_lost(id);
  CHECK(link_take_lost(BLUETOOTH_BOARD));
  CHECK(!link_linked(BLUETOOTH_BOARD));

  // One that never says what it is, is a remote.
  static Fake quiet;
  const uint8_t quiet_address[6] = {0x00, 0x23, 0x31, 0x00, 0x00, 0x0a};
  id = link_attach(quiet_address, 0, &fake_ops, &quiet);
  now = 3ull * 60750000;
  link_update(now);
  CHECK_EQ(link_kind(0), LINK_REMOTE);
  link_lost(id);
  link_take_lost(0);
  now = 0;
}

static void test_through_the_chip(void) {
  static Fake fake;
  int id = link_attach(slot0, "Nintendo RVL-CNT-01", &fake_ops, &fake);
  CHECK_EQ(link_kind(0), LINK_REMOTE);
  chip_command(0x0c03, 0, 0);  // Reset.
  CHECK_EQ(chip_event(), 0x0e);
  CHECK(backend_started);

  // Page scan on: the device calls.
  const uint8_t scan[] = {0x02};
  chip_command(0x0c1a, scan, sizeof scan);
  CHECK_EQ(chip_event(), 0x0e);
  bluetooth_update(now);
  CHECK_EQ(chip_event(), 0x04);
  CHECK(!memcmp(host(CHIP_EVENT + 2), slot0, 6));

  // Nobody answers for ten seconds: it calls again.
  now += 11ull * 60750000;
  bluetooth_update(now);
  CHECK_EQ(chip_event(), 0x04);

  // Answered: once interrupt is up, the game asks for its status, and its reports reach the game.
  answer_call();
  CHECK_EQ(fake.last[0], 0x15);
  const uint8_t status[] = {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8};
  link_input(id, status, sizeof status);
  bluetooth_update(now);
  CHECK(chip_collect(0x82));
  CHECK_EQ(rd8(CHIP_EVENT + 6), 0x51);
  CHECK_EQ(rd8(CHIP_EVENT + 8), 0xa1);
  CHECK_EQ(rd8(CHIP_EVENT + 9), 0x20);
  CHECK_EQ(rd8(CHIP_EVENT + 15), 0xc8);

  // The game's reports reach the device unchanged, by data or SET_REPORT.
  const uint8_t leds[] = {0xa2, 0x11, 0x10};
  chip_to_device(0, 0x41, leds, sizeof leds);
  CHECK_EQ(fake.last_bytes, 2);
  CHECK_EQ(fake.last[0], 0x11);
  CHECK_EQ(fake.last[1], 0x10);
  const uint8_t set_report[] = {0x52, 0x12, 0x00, 0x31};
  chip_to_device(0, 0x40, set_report, sizeof set_report);
  CHECK_EQ(fake.last[0], 0x12);
  CHECK_EQ(fake.last[2], 0x31);

  // Its batteries go: the game hears the link drop.
  link_lost(id);
  bluetooth_update(now);
  CHECK_EQ(chip_event(), 0x05);
  CHECK_EQ(rd8(CHIP_EVENT + 5), 0x13);
  CHECK_EQ(fake.closed, 1);

  // Back again: it calls, and is answered.
  id = link_attach(slot0, "Nintendo RVL-CNT-01", &fake_ops, &fake);
  bluetooth_update(now);
  CHECK_EQ(chip_event(), 0x04);
  answer_call();

  // The game hangs up: the device is turned off and stays quiet.
  const uint8_t hang_up[] = {0x00, 0x01, 0x13};
  chip_command(0x0406, hang_up, sizeof hang_up);
  CHECK_EQ(chip_event(), 0x0f);
  CHECK_EQ(chip_event(), 0x05);
  CHECK_EQ(fake.closed, 2);
  CHECK_EQ(fake.powered_off, 1);
  CHECK(!link_linked(0));
  bluetooth_update(now);
  CHECK_EQ(chip_event(), 0);

  // A search by the game asks the backend to look too.
  const uint8_t inquiry[] = {0x33, 0x8b, 0x9e, 8, 0};
  chip_command(0x0401, inquiry, sizeof inquiry);
  CHECK_EQ(searches, 1);
}

int main(int count, char **arguments) {
  memory_start();
  setenv("WII_RECOMP_REMOTES", "real", 1);
  setenv("WII_RECOMP_REMOTE_SLOTS", arguments[1], 1);
  test_slots_and_probing();
  test_through_the_chip();
  return finish();
}
