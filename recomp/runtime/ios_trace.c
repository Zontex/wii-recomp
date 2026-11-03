// A log of what each IOS device is asked, so nothing it can't answer goes unseen.
// Each kind of request is logged once, with its first answer. WII_RECOMP_IOS_TRACE=all
// logs every request, and WII_RECOMP_IOS_STRICT=1 stops at the first unhandled one.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "ios.h"

enum { MAX_KINDS = 1024 };

typedef struct {
  const char *device;
  uint32_t command, code;
} Kind;

static Kind seen[MAX_KINDS];
static int seen_count;

static const char *command_name(uint32_t command) {
  static const char *names[] = {"?", "open", "close", "read", "write", "seek", "ioctl", "ioctlv"};
  return command < sizeof names / sizeof names[0] ? names[command] : "?";
}

static int first_time(const char *device, uint32_t command, uint32_t code) {
  for (int i = 0; i < seen_count; i++) {
    if (seen[i].command == command && seen[i].code == code && !strcmp(seen[i].device, device)) return 0;
  }
  if (seen_count < MAX_KINDS) seen[seen_count++] = (Kind){device, command, code};
  return 1;
}

int ios_trace_all(void) {
  static int all = -1;
  if (all < 0) all = getenv("WII_RECOMP_IOS_TRACE") && !strcmp(getenv("WII_RECOMP_IOS_TRACE"), "all");
  return all;
}

void ios_trace(const char *device, uint32_t command, uint32_t request, int result) {
  int unhandled = result == IOS_UNHANDLED;
  uint32_t code = command == IOS_IOCTL || command == IOS_IOCTLV ? rd32(request + 12) : 0;
  if (!ios_trace_all() && !first_time(device, command, code)) return;
  if (unhandled && command == IOS_IOCTLV) {
    fprintf(stderr, "ios %s ioctlv %x: unhandled, %u in, %u out\n", device, code, rd32(request + 16), rd32(request + 20));
  } else if (unhandled && command == IOS_IOCTL) {
    fprintf(stderr, "ios %s ioctl %x: unhandled, %u bytes in, %u out\n", device, code, rd32(request + 20), rd32(request + 28));
  } else if (unhandled) fprintf(stderr, "ios %s %s %x: unhandled\n", device, command_name(command), code);
  else if (result == IOS_NO_REPLY) fprintf(stderr, "ios %s %s %x: held\n", device, command_name(command), code);
  else fprintf(stderr, "ios %s %s %x: %d\n", device, command_name(command), code, result);
  if (unhandled && getenv("WII_RECOMP_IOS_STRICT")) {
    fprintf(stderr, "stop: IOS can't answer %s %s %x yet\n", device, command_name(command), code);
    exit(9);
  }
}
