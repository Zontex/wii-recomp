// Installed titles, read from the console's storage through files.c. Layouts follow
// WiiBrew's descriptions of the TMD, the ticket and shared contents.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ios.h"
#include "titles.h"

enum {
  TMD_IOS = 0x184, TMD_TITLE = 0x18c, TMD_VIEW_FROM = 0x180, TMD_ACCESS_RIGHTS = 0x1d8, TMD_TITLE_VERSION = 0x1dc,
  TMD_CONTENT_COUNT = 0x1de, TMD_BOOT_INDEX = 0x1e0,
  TICKET_VERSION = 0x1bc, TICKET_VIEW_FROM = 0x1d0,
  SHARED = 0x8000, MAP_ENTRY_BYTES = 28, MAP_NAME_BYTES = 8, HASH_BYTES = 20,
  MAX_PATH = 1024, MAX_MAP_BYTES = 1 << 20,
};

static uint16_t big16(const uint8_t *data) { return (uint16_t)(data[0] << 8 | data[1]); }
static uint32_t big32(const uint8_t *data) { return (uint32_t)big16(data) << 16 | big16(data + 2); }
static uint64_t big64(const uint8_t *data) { return (uint64_t)big32(data) << 32 | big32(data + 4); }

// Reads a whole file from storage. Returns its size, or -1 if it's missing or too big.
static long read_storage(const char *path, uint8_t *out, size_t limit) {
  char full[MAX_PATH];
  if (!files_find(path, full, sizeof full)) return -1;
  FILE *file = fopen(full, "rb");
  if (!file) return -1;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  rewind(file);
  if (size < 0 || (size_t)size > limit || fread(out, 1, (size_t)size, file) != (size_t)size) size = -1;
  fclose(file);
  return size;
}

static int hex_name(const char *name, uint32_t *value) {
  char *end;
  if (strlen(name) < 8) return 0;
  *value = (uint32_t)strtoul(name, &end, 16);
  return end == name + 8;
}

static int compare_titles(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}

static int add_unique(uint64_t *out, int count, int limit, uint64_t title) {
  for (int i = 0; i < count; i++) if (out[i] == title) return count;
  if (count < limit) out[count++] = title;
  return count;
}

// Every name in a storage folder, from the save folder and from the read-only storage.
typedef void (*EachName)(const char *name, void *context);
static void each_name(const char *path, EachName run, void *context) {
  char full[MAX_PATH];
  for (int place = 0; place < 2; place++) {
    if (place == 0) files_save_path(path, full, sizeof full);
    else if (!files_storage_path(path, full, sizeof full)) break;
    DIR *folder = opendir(full);
    for (struct dirent *entry = folder ? readdir(folder) : 0; entry; entry = readdir(folder)) {
      if (entry->d_name[0] != '.') run(entry->d_name, context);
    }
    if (folder) closedir(folder);
  }
}

typedef struct {
  uint64_t *out;
  int count, limit, tickets;
  uint32_t high;
} Listing;

static void add_low(const char *name, void *context) {
  Listing *listing = context;
  uint32_t low;
  if (!hex_name(name, &low)) return;
  uint64_t title = (uint64_t)listing->high << 32 | low;
  char path[TITLE_PATH_BYTES];
  if (listing->tickets) {
    if (strcmp(name + 8, ".tik")) return;
  } else {
    if (name[8]) return;
    char found[MAX_PATH];
    snprintf(path, sizeof path, "/title/%08x/%08x/content/title.tmd", listing->high, low);
    if (!files_find(path, found, sizeof found)) return;
  }
  listing->count = add_unique(listing->out, listing->count, listing->limit, title);
}

static void add_high(const char *name, void *context) {
  Listing *listing = context;
  if (name[8] || !hex_name(name, &listing->high)) return;
  char path[TITLE_PATH_BYTES];
  snprintf(path, sizeof path, listing->tickets ? "/ticket/%08x" : "/title/%08x", listing->high);
  each_name(path, add_low, listing);
}

static int list_titles(uint64_t *out, int limit, int tickets) {
  Listing listing = {out, 0, limit, tickets, 0};
  each_name(tickets ? "/ticket" : "/title", add_high, &listing);
  qsort(out, (size_t)listing.count, sizeof out[0], compare_titles);
  return listing.count;
}

int titles_installed(uint64_t *out, int limit) { return list_titles(out, limit, 0); }
int titles_owned(uint64_t *out, int limit) { return list_titles(out, limit, 1); }

