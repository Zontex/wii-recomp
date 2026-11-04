// Picture copies. The game draws a scene, then copies the picture into memory to use as
// a texture. Here the picture stays on the graphics card, filed by its address, and the
// memory it would have landed in gets a sparse stamp in its place. A texture may use the
// card's copy only while the stamp is whole and its shape fits, as Dolphin does with its
// "EFB copies to texture only", which hashes memory it clears. Otherwise memory is read.
#include <string.h>

#include "cpu.h"
#include "debug.h"
#include "gaps.h"
#include "gx_backend.h"

enum {
  BP_COPY_ORIGIN = 0x49, BP_COPY_SIZE = 0x4a, BP_COPY_TARGET = 0x4b, BP_COPY_STRIDE = 0x4d, MAX_COPIES = 512,
  BP_TEXTURE_IMAGE = 0x88, BP_TEXTURE_IMAGE_HIGH = 0xa8, BP_TEXTURE_ADDRESS = 0x94, BP_TEXTURE_ADDRESS_HIGH = 0xb4,
  HALF_SIZE_BIT = 9, FORMAT_SHIFT = 3, FORMAT_MASK = 15, COORDINATE_MASK = 0x3ff, COORDINATE_BITS = 10, TEXTURE_SIZE_MASK = 0x3ff,
  TEXTURE_FORMAT_SHIFT = 20, TEXTURE_FORMAT_MASK = 15, BLOCK_BYTES = 32, ADDRESS_SHIFT = 5,
  COPY_R4 = 0, COPY_R8_LOW = 1, COPY_RA4 = 2, COPY_RA8 = 3, COPY_RGB565 = 4, COPY_RGB5A3 = 5, COPY_RGBA8 = 6, COPY_A8 = 7, COPY_R8 = 8, COPY_G8 = 9, COPY_B8 = 10,
  COPY_RG8 = 11, COPY_GB8 = 12, COPY_FORMATS = 16,
  TEXTURE_I4 = 0, TEXTURE_I8 = 1, TEXTURE_IA4 = 2, TEXTURE_IA8 = 3, TEXTURE_RGB565 = 4, TEXTURE_RGB5A3 = 5, TEXTURE_RGBA8 = 6, TEXTURE_C4 = 8, TEXTURE_C8 = 9,
  TEXTURE_C14X2 = 10, TEXTURE_CMPR = 14, TEXTURE_FORMATS = 16,
  STAMP_WORDS = 256, STAMP_COLUMN_STEP = 37,
};
#define STAMP_MIX 0x9e3779b1u

typedef struct {
  uint32_t address;
  GxTexture *texture, *depth_texture;
  int from_depth, live;
  uint32_t width, height, format, row_bytes, rows, stride, stamp;
} Copy;

static Copy copies[MAX_COPIES];
static int copy_count;
static uint32_t copies_made;

// What each copy format lands in memory as, which is always one of the texture formats.
static const uint8_t base_formats[COPY_FORMATS] = {
  [COPY_R4] = TEXTURE_I4, [COPY_R8_LOW] = TEXTURE_I8, [COPY_RA4] = TEXTURE_IA4, [COPY_RA8] = TEXTURE_IA8, [COPY_RGB565] = TEXTURE_RGB565,
  [COPY_RGB5A3] = TEXTURE_RGB5A3, [COPY_RGBA8] = TEXTURE_RGBA8, [COPY_A8] = TEXTURE_I8, [COPY_R8] = TEXTURE_I8, [COPY_G8] = TEXTURE_I8, [COPY_B8] = TEXTURE_I8,
  [COPY_RG8] = TEXTURE_IA8, [COPY_GB8] = TEXTURE_IA8, [13] = TEXTURE_RGBA8, [14] = TEXTURE_RGBA8, [15] = TEXTURE_RGBA8,
};
static const uint8_t texel_bits[TEXTURE_FORMATS] = {
  [TEXTURE_I4] = 4, [TEXTURE_I8] = 8, [TEXTURE_IA4] = 8, [TEXTURE_IA8] = 16, [TEXTURE_RGB565] = 16, [TEXTURE_RGB5A3] = 16, [TEXTURE_RGBA8] = 32,
  [TEXTURE_C4] = 4, [TEXTURE_C8] = 8, [TEXTURE_C14X2] = 16, [TEXTURE_CMPR] = 4,
};

static uint32_t copy_format(uint32_t control) {
  uint32_t code = (control >> FORMAT_SHIFT) & FORMAT_MASK;
  return code / 2 + (code & 1) * 8;
}

