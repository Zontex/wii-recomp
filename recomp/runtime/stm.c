// The power and reset controller: the event hook that waits for the buttons, and
// immediate requests such as dimming the screen.
#include "cpu.h"
#include "ios.h"

enum { EVENT_HOOK = 0x1000, DIM_SCREEN = 0x5001, SET_LED = 0x6002 };

int stm_immediate(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  switch (rd32(request + 12)) {
    case DIM_SCREEN: return 0;  // Turns dimming on or off. Our picture never dims.
    case SET_LED: return 0;     // The disc slot's light. There's no slot to light.
    default: return IOS_UNHANDLED;
  }
}

int stm_event_hook(uint32_t command, uint32_t request) {
  // This call stays open until the reset or power button is pressed, and the program
  // sends it again after every reply, so answering it would repeat it endlessly.
  if (command == IOS_IOCTL && rd32(request + 12) == EVENT_HOOK) return IOS_NO_REPLY;
  return IOS_UNHANDLED;
}
