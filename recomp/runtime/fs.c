// /dev/fs: creating, deleting, renaming and listing files in internal storage.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cpu.h"
#include "gaps.h"
#include "ios.h"

enum {
  GET_STATS = 2, CREATE_DIR = 3, READ_DIR = 4, SET_ATTR = 5, GET_ATTR = 6, DELETE = 7, RENAME = 8, CREATE_FILE = 9,
  GET_USAGE = 12, SHUTDOWN = 13, PATH_BYTES = 64, ATTR_PATH = 6, ATTR_BYTES = 0x4c, MAX_PATH = 1024, VECTOR_BYTES = 8,
  INVALID = -101, NOT_FOUND = -106, ALREADY_EXISTS = -105,
  // The storage's size, as Dolphin's FileSystem.h gives it: 16 KiB clusters, some kept for IOS.
  CLUSTER_BYTES = 0x4000, TOTAL_CLUSTERS = 0x7ec0, RESERVED_CLUSTERS = 0x300, USABLE_CLUSTERS = TOTAL_CLUSTERS - RESERVED_CLUSTERS,
  TOTAL_INODES = 0x17ff,
  NAME_BYTES = 13, READ_WRITE = 3, FILE_NOT_EMPTY = -115,
  // Where owner, group, permissions and attribute sit in a request's parameters (Dolphin's ISFSParams).
  ATTR_OWNER = 0, ATTR_GROUP = 4, ATTR_MODES = ATTR_PATH + PATH_BYTES, ATTR_FLAGS = ATTR_MODES + 3,
};

#define ATTRIBUTES_FILE "/.attributes"  // In the save folder. Listings skip names that start with a dot.

// Who owns a file or folder and what everyone may do with it. Anything not in the
// attributes file is owned by 0 and open to all, as files the Wii storage folder brought.
typedef struct {
  uint32_t owner;
  uint16_t group;
  uint8_t modes[3], flags;
} Attributes;

static const Attributes default_attributes = {0, 0, {READ_WRITE, READ_WRITE, READ_WRITE}, 0};

static uint32_t guest(uint32_t physical) { return physical | 0x80000000u; }

static void read_path(uint32_t address, char *out) {
  memcpy(out, host(address), PATH_BYTES);
  out[PATH_BYTES - 1] = 0;
}

// The attributes file has a line for each path given attributes: owner, group, flags, the
// three permissions, then the path. Lines for paths under `prefix` are dropped, or moved to
// `moved_to` when that's given. With `set`, the line for `path` becomes `*set`.
static void rewrite_attributes(const char *prefix, const char *moved_to, const char *path, const Attributes *set) {
  char full[MAX_PATH], temporary[MAX_PATH + 8], line[PATH_BYTES + 64], name[PATH_BYTES + PATH_BYTES];
  files_save_path(ATTRIBUTES_FILE, full, sizeof full);
  snprintf(temporary, sizeof temporary, "%s.new", full);
  FILE *in = fopen(full, "r");
  if (!in && !set) return;  // Nothing kept, and nothing to keep.
  files_make_parents(full);
  FILE *out = fopen(temporary, "w");
  if (!out) { if (in) fclose(in); return; }
  size_t length = prefix ? strlen(prefix) : 0;
  while (in && fgets(line, sizeof line, in)) {
    unsigned owner, group, flags, modes[3];
    if (sscanf(line, "%x %x %x %u %u %u %63[^\n]", &owner, &group, &flags, &modes[0], &modes[1], &modes[2], name) != 7) continue;
    if (set && !strcmp(name, path)) continue;
    if (prefix && !strncmp(name, prefix, length) && (!name[length] || name[length] == '/')) {
      if (!moved_to) continue;
      char renamed[PATH_BYTES + PATH_BYTES];
      snprintf(renamed, sizeof renamed, "%s%s", moved_to, name + length);
      snprintf(name, sizeof name, "%s", renamed);
    }
    fprintf(out, "%08x %04x %02x %u %u %u %s\n", owner, group, flags, modes[0], modes[1], modes[2], name);
  }
  if (set) fprintf(out, "%08x %04x %02x %u %u %u %s\n", set->owner, set->group, set->flags, set->modes[0], set->modes[1], set->modes[2], path);
  if (in) fclose(in);
  fclose(out);
  rename(temporary, full);
}

