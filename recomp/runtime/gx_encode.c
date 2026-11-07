// Copy encoding and EFB access values (gx_encode.h). The steps and their order follow
// Dolphin's TextureConversionShader.cpp, the encoder its hardware renderers use, and
// EFBInterface.cpp and VideoCommon.h for peeks and pokes (CREDITS.md).
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#include "gx_encode.h"

enum { BLOCK_BYTES = 32 };

static int bits_of(uint32_t format) {
  switch (format) {
    case COPY_R4: return 4;
    case COPY_R8_LOW: case COPY_RA4: case COPY_A8: case COPY_R8: case COPY_G8: case COPY_B8: return 8;
    case COPY_RA8: case COPY_RGB565: case COPY_RGB5A3: case COPY_RG8: case COPY_GB8: return 16;
    case COPY_RGBA8: return 32;
    default: return 0;
  }
}

int gx_copy_shape(uint32_t format, uint32_t width, uint32_t height, uint32_t *blocks_across, uint32_t *block_rows, uint32_t *block_bytes) {
  int bits = bits_of(format);
  if (!bits) return 0;
  uint32_t block_width = bits <= 8 ? 8 : 4, block_height = bits == 4 ? 8 : 4;
  *blocks_across = (width + block_width - 1) / block_width;
  *block_rows = (height + block_height - 1) / block_height;
  *block_bytes = bits == 32 ? BLOCK_BYTES * 2 : BLOCK_BYTES;
  return 1;
}

static int clamp(int value, int low, int high) { return value < low ? low : value > high ? high : value; }

// A stored pixel as the picture's format keeps it: no alpha in RGB8, six bits a channel
// in RGBA6, and 5, 6 and 5 bits in RGB565. Depth splits into its three bytes.
static void as_stored(const CopyEncoding *e, uint32_t pixel, int out[4]) {
  if (e->depth) {
    out[0] = (int)(pixel >> 16) & 255, out[1] = (int)(pixel >> 8) & 255, out[2] = (int)pixel & 255, out[3] = 255;
    return;
  }
  for (int i = 0; i < 4; i++) out[i] = (int)(pixel >> (i * 8)) & 255;
  if (e->pixel_format == PICTURE_RGBA6_Z24) {
    for (int i = 0; i < 4; i++) out[i] = (out[i] & 0xfc) | (out[i] >> 6);
  } else if (e->pixel_format == PICTURE_RGB565_Z16) {
    out[0] = (out[0] & 0xf8) | (out[0] >> 5);
    out[1] = (out[1] & 0xfc) | (out[1] >> 6);
    out[2] = (out[2] & 0xf8) | (out[2] >> 5);
    out[3] = 255;
  } else {
    out[3] = 255;
  }
}

// One row of texels, `row` from the copy's top, which may be just above or below it. A
// halved texel is the mean of its four pixels, as a linear sample between them gives.
static void fetch(const CopyEncoding *e, const CopySource *s, int x, int row, int out[4]) {
  int step = e->half ? 2 : 1;
  int top = e->clamp_top ? s->y : 0, bottom = (e->clamp_bottom ? s->y + (int)e->height * step : s->height) - step;
  int py = clamp(s->y + row * step, top, bottom > top ? bottom : top), px = s->x + x * step;
  int sum[4] = {0, 0, 0, 0};
  for (int dy = 0; dy < step; dy++) {
    for (int dx = 0; dx < step; dx++) {
      int value[4];
      int at_y = clamp(py + dy, 0, s->height - 1), at_x = clamp(px + dx, 0, s->width - 1);
      as_stored(e, s->pixels[(size_t)at_y * (size_t)s->width + (size_t)at_x], value);
      for (int i = 0; i < 4; i++) sum[i] += value[i];
    }
  }
  for (int i = 0; i < 4; i++) out[i] = sum[i] / (step * step);
}

void gx_copy_texel(const CopyEncoding *e, const CopySource *s, int x, int y, uint8_t texel[4]) {
  // The vertical filter: weights in 64ths for the row above, the texel's own and the row
  // below, on color only. Dolphin found it applies to depth copies too, on hardware.
  uint32_t above = (uint32_t)e->filter[0] + e->filter[1], middle = (uint32_t)e->filter[2] + e->filter[3] + e->filter[4];
  uint32_t below = (uint32_t)e->filter[5] + e->filter[6];
  int previous[4], current[4], next[4];
  fetch(e, s, x, y, current);
  if (above || below) {
    fetch(e, s, x, y - 1, previous);
    fetch(e, s, x, y + 1, next);
  } else {
    for (int i = 0; i < 4; i++) previous[i] = next[i] = 0;
  }
  int value[4];
  for (int i = 0; i < 3; i++) {
    uint32_t sum = ((uint32_t)previous[i] * above + (uint32_t)current[i] * middle + (uint32_t)next[i] * below) >> 6;
    // Weights adding up to 128 or more wrap past 511, as Dolphin's notes say.
    if (above + middle + below >= 128) sum &= 0x1ff;
    value[i] = sum > 255 ? 255 : (int)sum;
  }
  value[3] = current[3];
  if (e->gamma) {
    double power = 1.0 / (e->gamma == 1 ? 1.7 : 2.2);
    for (int i = 0; i < 3; i++) value[i] = (int)lround(pow(value[i] / 255.0, power) * 255.0);
  }
  if (e->intensity) {
    // Dolphin's constants, found by testing hardware, rounding a half up.
    int r = value[0], g = value[1], b = value[2];
    int yuv[3] = {66 * r + 129 * g + 25 * b + 16 * 256, -38 * r - 74 * g + 112 * b + 128 * 256, 112 * r - 94 * g - 18 * b + 128 * 256};
    for (int i = 0; i < 3; i++) value[i] = (yuv[i] >> 8) + ((yuv[i] >> 7) & 1);
  }
  for (int i = 0; i < 4; i++) texel[i] = (uint8_t)clamp(value[i], 0, 255);
}

