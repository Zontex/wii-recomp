// A GameCube disc in the drive, written by test_recomp_disc.py: its file table's byte
// offsets, read the same from an extracted folder and from an image, with its own start
// as the unencrypted area and no ticket or TMD. argv: image, system folder, disc bytes,
// the offset of a file and its text, a Wii disc's system folder.
#include <string.h>
#include "harness.h"
#include "drive.h"

static uint8_t *read_all(uint64_t bytes) {
  uint8_t *data = malloc(bytes);
  CHECK(drive_read(0, (uint32_t)bytes, data));
  return data;
}

int main(int count, char **arguments) {
  (void)count;
  uint64_t bytes = strtoull(arguments[3], 0, 0), file_at = strtoull(arguments[4], 0, 0);
  const char *text = arguments[5];
  CHECK(drive_insert(arguments[2]));
  CHECK(drive_is_gamecube());
  uint8_t *from_folder = read_all(bytes);
  CHECK(!memcmp(from_folder + file_at, text, strlen(text)));
  uint8_t start[0x20], tmd[16];
  CHECK(drive_read_raw(0, sizeof start, start));
  CHECK(!memcmp(start, from_folder, sizeof start));
  CHECK_EQ(start[0x1c], 0xc2);
  CHECK_EQ(drive_tmd(tmd, sizeof tmd), 0);

  CHECK(drive_insert(arguments[1]));
  CHECK(drive_is_gamecube());
  uint8_t *from_image = read_all(bytes);
  CHECK(!memcmp(from_folder, from_image, bytes));
  CHECK(drive_read_raw(0, sizeof start, start));
  CHECK(!memcmp(start, from_folder, sizeof start));
  CHECK_EQ(drive_ticket(tmd, sizeof tmd), 0);

  CHECK(drive_insert(arguments[6]));  // A Wii disc isn't one.
  CHECK(drive_has_disc());
  CHECK(!drive_is_gamecube());
  drive_eject();
  CHECK(!drive_is_gamecube());
  return finish();
}
