// Plays a Dolphin FIFO recording through the runtime's own graphics code, with no game
// and no window, and saves each frame as a PNG. It sets up the chip as Dolphin's FIFO
// Player does: see LoadRegisters, ClearEfb and WriteFrame in FifoPlayer.cpp.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#include "cpu.h"
#include "dff.h"
#include "gx_backend.h"
#include "gx_vulkan.h"

enum {
  COMMAND_BP = 0x61, BP_COMMAND_BYTES = 5, XF_REGISTERS_START = 0x1000,
  BP_DRAW_DONE = 0x45, BP_TOKEN = 0x47, BP_TOKEN_NOW = 0x48, BP_COPY_ORIGIN = 0x49, BP_COPY_SIZE = 0x4a, BP_COPY = 0x52,
  BP_PRELOAD = 0x63, BP_LOAD_PALETTE = 0x65, BP_PERFORMANCE = 0x67, COPY_HEIGHT_SHIFT = 10,
  PHYSICAL_MASK = 0x1fffffff, FRAME_NAME_DIGITS = 4,
  EXIT_USAGE = 2, EXIT_STOP = 10, RGB_BYTES = 3, RGBA_BYTES = 4,
};

Cpu cpu;

static uint8_t *picture;
static int picture_width, picture_height, pictures_in_frame;

static void stop_because(const char *message, const char *detail) {
  fprintf(stderr, "stop: %s: %s\n", message, detail);
  exit(EXIT_STOP);
}

static void keep_picture(const uint8_t *rgba, int width, int height) {
  size_t bytes = (size_t)width * (size_t)height * RGBA_BYTES;
  if (width != picture_width || height != picture_height) picture = realloc(picture, bytes);
  if (!picture) stop_because("no memory for a picture", strerror(errno));
  memcpy(picture, rgba, bytes);
  picture_width = width;
  picture_height = height;
  pictures_in_frame++;
}

static void write_chunk(FILE *to, const char *kind, const uint8_t *data, uint32_t bytes) {
  uint8_t head[8] = {(uint8_t)(bytes >> 24), (uint8_t)(bytes >> 16), (uint8_t)(bytes >> 8), (uint8_t)bytes};
  memcpy(head + 4, kind, 4);
  uLong check = crc32(crc32(0, 0, 0), head + 4, 4);
  if (bytes) check = crc32(check, data, bytes);
  uint8_t tail[4] = {(uint8_t)(check >> 24), (uint8_t)(check >> 16), (uint8_t)(check >> 8), (uint8_t)check};
  fwrite(head, 1, sizeof head, to);
  fwrite(data, 1, bytes, to);
  fwrite(tail, 1, sizeof tail, to);
}

