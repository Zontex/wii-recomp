// What's in the disc drive. A disc is a sorted list of pieces, each some bytes of the
// data partition at its own offset, read from a file: the files of an extracted disc,
// found through the disc's own file table, or the entries of a recompiled disc image.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "drive.h"
#include "title.h"

enum {
  NAME_LIMIT = 1024, MAX_PIECES = 16384, ENTRY_BYTES = 12,
  BOOT_DOL_OFFSET = 0x420, BOOT_FST_OFFSET = 0x424, BI2_AT = 0x440, APPLOADER_AT = 0x2440,
  // A recompiled disc image: see make_disc.py.
  IMAGE_HEADER_BYTES = 64, IMAGE_ENTRY_BYTES = 64, IMAGE_VERSION = 1, IMAGE_NAME_BYTES = 32,
  PARTITION = 1, RAW = 2, TICKET = 3, TMD = 4, CERTIFICATES = 5, NATIVE = 6,
  // The unencrypted area at the start of a disc, as a real one lays it out: the header,
  // a table of partition groups, and the region. The data partition goes where Nintendo's
  // discs put it. Only this much can be read unencrypted, as on a real drive.
  RAW_BYTES = 0x50000, HEADER_BYTES = 0x440, PARTITION_GROUPS_AT = 0x40000, PARTITIONS_AT = 0x40020,
  REGION_AT = 0x4e000, REGION_BYTES = 0x20, DATA_PARTITION_AT = 0xf800000,
  // Every Wii disc ends its region area with this word, and the Wii Menu checks it.
  REGION_MAGIC_AT = 0x4fffc, REGION_MAGIC = (int)0xc3f81a8e,
};

typedef struct {
  uint64_t offset;         // In the data partition.
  uint64_t source_offset;  // In the file it's read from.
  uint64_t size;
  int source;              // Index into sources: the image, or one extracted file.
} Piece;

typedef struct {
  uint64_t at, size;
} Part;

static Piece pieces[MAX_PIECES];
static int piece_count;
static char *sources[MAX_PIECES];
static int source_count;
static int has_disc;
static int is_image;
static Part ticket, tmd, native;  // In the image, or from the folder's own files.
static char folder_ticket[NAME_LIMIT], folder_tmd[NAME_LIMIT];
static uint8_t raw[RAW_BYTES];
static FILE *open_file;  // The last source read, kept open, as reads come in runs.
static int open_source = -1;

static int read_source(int source, uint64_t at, uint64_t bytes, uint8_t *out);

static uint32_t big32(const uint8_t *data) {
  return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
}
static uint32_t little32(const uint8_t *data) {
  return (uint32_t)data[3] << 24 | (uint32_t)data[2] << 16 | (uint32_t)data[1] << 8 | data[0];
}
static uint64_t little64(const uint8_t *data) { return (uint64_t)little32(data + 4) << 32 | little32(data); }

const char *drive_platform(void) {
#if defined(__APPLE__) && defined(__x86_64__)
  return "darwin-x86_64";
#elif defined(__APPLE__) && defined(__aarch64__)
  return "darwin-arm64";
#elif defined(__linux__) && defined(__aarch64__)
  return "linux-aarch64";
#elif defined(__linux__) && defined(__x86_64__)
  return "linux-x86_64";
#else
  return "unknown";
#endif
}

void drive_eject(void) {
  if (open_file) fclose(open_file);
  open_file = 0;
  open_source = -1;
  for (int i = 0; i < source_count; i++) free(sources[i]);
  piece_count = source_count = has_disc = is_image = 0;
  ticket = tmd = native = (Part){0, 0};
  memset(raw, 0, sizeof raw);
  folder_ticket[0] = folder_tmd[0] = 0;
}

int drive_has_disc(void) { return has_disc; }

static int add_source(const char *path) {
  if (source_count == MAX_PIECES) return -1;
  sources[source_count] = strdup(path);
  return source_count++;
}

static int add_piece(uint64_t offset, uint64_t source_offset, uint64_t size, int source) {
  if (piece_count == MAX_PIECES) {
    fprintf(stderr, "drive: the disc has more than %d files\n", MAX_PIECES);
    return 0;
  }
  pieces[piece_count++] = (Piece){offset, source_offset, size, source};
  return 1;
}

