// Reads Dolphin's FIFO recordings. The layout follows Source/Core/Core/FifoPlayer/FifoDataFile.cpp.
// Everything in the file is little-endian, except the commands and memory, which are the Wii's own.
#include "dff.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_ID 0x0d01f1f0u
enum {
  FILE_VERSION = 6, HEADER_BYTES = 128, FRAME_BYTES = 64, UPDATE_BYTES = 24, WORD_BYTES = 4,
  HEADER_VERSION = 4, HEADER_MINIMUM_LOADER = 8, HEADER_BP = 12, HEADER_CP = 24, HEADER_XF = 36, HEADER_XF_REGISTERS = 48,
  HEADER_FRAMES = 60, HEADER_FRAME_COUNT = 68, HEADER_FLAGS = 72, HEADER_TEXTURE = 76, HEADER_MEM1 = 88, HEADER_MEM2 = 92, HEADER_GAME_ID = 96,
  SIZE_AFTER_OFFSET = 8,
  FRAME_FIFO_SIZE = 8, FRAME_FIFO_START = 12, FRAME_FIFO_END = 16, FRAME_UPDATES = 20, FRAME_UPDATE_COUNT = 28,
  UPDATE_ADDRESS = 4, UPDATE_DATA = 8, UPDATE_SIZE = 16, UPDATE_TYPE = 20,
  EXIT_STOP = 10,
};

typedef struct {
  const char *path;
  const uint8_t *bytes;
  uint64_t size;
} File;

static void stop(const File *file, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  fprintf(stderr, "stop: %s: ", file->path);
  vfprintf(stderr, format, arguments);
  fprintf(stderr, "\n");
  va_end(arguments);
  exit(EXIT_STOP);
}

static uint32_t read32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read64(const uint8_t *p) { return (uint64_t)read32(p) | ((uint64_t)read32(p + 4) << 32); }

static const uint8_t *piece(const File *file, uint64_t offset, uint64_t bytes, const char *what) {
  if (offset > file->size || bytes > file->size - offset) {
    stop(file, "%s runs past the end of the %llu-byte file, so it is cut short or damaged", what, (unsigned long long)file->size);
  }
  return file->bytes + offset;
}

// A short block leaves the rest zero, as Dolphin's loader does.
static void read_words(const File *file, const uint8_t *entry, uint32_t *out, uint32_t count, const char *what) {
  uint32_t recorded = read32(entry + SIZE_AFTER_OFFSET);
  if (recorded < count) count = recorded;
  const uint8_t *from = piece(file, read64(entry), (uint64_t)count * WORD_BYTES, what);
  for (uint32_t i = 0; i < count; i++) out[i] = read32(from + i * WORD_BYTES);
}

static void read_frame(const File *file, const uint8_t *entry, uint32_t number, DffFrame *frame) {
  char what[64];
  frame->fifo_bytes = read32(entry + FRAME_FIFO_SIZE);
  frame->fifo_start = read32(entry + FRAME_FIFO_START);
  frame->fifo_end = read32(entry + FRAME_FIFO_END);
  frame->update_count = read32(entry + FRAME_UPDATE_COUNT);
  snprintf(what, sizeof what, "frame %u's commands", number);
  frame->fifo = piece(file, read64(entry), frame->fifo_bytes, what);
  snprintf(what, sizeof what, "frame %u's memory list", number);
  const uint8_t *list = piece(file, read64(entry + FRAME_UPDATES), (uint64_t)frame->update_count * UPDATE_BYTES, what);
  frame->updates = calloc(frame->update_count ? frame->update_count : 1, sizeof *frame->updates);
  if (!frame->updates) stop(file, "not enough memory");
  uint32_t last_position = 0;
  for (uint32_t i = 0; i < frame->update_count; i++) {
    const uint8_t *from = list + (uint64_t)i * UPDATE_BYTES;
    DffUpdate *update = &frame->updates[i];
    update->fifo_position = read32(from);
    update->address = read32(from + UPDATE_ADDRESS);
    update->bytes = read32(from + UPDATE_SIZE);
    update->type = from[UPDATE_TYPE];
    snprintf(what, sizeof what, "frame %u's memory update %u", number, i);
    update->data = piece(file, read64(from + UPDATE_DATA), update->bytes, what);
    if (update->fifo_position < last_position || update->fifo_position > frame->fifo_bytes) {
      stop(file, "frame %u's memory update %u sits at command byte %u, out of order or past the frame's %u bytes",
           number, i, update->fifo_position, frame->fifo_bytes);
    }
    last_position = update->fifo_position;
  }
}