// A TV shows no alpha, and many pictures leave it at 0, so the file has none.
static void save_png(const char *path) {
  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  size_t row_bytes = (size_t)picture_width * RGB_BYTES + 1, raw_bytes = row_bytes * (size_t)picture_height;
  uint8_t *raw = malloc(raw_bytes);
  uLongf packed_bytes = compressBound(raw_bytes);
  uint8_t *packed = malloc(packed_bytes);
  if (!raw || !packed) stop_because("no memory for a picture", path);
  for (int y = 0; y < picture_height; y++) {
    uint8_t *to = raw + (size_t)y * row_bytes;
    const uint8_t *from = picture + (size_t)y * (size_t)picture_width * RGBA_BYTES;
    *to++ = 0;  // PNG's "no filter" for this row.
    for (int x = 0; x < picture_width; x++, from += RGBA_BYTES, to += RGB_BYTES) memcpy(to, from, RGB_BYTES);
  }
  if (compress2(packed, &packed_bytes, raw, raw_bytes, Z_BEST_SPEED) != Z_OK) stop_because("can't pack a picture", path);
  uint32_t w = (uint32_t)picture_width, h = (uint32_t)picture_height;
  uint8_t header[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
  FILE *to = fopen(path, "wb");
  if (!to) stop_because("can't write a picture", path);
  fwrite(signature, 1, sizeof signature, to);
  write_chunk(to, "IHDR", header, sizeof header);
  write_chunk(to, "IDAT", packed, (uint32_t)packed_bytes);
  write_chunk(to, "IEND", 0, 0);
  if (ferror(to) || fclose(to)) stop_because("can't write a picture", path);
  free(raw);
  free(packed);
}

static void write_bp(uint32_t reg, uint32_t value) {
  uint8_t command[BP_COMMAND_BYTES] = {COMMAND_BP, (uint8_t)reg, (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value};
  gx_parse(command, sizeof command);
}

// Writing these does something once, rather than setting state.
static int bp_is_state(uint32_t reg) {
  return reg != BP_DRAW_DONE && reg != BP_TOKEN && reg != BP_TOKEN_NOW && reg != BP_COPY
      && reg != BP_PRELOAD && reg != BP_LOAD_PALETTE && reg != BP_PERFORMANCE;
}

// BP registers go through the command stream, as some feed the color stages' banks.
static void load_registers(const Dff *dff) {
  for (uint32_t reg = 0; reg < DFF_BP_WORDS; reg++) {
    if (bp_is_state(reg)) write_bp(reg, dff->bp[reg]);
  }
  memcpy(gx.cp, dff->cp, sizeof gx.cp);
  memcpy(gx.xf, dff->xf, sizeof dff->xf);
  memcpy(&gx.xf[XF_REGISTERS_START], dff->xf_registers, sizeof dff->xf_registers);
  gx_xf_note(0, XF_SIZE);
  gx_cp_written++;
  gx_restore_palettes(dff->texture_memory);
}

// The recording starts mid-game, so the first frames can land on the picture before it.
// Clears everything to the recorded color, as the FIFO Player does.
static void clear_whole_picture(const Dff *dff) {
  write_bp(BP_COPY_ORIGIN, 0);
  write_bp(BP_COPY_SIZE, (EFB_WIDTH - 1) | ((EFB_HEIGHT - 1) << COPY_HEIGHT_SHIFT));
  gx_clear();
  write_bp(BP_COPY_ORIGIN, dff->bp[BP_COPY_ORIGIN]);
  write_bp(BP_COPY_SIZE, dff->bp[BP_COPY_SIZE]);
}

static double now_ms(void);
static double memory_ms;  // Time spent putting recorded memory in, which isn't the chip's work.

static void write_memory(const DffUpdate *update) {
  double began = now_ms();
  uint32_t physical = update->address & PHYSICAL_MASK;
  uint8_t *base = mem1;
  uint64_t size = MEM1_SIZE;
  if (physical & MEM2_START) {
    physical -= MEM2_START;
    base = mem2;
    size = MEM2_SIZE;
  }
  if ((uint64_t)physical + update->bytes > size) {
    fprintf(stderr, "stop: recorded memory at %08x, %u bytes, is outside the Wii's memory\n", update->address, update->bytes);
    exit(EXIT_STOP);
  }
  memcpy(base + physical, update->data, update->bytes);
  memory_ms += now_ms() - began;
}

// Memory goes in just as the commands reach the place it was recorded at. Those places
// are always between commands, since Dolphin notes them before adding the command.
static void play_frame(const DffFrame *frame) {
  uint32_t at = 0;
  for (uint32_t i = 0; i < frame->update_count; i++) {
    const DffUpdate *update = &frame->updates[i];
    gx_parse(frame->fifo + at, update->fifo_position - at);
    at = update->fifo_position;
    write_memory(update);
  }
  gx_parse(frame->fifo + at, frame->fifo_bytes - at);
}

static void make_folder(const char *path) {
  if (mkdir(path, 0777) && errno != EEXIST) stop_because(path, strerror(errno));
}

static uint32_t parse_frame_limit(const char *text) {
  char *end;
  unsigned long limit = strtoul(text, &end, 10);
  if (!*text || *end || !limit) stop_because("--frames wants a count above zero, not", text);
  return (uint32_t)limit;
}

static double clock_ms(clockid_t clock) {
  struct timespec t;
  clock_gettime(clock, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}
static double now_ms(void) { return clock_ms(CLOCK_MONOTONIC); }

int main(int argc, char **argv) {
  uint32_t limit = UINT32_MAX, repeats = 1;
  int bad = argc < 3;
  for (int i = 3; i + 1 < argc + 1 && !bad; i += 2) {
    if (i + 1 >= argc) bad = 1;
    else if (!strcmp(argv[i], "--frames")) limit = parse_frame_limit(argv[i + 1]);
    else if (!strcmp(argv[i], "--repeat")) repeats = parse_frame_limit(argv[i + 1]);
    else bad = 1;
  }
  if (bad) {
    fprintf(stderr, "usage: %s <recording.dff> <folder for the frames> [--frames <count>] [--repeat <times>]\n"
                    "  --repeat plays the frames again and again, saving the pictures only the first time, to time them.\n", argv[0]);
    return EXIT_USAGE;
  }
  Dff *dff = dff_load(argv[1]);
  printf("%s: version %u, %s, game %s, memory %u + %u MiB, %u frames\n", argv[1], dff->version, dff->flags & DFF_FLAG_WII ? "Wii" : "GameCube",
         dff->game_id[0] ? dff->game_id : "unknown", dff->mem1_bytes >> 20, dff->mem2_bytes >> 20, dff->frame_count);
  if (dff->mem1_bytes > MEM1_SIZE || dff->mem2_bytes > MEM2_SIZE) stop_because(argv[1], "it was recorded with more memory than a Wii has");
  if (!dff->frame_count) stop_because(argv[1], "it holds no frames");
  make_folder(argv[2]);
  guest_memory_init();
  if (!mem1 || !mem2) stop_because("no memory for the Wii's", strerror(errno));

  gx_backend = &gx_backend_vulkan;
  vk_picture_sink = keep_picture;
  gx_backend->window_ready(0);
  load_registers(dff);
  clear_whole_picture(dff);

  uint32_t frames = dff->frame_count < limit ? dff->frame_count : limit, saved = 0, no_copy = 0, several = 0;
  for (uint32_t i = 0; i < frames; i++) {
    pictures_in_frame = 0;
    play_frame(&dff->frames[i]);
    if (pictures_in_frame > 1) several++;
    if (!pictures_in_frame) no_copy++;
    // With no new picture, a TV goes on showing the last one.
    if (!picture) {
      fprintf(stderr, "frame %u: nothing to save yet, as no copy has reached the screen\n", i);
      continue;
    }
    char path[4096];
    if (snprintf(path, sizeof path, "%s/frame-%0*u.png", argv[2], FRAME_NAME_DIGITS, i) >= (int)sizeof path) stop_because("folder name too long", argv[2]);
    save_png(path);
    saved++;
  }
  // Each turn after the first starts from the recording's memory and registers again, so
  // every turn draws the same. The first frame of a turn also brings all memory in.
  // Pictures aren't read back while timing, as a game never waits for that.
  double slowest = 0, total = 0, working = 0;
  uint32_t timed = 0;
  vk_picture_sink = 0;
  for (uint32_t turn = 1; turn < repeats; turn++) {
    load_registers(dff);
    for (uint32_t i = 0; i < frames; i++) {
      double began = now_ms(), began_working = clock_ms(CLOCK_THREAD_CPUTIME_ID), memory_before = memory_ms;
      play_frame(&dff->frames[i]);
      double took = now_ms() - began - (memory_ms - memory_before);
      if (i == 0) continue;
      total += took, timed++;
      working += clock_ms(CLOCK_THREAD_CPUTIME_ID) - began_working - (memory_ms - memory_before);
      if (took > slowest) slowest = took;
    }
  }
  // Working time leaves out waiting for the graphics chip.
  if (timed) {
    printf("timed %u frames: %.2f ms each on average, %.2f ms at most, %.2f ms of it working, first frames of each turn and putting memory in left out\n",
           timed, total / timed, slowest, working / timed);
  }
  printf("saved %u of %u frames at %dx%d: %u draws, %u copies to the screen, %u frames with none, %u with several\n", saved, frames, picture_width,
         picture_height, gx_stats.draws, gx_stats.to_screen, no_copy, several);
  return 0;
}
