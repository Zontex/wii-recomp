// The files the Wii Menu makes on a new Wii. argv: storage folder (holding only the
// WiiConnect24 settings), save folder (empty).
#include "harness.h"
#include "ios.h"
#include "title.h"
#include <stddef.h>
#include <sys/stat.h>
const Title *title = &(Title){.virtual_bluetooth = 1};
static size_t registered_bytes;  // The settings' size when the console last listed its remotes in them.
void bluetooth_register_remote(uint8_t *settings, size_t bytes) { registered_bytes = bytes; }
void camera_set_bar_above(int above) {}
uint32_t hw_read(uint32_t a, int n) { return 0; }
void hw_write(uint32_t a, uint32_t v, int n) {}

static uint8_t data[7 << 20];

// The file's size, with its bytes in `data`, or -1 when it isn't in the save folder.
static long load(const char *saves, const char *path) {
  char full[1024];
  snprintf(full, sizeof full, "%s%s", saves, path);
  FILE *file = fopen(full, "rb");
  if (!file) return -1;
  long bytes = (long)fread(data, 1, sizeof data, file);
  fclose(file);
  return bytes;
}

static uint32_t at32(uint32_t at) { return (uint32_t)data[at] << 24 | data[at + 1] << 16 | data[at + 2] << 8 | data[at + 3]; }
static uint32_t at16(uint32_t at) { return (uint32_t)data[at] << 8 | data[at + 1]; }