static File read_file(const char *path) {
  File file = {path, 0, 0};
  FILE *from = fopen(path, "rb");
  if (!from) stop(&file, "can't open it");
  fseeko(from, 0, SEEK_END);
  off_t size = ftello(from);
  fseeko(from, 0, SEEK_SET);
  if (size < 0) stop(&file, "can't read it");
  uint8_t *bytes = malloc((size_t)size + 1);
  if (!bytes) stop(&file, "not enough memory for %lld bytes", (long long)size);
  if (fread(bytes, 1, (size_t)size, from) != (size_t)size) stop(&file, "can't read it");
  fclose(from);
  file.bytes = bytes;
  file.size = (uint64_t)size;
  return file;
}

Dff *dff_load(const char *path) {
  File file = read_file(path);
  if (file.size < HEADER_BYTES) stop(&file, "only %llu bytes, too short for a FIFO recording's header", (unsigned long long)file.size);
  const uint8_t *header = file.bytes;
  uint32_t id = read32(header), version = read32(header + HEADER_VERSION), minimum = read32(header + HEADER_MINIMUM_LOADER);
  if (id != FILE_ID) stop(&file, "not a Dolphin FIFO recording: its first word is %08x, not %08x", id, FILE_ID);
  if (version != FILE_VERSION) {
    stop(&file, "file version %u is not supported, only %d, which current Dolphin builds write. Record it again with a current build", version, FILE_VERSION);
  }
  if (minimum > FILE_VERSION) stop(&file, "needs a loader of version %u, and this one is %d", minimum, FILE_VERSION);

  Dff *dff = calloc(1, sizeof *dff);
  if (!dff) stop(&file, "not enough memory");
  dff->version = version;
  dff->flags = read32(header + HEADER_FLAGS);
  dff->mem1_bytes = read32(header + HEADER_MEM1);
  dff->mem2_bytes = read32(header + HEADER_MEM2);
  memcpy(dff->game_id, header + HEADER_GAME_ID, DFF_GAME_ID_BYTES);

  read_words(&file, header + HEADER_BP, dff->bp, DFF_BP_WORDS, "the BP registers");
  read_words(&file, header + HEADER_CP, dff->cp, DFF_CP_WORDS, "the CP registers");
  read_words(&file, header + HEADER_XF, dff->xf, DFF_XF_WORDS, "XF memory");
  read_words(&file, header + HEADER_XF_REGISTERS, dff->xf_registers, DFF_XF_REGISTER_WORDS, "the XF registers");
  uint32_t texture_bytes = read32(header + HEADER_TEXTURE + SIZE_AFTER_OFFSET);
  if (texture_bytes > DFF_TEXTURE_BYTES) texture_bytes = DFF_TEXTURE_BYTES;
  memcpy(dff->texture_memory, piece(&file, read64(header + HEADER_TEXTURE), texture_bytes, "texture memory"), texture_bytes);

  dff->frame_count = read32(header + HEADER_FRAME_COUNT);
  const uint8_t *list = piece(&file, read64(header + HEADER_FRAMES), (uint64_t)dff->frame_count * FRAME_BYTES, "the frame list");
  dff->frames = calloc(dff->frame_count ? dff->frame_count : 1, sizeof *dff->frames);
  if (!dff->frames) stop(&file, "not enough memory");
  for (uint32_t i = 0; i < dff->frame_count; i++) read_frame(&file, list + (uint64_t)i * FRAME_BYTES, i, &dff->frames[i]);
  return dff;
}
