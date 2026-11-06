// The power and reset controller: the event hook that waits for the buttons, and
// immediate requests such as dimming the screen or turning the console off.
#include <stdatomic.h>
#include <stdio.h>

#include "console.h"
#include "cpu.h"
#include "ios.h"

// Codes and the events' values as Dolphin's IOS/STM/STM.h names them.
enum {
  EVENT_HOOK = 0x1000, HOT_RESET = 0x2001, HOT_RESET_FOR_PD = 0x2002, SHUTDOWN = 0x2003, IDLE = 0x2004,
  RELEASE_EVENT_HOOK = 0x3002, DIM_SCREEN = 0x5001, SET_LED = 0x6002,
  EVENT_RESET = 0x00020000, EVENT_POWER = 0x00000800, ALREADY_WAITING = -2,
};

static uint32_t held_hook;      // The event hook request waiting for a button, or 0.
static atomic_int listening;    // Whether one is, for the window's thread.

// Answers the waiting event hook with an event, written to its output.
static void answer_hook(uint32_t event) {
  uint32_t request = held_hook, output = rd32(request + 24) | 0x80000000u;
  held_hook = 0;
  atomic_store(&listening, 0);
  if (rd32(request + 28) >= 4) wr32(output, event);
  ios_complete(request, 0);
}

int stm_immediate(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  switch (rd32(request + 12)) {
    case DIM_SCREEN: return 0;  // Turns dimming on or off. Our picture never dims.
    case SET_LED: return 0;     // The disc slot's light. There's no slot to light.
    case RELEASE_EVENT_HOOK:    // Ends the wait with no event, as Dolphin does.
      if (!held_hook) return IOS_ENOENT;
      answer_hook(0);
      return 0;
    // The power goes, so neither of these is ever answered. Standby, which WiiConnect24
    // asks for in place of turning off, is off here too: nothing runs while it waits.
    case SHUTDOWN: console_power_off("off");
    case IDLE: console_power_off("into standby");
    // The whole Wii starts again, from its system menu, which reads why from NANDBOOTINFO.
    case HOT_RESET:
    case HOT_RESET_FOR_PD: console_hot_reset();
    default: return IOS_UNHANDLED;
  }
}

int stm_event_hook(uint32_t command, uint32_t request) {
  // This call stays open until the reset or power button is pressed, and the program
  // sends it again after every reply, so answering it would repeat it endlessly.
  if (command == IOS_IOCTL && rd32(request + 12) == EVENT_HOOK) {
    if (held_hook) return ALREADY_WAITING;  // Only one may wait, as Dolphin answers.
    held_hook = request;
    atomic_store(&listening, 1);
    return IOS_NO_REPLY;
  }
  return IOS_UNHANDLED;
}

// The handle closed: the held request is dropped unanswered, as Dolphin drops it.
void stm_event_hook_closed(void) {
  held_hook = 0;
  atomic_store(&listening, 0);
}

// The reset button was pressed. A program that isn't waiting on the hook misses it.
void stm_reset_button(void) {
  if (held_hook) answer_hook(EVENT_RESET);
}

void stm_power_button(void) {
  if (held_hook) answer_hook(EVENT_POWER);
}

int stm_listening(void) { return atomic_load(&listening); }
