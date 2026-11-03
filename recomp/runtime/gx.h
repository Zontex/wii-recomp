// The Wii's graphics chip, rebuilt in parts: the command stream (gx_fifo.c),
// vertices (gx_vertex.c), textures (gx_texture.c) and drawing (gx_draw.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define XF_SIZE 0x1100
#define MAX_TEXTURE_MAPS 8

typedef struct {
  uint32_t cp[256];       // Vertex formats and array locations.
  uint32_t bp[256];       // Pixel state: textures, blending, depth.
  uint32_t xf[XF_SIZE];   // Matrices and transform state.
  uint32_t tev_color[8];  // Color stage registers, which share BP addresses
  uint32_t tev_konst[8];  // with the stage constants; a flag picks between them.
} GxState;

extern GxState gx;

// Counters for the once-a-second report in debug.c.
typedef struct { unsigned draws, copies, to_screen, draw_done, new_shaders, passes; float slowest_shader_ms, worst_frame_ms; } GxStats;
extern GxStats gx_stats;

enum { VERTEX_MATRIX_POSITION = 0, VERTEX_MATRIX_COORDINATE = 1, VERTEX_MATRICES = 1 + MAX_TEXTURE_MAPS, VERTEX_MATRIX_SLOTS = 12, VERTEX_NORMALS_UV = 5 };
// A spare matrix slot that tells a corner of a line or point drawn as a quad. See gx_draw_expanded.
enum { VERTEX_CORNER = VERTEX_MATRICES };

// A vertex as the game sent it, before any matrix. The shader's inputs follow this.
typedef struct {
  float position[3], normal[3];
  float color[2][4];
  // A vertex with three normals keeps the other two over the last coordinates, as they
  // come with few of those, and everything else then costs nothing extra to pass around.
  union {
    float uv[MAX_TEXTURE_MAPS][2];
    struct { float uv_kept[VERTEX_NORMALS_UV][2]; float tangent[3], binormal[3]; };
  };
  uint8_t matrix[VERTEX_MATRIX_SLOTS];  // Index of the position's matrix, then each coordinate's.
} Vertex;

enum {
  ATTRIBUTE_POSITION = 0, ATTRIBUTE_NORMAL = 1, ATTRIBUTE_COLOR = 2, ATTRIBUTE_UV = 4, ATTRIBUTE_MATRICES = 12,
  ATTRIBUTE_MATRIX_GROUPS = 3,
};

enum {
  PRIM_QUADS = 0, PRIM_QUADS2 = 1, PRIM_TRIANGLES = 2, PRIM_STRIP = 3, PRIM_FAN = 4,
  PRIM_LINES = 5, PRIM_LINE_STRIP = 6, PRIM_POINTS = 7,
};

int gx_transform_bounds(const Vertex *vertices, int count, float bounds[4]);
int gx_transform_depths(const Vertex *vertices, int count, float *low, float *high);
int gx_fog_none_between(double low, double high);
// Where a vertex lands, over w as in gl_Position, and whether it is in front of the camera.
int gx_transform_screen(const Vertex *vertex, float *x, float *y);
// Lines (1) or points (2) are being drawn as quads, each corner moved by this much over w.
extern int gx_expanding;
extern float gx_expand_size[2];
extern Vertex *gx_reserved;
extern float gx_last_registers_alpha[4];
int gx_output_is_opaque(void);
extern int gx_bounds_known;
extern float gx_bounds[4];

void gx_parse(const uint8_t *data, uint32_t bytes);
void gx_stream_report(void);

// The bytes `count` vertices take in the command stream.
uint32_t gx_vertices_size(int format, int count);
// Decodes them into `out`, and returns how many there are after leaving out any the game skipped.
int gx_read_vertices(const uint8_t *data, int format, int count, Vertex *out);
// How many normals the current vertices carry: 0, 1, or 3 with binormal and tangent.
int gx_vertex_normals(int format);
// The last value seen of each normal the chip keeps for vertices with none. See gx_vertex.c.
enum { CACHED_NORMAL, CACHED_TANGENT, CACHED_BINORMAL, CACHED_NORMALS };
extern float gx_cached_normals[CACHED_NORMALS][4];

