// Turning the picture's pixels into the bytes a copy leaves in memory, and EFB peeks
// and pokes into pixels and back. Pure arithmetic with no graphics API, so it can be
// tested with made-up pixels (test/c/test_encode.c).
#pragma once
#include <stdint.h>

// The copy formats as gx_copy.c reads them from the copy register: code / 2 + (code & 1) * 8.
enum {
  COPY_R4 = 0, COPY_R8_LOW = 1, COPY_RA4 = 2, COPY_RA8 = 3, COPY_RGB565 = 4, COPY_RGB5A3 = 5, COPY_RGBA8 = 6, COPY_A8 = 7, COPY_R8 = 8,
  COPY_G8 = 9, COPY_B8 = 10, COPY_RG8 = 11, COPY_GB8 = 12, COPY_FORMATS = 16,
};
// The picture's pixel formats, BP 0x43 bits 0-2.
enum { PICTURE_RGB8_Z24 = 0, PICTURE_RGBA6_Z24 = 1, PICTURE_RGB565_Z16 = 2, PICTURE_Z24 = 3 };

// Everything about a copy that decides its bytes, read from the registers when it's made.
typedef struct {
  uint8_t format;        // One of the COPY_ values.
  uint8_t depth;         // From the depth buffer: each pixel's 24 bits, the high byte as red.
  uint8_t pixel_format;  // The picture's, a PICTURE_ value, for color copies.
  uint8_t intensity;     // Copy register bits 15 and 16 both set: the color turns into YUV first.
  uint8_t half;          // Each texel is a 2 by 2 box of pixels.
  uint8_t clamp_top, clamp_bottom;  // The filter's rows above and below stop at the copy's edge.
  uint8_t gamma;         // Copy register bits 7-8: 1.0, 1.7, 2.2, 2.2.
  uint8_t filter[7];     // BP 0x53-0x54: two weights for the row above, three for the pixel's own, two below.
  uint32_t width, height;  // The copy's size in texels, after any halving.
  uint32_t stride;         // Bytes from one row of blocks to the next, BP 0x4d.
} CopyEncoding;

// Pixels to copy from, top row first. Color is red, green, blue and alpha bytes, red in
// the low byte; depth is the 24-bit value. (x, y) is where the copy starts, in pixels
// before halving, and rows and columns outside the area are its nearest edge.
typedef struct {
  const uint32_t *pixels;
  int width, height;  // Of the whole area held.
  int x, y;
} CopySource;

// The shape a copy has in memory: blocks across and down, and the bytes in each block.
// Says 0 for a format with no shape (13 to 15).
int gx_copy_shape(uint32_t format, uint32_t width, uint32_t height, uint32_t *blocks_across, uint32_t *block_rows, uint32_t *block_bytes);
// Writes the copy's rows of blocks into `out`, `stride` apart. The bytes between rows are left as they were.
void gx_encode_copy(const CopyEncoding *encoding, const CopySource *source, uint8_t *out);
// One texel as the format's encoder sees it, before packing: red, green, blue and alpha
// after the pixel format, halving, filter, gamma and YUV.
void gx_copy_texel(const CopyEncoding *encoding, const CopySource *source, int x, int y, uint8_t texel[4]);

// What a CPU read of the picture's color at a pixel gives, as 0xAARRGGBB. `rgba` is the
// pixel as stored, red in the low byte. Alpha read mode is PE 0x08 bits 0-1.
uint32_t gx_peek_color_value(uint32_t rgba, int pixel_format, int alpha_read_mode);
// The same for depth, from the stored value in 0 to 1. A picture of 16-bit depth gives that.
uint32_t gx_peek_depth_value(double depth, int pixel_format, int depth_format);
// A poke's value as a pixel to store, red in the low byte, and as depth in 0 to 1.
uint32_t gx_poke_color_value(uint32_t value);
double gx_poke_depth_value(uint32_t value);
// The 24-bit depth a stored value from 0 to 1 holds.
uint32_t gx_depth_24(double depth);
