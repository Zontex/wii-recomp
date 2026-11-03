// /dev/fs and open files, on two real folders. argv: storage folder, save folder.
#include "harness.h"
#include "ios.h"
#include "title.h"
#include <stddef.h>
const Title *title = &(Title){0};
void bluetooth_register_remote(uint8_t *settings, size_t bytes) {}
void camera_set_bar_above(int above) {}
int files_request(int id, uint32_t command, uint32_t request);
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}
enum { REQUEST = 0x80001000u, IN = 0x80002000u, OUT = 0x80003000u, DATA = 0x80004000u, VECTORS = 0x80005000u, ATTR_PATH = 6 };
enum { CREATE_DIR = 3, READ_DIR = 4, GET_ATTR = 6, DELETE = 7, RENAME = 8, CREATE_FILE = 9 };

static int ioctl_path(uint32_t code, const char *path, int offset, const char *other) {
  memset(host(IN), 0, 256);
  strcpy((char *)host(IN + offset), path);
  if (other) strcpy((char *)host(IN + 64), other);
  wr32(REQUEST + 12, code);
  wr32(REQUEST + 16, IN & 0x7fffffff);
  wr32(REQUEST + 24, OUT & 0x7fffffff);
  wr32(REQUEST + 28, 0x4c);
  return fs_request(IOS_IOCTL, REQUEST);
}
static int list_count(const char *path) {
  strcpy((char *)host(IN), path);
  wr32(REQUEST + 12, READ_DIR);
  wr32(REQUEST + 16, 1);
  wr32(REQUEST + 24, VECTORS & 0x7fffffff);
  wr32(VECTORS, IN & 0x7fffffff);
  wr32(VECTORS + 8, OUT & 0x7fffffff);
  int result = fs_request(IOS_IOCTLV, REQUEST);
  if (result) return result;
  return (int)rd32(OUT);
}
static int put_bytes(int id, const char *text) {
  strcpy((char *)host(DATA), text);
  wr32(REQUEST + 12, DATA & 0x7fffffff);
  wr32(REQUEST + 16, (uint32_t)strlen(text));
  return files_request(id, IOS_WRITE, REQUEST);
}
static int exists(const char *folder, const char *path) {
  char full[1024], found[1024];
  snprintf(full, sizeof full, "%s%s", folder, path);
  FILE *f = fopen(full, "rb");
  if (f) fclose(f);
  (void)found;
  return f != 0;
}
int main(int count, char **arguments) {
  memory_start();
  const char *storage = arguments[1], *saves = arguments[2];
  setenv("WII_RECOMP_SAVES", saves, 1);
  files_start(storage);
  char full[1024];

  // Reads fall through to storage; saves win once they exist.
  CHECK(files_find("/shared2/only_storage", full, sizeof full));
  CHECK(strstr(full, storage) == full);
  CHECK(!files_find("/missing", full, sizeof full));

  // Writing a storage file lands in saves on close.
  int id = files_open("/shared2/only_storage");
  CHECK(id >= 0);
  CHECK_EQ(put_bytes(id, "new"), 3);
  files_close(id);
  CHECK(exists(saves, "/shared2/only_storage"));
  CHECK(files_find("/shared2/only_storage", full, sizeof full));
  CHECK(strstr(full, saves) == full);

  // An untouched file isn't copied.
  id = files_open("/shared2/read_only");
  CHECK(id >= 0);
  files_close(id);
  CHECK(!exists(saves, "/shared2/read_only"));

  // Create, attributes, folder, listing that merges both places without doubles.
  CHECK_EQ(ioctl_path(CREATE_FILE, "/shared2/made", ATTR_PATH, 0), 0);
  CHECK_EQ((uint32_t)ioctl_path(CREATE_FILE, "/shared2/made", ATTR_PATH, 0), (uint32_t)-105);
  CHECK_EQ((uint32_t)ioctl_path(CREATE_FILE, "/shared2/read_only", ATTR_PATH, 0), (uint32_t)-105);
  CHECK_EQ(ioctl_path(CREATE_DIR, "/new/folder", ATTR_PATH, 0), 0);
  CHECK(!exists(storage, "/shared2/made"));
  CHECK_EQ(ioctl_path(GET_ATTR, "/shared2/read_only", 0, 0), 0);
  CHECK_EQ((uint32_t)ioctl_path(GET_ATTR, "/missing", 0, 0), (uint32_t)-106);
  CHECK_EQ(list_count("/shared2"), 3);  // only_storage in both, read_only, made.
  CHECK_EQ(list_count("/shared2/made"), -101);  // A file isn't a folder: NANDGetType depends on it.

  // Deleting and renaming storage files never touches storage.
  CHECK_EQ(ioctl_path(DELETE, "/shared2/read_only", 0, 0), 0);
  CHECK_EQ(ioctl_path(RENAME, "/shared2/read_only", 0, "/shared2/moved"), 0);
  CHECK(exists(saves, "/shared2/moved"));
  CHECK_EQ(ioctl_path(RENAME, "/shared2/moved", 0, "/shared2/moved2"), 0);
  CHECK(!exists(saves, "/shared2/moved"));
  CHECK(exists(saves, "/shared2/moved2"));
  CHECK_EQ(ioctl_path(DELETE, "/shared2/made", 0, 0), 0);
  CHECK(!exists(saves, "/shared2/made"));

  // No storage folder at all, as when none is given on the command line.
  files_start(0);
  CHECK(!files_find("/shared2/read_only", full, sizeof full));
  CHECK(strstr(full, saves) == full);
  return finish();
}
