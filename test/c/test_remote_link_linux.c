// The Linux backend (remote_link_linux.c) with sockets standing in for a remote's two
// L2CAP channels: reports both ways, the hand-over to the next title, the game hanging
// up, and the remote going away. Built and run on Linux only, with remote_link.c.
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "harness.h"
#include "remote_link.h"

uint64_t time_base(void) { return 0; }
void link_loopback_start(void) {}
int link_linux_adopt(const uint8_t address[6], const char *name, int control, int interrupt);
int link_linux_take_over(const char *list);

static void pause_briefly(void) { nanosleep(&(struct timespec){0, 2000000}, 0); }

static int poll_report(int slot, uint8_t *report) {
  for (int tries = 0; tries < 500; tries++) {
    int bytes = link_poll(slot, report);
    if (bytes) return bytes;
    pause_briefly();
  }
  return 0;
}

static void pair_of(int sockets[2]) { socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets); }

int main(int count, char **arguments) {
  memory_start();
  setenv("WII_RECOMP_REMOTES", "real", 1);
  setenv("WII_RECOMP_REMOTE_SLOTS", arguments[1], 1);
  int control[2], interrupt[2];
  pair_of(control);
  pair_of(interrupt);
  const uint8_t address[6] = {0x00, 0x1f, 0x32, 0xaa, 0xbb, 0xcc};
  CHECK(link_linux_adopt(address, "Nintendo RVL-CNT-01", control[0], interrupt[0]) >= 0);
  CHECK(link_linked(0));

  // The remote's reports arrive without their 0xa1, and the game's leave with 0xa2.
  const uint8_t status[] = {0xa1, 0x20, 0x00, 0x00, 0x02, 0x00, 0x00, 0xc8};
  CHECK_EQ(write(interrupt[1], status, sizeof status), sizeof status);
  uint8_t report[LINK_REPORT_BYTES] = {0};
  CHECK_EQ(poll_report(0, report), 7);
  CHECK_EQ(report[0], 0x20);
  CHECK_EQ(report[6], 0xc8);
  const uint8_t leds[] = {0x11, 0x10};
  link_send(0, leds, sizeof leds);
  uint8_t sent[32] = {0};
  CHECK_EQ(read(interrupt[1], sent, sizeof sent), 3);
  CHECK_EQ(sent[0], 0xa2);
  CHECK_EQ(sent[1], 0x11);

  // Handing over: the sockets' numbers go to the next title, kept open through exec.
  link_backend_handover();
  const char *list = getenv("WII_RECOMP_LINKS");
  CHECK(list && strstr(list, "00:1f:32:aa:bb:cc,") && strstr(list, "Nintendo RVL-CNT-01"));

  // The game hangs up: the channels close, which turns a remote off.
  link_guest_disconnected(0);
  CHECK(!link_linked(0));
  CHECK_EQ(read(interrupt[1], sent, sizeof sent), 0);
  close(interrupt[1]);
  close(control[1]);

  // The next title takes over a remote from the list, and finds it.
  pair_of(control);
  pair_of(interrupt);
  char handed[128];
  snprintf(handed, sizeof handed, "00:1f:32:aa:bb:cc,%d,%d,Nintendo RVL-CNT-01", control[0], interrupt[0]);
  CHECK_EQ(link_linux_take_over(handed), 1);
  CHECK(link_linked(0));
  CHECK_EQ(write(interrupt[1], status, sizeof status), sizeof status);
  CHECK_EQ(poll_report(0, report), 7);

  // Out of range: the channel closes, and the slot is freed.
  close(interrupt[1]);
  int lost = 0;
  for (int tries = 0; tries < 500 && !lost; tries++) {
    lost = link_take_lost(0);
    pause_briefly();
  }
  CHECK(lost);
  CHECK(!link_linked(0));

  // With no Bluetooth here, starting says what's missing and carries on.
  link_backend_start(LINK_REAL);
  pause_briefly();
  return finish();
}
