// A stand-in for IOS, the Wii's security processor. The game sends it requests
// through shared memory, like opening a device, and gets replies by interrupt.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "hardware.h"
#include "ios.h"

#define IPC_MESSAGE 0xcd000000u
#define IPC_CONTROL 0xcd000004u
#define IPC_REPLY 0xcd000008u
#define IRQ_FLAGS 0xcd000030u
#define IRQ_IPC_FLAG 0x40000000u

enum { CONTROL_SEND = 1, CONTROL_ACK = 2, CONTROL_REPLY = 4, CONTROL_RELAUNCH = 8,
       CONTROL_ENABLE_REPLY = 0x10, CONTROL_ENABLE_ACK = 0x20 };
enum { REPLY_MARKER = 8, MAX_QUEUED = 64, MAX_OPEN = 32, NOT_FOUND = -106 };

static uint32_t message, reply_address;
static uint32_t ack, reply, enable_ack, enable_reply;
static uint32_t queue[MAX_QUEUED];
static int queued;
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
  int file = device < 0 && !is_device ? files_open(path) : -1;
  fprintf(stderr, "ios open %s -> %s\n", path, device >= 0 || file >= 0 ? "ok" : "not found");
  if (device < 0 && file < 0) return is_device ? IOS_ENOENT : NOT_FOUND;
  for (int fd = 3; fd < MAX_OPEN; fd++) {
    if (!open_device[fd] && !open_file[fd]) {
      open_device[fd] = device + 1;
      open_file[fd] = file + 1;
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
  if (queued == MAX_QUEUED) {  // Dropping a reply would leave the program waiting forever.
    fprintf(stderr, "stop: more than %d IOS replies waiting\n", MAX_QUEUED);
    exit(9);
  }
  queue[queued++] = request & ~0x80000000u;
}

// One reply at a time, once the game has finished with the last one.
void ipc_update(void) {
  if (reply || ack || !queued) return;
  reply_address = queue[0];
  memmove(queue, queue + 1, (size_t)--queued * sizeof queue[0]);
  reply = 1;
  update_irq();
}

int ipc_read(uint32_t address, uint32_t *value) {
  if (address == IPC_MESSAGE) *value = message;
  else if (address == IPC_REPLY) *value = reply_address;
  else if (address == IPC_CONTROL) {
    *value = (ack ? CONTROL_ACK : 0) | (reply ? CONTROL_REPLY : 0)
        | (enable_reply ? CONTROL_ENABLE_REPLY : 0) | (enable_ack ? CONTROL_ENABLE_ACK : 0);
  } else if (address == IRQ_FLAGS) {
    *value = irq_pending() ? IRQ_IPC_FLAG : 0;
  } else {
    return 0;
  }
  return 1;
}

int ipc_write(uint32_t address, uint32_t value) {
  if (address == IPC_MESSAGE) {
    message = value;
  } else if (address == IPC_CONTROL) {
    if (value & CONTROL_ACK) ack = 0;
    if (value & CONTROL_REPLY) reply = 0;
    enable_reply = value & CONTROL_ENABLE_REPLY;
    enable_ack = value & CONTROL_ENABLE_ACK;
    if (value & CONTROL_SEND) {
      handle(message);
      ack = 1;
    }
    update_irq();
  } else if (address == IRQ_FLAGS) {
    clear_irq(IRQ_IPC);
    update_irq();
  } else {
    return 0;
  }
  return 1;
}