static int by_offset(const void *a, const void *b) {
  uint64_t x = ((const Piece *)a)->offset, y = ((const Piece *)b)->offset;
  return x < y ? -1 : x > y;
}

static uint8_t *load(const char *path, size_t *length) {
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  fseek(file, 0, SEEK_END);
  *length = (size_t)ftell(file);
  rewind(file);
  uint8_t *data = malloc(*length ? *length : 1);
  int ok = fread(data, 1, *length, file) == *length;
  fclose(file);
  if (!ok) { free(data); return 0; }
  return data;
}

static void read_whole_into(const char *path, uint8_t *out, size_t limit) {
  FILE *file = fopen(path, "rb");
  if (!file) return;
  if (fread(out, 1, limit, file)) {}
  fclose(file);
}

static void put32(uint8_t *at, uint32_t value) {
  at[0] = (uint8_t)(value >> 24), at[1] = (uint8_t)(value >> 16), at[2] = (uint8_t)(value >> 8), at[3] = (uint8_t)value;
}

// One partition group holding one data partition, and the region area's closing word.
static void write_partition_table(void) {
  put32(raw + REGION_MAGIC_AT, (uint32_t)REGION_MAGIC);
  put32(raw + PARTITION_GROUPS_AT, 1);
  put32(raw + PARTITION_GROUPS_AT + 4, PARTITIONS_AT >> 2);
  put32(raw + PARTITIONS_AT, DATA_PARTITION_AT >> 2);
  put32(raw + PARTITIONS_AT + 4, 0);  // Data, rather than an update or a channel.
}

static int add_file(uint64_t offset, const char *path) {
  struct stat info;
  if (stat(path, &info)) { perror(path); return 0; }
  int source = add_source(path);
  return source >= 0 && add_piece(offset, 0, (uint64_t)info.st_size, source);
}

// The table lists entries in order; a directory says where it ends.
static int walk(const uint8_t *table, size_t table_bytes, const char *names, int index, int end, const char *folder) {
  while (index < end) {
    if ((size_t)(index + 1) * ENTRY_BYTES > table_bytes) return -1;
    const uint8_t *entry = table + (size_t)index * ENTRY_BYTES;
    char path[NAME_LIMIT];
    snprintf(path, sizeof path, "%s/%s", folder, names + (big32(entry) & 0xffffff));
    if (entry[0]) {
      index = walk(table, table_bytes, names, index + 1, (int)big32(entry + 8), path);
      if (index < 0) return -1;
    } else {
      if (!add_file((uint64_t)big32(entry + 4) << 2, path)) return -1;
      index++;
    }
  }
  return index;
}

static int insert_folder(const char *folder) {
  char path[NAME_LIMIT], files[NAME_LIMIT];
  size_t boot_bytes, table_bytes;
  snprintf(path, sizeof path, "%s/boot.bin", folder);
  uint8_t *boot = load(path, &boot_bytes);
  snprintf(path, sizeof path, "%s/fst.bin", folder);
  uint8_t *table = load(path, &table_bytes);
  int ok = boot && table && boot_bytes >= BOOT_FST_OFFSET + 4 && table_bytes >= ENTRY_BYTES;
  if (ok && big32(boot)) {  // A system title's folder has no game ID: the drive stays empty.
    snprintf(path, sizeof path, "%s/boot.bin", folder);
    ok = add_file(0, path);
    struct stat info;  // Read by the Wii Menu to boot a disc, and only in extractions that keep them.
    snprintf(path, sizeof path, "%s/bi2.bin", folder);
    if (!stat(path, &info)) ok = ok && add_file(BI2_AT, path);
    snprintf(path, sizeof path, "%s/apploader.img", folder);
    if (!stat(path, &info)) ok = ok && add_file(APPLOADER_AT, path);
    snprintf(path, sizeof path, "%s/main.dol", folder);
    ok = ok && add_file((uint64_t)big32(boot + BOOT_DOL_OFFSET) << 2, path);
    snprintf(path, sizeof path, "%s/fst.bin", folder);
    ok = ok && add_file((uint64_t)big32(boot + BOOT_FST_OFFSET) << 2, path);
    int total = (int)big32(table + 8);
    snprintf(files, sizeof files, "%s/../files", folder);
    ok = ok && total > 0 && (size_t)total * ENTRY_BYTES <= table_bytes &&
         walk(table, table_bytes, (const char *)table + (size_t)total * ENTRY_BYTES, 1, total, files) >= 0;
    memcpy(raw, boot, HEADER_BYTES);  // Replaced by the disc's own header where the extraction kept it.
    snprintf(path, sizeof path, "%s/../disc/header.bin", folder);
    read_whole_into(path, raw, HEADER_BYTES);
    snprintf(path, sizeof path, "%s/../disc/region.bin", folder);
    read_whole_into(path, raw + REGION_AT, REGION_BYTES);
    snprintf(folder_ticket, sizeof folder_ticket, "%s/../ticket.bin", folder);
    snprintf(folder_tmd, sizeof folder_tmd, "%s/../tmd.bin", folder);
    has_disc = ok;
  }
  free(boot);
  free(table);
  if (!ok) fprintf(stderr, "drive: %s isn't an extracted disc\n", folder);
  return ok;
}

