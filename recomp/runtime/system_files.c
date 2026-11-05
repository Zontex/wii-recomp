// Files the Wii Menu makes when a new Wii first starts, which games expect to find:
// WiiConnect24's settings, lists and mailboxes, and the Mii database. A game such as
// Mario Kart Wii stops at "Could not write to/read from Wii system memory" without
// them. Any that neither the save folder nor the Wii storage folder has are made here,
// empty, in the save folder, so the storage folder is never written to.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ios.h"

enum {
  MAX_PATH = 1024, CONFIG_BYTES = 0x400, URL_BYTES = 0x80, MAIL_ENTRY_BYTES = 0x80,
  VFF_HEADER_BYTES = 0x20, VFF_ROOT_BYTES = 0x1000, VFF_CLUSTER_BYTES = 0x200, FAT12_MOST_CLUSTERS = 4084,
  FAT_ENTRY_BYTES = 32, FAT_FOLDER = 0x10, FAT_LONG_NAME = 0x0f, FAT_DATE_2010_01_01 = 0x3c21,
  MII_DATABASE_BYTES = 0x1f1e0, MII_PARADE = 0x1d00,
};

static void put32(uint8_t *at, uint32_t value) {
  at[0] = value >> 24; at[1] = value >> 16; at[2] = value >> 8; at[3] = value;
}

static void put16(uint8_t *at, uint16_t value) { at[0] = value >> 8; at[1] = value; }

static void put16_little(uint8_t *at, uint16_t value) { at[0] = value; at[1] = value >> 8; }

// WiiConnect24's settings, as before the console has asked for its ID: the Wii's
// address domain and Nintendo's servers, then a sum of every word before the last.
static void fill_settings(uint8_t *data, uint32_t bytes) {
  static const char *servers[] = {
      "https://amw.wc24.wii.com/cgi-bin/account.cgi", "http://rcw.wc24.wii.com/cgi-bin/check.cgi",
      "http://mtw.wc24.wii.com/cgi-bin/receive.cgi", "http://mtw.wc24.wii.com/cgi-bin/delete.cgi",
      "http://mtw.wc24.wii.com/cgi-bin/send.cgi"};
  memcpy(data, "WcCf", 4);
  put32(data + 4, 8);  // Version.
  strcpy((char *)data + 0x18, "@wii.com");
  for (int i = 0; i < 5; i++) strcpy((char *)data + 0x9c + i * URL_BYTES, servers[i]);
  uint32_t sum = 0;
  for (uint32_t at = 0; at < bytes - 4; at += 4) sum += (uint32_t)data[at] << 24 | data[at + 1] << 16 | data[at + 2] << 8 | data[at + 3];
  put32(data + bytes - 4, sum);
}

static void fill_friends(uint8_t *data, uint32_t bytes) {
  memcpy(data, "WcFl", 4);
  put32(data + 4, 2);
  put32(data + 8, 100);  // Room for 100 friends.
}

static void fill_friend_status(uint8_t *data, uint32_t bytes) {
  memcpy(data, "WcFs", 4);
  put32(data + 4, 2);
  put32(data + 8, 0x150);
}

// The list of things to download in the background: room for 120, and none yet.
static void fill_downloads(uint8_t *data, uint32_t bytes) {
  memcpy(data, "WcDl", 4);
  put32(data + 4, 1);
  put16(data + 0x10, 32);   // Most parts one download may have.
  put16(data + 0x12, 8);    // Mail slots kept back.
  put16(data + 0x14, 120);  // Most downloads.
}

// A mailbox's index: a header, then entries chained together as free.
static void fill_mail_index(uint8_t *data, uint32_t bytes, uint32_t mailbox_bytes) {
  uint32_t entries = bytes / MAIL_ENTRY_BYTES - 1;
  memcpy(data, "WcTf", 4);
  put32(data + 4, 4);
  put32(data + 0xc, entries);
  put32(data + 0x14, bytes);
  put32(data + 0x18, 1);  // The next message's number.
  put32(data + 0x1c, MAIL_ENTRY_BYTES);  // The first free entry.
  put32(data + 0x24, mailbox_bytes);
  memset(data + 0x58, '0', 39);
  for (uint32_t i = 1; i < entries; i++) put16(data + i * MAIL_ENTRY_BYTES + 0xe, (i + 1) * MAIL_ENTRY_BYTES);
}

static void fill_send_index(uint8_t *data, uint32_t bytes) { fill_mail_index(data, bytes, 2 << 20); }

static void fill_receive_index(uint8_t *data, uint32_t bytes) { fill_mail_index(data, bytes, 7 << 20); }

static void folder_entry(uint8_t *at, const char *name, uint16_t cluster) {
  memcpy(at, name, 11);
  at[11] = FAT_FOLDER;
  put16_little(at + 16, FAT_DATE_2010_01_01);  // Made,
  put16_little(at + 18, FAT_DATE_2010_01_01);  // last opened,
  put16_little(at + 24, FAT_DATE_2010_01_01);  // and last changed.
  put16_little(at + 26, cluster);
}