static Attributes attributes_of(const char *path) {
  char full[MAX_PATH], line[PATH_BYTES + 64], name[PATH_BYTES];
  Attributes found = default_attributes;
  files_save_path(ATTRIBUTES_FILE, full, sizeof full);
  FILE *in = fopen(full, "r");
  while (in && fgets(line, sizeof line, in)) {
    unsigned owner, group, flags, modes[3];
    if (sscanf(line, "%x %x %x %u %u %u %63[^\n]", &owner, &group, &flags, &modes[0], &modes[1], &modes[2], name) != 7) continue;
    if (strcmp(name, path)) continue;
    found = (Attributes){owner, (uint16_t)group, {(uint8_t)modes[0], (uint8_t)modes[1], (uint8_t)modes[2]}, (uint8_t)flags};
  }
  if (in) fclose(in);
  return found;
}

static Attributes read_attributes(uint32_t in) {
  return (Attributes){rd32(in + ATTR_OWNER), rd16(in + ATTR_GROUP), {rd8(in + ATTR_MODES), rd8(in + ATTR_MODES + 1), rd8(in + ATTR_MODES + 2)}, rd8(in + ATTR_FLAGS)};
}

static int kind_of(const char *path);
static int parent_there(const char *path);
static int valid_path(const char *path);

// CREATEFILE and CREATEDIR: the name must be valid and its folder there, as on a Wii. The
// folder may be only in the Wii storage folder, so the save folder gets its copy of it.
static int create(const char *path, int folder) {
  char full[MAX_PATH], found[MAX_PATH];
  if (!valid_path(path)) return INVALID;
  if (files_find(path, found, sizeof found)) return ALREADY_EXISTS;
  if (!parent_there(path)) return NOT_FOUND;
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
  rewrite_attributes(path, 0, 0, 0);
  return 0;
}

// A file from the Wii storage folder copied into the save folder, written whole.
static int copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in) return NOT_FOUND;
  fseek(in, 0, SEEK_END);
  long size = ftell(in);
  rewind(in);
  uint8_t *data = malloc(size > 0 ? (size_t)size : 1);
  int ok = data && fread(data, 1, (size_t)size, in) == (size_t)size && files_write_whole(to, data, (size_t)size);
  fclose(in);
  free(data);
  return ok ? 0 : NOT_FOUND;
}

// The type of what's at a Wii path, through both folders: 1 a file, 2 a folder, 0 nothing.
static int kind_of(const char *path) {
  char found[MAX_PATH];
  struct stat info;
  if (!files_find(path, found, sizeof found) || stat(found, &info)) return 0;
  return S_ISDIR(info.st_mode) ? 2 : 1;
}

// Whether the folder a path would go in is there, as IOS needs it to be.
static int parent_there(const char *path) {
  char parent[PATH_BYTES];
  snprintf(parent, sizeof parent, "%s", path);
  char *last = strrchr(parent, '/');
  if (!last) return 0;
  if (last == parent) return 1;  // The top folder.
  *last = 0;
  return kind_of(parent) == 2;
}

// A name IOS takes: a path from the top, each part at most 12 characters.
static int valid_path(const char *path) {
  if (path[0] != '/') return 0;
  const char *last = strrchr(path, '/') + 1;
  return *last && strlen(last) < NAME_BYTES;
}

