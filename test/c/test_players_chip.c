// Four players through the virtual Bluetooth chip: each player who joins makes a simulated
// remote call from their own slot, each remote's reports carry its own player's buttons
// and attachment, and a player who leaves is a remote whose link dropped. Compiled with
// bluetooth.c, bluetooth_channels.c, wii_remote.c, remote_link.c, wii_attachment.c,
// wii_attachment_key.c, wii_board.c, wii_camera.c, input.c and gaps.c.
#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "chip.h"
#include "input.h"
#include "remote_link.h"
#include "title.h"

const Title *title = &(Title){.virtual_bluetooth = 1};
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
static uint64_t now;
uint64_t time_base(void) { return now; }
void link_backend_start(int mode) {}
void link_backend_search(void) {}
void link_backend_handover(void) {}
void link_loopback_start(void) {}

enum { REPORT_TICKS = 607500 };

// The newest input report from a device, after time passes; 0 if none came.
static int report_from(int device, uint8_t out[24]) {
  int found = 0;
  now += REPORT_TICKS;
  bluetooth_update(now);
  while (chip_collect(0x82)) {
    if (chip_packet_device() == device && rd8(CHIP_EVENT + 8) == 0xa1) {
      memcpy(out, host(CHIP_EVENT + 9), 24);
      found = 1;
    }
  }
  return found;
}

static void expect_call(int device) {
  bluetooth_update(now);
  CHECK_EQ(chip_event(), 0x04);
  uint8_t address[6];
  chip_address(device, address);
  CHECK(!memcmp(host(CHIP_EVENT + 2), address, 6));
  chip_answer_call(device);
  // Its status comes first: a Classic Controller plugged in, the battery full.
  uint8_t report[24];
  CHECK(chip_collect(0x82));
  CHECK_EQ(chip_packet_device(), device);
  memcpy(report, host(CHIP_EVENT + 9), 24);
  CHECK_EQ(report[0], 0x20);
  CHECK_EQ(report[3], 0x02);
  CHECK_EQ(report[6], 0xc8);
}

int main(void) {
  memory_start();
  setenv("WII_RECOMP_REMOTES", "simulated", 1);
  chip_command(0x0c03, 0, 0);
  CHECK_EQ(chip_event(), 0x0e);
  const uint8_t scan[] = {0x02};
  chip_command(0x0c1a, scan, sizeof scan);
  CHECK_EQ(chip_event(), 0x0e);

  // Player 1 is there from the start; nobody else calls until they join.
  expect_call(0);
  bluetooth_update(now);
  CHECK_EQ(chip_event(), 0);
  for (int player = 1; player < PLAYERS; player++) input_connect(player, 1);
  for (int player = 1; player < PLAYERS; player++) expect_call(player);

  // Each remote sends buttons, motion and attachment; each carries its own player's.
  static const uint32_t held[PLAYERS] = {BUTTON_A, BUTTON_B, BUTTON_ONE | BUTTON_UP, BUTTON_HOME};
  const uint8_t mode[] = {0x12, 0x04, 0x35};
  for (int player = 0; player < PLAYERS; player++) {
    chip_output(player, mode, sizeof mode);
    input_hold(player, held[player], 1);
  }
  input_stick(3, 1, 1);
  uint8_t report[24];
  for (int player = 0; player < PLAYERS; player++) {
    CHECK(report_from(player, report));
    CHECK_EQ(report[0], 0x35);
    CHECK_EQ(report[1], held[player] & 0x1f);
    CHECK_EQ(report[2], (held[player] >> 8) & 0x9f);
    // The Classic Controller's left stick, from byte 6 of the report on.
    CHECK_EQ(report[6] & 63, player == 3 ? 63 : 32);
  }

  // Player 3 leaves: the game hears their remote's link drop, and only theirs.
  input_connect(2, 0);
  bluetooth_update(now);
  CHECK_EQ(chip_event(), 0x05);
  CHECK_EQ(rd8(CHIP_EVENT + 3), 0x02);  // Connection 0x102.
  CHECK_EQ(rd8(CHIP_EVENT + 4), 0x01);
  CHECK_EQ(rd8(CHIP_EVENT + 5), 0x13);
  CHECK_EQ(chip_event(), 0);
  CHECK(!report_from(2, report));
  CHECK(report_from(3, report));

  // And back: their remote calls again.
  input_connect(2, 1);
  expect_call(2);
  return finish();
}
