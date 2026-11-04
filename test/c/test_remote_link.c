// Real devices through the link (remote_link.c) and the virtual Bluetooth chip: a fake
// device links, calls the console, passes reports both ways, drops, wakes, is hung up
// on, and is asked what it is. Compiled with bluetooth.c, bluetooth_channels.c,
// wii_remote.c, remote_link.c and gaps.c. The argument is a file for remembered slots.
#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "bluetooth.h"
#include "ios.h"
#include "remote_link.h"
#include "title.h"
#include "wii_remote.h"

const Title *title = &(Title){.virtual_bluetooth = 1, .calibration_flag = 1};
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
uint32_t input_buttons(void) { return 0; }
void input_pointer_now(float *x, float *y) { *x = *y = 0; }
void attachment_read(uint32_t address, uint8_t *out, uint32_t bytes) { memset(out, 0, bytes); }
void attachment_write(uint32_t address, const uint8_t *data, uint32_t bytes) {}
void attachment_fill(uint8_t *out, uint32_t bytes) {}
void board_read(uint32_t address, uint8_t *out, uint32_t bytes) {}
void board_write(uint32_t address, const uint8_t *data, uint32_t bytes) {}
void board_fill(uint8_t *out, uint32_t bytes) {}
void camera_read(uint32_t address, uint8_t *out, uint32_t bytes) {}
void camera_write(uint32_t address, const uint8_t *data, uint32_t bytes) {}
void camera_fill(float x, float y, uint8_t *out, uint32_t bytes) {}
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

enum { COMMAND_REQUEST = 0x80001000u, EVENTS_REQUEST = 0x80001200u, DATA_REQUEST = 0x80001400u, COMMAND = 0x80002000u,
       ENDPOINT = 0x80002100u, EVENT = 0x80003000u, BUFFER = 0x80003400u };
static const uint8_t slot0[6] = {0x01, 0x00, 0x1f, 0x19, 0x00, 0x00};
static int pending[2], ready[2];

void ios_complete(uint32_t request, int result) {
  if (request == EVENTS_REQUEST) ready[0] = 1;
  if (request == DATA_REQUEST) ready[1] = 1;
}

static void set_up(uint32_t request, uint32_t code, uint32_t vector, uint32_t address, uint32_t bytes) {
  memset(host(request), 0, 0x200);
  wr32(request + 12, code);
  wr32(request + 24, (request + 0x100) & 0x7fffffff);
  wr32(request + 0x100 + vector * 8, address & 0x7fffffff);
  wr32(request + 0x100 + vector * 8 + 4, bytes);
}

static void command(int opcode, const uint8_t *parameters, uint32_t bytes) {
  wr8(COMMAND, opcode & 0xff);
  wr8(COMMAND + 1, opcode >> 8);
  wr8(COMMAND + 2, bytes);
  if (bytes) memcpy(host(COMMAND + 3), parameters, bytes);
  set_up(COMMAND_REQUEST, 0, 6, COMMAND, bytes + 3);
  bluetooth_request(IOS_IOCTLV, COMMAND_REQUEST);
}

static int collect(int endpoint) {
  int kind = endpoint == 0x82;
  uint32_t request = kind ? DATA_REQUEST : EVENTS_REQUEST, buffer = BUFFER + (uint32_t)kind * 0x200;
  if (!pending[kind]) {
    set_up(request, kind ? 1 : 2, 2, buffer, 0x100);
    wr32(request + 0x100, (ENDPOINT + (uint32_t)kind) & 0x7fffffff);
    wr8(ENDPOINT + (uint32_t)kind, endpoint);
    pending[kind] = 1;
    bluetooth_request(IOS_IOCTLV, request);
  }
  memset(host(EVENT), 0, 0x100);
  if (!ready[kind]) return 0;
  memcpy(host(EVENT), host(buffer), 0x100);
  pending[kind] = ready[kind] = 0;
  return 1;
}

static int event(void) { return collect(0x81) ? rd8(EVENT) : 0; }
static int signal_back(void) { return collect(0x82) && rd8(EVENT + 6) == 1 ? rd8(EVENT + 8) : 0; }

// The console's L2CAP signal, or HID data, to the device on connection 0x100.
static void to_device(int channel, const uint8_t *frame, uint32_t bytes) {
  uint8_t packet[64] = {0x00, 0x21, (uint8_t)(bytes + 4), 0, (uint8_t)bytes, 0, (uint8_t)channel, 0};
  memcpy(packet + 8, frame, bytes);
  bluetooth_receive_packet(0, packet);
}
static void signal_device(int code, int id, const uint8_t *body, uint32_t bytes) {
  uint8_t frame[32] = {(uint8_t)code, (uint8_t)id, (uint8_t)bytes, 0};
  memcpy(frame + 4, body, bytes);
  to_device(1, frame, bytes + 4);
}