// RENAME, as Dolphin's FS answers it: both names must be valid, the new one's folder there,
// and anything already there of the same type, which it replaces.
static int move(const char *from, const char *to) {
  char source[MAX_PATH], target[MAX_PATH], saved[MAX_PATH];
  if (!valid_path(from) || !valid_path(to)) return INVALID;
  if (!files_find(from, source, sizeof source)) return NOT_FOUND;
  if (!parent_there(to)) return NOT_FOUND;
  int kind = kind_of(from), there = kind_of(to);
  if (there && there != kind) return INVALID;
  if (kind == 2 && strcmp(source, (files_save_path(from, saved, sizeof saved), saved))) {
    gap_hit("ios.rename_folder_from_storage", 0);  // A folder only in the Wii storage folder: not copied.
    return INVALID;
  }
  files_save_path(to, target, sizeof target);
  files_save_path(from, saved, sizeof saved);
  files_make_parents(target);
  remove_tree(target);
  int result = !strcmp(source, saved) ? (rename(source, target) ? NOT_FOUND : 0) : copy_file(source, target);
  if (!result) {
    rewrite_attributes(to, 0, 0, 0);
    rewrite_attributes(from, to, 0, 0);
  }
  return result;
}

// SETATTR, as Dolphin's FS.cpp answers it. Changing the owner of a file with something in
// it is refused, so a title can't hand its data to another. There are no callers'
// identities here, so every caller may change anything, as the system menu may.
static int set_attributes(uint32_t in) {
  char path[PATH_BYTES], found[MAX_PATH];
  read_path(in + ATTR_PATH, path);
  if (path[0] != '/') return INVALID;
  if (!files_find(path, found, sizeof found)) return NOT_FOUND;
  Attributes now = attributes_of(path), wanted = read_attributes(in);
  struct stat info;
  if (wanted.owner != now.owner && !stat(found, &info) && S_ISREG(info.st_mode) && info.st_size) return FILE_NOT_EMPTY;
  rewrite_attributes(0, 0, path, &wanted);
  return 0;
}

// A new file or folder keeps the permissions and flags asked for. Its owner would be the
// caller, whom we can't tell apart, so it's 0.
static int create_with_attributes(uint32_t in, int folder) {
  char path[PATH_BYTES];
  read_path(in + ATTR_PATH, path);
  int result = create(path, folder);
  Attributes wanted = read_attributes(in);
  wanted.owner = 0;
  wanted.group = 0;
  int usual = wanted.flags == default_attributes.flags && !memcmp(wanted.modes, default_attributes.modes, sizeof wanted.modes);
  if (!result && !usual) rewrite_attributes(0, 0, path, &wanted);
  return result;
}

static int get_attributes(const char *path, uint32_t out, uint32_t bytes) {
  char found[MAX_PATH];
  if (!files_find(path, found, sizeof found)) return NOT_FOUND;
  if (bytes >= ATTR_BYTES) {
    Attributes attributes = attributes_of(path);
    memset(host(out), 0, ATTR_BYTES);
    snprintf((char *)host(out + ATTR_PATH), PATH_BYTES, "%s", path);
    wr32(out + ATTR_OWNER, attributes.owner);
    wr16(out + ATTR_GROUP, attributes.group);
    for (int i = 0; i < 3; i++) wr8(out + ATTR_MODES + i, attributes.modes[i]);
    wr8(out + ATTR_FLAGS, attributes.flags);
  }
  return 0;
}

// Dolphin keeps its own record of owners and permissions in the storage's top folder, as
// fst.bin. It isn't one of the Wii's files, so it's left out there.
static int host_only(const char *wii_folder, const char *name) {
  return name[0] == '.' || (!strcmp(wii_folder, "/") && !strcmp(name, "fst.bin"));
}

