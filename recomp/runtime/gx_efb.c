// The processor reading and writing the picture directly: EFB peeks and pokes, at
// 0x08000000 to 0x08ffffff. Each word is one pixel: x in bits 2 to 11, y in bits 12 to
// 21, and bit 22 picks depth over color, as Dolphin's MMU.cpp decodes it (CREDITS.md).
//
// A peek waits for the card. The first after anything changed the picture reads back the
// block of 32 by 32 pixels around it: Super Mario Galaxy 2 peeks once or twice a frame,
// and reading the whole picture cost the Pi's card 1.9 ms of each. A second that falls
// outside reads the whole picture, as games that peek many pixels at a time do.
//
// Galaxy 2 peeks as each frame begins, before drawing anything, at the depth of the frame
// just shown. Such a read is told to the backend, which may make the next one early, as
// soon as that part of the picture stops changing, so the peek doesn't wait for the work after.
#include <stdlib.h>
#include <string.h>

#include "gaps.h"
#include "gx_backend.h"
#include "gx_encode.h"
#include "hardware.h"

enum {
  EFB_SPACE_MASK = 0x0f000000u, EFB_SPACE = 0x08000000u, DEPTH_BIT = 0x00400000u, BOTH_BIT = 0x00800000u,
  PE_Z_CONFIG = 0xcc001000u, PE_ALPHA_CONFIG = 0xcc001002u, PE_ALPHA_READ = 0xcc001008u,
  BP_PIXEL_FORMAT = 0x43, PIXEL_FORMAT_MASK = 7, DEPTH_FORMAT_SHIFT = 3,
  Z_COMPARE_BIT = 0, Z_FUNCTION_SHIFT = 1, Z_ALWAYS = 7, Z_UPDATE_BIT = 4,
  BLEND_BIT = 0, LOGIC_BIT = 1, COLOR_UPDATE_BIT = 3, ALPHA_UPDATE_BIT = 4,
};

static unsigned changes = 1;  // Anything drawn, cleared or poked since the last read back.
enum { PEEK_BLOCK = 32 };
static struct { unsigned seen; GxRectangle area; uint32_t pixels[EFB_WIDTH * EFB_HEIGHT]; } held[2];

static int drawn;  // Anything since the last picture was shown.
void gx_efb_changed(void) { changes++, drawn = 1; }
void gx_efb_shown(void) { changes++, drawn = 0; }

typedef struct { int depth, x, y; uint32_t value; } Access;

static void peek_work(void *argument) {
  Access *access = argument;
  int kind = access->depth;
  GxRectangle *area = &held[kind].area;
  int inside = held[kind].seen == changes && access->x >= area->x && access->y >= area->y && access->x < area->x + area->width
               && access->y < area->y + area->height;
  if (!inside) {
    GxRectangle want = {0, 0, EFB_WIDTH, EFB_HEIGHT};
    if (held[kind].seen != changes) {  // The first peek since a change: just its block.
      want.x = access->x & ~(PEEK_BLOCK - 1), want.y = access->y & ~(PEEK_BLOCK - 1);
      want.width = want.x + PEEK_BLOCK <= EFB_WIDTH ? PEEK_BLOCK : EFB_WIDTH - want.x;
      want.height = want.y + PEEK_BLOCK <= EFB_HEIGHT ? PEEK_BLOCK : EFB_HEIGHT - want.y;
    }
    unsigned ticket = gx_backend->read_picture ? gx_backend->read_picture(kind, &want) : 0;
    int width = 0, height = 0;
    const uint8_t *read = ticket ? gx_backend->read_finish(ticket, &width, &height) : 0;
    if (!read || width != want.width || height != want.height) {
      gap_hit("pixel.efb_peek_cpu_reads_color_depth", 0x100u | (unsigned)kind);
      access->value = 0;
      return;
    }
    for (int row = 0; row < height; row++) {
      memcpy(&held[kind].pixels[(want.y + row) * EFB_WIDTH + want.x], read + (size_t)row * (size_t)width * 4, (size_t)width * 4);
    }
    held[kind].seen = changes;
    *area = want;
    if (!drawn && gx_backend->expect_read) gx_backend->expect_read(kind, &want);
  }
  uint32_t stored = held[kind].pixels[access->y * EFB_WIDTH + access->x], format = gx.bp[BP_PIXEL_FORMAT];
  if (kind) {
    float depth;
    memcpy(&depth, &stored, sizeof depth);
    access->value = gx_peek_depth_value(depth, (int)(format & PIXEL_FORMAT_MASK), (int)((format >> DEPTH_FORMAT_SHIFT) & 7));
  } else {
    access->value = gx_peek_color_value(stored, (int)(format & PIXEL_FORMAT_MASK), (int)(hw_peek(PE_ALPHA_READ, 2) & 3));
  }
}

