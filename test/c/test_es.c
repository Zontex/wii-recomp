// /dev/es requests made the way a program makes them, on the made-up storage that
// test_recomp_runtime.py writes. argv: storage folder, save folder, disc system folder.
#include "harness.h"
#include "ios.h"
#include "title.h"
#include "drive.h"
#include <stddef.h>
#include <string.h>
int console_running(void) { return 0; }
void console_restart(int boot_disc) { exit(99); }
static Title test_title = {.title_high = 1, .title_low = 2};  // title.c holds `title`, linked in.
void bluetooth_register_remote(uint8_t *settings, size_t bytes) {}
void camera_set_bar_above(int above) {}
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
uint64_t time_base(void) { return 0; }
void stage_add(int stage, uint64_t ticks) {}
int ios_trace_all(void) { return 0; }

enum { REQUEST = 0x80001000u, VECTORS = 0x80002000u, BUFFERS = 0x80010000u, BUFFER_STEP = 0x4000 };
enum { ES_EINVAL = -1017 };

// Sends an ioctlv whose buffers have the given sizes, each in its own area of memory.
static int ioctlv(uint32_t code, int inputs, int outputs, const uint32_t *sizes) {
  wr32(REQUEST + 12, code);
  wr32(REQUEST + 16, (uint32_t)inputs);
  wr32(REQUEST + 20, (uint32_t)outputs);
  wr32(REQUEST + 24, VECTORS & 0x7fffffff);
  for (int i = 0; i < inputs + outputs; i++) {
    wr32(VECTORS + i * 8, (BUFFERS + i * BUFFER_STEP) & 0x7fffffff);
    wr32(VECTORS + i * 8 + 4, sizes[i]);
  }
  return es_request(IOS_IOCTLV, REQUEST);
}
static uint32_t buffer(int i) { return BUFFERS + (uint32_t)i * BUFFER_STEP; }
static void put_title(int i, uint64_t title) { wr32(buffer(i), (uint32_t)(title >> 32)); wr32(buffer(i) + 4, (uint32_t)title); }

int main(int count, char **arguments) {
  title = &test_title;
  memory_start();
  files_start(arguments[1]);
  drive_insert(arguments[3]);

  CHECK_EQ(ioctlv(0x0e, 0, 1, (uint32_t[]){4}), 0);
  CHECK_EQ(rd32(buffer(0)), 3);
  wr32(buffer(0), 2);
  CHECK_EQ(ioctlv(0x0f, 1, 1, (uint32_t[]){4, 16}), 0);
  CHECK_EQ(rd32(buffer(1) + 4), 2);
  CHECK_EQ(rd32(buffer(1) + 8), 0x00010001);
  CHECK_EQ(ioctlv(0x0e, 1, 1, (uint32_t[]){4, 4}), ES_EINVAL);  // Wrong buffers.
  wr32(buffer(0), 5);
  CHECK_EQ(ioctlv(0x0f, 1, 1, (uint32_t[]){4, 8}), ES_EINVAL);  // No room for five.

  put_title(0, 0x0000000100000002ull);
  CHECK_EQ(ioctlv(0x34, 1, 1, (uint32_t[]){8, 4}), 0);
  uint32_t tmd_size = rd32(buffer(1));
  CHECK_EQ(tmd_size, 0x1e4 + 2 * 36);
  wr32(buffer(1), tmd_size);
  CHECK_EQ(ioctlv(0x35, 2, 1, (uint32_t[]){8, 4, tmd_size}), 0);
  CHECK_EQ(rd32(buffer(2) + 0x18c + 4), 2);
  CHECK_EQ(ioctlv(0x10, 1, 1, (uint32_t[]){8, 4}), 0);
  CHECK_EQ(rd32(buffer(1)), 2);
  CHECK_EQ(ioctlv(0x14, 1, 1, (uint32_t[]){8, 4}), 0);
  CHECK_EQ(rd32(buffer(1)), 0x80);
  CHECK_EQ(ioctlv(0x12, 1, 1, (uint32_t[]){8, 4}), 0);
  CHECK_EQ(rd32(buffer(1)), 1);
  wr32(buffer(1), 1);
  CHECK_EQ(ioctlv(0x13, 2, 1, (uint32_t[]){8, 4, 0xd8}), 0);
  CHECK_EQ(rd32(buffer(2) + 4), 0x11223344);
  put_title(0, 0x0000000100000099ull);
  CHECK_EQ(ioctlv(0x34, 1, 1, (uint32_t[]){8, 4}), -106);  // Not installed.

  CHECK_EQ(ioctlv(0x20, 0, 1, (uint32_t[]){8}), 0);
  CHECK_EQ(rd32(buffer(0) + 4), 2);
  put_title(0, 0x0001000053414141ull);
  CHECK_EQ(ioctlv(0x1d, 1, 1, (uint32_t[]){8, 30}), 0);
  CHECK(!strcmp((char *)host(buffer(1)), "/title/00010000/53414141/data"));
  CHECK_EQ(ioctlv(0x1b, 1, 1, (uint32_t[]){0, 0xd8}), 0);  // The disc's own ticket.
  CHECK_EQ(rd32(buffer(1) + 4), 0x99887766);

  wr32(buffer(0), 0);  // The running title's content with index 0.
  int handle = ioctlv(0x09, 1, 0, (uint32_t[]){4});
  CHECK(handle >= 0);
  wr32(buffer(0), (uint32_t)handle);
  CHECK_EQ(ioctlv(0x0a, 1, 1, (uint32_t[]){4, 3}), 3);
  CHECK(!memcmp(host(buffer(1)), "pri", 3));
  wr32(buffer(1), 2);
  wr32(buffer(2), 1);
  CHECK_EQ(ioctlv(0x23, 3, 0, (uint32_t[]){4, 4, 4}), 5);
  CHECK_EQ(ioctlv(0x0a, 1, 1, (uint32_t[]){4, 16}), 2);
  CHECK(!memcmp(host(buffer(1)), "te", 2));
  CHECK_EQ(ioctlv(0x0b, 1, 0, (uint32_t[]){4}), 0);
  CHECK_EQ(ioctlv(0x0a, 1, 1, (uint32_t[]){4, 3}), ES_EINVAL);  // Closed.
  put_title(0, 0x0000000100000002ull);
  wr32(buffer(2), 1);  // The shared content.
  handle = ioctlv(0x24, 3, 0, (uint32_t[]){8, 0xd8, 4});
  CHECK(handle >= 0);
  wr32(buffer(0), (uint32_t)handle);
  CHECK_EQ(ioctlv(0x0a, 1, 1, (uint32_t[]){4, 16}), 5);
  CHECK(!memcmp(host(buffer(1)), "share", 5));

  CHECK_EQ(es_request(IOS_IOCTL, REQUEST), IOS_UNHANDLED);
  CHECK_EQ(ioctlv(0x7f, 0, 0, 0), IOS_UNHANDLED);
  return finish();
}
