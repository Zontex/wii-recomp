// The IOS devices a program can open, by exact name. A request a device doesn't
// model answers IOS_UNHANDLED, which ios.c logs and turns into an error.
#include <string.h>

#include "bluetooth.h"
#include "cpu.h"
#include "game.h"
#include "ios.h"

typedef int (*IosHandler)(uint32_t command, uint32_t request);

typedef struct {
  const char *name;
  int prefix;  // Also answers any name that starts with this one.
  IosHandler request;
} IosDevice;

static int unmodelled(uint32_t command, uint32_t request) { return IOS_UNHANDLED; }

// The Bluetooth chip only replies when something happens. Without the virtual chip,
// controllers come straight from input.c and nothing ever happens here.
static int bluetooth(uint32_t command, uint32_t request) {
  return game_virtual_bluetooth ? bluetooth_request(command, request) : IOS_NO_REPLY;
}

// Other USB devices, such as a keyboard, are never plugged in, so they never reply.
static int usb(uint32_t command, uint32_t request) { return IOS_NO_REPLY; }

static const IosDevice devices[] = {
  {"/dev/es", 0, es_request},
  {"/dev/fs", 0, fs_request},
  {"/dev/di", 0, dvd_request},
  {"/dev/stm/immediate", 0, stm_immediate},
  {"/dev/stm/eventhook", 0, stm_event_hook},
  {"/dev/usb/oh1/57e/305", 0, bluetooth},
  {"/dev/usb/", 1, usb},
  {"/dev/net/kd/request", 0, net_kd_request},
  {"/dev/net/kd/time", 0, net_kd_time},
  {"/dev/net/ncd/manage", 0, net_ncd_manage},
  {"/dev/net/wd/command", 0, unmodelled},
  {"/dev/net/ip/top", 0, unmodelled},
  {"/dev/net/ssl", 0, unmodelled},
  {"/dev/sdio/", 1, sdio_request},
  {"/dev/aes", 0, unmodelled},
  {"/dev/sha", 0, unmodelled},
  {"/dev/wl0", 0, unmodelled},
};

enum { DEVICE_COUNT = sizeof devices / sizeof devices[0] };

int ios_find_device(const char *path) {
  for (int i = 0; i < DEVICE_COUNT; i++) {
    const IosDevice *device = &devices[i];
    if (device->prefix ? !strncmp(path, device->name, strlen(device->name)) : !strcmp(path, device->name)) return i;
  }
  return -1;
}

const char *ios_device_name(int device) { return devices[device].name; }

int ios_device_request(int device, uint32_t command, uint32_t request) {
  return devices[device].request(command, request);
}