static int insert_image(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) { perror(path); return 0; }
  uint8_t header[IMAGE_HEADER_BYTES];
  int ok = fread(header, 1, sizeof header, file) == sizeof header && !memcmp(header, "WIIRDISC", 8);
  if (ok && little32(header + 8) != IMAGE_VERSION) {
    fprintf(stderr, "drive: %s is a disc image of version %u, and this console reads version %d\n", path, little32(header + 8),
            IMAGE_VERSION);
    ok = 0;
  }
  uint32_t count = ok ? little32(header + 12) : 0;
  Piece raw_parts[8];
  int raw_count = 0;
  ok = ok && count <= MAX_PIECES && !fseeko(file, (off_t)little64(header + 16), SEEK_SET);
  int source = ok ? add_source(path) : 0;
  for (uint32_t i = 0; ok && i < count; i++) {
    uint8_t entry[IMAGE_ENTRY_BYTES];
    if (fread(entry, 1, sizeof entry, file) != sizeof entry) { ok = 0; break; }
    uint32_t kind = little32(entry);
    uint64_t disc_offset = little64(entry + 8), at = little64(entry + 16), size = little64(entry + 24);
    char name[IMAGE_NAME_BYTES + 1] = {0};
    memcpy(name, entry + 32, IMAGE_NAME_BYTES);
    if (kind == PARTITION) ok = add_piece(disc_offset, at, size, source);
    else if (kind == RAW && disc_offset < RAW_BYTES) raw_parts[raw_count < 8 ? raw_count++ : 7] = (Piece){disc_offset, at, size, source};
    else if (kind == TICKET) ticket = (Part){at, size};
    else if (kind == TMD) tmd = (Part){at, size};
    else if (kind == NATIVE && !strcmp(name, drive_platform())) native = (Part){at, size};
  }
  fclose(file);
  for (int i = 0; ok && i < raw_count; i++) {  // Small, so read once.
    uint64_t room = RAW_BYTES - raw_parts[i].offset, bytes = raw_parts[i].size < room ? raw_parts[i].size : room;
    ok = read_source(source, raw_parts[i].source_offset, bytes, raw + raw_parts[i].offset);
  }
  if (!ok) fprintf(stderr, "drive: %s isn't a readable disc image\n", path);
  is_image = has_disc = ok;
  return ok;
}

int drive_insert(const char *path) {
  drive_eject();
  struct stat info;
  if (stat(path, &info)) { perror(path); return 0; }
  int ok = S_ISDIR(info.st_mode) ? insert_folder(path) : insert_image(path);
  if (!ok) { drive_eject(); return 0; }
  qsort(pieces, (size_t)piece_count, sizeof *pieces, by_offset);
  if (has_disc) write_partition_table();
  if (has_disc) fprintf(stderr, "drive: %s, %d pieces\n", path, piece_count);
  return 1;
}

