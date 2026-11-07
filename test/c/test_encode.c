// Picture copies turned into memory bytes, and EFB peek and poke values, from made-up
// pixels. Each expected byte is worked out by hand from the formats' layouts: tiles of
// 8x8 for 4 bits, 8x4 for 8 bits, 4x4 for 16 and 32 bits, rows of tiles a stride apart,
// big-endian 16-bit texels, and RGBA8 as an alpha-red half then a green-blue half.
#include <string.h>

#include "gx_encode.h"
#include "../harness.h"

static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; }

static uint32_t pixels[16 * 16];
static uint8_t out[1024];

// Identity filter: the row's own weights add up to 64.
static CopyEncoding plain(int format, int width, int height, int stride) {
  CopyEncoding e;
  memset(&e, 0, sizeof e);
  e.format = (uint8_t)format;
  e.pixel_format = PICTURE_RGB8_Z24;
  e.filter[2] = 21, e.filter[3] = 22, e.filter[4] = 21;
  e.clamp_top = e.clamp_bottom = 1;
  e.width = (uint32_t)width, e.height = (uint32_t)height, e.stride = (uint32_t)stride;
  return e;
}

static CopySource whole(void) { return (CopySource){pixels, 16, 16, 0, 0}; }

static void fill(uint32_t value) { for (int i = 0; i < 16 * 16; i++) pixels[i] = value; }

