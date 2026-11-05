// Records frames as the chip reads them, into Dolphin's FIFO recording format (dff.h),
// which recomp/tools/replay plays back with no game running. Any scene of any game can
// then be timed, profiled and compared in seconds, again and again.
//
// A recording starts at the end of a frame: the chip's registers as they stand there,
// then each top-level command read, a frame ending at each copy to the screen. Display
// lists are called from memory, so their bytes aren't recorded. Just before each frame's
// first draw or list go the pieces of memory that changed since the frame before, all
// memory for the first. Memory is taken once a frame, on the chip's side, which may run a
// little behind the game, so a recording is a faithful picture of what was drawn, not a
// cycle-exact one: memory a game rewrites between two draws of a frame shows its last state.
//
// Ask with the control pipe's "record <frames> <file>", or WII_RECOMP_RECORD=<frame>:<frames>:<file>.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "dff.h"
#include "gx.h"
#include "title.h"

enum {
  FILE_ID = 0x0d01f1f0, FILE_VERSION = 6, LOADER_VERSION = 1, HEADER_BYTES = 128, FRAME_BYTES = 64, UPDATE_BYTES = 24,
  XF_REGISTERS_START = 0x1000, CHUNK_BYTES = 4096, COMMAND_BP = 0x61, BP_TEV_REGISTER = 0xe0, TEV_KONST_FLAG = 0x800000,
};

typedef struct { uint8_t *bytes; size_t used, size; } Bytes;
typedef struct { uint32_t position, address; size_t data_at; uint32_t bytes; } Update;
typedef struct { size_t fifo_at, fifo_bytes; size_t first_update; uint32_t update_count; } Frame;

static _Atomic int requested;  // Frames asked for, taken by the chip's side at a frame's end.
static char requested_path[4096], path[4096];
static int frames_wanted, frames_done, recording;
static Bytes fifo, data;
static Update *updates;
static size_t update_count, update_room;
static Frame *frames;
static uint8_t *shadow[2];  // MEM1 and MEM2 as last recorded.
static uint32_t start_bp[256], start_cp[256], start_xf[DFF_XF_WORDS], start_xf_registers[DFF_XF_REGISTER_WORDS];
static uint8_t start_texture[DFF_TEXTURE_BYTES];
static int depth, memory_taken;

static void add(Bytes *to, const void *from, size_t bytes) {
  if (to->used + bytes > to->size) {
    size_t size = to->size ? to->size : 1 << 20;
    while (size < to->used + bytes) size *= 2;
    uint8_t *grown = realloc(to->bytes, size);
    if (!grown) { fprintf(stderr, "stop: record: no memory for %zu bytes\n", size); exit(10); }
    to->bytes = grown, to->size = size;
  }
  memcpy(to->bytes + to->used, from, bytes);
  to->used += bytes;
}

void gx_record_request(int count, const char *file) {
  if (count <= 0 || !file || !*file) return;
  snprintf(requested_path, sizeof requested_path, "%s", file);
  atomic_store(&requested, count);
}

static void note_update(uint32_t address, const uint8_t *from, uint32_t bytes) {
  if (update_count == update_room) {
    update_room = update_room ? update_room * 2 : 1024;
    updates = realloc(updates, update_room * sizeof *updates);
    if (!updates) { fprintf(stderr, "stop: record: no memory\n"); exit(10); }
  }
  updates[update_count++] = (Update){(uint32_t)fifo.used - (uint32_t)frames[frames_done].fifo_at, address, data.used, bytes};
  add(&data, from, bytes);
}

// The pieces of one memory that changed since last time, joined where they touch.
static void note_changes(int which, uint32_t start, uint32_t size) {
  const uint8_t *now = host(start | 0x80000000u);
  uint8_t *before = shadow[which];
  uint32_t run = 0, run_bytes = 0;
  for (uint32_t at = 0; at <= size; at += CHUNK_BYTES) {
    int changed = at < size && memcmp(now + at, before + at, CHUNK_BYTES);
    if (changed) {
      if (!run_bytes) run = at;
      run_bytes += CHUNK_BYTES;
      memcpy(before + at, now + at, CHUNK_BYTES);
      continue;
    }
    if (run_bytes) note_update(start + run, now + run, run_bytes);
    run_bytes = 0;
  }
}

