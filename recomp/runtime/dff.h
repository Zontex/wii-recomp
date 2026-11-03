// Dolphin's FIFO recordings (.dff): the chip's state at the start, then for each frame
// the commands sent and the memory they used. See tools/replay, which plays them back.
#pragma once
#include <stdint.h>

enum {
  DFF_BP_WORDS = 256, DFF_CP_WORDS = 256, DFF_XF_WORDS = 4096, DFF_XF_REGISTER_WORDS = 88,
  DFF_TEXTURE_BYTES = 1024 * 1024, DFF_GAME_ID_BYTES = 8, DFF_FLAG_WII = 1,
};

// Why memory was recorded. Replaying treats them all the same.
enum { DFF_UPDATE_TEXTURE = 1, DFF_UPDATE_XF = 2, DFF_UPDATE_VERTICES = 4, DFF_UPDATE_TEXTURE_MEMORY = 8 };

typedef struct {
  uint32_t fifo_position;  // Goes into memory once the frame's commands reach here.
  uint32_t address, bytes;
  uint8_t type;
  const uint8_t *data;
} DffUpdate;

typedef struct {
  const uint8_t *fifo;
  uint32_t fifo_bytes, fifo_start, fifo_end;
  uint32_t update_count;
  DffUpdate *updates;  // In order of fifo_position.
} DffFrame;

typedef struct {
  uint32_t version, flags, mem1_bytes, mem2_bytes;
  char game_id[DFF_GAME_ID_BYTES + 1];
  uint32_t bp[DFF_BP_WORDS], cp[DFF_CP_WORDS], xf[DFF_XF_WORDS], xf_registers[DFF_XF_REGISTER_WORDS];
  uint8_t texture_memory[DFF_TEXTURE_BYTES];
  uint32_t frame_count;
  DffFrame *frames;
} Dff;

// Reads a whole file. Stops the program with a message if it can't be used. What it
// returns lasts as long as the program, and frames point into the file's own bytes.
Dff *dff_load(const char *path);