int main(void) {
  // RGB565 from an RGB8 picture: 0xff, 0x80, 0x10 is 31, 32, 2.
  fill(rgba(0xff, 0x80, 0x10, 0x00));
  CopyEncoding e = plain(COPY_RGB565, 4, 4, 32);
  CopySource s = whole();
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0xfc);
  CHECK_EQ(out[1], 0x02);
  CHECK_EQ(out[30], 0xfc);

  // RGBA8: texel 0's alpha and red, then later its green and blue, 32 bytes on.
  memset(out, 0, sizeof out);
  e = plain(COPY_RGBA8, 4, 4, 64);
  e.pixel_format = PICTURE_RGBA6_Z24;
  fill(rgba(0x10, 0x20, 0x30, 0x40));
  pixels[1] = rgba(0x81, 0x02, 0x03, 0xff);  // Six bits a channel: 0x81 keeps 0x80 and repeats its top bits.
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0x41);  // 0x40 keeps its six bits, then its top two again: 0x40 | 0x01.
  CHECK_EQ(out[1], 0x10);
  CHECK_EQ(out[2], 0xff);
  CHECK_EQ(out[3], 0x82);
  CHECK_EQ(out[32], 0x20);
  CHECK_EQ(out[33], 0x30);
  CHECK_EQ(out[34], 0x00);  // 0x02 has nothing in its top six bits but 0.
  CHECK_EQ(out[35], 0x00);
  // An RGB8 picture has no alpha, so it's full.
  e.pixel_format = PICTURE_RGB8_Z24;
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0xff);
  CHECK_EQ(out[3], 0x81);

  // RGB5A3: opaque texels take five bits a color, others four with three of alpha.
  e = plain(COPY_RGB5A3, 4, 4, 32);
  e.pixel_format = PICTURE_RGBA6_Z24;
  fill(rgba(0xf8, 0x08, 0x80, 0xff));
  pixels[1] = rgba(0xf0, 0x10, 0x80, 0x80);
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0x80 | (31 << 2) | 0);  // 1, red 31, green 1 split over the bytes, blue 16.
  CHECK_EQ(out[1], (1 << 5) | 16);
  CHECK_EQ(out[2], (4 << 4) | 0xf);  // Alpha 0x80 is 4 of 7, red 0xf.
  CHECK_EQ(out[3], (1 << 4) | 8);

  // I4: two texels a byte, the left one high, in 8x8 tiles.
  memset(out, 0, sizeof out);
  for (int i = 0; i < 16 * 16; i++) pixels[i] = rgba((i % 16) * 16, 0, 0, 0);
  e = plain(COPY_R4, 16, 8, 64);
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0x01);
  CHECK_EQ(out[3], 0x67);
  CHECK_EQ(out[4], 0x01);   // The tile's second row starts again from column 0.
  CHECK_EQ(out[32], 0x89);  // The second tile holds columns 8 to 15.
  CHECK_EQ(out[35], 0xef);

  // One-byte formats in 8x4 tiles, from each channel.
  fill(rgba(1, 2, 3, 4));
  int channels[][2] = {{COPY_R8, 1}, {COPY_R8_LOW, 1}, {COPY_G8, 2}, {COPY_B8, 3}, {COPY_A8, 255}};
  for (int i = 0; i < 5; i++) {
    e = plain(channels[i][0], 8, 4, 32);
    gx_encode_copy(&e, &s, out);
    CHECK_EQ(out[31], channels[i][1]);
  }
  e.pixel_format = PICTURE_RGBA6_Z24;  // Now alpha is kept: 4 has no top six bits but 4.
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 4);

  // Two-channel formats: IA4 alpha high, IA8 alpha first, RG8 green first, GB8 blue first.
  fill(rgba(0xa0, 0xb0, 0xc0, 0xd0));
  e = plain(COPY_RA4, 8, 4, 32);
  e.pixel_format = PICTURE_RGBA6_Z24;
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0xda);
  e = plain(COPY_RA8, 4, 4, 32);
  e.pixel_format = PICTURE_RGBA6_Z24;
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0xd3);
  CHECK_EQ(out[1], 0xa2);
  e = plain(COPY_RG8, 4, 4, 32);
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0xb0);
  CHECK_EQ(out[1], 0xa0);
  e = plain(COPY_GB8, 4, 4, 32);
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0xc0);
  CHECK_EQ(out[1], 0xb0);

  // Intensity: white is Y 235 and black 16, as the YUV the copy unit makes.
  fill(rgba(255, 255, 255, 255));
  e = plain(COPY_R8, 8, 4, 32);
  e.intensity = 1;
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 235);
  fill(rgba(0, 0, 0, 255));
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 16);
  // Pure red: 66 * 255 + 4096 = 20926, over 256 is 81 and a half and more, so 82.
  fill(rgba(255, 0, 0, 0));
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 82);

  // Rows of tiles go a stride apart, and the bytes between are left alone.
  memset(out, 0xee, sizeof out);
  fill(rgba(7, 0, 0, 0));
  e = plain(COPY_R8, 8, 8, 48);
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[31], 7);
  CHECK_EQ(out[32], 0xee);
  CHECK_EQ(out[47], 0xee);
  CHECK_EQ(out[48], 7);
  CHECK_EQ(out[80], 0xee);

  // Half size: each texel is the mean of a 2 by 2 box.
  for (int i = 0; i < 16 * 16; i++) pixels[i] = rgba((i % 16) % 2 ? 10 : 20, 0, 0, 0);
  pixels[16] = rgba(50, 0, 0, 0);
  e = plain(COPY_R8, 8, 4, 32);
  e.half = 1;
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], (20 + 10 + 50 + 10) / 4);
  CHECK_EQ(out[1], 15);

  // The filter: a third each of the rows above, at and below. With the top clamped, the
  // row above the first is the first; without, it's the picture's row above the copy.
  for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) pixels[y * 16 + x] = rgba(y * 12, 0, 0, 200);
  e = plain(COPY_R8, 8, 4, 32);
  memset(e.filter, 0, sizeof e.filter);
  e.filter[1] = 21, e.filter[3] = 22, e.filter[6] = 21;
  s.y = 4;
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], (48 * 21 + 48 * 22 + 60 * 21) >> 6);
  CHECK_EQ(out[8], (48 * 21 + 60 * 22 + 72 * 21) >> 6);
  e.clamp_top = 0;
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], (36 * 21 + 48 * 22 + 60 * 21) >> 6);
  // Alpha isn't filtered, and weights adding up past 64 brighten, stopping at 255.
  e.format = COPY_A8;
  e.pixel_format = PICTURE_RGBA6_Z24;
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0xcb);  // 200 in six bits.
  e.format = COPY_R8;
  memset(e.filter, 0, sizeof e.filter);
  e.filter[3] = 63, e.filter[2] = 63;
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 48 * 126 >> 6);
  s.y = 12;
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 255);

  // Depth: its high byte reads as red, the middle as green and the low as blue.
  fill(0x123456);
  e = plain(COPY_RGBA8, 4, 4, 64);
  e.depth = 1;
  s = whole();
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0xff);
  CHECK_EQ(out[1], 0x12);
  CHECK_EQ(out[32], 0x34);
  CHECK_EQ(out[33], 0x56);
  e.format = COPY_RG8;  // Z16: the middle byte first.
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0x34);
  CHECK_EQ(out[1], 0x12);
  e.format = COPY_B8;
  e.width = 8;
  gx_encode_copy(&e, &s, out);
  CHECK_EQ(out[0], 0x56);

  // A whole row at a time gives what each texel on its own gives: filters, the edges held
  // or not, and the picture formats, against gx_copy_texel on made-up pixels.
  {
    static uint32_t source_pixels[16 * 12];
    uint32_t seed = 12345;
    for (int i = 0; i < 16 * 12; i++) seed = seed * 1103515245u + 12345u, source_pixels[i] = seed ^ (seed >> 13);
    CopySource source = {source_pixels, 16, 12, 3, 2};
    const uint8_t filters[][7] = {{0, 0, 21, 22, 21, 0, 0}, {10, 11, 11, 10, 11, 10, 1}, {0, 30, 40, 30, 0, 20, 10}, {63, 63, 63, 63, 63, 63, 63}};
    for (int f = 0; f < 4; f++) {
      for (int edges = 0; edges < 4; edges++) {
        for (int picture = 0; picture < 3; picture++) {
          CopyEncoding each = plain(COPY_RGBA8, 8, 8, 128);
          memcpy(each.filter, filters[f], 7);
          each.clamp_top = edges & 1, each.clamp_bottom = (edges >> 1) & 1;
          each.pixel_format = picture == 0 ? PICTURE_RGB8_Z24 : picture == 1 ? PICTURE_RGBA6_Z24 : PICTURE_RGB565_Z16;
          uint8_t blocks[2 * 128];
          gx_encode_copy(&each, &source, blocks);
          int wrong = 0;
          for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
              uint8_t want[4];
              gx_copy_texel(&each, &source, x, y, want);
              const uint8_t *block = blocks + (y / 4) * 128 + (x / 4) * 64;
              int index = (y % 4) * 4 + x % 4;
              uint8_t got[4] = {block[index * 2 + 1], block[32 + index * 2], block[32 + index * 2 + 1], block[index * 2]};
              wrong += memcmp(got, want, 4) != 0;
            }
          }
          CHECK_EQ(wrong, 0);
        }
      }
    }
  }

  // Formats with no shape.
  uint32_t across, rows, bytes;
  CHECK_EQ(gx_copy_shape(13, 4, 4, &across, &rows, &bytes), 0);
  CHECK_EQ(gx_copy_shape(COPY_R4, 9, 9, &across, &rows, &bytes), 1);
  CHECK_EQ(across, 2);
  CHECK_EQ(rows, 2);
  CHECK_EQ(gx_copy_shape(COPY_RGBA8, 5, 4, &across, &rows, &bytes), 1);
  CHECK_EQ(bytes, 64);

  // Peeks read 0xAARRGGBB, with alpha as the read mode says.
  uint32_t stored = rgba(0x11, 0x22, 0x33, 0x81);
  CHECK_EQ(gx_peek_color_value(stored, PICTURE_RGBA6_Z24, 2), 0x82102030u);
  CHECK_EQ(gx_peek_color_value(stored, PICTURE_RGBA6_Z24, 1), 0xff102030u);
  CHECK_EQ(gx_peek_color_value(stored, PICTURE_RGBA6_Z24, 0), 0x00102030u);
  CHECK_EQ(gx_peek_color_value(stored, PICTURE_RGB8_Z24, 2), 0xff112233u);
  CHECK_EQ(gx_peek_color_value(rgba(0xff, 0x81, 0x0f, 0), PICTURE_RGB565_Z16, 2), 0xffff8208u);
  // Depth peeks give 24 bits, or 16 in a picture of 16-bit depth.
  CHECK_EQ(gx_peek_depth_value(0.5, PICTURE_RGB8_Z24, 0), 0x800000);
  CHECK_EQ(gx_peek_depth_value(1.0, PICTURE_RGB8_Z24, 0), 0xffffff);
  CHECK_EQ(gx_peek_depth_value(0.5, PICTURE_RGB565_Z16, 0), 0x8000);
  // Near format: one leading one, its zero dropped, then 14 bits.
  CHECK_EQ(gx_peek_depth_value(0x9abcde / 16777216.0, PICTURE_RGB565_Z16, 1), (1u << 14) | ((0x9abcdeu >> 8) & 0x3fff));
  // All ones stop at the most the exponent holds, and the next bit is kept.
  CHECK_EQ(gx_peek_depth_value(1.0, PICTURE_RGB565_Z16, 1), (3u << 14) | 0x3fff);
  // Pokes write 0xAARRGGBB and 24-bit depth, which reads back the same.
  CHECK_EQ(gx_poke_color_value(0x80112233u), rgba(0x11, 0x22, 0x33, 0x80));
  CHECK_EQ(gx_depth_24(gx_poke_depth_value(0x00abcdefu)), 0xabcdef);
  return finish();
}
