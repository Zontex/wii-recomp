// Hollywood's registers in ios.c, the reset button through stm.c, and the drive's eject.
#include "../../recomp/runtime/ios.c"
#include "../../recomp/runtime/stm.c"
#include "../../recomp/runtime/dvd.c"
#include "../harness.h"
#include <string.h>

uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
static int irq_up, disc_in = 1;
void raise_irq(int bit) { irq_up = 1; }
void clear_irq(int bit) { irq_up = 0; }
int irq_pending(void) { return irq_up; }
int drive_has_disc(void) { return disc_in; }
int drive_read(uint64_t o, uint32_t b, uint8_t *out) { return 1; }
int drive_read_raw(uint64_t o, uint32_t b, uint8_t *out) { return 1; }
size_t drive_tmd(uint8_t *out, size_t limit) { return 0; }
int console_setting(const char *name, char *out, size_t limit) { return 0; }
static int ejects;
void console_eject(void) { ejects++; }
void console_power_off(const char *how) { exit(20); }
void console_hot_reset(void) { exit(21); }
void stage_add(int stage, uint64_t ticks) {}
uint64_t time_base(void) { return 0; }
int ios_find_device(const char *path) { return -1; }
const char *ios_device_name(int device) { return "?"; }
int ios_device_request(int device, uint32_t command, uint32_t request) { return IOS_UNHANDLED; }
void ios_device_closed(int device) {}
void ios_trace(const char *device, uint32_t command, uint32_t request, int result) {}
int files_open(const char *path, uint32_t mode) { (void)mode; return FS_ENOENT; }
void files_close(int id) {}
int files_request(int id, uint32_t command, uint32_t request) { return 0; }
const char *files_name(int id) { return "?"; }

static int gaps;
static char last_gap[64];
void gap_hit(const char *id, uint32_t detail) { gaps++; snprintf(last_gap, sizeof last_gap, "%s", id); }

static uint32_t read_at(uint32_t address) {
  uint32_t value = 0xdeadbeef;
  CHECK(ipc_read(address, &value));
  return value;
}

static void test_mirrors_and_pins(void) {
  CHECK(ipc_write(0xcd800000u, 0x00123400u));
  CHECK_EQ(read_at(0xcd000000u), 0x00123400u);  // One register, seen at both places.
  CHECK_EQ(read_at(0xcd8000c8u), 0x80);  // The disc slot reports a disc.
  disc_in = 0;
  CHECK_EQ(read_at(0xcd0000c8u), 0);
  ipc_write(0xcd8000c0u, 0x0000c120u | 0x00010000u);  // AV pins, sensor bar, slot light, and a pin it doesn't own.
  CHECK_EQ(read_at(0xcd0000c0u), 0x0000c120u);
  uint32_t value;
  CHECK(!ipc_read(0xcd006c00u, &value));  // The audio interface isn't Hollywood's.
  CHECK(!ipc_read(0xcd000400u, &value));
  CHECK_EQ(gaps, 0);
}

static void test_reported_requests(void) {
  ipc_write(0xcd000034u, 0x40000000u);  // IPC only, as games set it.
  ipc_write(0xcd000024u, 0);  // Solid colour off.
  CHECK_EQ(gaps, 0);
  ipc_write(0xcd000034u, 0x40000401u);  // The timer and GPIO lines too.
  CHECK_EQ(gaps, 1);
  CHECK(!strcmp(last_gap, "hw.other_hollywood_interrupts_timer_gpio"));
  ipc_write(0xcd800024u, 0x10808001u);
  CHECK(!strcmp(last_gap, "hw.vi_solid_color_hollywood_0x24"));
  ipc_write(0xcd0000c0u, 0x200);  // The eject pin going up throws the disc out.
  CHECK_EQ(ejects, 1);
  ipc_write(0xcd0000c0u, 0x200);  // Held up, it doesn't again.
  CHECK_EQ(ejects, 1);
  ipc_write(0xcd000004u, 0x38);  // What the SDK writes after every reply: not a gap.
  CHECK_EQ(gaps, 2);
}

enum { HOOK = 0x80001000u, RELEASE = 0x80001100u, HOOK_OUT = 0x00001200u };

static void ioctl(uint32_t request, uint32_t code, uint32_t output) {
  memset(host(request), 0, 0x40);
  wr32(request, IOS_IOCTL);
  wr32(request + 12, code);
  wr32(request + 24, output);
  wr32(request + 28, output ? 4 : 0);
}

static void test_reset_button(void) {
  stm_reset_button();  // Nobody waiting: nothing happens.
  CHECK_EQ(queued, 0);
  ioctl(HOOK, 0x1000, HOOK_OUT);
  CHECK_EQ(stm_event_hook(IOS_IOCTL, HOOK), IOS_NO_REPLY);
  CHECK_EQ(stm_event_hook(IOS_IOCTL, HOOK), -2);  // Only one may wait.
  stm_reset_button();
  CHECK_EQ(queued, 1);
  CHECK_EQ(rd32(0x80000000u | HOOK_OUT), 0x20000);
  CHECK_EQ(rd32(HOOK + 4), 0);
  ioctl(RELEASE, 0x3002, 0);
  CHECK_EQ(stm_immediate(IOS_IOCTL, RELEASE), IOS_ENOENT);  // Nothing to release.
  stm_event_hook(IOS_IOCTL, HOOK);
  CHECK_EQ(stm_immediate(IOS_IOCTL, RELEASE), 0);
  CHECK_EQ(queued, 2);
  CHECK_EQ(rd32(0x80000000u | HOOK_OUT), 0);
}

static void test_eject(void) {
  int before = gaps;
  enum { REQUEST = 0x80002000u, INPUT = 0x00002100u };
  ioctl(REQUEST, 0xe3, 0);
  wr32(REQUEST + 16, INPUT);
  wr32(0x80000000u | INPUT, 0xe3000000u);
  wr32(0x80000004u | INPUT, 0);
  CHECK_EQ(dvd_request(IOS_IOCTL, REQUEST), 1);  // Stopping the motor is fine.
  CHECK_EQ(gaps, before);
  wr32(0x80000004u | INPUT, 1);
  int ejected = ejects;
  dvd_request(IOS_IOCTL, REQUEST);  // Throwing the disc out.
  CHECK_EQ(gaps, before);
  CHECK_EQ(ejects, ejected + 1);
}

int main(void) {
  memory_start();
  test_mirrors_and_pins();
  test_reported_requests();
  test_reset_button();
  test_eject();
  return finish();
}