// Adds a folder's names to `names`, skipping any already listed. Returns the new count.
static uint32_t list_folder(const char *wii_folder, const char *full, char (*names)[NAME_BYTES], uint32_t count, uint32_t limit) {
  DIR *folder = opendir(full);
  for (struct dirent *entry = folder ? readdir(folder) : 0; entry && count < limit; entry = readdir(folder)) {
    if (host_only(wii_folder, entry->d_name)) continue;
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
  uint32_t count = list_folder(path, saved, names, 0, limit);
  if (files_storage_path(path, found, sizeof found)) count = list_folder(path, found, names, count, limit);
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

// What a folder and everything in it take: an inode each, and each file's size in whole
// clusters, as Dolphin's host file system counts them.
static void folder_usage(const char *path, uint64_t *clusters, uint64_t *inodes) {
  enum { MOST_NAMES = 4096 };
  char saved[MAX_PATH], stored[MAX_PATH], child[PATH_BYTES * 2], found[MAX_PATH];
  char (*names)[NAME_BYTES] = calloc(MOST_NAMES, NAME_BYTES);
  files_save_path(path, saved, sizeof saved);
  uint32_t count = list_folder(path, saved, names, 0, MOST_NAMES);
  if (files_storage_path(path, stored, sizeof stored)) count = list_folder(path, stored, names, count, MOST_NAMES);
  *inodes += 1;  // The folder itself.
  for (uint32_t i = 0; i < count; i++) {
    snprintf(child, sizeof child, "%s/%s", strcmp(path, "/") ? path : "", names[i]);
    struct stat info;
    if (!files_find(child, found, sizeof found) || stat(found, &info)) continue;
    if (S_ISDIR(info.st_mode)) {
      folder_usage(child, clusters, inodes);
    } else {
      *inodes += 1;
      *clusters += ((uint64_t)info.st_size + CLUSTER_BYTES - 1) / CLUSTER_BYTES;
    }
  }
  free(names);
}

// The whole storage's numbers: its clusters and inodes, used and free, as IOS reports them.
static void write_stats(uint32_t out) {
  uint64_t clusters = 0, inodes = 0;
  folder_usage("/", &clusters, &inodes);
  if (clusters > USABLE_CLUSTERS) clusters = USABLE_CLUSTERS;
  if (inodes > TOTAL_INODES) inodes = TOTAL_INODES;
  const uint32_t stats[7] = {CLUSTER_BYTES, (uint32_t)(USABLE_CLUSTERS - clusters), (uint32_t)clusters, 0, RESERVED_CLUSTERS,
                             (uint32_t)(TOTAL_INODES - inodes), (uint32_t)inodes};
  for (uint32_t i = 0; i < 7; i++) wr32(out + i * 4, stats[i]);
}

// One folder's numbers: the clusters and inodes it and everything in it take.
static int get_usage(uint32_t request) {
  uint32_t vectors = guest(rd32(request + 24));
  if (rd32(request + 16) != 1 || rd32(request + 20) != 2) return INVALID;
  char path[PATH_BYTES], found[MAX_PATH];
  read_path(guest(rd32(vectors)), path);
  struct stat info;
  if (path[0] != '/') return INVALID;
  if (!files_find(path, found, sizeof found) || stat(found, &info)) return NOT_FOUND;
  if (!S_ISDIR(info.st_mode)) return INVALID;
  uint64_t clusters = 0, inodes = 0;
  folder_usage(path, &clusters, &inodes);
  wr32(guest(rd32(vectors + VECTOR_BYTES)), (uint32_t)clusters);
  wr32(guest(rd32(vectors + 2 * VECTOR_BYTES)), (uint32_t)inodes);
  return 0;
}

int fs_request(uint32_t command, uint32_t request) {
  uint32_t code = rd32(request + 12);
  if (command == IOS_IOCTLV) {
    if (code == READ_DIR) return read_folder(request);
    if (code == GET_USAGE) return get_usage(request);
    return IOS_UNHANDLED;
  }
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  uint32_t in = guest(rd32(request + 16)), out = guest(rd32(request + 24));
  char path[PATH_BYTES], other[PATH_BYTES];
  switch (code) {
    case GET_STATS: write_stats(out); return 0;
    case CREATE_DIR: case CREATE_FILE: return create_with_attributes(in, code == CREATE_DIR);
    case GET_ATTR: read_path(in, path); return get_attributes(path, out, rd32(request + 28));
    case DELETE: read_path(in, path); return erase(path);
    case RENAME: read_path(in, path); read_path(in + PATH_BYTES, other); return move(path, other);
    case SET_ATTR: return set_attributes(in);
    case SHUTDOWN: files_save_all(); return 0;  // IOS writes everything out before the power goes.
    default: return IOS_UNHANDLED;
  }
}
