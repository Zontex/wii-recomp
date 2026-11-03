// /dev/fs: creating, deleting, renaming and listing files in internal storage.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cpu.h"
#include "ios.h"

enum {
  GET_STATS = 2, CREATE_DIR = 3, READ_DIR = 4, SET_ATTR = 5, GET_ATTR = 6, DELETE = 7, RENAME = 8, CREATE_FILE = 9,
  GET_USAGE = 12, PATH_BYTES = 64, ATTR_PATH = 6, ATTR_BYTES = 0x4c, MAX_PATH = 1024, VECTOR_BYTES = 8,
  INVALID = -101, NOT_FOUND = -106, ALREADY_EXISTS = -105, BLOCK_BYTES = 0x4000, FREE_BLOCKS = 0x4000, FREE_INODES = 0x4000,
  NAME_BYTES = 13, READ_WRITE = 3,
};

static uint32_t guest(uint32_t physical) { return physical | 0x80000000u; }

static void read_path(uint32_t address, char *out) {
  memcpy(out, host(address), PATH_BYTES);
  out[PATH_BYTES - 1] = 0;
}

static int create(const char *path, int folder) {
  char full[MAX_PATH], found[MAX_PATH];
  if (files_find(path, found, sizeof found)) return ALREADY_EXISTS;
  files_save_path(path, full, sizeof full);
  files_make_parents(full);
  if (folder) return mkdir(full, 0700) ? NOT_FOUND : 0;
  FILE *file = fopen(full, "wb");
  if (!file) return NOT_FOUND;
  fclose(file);
  return 0;
}

static void remove_tree(const char *full) {
  struct stat info;
  if (stat(full, &info)) return;
  if (!S_ISDIR(info.st_mode)) {
    unlink(full);
    return;
  }
  DIR *folder = opendir(full);
  for (struct dirent *entry = folder ? readdir(folder) : 0; entry; entry = readdir(folder)) {
    if (entry->d_name[0] == '.') continue;
    char child[MAX_PATH + 256];
    snprintf(child, sizeof child, "%s/%s", full, entry->d_name);
    remove_tree(child);
  }
  if (folder) closedir(folder);
  rmdir(full);
}

static int erase(const char *path) {
  char full[MAX_PATH], found[MAX_PATH];
  if (!files_find(path, found, sizeof found)) return NOT_FOUND;
  files_save_path(path, full, sizeof full);
  remove_tree(full);  // Anything only in the Wii storage folder stays, untouched.
  return 0;
}

static int copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : 0;
  char block[BLOCK_BYTES];
  size_t bytes;
  while (out && (bytes = fread(block, 1, sizeof block, in)) > 0) fwrite(block, 1, bytes, out);
  if (in) fclose(in);
  if (out) fclose(out);
  return in && out ? 0 : NOT_FOUND;
}

static int move(const char *from, const char *to) {
  char source[MAX_PATH], target[MAX_PATH], saved[MAX_PATH];
  if (!files_find(from, source, sizeof source)) return NOT_FOUND;
  files_save_path(to, target, sizeof target);
  files_save_path(from, saved, sizeof saved);
  files_make_parents(target);
  remove_tree(target);
  if (!strcmp(source, saved)) return rename(source, target) ? NOT_FOUND : 0;
  return copy_file(source, target);
}

static int get_attributes(const char *path, uint32_t out, uint32_t bytes) {
  char found[MAX_PATH];
  if (!files_find(path, found, sizeof found)) return NOT_FOUND;
  if (bytes >= ATTR_BYTES) {
    memset(host(out), 0, ATTR_BYTES);
    snprintf((char *)host(out + ATTR_PATH), PATH_BYTES, "%s", path);
    wr8(out + ATTR_PATH + PATH_BYTES, READ_WRITE);
    wr8(out + ATTR_PATH + PATH_BYTES + 1, READ_WRITE);
    wr8(out + ATTR_PATH + PATH_BYTES + 2, READ_WRITE);
  }
  return 0;
}