static Copy *copy_for(uint32_t address) {
  for (int i = 0; i < copy_count; i++) {
    if (copies[i].address == address) return &copies[i];
  }
  if (copy_count == MAX_COPIES) return &copies[0];
  Copy *copy = &copies[copy_count++];
  copy->address = address;
  copy->texture = gx_backend->texture_create();
  copy->depth_texture = gx_backend->texture_create();
  return copy;
}

// The words reach across every row and column. Each word's value depends on where it
// is, so in a small copy, where places repeat, a place still has only one right answer.
static uint8_t *stamp_word(const Copy *copy, uint32_t i, uint32_t *value) {
  uint32_t row = i * copy->rows / STAMP_WORDS, column = i * STAMP_COLUMN_STEP * BLOCK_BYTES % copy->row_bytes;
  uint32_t offset = row * copy->stride + column;
  *value = (copy->stamp + offset) * STAMP_MIX;
  return host(((copy->address << ADDRESS_SHIFT) + offset) | 0x80000000u);
}

static void stamp_memory(const Copy *copy) {
  for (uint32_t i = 0; i < STAMP_WORDS; i++) {
    uint32_t value;
    uint8_t *word = stamp_word(copy, i, &value);
    if (word) memcpy(word, &value, sizeof value);
  }
}

static int stamp_is_whole(const Copy *copy) {
  for (uint32_t i = 0; i < STAMP_WORDS; i++) {
    uint32_t value;
    const uint8_t *word = stamp_word(copy, i, &value);
    if (word && memcmp(word, &value, sizeof value)) return 0;
  }
  return 1;
}

static uint32_t copy_start(const Copy *copy) { return copy->address << ADDRESS_SHIFT; }
static uint32_t copy_end(const Copy *copy) { return copy_start(copy) + copy->rows * copy->stride; }

// A new copy lands on any older one it touches, as it would in memory.
static void drop_overlapping(const Copy *copy) {
  for (int i = 0; i < copy_count; i++) {
    Copy *other = &copies[i];
    if (other != copy && other->live && copy_start(other) < copy_end(copy) && copy_start(copy) < copy_end(other)) other->live = 0;
  }
}

// Single-channel copies repeat that channel, as the game reads them as intensity.
static GxChannels channels_for(uint32_t control) {
  uint32_t format = copy_format(control);
  GxChannels channels = {GX_CHANNEL_RED, GX_CHANNEL_GREEN, GX_CHANNEL_BLUE, GX_CHANNEL_ALPHA};
  if (format == COPY_R4 || format == COPY_R8_LOW || format == COPY_R8) channels.green = channels.blue = channels.alpha = GX_CHANNEL_RED;
  else if (format == COPY_RA4 || format == COPY_RA8) channels.green = channels.blue = GX_CHANNEL_RED;
  else if (format == COPY_A8) channels.red = channels.green = channels.blue = GX_CHANNEL_ALPHA;
  else if (format == COPY_G8) channels.red = channels.blue = channels.alpha = GX_CHANNEL_GREEN;
  else if (format == COPY_B8) channels.red = channels.green = channels.alpha = GX_CHANNEL_BLUE;
  // A picture with no alpha copies it as full, as in Dolphin's TextureConverterShaderGen.cpp.
  if (!gx_target_has_alpha()) {
    uint8_t *each[4] = {&channels.red, &channels.green, &channels.blue, &channels.alpha};
    for (int i = 0; i < 4; i++) {
      if (*each[i] == GX_CHANNEL_ALPHA) *each[i] = GX_CHANNEL_ONE;
    }
  }
  return channels;
}

void gx_copy_rectangle(int *x, int *y, int *width, int *height) {
  uint32_t origin = gx.bp[BP_COPY_ORIGIN], size = gx.bp[BP_COPY_SIZE];
  *x = (int)(origin & COORDINATE_MASK);
  *y = (int)((origin >> COORDINATE_BITS) & COORDINATE_MASK);
  *width = (int)(size & COORDINATE_MASK) + 1;
  *height = (int)((size >> COORDINATE_BITS) & COORDINATE_MASK) + 1;
}

// Works out the shape a copy has in memory. Says no if it has none.
static int fit_copy(Copy *copy, uint32_t control, uint32_t width, uint32_t height, uint32_t stride) {
  uint32_t format = base_formats[copy_format(control)], bits = texel_bits[format];
  if (copy_format(control) == COPY_RG8 || copy_format(control) == COPY_GB8) gap_hit("pixel.copy_formats_rg8_gb8", copy_format(control));
  uint32_t block_width = bits <= 8 ? 8 : 4, block_height = bits == 4 ? 8 : 4;
  uint32_t block_bytes = format == TEXTURE_RGBA8 ? BLOCK_BYTES * 2 : BLOCK_BYTES;
  copy->format = format;
  copy->width = width;
  copy->height = height;
  copy->stride = stride;
  copy->rows = (height + block_height - 1) / block_height;
  copy->row_bytes = (width + block_width - 1) / block_width * block_bytes;
  return copy->rows && copy->row_bytes;
}

