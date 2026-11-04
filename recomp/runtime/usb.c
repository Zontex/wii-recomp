// The Wii's USB ports, with nothing plugged in. Programs still open the USB devices to
// look for a keyboard, a microphone or another accessory, and each answers as a Wii with
// empty ports does: device lists come back empty, a device by its IDs can't be opened,
// and a request that waits for something to be plugged in waits for good.
//
// Request numbers, argument layouts and answers are from WiiBrew's /dev/usb/oh0 and
// /dev/usb/kbd pages and Dolphin's OH0.cpp and HIDv4.cpp, for the IOS we present (36):
// it has oh0, the older HID interface and the keyboard, and no /dev/usb/ven.
#include <stdio.h>

#include "cpu.h"
#include "gaps.h"
#include "ios.h"
#include "ios_vectors.h"

enum {
  // /dev/usb/oh0, the root of the external ports.
  OH0_DEVICE_LIST = 12, OH0_ROOT_HUB_DESCRIPTOR = 15, OH0_INSERT_HOOK = 27, OH0_CLASS_CHANGE_HOOK = 28,
  OH0_INSERT_HOOK_WITH_ID = 30, OH0_CANCEL_INSERT_HOOK = 31, DEVICE_ENTRY_BYTES = 8,
  ROOT_HUB_DESCRIPTOR = 0x02000302,  // The same whatever is plugged in, as measured on a Wii (Dolphin's OH0.cpp).
  USB_CANCELLED = -7022,             // A cancelled insertion hook's answer.
  // /dev/usb/hid, version 4.
  HID_DEVICE_CHANGE = 0, HID_SET_SUSPEND = 1, HID_CONTROL = 2, HID_INTERRUPT_IN = 3, HID_INTERRUPT_OUT = 4,
  HID_STRING = 5, HID_VERSION = 6, HID_SHUTDOWN = 7, HID_CANCEL_INTERRUPT = 8,
  HID_VERSION_NUMBER = 0x40001, HID_LIST_BYTES = 0x600, HID_TRANSFER_BYTES = 32, HID_DEVICE_ID_AT = 16, HID_CANCEL_BYTES = 8,
  END_OF_LIST = -1, MAX_HOOKS = 16,
};

static uint32_t guest(uint32_t physical) { return physical | 0x80000000u; }

// Insertion hooks waiting for a device that never comes, until they're cancelled.
static struct {
  uint32_t request, id;
} hooks[MAX_HOOKS];

static int hold_hook(uint32_t request, uint32_t id) {
  for (int i = 0; i < MAX_HOOKS; i++) {
    if (hooks[i].request) continue;
    hooks[i].request = request;
    hooks[i].id = id;
    return IOS_NO_REPLY;
  }
  gap_hit("limit.usb_insertion_hooks", MAX_HOOKS);
  return IOS_EINVAL;
}

static int oh0_ioctlv(uint32_t code, uint32_t request) {
  IosVectors vectors = ios_vectors(request);
  switch (code) {
    case OH0_DEVICE_LIST: {  // How many to list and an interface class in; the count and the list out.
      uint32_t most = ios_in_sized(&vectors, 0, 1), count = ios_out_sized(&vectors, 0, 1);
      if (vectors.inputs != 2 || vectors.outputs != 2 || !most || !count) return IOS_EINVAL;
      if (ios_out_bytes(&vectors, 1) != rd8(most) * DEVICE_ENTRY_BYTES) return IOS_EINVAL;
      wr8(count, 0);
      return 0;
    }
    case OH0_INSERT_HOOK:  // A vendor and product ID in. Answers once that device is plugged in.
      if (vectors.inputs != 2 || vectors.outputs != 0) return IOS_EINVAL;
      return hold_hook(request, 0);
    case OH0_INSERT_HOOK_WITH_ID: {  // The same, and an ID out that cancels it: here the vendor and product.
      uint32_t vendor = ios_in_sized(&vectors, 0, 2), product = ios_in_sized(&vectors, 1, 2), id = ios_out_sized(&vectors, 0, 4);
      if (vectors.inputs != 3 || vectors.outputs != 1 || !vendor || !product || !id) return IOS_EINVAL;
      uint32_t hook = rd16(vendor) << 16 | rd16(product);
      wr32(id, hook);
      return hold_hook(request, hook);
    }
    case OH0_CLASS_CHANGE_HOOK:  // Answers when a device's class changes, which needs a device.
      if (vectors.inputs != 1 || vectors.outputs != 0) return IOS_EINVAL;
      return IOS_NO_REPLY;
    default:
      return IOS_UNHANDLED;  // Root hub port status, among others.
  }
}