// A mailbox: a small FAT disk (a VFF) holding one empty folder, MB, for the messages.
// Two copies of the allocation table, the top folder, then the data, 512 bytes a cluster.
static void fill_mailbox(uint8_t *data, uint32_t bytes) {
  memcpy(data, "VFF ", 4);
  data[4] = 0xfe; data[5] = 0xff;  // Byte order mark.
  data[6] = 1;                      // Version.
  put32(data + 8, bytes);
  put16(data + 12, VFF_HEADER_BYTES);
  uint32_t table = bytes / VFF_CLUSTER_BYTES * 2, top = VFF_HEADER_BYTES + 2 * table, folder = top + VFF_ROOT_BYTES;
  int fat12 = (bytes - folder) / VFF_CLUSTER_BYTES <= FAT12_MOST_CLUSTERS;  // FAT's type goes by its data clusters.
  for (int copy = 0; copy < 2; copy++) {
    uint8_t *at = data + VFF_HEADER_BYTES + copy * table;
    // The two reserved entries, then cluster 2 (the MB folder) as a chain's end.
    static const uint8_t fat12_start[] = {0xf0, 0xff, 0xff, 0xff, 0x0f}, fat16_start[] = {0xf0, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (fat12) memcpy(at, fat12_start, sizeof fat12_start);
    else memcpy(at, fat16_start, sizeof fat16_start);
  }
  // The folder's long name, "mb", then its short name, MB.
  uint8_t *name = data + top, checksum = 0;
  const char *short_name = "MB         ";
  for (int i = 0; i < 11; i++) checksum = (uint8_t)(((checksum & 1) << 7) + (checksum >> 1) + (uint8_t)short_name[i]);
  memset(name, 0xff, FAT_ENTRY_BYTES);
  name[0] = 0x41;  // The first and last part of the name.
  memcpy(name + 1, "m\0b\0\0\0", 6);
  name[11] = FAT_LONG_NAME;
  name[12] = 0;
  name[13] = checksum;
  name[26] = name[27] = 0;
  folder_entry(data + top + FAT_ENTRY_BYTES, short_name, 2);
  folder_entry(data + folder, ".          ", 2);
  folder_entry(data + folder + FAT_ENTRY_BYTES, "..         ", 0);
}

// The Mii database with no Miis: its two parts' marks, then a CRC-16 of the rest.
static void fill_miis(uint8_t *data, uint32_t bytes) {
  memcpy(data, "RNOD", 4);
  memcpy(data + MII_PARADE, "RNHD", 4);
  uint16_t crc = 0;
  for (uint32_t i = 0; i < bytes - 2; i++) {
    crc ^= (uint16_t)(data[i] << 8);
    for (int bit = 0; bit < 8; bit++) crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
  }
  put16(data + bytes - 2, crc);
}

typedef struct {
  const char *path;
  uint32_t bytes;
  void (*fill)(uint8_t *data, uint32_t bytes);
} SystemFile;

static const SystemFile system_files[] = {
    {"/shared2/wc24/nwc24msg.cfg", CONFIG_BYTES, fill_settings},
    {"/shared2/wc24/nwc24msg.cbk", CONFIG_BYTES, fill_settings},  // The settings' backup copy.
    {"/shared2/wc24/nwc24fl.bin", 32864, fill_friends},
    {"/shared2/wc24/nwc24fls.bin", 12800, fill_friend_status},
    {"/shared2/wc24/nwc24dl.bin", 63488, fill_downloads},
    {"/shared2/wc24/mbox/wc24send.ctl", 0x4000, fill_send_index},
    {"/shared2/wc24/mbox/wc24recv.ctl", 0x8000, fill_receive_index},
    {"/shared2/wc24/mbox/wc24send.mbx", 2 << 20, fill_mailbox},
    {"/shared2/wc24/mbox/wc24recv.mbx", 7 << 20, fill_mailbox},
    {"/shared2/menu/FaceLib/RFL_DB.dat", MII_DATABASE_BYTES, fill_miis},
};

void system_files_make(void) {
  for (size_t i = 0; i < sizeof system_files / sizeof *system_files; i++) {
    const SystemFile *file = &system_files[i];
    char full[MAX_PATH];
    if (files_find(file->path, full, sizeof full)) continue;
    uint8_t *data = calloc(1, file->bytes);
    file->fill(data, file->bytes);
    files_save_path(file->path, full, sizeof full);
    files_make_parents(full);
    FILE *out = fopen(full, "wb");
    if (out && fwrite(data, 1, file->bytes, out) == file->bytes) {
      fprintf(stderr, "storage: made a new %s, as the Wii Menu does when a Wii first starts\n", file->path);
    } else {
      fprintf(stderr, "storage: couldn't make %s in the save folder\n", file->path);
    }
    if (out) fclose(out);
    free(data);
  }
}
