// Rarely used IOS requests: the reply queue, /dev/aes, /dev/sha, the empty USB ports and
// the drive's cover status. Compiled with ios.c, usb.c, crypto.c, dvd.c and gaps.c.
#include <string.h>
#include "harness.h"
#include "ios.h"

// What the files under test expect of the rest of the runtime.
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
void raise_irq(int bit) {}
void clear_irq(int bit) {}
int irq_pending(void) { return 0; }
int ios_find_device(const char *path) { return -1; }
const char *ios_device_name(int device) { return "?"; }
int ios_device_request(int device, uint32_t command, uint32_t request) { return IOS_UNHANDLED; }
void ios_device_closed(int device) {}
int files_open(const char *path, uint32_t mode) { return FS_ENOENT; }
void files_close(int id) {}
const char *files_name(int id) { return "?"; }
int files_request(int id, uint32_t command, uint32_t request) { return 0; }
void ios_trace(const char *device, uint32_t command, uint32_t request, int result) {}
static int disc_in;
int drive_has_disc(void) { return disc_in; }
int drive_read(uint64_t offset, uint32_t bytes, uint8_t *out) { return 0; }
int drive_read_raw(uint64_t offset, uint32_t bytes, uint8_t *out) { return 0; }
size_t drive_tmd(uint8_t *out, size_t limit) { return 0; }
int console_setting(const char *name, char *out, size_t limit) { return 0; }
void console_eject(void) {}
void stage_add(int stage, uint64_t ticks) {}
uint64_t time_base(void) { return 0; }
int ipc_read(uint32_t address, uint32_t *value);
int ipc_write(uint32_t address, uint32_t value);
void ipc_update(void);

enum { REQUEST = 0x80001000u, VECTORS = 0x80001100u, IN = 0x80002000u, OUT = 0x80003000u, BUFFERS = 0x80004000u };
enum { IPC_REPLY = 0xcd000008u, IPC_CONTROL = 0xcd000004u, CONTROL_REPLY = 4 };

static void make_ioctl(uint32_t request, uint32_t code, uint32_t in, uint32_t in_bytes, uint32_t out, uint32_t out_bytes) {
  memset(host(request), 0, 0x40);
  wr32(request, IOS_IOCTL);
  wr32(request + 12, code);
  wr32(request + 16, in & 0x7fffffff);
  wr32(request + 20, in_bytes);
  wr32(request + 24, out & 0x7fffffff);
  wr32(request + 28, out_bytes);
}

// An ioctlv with buffers given as address and size pairs, inputs first.
static void make_ioctlv(uint32_t request, uint32_t code, uint32_t inputs, uint32_t outputs, const uint32_t *buffers) {
  memset(host(request), 0, 0x40);
  wr32(request, IOS_IOCTLV);
  wr32(request + 12, code);
  wr32(request + 16, inputs);
  wr32(request + 20, outputs);
  wr32(request + 24, VECTORS & 0x7fffffff);
  for (uint32_t i = 0; i < inputs + outputs; i++) {
    wr32(VECTORS + i * 8, buffers[2 * i] & 0x7fffffff);
    wr32(VECTORS + i * 8 + 4, buffers[2 * i + 1]);
  }
}

static void put_hex(uint32_t address, const char *hex) {
  for (uint32_t i = 0; hex[2 * i]; i++) {
    unsigned byte;
    sscanf(hex + 2 * i, "%2x", &byte);
    wr8(address + i, byte);
  }
}

static int same_hex(uint32_t address, const char *hex) {
  for (uint32_t i = 0; hex[2 * i]; i++) {
    unsigned byte;
    sscanf(hex + 2 * i, "%2x", &byte);
    if (rd8(address + i) != byte) return 0;
  }
  return 1;
}

// Takes the next reply the game would see, or 0.
static uint32_t next_reply(void) {
  uint32_t address = 0, control = 0;
  ipc_update();
  ipc_read(IPC_CONTROL, &control);
  if (!(control & CONTROL_REPLY)) return 0;
  ipc_read(IPC_REPLY, &address);
  ipc_write(IPC_CONTROL, CONTROL_REPLY);
  return address;
}

static int aes(uint32_t code, const char *key, const char *iv, uint32_t data, uint32_t bytes) {
  put_hex(IN + 0x100, key);
  put_hex(OUT + 0x100, iv);
  const uint32_t buffers[] = {data, bytes, IN + 0x100, 16, OUT, bytes, OUT + 0x100, 16};
  make_ioctlv(REQUEST, code, 2, 2, buffers);
  return aes_request(IOS_IOCTLV, REQUEST);
}

