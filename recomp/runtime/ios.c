// A stand-in for IOS, the Wii's security processor. The game sends it requests
// through shared memory, like opening a device, and gets replies by interrupt.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "console.h"
#include "drive.h"
#include "hardware.h"
#include "ios.h"
#include "gaps.h"

// Hollywood's registers, as offsets the game reaches at both 0xCD000000 and 0xCD800000,
// as Dolphin maps them (HW/WII_IPC.cpp).
enum {
  HOLLYWOOD_BASE = 0xcd000000u, HOLLYWOOD_MIRROR = 0x00800000u, HOLLYWOOD_SIZE = 0x400,
  IPC_MESSAGE = 0x00, IPC_CONTROL = 0x04, IPC_REPLY = 0x08, VI_SOLID = 0x24, IRQ_FLAGS = 0x30, IRQ_MASK = 0x34,
  GPIO_OUT = 0xc0, GPIO_DIRECTION = 0xc4, GPIO_IN = 0xc8,
};
#define IRQ_IPC_FLAG 0x40000000u
#define VI_SOLID_ENABLE 1u  // The rest of the register is the colour, as WiiBrew describes it.
// The pins the game's side owns, and which way each points at power-on, from Dolphin.
// The slot light and sensor bar power have nothing to drive here, and Dolphin leaves
// them alone too. The eject pin would throw the disc out.
enum {
  GPIO_SLOT_LIGHT = 0x20, GPIO_DISC_IN = 0x80, GPIO_SENSOR_BAR = 0x100, GPIO_EJECT = 0x200,
  GPIO_AV_CLOCK = 0x4000, GPIO_AV_DATA = 0x8000,
  GPIO_OWNED = GPIO_SLOT_LIGHT | GPIO_DISC_IN | GPIO_SENSOR_BAR | GPIO_EJECT | GPIO_AV_CLOCK | GPIO_AV_DATA,
  GPIO_OUTPUTS = GPIO_SLOT_LIGHT | GPIO_SENSOR_BAR | GPIO_EJECT | GPIO_AV_CLOCK | GPIO_AV_DATA,
};

enum { CONTROL_SEND = 1, CONTROL_ACK = 2, CONTROL_REPLY = 4, CONTROL_RELAUNCH = 8,
       CONTROL_ENABLE_REPLY = 0x10, CONTROL_ENABLE_ACK = 0x20 };
enum { REPLY_MARKER = 8, MAX_OPEN = 32, NOT_FOUND = -106 };

static uint32_t message, reply_address, vi_solid, irq_mask = IRQ_IPC_FLAG;
static uint32_t gpio_out, gpio_direction = GPIO_OUTPUTS;
static uint32_t ack, reply, enable_ack, enable_reply;
// Replies wait here until the game takes them, one at a time. Any number can wait: devices
// that hold requests, such as the Bluetooth chip, may finish many at once, and IOS never
// drops a reply.
static uint32_t *queue;
static size_t queued, queue_room;
static int open_device[MAX_OPEN];   // Device number plus one.
static int open_file[MAX_OPEN];     // File handle plus one.

static void update_irq(void) {
  if ((reply && enable_reply) || (ack && enable_ack)) raise_irq(IRQ_IPC);
  else clear_irq(IRQ_IPC);
}

static uint32_t guest(uint32_t physical) { return physical | 0x80000000u; }

static int open_request(uint32_t request) {
  char path[64] = {0};
  uint32_t name = guest(rd32(guest(request) + 12));
  for (int i = 0; i < 63; i++) {
    path[i] = (char)rd8(name + i);
    if (!path[i]) break;
  }
  int device = ios_find_device(path);
  int is_device = !strncmp(path, "/dev/", 5);
  // The mode says whether the file may be read, written or both (Dolphin's FS.cpp keeps its low two bits).
  int file = device < 0 && !is_device ? files_open(path, rd32(guest(request) + 16)) : IOS_ENOENT;
  if (device >= 0 || file >= 0) fprintf(stderr, "ios open %s -> ok\n", path);
  else fprintf(stderr, "ios open %s -> %d\n", path, file);
  // /dev/usb/ven is only in IOS 57 to 59. The IOS we present has none, as on a Wii running it.
  if (!strcmp(path, "/dev/usb/ven")) gap_hit("ios.dev_usb_ven", 0);
  if (device < 0 && file < 0) return file;
  for (int fd = 3; fd < MAX_OPEN; fd++) {
    if (!open_device[fd] && !open_file[fd]) {
      open_device[fd] = device + 1;
      open_file[fd] = file >= 0 ? file + 1 : 0;
      return fd;
    }
  }
  return NOT_FOUND;
}