void gx_copy_to_texture(uint32_t control, int from_depth) {
  GxRectangle from;
  gx_copy_rectangle(&from.x, &from.y, &from.width, &from.height);
  // Dolphin's hardware tests: a copy that starts past the picture has nothing to copy, one
  // running past its edge is cut there, and with a stride of 0 nothing is copied at all.
  uint32_t stride = gx.bp[BP_COPY_STRIDE] << ADDRESS_SHIFT;
  if (from.x >= EFB_WIDTH || from.y >= EFB_HEIGHT || !stride) return;
  if (from.x + from.width > EFB_WIDTH) from.width = EFB_WIDTH - from.x;
  if (from.y + from.height > EFB_HEIGHT) from.height = EFB_HEIGHT - from.y;
  int half = (control >> HALF_SIZE_BIT) & 1;
  uint32_t copy_width = (uint32_t)(half ? from.width / 2 : from.width), copy_height = (uint32_t)(half ? from.height / 2 : from.height);
  Copy *copy = copy_for(gx.bp[BP_COPY_TARGET] & 0xffffff);
  copy->live = 0;
  if (!fit_copy(copy, control, copy_width, copy_height, stride)) return;
  if (stride < copy->row_bytes) {
    debug_unsupported("gx.0x4d_copy_stride", "picture copy with a stride below its row", stride);
    return;
  }
  // Copies keep the buffer's full detail. Shrinking them would blend the edges of a
  // cut-out picture with whatever color surrounds it.
  int width = (int)copy_width * EFB_SCALE, height = (int)copy_height * EFB_SCALE;
  copy->from_depth = from_depth;
  if (from_depth) gx_backend->copy_depth(copy->depth_texture, &from, width, height);
  else gx_backend->copy_color(copy->texture, &from, width, height, channels_for(control), half);
  copy->stamp = ++copies_made;
  copy->live = 1;
  drop_overlapping(copy);
  stamp_memory(copy);
}

// The copy a map's texture could be. Whatever shape a texture has, the first question is
// whether the copy is still in memory, as a new texture there is no concern of ours. If
// it is, the game is reading the copy some other way, which only real pixels could show.
static Copy *usable_copy(int map) {
  uint32_t address = gx.bp[map < 4 ? BP_TEXTURE_ADDRESS + map : BP_TEXTURE_ADDRESS_HIGH + map - 4] & 0xffffff;
  uint32_t image = gx.bp[map < 4 ? BP_TEXTURE_IMAGE + map : BP_TEXTURE_IMAGE_HIGH + map - 4];
  for (int i = 0; i < copy_count; i++) {
    Copy *copy = &copies[i];
    if (copy->address != address || !copy->live) continue;
    if (!stamp_is_whole(copy)) {
      copy->live = 0;
      return 0;
    }
    uint32_t format = (image >> TEXTURE_FORMAT_SHIFT) & TEXTURE_FORMAT_MASK;
    uint32_t width = (image & TEXTURE_SIZE_MASK) + 1, height = ((image >> COORDINATE_BITS) & TEXTURE_SIZE_MASK) + 1;
    if (format == TEXTURE_CMPR || texel_bits[format] != texel_bits[copy->format]) {
      debug_unsupported("vtx.copies_as_textures", "picture copy read as a format of another size", (copy->format << 8) | format);
      return 0;
    }
    if (width != copy->width || height != copy->height) {
      debug_unsupported("vtx.copies_as_textures", "picture copy read at another size", (width << 16) | height);
      return 0;
    }
    if (copy->stride != copy->row_bytes) {
      debug_unsupported("vtx.copies_as_textures", "picture copy with gaps between its rows read whole", copy->stride);
      return 0;
    }
    if (format == TEXTURE_C4 || format == TEXTURE_C8 || format == TEXTURE_C14X2) debug_unsupported("vtx.copies_as_textures", "picture copy read through a palette", format);
    else if (format != copy->format) debug_unsupported("vtx.copies_as_textures", "picture copy read as another format", (copy->format << 8) | format);
    return copy;
  }
  return 0;
}

GxTexture *gx_copy_find(int map) {
  Copy *copy = usable_copy(map);
  if (!copy) return 0;
  return copy->from_depth ? copy->depth_texture : copy->texture;
}

int gx_copy_is_depth(int map) {
  Copy *copy = usable_copy(map);
  return copy && copy->from_depth;
}
