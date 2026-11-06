// The Linux HID backend (remote_link_hidraw.c) with a socket standing in for a remote's
// hidraw device: what counts as a remote, its address, refusing one the kernel's own
// driver holds, reports both ways, sleeping when the game hangs up and waking with a
// press, and going away. Built and run on Linux only, with remote_link.c. The argument is
// a folder for files.
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "harness.h"
#include "remote_link.h"
#include "remote_link_hidraw.h"

uint64_t time_base(void) { return 0; }
void link_loopback_start(void) {}
void link_backend_start(int mode) {}
void link_backend_search(void) {}
void link_backend_handover(void) {}

static void pause_briefly(void) { nanosleep(&(struct timespec){0, 2000000}, 0); }

static int poll_report(int slot, uint8_t *report) {
  for (int tries = 0; tries < 500; tries++) {
    int bytes = link_poll(slot, report);
    if (bytes) return bytes;
    pause_briefly();
  }
  return 0;
}

int main(int count, char **arguments) {
  memory_start();
  setenv("WII_RECOMP_REMOTES", "real", 1);
  char path[1024];
  snprintf(path, sizeof path, "%s/remotes.txt", arguments[1]);
  setenv("WII_RECOMP_REMOTE_SLOTS", path, 1);

  // Remotes and boards over Bluetooth: Nintendo's, as either product number.
  CHECK(link_hidraw_is_remote(5, 0x057e, 0x0306));
  CHECK(link_hidraw_is_remote(5, 0x057e, 0x0330));
  CHECK(!link_hidraw_is_remote(5, 0x057e, 0x2009));  // A Switch Pro Controller is SDL's.
  CHECK(!link_hidraw_is_remote(3, 0x057e, 0x0306));  // By USB: a DolphinBar or the like, not ours.
  uint8_t address[6];
  CHECK(link_hidraw_address("00:1f:32:aa:bb:cc", address));
  CHECK_EQ(address[0], 0x00);
  CHECK_EQ(address[5], 0xcc);
  CHECK(!link_hidraw_address("00:1f:32:aa:bb", address));
  CHECK(!link_hidraw_address("", address));

  // The driver holding a device, from sysfs, as a tree of links.
  char folder[1024], link[1100], driver[64];
  snprintf(folder, sizeof folder, "%s/class/hidraw3/device", arguments[1]);
  snprintf(path, sizeof path, "%s/class", arguments[1]);
  mkdir(path, 0700);
  snprintf(path, sizeof path, "%s/class/hidraw3", arguments[1]);
  mkdir(path, 0700);
  mkdir(folder, 0700);
  snprintf(link, sizeof link, "%s/driver", folder);
  CHECK_EQ(symlink("../../../bus/hid/drivers/wiimote", link), 0);
  snprintf(path, sizeof path, "%s/class", arguments[1]);
  CHECK(link_hidraw_driver(path, "hidraw3", driver, sizeof driver));
  CHECK(!strcmp(driver, "wiimote"));
  CHECK(!link_hidraw_driver(path, "hidraw4", driver, sizeof driver));

  // A remote: its reports arrive with their IDs, as the core wants, and the game's leave so.
  int ends[2];
  socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ends);
  const uint8_t remote_address[6] = {0x00, 0x1f, 0x32, 0xaa, 0xbb, 0xcc};
  CHECK(link_hidraw_adopt(ends[0], "hidraw7", remote_address, "Nintendo RVL-CNT-01") >= 0);
  CHECK(link_linked(0));
  CHECK_EQ(link_kind(0), LINK_REMOTE);
  const uint8_t status[] = {0x20, 0x00, 0x00, 0x02, 0x00, 0x00, 0xc8};
  CHECK_EQ(write(ends[1], status, sizeof status), sizeof status);
  uint8_t report[LINK_REPORT_BYTES] = {0};
  CHECK_EQ(poll_report(0, report), 7);
  CHECK_EQ(report[0], 0x20);
  const uint8_t leds[] = {0x11, 0x10};
  link_send(0, leds, sizeof leds);
  uint8_t sent[32] = {0};
  CHECK_EQ(read(ends[1], sent, sizeof sent), 2);
  CHECK_EQ(sent[0], 0x11);

  // The game hangs up: from here the remote can't be turned off, so it sleeps, and a
  // press wakes it.
  link_take_wake(0);
  link_guest_disconnected(0);
  CHECK(link_linked(0));
  const uint8_t pressed[] = {0x30, 0x00, 0x08};
  CHECK_EQ(write(ends[1], pressed, sizeof pressed), sizeof pressed);
  int woke = 0;
  for (int tries = 0; tries < 500 && !woke; tries++) {
    woke = link_take_wake(0);
    if (!woke) pause_briefly();
  }
  CHECK(woke);

  // A board says so in its name, and takes the board's slot.
  int board_ends[2];
  socketpair(AF_UNIX, SOCK_SEQPACKET, 0, board_ends);
  const uint8_t board_address[6] = {0x00, 0x23, 0x31, 0x00, 0x00, 0x09};
  CHECK(link_hidraw_adopt(board_ends[0], "hidraw8", board_address, "Nintendo RVL-WBC-01") >= 0);
  CHECK(link_linked(BLUETOOTH_BOARD));
  CHECK_EQ(link_kind(BLUETOOTH_BOARD), LINK_BOARD);

  // Out of range: the device closes, and the slot is freed.
  close(ends[1]);
  int lost = 0;
  for (int tries = 0; tries < 500 && !lost; tries++) {
    lost = link_take_lost(0);
    if (!lost) pause_briefly();
  }
  CHECK(lost);
  CHECK(!link_linked(0));
  CHECK(link_linked(BLUETOOTH_BOARD));
  return finish();
}
