// Textures: turns each of the Wii's image formats into RGBA.
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "gx.h"
#include "memory_watch.h"

enum {
  FORMAT_I4 = 0, FORMAT_I8 = 1, FORMAT_IA4 = 2, FORMAT_IA8 = 3, FORMAT_RGB565 = 4, FORMAT_RGB5A3 = 5,
  FORMAT_RGBA8 = 6, FORMAT_C4 = 8, FORMAT_C8 = 9, FORMAT_C14X2 = 10, FORMAT_CMPR = 14, PALETTE_INDEX_14 = 0x3fff,
  BP_IMAGE_LOW = 0x88, BP_IMAGE_HIGH = 0xa8, BP_ADDRESS_LOW = 0x94, BP_ADDRESS_HIGH = 0xb4,
  BP_PALETTE_LOW = 0x98, BP_PALETTE_HIGH = 0xb8, BP_PALETTE_SOURCE = 0x64, BP_PALETTE_LOAD = 0x65,
  PALETTE_BYTES = 0x80000, PALETTE_FORMAT_IA8 = 0, PALETTE_FORMAT_RGB565 = 1, PALETTE_SLOT_SHIFT = 9,
  BP_MODE_LOW = 0x80, BP_MODE_HIGH = 0xa0, BP_LIMITS_LOW = 0x84, BP_LIMITS_HIGH = 0xa4, MIP_FILTER_SHIFT = 5, MIP_FILTER_MASK = 3,
  MAX_LEVEL_SHIFT = 8, LEVEL_MASK = 255, LEVEL_STEPS = 16, RGBA_BYTES = 4,
};

static uint8_t palette_memory[PALETTE_BYTES];
static uint8_t *pixels;
static uint32_t pixels_size;

void gx_load_palette(void) {
  uint32_t source = (gx.bp[BP_PALETTE_SOURCE] << 5) | 0x80000000u, load = gx.bp[BP_PALETTE_LOAD];
  uint32_t offset = (load & 0x3ff) << PALETTE_SLOT_SHIFT, bytes = ((load >> 10) & 0x7ff) * 32;
  uint8_t *from = host(source);
  if (from && offset + bytes <= PALETTE_BYTES) memcpy(palette_memory + offset, from, bytes);
}

// Fills palette memory from a copy of the chip's texture memory, as a recording holds.
// Dolphin keeps palettes in the first half of it, at the same places as here.
void gx_restore_palettes(const uint8_t *texture_memory) { memcpy(palette_memory, texture_memory, PALETTE_BYTES); }

static void put(uint8_t *to, int r, int g, int b, int a) {
  to[0] = (uint8_t)r; to[1] = (uint8_t)g; to[2] = (uint8_t)b; to[3] = (uint8_t)a;
}

static void rgb5a3(uint32_t v, uint8_t *to) {
  if (v & 0x8000) {
    put(to, (int)((v >> 10) & 31) * 255 / 31, (int)((v >> 5) & 31) * 255 / 31, (int)(v & 31) * 255 / 31, 255);
  } else {
    put(to, (int)((v >> 8) & 15) * 17, (int)((v >> 4) & 15) * 17, (int)(v & 15) * 17, (int)((v >> 12) & 7) * 255 / 7);
  }
}

static void rgb565(uint32_t v, uint8_t *to) {
  put(to, (int)(v >> 11) * 255 / 31, (int)((v >> 5) & 63) * 255 / 63, (int)(v & 31) * 255 / 31, 255);
}

static void palette_color(uint32_t index, uint32_t offset, int format, uint8_t *to) {
  const uint8_t *p = palette_memory + ((offset + index * 2) & (PALETTE_BYTES - 1));
  uint32_t v = ((uint32_t)p[0] << 8) | p[1];
  if (format == PALETTE_FORMAT_IA8) put(to, (int)(v & 255), (int)(v & 255), (int)(v & 255), (int)(v >> 8));
  else if (format == PALETTE_FORMAT_RGB565) rgb565(v, to);
  else rgb5a3(v, to);
}