// Pokes go straight in. The PE's poke settings could also test depth, blend or leave
// channels alone, which Dolphin doesn't follow either, so those are named.
static void poke_work(void *argument) {
  Access *access = argument;
  if (access->depth) {
    uint32_t z = hw_peek(PE_Z_CONFIG, 2);
    if (((z >> Z_COMPARE_BIT) & 1 && ((z >> Z_FUNCTION_SHIFT) & 7) != Z_ALWAYS) || !((z >> Z_UPDATE_BIT) & 1)) gap_hit("gx.pe_z_alpha_destination_alpha_config", z);
  } else {
    uint32_t alpha = hw_peek(PE_ALPHA_CONFIG, 2);
    if ((alpha >> BLEND_BIT) & 1 || (alpha >> LOGIC_BIT) & 1 || !((alpha >> COLOR_UPDATE_BIT) & 1) || !((alpha >> ALPHA_UPDATE_BIT) & 1)) {
      gap_hit("gx.pe_z_alpha_destination_alpha_config", alpha | 0x10000u);
    }
  }
  if (!gx_backend->poke) {
    gap_hit("pixel.efb_poke_cpu_writes", 0x100u | (unsigned)access->depth);
    return;
  }
  gx_backend->poke(access->depth, access->x, access->y, gx_poke_color_value(access->value), (float)gx_poke_depth_value(access->value));
  gx_efb_changed();
}

// Says no for an address outside the picture's space. Outside the picture itself, a
// peek reads 0 and a poke does nothing, as in Dolphin.
static int decode(uint32_t address, int bytes, int writing, Access *access) {
  if ((address & EFB_SPACE_MASK) != EFB_SPACE) return 0;
  access->x = (int)((address & 0xfff) >> 2);
  access->y = (int)((address >> 12) & 0x3ff);
  access->depth = (address & DEPTH_BIT) != 0;
  if (address & BOTH_BIT) {  // Color and depth at once, which nothing has found the meaning of.
    gap_hit(writing ? "pixel.efb_poke_cpu_writes" : "pixel.efb_peek_cpu_reads_color_depth", address);
    return -1;
  }
  if (bytes != 4) gap_hit(writing ? "pixel.efb_poke_cpu_writes" : "pixel.efb_peek_cpu_reads_color_depth", 0x200u | (unsigned)bytes);
  return access->x < EFB_WIDTH && access->y < EFB_HEIGHT ? 1 : -1;
}

int gx_efb_read(uint32_t address, int bytes, uint32_t *value) {
  Access access;
  int found = decode(address, bytes, 0, &access);
  if (!found) return 0;
  *value = 0;
  if (found < 0 || !gx_draw_started()) return 1;
  gx_run_on_graphics(peek_work, &access);
  // A narrower read takes the word's low bytes, as Dolphin's hardware reads do.
  *value = bytes == 4 ? access.value : bytes == 2 ? access.value & 0xffff : access.value & 0xff;
  return 1;
}

int gx_efb_write(uint32_t address, uint32_t value, int bytes) {
  Access access;
  int found = decode(address, bytes, 1, &access);
  if (found <= 0 || !gx_draw_started()) return found != 0;
  access.value = value;
  gx_run_on_graphics(poke_work, &access);
  return 1;
}

// The bounding box: the smallest rectangle around every pixel drawn since the game set
// it, with BP 0x55 holding left and right and 0x56 top and bottom, ten bits each. Reading
// PE 0x10 to 0x16 waits for the card and turns it off, as in Dolphin's PixelEngine.cpp.
enum { BP_BOX_FIRST = 0x55, PE_BOX_FIRST = 0xcc001010u, PE_BOX_END = 0xcc001018u, BOX_BITS = 10, BOX_MASK = 0x3ff };
int gx_bounding_box_on;
static int box[4];

// Before anything is drawn the backend hasn't started: the values wait for it.
static int box_waiting;

static void box_send(int first) {
  if (gx_backend->bounding_box_set && gx_backend->bounding_box_set(first, box[first], box[first + 1])) {
    gx_bounding_box_on = 1;
    gx_settings_written++;  // Shaders change to widen it.
  } else {
    gap_hit("pixel.bounding_box", (uint32_t)box[first] | (uint32_t)box[first + 1] << BOX_BITS);
  }
}

void gx_bounding_box_write(int reg, uint32_t data) {
  int first = reg == BP_BOX_FIRST ? 0 : 2;
  box[first] = (int)(data & BOX_MASK);
  box[first + 1] = (int)((data >> BOX_BITS) & BOX_MASK);
  if (gx_draw_started()) box_send(first);
  else box_waiting |= 1 << first;
}

void gx_bounding_box_started(void) {
  for (int first = 0; first < 4; first += 2) {
    if (box_waiting & (1 << first)) box_send(first);
  }
  box_waiting = 0;
}

void gx_bounding_box_off(void) {
  if (!gx_bounding_box_on) return;
  gx_bounding_box_on = 0;
  gx_settings_written++;
}

static void box_work(void *argument) {
  (void)argument;
  if (gx_bounding_box_on) gx_backend->bounding_box_get(box);
  gx_bounding_box_off();
}

int gx_bounding_box_read(uint32_t address, uint32_t *value) {
  if (address < PE_BOX_FIRST || address >= PE_BOX_END) return 0;
  gx_run_on_graphics(box_work, 0);
  *value = (uint32_t)box[(address - PE_BOX_FIRST) / 2] & 0xffff;
  return 1;
}