int main(int count, char **arguments) {
  memory_start();
  const char *storage = arguments[1], *saves = arguments[2];
  setenv("WII_RECOMP_SAVES", saves, 1);
  files_start(storage);
  system_files_make();

  // Storage had the settings, so they aren't made again; their backup is.
  CHECK_EQ(load(saves, "/shared2/wc24/nwc24msg.cfg"), -1);
  CHECK_EQ(load(saves, "/shared2/wc24/nwc24msg.cbk"), 0x400);
  CHECK(!memcmp(data, "WcCf", 4));
  CHECK_EQ(at32(4), 8);
  CHECK(!strcmp((char *)data + 0x18, "@wii.com"));
  uint32_t sum = 0;
  for (uint32_t i = 0; i < 0x3fc; i += 4) sum += at32(i);
  CHECK_EQ(at32(0x3fc), sum);

  CHECK_EQ(load(saves, "/shared2/wc24/nwc24fl.bin"), 32864);
  CHECK(!memcmp(data, "WcFl", 4));
  CHECK_EQ(at32(8), 100);
  CHECK_EQ(load(saves, "/shared2/wc24/nwc24fls.bin"), 12800);
  CHECK(!memcmp(data, "WcFs", 4));
  CHECK_EQ(load(saves, "/shared2/wc24/nwc24dl.bin"), 63488);
  CHECK(!memcmp(data, "WcDl", 4));
  CHECK_EQ(at16(0x14), 120);

  // A mailbox index: 127 free entries chained one to the next, the last ending it.
  CHECK_EQ(load(saves, "/shared2/wc24/mbox/wc24send.ctl"), 0x4000);
  CHECK(!memcmp(data, "WcTf", 4));
  CHECK_EQ(at32(0xc), 127);
  CHECK_EQ(at32(0x24), 2 << 20);
  CHECK_EQ(at16(0x80 + 0xe), 0x100);
  CHECK_EQ(at16(0x3f00 + 0xe), 0x3f80);
  CHECK_EQ(at16(0x3f80 + 0xe), 0);

  // The 2 MB mailbox has few enough clusters for FAT12, the 7 MB one needs FAT16.
  CHECK_EQ(load(saves, "/shared2/wc24/mbox/wc24send.mbx"), 2 << 20);
  CHECK(!memcmp(data, "VFF ", 4));
  static const uint8_t fat12[] = {0xf0, 0xff, 0xff, 0xff, 0x0f, 0x00};
  CHECK(!memcmp(data + 0x20, fat12, sizeof fat12));
  CHECK(!memcmp(data + 0x2020, fat12, sizeof fat12));
  CHECK(!memcmp(data + 0x4040, "MB         ", 11));
  CHECK_EQ(data[0x4020 + 13], 0x94);  // The long name's checksum of MB.
  CHECK(!memcmp(data + 0x5020, ".          ", 11));
  CHECK_EQ(load(saves, "/shared2/wc24/mbox/wc24recv.mbx"), 7 << 20);
  static const uint8_t fat16[] = {0xf0, 0xff, 0xff, 0xff, 0xff, 0xff};
  CHECK(!memcmp(data + 0x20, fat16, sizeof fat16));
  CHECK(!memcmp(data + 0xe040, "MB         ", 11));

  // The Mii database, with its CRC-16 over everything before it.
  CHECK_EQ(load(saves, "/shared2/menu/FaceLib/RFL_DB.dat"), 0x1f1e0);
  CHECK(!memcmp(data, "RNOD", 4));
  CHECK(!memcmp(data + 0x1d00, "RNHD", 4));
  uint16_t crc = 0;
  for (uint32_t i = 0; i < 0x1f1de; i++) {
    crc ^= (uint16_t)(data[i] << 8);
    for (int bit = 0; bit < 8; bit++) crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
  }
  CHECK_EQ(at16(0x1f1de), crc);

  // The system settings, with every item Dolphin's SysConf.cpp gives a new Wii.
  CHECK_EQ(load(saves, "/shared2/sys/SYSCONF"), 0x4000);
  CHECK(!memcmp(data, "SCv0", 4));
  CHECK(!memcmp(data + 0x3ffc, "SCed", 4));
  uint32_t items = at16(4), found = 0;
  CHECK_EQ(items, 27);
  for (uint32_t i = 0; i < items; i++) {
    uint32_t at = at16(6 + i * 2), type = data[at] >> 5, name_bytes = (data[at] & 31) + 1;
    const char *name = (const char *)data + at + 1;
    uint32_t value = at + 1 + name_bytes, size = type == 1 ? at16(value) + 1 : type == 2 ? data[value] + 1u : type == 5 ? 4 : 1;
    value += type == 1 ? 2 : type == 2 ? 1 : 0;
    CHECK(value + size <= at16(6 + (i + 1) * 2));  // Each ends where the next, or the end mark, starts.
    if (name_bytes == 7 && !memcmp(name, "BT.DINF", 7)) {
      found++;
      CHECK_EQ(size, 0x461);
      CHECK_EQ(data[value], 0);
    }
    if (name_bytes == 7 && !memcmp(name, "IPL.LNG", 7)) {
      found++;
      CHECK_EQ(data[value], 1);
    }
    if (name_bytes == 8 && !memcmp(name, "IPL.SADR", 8)) {
      found++;
      CHECK_EQ(size, 0x1008);
      CHECK_EQ(data[value], 0x6c);
    }
    if (name_bytes == 7 && !memcmp(name, "IPL.NIK", 7)) {
      found++;
      CHECK_EQ(size, 22);
      CHECK_EQ(data[value + 1], 'W');
      CHECK_EQ(data[value + 21], 3);
    }
  }
  CHECK_EQ(found, 4);
  // A game opening them finds the console's remotes listed.
  CHECK(files_open("/shared2/sys/SYSCONF", 1) >= 0);
  CHECK_EQ(registered_bytes, 0x4000);

  // A file the game has since changed is kept on the next start.
  char full[1024];
  snprintf(full, sizeof full, "%s/shared2/wc24/nwc24fl.bin", saves);
  FILE *file = fopen(full, "wb");
  fputs("changed", file);
  fclose(file);
  system_files_make();
  CHECK_EQ(load(saves, "/shared2/wc24/nwc24fl.bin"), 7);
  return finish();
}