// CMPR blocks: four 4x4 tiles, each two colors and two bits per pixel.
static void cmpr_tile(const uint8_t *s, uint8_t *out, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  uint32_t c0 = ((uint32_t)s[0] << 8) | s[1], c1 = ((uint32_t)s[2] << 8) | s[3];
  uint8_t colors[4][4];
  rgb565(c0, colors[0]);
  rgb565(c1, colors[1]);
  for (int i = 0; i < 3; i++) {
    if (c0 > c1) {
      colors[2][i] = (uint8_t)((2 * colors[0][i] + colors[1][i]) / 3);
      colors[3][i] = (uint8_t)((colors[0][i] + 2 * colors[1][i]) / 3);
    } else {
      colors[2][i] = (uint8_t)((colors[0][i] + colors[1][i]) / 2);
      colors[3][i] = 0;
    }
  }
  colors[2][3] = 255;
  colors[3][3] = c0 > c1 ? 255 : 0;
  for (uint32_t row = 0; row < 4; row++) {
    for (uint32_t column = 0; column < 4; column++) {
      if (x + column >= width || y + row >= height) continue;
      int index = (s[4 + row] >> (6 - column * 2)) & 3;
      memcpy(out + ((y + row) * width + x + column) * 4, colors[index], 4);
    }
  }
}

