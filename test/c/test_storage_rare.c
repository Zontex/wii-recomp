// /dev/fs attributes, open modes and shutdown, on a real save folder. argv: save folder.
#include "harness.h"
#include "ios.h"
#include "title.h"
#include <stddef.h>
#include <string.h>
const Title *title = &(Title){0};
void bluetooth_register_remote(uint8_t *settings, size_t bytes) {}
void camera_set_bar_above(int above) {}
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
enum { REQUEST = 0x80001000u, IN = 0x80002000u, OUT = 0x80003000u, DATA = 0x80004000u };
enum { CREATE_DIR = 3, SET_ATTR = 5, GET_ATTR = 6, DELETE = 7, RENAME = 8, CREATE_FILE = 9, SHUTDOWN = 13 };
enum { READ = 1, WRITE = 2, NONE = 0 };

static int fs_ioctl(uint32_t code, uint32_t in_bytes, uint32_t out_bytes) {
  wr32(REQUEST + 12, code);
  wr32(REQUEST + 16, IN & 0x7fffffff);
  wr32(REQUEST + 20, in_bytes);
  wr32(REQUEST + 24, OUT & 0x7fffffff);
  wr32(REQUEST + 28, out_bytes);
  return fs_request(IOS_IOCTL, REQUEST);
}

// The parameters CREATEFILE, CREATEDIR and SETATTR take.
static int with_attributes(uint32_t code, const char *path, uint32_t owner, int owner_mode, int group_mode, int other_mode, int flags) {
  memset(host(IN), 0, 0x4c);
  wr32(IN, owner);
  wr16(IN + 4, 0x3031);
  strcpy((char *)host(IN + 6), path);
  wr8(IN + 70, owner_mode);
  wr8(IN + 71, group_mode);
  wr8(IN + 72, other_mode);
  wr8(IN + 73, flags);
  return fs_ioctl(code, 0x4c, 0);
}

static int with_path(uint32_t code, const char *path, const char *other) {
  memset(host(IN), 0, 128);
  strcpy((char *)host(IN), path);
  if (other) strcpy((char *)host(IN + 64), other);
  return fs_ioctl(code, 128, 0x4c);
}

static int io(int id, uint32_t command, const char *text) {
  if (text) strcpy((char *)host(DATA), text);
  wr32(REQUEST + 12, DATA & 0x7fffffff);
  wr32(REQUEST + 16, text ? (uint32_t)strlen(text) : 4);
  return files_request(id, command, REQUEST);
}

static long saved_size(const char *saves, const char *path) {
  char full[1024];
  snprintf(full, sizeof full, "%s%s", saves, path);
  FILE *file = fopen(full, "rb");
  if (!file) return -1;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fclose(file);
  return size;
}

int main(int count, char **arguments) {
  memory_start();
  const char *saves = arguments[1];
  setenv("WII_RECOMP_SAVES", saves, 1);
  files_start(0);

  // A new file keeps the permissions and flags it was made with; its owner is 0.
  CHECK_EQ(with_attributes(CREATE_DIR, "/title", 0, 3, 3, 3, 0), 0);
  CHECK_EQ(with_attributes(CREATE_FILE, "/title/a", 0x1234, READ | WRITE, READ, NONE, 0x10), 0);
  CHECK_EQ(with_path(GET_ATTR, "/title/a", 0), 0);
  CHECK_EQ(rd32(OUT), 0);
  CHECK_EQ(rd8(OUT + 70), READ | WRITE);
  CHECK_EQ(rd8(OUT + 71), READ);
  CHECK_EQ(rd8(OUT + 72), NONE);
  CHECK_EQ(rd8(OUT + 73), 0x10);
  CHECK_EQ(with_path(GET_ATTR, "/title", 0), 0);
  CHECK_EQ(rd8(OUT + 72), READ | WRITE);  // Nothing asked for: open to all.

  // SETATTR keeps what it's given, including a new owner for an empty file...
  CHECK_EQ(with_attributes(SET_ATTR, "/title/a", 0x1000, READ, READ, READ, 0), 0);
  CHECK_EQ(with_path(GET_ATTR, "/title/a", 0), 0);
  CHECK_EQ(rd32(OUT), 0x1000);
  CHECK_EQ(rd16(OUT + 4), 0x3031);
  CHECK_EQ(rd8(OUT + 70), READ);
  CHECK_EQ(rd8(OUT + 73), 0);
  CHECK_EQ((uint32_t)with_attributes(SET_ATTR, "/title/missing", 0, 3, 3, 3, 0), (uint32_t)-106);
  CHECK_EQ((uint32_t)with_attributes(SET_ATTR, "relative", 0, 3, 3, 3, 0), (uint32_t)-101);

  // ...but not for one with something in it.
  int id = files_open("/title/a", READ | WRITE);
  CHECK(id >= 0);
  CHECK_EQ(io(id, IOS_WRITE, "data"), 4);
  files_close(id);
  CHECK_EQ((uint32_t)with_attributes(SET_ATTR, "/title/a", 0x2000, READ, READ, READ, 0), (uint32_t)-115);
  CHECK_EQ(with_attributes(SET_ATTR, "/title/a", 0x1000, READ | WRITE, READ, READ, 0), 0);

  // Attributes move with a rename and go with a delete.
  CHECK_EQ(with_path(RENAME, "/title/a", "/title/b"), 0);
  CHECK_EQ(with_path(GET_ATTR, "/title/b", 0), 0);
  CHECK_EQ(rd32(OUT), 0x1000);
  CHECK_EQ(with_path(DELETE, "/title/b", 0), 0);
  CHECK_EQ(with_attributes(CREATE_FILE, "/title/b", 0, 3, 3, 3, 0), 0);
  CHECK_EQ(with_path(GET_ATTR, "/title/b", 0), 0);
  CHECK_EQ(rd32(OUT), 0);
  CHECK_EQ(rd8(OUT + 70), READ | WRITE);

  // Open modes: a handle may only do what it was opened for. Folders can't be opened.
  id = files_open("/title/b", READ);
  CHECK(id >= 0);
  CHECK_EQ((uint32_t)io(id, IOS_WRITE, "no"), (uint32_t)-102);
  CHECK_EQ(io(id, IOS_READ, 0), 0);
  files_close(id);
  id = files_open("/title/b", WRITE);
  CHECK_EQ((uint32_t)io(id, IOS_READ, 0), (uint32_t)-102);
  CHECK_EQ(io(id, IOS_WRITE, "yes"), 3);

  // SHUTDOWN writes out what's open before the power goes, without closing it.
  CHECK_EQ(saved_size(saves, "/title/b"), 0);
  CHECK_EQ(fs_ioctl(SHUTDOWN, 0, 0), 0);
  CHECK_EQ(saved_size(saves, "/title/b"), 3);
  CHECK_EQ(io(id, IOS_WRITE, "!"), 1);
  files_close(id);
  CHECK_EQ(saved_size(saves, "/title/b"), 4);
  CHECK_EQ((uint32_t)files_open("/title", READ), (uint32_t)-101);
  CHECK_EQ((uint32_t)files_open("/title/none", READ), (uint32_t)-106);
  return finish();
}