typedef struct {
  uint32_t width, height, levels;  // Each level follows the last in `rgba`, half the size.
  uint8_t *rgba;
} DecodedTexture;

// Decodes the texture a map points at, or returns 0 if it has none.
DecodedTexture gx_decode_texture(int map, uint64_t *key);
void gx_load_palette(void);
void gx_restore_palettes(const uint8_t *texture_memory);

enum { MAX_STAGES = 16, TEV_INDIRECT_SOURCES = 4, TEV_INDIRECT_MATRICES = 3 };

// One color stage, as far as a shader needs to know. See gx_tev.c.
typedef struct {
  uint8_t color_in[4], alpha_in[4];
  uint8_t color_bias, color_subtract, color_clamp, color_scale, color_target;
  uint8_t alpha_bias, alpha_subtract, alpha_clamp, alpha_scale, alpha_target;
  uint8_t reads_texture, texture_on, map, coordinate, texture_swap;
  uint8_t reads_raster, raster, raster_swap;
  uint8_t konst_color, konst_alpha;
  // `indirect` works out the coordinate. `indirect_used` also samples through it, and
  // `indirect_read` means it reads a fetch. Without that, a coordinate may still be wrapped or
  // be needed by the next stage. Matrix is 1-3, or 0 for none.
  uint8_t indirect, indirect_used, indirect_read, indirect_fetch, indirect_format, indirect_bias, indirect_matrix, indirect_kind, bump_alpha;
  uint8_t wrap_s, wrap_t, add_previous;
} TevStage;

// One of the four textures indirect stages read their offsets from.
typedef struct {
  uint8_t map, coordinate, shift_s, shift_t;
} TevIndirectSource;


// What decides which shader a draw needs. Zeroed before filling, so it can be compared and hashed.
typedef struct {
  uint8_t stages, alpha_test, alpha_first, alpha_second, alpha_logic, constant_alpha;
  uint8_t wrap_output;  // The last stage may leave a value outside 0 to 1, and only its low byte counts.
  uint8_t projected;  // Which coordinates need dividing by their third value at each pixel.
  uint8_t no_coordinates;  // The game enabled no texture coordinates, which changes what stages read.
  uint8_t bump_alpha;  // Some stage makes a bump alpha.
  uint8_t biased_maps;  // Maps with a level of detail bias, which most have none of.
  uint8_t depth_mode, depth_format, depth_from_copy;
  uint8_t fog_type, fog_orthographic, fog_range, fog_none;  // None: proven to add nothing to this draw.
  uint8_t logic_help;  // What the shader does for a logic operation done by blending. See read_logic in gx_tev.c.  // Fog's function, 0 for none. See read_fog in gx_tev.c.
  TevIndirectSource indirect_source[TEV_INDIRECT_SOURCES];
  TevStage stage[MAX_STAGES];
} TevKey;

// What a shader reads at run time. The layout matches the PixelValues block.
typedef struct {
  float registers[4][4], constants[4][4];
  float alpha_settings[4];  // First and second alpha reference, the constant output alpha, then the depth bias.
  float indirect_row_s[TEV_INDIRECT_MATRICES][4], indirect_row_t[TEV_INDIRECT_MATRICES][4];
  float map_size[MAX_TEXTURE_MAPS][4];  // Each map's width and height, then each coordinate's scale.
  float map_bias[MAX_TEXTURE_MAPS / 4][4];
  float fog_color[4], fog_settings[4];  // Color in 255ths, then A, C, B's magnitude and B's shift.
  float fog_range_center[4];  // The picture column at the center and the viewport's half width.
  float fog_range_k[3][4];  // Ten values from the edge in to the center.
} TevValues;

typedef struct {
  char *characters;
  size_t used, size;
} ShaderText;

// One lighting control word, as far as a shader needs to know. See gx_light.c.
typedef struct {
  uint8_t vertex_base, lit, vertex_start, lights, diffuse, attenuation;
} ChannelKey;

