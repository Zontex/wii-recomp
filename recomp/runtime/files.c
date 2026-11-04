// Files in the Wii's internal storage. Reads come from the save folder, then from a
// Wii storage folder such as Dolphin's. Writes only ever go to the save folder.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bluetooth.h"
#include "wii_camera.h"
#include "title.h"
#include "cpu.h"
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

typedef struct {
  uint8_t *data;
  long size, capacity, position;
  int changed, mode;
  char path[128];
} Handle;

static Handle handles[MAX_HANDLES];
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

int files_open(const char *path, uint32_t mode) {
  char full[MAX_PATH];
  struct stat info;
  if (!files_find(path, full, sizeof full)) return FS_ENOENT;
  if (!stat(full, &info) && S_ISDIR(info.st_mode)) return FS_INVALID;
  FILE *file = fopen(full, "rb");
  if (!file) return FS_ACCESS_DENIED;
  for (int i = 0; i < MAX_HANDLES; i++) {
    if (handles[i].data) continue;
    fseek(file, 0, SEEK_END);
    Handle *handle = &handles[i];
    handle->size = ftell(file);
    rewind(file);
    handle->capacity = handle->size + 1;
    handle->data = malloc((size_t)handle->capacity);
    handle->position = 0;
    handle->changed = 0;
    handle->mode = (int)(mode & (MODE_READ | MODE_WRITE));
    snprintf(handle->path, sizeof handle->path, "%s", path);
    fread(handle->data, 1, (size_t)handle->size, file);
    if (title->virtual_bluetooth && !strcmp(path, SETTINGS_FILE)) {
      bluetooth_register_remote(handle->data, (size_t)handle->size);
      camera_set_bar_above(settings_byte(handle->data, (size_t)handle->size, "BT.BAR") == 1);
    }
    fclose(file);
    return i;
  }
  fclose(file);
  return FS_NO_FREE_HANDLE;
}

const char *files_name(int id) { return handles[id].path; }

// Writes a changed file out to the save folder, keeping it open.
static void save(Handle *handle) {
  if (handle->changed) {
    char full[MAX_PATH];
    files_save_path(handle->path, full, sizeof full);
    files_make_parents(full);
    FILE *file = fopen(full, "wb");
    if (file) {
      fwrite(handle->data, 1, (size_t)handle->size, file);
      fclose(file);
    } else {
      fprintf(stderr, "files: can't save %s: %s\n", full, strerror(errno));
    }
    handle->changed = 0;
  }
}

// Everything written so far reaches the save folder, as before the power goes off.
void files_save_all(void) {
  for (int i = 0; i < MAX_HANDLES; i++) {
    if (handles[i].data) save(&handles[i]);
  }
}

void files_close(int id) {
  Handle *handle = &handles[id];
  save(handle);
  free(handle->data);
  handle->data = 0;
}

static int write_bytes(Handle *handle, const uint8_t *from, long bytes) {
  long end = handle->position + bytes;
  if (end > handle->capacity) {
    handle->capacity = end * 2;
    handle->data = realloc(handle->data, (size_t)handle->capacity);
  }
  memcpy(handle->data + handle->position, from, (size_t)bytes);
  handle->position = end;
  if (end > handle->size) handle->size = end;
  handle->changed = 1;
  return (int)bytes;
}

int files_request(int id, uint32_t command, uint32_t request) {
  Handle *handle = &handles[id];
  if (command == IOS_READ) {
    if (!(handle->mode & MODE_READ)) return FS_ACCESS_DENIED;
    long left = handle->size - handle->position, bytes = rd32(request + 16);
    if (bytes > left) bytes = left;
    memcpy(host(rd32(request + 12) | 0x80000000u), handle->data + handle->position, (size_t)bytes);
    handle->position += bytes;
    return (int)bytes;
  }
  if (command == IOS_WRITE) {
    if (!(handle->mode & MODE_WRITE)) return FS_ACCESS_DENIED;
    return write_bytes(handle, host(rd32(request + 12) | 0x80000000u), rd32(request + 16));
  }
  if (command == IOS_SEEK) {
    long offset = (int32_t)rd32(request + 12);
    uint32_t from = rd32(request + 16);
    if (from == SEEK_FROM_CURRENT) offset += handle->position;
    else if (from == SEEK_FROM_END) offset += handle->size;
    handle->position = offset;
    return (int)offset;
  }
  if (command == IOS_IOCTL && rd32(request + 12) == GET_FILE_STATS) {
    uint32_t out = rd32(request + 24) | 0x80000000u;
    wr32(out, (uint32_t)handle->size);
    wr32(out + 4, (uint32_t)handle->position);
  }
  return 0;
}
