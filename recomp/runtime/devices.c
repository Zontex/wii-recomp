// The IOS devices the game opens. Anything unmodelled answers "fine" and logs it.
#include <stdio.h>
#include <string.h>

#include "bluetooth.h"
#include "cpu.h"
#include "game.h"
#include "ios.h"

#define STM_EVENT_HOOK 0x1000u

static const char *device_names[] = {
  "/dev/es", "/dev/stm/immediate", "/dev/stm/eventhook", "/dev/di", "/dev/fs", "/dev/net", "/dev/usb/oh1/57e/305", "/dev/usb",
  "/dev/sdio", "/dev/aes", "/dev/sha", "/dev/wl0", 0,
};

int ios_find_device(const char *path) {
  for (int i = 0; device_names[i]; i++) {
    if (!strncmp(path, device_names[i], strlen(device_names[i]))) return i;
  }
  return -1;
}

int ios_device_request(int device, uint32_t command, uint32_t request) {
  if (!strcmp(device_names[device], "/dev/di")) return dvd_request(command, request);
  if (!strcmp(device_names[device], "/dev/es")) return es_request(command, request);
  if (!strcmp(device_names[device], "/dev/fs")) return fs_request(command, request);
  // The Bluetooth chip only replies when something happens, and nothing is paired.
  if (!strcmp(device_names[device], "/dev/usb/oh1/57e/305") && game_virtual_bluetooth) return bluetooth_request(command, request);
  if (!strncmp(device_names[device], "/dev/usb", 8)) return IOS_NO_REPLY;
  // This call stays open until the reset or power button is pressed, and the game
  // sends it again after every reply, so answering it would repeat it endlessly.
  if (!strcmp(device_names[device], "/dev/stm/eventhook") && command == IOS_IOCTL && rd32(request + 12) == STM_EVENT_HOOK) return IOS_NO_REPLY;
  fprintf(stderr, "ios %s command %u: %08x %08x %08x %08x %08x\n", device_names[device], command,
          rd32(request + 12), rd32(request + 16), rd32(request + 20), rd32(request + 24), rd32(request + 28));
  return 0;
}
