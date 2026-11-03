// The SD card slot. No card is ever in it: the status says so, and a wait for one to go
// in is held for good. Commands follow Dolphin's SDIOSlot0.cpp (GPL-2.0+).
#include "cpu.h"
#include "ios.h"

enum {
  SEND_COMMAND = 0x07, GET_STATUS = 0x0b, CARD_NOT_EXIST = 0,
  EVENT_REGISTER = 0x40, EVENT_UNREGISTER = 0x41, EVENT_INVALID = 0xc210000,
};

static uint32_t waiting;  // The request waiting for a card to go in or out.

int sdio_request(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  uint32_t code = rd32(request + 12), in = rd32(request + 16), out = rd32(request + 24);
  if (code == GET_STATUS) {
    if (!out || rd32(request + 28) < 4) return IOS_EINVAL;
    wr32(out | 0x80000000u, CARD_NOT_EXIST);
    return 0;
  }
  if (code != SEND_COMMAND || !in || rd32(request + 20) < 4) return IOS_UNHANDLED;
  switch (rd32(in | 0x80000000u)) {
    case EVENT_REGISTER:  // Answered when a card goes in, which it never does.
      waiting = request;
      return IOS_NO_REPLY;
    case EVENT_UNREGISTER:  // The wait ends without an event.
      if (!waiting) return IOS_EINVAL;
      ios_complete(waiting, EVENT_INVALID);
      waiting = 0;
      return 0;
    default:
      return IOS_UNHANDLED;
  }
}
