// The power and reset controller: the event hook that waits for the buttons, and
// immediate requests such as dimming the screen.
#include "cpu.h"
#include "ios.h"

// Codes and the reset event's value as Dolphin's IOS/STM/STM.h names them.
enum { EVENT_HOOK = 0x1000, RELEASE_EVENT_HOOK = 0x3002, DIM_SCREEN = 0x5001, SET_LED = 0x6002, EVENT_RESET = 0x00020000,
       ALREADY_WAITING = -2 };

static uint32_t held_hook;  // The event hook request waiting for a button, or 0.

// Answers the waiting event hook with an event, written to its output.
static void answer_hook(uint32_t event) {
  uint32_t request = held_hook, output = rd32(request + 24) | 0x80000000u;
  held_hook = 0;
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
    default: return IOS_UNHANDLED;
  }
}

int stm_event_hook(uint32_t command, uint32_t request) {
  // This call stays open until the reset or power button is pressed, and the program
  // sends it again after every reply, so answering it would repeat it endlessly.
  if (command == IOS_IOCTL && rd32(request + 12) == EVENT_HOOK) {
    if (held_hook) return ALREADY_WAITING;  // Only one may wait, as Dolphin answers.
    held_hook = request;
    return IOS_NO_REPLY;
  }
  return IOS_UNHANDLED;
}

// The reset button was pressed. A program that isn't waiting on the hook misses it.
void stm_reset_button(void) {
  if (held_hook) answer_hook(EVENT_RESET);
}