// The game answers a device's call, through to both channels open.
static void answer_call(void) {
  command(0x0409, slot0, 6);
  CHECK_EQ(event(), 0x0f);
  CHECK_EQ(event(), 0x03);
  const uint8_t policy[] = {0x00, 0x01, 0x0f, 0x00};
  command(0x080d, policy, sizeof policy);
  CHECK_EQ(event(), 0x0e);
  CHECK_EQ(signal_back(), 2);  // The device opens control.
  const uint8_t control_open[] = {0x50, 0x00, 0x40, 0x00, 0, 0, 0, 0}, control_set[] = {0x40, 0x00, 0, 0, 0, 0};
  signal_device(3, 1, control_open, sizeof control_open);
  CHECK_EQ(signal_back(), 4);
  signal_device(5, 2, control_set, sizeof control_set);
  CHECK_EQ(signal_back(), 2);  // Then interrupt.
  const uint8_t interrupt_open[] = {0x51, 0x00, 0x41, 0x00, 0, 0, 0, 0}, interrupt_set[] = {0x41, 0x00, 0, 0, 0, 0};
  signal_device(3, 3, interrupt_open, sizeof interrupt_open);
  CHECK_EQ(signal_back(), 4);
  signal_device(5, 4, interrupt_set, sizeof interrupt_set);
}

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
  command(0x0c03, 0, 0);  // Reset.
  CHECK_EQ(event(), 0x0e);
  CHECK(backend_started);

  // Page scan on: the device calls.
  const uint8_t scan[] = {0x02};
  command(0x0c1a, scan, sizeof scan);
  CHECK_EQ(event(), 0x0e);
  bluetooth_update(now);
  CHECK_EQ(event(), 0x04);
  CHECK(!memcmp(host(EVENT + 2), slot0, 6));

  // Nobody answers for ten seconds: it calls again.
  now += 11ull * 60750000;
  bluetooth_update(now);
  CHECK_EQ(event(), 0x04);

  // Answered: once interrupt is up, the game asks for its status, and its reports reach the game.
  answer_call();
  CHECK_EQ(fake.last[0], 0x15);
  const uint8_t status[] = {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8};
  link_input(id, status, sizeof status);
  bluetooth_update(now);
  CHECK(collect(0x82));
  CHECK_EQ(rd8(EVENT + 6), 0x51);
  CHECK_EQ(rd8(EVENT + 8), 0xa1);
  CHECK_EQ(rd8(EVENT + 9), 0x20);
  CHECK_EQ(rd8(EVENT + 15), 0xc8);

  // The game's reports reach the device unchanged, by data or SET_REPORT.
  const uint8_t leds[] = {0xa2, 0x11, 0x10};
  to_device(0x41, leds, sizeof leds);
  CHECK_EQ(fake.last_bytes, 2);
  CHECK_EQ(fake.last[0], 0x11);
  CHECK_EQ(fake.last[1], 0x10);
  const uint8_t set_report[] = {0x52, 0x12, 0x00, 0x31};
  to_device(0x40, set_report, sizeof set_report);
  CHECK_EQ(fake.last[0], 0x12);
  CHECK_EQ(fake.last[2], 0x31);

  // Its batteries go: the game hears the link drop.
  link_lost(id);
  bluetooth_update(now);
  CHECK_EQ(event(), 0x05);
  CHECK_EQ(rd8(EVENT + 5), 0x13);
  CHECK_EQ(fake.closed, 1);

  // Back again: it calls, and is answered.
  id = link_attach(slot0, "Nintendo RVL-CNT-01", &fake_ops, &fake);
  bluetooth_update(now);
  CHECK_EQ(event(), 0x04);
  answer_call();

  // The game hangs up: the device is turned off and stays quiet.
  const uint8_t hang_up[] = {0x00, 0x01, 0x13};
  command(0x0406, hang_up, sizeof hang_up);
  CHECK_EQ(event(), 0x0f);
  CHECK_EQ(event(), 0x05);
  CHECK_EQ(fake.closed, 2);
  CHECK_EQ(fake.powered_off, 1);
  CHECK(!link_linked(0));
  bluetooth_update(now);
  CHECK_EQ(event(), 0);

  // A search by the game asks the backend to look too.
  const uint8_t inquiry[] = {0x33, 0x8b, 0x9e, 8, 0};
  command(0x0401, inquiry, sizeof inquiry);
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