// A row of texels. Most copies have no filter beyond the texel's own row, no gamma, no
// YUV and no halving, and those take each pixel as it's stored, with no more working out.
// One row of pixels as stored, the copy's row `y` with its edges held as fetch holds them.
static void stored_row(const CopyEncoding *e, const CopySource *s, int y, int count, int *out) {
  int top = e->clamp_top ? s->y : 0, bottom = (e->clamp_bottom ? s->y + (int)e->height : s->height) - 1;
  int at_y = clamp(clamp(s->y + y, top, bottom > top ? bottom : top), 0, s->height - 1);
  const uint32_t *line = s->pixels + (size_t)at_y * (size_t)s->width;
  for (int x = 0; x < count; x++) as_stored(e, line[clamp(s->x + x, 0, s->width - 1)], out + x * 4);
}

// A row of texels. Most copies have no gamma, no YUV and no halving, and those take each
// pixel as it's stored: the rows above and below the texel's, for the vertical filter,
// are worked out once for the whole row rather than for each texel, which is what
// gx_copy_texel does for the rest.
static void texel_row(const CopyEncoding *e, const CopySource *s, int y, int count, uint8_t *row) {
  if (e->half || e->gamma || e->intensity) {
    for (int x = 0; x < count; x++) gx_copy_texel(e, s, x, y, row + x * 4);
    return;
  }
  static int *rows;
  static size_t rows_size;
  if ((size_t)count * 12 > rows_size) rows = realloc(rows, (rows_size = (size_t)count * 12) * sizeof *rows);
  int *previous = rows, *current = rows + count * 4, *next = rows + count * 8;
  uint32_t above = (uint32_t)e->filter[0] + e->filter[1], middle = (uint32_t)e->filter[2] + e->filter[3] + e->filter[4];
  uint32_t below = (uint32_t)e->filter[5] + e->filter[6];
  int wraps = above + middle + below >= 128;  // Weights adding up to 128 or more wrap past 511, as in gx_copy_texel.
  stored_row(e, s, y, count, current);
  if (above || below) {
    stored_row(e, s, y - 1, count, previous);
    stored_row(e, s, y + 1, count, next);
  }
  for (int x = 0; x < count; x++) {
    for (int i = 0; i < 3; i++) {
      int at = x * 4 + i;
      uint32_t sum = (uint32_t)current[at] * middle;
      if (above || below) sum += (uint32_t)previous[at] * above + (uint32_t)next[at] * below;
      sum >>= 6;
      if (wraps) sum &= 0x1ff;
      row[at] = (uint8_t)(sum > 255 ? 255 : sum);
    }
    row[x * 4 + 3] = (uint8_t)current[x * 4 + 3];
  }
}

