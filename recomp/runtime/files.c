// Files in the Wii's internal storage. Reads come from the save folder, then from a
// Wii storage folder such as Dolphin's. Writes only ever go to the save folder.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bluetooth.h"
#include "wii_camera.h"
#include "title.h"
#include "cpu.h"
#include "gaps.h"
#include "ios.h"

#define SETTINGS_FILE "/shared2/sys/SYSCONF"

enum {
  MAX_HANDLES = 32, MAX_PATH = 1024, SEEK_FROM_START = 0, SEEK_FROM_CURRENT = 1, SEEK_FROM_END = 2,
  GET_FILE_STATS = 11,
};

// What an open may do, the low two bits of its mode, and the answers IOS gives (Dolphin's
// FileSystem.h lists both): a folder can't be opened, and a read on a handle opened only for
// writing, or the other way round, is refused.
enum { MODE_READ = 1, MODE_WRITE = 2, FS_INVALID = -101, FS_ACCESS_DENIED = -102, FS_NO_FREE_HANDLE = -109 };

// A file's contents while any handle has it open. Every handle on the same file shares
// one, so a write through one is read through another at once, as on a Wii.
typedef struct {
  uint8_t *data;
  long size, capacity;
  int changed, users;
  char path[128];
} Contents;

typedef struct {
  Contents *contents;  // 0 when the handle is free.
  long position;
  int mode;
} Handle;

static Handle handles[MAX_HANDLES];
static Contents contents[MAX_HANDLES];
static const char *storage, *saves;

void files_start(const char *folder) {
  storage = folder;
  saves = getenv("WII_RECOMP_SAVES") ? getenv("WII_RECOMP_SAVES") : "recomp/build/saves";
}

void files_save_path(const char *path, char *out, unsigned long size) { snprintf(out, size, "%s%s", saves, path); }

int files_storage_path(const char *path, char *out, unsigned long size) {
  if (!storage) return 0;
  snprintf(out, size, "%s%s", storage, path);
  return 1;
}

// Makes every folder above a file in the save folder.
void files_make_parents(const char *full) {
  char partial[MAX_PATH];
  snprintf(partial, sizeof partial, "%s", full);
  for (char *at = partial + 1; *at; at++) {
    if (*at != '/') continue;
    *at = 0;
    mkdir(partial, 0700);
    *at = '/';
  }
}

int files_find(const char *path, char *out, unsigned long size) {
  struct stat info;
  files_save_path(path, out, size);
  if (!stat(out, &info)) return 1;
  if (!storage) return 0;
  snprintf(out, size, "%s%s", storage, path);
  return !stat(out, &info);
}

// Writes a whole file so that a power cut leaves the old one or the new one, never part:
// a temporary file beside it, flushed to the disk, then renamed over it.
int files_write_whole(const char *full, const void *data, size_t bytes) {
  // Named with a dot first, so a listing never shows one a power cut left behind.
  char temporary[MAX_PATH + 8];
  const char *slash = strrchr(full, '/');
  int folder = slash ? (int)(slash - full) + 1 : 0;
  snprintf(temporary, sizeof temporary, "%.*s.%s.part", folder, full, full + folder);
  FILE *file = fopen(temporary, "wb");
  if (!file) return 0;
  int written = fwrite(data, 1, bytes, file) == bytes && !fflush(file) && !fsync(fileno(file));
  if (fclose(file) || !written || rename(temporary, full)) {
    unlink(temporary);
    return 0;
  }
  return 1;
}

// A one-byte value from the console settings, or -1. Each item is a type and name
// length byte, the name, then the value.
static int settings_byte(const uint8_t *data, size_t bytes, const char *name) {
  if (bytes < 6) return -1;
  unsigned count = (unsigned)data[4] << 8 | data[5];
  size_t length = strlen(name);
  for (unsigned i = 0; i < count && 8 + 2 * i <= bytes; i++) {
    size_t at = (size_t)data[6 + 2 * i] << 8 | data[7 + 2 * i];
    if (at + 1 + length + 1 > bytes || (size_t)(data[at] & 0x1f) + 1 != length) continue;
    if (!memcmp(data + at + 1, name, length)) return data[at + 1 + length];
  }
  return -1;
}

// The contents of a file another handle has open, or 0.
static Contents *open_contents(const char *path) {
  for (int i = 0; i < MAX_HANDLES; i++) {
    if (contents[i].users && !strcmp(contents[i].path, path)) return &contents[i];
  }
  return 0;
}

// The value of a fixed-size item in the console settings, as the open file has it, or as
// the storage does. Returns its size, or 0 when it isn't there. Items are a type and name
// length byte, the name, then the value: a byte (type 3), two (4), four (5) or eight (6).
int files_system_setting(const char *name, uint8_t *out, size_t limit) {
  static const size_t sizes[8] = {0, 0, 0, 1, 2, 4, 8, 1};
  char full[MAX_PATH];
  uint8_t data[0x4000];
  size_t bytes = 0, length = strlen(name);
  Contents *open_file = open_contents(SETTINGS_FILE);
  if (open_file) {
    bytes = (size_t)open_file->size < sizeof data ? (size_t)open_file->size : sizeof data;
    memcpy(data, open_file->data, bytes);
  } else if (files_find(SETTINGS_FILE, full, sizeof full)) {
    FILE *file = fopen(full, "rb");
    if (file) {
      bytes = fread(data, 1, sizeof data, file);
      fclose(file);
    }
  }
  if (bytes < 6) return 0;
  unsigned count = (unsigned)data[4] << 8 | data[5];
  for (unsigned i = 0; i < count && 8 + 2 * i <= bytes; i++) {
    size_t at = (size_t)data[6 + 2 * i] << 8 | data[7 + 2 * i];
    if (at >= bytes || (size_t)(data[at] & 0x1f) + 1 != length) continue;
    size_t size = sizes[data[at] >> 5];
    if (!size || size > limit || at + 1 + length + size > bytes || memcmp(data + at + 1, name, length)) continue;
    memcpy(out, data + at + 1 + length, size);
    return (int)size;
  }
  return 0;
}

