// The power and reset controller: the buttons through the event hook, turning off,
// standby and hot reset, and a closed hook. Compiled with stm.c.
#include <setjmp.h>
#include <string.h>
#include "harness.h"
#include "ios.h"

enum { HOOK = 0x80001000u, IMMEDIATE = 0x80001100u, OUT = 0x80002000u };
enum { EVENT_HOOK = 0x1000, HOT_RESET = 0x2001, SHUTDOWN = 0x2003, IDLE = 0x2004, RELEASE = 0x3002, DIM = 0x5001,
       EVENT_RESET = 0x20000, EVENT_POWER = 0x800 };

// What stm.c expects of the rest of the runtime: the console's power, and replies.
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
static jmp_buf powered;
static const char *power_how;
static int resets, completed, completed_result;
static uint32_t completed_request;
void console_power_off(const char *how) { power_how = how; longjmp(powered, 1); }
void console_hot_reset(void) { resets++; longjmp(powered, 1); }
void ios_complete(uint32_t request, int result) { completed++; completed_request = request; completed_result = result; }

static void make_ioctl(uint32_t request, uint32_t code) {
  memset(host(request), 0, 0x40);
  wr32(request, IOS_IOCTL);
  wr32(request + 12, code);
  wr32(request + 24, OUT & 0x7fffffff);
  wr32(request + 28, 4);
}

static int hook(void) {
  make_ioctl(HOOK, EVENT_HOOK);
  return stm_event_hook(IOS_IOCTL, HOOK);
}

// Runs an immediate request that never returns, as the power goes.
static int immediate_powers(uint32_t code) {
  make_ioctl(IMMEDIATE, code);
  if (setjmp(powered)) return 1;
  stm_immediate(IOS_IOCTL, IMMEDIATE);
  return 0;
}

int main(void) {
  memory_start();

  // The hook waits for a button. Only one may wait.
  CHECK(hook() == IOS_NO_REPLY);
  CHECK(stm_listening());
  make_ioctl(HOOK + 0x40, EVENT_HOOK);
  CHECK(stm_event_hook(IOS_IOCTL, HOOK + 0x40) == -2);

  // The power button answers it with the power event.
  stm_power_button();
  CHECK_EQ(completed, 1);
  CHECK_EQ(completed_request, HOOK);
  CHECK_EQ(rd32(OUT), EVENT_POWER);
  CHECK(!stm_listening());
  stm_power_button();  // With nothing waiting, a press is missed.
  CHECK_EQ(completed, 1);

  // And the reset button with the reset event.
  hook();
  stm_reset_button();
  CHECK_EQ(completed, 2);
  CHECK_EQ(rd32(OUT), EVENT_RESET);

  // Releasing the hook ends the wait with no event; with none waiting it's not found.
  hook();
  make_ioctl(IMMEDIATE, RELEASE);
  CHECK_EQ(stm_immediate(IOS_IOCTL, IMMEDIATE), 0);
  CHECK_EQ(completed, 3);
  CHECK_EQ(rd32(OUT), 0);
  CHECK(stm_immediate(IOS_IOCTL, IMMEDIATE) == IOS_ENOENT);

  // A closed hook is dropped unanswered, and a new one may wait.
  hook();
  stm_event_hook_closed();
  CHECK(!stm_listening());
  CHECK_EQ(completed, 3);
  CHECK(hook() == IOS_NO_REPLY);

  // Dimming is answered; turning off, standby and hot reset never return.
  make_ioctl(IMMEDIATE, DIM);
  CHECK_EQ(stm_immediate(IOS_IOCTL, IMMEDIATE), 0);
  CHECK(immediate_powers(SHUTDOWN));
  CHECK(power_how && !strcmp(power_how, "off"));
  CHECK(immediate_powers(IDLE));
  CHECK(power_how && !strcmp(power_how, "into standby"));
  CHECK(immediate_powers(HOT_RESET));
  CHECK_EQ(resets, 1);

  // Anything else is left to ios.c to report.
  make_ioctl(IMMEDIATE, 0x7001);
  CHECK(stm_immediate(IOS_IOCTL, IMMEDIATE) == IOS_UNHANDLED);
  return finish();
}
