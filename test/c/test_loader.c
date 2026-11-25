// What the firmware leaves in low memory before a disc's game starts (loader.c's
// set_up_low_memory), for a European and an American disc. Compiled with loader.c.
#include <string.h>
#include "harness.h"
#include "ios.h"
#include "loader.h"
#include "title.h"
#include "titles.h"

const Title *title = &(Title){.ios_version = 56};
long title_tmd(uint64_t id, uint8_t *out, size_t limit) { return -1; }  // No IOS in storage.
uint16_t tmd_title_version(const uint8_t *tmd) { return 0; }
void title_verify_program(const uint8_t *data, unsigned long length) {}
int drive_read(uint64_t offset, uint32_t bytes, uint8_t *out) { return 0; }

// The console's own setting.txt: none, unless a test gives its VIDEO.
static const char *video_setting;
int console_setting(const char *name, char *out, size_t limit) {
  if (!video_setting || strcmp(name, "VIDEO")) return 0;
  snprintf(out, limit, "%s", video_setting);
  return 1;
}
int console_settings_raw(uint8_t *out) {
  for (int i = 0; i < 0x100; i++) out[i] = (uint8_t)(0xa0 ^ i);  // Scrambled, as it's stored.
  return 1;
}
int files_system_setting(const char *name, uint8_t *out, size_t limit) { return 0; }

// The video interface's registers as the boot leaves them.
static struct { uint32_t address, value; } writes[8];
static int write_count;
void hw_write(uint32_t address, uint32_t value, int bytes) {
  if (write_count < 8) writes[write_count].address = address, writes[write_count++].value = value;
}
static uint32_t written(uint32_t address) {
  for (int i = 0; i < write_count; i++) if (writes[i].address == address) return writes[i].value;
  return 0xdeadbeef;
}

static uint8_t table[100];
extern int gamecube_mode;

static void start(const char *game_id) {
  Boot boot = {.table = table, .table_bytes = sizeof table};
  memcpy(boot.header, game_id, 6);
  write_count = 0;
  set_up_low_memory(&boot);
}

int main(void) {
  memory_start();
  for (int i = 0; i < (int)sizeof table; i++) table[i] = (uint8_t)i;

  // A European disc, on a console with no setting.txt of its own: PAL, from the disc's
  // region letter, and the video interface left running with a display interrupt in each
  // field, as the system menu leaves it.
  start("RMCP01");
  CHECK_EQ(rd32(0x800000cc), 1);
  CHECK(!memcmp(host(0x80003180u), "RMCP", 4));  // The game code, which some games check their region by.
  CHECK_EQ(written(0xcc002002u), 1);
  CHECK_EQ(written(0xcc002030u), 1u << 28 | 263u << 16 | 430u);
  CHECK_EQ(written(0xcc002034u), 1u << 28 | 1u << 16 | 1u);

  // The file table just below the system menu's arena top, 32-byte aligned, and the arena
  // ending at it: nothing spare is taken from the game's heap.
  uint32_t table_at = (0x817fec60u - (uint32_t)sizeof table) & ~31u;
  CHECK_EQ(rd32(0x80000038), table_at);
  CHECK_EQ(rd32(0x8000003c), sizeof table);
  CHECK_EQ(rd32(0x80000034), table_at);
  CHECK_EQ(rd32(0x80003110), table_at);
  CHECK(!memcmp(host(table_at), table, sizeof table));
  // setting.txt as stored, still scrambled, where the system menu leaves it.
  CHECK_EQ(rd8(0x80003800u), 0xa0);
  CHECK_EQ(rd8(0x800038ffu), 0xa0 ^ 0xff);

  // An American disc: NTSC, and the video interface left for the game's VIInit.
  start("RMCE01");
  CHECK_EQ(rd32(0x800000cc), 0);
  CHECK(!memcmp(host(0x80003180u), "RMCE", 4));
  CHECK_EQ(write_count, 0);

  // A console's own setting.txt wins over the disc's letter.
  video_setting = "PAL";
  start("RMCE01");
  CHECK_EQ(rd32(0x800000cc), 1);
  video_setting = "NTSC";
  start("RMCP01");
  CHECK_EQ(rd32(0x800000cc), 0);

  // A GameCube disc: its own ID with the GameCube mark, the file table at the top of main
  // memory with the arena below it, 16 MB of audio memory, a GameCube's clocks, and
  // nothing of the Wii's.
  video_setting = 0;
  memset(host(0x80003100u), 0, 0x100);
  memset(host(0x80003800u), 0, 0x100);
  gamecube_mode = 1;
  Boot boot = {.table = table, .table_bytes = sizeof table};
  memcpy(boot.header, "GTSE01", 6);
  boot.header[0x1c] = 0xc2, boot.header[0x1d] = 0x33, boot.header[0x1e] = 0x9f, boot.header[0x1f] = 0x3d;
  set_up_low_memory(&boot);
  CHECK_EQ(rd32(0x8000001c), 0xc2339f3du);
  CHECK_EQ(rd32(0x80000018), 0);
  CHECK_EQ(rd32(0x8000002c), 0x10000006u);
  CHECK_EQ(rd32(0x800000d0), 0x01000000u);
  CHECK_EQ(rd32(0x800000f8), 162000000u);
  CHECK_EQ(rd32(0x800000fc), 486000000u);
  CHECK_EQ(rd32(0x800000cc), 0);  // An American disc: NTSC.
  table_at = (0x81800000u - (uint32_t)sizeof table) & ~31u;
  CHECK_EQ(rd32(0x80000038), table_at);
  CHECK_EQ(rd32(0x80000034), table_at);
  CHECK(!memcmp(host(table_at), table, sizeof table));
  CHECK_EQ(rd32(0x80003100), 0);  // No Wii memory sizes, IOS or settings.
  CHECK_EQ(rd32(0x80003140), 0);
  CHECK_EQ(rd8(0x80003800u), 0);
  return finish();
}