static int sha(uint32_t code, uint32_t data, uint32_t bytes) {
  const uint32_t buffers[] = {data, bytes, OUT, 0x1c, OUT + 0x40, 20};
  make_ioctlv(REQUEST, code, 1, 2, buffers);
  return sha_request(IOS_IOCTLV, REQUEST);
}

int main(void) {
  memory_start();

  // Any number of replies wait, in order, where 64 used to stop the program.
  for (uint32_t i = 0; i < 200; i++) {
    wr32(BUFFERS + i * 0x20, IOS_IOCTL);
    ios_complete(BUFFERS + i * 0x20, (int)i);
  }
  int in_order = 1;
  for (uint32_t i = 0; i < 200; i++) in_order &= next_reply() == ((BUFFERS + i * 0x20) & 0x7fffffff);
  CHECK(in_order);
  CHECK_EQ(rd32(BUFFERS + 199 * 0x20 + 4), 199);
  CHECK_EQ(next_reply(), 0);

  // AES-128: FIPS-197's example block, then SP 800-38A's CBC example, in two pieces so
  // the IV carries on, and back again.
  put_hex(IN, "00112233445566778899aabbccddeeff");
  CHECK_EQ(aes(2, "000102030405060708090a0b0c0d0e0f", "00000000000000000000000000000000", IN, 16), 0);
  CHECK(same_hex(OUT, "69c4e0d86a7b0430d8cdb78070b4c55a"));
  put_hex(IN, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
  CHECK_EQ(aes(2, "2b7e151628aed2a6abf7158809cf4f3c", "000102030405060708090a0b0c0d0e0f", IN, 16), 0);
  CHECK(same_hex(OUT, "7649abac8119b246cee98e9b12e9197d"));
  CHECK(same_hex(OUT + 0x100, "7649abac8119b246cee98e9b12e9197d"));  // The IV to carry on with.
  put_hex(IN + 0x100, "2b7e151628aed2a6abf7158809cf4f3c");
  const uint32_t second[] = {IN + 16, 16, IN + 0x100, 16, OUT + 16, 16, OUT + 0x100, 16};
  make_ioctlv(REQUEST, 2, 2, 2, second);
  CHECK_EQ(aes_request(IOS_IOCTLV, REQUEST), 0);
  CHECK(same_hex(OUT + 16, "5086cb9b507219ee95db113a917678b2"));
  memcpy(host(IN + 0x200), host(OUT), 32);
  CHECK_EQ(aes(3, "2b7e151628aed2a6abf7158809cf4f3c", "000102030405060708090a0b0c0d0e0f", IN + 0x200, 32), 0);
  CHECK(same_hex(OUT, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"));
  CHECK_EQ((uint32_t)aes(2, "2b7e151628aed2a6abf7158809cf4f3c", "00", IN, 15), (uint32_t)IOS_EINVAL);  // Not whole blocks.
  const uint32_t copy[] = {IN, 4, OUT + 0x300, 4};
  make_ioctlv(REQUEST, 0, 1, 1, copy);
  CHECK_EQ(aes_request(IOS_IOCTLV, REQUEST), 0);
  CHECK_EQ(rd32(OUT + 0x300), rd32(IN));

  // SHA-1: short and two-chunk messages, and one sent as a whole chunk then the rest.
  strcpy((char *)host(IN), "abc");
  CHECK_EQ(sha(0, 0, 0), 0);
  CHECK_EQ(sha(2, IN, 3), 0);
  CHECK(same_hex(OUT + 0x40, "a9993e364706816aba3e25717850c26c9cd0d89d"));
  strcpy((char *)host(IN), "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
  CHECK_EQ(sha(0, 0, 0), 0);
  CHECK_EQ(sha(2, IN, 56), 0);
  CHECK(same_hex(OUT + 0x40, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
  memset(host(IN), 'a', 64);
  strcpy((char *)host(IN + 64), "abc");
  CHECK_EQ(sha(0, 0, 0), 0);
  CHECK_EQ((uint32_t)sha(1, IN, 63), (uint32_t)IOS_EINVAL);  // Only whole chunks before the last.
  CHECK_EQ(sha(1, IN, 64), 0);
  CHECK_EQ(rd32(OUT + 24), 512);  // Bits so far.
  CHECK_EQ(sha(2, IN + 64, 3), 0);
  CHECK(same_hex(OUT + 0x40, "a5177e48d19a714d0463dbeafaab7f5c6d140ff3"));
  CHECK_EQ((uint32_t)sha(3, IN, 0), (uint32_t)IOS_UNHANDLED);  // HMAC isn't done.

  // /dev/usb/oh0 with nothing plugged in: an empty list, the fixed hub descriptor, and an
  // insertion hook that waits until it's cancelled.
  wr8(IN, 4);
  wr8(IN + 1, 3);
  wr8(OUT, 0xff);
  const uint32_t list[] = {IN, 1, IN + 1, 1, OUT, 1, OUT + 0x10, 32};
  make_ioctlv(REQUEST, 12, 2, 2, list);
  CHECK_EQ(usb_oh0_request(IOS_IOCTLV, REQUEST), 0);
  CHECK_EQ(rd8(OUT), 0);
  make_ioctl(REQUEST, 15, 0, 0, OUT, 4);
  CHECK_EQ(usb_oh0_request(IOS_IOCTL, REQUEST), 0);
  CHECK_EQ(rd32(OUT), 0x02000302);
  wr16(IN, 0x057e);
  wr16(IN + 2, 0x0308);
  wr8(IN + 4, 0);
  const uint32_t hook[] = {IN, 2, IN + 2, 2, IN + 4, 1, OUT, 4};
  make_ioctlv(BUFFERS, 30, 3, 1, hook);
  CHECK_EQ(usb_oh0_request(IOS_IOCTLV, BUFFERS), IOS_NO_REPLY);
  CHECK_EQ(next_reply(), 0);
  memcpy(host(IN + 0x40), host(OUT), 4);
  make_ioctl(REQUEST, 31, IN + 0x40, 4, 0, 0);
  CHECK_EQ(usb_oh0_request(IOS_IOCTL, REQUEST), 0);
  CHECK_EQ(next_reply(), BUFFERS & 0x7fffffff);
  CHECK_EQ(rd32(BUFFERS + 4), (uint32_t)-7022);
  make_ioctlv(REQUEST, 20, 1, 1, list);
  CHECK_EQ(usb_oh0_request(IOS_IOCTLV, REQUEST), IOS_UNHANDLED);  // Port status isn't known.

  // /dev/usb/hid: the first device list comes at once and is empty, the next waits until a
  // shutdown lets it go, and no device has any ID.
  make_ioctl(REQUEST, 6, 0, 0, 0, 0);
  CHECK_EQ(usb_hid_request(IOS_IOCTL, REQUEST), 0x40001);
  make_ioctl(REQUEST, 0, 0, 0, OUT, 0x600);
  CHECK_EQ(usb_hid_request(IOS_IOCTL, REQUEST), 0);
  CHECK_EQ(rd32(OUT), 0xffffffff);
  make_ioctl(BUFFERS, 0, 0, 0, OUT + 0x800, 0x600);
  CHECK_EQ(usb_hid_request(IOS_IOCTL, BUFFERS), IOS_NO_REPLY);
  make_ioctl(REQUEST, 7, 0, 0, 0, 0);
  CHECK_EQ(usb_hid_request(IOS_IOCTL, REQUEST), 0);
  CHECK_EQ(next_reply(), BUFFERS & 0x7fffffff);
  CHECK_EQ(rd32(BUFFERS + 4), 0xffffffff);
  CHECK_EQ(rd32(OUT + 0x800), 0xffffffff);
  make_ioctl(REQUEST, 3, IN, 32, 0, 0);
  CHECK_EQ((uint32_t)usb_hid_request(IOS_IOCTL, REQUEST), (uint32_t)IOS_ENOENT);
  make_ioctl(REQUEST, 3, IN, 8, 0, 0);
  CHECK_EQ((uint32_t)usb_hid_request(IOS_IOCTL, REQUEST), (uint32_t)IOS_EINVAL);

  // /dev/usb/kbd: an event waits for a keyboard, lights are fine, reads fail.
  make_ioctl(REQUEST, 0, 0, 0, OUT, 16);
  CHECK_EQ(usb_kbd_request(IOS_IOCTL, REQUEST), IOS_NO_REPLY);
  CHECK_EQ(usb_kbd_request(IOS_WRITE, REQUEST), 0);
  CHECK_EQ((uint32_t)usb_kbd_request(IOS_READ, REQUEST), (uint32_t)IOS_EINVAL);

  // DI GetCoverStatus: 1 with the drive empty, 2 with a disc in, and a security error with
  // no room for the answer.
  make_ioctl(REQUEST, 0x88, IN, 0x20, OUT, 0x20);
  CHECK_EQ(dvd_request(IOS_IOCTL, REQUEST), 1);
  CHECK_EQ(rd32(OUT), 1);
  disc_in = 1;
  CHECK_EQ(dvd_request(IOS_IOCTL, REQUEST), 1);
  CHECK_EQ(rd32(OUT), 2);
  make_ioctl(REQUEST, 0x88, IN, 0x20, OUT, 2);
  CHECK_EQ(dvd_request(IOS_IOCTL, REQUEST), 32);
  return finish();
}