static void handle(uint32_t physical) {
  uint32_t request = guest(physical);
  uint32_t command = rd32(request);
  int fd = (int)rd32(request + 8);
  int result = 0;
  if (command == IOS_OPEN) {
    result = open_request(physical);
  } else if (fd >= 3 && fd < MAX_OPEN && open_file[fd]) {
    if (command == IOS_CLOSE) {
      files_close(open_file[fd] - 1);
      open_file[fd] = 0;
    } else {
      result = files_request(open_file[fd] - 1, command, request);
      ios_trace(files_name(open_file[fd] - 1), command, request, result);
    }
  } else if (fd >= 3 && fd < MAX_OPEN && open_device[fd]) {
    int device = open_device[fd] - 1;
    if (command == IOS_CLOSE) {
      open_device[fd] = 0;
      ios_device_closed(device);
    } else {
      result = ios_device_request(device, command, request);
      ios_trace(ios_device_name(device), command, request, result);
      if (result == IOS_UNHANDLED) result = IOS_EINVAL;
    }
  } else {
    result = NOT_FOUND;
  }
  if (result != IOS_NO_REPLY) ios_complete(request, result);
}

// Finishes a request, at once or for a device that held it open until it had something.
void ios_complete(uint32_t request, int result) {
  uint32_t command = rd32(request);
  wr32(request + 4, (uint32_t)result);
  wr32(request + 8, command);
  wr32(request, REPLY_MARKER);
  if (queued == queue_room) {
    queue_room = queue_room ? queue_room * 2 : 64;
    queue = realloc(queue, queue_room * sizeof *queue);
    if (!queue) { fprintf(stderr, "stop: no memory for IOS replies\n"); exit(9); }
  }
  queue[queued++] = request & ~0x80000000u;
}

void ipc_describe(void) {
  fprintf(stderr, "state: IPC reply %u ack %u, %zu replies waiting, enabled reply %u ack %u, Hollywood mask %08x\n",
          reply, ack, queued, enable_reply, enable_ack, irq_mask);
}

// One reply at a time, once the game has finished with the last one.
void ipc_update(void) {
  if (reply || ack || !queued) return;
  reply_address = queue[0];
  memmove(queue, queue + 1, --queued * sizeof queue[0]);
  reply = 1;
  update_irq();
}

// The register's offset, or -1 for an address outside Hollywood's registers.
static int hollywood_offset(uint32_t address) {
  uint32_t offset = (address & ~HOLLYWOOD_MIRROR) - HOLLYWOOD_BASE;
  return offset < HOLLYWOOD_SIZE ? (int)offset : -1;
}

int ipc_read(uint32_t address, uint32_t *value) {
  switch (hollywood_offset(address)) {
    case IPC_MESSAGE: *value = message; break;
    case IPC_REPLY: *value = reply_address; break;
    case IPC_CONTROL:
      *value = (ack ? CONTROL_ACK : 0) | (reply ? CONTROL_REPLY : 0)
          | (enable_reply ? CONTROL_ENABLE_REPLY : 0) | (enable_ack ? CONTROL_ENABLE_ACK : 0);
      break;
    case IRQ_FLAGS: *value = irq_pending() ? IRQ_IPC_FLAG : 0; break;
    case IRQ_MASK: *value = irq_mask; break;
    case VI_SOLID: *value = vi_solid; break;
    case GPIO_OUT: *value = gpio_out; break;
    case GPIO_DIRECTION: *value = gpio_direction; break;
    case GPIO_IN: *value = drive_has_disc() ? GPIO_DISC_IN : 0; break;
    default: return 0;
  }
  return 1;
}

int ipc_write(uint32_t address, uint32_t value) {
  switch (hollywood_offset(address)) {
    case IPC_MESSAGE: message = value; break;
    case IPC_CONTROL:
      if (value & CONTROL_ACK) ack = 0;
      if (value & CONTROL_REPLY) reply = 0;
      enable_reply = value & CONTROL_ENABLE_REPLY;
      enable_ack = value & CONTROL_ENABLE_ACK;
      // CONTROL_RELAUNCH needs nothing: the SDK sets it after taking every reply (Wii Fit
      // writes 0x38 each time), and games reload IOS through ES.
      if (value & CONTROL_SEND) {
        handle(message);
        ack = 1;
      }
      update_irq();
      break;
    case IRQ_FLAGS:
      clear_irq(IRQ_IPC);
      update_irq();
      break;
    case IRQ_MASK:
      // Only IPC replies reach the game. Hollywood's timer, GPIO and other lines belong
      // to IOS, and nothing here raises them.
      irq_mask = value;
      if (value & ~IRQ_IPC_FLAG) gap_hit("hw.other_hollywood_interrupts_timer_gpio", value);
      break;
    case VI_SOLID:
      vi_solid = value;
      if (value & VI_SOLID_ENABLE) gap_hit("hw.vi_solid_color_hollywood_0x24", value);
      break;
    case GPIO_OUT: {
      uint32_t gpio_out_before = gpio_out;
      gpio_out = (value & GPIO_OWNED) | (gpio_out & ~GPIO_OWNED);
      if (value & GPIO_EJECT & ~gpio_out_before) console_eject();  // The pin going up throws the disc out.
      break;
    }
    case GPIO_DIRECTION: gpio_direction = (value & GPIO_OWNED) | (gpio_direction & ~GPIO_OWNED); break;
    case GPIO_IN: break;  // Read only.
    default: return 0;
  }
  return 1;
}