// One format's tiles. Always inlined with `format` a constant, so each format gets a loop
// of its own instead of choosing per pixel; games decode whole movie frames this way.
static inline __attribute__((always_inline)) void decode_tiles(const uint8_t *s, const int format, uint32_t width,
                                                               uint32_t height, uint32_t palette, int palette_format,
                                                               uint8_t *out) {
  static const uint8_t tile_w[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
  static const uint8_t tile_h[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
  static const uint8_t tile_bytes[16] = {32, 32, 32, 32, 32, 32, 64, 0, 32, 32, 32, 0, 0, 0, 32, 0};
  const uint32_t tw = tile_w[format], th = tile_h[format];
  for (uint32_t y = 0; y < height; y += th) {
    uint32_t rows = height - y < th ? height - y : th;
    for (uint32_t x = 0; x < width; x += tw, s += tile_bytes[format]) {
      uint32_t columns = width - x < tw ? width - x : tw;
      const uint8_t *green_blue = s + 32;  // RGBA8: a tile's alpha and red, then its green and blue.
      for (uint32_t row = 0; row < rows; row++) {
        uint8_t *pixel = out + ((y + row) * width + x) * 4;
        for (uint32_t column = 0; column < columns; column++, pixel += 4) {
          uint32_t index = row * tw + column;
          switch (format) {
            case FORMAT_I4: {
              int v = (s[index / 2] >> (index & 1 ? 0 : 4)) & 15;
              put(pixel, v * 17, v * 17, v * 17, v * 17);
              break;
            }
            case FORMAT_I8: put(pixel, s[index], s[index], s[index], s[index]); break;
            case FORMAT_IA4: put(pixel, (s[index] & 15) * 17, (s[index] & 15) * 17, (s[index] & 15) * 17, (s[index] >> 4) * 17); break;
            case FORMAT_IA8: put(pixel, s[index * 2 + 1], s[index * 2 + 1], s[index * 2 + 1], s[index * 2]); break;
            case FORMAT_RGB565: rgb565(((uint32_t)s[index * 2] << 8) | s[index * 2 + 1], pixel); break;
            case FORMAT_RGB5A3: rgb5a3(((uint32_t)s[index * 2] << 8) | s[index * 2 + 1], pixel); break;
            case FORMAT_RGBA8: put(pixel, s[index * 2 + 1], green_blue[index * 2], green_blue[index * 2 + 1], s[index * 2]); break;
            case FORMAT_C4: palette_color((s[index / 2] >> (index & 1 ? 0 : 4)) & 15u, palette, palette_format, pixel); break;
            case FORMAT_C8: palette_color(s[index], palette, palette_format, pixel); break;
            case FORMAT_C14X2:
              palette_color((((uint32_t)s[index * 2] << 8) | s[index * 2 + 1]) & PALETTE_INDEX_14, palette, palette_format, pixel);
              break;
            default: put(pixel, 0, 0, 0, 0); break;
          }
        }
      }
    }
  }
}

static void decode(const uint8_t *s, int format, uint32_t width, uint32_t height, uint32_t palette, int palette_format, uint8_t *out) {
  switch (format) {
    case FORMAT_CMPR:
      for (uint32_t y = 0; y < height; y += 8) {
        for (uint32_t x = 0; x < width; x += 8) {
          for (uint32_t b = 0; b < 4; b++, s += 8) cmpr_tile(s, out, x + (b & 1) * 4, y + (b >> 1) * 4, width, height);
        }
      }
      break;
    case FORMAT_I4: decode_tiles(s, FORMAT_I4, width, height, palette, palette_format, out); break;
    case FORMAT_I8: decode_tiles(s, FORMAT_I8, width, height, palette, palette_format, out); break;
    case FORMAT_IA4: decode_tiles(s, FORMAT_IA4, width, height, palette, palette_format, out); break;
    case FORMAT_IA8: decode_tiles(s, FORMAT_IA8, width, height, palette, palette_format, out); break;
    case FORMAT_RGB565: decode_tiles(s, FORMAT_RGB565, width, height, palette, palette_format, out); break;
    case FORMAT_RGB5A3: decode_tiles(s, FORMAT_RGB5A3, width, height, palette, palette_format, out); break;
    case FORMAT_RGBA8: decode_tiles(s, FORMAT_RGBA8, width, height, palette, palette_format, out); break;
    case FORMAT_C4: decode_tiles(s, FORMAT_C4, width, height, palette, palette_format, out); break;
    case FORMAT_C8: decode_tiles(s, FORMAT_C8, width, height, palette, palette_format, out); break;
    case FORMAT_C14X2: decode_tiles(s, FORMAT_C14X2, width, height, palette, palette_format, out); break;
    default: break;  // Left as the zeros it was cleared to.
  }
}

// Rows and columns of an image come in whole tiles, so memory holds that much of each level.
static uint32_t image_bytes(int format, uint32_t width, uint32_t height) {
  static const uint8_t bits[16] = {4, 8, 8, 16, 16, 16, 32, 0, 4, 8, 16, 0, 0, 0, 4, 0};
  uint32_t tile_width = bits[format] <= 8 ? 8 : 4, tile_height = bits[format] == 4 ? 8 : 4;
  uint32_t aligned_w = (width + tile_width - 1) & ~(tile_width - 1), aligned_h = (height + tile_height - 1) & ~(tile_height - 1);
  return aligned_w * aligned_h * bits[format] / 8;
}

static uint32_t level_size(uint32_t size, uint32_t level) { return size >> level ? size >> level : 1; }

// With a mip filter on, smaller versions follow the image in memory, as many as the
// largest level of detail reaches and the size allows. See Dolphin's TextureInfo.
static uint32_t stored_levels(int map, uint32_t width, uint32_t height) {
  uint32_t mode = gx.bp[map < 4 ? BP_MODE_LOW + map : BP_MODE_HIGH + map - 4], limits = gx.bp[map < 4 ? BP_LIMITS_LOW + map : BP_LIMITS_HIGH + map - 4];
  if (!((mode >> MIP_FILTER_SHIFT) & MIP_FILTER_MASK)) return 1;
  uint32_t wanted = ((limits >> MAX_LEVEL_SHIFT) & LEVEL_MASK) + LEVEL_STEPS - 1, levels = 1;
  wanted /= LEVEL_STEPS;
  for (uint32_t size = width > height ? width : height; size > 1 && levels <= wanted; size >>= 1) levels++;
  return levels;
}

static uint64_t hash_bytes(const uint8_t *data, uint32_t bytes) {
  uint64_t value = 1469598103934665603ull;
  for (uint32_t i = 0; i < bytes; i += 1 + bytes / 4096) value = (value ^ data[i]) * 1099511628211ull;
  return value;
}

// Unwritten memory hashes as it did before, so only a written range is read again.
static uint64_t hash_texture_bytes(uint32_t address, const uint8_t *source, uint32_t bytes, TextureMemo *memo) {
  if (!memo) return hash_bytes(source, bytes);
  if (memo->hashed_address == address && memo->hashed_bytes == bytes && memo->watched != WATCH_NONE && watch_count(address, bytes) == memo->watched) {
    return memo->bytes_hash;
  }
  memo->watched = watch_range(address, bytes);
  memo->hashed_address = address;
  memo->hashed_bytes = bytes;
  return memo->bytes_hash = hash_bytes(source, bytes);
}

DecodedTexture gx_decode_texture(int map, TextureMemo *memo) {
  DecodedTexture texture = {0, 0, 0};
  uint32_t image = gx.bp[(map < 4 ? BP_IMAGE_LOW + map : BP_IMAGE_HIGH + map - 4)];
  uint32_t address = gx.bp[(map < 4 ? BP_ADDRESS_LOW + map : BP_ADDRESS_HIGH + map - 4)] & 0xffffff;
  uint32_t palette = gx.bp[(map < 4 ? BP_PALETTE_LOW + map : BP_PALETTE_HIGH + map - 4)];
  uint32_t width = (image & 0x3ff) + 1, height = ((image >> 10) & 0x3ff) + 1;
  int format = (int)((image >> 20) & 15);
  const uint8_t *source = host((address << 5) | 0x80000000u);
  uint32_t levels = stored_levels(map, width, height), bytes = 0, pixel_count = 0;
  for (uint32_t level = 0; level < levels; level++) {
    uint32_t next = image_bytes(format, level_size(width, level), level_size(height, level));
    if (level && !host(((address << 5) + bytes + next - 1) | 0x80000000u)) {
      levels = level;
      break;
    }
    bytes += next;
    pixel_count += level_size(width, level) * level_size(height, level);
  }
  if (!source || !bytes) return texture;
  uint32_t palette_offset = (palette & 0x3ff) << PALETTE_SLOT_SHIFT;
  uint64_t identity = hash_texture_bytes((address << 5) | 0x80000000u, source, bytes, memo) ^ ((uint64_t)image << 20) ^ ((uint64_t)address << 40) ^ palette_offset ^ ((uint64_t)levels << 60);
  // The same image can be drawn with new colors, so the palette is part of what it is.
  if (format == FORMAT_C4 || format == FORMAT_C8 || format == FORMAT_C14X2) {
    uint32_t colors = format == FORMAT_C4 ? 16 : format == FORMAT_C8 ? 256 : PALETTE_INDEX_14 + 1;
    uint32_t palette_bytes = colors * 2 > PALETTE_BYTES - palette_offset ? PALETTE_BYTES - palette_offset : colors * 2;
    identity ^= hash_bytes(palette_memory + palette_offset, palette_bytes) * 3 + (uint64_t)((palette >> 10) & 3);
  }
  if (memo && memo->key == identity) return texture;
  if (memo) memo->key = identity;
  if (pixel_count * RGBA_BYTES > pixels_size) {
    pixels_size = pixel_count * RGBA_BYTES;
    pixels = realloc(pixels, pixels_size);
  }
  memset(pixels, 0, pixel_count * RGBA_BYTES);
  uint8_t *out = pixels;
  for (uint32_t level = 0; level < levels; level++) {
    uint32_t level_width = level_size(width, level), level_height = level_size(height, level);
    decode(source, format, level_width, level_height, palette_offset, (int)((palette >> 10) & 3), out);
    source += image_bytes(format, level_width, level_height);
    out += level_width * level_height * RGBA_BYTES;
  }
  texture.width = width;
  texture.height = height;
  texture.levels = levels;
  texture.rgba = pixels;
  return texture;
}