// Adds a folder's names to `names`, skipping any already listed. Returns the new count.
static uint32_t list_folder(const char *full, char (*names)[NAME_BYTES], uint32_t count, uint32_t limit) {
  DIR *folder = opendir(full);
  for (struct dirent *entry = folder ? readdir(folder) : 0; entry && count < limit; entry = readdir(folder)) {
    if (entry->d_name[0] == '.') continue;
    int seen = 0;
    for (uint32_t i = 0; i < count; i++) seen |= !strncmp(names[i], entry->d_name, NAME_BYTES - 1);
    if (seen) continue;
    snprintf(names[count++], NAME_BYTES, "%s", entry->d_name);
  }
  if (folder) closedir(folder);
  return count;
}

static int read_folder(uint32_t request) {
  uint32_t inputs = rd32(request + 16), vectors = guest(rd32(request + 24));
  char path[PATH_BYTES], saved[MAX_PATH], found[MAX_PATH];
  read_path(guest(rd32(vectors)), path);
  if (!files_find(path, found, sizeof found)) return NOT_FOUND;
  struct stat info;
  // A file can't be listed. NANDGetType relies on this answer to tell files from folders.
  if (!stat(found, &info) && !S_ISDIR(info.st_mode)) return INVALID;
  // Asked once for the count, then again with room for the names.
  int with_names = inputs == 2;
  uint32_t limit = with_names ? rd32(guest(rd32(vectors + VECTOR_BYTES))) : 4096;
  char (*names)[NAME_BYTES] = calloc(limit ? limit : 1, NAME_BYTES);
  files_save_path(path, saved, sizeof saved);
  uint32_t count = list_folder(saved, names, 0, limit);
  if (files_storage_path(path, found, sizeof found)) count = list_folder(found, names, count, limit);
  uint32_t count_vector = vectors + (with_names ? 3 : 1) * VECTOR_BYTES;
  wr32(guest(rd32(count_vector)), count);
  if (with_names) {
    uint32_t to = guest(rd32(vectors + 2 * VECTOR_BYTES));
    for (uint32_t i = 0; i < count; i++) {
      size_t bytes = strlen(names[i]) + 1;
      memcpy(host(to), names[i], bytes);
      to += (uint32_t)bytes;
    }
  }
  free(names);
  return 0;
}

static void write_stats(uint32_t out) {
  static const uint32_t stats[7] = {BLOCK_BYTES, FREE_BLOCKS, 0, 0, 0, FREE_INODES, 0};
  for (uint32_t i = 0; i < 7; i++) wr32(out + i * 4, stats[i]);
}

int fs_request(uint32_t command, uint32_t request) {
  uint32_t code = rd32(request + 12);
  if (command == IOS_IOCTLV) {
    if (code == READ_DIR) return read_folder(request);
    if (code == GET_USAGE) {
      uint32_t vectors = guest(rd32(request + 24));
      wr32(guest(rd32(vectors + VECTOR_BYTES)), 0);
      wr32(guest(rd32(vectors + 2 * VECTOR_BYTES)), 0);
      return 0;
    }
    return IOS_UNHANDLED;
  }
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  uint32_t in = guest(rd32(request + 16)), out = guest(rd32(request + 24));
  char path[PATH_BYTES], other[PATH_BYTES];
  switch (code) {
    case GET_STATS: write_stats(out); return 0;
    case CREATE_DIR: read_path(in + ATTR_PATH, path); return create(path, 1);
    case CREATE_FILE: read_path(in + ATTR_PATH, path); return create(path, 0);
    case GET_ATTR: read_path(in, path); return get_attributes(path, out, rd32(request + 28));
    case DELETE: read_path(in, path); return erase(path);
    case RENAME: read_path(in, path); read_path(in + PATH_BYTES, other); return move(path, other);
    case SET_ATTR: return 0;  // Owners and permissions aren't kept yet: docs/native_ios.md, stage 2.
    default: return IOS_UNHANDLED;
  }
}
