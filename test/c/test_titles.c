// Installed titles, from a made-up storage folder written by test_recomp_runtime.py.
// argv: storage folder, save folder.
#include "harness.h"
#include "ios.h"
#include "titles.h"
#include <stddef.h>
#include <string.h>
const int game_virtual_bluetooth = 0;
void bluetooth_register_remote(uint8_t *settings, size_t bytes) {}
void camera_set_bar_above(int above) {}
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}

enum { MENU_HIGH = 1, MENU_LOW = 2 };
#define MENU 0x0000000100000002ull
#define CHANNEL 0x0001000148414141ull
#define BROKEN 0x0001000148424242ull

static uint8_t tmd[TMD_MAX_BYTES], tickets[TICKET_BYTES * MAX_TICKETS], view[TMD_MAX_BYTES];

int main(int count, char **arguments) {
  memory_start();
  files_start(arguments[1]);

  uint64_t titles[8];
  int installed = titles_installed(titles, 8);
  CHECK_EQ(installed, 3);  // A title folder with a TMD counts, even a broken one, as on a Wii.
  CHECK_EQ(titles[0], MENU);
  CHECK_EQ(titles[1], CHANNEL);
  CHECK_EQ(titles[2], BROKEN);
  CHECK_EQ(titles_installed(titles, 1), 1);
  int owned = titles_owned(titles, 8);
  CHECK_EQ(owned, 1);
  CHECK_EQ(titles[0], MENU);

  long size = title_tmd(MENU, tmd, sizeof tmd);
  CHECK_EQ(size, TMD_HEADER_BYTES + 2 * TMD_CONTENT_BYTES);
  CHECK_EQ(tmd_title(tmd), MENU);
  CHECK_EQ(tmd_ios(tmd), 0x0000000100000050ull);
  CHECK_EQ(title_tmd(BROKEN, tmd + 0, sizeof tmd), (unsigned long long)-1);
  CHECK_EQ(title_tmd(0x0000000100000099ull, tmd, sizeof tmd), (unsigned long long)-1);
  CHECK_EQ(title_tmd(MENU, tmd, 16), (unsigned long long)-1);  // No room.
  size = title_tmd(MENU, tmd, sizeof tmd);

  TmdContent first = tmd_content(tmd, 0), second = tmd_content(tmd, 1);
  CHECK_EQ(first.id, 0x1f);
  CHECK_EQ(first.index, 0);
  CHECK_EQ(second.id, 0x20);
  CHECK_EQ(second.type, 0x8001);
  CHECK_EQ(second.size, 5);
  char path[TITLE_PATH_BYTES];
  CHECK(title_content_path(MENU, &first, path, sizeof path));
  CHECK(!strcmp(path, "/title/00000001/00000002/content/0000001f.app"));
  CHECK(title_content_path(MENU, &second, path, sizeof path));
  CHECK(!strcmp(path, "/shared1/00000007.app"));

  CHECK_EQ(tmd_view_bytes(tmd, (size_t)size), TMD_VIEW_HEADER_BYTES + 2 * TMD_VIEW_CONTENT_BYTES);
  CHECK_EQ(tmd_view(tmd, (size_t)size, view, 8), 0);  // No room.
  size_t view_size = tmd_view(tmd, (size_t)size, view, sizeof view);
  CHECK_EQ(view_size, 0x80);
  CHECK_EQ(view[0], 1);                                  // TMD format version.
  CHECK(!memcmp(view + 4, tmd + 0x184, 8));             // Required IOS.
  CHECK(!memcmp(view + 0x0c, tmd + 0x18c, 8));          // Title.
  CHECK_EQ(view[0x58] << 8 | view[0x59], 0x1234);       // Title version.
  CHECK_EQ(view[0x5a] << 8 | view[0x5b], 2);            // Contents.
  CHECK_EQ(view[0x5c] | view[0x5d] | view[0x5e], 0);    // Boot index, widened.
  CHECK_EQ(view[0x5f], 1);
  CHECK(!memcmp(view + 0x60, tmd + 0x1e4, 16));
  CHECK(!memcmp(view + 0x70, tmd + 0x1e4 + 36, 16));

  long ticket_size = title_tickets(MENU, tickets, sizeof tickets);
  CHECK_EQ(ticket_size, TICKET_BYTES);
  uint8_t ticket[TICKET_VIEW_BYTES];
  ticket_view(tickets, ticket);
  CHECK_EQ(ticket[3], 0);
  CHECK(!memcmp(ticket + 4, tickets + 0x1d0, TICKET_VIEW_BYTES - 4));
  CHECK_EQ(title_tickets(CHANNEL, tickets, sizeof tickets), (unsigned long long)-1);

  CHECK_EQ(tmd_check(tmd, TMD_HEADER_BYTES), -1);  // Too short for its contents.
  return finish();
}