// Reads a file in, into a free slot. 0 when it can't be read.
static Contents *load_contents(const char *path, const char *full) {
  Contents *loaded = 0;
  for (int i = 0; i < MAX_HANDLES && !loaded; i++) if (!contents[i].users) loaded = &contents[i];
  FILE *file = loaded ? fopen(full, "rb") : 0;
  if (!file) return 0;
  fseek(file, 0, SEEK_END);
  loaded->size = ftell(file);
  rewind(file);
  loaded->capacity = loaded->size + 1;
  loaded->data = malloc((size_t)loaded->capacity);
  loaded->changed = 0;
  snprintf(loaded->path, sizeof loaded->path, "%s", path);
  if (fread(loaded->data, 1, (size_t)loaded->size, file) != (size_t)loaded->size) {
    fclose(file);
    free(loaded->data);
    return 0;
  }
  fclose(file);
  if (title->virtual_bluetooth && !strcmp(path, SETTINGS_FILE)) {
    bluetooth_register_remote(loaded->data, (size_t)loaded->size);
    camera_set_bar_above(settings_byte(loaded->data, (size_t)loaded->size, "BT.BAR") == 1);
  }
  return loaded;
}

int files_open(const char *path, uint32_t mode) {
  char full[MAX_PATH];
  struct stat info;
  if (!files_find(path, full, sizeof full)) return FS_ENOENT;
  if (!stat(full, &info) && S_ISDIR(info.st_mode)) return FS_INVALID;
  int free_handle = -1;
  for (int i = 0; i < MAX_HANDLES && free_handle < 0; i++) if (!handles[i].contents) free_handle = i;
  if (free_handle < 0) return FS_NO_FREE_HANDLE;
  Contents *shared = open_contents(path);
  if (!shared) shared = load_contents(path, full);
  if (!shared) return FS_ACCESS_DENIED;
  shared->users++;
  handles[free_handle] = (Handle){shared, 0, (int)(mode & (MODE_READ | MODE_WRITE))};
  return free_handle;
}

const char *files_name(int id) { return handles[id].contents->path; }

// Writes changed contents out to the save folder, keeping them open.
static void save(Contents *file) {
  if (!file->changed) return;
  char full[MAX_PATH];
  files_save_path(file->path, full, sizeof full);
  files_make_parents(full);
  if (!files_write_whole(full, file->data, (size_t)file->size)) fprintf(stderr, "files: can't save %s: %s\n", full, strerror(errno));
  file->changed = 0;
}

// Everything written so far reaches the save folder, as before the power goes off.
void files_save_all(void) {
  for (int i = 0; i < MAX_HANDLES; i++) {
    if (contents[i].users) save(&contents[i]);
  }
}

void files_close(int id) {
  Contents *file = handles[id].contents;
  handles[id].contents = 0;
  save(file);
  if (--file->users) return;
  free(file->data);
  file->data = 0;
}

static int write_bytes(Handle *handle, const uint8_t *from, long bytes) {
  Contents *file = handle->contents;
  long end = handle->position + bytes;
  if (end > file->capacity) {
    file->capacity = end * 2;
    file->data = realloc(file->data, (size_t)file->capacity);
  }
  memcpy(file->data + handle->position, from, (size_t)bytes);
  handle->position = end;
  if (end > file->size) file->size = end;
  file->changed = 1;
  return (int)bytes;
}

int files_request(int id, uint32_t command, uint32_t request) {
  Handle *handle = &handles[id];
  Contents *file = handle->contents;
  if (command == IOS_READ) {
    if (!(handle->mode & MODE_READ)) return FS_ACCESS_DENIED;
    long left = file->size - handle->position, bytes = rd32(request + 16);
    if (bytes > left) bytes = left > 0 ? left : 0;
    memcpy(host(rd32(request + 12) | 0x80000000u), file->data + handle->position, (size_t)bytes);
    handle->position += bytes;
    return (int)bytes;
  }
  if (command == IOS_WRITE) {
    if (!(handle->mode & MODE_WRITE)) return FS_ACCESS_DENIED;
    return write_bytes(handle, host(rd32(request + 12) | 0x80000000u), rd32(request + 16));
  }
  if (command == IOS_SEEK) {
    // A position outside the file is refused, as IOS refuses it (Dolphin's FS.cpp).
    long offset = (int32_t)rd32(request + 12);
    uint32_t from = rd32(request + 16);
    if (from == SEEK_FROM_CURRENT) offset += handle->position;
    else if (from == SEEK_FROM_END) offset += file->size;
    else if (from != SEEK_FROM_START) return FS_INVALID;
    if (offset < 0 || offset > file->size) return FS_INVALID;
    handle->position = offset;
    return (int)offset;
  }
  if (command == IOS_IOCTL && rd32(request + 12) == GET_FILE_STATS) {
    uint32_t out = rd32(request + 24) | 0x80000000u;
    if (rd32(request + 28) < 8) return FS_INVALID;
    wr32(out, (uint32_t)file->size);
    wr32(out + 4, (uint32_t)handle->position);
    return 0;
  }
  // A file handle takes nothing else (Dolphin answers the same).
  gap_hit("ios.file_handle_other_requests", command == IOS_IOCTL ? rd32(request + 12) : command);
  return FS_INVALID;
}
