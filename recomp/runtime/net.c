// The network devices. Nintendo's online services are gone, so this console is
// never online, but WiiConnect24's scheduler and clock still answer as a real one does.
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "ios.h"
#include "ios_vectors.h"

#define CONFIG_PATH "/shared2/sys/net/02/config.dat"

enum {
  KD_SUSPEND_SCHEDULER = 1, KD_TRY_SUSPEND_SCHEDULER = 2, KD_RESUME_SCHEDULER = 3, KD_LOCK_SOCKET = 8, KD_UNLOCK_SOCKET = 9,
  KD_SAVE_MAIL_NOW = 0x0d, KD_REQUEST_USER_ID = 0x0f, KD_GET_SCHEDULER_STAT = 0x1e, SCHEDULER_BYTES = 256, KD_SET_SCRIPT_MODE = 0x22, KD_REQUEST_SHUTDOWN = 0x28, USER_ID_BYTES = 16,
  TIME_GET_UNIVERSAL = 0x14, TIME_SET_RTC_COUNTER = 0x17, TIME_GET_DIFFERENCE = 0x18,
  NCD_GET_CONFIG = 3, NCD_SET_CONFIG = 4, NCD_READ_CONFIG = 5, NCD_WRITE_CONFIG = 6, NCD_GET_LINK_STATUS = 7,
  LINK_NONE = 2, CONFIG_BYTES = 0x1b5c,
  NCD_GET_WIRELESS_MAC = 8, MAC_BYTES = 6,
};

static uint32_t guest(uint32_t physical) { return physical | 0x80000000u; }

// The output of an ioctl, if it has room for `bytes`.
static uint32_t ioctl_out(uint32_t request, uint32_t bytes) {
  uint32_t out = rd32(request + 24);
  return out && rd32(request + 28) >= bytes ? guest(out) : 0;
}

int net_kd_request(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  switch (rd32(request + 12)) {
    case KD_TRY_SUSPEND_SCHEDULER: return 0;  // As IOS does, with no answer word.
    case KD_GET_SCHEDULER_STAT: {  // No errors and no mail checks, with every permission on.
      uint32_t out = ioctl_out(request, 16), bytes = rd32(request + 28);
      if (!out || out & 3) return IOS_EINVAL;
      memset(host(out), 0, bytes < SCHEDULER_BYTES ? bytes : SCHEDULER_BYTES);
      wr32(out + 4, 0xffffffffu);
      return 0;
    }
    case KD_REQUEST_USER_ID: {
      uint32_t out = ioctl_out(request, USER_ID_BYTES);
      if (!out) return IOS_EINVAL;
      wc24_request_user_id(host(out));
      return 0;
    }
    case KD_SUSPEND_SCHEDULER:  // Nothing is scheduled, so it's already suspended, or resumed.
    case KD_RESUME_SCHEDULER:
    case KD_LOCK_SOCKET:        // Nothing else uses the network to lock it from.
    case KD_UNLOCK_SOCKET:
    case KD_SAVE_MAIL_NOW:      // No mail arrives, so there's none to save.
    case KD_SET_SCRIPT_MODE:    // Scripts come from the network, so none ever run.
    case KD_REQUEST_SHUTDOWN: { // Sent before the console resets or goes back to the Wii Menu. Nothing runs to stop.
      uint32_t out = ioctl_out(request, 4);
      if (out) wr32(out, 0);
      return 0;
    }
    default: return IOS_UNHANDLED;
  }
}

// The saved network settings, /shared2/sys/net/02/config.dat.
static uint8_t config[CONFIG_BYTES];
static int config_loaded;

static void load_config(void) {
  char full[1024];
  memset(config, 0, sizeof config);
  FILE *file = files_find(CONFIG_PATH, full, sizeof full) ? fopen(full, "rb") : 0;
  if (file) {
    fread(config, 1, sizeof config, file);
    fclose(file);
  }
  config_loaded = 1;
}

static void save_config(void) {
  char full[1024];
  files_save_path(CONFIG_PATH, full, sizeof full);
  files_make_parents(full);
  FILE *file = fopen(full, "wb");
  if (!file) return;
  fwrite(config, 1, sizeof config, file);
  fclose(file);
}

// The network settings manager. The answer word goes in the first output.
int net_ncd_manage(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTLV) return IOS_UNHANDLED;
  IosVectors vectors = ios_vectors(request);
  if (rd32(request + 12) == NCD_GET_WIRELESS_MAC) {  // A fixed, made-up, locally assigned address.
    static const uint8_t address[MAC_BYTES] = {0x02, 0x00, 0x5e, 0x10, 0x00, 0x01};
    uint32_t result = ios_out_sized(&vectors, 0, 4), out = ios_out_sized(&vectors, 1, MAC_BYTES);
    if (!result || !out) return IOS_EINVAL;
    for (int i = 0; i < MAC_BYTES; i++) wr8(out + (uint32_t)i, address[i]);
    wr32(result, 0);
    return 0;
  }
  if (rd32(request + 12) == NCD_GET_LINK_STATUS) {  // Never connected.
    uint32_t out = ios_out_sized(&vectors, 0, 8);
    if (!out) return IOS_EINVAL;
    wr32(out, 0);
    wr32(out + 4, LINK_NONE);
    return 0;
  }
  uint32_t code = rd32(request + 12);
  if (code == NCD_GET_CONFIG || code == NCD_READ_CONFIG) {  // The settings, then the answer word.
    if (code == NCD_READ_CONFIG || !config_loaded) load_config();
    uint32_t out = ios_out(&vectors, 0), bytes = ios_out_bytes(&vectors, 0), result = ios_out_sized(&vectors, 1, 8);
    if (!out) return IOS_EINVAL;
    memcpy(host(out), config, bytes < CONFIG_BYTES ? bytes : CONFIG_BYTES);
    if (result) wr32(result, 0), wr32(result + 4, 0);
    return 0;
  }
  if (code == NCD_SET_CONFIG || code == NCD_WRITE_CONFIG) {  // New settings, kept, and saved if asked.
    uint32_t in = ios_in(&vectors, 0), bytes = ios_in_bytes(&vectors, 0), result = ios_out_sized(&vectors, 0, 4);
    if (!in) return IOS_EINVAL;
    if (!config_loaded) load_config();
    memcpy(config, host(in), bytes < CONFIG_BYTES ? bytes : CONFIG_BYTES);
    if (code == NCD_WRITE_CONFIG) save_config();
    if (result) wr32(result, 0);
    return 0;
  }
  return IOS_UNHANDLED;
}

// The WiiConnect24 clock. Each answer starts with a result word, and times follow it.
int net_kd_time(uint32_t command, uint32_t request) {
  static uint32_t rtc;  // The console clock's counter, as the program last set it.
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  uint32_t out = ioctl_out(request, 12);
  if (!out) return IOS_EINVAL;
  uint64_t now = (uint64_t)time(0);  // Seconds since 1970, in UTC.
  switch (rd32(request + 12)) {
    case TIME_GET_UNIVERSAL: break;
    case TIME_SET_RTC_COUNTER: {
      uint32_t in = rd32(request + 16);
      if (!in || rd32(request + 20) < 4) return IOS_EINVAL;
      rtc = rd32(guest(in));
      wr32(out, 0);
      return 0;
    }
    case TIME_GET_DIFFERENCE: now -= rtc; break;
    default: return IOS_UNHANDLED;
  }
  wr32(out, 0);
  wr32(out + 4, (uint32_t)(now >> 32));
  wr32(out + 8, (uint32_t)now);
  return 0;
}