int usb_oh0_request(uint32_t command, uint32_t request) {
  uint32_t code = rd32(request + 12);
  if (command == IOS_IOCTLV) return oh0_ioctlv(code, request);
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  uint32_t in = rd32(request + 16), in_bytes = rd32(request + 20), out = rd32(request + 24), out_bytes = rd32(request + 28);
  if (code == OH0_ROOT_HUB_DESCRIPTOR) {
    if (!out || out_bytes != 4) return IOS_EINVAL;
    wr32(guest(out), ROOT_HUB_DESCRIPTOR);
    return 0;
  }
  if (code == OH0_CANCEL_INSERT_HOOK) {
    if (!in || in_bytes != 4) return IOS_EINVAL;
    uint32_t id = rd32(guest(in));
    for (int i = 0; i < MAX_HOOKS; i++) {
      if (!hooks[i].request || hooks[i].id != id) continue;
      ios_complete(hooks[i].request, USB_CANCELLED);
      hooks[i].request = 0;
    }
    return 0;
  }
  return IOS_UNHANDLED;
}

// The HID device list is sent at once the first time it's asked for, and again after a
// shutdown. Otherwise the request waits for a change, which never comes.
static uint32_t waiting_change;
static int list_due = 1;

static void send_device_list(uint32_t request, int result) {
  wr32(guest(rd32(request + 24)), (uint32_t)END_OF_LIST);  // An empty list is just its end.
  ios_complete(request, result);
}

int usb_hid_request(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  uint32_t code = rd32(request + 12), in = rd32(request + 16), in_bytes = rd32(request + 20);
  uint32_t out = rd32(request + 24), out_bytes = rd32(request + 28);
  switch (code) {
    case HID_VERSION: return HID_VERSION_NUMBER;
    case HID_SET_SUSPEND: return 0;  // IOS itself does nothing with it.
    case HID_DEVICE_CHANGE:
      if (!out || out_bytes != HID_LIST_BYTES) return IOS_EINVAL;
      if (list_due) {
        list_due = 0;
        wr32(guest(out), (uint32_t)END_OF_LIST);
        return 0;
      }
      waiting_change = request;
      return IOS_NO_REPLY;
    case HID_SHUTDOWN:  // Lets go of a waiting device list request, with -1.
      if (waiting_change) {
        send_device_list(waiting_change, -1);
        waiting_change = 0;
        list_due = 1;
      }
      return 0;
    case HID_CONTROL: case HID_INTERRUPT_IN: case HID_INTERRUPT_OUT: case HID_STRING:
      if (!in || in_bytes != HID_TRANSFER_BYTES) return IOS_EINVAL;
      return IOS_ENOENT;  // No device has the ID at in + HID_DEVICE_ID_AT.
    case HID_CANCEL_INTERRUPT:
      if (!in || in_bytes != HID_CANCEL_BYTES) return IOS_EINVAL;
      return IOS_ENOENT;
    default:
      return IOS_UNHANDLED;
  }
}

// /dev/usb/kbd: reading an event waits until a keyboard sends one, so with none plugged
// in it waits for good, as WiiBrew describes. Writes set the keyboard's lights, which
// IOS accepts with or without one. Reads, seeks and ioctlvs always fail.
int usb_kbd_request(uint32_t command, uint32_t request) {
  (void)request;
  if (command == IOS_IOCTL) return IOS_NO_REPLY;
  if (command == IOS_WRITE) return 0;
  return IOS_EINVAL;
}