static void begin_frame(void) {
  frames = realloc(frames, (size_t)(frames_done + 1) * sizeof *frames);
  if (!frames) { fprintf(stderr, "stop: record: no memory\n"); exit(10); }
  frames[frames_done] = (Frame){fifo.used, 0, update_count, 0};
  memory_taken = 0;
}

static void take_memory(void) {
  note_changes(0, MEM1_START, MEM1_SIZE);
  note_changes(1, MEM2_START, MEM2_SIZE);
  frames[frames_done].update_count = (uint32_t)(update_count - frames[frames_done].first_update);
  memory_taken = 1;
}

static void bp_command(uint32_t reg, uint32_t value) {
  uint8_t command[5] = {COMMAND_BP, (uint8_t)reg, (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value};
  add(&fifo, command, sizeof command);
}

static void start(int count) {
  snprintf(path, sizeof path, "%s", requested_path);
  frames_wanted = count, frames_done = 0;
  fifo.used = data.used = 0;
  update_count = 0;
  for (int i = 0; i < 2; i++) {
    free(shadow[i]);
    shadow[i] = calloc(i ? MEM2_SIZE : MEM1_SIZE, 1);  // So the first frame takes all memory that isn't zero.
    if (!shadow[i]) { fprintf(stderr, "record: no memory to compare with\n"); return; }
  }
  memcpy(start_bp, gx.bp, sizeof start_bp);
  memcpy(start_cp, gx.cp, sizeof start_cp);
  memcpy(start_xf, gx.xf, sizeof start_xf);
  memcpy(start_xf_registers, &gx.xf[XF_REGISTERS_START], sizeof start_xf_registers);
  memset(start_texture, 0, sizeof start_texture);
  gx_save_palettes(start_texture);
  recording = 1;
  begin_frame();
  // The stage registers and constants share BP addresses, so the registers alone hold
  // only whichever was written last. Both are put back by commands, that one last.
  for (int i = 0; i < 8; i++) {
    uint32_t last = gx.bp[BP_TEV_REGISTER + i], *other = last & TEV_KONST_FLAG ? gx.tev_color : gx.tev_konst;
    bp_command(BP_TEV_REGISTER + i, other[i]);
    bp_command(BP_TEV_REGISTER + i, last);
  }
  fprintf(stderr, "record: %d frames to %s\n", count, path);
}

static void put32(uint8_t *at, uint32_t value) { for (int i = 0; i < 4; i++) at[i] = (uint8_t)(value >> (i * 8)); }
static void put64(uint8_t *at, uint64_t value) { put32(at, (uint32_t)value); put32(at + 4, (uint32_t)(value >> 32)); }

static int write_words(FILE *to, uint64_t *at, const uint32_t *words, uint32_t count) {
  uint8_t bytes[4];
  for (uint32_t i = 0; i < count; i++) {
    put32(bytes, words[i]);
    if (fwrite(bytes, 1, 4, to) != 4) return 0;
  }
  *at += (uint64_t)count * 4;
  return 1;
}

// The layout follows Dolphin's FifoDataFile.cpp, as dff.c reads it.
static void finish(void) {
  recording = 0;
  FILE *to = fopen(path, "wb");
  if (!to) { fprintf(stderr, "record: can't write %s\n", path); return; }
  uint64_t frame_list = HEADER_BYTES, update_list = frame_list + (uint64_t)frames_done * FRAME_BYTES;
  uint64_t bp_at = update_list + (uint64_t)update_count * UPDATE_BYTES, cp_at = bp_at + 256 * 4, xf_at = cp_at + 256 * 4;
  uint64_t xf_registers_at = xf_at + DFF_XF_WORDS * 4, texture_at = xf_registers_at + DFF_XF_REGISTER_WORDS * 4;
  uint64_t fifo_at = texture_at + DFF_TEXTURE_BYTES, data_at = fifo_at + fifo.used;
  uint8_t header[HEADER_BYTES] = {0};
  put32(header, FILE_ID), put32(header + 4, FILE_VERSION), put32(header + 8, LOADER_VERSION);
  put64(header + 12, bp_at), put32(header + 20, 256);
  put64(header + 24, cp_at), put32(header + 32, 256);
  put64(header + 36, xf_at), put32(header + 44, DFF_XF_WORDS);
  put64(header + 48, xf_registers_at), put32(header + 56, DFF_XF_REGISTER_WORDS);
  put64(header + 60, frame_list), put32(header + 68, (uint32_t)frames_done);
  put32(header + 72, DFF_FLAG_WII);
  put64(header + 76, texture_at), put32(header + 84, DFF_TEXTURE_BYTES);
  put32(header + 88, MEM1_SIZE), put32(header + 92, MEM2_SIZE);
  for (int i = 0; i < 4; i++) header[96 + i] = (uint8_t)(title->title_low >> (24 - i * 8));
  int ok = fwrite(header, 1, sizeof header, to) == sizeof header;
  for (int i = 0; ok && i < frames_done; i++) {
    uint8_t entry[FRAME_BYTES] = {0};
    put64(entry, fifo_at + frames[i].fifo_at), put32(entry + 8, (uint32_t)frames[i].fifo_bytes);
    put64(entry + 20, update_list + (uint64_t)frames[i].first_update * UPDATE_BYTES), put32(entry + 28, frames[i].update_count);
    ok = fwrite(entry, 1, sizeof entry, to) == sizeof entry;
  }
  for (size_t i = 0; ok && i < update_count; i++) {
    uint8_t entry[UPDATE_BYTES] = {0};
    put32(entry, updates[i].position), put32(entry + 4, updates[i].address);
    put64(entry + 8, data_at + updates[i].data_at), put32(entry + 16, updates[i].bytes);
    entry[20] = DFF_UPDATE_TEXTURE;
    ok = fwrite(entry, 1, sizeof entry, to) == sizeof entry;
  }
  uint64_t at = bp_at;
  ok = ok && write_words(to, &at, start_bp, 256) && write_words(to, &at, start_cp, 256) && write_words(to, &at, start_xf, DFF_XF_WORDS)
       && write_words(to, &at, start_xf_registers, DFF_XF_REGISTER_WORDS);
  ok = ok && fwrite(start_texture, 1, sizeof start_texture, to) == sizeof start_texture;
  ok = ok && fwrite(fifo.bytes, 1, fifo.used, to) == fifo.used && fwrite(data.bytes, 1, data.used, to) == data.used;
  ok = !fclose(to) && ok;
  fprintf(stderr, "record: %s %s: %d frames, %zu command bytes, %zu memory pieces of %zu bytes\n", ok ? "wrote" : "couldn't write", path,
          frames_done, fifo.used, update_count, data.used);
}

void gx_record_list_enter(void) { depth++; }
void gx_record_list_leave(void) { depth--; }

enum { COMMAND_CALL_LIST = 0x40, COMMAND_DRAW = 0x80 };

void gx_record_command(const uint8_t *bytes, uint32_t count) {
  if (!recording || depth) return;
  if (!memory_taken && (bytes[0] >= COMMAND_DRAW || bytes[0] == COMMAND_CALL_LIST)) take_memory();
  add(&fifo, bytes, count);
}

// After the command that copied a picture to the screen.
void gx_record_frame_shown(void) {
  static int from_environment = -1;
  if (from_environment < 0) {
    const char *asked = getenv("WII_RECOMP_RECORD");
    int frame = 0, count = 0, used = 0;
    from_environment = asked && sscanf(asked, "%d:%d:%n", &frame, &count, &used) == 2 && used ? frame : 0;
    if (from_environment) snprintf(requested_path, sizeof requested_path, "%s", asked + used), frames_wanted = count;
  }
  if (from_environment > 0 && --from_environment == 0) atomic_store(&requested, frames_wanted);
  if (recording) {
    if (!memory_taken) take_memory();
    frames[frames_done].fifo_bytes = fifo.used - frames[frames_done].fifo_at;
    if (++frames_done == frames_wanted) finish();
    else begin_frame();
    return;
  }
  int count = atomic_exchange(&requested, 0);
  if (count > 0) start(count);
}
