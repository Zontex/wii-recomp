// The drive reads the same disc from an extracted folder and from a recompiled disc
// image, written by test_recomp_disc.py. argv: image, system folder, cache folder, disc
// bytes to compare, a folder with no game ID.
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
  uint64_t bytes = strtoull(arguments[4], 0, 0);
  CHECK(drive_insert(arguments[2]));
  CHECK(drive_has_disc());
  uint8_t *from_folder = read_all(bytes);
  uint8_t ticket[16], tmd[16];
  CHECK_EQ(drive_ticket(ticket, sizeof ticket), sizeof ticket);
  char path[1024];
  CHECK(!drive_native(arguments[3], path, sizeof path));  // A folder carries no native code.

  CHECK(drive_insert(arguments[1]));
  CHECK(drive_has_disc());
  uint8_t *from_image = read_all(bytes);
  CHECK(!memcmp(from_folder, from_image, bytes));
  uint8_t piece[3];  // Across the end of one file, through padding and into the next.
  CHECK(drive_read(0x253e, 3, piece));
  CHECK(!memcmp(piece, from_folder + 0x253e, 3));
  uint8_t image_ticket[16];
  CHECK_EQ(drive_ticket(image_ticket, sizeof image_ticket), sizeof image_ticket);
  CHECK(!memcmp(ticket, image_ticket, sizeof ticket));
  CHECK_EQ(drive_tmd(tmd, sizeof tmd), sizeof tmd);
  CHECK_EQ(tmd[0], 0x7d);

  CHECK(drive_native(arguments[3], path, sizeof path));
  FILE *file = fopen(path, "rb");
  char text[32] = {0};
  CHECK(file && fread(text, 1, sizeof text - 1, file) > 0);
  CHECK(!strcmp(text, drive_platform()));  // The test's stand-in library holds its platform's name.
  if (file) fclose(file);
  char again[1024];
  CHECK(drive_native(arguments[3], again, sizeof again));
  CHECK(!strcmp(path, again));

  CHECK(drive_insert(arguments[5]));  // A system title's folder: nothing in the drive.
  CHECK(!drive_has_disc());
  CHECK(!drive_read(0, 4, piece));
  drive_eject();
  CHECK(!drive_has_disc());
  return finish();
}
