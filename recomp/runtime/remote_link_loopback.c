// Loopback (WII_RECOMP_REMOTES=loopback): the simulated remote and board, plugged in
// behind the link as though they were real devices. They run on a thread of their own,
// as a real device's reports arrive on a backend's thread, so a game played this way
// goes through everything a real device does: the rings, finding a slot, calling the
// console, turning off when the game hangs up, waking with a press, and carrying on into
// the next title. Only the radio is missing.
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "input.h"
#include "remote_link.h"
#include "title.h"
#include "wii_remote.h"

enum { OUTPUTS = 64, DEVICES = 2 };

typedef struct {
  int simulated;  // Its number to the simulated devices: 0, or BLUETOOTH_BOARD.
  _Atomic int id, on;
} Loop;

static Loop loops[DEVICES] = {{0, -1, 0}, {BLUETOOTH_BOARD, -1, 0}};
static const char *names[DEVICES] = {"Nintendo RVL-CNT-01", "Nintendo RVL-WBC-01"};
static const uint8_t addresses[DEVICES][6] = {{0x00, 0x17, 0xab, 0x00, 0x00, 0x01}, {0x00, 0x17, 0xab, 0x00, 0x00, 0x02}};

// The game's reports, from the emulation thread, waiting for the devices' thread.
static struct {
  Loop *loop;
  uint8_t report[LINK_REPORT_BYTES];
  uint32_t bytes;
} outputs[OUTPUTS];
static int output_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int loop_write(void *device, const uint8_t *report, uint32_t bytes) {
  Loop *loop = device;
  if (!loop->on) return -1;
  pthread_mutex_lock(&lock);
  int room = output_count < OUTPUTS;
  if (room) {
    outputs[output_count].loop = loop;
    memcpy(outputs[output_count].report, report, bytes > LINK_REPORT_BYTES ? LINK_REPORT_BYTES : bytes);
    outputs[output_count++].bytes = bytes > LINK_REPORT_BYTES ? LINK_REPORT_BYTES : bytes;
  }
  pthread_mutex_unlock(&lock);
  return room ? (int)bytes : -1;
}

// A loopback device always turns off, as a real remote does when the console hangs up.
static int loop_close(void *device, int power_off) {
  Loop *loop = device;
  (void)power_off;
  loop->on = 0;
  loop->id = -1;
  return 1;
}

static const LinkOps ops = {loop_write, loop_close};

static void plug_in(Loop *loop) {
  loop->on = 1;
  int which = (int)(loop - loops);
  loop->id = link_attach(addresses[which], names[which], &ops, loop);
}

// The simulated devices' reports go into the link instead of straight to the game.
static void into_link(int simulated, const uint8_t *report, uint32_t bytes) {
  for (int i = 0; i < DEVICES; i++) {
    if (loops[i].simulated == simulated && loops[i].on) link_input(loops[i].id, report, bytes);
  }
}

static void *devices_thread(void *unused) {
  (void)unused;
  const struct timespec millisecond = {0, 1000000};
  for (;;) {
    // A device the game turned off comes back when a button is pressed.
    if (input_buttons()) {
      for (int i = 0; i < DEVICES; i++) {
        if (!loops[i].on && (i == 0 || title->calibration_flag)) plug_in(&loops[i]);
      }
    }
    pthread_mutex_lock(&lock);
    int count = output_count;
    static typeof(outputs) taken;
    memcpy(taken, outputs, sizeof outputs[0] * (size_t)count);
    output_count = 0;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < count; i++) {
      if (taken[i].loop->on) wii_remote_simulated_output(taken[i].loop->simulated, taken[i].report, taken[i].bytes);
    }
    for (int i = 0; i < DEVICES; i++) {
      if (loops[i].on) wii_remote_simulated_update(loops[i].simulated, time_base());
    }
    nanosleep(&millisecond, 0);
  }
  return 0;
}

void link_loopback_start(void) {
  wii_remote_simulated_sink(into_link);
  plug_in(&loops[0]);
  if (title->calibration_flag) plug_in(&loops[1]);
  pthread_t thread;
  pthread_create(&thread, 0, devices_thread, 0);
  pthread_detach(thread);
}