void gx_encode_copy(const CopyEncoding *e, const CopySource *s, uint8_t *out) {
  uint32_t across, rows, block_bytes;
  if (!gx_copy_shape(e->format, e->width, e->height, &across, &rows, &block_bytes)) return;
  int bits = bits_of(e->format), block_width = bits <= 8 ? 8 : 4, block_height = bits == 4 ? 8 : 4;
  int texels_across = (int)across * block_width;
  static uint8_t *strip;
  static size_t strip_size;
  size_t strip_need = (size_t)texels_across * (size_t)block_height * 4;
  if (strip_need > strip_size) strip = realloc(strip, strip_size = strip_need);
  for (uint32_t row = 0; row < rows; row++) {
    uint8_t *block = out + (size_t)row * e->stride;
    for (int ty = 0; ty < block_height; ty++) texel_row(e, s, (int)row * block_height + ty, texels_across, strip + (size_t)ty * texels_across * 4);
    for (uint32_t column = 0; column < across; column++, block += block_bytes) {
      for (int ty = 0; ty < block_height; ty++) {
        for (int tx = 0; tx < block_width; tx++) {
          int x = (int)column * block_width + tx, index = ty * block_width + tx;
          const uint8_t *t = strip + ((size_t)ty * texels_across + x) * 4;
          switch (e->format) {
            case COPY_R4: {
              uint8_t *byte = &block[index / 2];
              *byte = (uint8_t)(index & 1 ? (*byte & 0xf0) | (t[0] >> 4) : (t[0] & 0xf0) | (*byte & 0x0f));
              break;
            }
            case COPY_R8_LOW: case COPY_R8: block[index] = t[0]; break;
            case COPY_A8: block[index] = t[3]; break;
            case COPY_G8: block[index] = t[1]; break;
            case COPY_B8: block[index] = t[2]; break;
            case COPY_RA4: block[index] = (uint8_t)((t[3] & 0xf0) | (t[0] >> 4)); break;
            case COPY_RA8: block[index * 2] = t[3], block[index * 2 + 1] = t[0]; break;
            case COPY_RG8: block[index * 2] = t[1], block[index * 2 + 1] = t[0]; break;
            case COPY_GB8: block[index * 2] = t[2], block[index * 2 + 1] = t[1]; break;
            case COPY_RGB565: {
              uint32_t v = ((uint32_t)(t[0] >> 3) << 11) | ((uint32_t)(t[1] >> 2) << 5) | (t[2] >> 3);
              block[index * 2] = (uint8_t)(v >> 8), block[index * 2 + 1] = (uint8_t)v;
              break;
            }
            case COPY_RGB5A3: {
              // Opaque enough for three bits of alpha to be all ones: five bits a color instead.
              uint32_t v = t[3] >= 224 ? 0x8000u | ((uint32_t)(t[0] >> 3) << 10) | ((uint32_t)(t[1] >> 3) << 5) | (t[2] >> 3)
                                       : ((uint32_t)(t[3] >> 5) << 12) | ((uint32_t)(t[0] >> 4) << 8) | ((uint32_t)(t[1] >> 4) << 4) | (t[2] >> 4);
              block[index * 2] = (uint8_t)(v >> 8), block[index * 2 + 1] = (uint8_t)v;
              break;
            }
            case COPY_RGBA8:  // Alpha and red in the block's first half, green and blue in its second.
              block[index * 2] = t[3], block[index * 2 + 1] = t[0];
              block[BLOCK_BYTES + index * 2] = t[1], block[BLOCK_BYTES + index * 2 + 1] = t[2];
              break;
            default: break;
          }
        }
      }
    }
  }
}

uint32_t gx_peek_color_value(uint32_t rgba, int pixel_format, int alpha_read_mode) {
  uint32_t color = (rgba & 0xff00ff00u) | ((rgba >> 16) & 0xff) | ((rgba & 0xff) << 16);  // To 0xAARRGGBB.
  if (pixel_format == PICTURE_RGBA6_Z24) {
    color &= 0xfcfcfcfcu;
    color |= (color >> 6) & 0x03030303u;
  } else if (pixel_format == PICTURE_RGB565_Z16) {
    color &= 0xf8fcf8u;
    color |= (color >> 5) & 0x070007u;
    color |= (color >> 6) & 0x000300u;
  }
  if (pixel_format != PICTURE_RGBA6_Z24) color |= 0xff000000u;
  // PE 0x08: 0 reads alpha as 0, 1 as 0xff, 2 as stored.
  if (alpha_read_mode == 1) return color | 0xff000000u;
  if (alpha_read_mode == 2) return color;
  return color & 0x00ffffffu;
}

uint32_t gx_depth_24(double depth) {
  double steps = depth * 16777216.0;
  return steps <= 0 ? 0 : steps >= 0xffffff ? 0xffffff : (uint32_t)steps;
}

// Sixteen bits of depth: linear, or a small float with 2, 3 or 4 bits of exponent
// counting the leading ones, as Dolphin's CompressZ16 works it out.
static uint32_t compress_z16(uint32_t z, int format) {
  int exponent_bits, most;
  if (format == 1) exponent_bits = 2, most = 3;
  else if (format == 2) exponent_bits = 3, most = 7;
  else if (format == 3) exponent_bits = 4, most = 12;
  else return z >> 8;
  int ones = 0;
  while (ones < 24 && (z >> (23 - ones)) & 1) ones++;
  int next_is_one = 0;
  if (ones >= most) ones = most, next_is_one = 1;
  int mantissa_bits = 16 - exponent_bits, top = 24 - ones > mantissa_bits ? 24 - ones : mantissa_bits;
  if (!next_is_one) top -= 1;
  int bottom = top - mantissa_bits;
  return ((uint32_t)ones << mantissa_bits) | ((z >> bottom) & ((1u << mantissa_bits) - 1));
}

uint32_t gx_peek_depth_value(double depth, int pixel_format, int depth_format) {
  uint32_t z = gx_depth_24(depth);
  return pixel_format == PICTURE_RGB565_Z16 ? compress_z16(z, depth_format) : z;
}

uint32_t gx_poke_color_value(uint32_t value) { return (value & 0xff00ff00u) | ((value >> 16) & 0xff) | ((value & 0xff) << 16); }
double gx_poke_depth_value(uint32_t value) { return (double)(value & 0xffffff) / 16777216.0; }