long title_tmd(uint64_t title, uint8_t *out, size_t limit) {
  char path[TITLE_PATH_BYTES];
  snprintf(path, sizeof path, "/title/%08x/%08x/content/title.tmd", (uint32_t)(title >> 32), (uint32_t)title);
  long size = read_storage(path, out, limit);
  return size >= 0 && tmd_check(out, (size_t)size) >= 0 ? size : -1;
}

long title_tickets(uint64_t title, uint8_t *out, size_t limit) {
  char path[TITLE_PATH_BYTES];
  snprintf(path, sizeof path, "/ticket/%08x/%08x.tik", (uint32_t)(title >> 32), (uint32_t)title);
  long size = read_storage(path, out, limit);
  return size >= TICKET_BYTES && size % TICKET_BYTES == 0 ? size : -1;
}

int tmd_check(const uint8_t *tmd, size_t bytes) {
  if (bytes < TMD_HEADER_BYTES) return -1;
  int count = big16(tmd + TMD_CONTENT_COUNT);
  return count <= TMD_MAX_CONTENTS && bytes >= (size_t)TMD_HEADER_BYTES + (size_t)count * TMD_CONTENT_BYTES ? count : -1;
}

uint64_t tmd_title(const uint8_t *tmd) { return big64(tmd + TMD_TITLE); }
uint64_t tmd_ios(const uint8_t *tmd) { return big64(tmd + TMD_IOS); }
uint16_t tmd_title_version(const uint8_t *tmd) { return big16(tmd + TMD_TITLE_VERSION); }

TmdContent tmd_content(const uint8_t *tmd, int number) {
  const uint8_t *record = tmd + TMD_HEADER_BYTES + number * TMD_CONTENT_BYTES;
  TmdContent content = {big32(record), big16(record + 4), big16(record + 6), big64(record + 8), record + 16};
  return content;
}

size_t tmd_view_bytes(const uint8_t *tmd, size_t bytes) {
  int count = tmd_check(tmd, bytes);
  return count < 0 ? 0 : TMD_VIEW_HEADER_BYTES + (size_t)count * TMD_VIEW_CONTENT_BYTES;
}

// The header from the TMD's version to its access rights, then its title version and
// content count, then each content without its hash, as Dolphin's GetRawView and
// libogc's tmd_view lay it out: the contents start at 0x5c.
size_t tmd_view(const uint8_t *tmd, size_t bytes, uint8_t *out, size_t limit) {
  size_t size = tmd_view_bytes(tmd, bytes);
  if (!size || size > limit) return 0;
  size_t at = TMD_ACCESS_RIGHTS - TMD_VIEW_FROM;
  memcpy(out, tmd + TMD_VIEW_FROM, at);
  memcpy(out + at, tmd + TMD_TITLE_VERSION, 4);  // Title version and content count.
  int count = tmd_check(tmd, bytes);
  for (int i = 0; i < count; i++) {
    const uint8_t *record = tmd + TMD_HEADER_BYTES + i * TMD_CONTENT_BYTES;
    memcpy(out + TMD_VIEW_HEADER_BYTES + i * TMD_VIEW_CONTENT_BYTES, record, TMD_VIEW_CONTENT_BYTES);
  }
  return size;
}

// The ticket's format version as a word, then everything from its ticket ID on.
void ticket_view(const uint8_t *ticket, uint8_t *out) {
  memset(out, 0, 4);
  out[3] = ticket[TICKET_VERSION];
  memcpy(out + 4, ticket + TICKET_VIEW_FROM, TICKET_VIEW_BYTES - 4);
}

static int shared_content_path(const uint8_t *hash, char *out, size_t limit) {
  static uint8_t *map;
  static long map_size = -1;
  if (map_size < 0) {
    map = malloc(MAX_MAP_BYTES);
    map_size = read_storage("/shared1/content.map", map, MAX_MAP_BYTES);
  }
  for (long at = 0; at + MAP_ENTRY_BYTES <= map_size; at += MAP_ENTRY_BYTES) {
    if (memcmp(map + at + MAP_NAME_BYTES, hash, HASH_BYTES)) continue;
    snprintf(out, limit, "/shared1/%.8s.app", (const char *)map + at);
    return 1;
  }
  return 0;
}

int title_content_path(uint64_t title, const TmdContent *content, char *out, size_t limit) {
  char found[MAX_PATH];
  if (content->type & SHARED) {
    if (!shared_content_path(content->hash, out, limit)) return 0;
  } else {
    snprintf(out, limit, "/title/%08x/%08x/content/%08x.app", (uint32_t)(title >> 32), (uint32_t)title, content->id);
  }
  return files_find(out, found, sizeof found);
}