// The piece holding `offset`, or the first one after it, or piece_count.
static int piece_from(uint64_t offset) {
  int low = 0, high = piece_count;
  while (low < high) {
    int middle = (low + high) / 2;
    if (pieces[middle].offset + pieces[middle].size <= offset) low = middle + 1; else high = middle;
  }
  return low;
}

static FILE *source_file(int source) {
  if (source != open_source) {
    if (open_file) fclose(open_file);
    open_file = fopen(sources[source], "rb");
    open_source = open_file ? source : -1;
    if (!open_file) perror(sources[source]);
  }
  return open_file;
}

static int read_source(int source, uint64_t at, uint64_t bytes, uint8_t *out) {
  FILE *file = source_file(source);
  return file && !fseeko(file, (off_t)at, SEEK_SET) && fread(out, 1, (size_t)bytes, file) == bytes;
}

int drive_read(uint64_t offset, uint32_t bytes, uint8_t *out) {
  if (!has_disc) return 0;
  static int logged = -1;  // WII_RECOMP_DISC_LOG: each read's place in the data partition and size.
  if (logged < 0) logged = getenv("WII_RECOMP_DISC_LOG") != 0;
  if (logged) fprintf(stderr, "disc read %llx %x\n", (unsigned long long)offset, bytes);
  int index = piece_from(offset);
  while (bytes) {
    const Piece *piece = index < piece_count ? &pieces[index] : 0;
    if (!piece || offset < piece->offset) {
      // Reads are rounded up, so they can run into the padding between files.
      uint64_t gap = piece ? piece->offset - offset : bytes;
      uint32_t part = gap < bytes ? (uint32_t)gap : bytes;
      memset(out, 0, part);
      offset += part, out += part, bytes -= part;
      continue;
    }
    uint64_t inside = offset - piece->offset, left = piece->size - inside;
    uint32_t part = left < bytes ? (uint32_t)left : bytes;
    if (!read_source(piece->source, piece->source_offset + inside, part, out)) return 0;
    offset += part, out += part, bytes -= part;
    index++;
  }
  return 1;
}

int drive_read_raw(uint64_t offset, uint32_t bytes, uint8_t *out) {
  if (!has_disc || offset > RAW_BYTES || bytes > RAW_BYTES - offset) return 0;
  memcpy(out, raw + offset, bytes);
  return 1;
}

static size_t read_part(Part part, const char *folder_path, uint8_t *out, size_t limit) {
  if (!has_disc) return 0;
  if (is_image) {
    size_t bytes = part.size < limit ? (size_t)part.size : limit;
    return part.size && read_source(0, part.at, bytes, out) ? bytes : 0;
  }
  FILE *file = fopen(folder_path, "rb");
  if (!file) return 0;
  size_t got = fread(out, 1, limit, file);
  fclose(file);
  return got;
}

size_t drive_ticket(uint8_t *out, size_t limit) { return read_part(ticket, folder_ticket, out, limit); }
size_t drive_tmd(uint8_t *out, size_t limit) { return read_part(tmd, folder_tmd, out, limit); }

int drive_native(const char *folder, char *path, size_t limit) {
  if (!has_disc || !is_image || !native.size) return 0;
  uint8_t *code = malloc((size_t)native.size);
  if (!code || !read_source(0, native.at, native.size, code)) { free(code); return 0; }
  mkdir(folder, 0755);
  snprintf(path, limit, "%s/%016llx.%s", folder, (unsigned long long)title_check(code, (unsigned long)native.size),
#ifdef __APPLE__
           "dylib");
#else
           "so");
#endif
  struct stat info;
  int ok = !stat(path, &info) && (uint64_t)info.st_size == native.size;  // Written by an earlier run.
  if (!ok) {
    char partial[NAME_LIMIT + 16];
    snprintf(partial, sizeof partial, "%s.partial", path);
    FILE *file = fopen(partial, "wb");
    ok = file && fwrite(code, 1, (size_t)native.size, file) == native.size;
    if (file && fclose(file)) ok = 0;
    ok = ok && !rename(partial, path);  // Only a whole library gets the real name.
    if (!ok) fprintf(stderr, "drive: can't write the disc's native code to %s: %s\n", path, strerror(errno));
  }
  free(code);
  return ok;
}