// How one texture coordinate is made. See gx_transform.c.
typedef struct {
  uint8_t generated, from_color, source, projected, normalized, keeps_z;
  uint8_t third_varies;  // Its third value may not be 1 at the end.
  uint8_t emboss, emboss_source, emboss_light;
} CoordinateKey;

// What decides which vertex shader a draw needs. Zeroed before filling, like TevKey.
typedef struct {
  uint8_t coordinates_read, coordinates_made, coordinates_projected, channels_read, channels_used, has_normal, normals_present, embossed, uses_lights, post_matrices;
  uint8_t expanded;  // Lines or points, drawn as quads.
  ChannelKey color[2], alpha[2];
  CoordinateKey coordinate[MAX_TEXTURE_MAPS];
} VertexKey;

// Where shader text says each varying goes.
enum { GX_VARYING_UV = 0, GX_VARYING_COLOR = MAX_TEXTURE_MAPS };

enum {
  CONSTANTS_PIXEL, CONSTANTS_DRAW, CONSTANTS_MATRICES, CONSTANTS_NORMALS, CONSTANTS_POST, CONSTANTS_LIGHTS, CONSTANTS_BLOCKS,
};

void gx_text_add(ShaderText *text, const char *format, ...);
unsigned gx_tev_read(TevKey *key, TevValues *values);
void gx_tev_reads(const TevKey *key, unsigned *maps, unsigned *coordinates, unsigned *channels);
void gx_tev_write_shader(const TevKey *key, ShaderText *text);
unsigned gx_tev_apply(TevKey *key);

void gx_transform_read(TevKey *tev, VertexKey *key);
void gx_transform_write_shader(const VertexKey *key, ShaderText *text);
void gx_transform_apply(const VertexKey *key);
void gx_light_read(VertexKey *key, int channel);
int gx_light_plain(int channel);
void gx_light_read_colors(float material[2][4], float ambient[2][4]);
void gx_light_send(void);
void gx_light_write_functions(ShaderText *text);
void gx_light_write_channel(ShaderText *text, const VertexKey *key, int channel);

void gx_shader_use(const TevKey *tev, const VertexKey *vertex);
void gx_constants_send(int binding, const void *data, size_t bytes);

// The chip's own picture buffer, kept apart from the window as games draw helper
// images in the part of it that never reaches the screen.
#define EFB_WIDTH 640
#define EFB_HEIGHT 528
// The picture's size against the Wii's. It can be a fraction such as 5/2, so always
// multiply by it, as in width * EFB_SCALE, which leaves the division for last.
#ifndef EFB_SCALE
#define EFB_SCALE 2  // Sharper than the Wii. build_file.py sets it for each platform.
#endif
#define MAX_VERTICES 65536
struct GxTexture;
void gx_copy_rectangle(int *x, int *y, int *width, int *height);
void gx_copy_to_texture(uint32_t control, int from_depth);
// Whether the picture's pixel format keeps alpha. Only RGBA6 does, and the others read as full.
int gx_target_has_alpha(void);
// Whether the constant output alpha is in use: it needs alpha written and kept. See Dolphin's PixelShaderGen.cpp.
int gx_constant_alpha_used(void);
int gx_blend_needs_second_output(const TevKey *key);
struct GxTexture *gx_copy_find(int map);
int gx_copy_is_depth(int map);

// Vertices go straight into the buffer the shader reads: reserve room, decode, draw.
extern int gx_vertex_format;
int gx_draw_wanted(int primitive, int count);
Vertex *gx_draw_reserve(int count);
void gx_draw(int primitive, int count);
// Draws lines, line strips and points from the command stream, and says if it did.
int gx_draw_expanded(int primitive, int format, const uint8_t *data, int count);
void gx_clear(void);
void gx_present(void);


struct GLFWwindow;
void window_create(void);
struct GLFWwindow *window_handle(void);
void window_refresh_size(void);
void window_size(int *width, int *height);
void window_run(void);
