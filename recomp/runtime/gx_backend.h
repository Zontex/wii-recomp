// The line between what the Wii's graphics chip means and how a graphics API does it.
// Above it, gx_draw.c, gx_copy.c and gx_tev.c read the game's registers and know no API.
// Below it, gx_gl.c and gx_vulkan*.c know nothing of the game.
//
// Everything crossing the line counts in the game's own pixels, from the top left of
// its picture buffer. Each backend scales by EFB_SCALE and turns rows over if it must.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gx.h"

struct GLFWwindow;
typedef struct GxTexture GxTexture;
typedef unsigned GxStage;   // One compiled shader stage. 0 means none.
typedef unsigned GxShader;  // A vertex and a fragment stage together. 0 means none.

enum {
  GX_FACTOR_ZERO, GX_FACTOR_ONE, GX_FACTOR_SOURCE_COLOR, GX_FACTOR_INVERSE_SOURCE_COLOR, GX_FACTOR_TARGET_COLOR,
  GX_FACTOR_INVERSE_TARGET_COLOR, GX_FACTOR_SOURCE_ALPHA, GX_FACTOR_INVERSE_SOURCE_ALPHA, GX_FACTOR_TARGET_ALPHA,
  GX_FACTOR_INVERSE_TARGET_ALPHA, GX_FACTOR_SOURCE1_ALPHA, GX_FACTOR_INVERSE_SOURCE1_ALPHA,
};
// In the chip's own order, so a register's value can be used directly.
enum { GX_TEST_NEVER, GX_TEST_LESS, GX_TEST_EQUAL, GX_TEST_LESS_EQUAL, GX_TEST_GREATER, GX_TEST_NOT_EQUAL, GX_TEST_GREATER_EQUAL, GX_TEST_ALWAYS };
enum { GX_CULL_NONE, GX_CULL_FRONT, GX_CULL_BACK, GX_CULL_ALL };
enum { GX_WRAP_CLAMP, GX_WRAP_REPEAT, GX_WRAP_MIRROR };
enum { GX_MIP_NONE, GX_MIP_NEAREST, GX_MIP_LINEAR };
enum { GX_CHANNEL_RED, GX_CHANNEL_GREEN, GX_CHANNEL_BLUE, GX_CHANNEL_ALPHA, GX_CHANNEL_ONE };

// Blocks of values for shaders. Each is at most GX_CONSTANT_BYTES.
enum { GX_CONSTANT_BLOCKS = CONSTANTS_BLOCKS, GX_CONSTANT_BYTES = 2048 };
enum { GX_STAGE_VERTEX, GX_STAGE_FRAGMENT };
enum { GX_ATTRIBUTE_FLOAT, GX_ATTRIBUTE_BYTE };  // Floats, or whole unsigned bytes read as uvec.

// How pixels land in the picture. Blending reads a second shader output, the color
// stages' alpha, so that a constant output alpha changes only what is stored. Zeroed
// before filling, so it can be compared and hashed.
typedef struct {
  uint8_t blend_on, blend_subtract, blend_subtract_alpha, blend_source, blend_target, blend_source_alpha, blend_target_alpha;
  uint8_t depth_test_on, depth_test, depth_write;
  uint8_t color_write, alpha_write;
  uint8_t cull;
} GxRenderState;

// Depth runs from depth_near, for the chip's z of -w, to depth_far, for 0, within 0 to 1.
typedef struct { float x, y, width, height, depth_near, depth_far; } GxViewport;
typedef struct { int x, y, width, height; } GxRectangle;
typedef struct { uint8_t red, green, blue, alpha; } GxChannels;  // Where each channel reads from when sampled.

// Levels of detail count in 16ths. Anisotropy is 0, or 1 and 2 for up to 2 and 4 samples
// along a slope; backends mark a sampler table's slots in use in its top bits, as
// GX_SAMPLING_IN_USE. Zeroed before filling, so it can be compared and hashed.
typedef struct {
  uint8_t wrap_s, wrap_t, magnify_linear, minify_linear, mip_mode, min_level, max_level, anisotropy;
} GxSampling;
#define GX_SAMPLING_IN_USE 0x80u
_Static_assert(sizeof(GxSampling) == 8, "backends use a sampling as a 64-bit key");

typedef struct {
  GxRectangle area;
  float color[4];
  double depth;
  int color_write, alpha_write, depth_write;
} GxClear;

// One input of the vertex shader.
typedef struct {
  int location, components, kind;
  size_t offset;
} GxAttribute;

typedef struct {
  const char *name;

  // Window thread, in this order.
  void (*window_prepare)(void);                    // Before the window exists.
  void (*window_ready)(struct GLFWwindow *window); // Just after.

  // Everything else runs on the game's thread.
  void (*start)(const GxAttribute *attributes, int attribute_count, size_t vertex_bytes);

  // Text as in the note below. Compiling can take many milliseconds, so callers keep the result.
  GxStage (*stage_compile)(int kind, const char *text);
  GxShader (*shader_link)(GxStage vertex, GxStage fragment);
  void (*shader_use)(GxShader shader);
  void (*constants_send)(int block, const void *data, size_t bytes);

  void (*render_state)(const GxRenderState *state);
  void (*viewport)(const GxViewport *viewport);
  void (*scissor)(const GxRectangle *scissor);

  GxTexture *(*texture_create)(void);
  // Replaces the picture. `rgba` holds `levels` versions one after the other, each half the
  // size of the last and at least 1 by 1. Draws sent earlier still see the old one.
  void (*texture_upload)(GxTexture *texture, uint32_t width, uint32_t height, uint32_t levels, const uint8_t *rgba);
  // Stays until changed, and follows the texture if its picture is replaced.
  void (*texture_bind)(int map, GxTexture *texture, const GxSampling *sampling);

  // Room for vertices, to fill before the draw that follows. Nothing else may reserve in between.
  void *(*vertices_reserve)(int count);
  void (*draw)(int primitive, int count);  // A PRIM_ value other than lines and points.
  void (*clear)(const GxClear *clear);

  // Copies from the picture, shrinking or stretching to width by height. Only a smooth
  // copy mixes neighbouring pixels.
  void (*copy_color)(GxTexture *to, const GxRectangle *from, int width, int height, GxChannels channels, int smooth);
  // The result reads as depth, depth, depth, 1.
  void (*copy_depth)(GxTexture *to, const GxRectangle *from, int width, int height);

  void (*present)(const GxRectangle *from);
} GxBackend;

// Shader text has no #version line. The backend puts its own first, with these:
//   GX_VARYING(n)   before a varying or the color output: its location.
//   GX_BLOCK(n)     before a uniform block: which block of constants_send it reads.
//   GX_MAP(n)       before `uniform sampler2D mapN`: which texture map it reads.
//   GX_DEPTH(z, w)  the clip space depth for the chip's z, which runs from -w to 0.
//   GX_NO_DUAL_SOURCE  defined where a shader can't have a second color output.
// Blocks use std140, so the C structs must match that layout.

extern const GxBackend *gx_backend;
extern const GxBackend gx_backend_opengl, gx_backend_vulkan;

// Picks from WII_RECOMP_RENDERER (opengl or vulkan), once GLFW has started.
void gx_backend_choose(void);
