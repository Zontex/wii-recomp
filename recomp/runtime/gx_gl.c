// The OpenGL backend. OpenGL counts rows from the bottom and the game from the top,
// so this picture is stored upside down in memory and copies are turned over.
#define GL_SILENCE_DEPRECATION
#define GL_GLEXT_PROTOTYPES
#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "gx_backend.h"

enum {
  VERTEX_BUFFER_BYTES = 64 * 1024 * 1024, CONSTANT_BUFFER_BYTES = 32 * 1024 * 1024, LOG_BYTES = 1024,
  TEXT_BYTES = 64 * 1024, SETUP_UNIT = MAX_TEXTURE_MAPS,  // A texture unit no draw reads, for handling textures.
  VERTICES_PER_QUAD = 4, INDICES_PER_QUAD = 6, QUADS = MAX_VERTICES / VERTICES_PER_QUAD,
  SAMPLER_SLOTS = 8192, LEVEL_STEPS = 16,
};

static const char *const prefix =
  "#version 330 core\n"
  "#define GX_VARYING(n)\n"
  "#define GX_BLOCK(n) layout(std140)\n"
  "#define GX_MAP(n)\n"
  "#define GX_DEPTH(z, w) ((z) * 2.0 + (w))\n";

// Blocks are found by name, as OpenGL 3.3 can't set their numbers in the shader.
static const char *const block_names[CONSTANTS_BLOCKS] = {
  [CONSTANTS_PIXEL] = "PixelValues", [CONSTANTS_DRAW] = "DrawValues", [CONSTANTS_MATRICES] = "Matrices",
  [CONSTANTS_NORMALS] = "NormalMatrices", [CONSTANTS_POST] = "PostMatrices", [CONSTANTS_LIGHTS] = "Lights",
};

struct GxTexture {
  GLuint name;
  int width, height;
};

typedef struct {
  uint8_t last[GX_CONSTANT_BYTES];
  size_t bytes;
} Block;

static GLuint vertex_array, buffer, quad_indices, constant_buffer, efb, copy_framebuffer;
static size_t stream_at, vertex_bytes;
static Block blocks[GX_CONSTANT_BLOCKS];
static GLintptr constants_at;
static GLint constant_alignment;
static struct { uint64_t key; GLuint sampler; } samplers[SAMPLER_SLOTS];
static GLuint map_texture[MAX_TEXTURE_MAPS], map_sampler[MAX_TEXTURE_MAPS];

static void window_prepare(void) {
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
}

static void window_ready(struct GLFWwindow *window) { (void)window; }

static void start(const GxAttribute *attributes, int attribute_count, size_t bytes) {
  glfwMakeContextCurrent(window_handle());
  glfwSwapInterval(0);
  GLuint color, depth;
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_WIDTH * EFB_SCALE, EFB_HEIGHT * EFB_SCALE, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
  glGenTextures(1, &depth);
  glBindTexture(GL_TEXTURE_2D, depth);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, EFB_WIDTH * EFB_SCALE, EFB_HEIGHT * EFB_SCALE, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 0);
  glGenFramebuffers(1, &efb);
  glBindFramebuffer(GL_FRAMEBUFFER, efb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
  glGenFramebuffers(1, &copy_framebuffer);
  glGenVertexArrays(1, &vertex_array);
  glBindVertexArray(vertex_array);
  glGenBuffers(1, &buffer);
  glBindBuffer(GL_ARRAY_BUFFER, buffer);
  glBufferData(GL_ARRAY_BUFFER, VERTEX_BUFFER_BYTES, 0, GL_STREAM_DRAW);
  vertex_bytes = bytes;
  for (int i = 0; i < attribute_count; i++) {
    GLuint location = (GLuint)attributes[i].location;
    if (attributes[i].kind == GX_ATTRIBUTE_BYTE) {
      glVertexAttribIPointer(location, attributes[i].components, GL_UNSIGNED_BYTE, (GLsizei)bytes, (void *)attributes[i].offset);
    } else {
      glVertexAttribPointer(location, attributes[i].components, GL_FLOAT, GL_FALSE, (GLsizei)bytes, (void *)attributes[i].offset);
    }
    glEnableVertexAttribArray(location);
  }
  static uint16_t indices[QUADS * INDICES_PER_QUAD];
  for (int quad = 0; quad < QUADS; quad++) {
    static const int corners[INDICES_PER_QUAD] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < INDICES_PER_QUAD; i++) indices[quad * INDICES_PER_QUAD + i] = (uint16_t)(quad * VERTICES_PER_QUAD + corners[i]);
  }
  glGenBuffers(1, &quad_indices);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quad_indices);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &constant_alignment);
  glGenBuffers(1, &constant_buffer);
  glBindBuffer(GL_UNIFORM_BUFFER, constant_buffer);
  glBufferData(GL_UNIFORM_BUFFER, CONSTANT_BUFFER_BYTES, 0, GL_STREAM_DRAW);
}

static GLuint compile(GLenum kind, const char *text) {
  static char source[TEXT_BYTES];
  if ((size_t)snprintf(source, sizeof source, "%s%s", prefix, text) >= sizeof source) {
    fprintf(stderr, "shader: text too long\n");
    exit(9);
  }
  const char *pointer = source;
  GLuint shader = glCreateShader(kind);
  glShaderSource(shader, 1, &pointer, 0);
  glCompileShader(shader);
  GLint ok;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[LOG_BYTES];
    glGetShaderInfoLog(shader, sizeof log, 0, log);
    fprintf(stderr, "shader: %s\n", log);
    exit(9);
  }
  return shader;
}

static GxStage stage_compile(int kind, const char *text) { return compile(kind == GX_STAGE_VERTEX ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, text); }

static GxShader shader_link(GxStage vertex, GxStage fragment) {
  GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  GLint ok;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[LOG_BYTES];
    glGetProgramInfoLog(program, sizeof log, 0, log);
    fprintf(stderr, "shader: %s\n", log);
    exit(9);
  }
  glUseProgram(program);
  for (size_t i = 0; i < sizeof block_names / sizeof block_names[0]; i++) {
    GLuint block = glGetUniformBlockIndex(program, block_names[i]);
    if (block != GL_INVALID_INDEX) glUniformBlockBinding(program, block, (GLuint)i);
  }
  for (int map = 0; map < MAX_TEXTURE_MAPS; map++) {
    char name[8];
    snprintf(name, sizeof name, "map%d", map);
    GLint at = glGetUniformLocation(program, name);
    if (at >= 0) glUniform1i(at, map);
  }
  return program;
}

static void shader_use(GxShader shader) { glUseProgram(shader); }

static void place_block(int index) {
  size_t bytes = blocks[index].bytes;
  void *to = glMapBufferRange(GL_UNIFORM_BUFFER, constants_at, (GLsizeiptr)bytes,
                              GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
  memcpy(to, blocks[index].last, bytes);
  glUnmapBuffer(GL_UNIFORM_BUFFER);
  glBindBufferRange(GL_UNIFORM_BUFFER, (GLuint)index, constant_buffer, constants_at, (GLsizeiptr)bytes);
  constants_at += (GLintptr)((bytes + (size_t)constant_alignment - 1) & ~((size_t)constant_alignment - 1));
}

// Each block lands in a fresh part of one large buffer, as earlier draws may still be
// reading the old part. A fresh buffer loses them all, so all are placed again.
static void constants_send(int index, const void *data, size_t bytes) {
  Block *block = &blocks[index];
  if (block->bytes == bytes && !memcmp(block->last, data, bytes)) return;
  memcpy(block->last, data, bytes);
  block->bytes = bytes;
  glBindBuffer(GL_UNIFORM_BUFFER, constant_buffer);
  if (constants_at + GX_CONSTANT_BLOCKS * GX_CONSTANT_BYTES > CONSTANT_BUFFER_BYTES) {
    glBufferData(GL_UNIFORM_BUFFER, CONSTANT_BUFFER_BYTES, 0, GL_STREAM_DRAW);
    constants_at = 0;
    for (int i = 0; i < GX_CONSTANT_BLOCKS; i++) {
      if (blocks[i].bytes && i != index) place_block(i);
    }
  }
  place_block(index);
}

static GLenum factor(int which) {
  static const GLenum factors[] = {
    [GX_FACTOR_ZERO] = GL_ZERO, [GX_FACTOR_ONE] = GL_ONE, [GX_FACTOR_SOURCE_COLOR] = GL_SRC_COLOR,
    [GX_FACTOR_INVERSE_SOURCE_COLOR] = GL_ONE_MINUS_SRC_COLOR, [GX_FACTOR_TARGET_COLOR] = GL_DST_COLOR,
    [GX_FACTOR_INVERSE_TARGET_COLOR] = GL_ONE_MINUS_DST_COLOR, [GX_FACTOR_SOURCE_ALPHA] = GL_SRC_ALPHA,
    [GX_FACTOR_INVERSE_SOURCE_ALPHA] = GL_ONE_MINUS_SRC_ALPHA, [GX_FACTOR_TARGET_ALPHA] = GL_DST_ALPHA,
    [GX_FACTOR_INVERSE_TARGET_ALPHA] = GL_ONE_MINUS_DST_ALPHA, [GX_FACTOR_SOURCE1_ALPHA] = GL_SRC1_ALPHA,
    [GX_FACTOR_INVERSE_SOURCE1_ALPHA] = GL_ONE_MINUS_SRC1_ALPHA,
  };
  return factors[which];
}

static void render_state(const GxRenderState *state) {
  static const GLenum tests[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
  if (state->blend_on) {
    glEnable(GL_BLEND);
    glBlendEquationSeparate(state->blend_subtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, state->blend_subtract_alpha ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
    glBlendFuncSeparate(factor(state->blend_source), factor(state->blend_target), factor(state->blend_source_alpha), factor(state->blend_target_alpha));
  } else {
    glDisable(GL_BLEND);
  }
  if (state->depth_test_on) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(tests[state->depth_test]);
  } else {
    glDisable(GL_DEPTH_TEST);
  }
  glDepthMask(state->depth_write ? GL_TRUE : GL_FALSE);
  glColorMask(state->color_write, state->color_write, state->color_write, state->alpha_write);
  if (state->cull != GX_CULL_NONE) {
    glEnable(GL_CULL_FACE);
    glCullFace(state->cull == GX_CULL_FRONT ? GL_FRONT : state->cull == GX_CULL_BACK ? GL_BACK : GL_FRONT_AND_BACK);
  } else {
    glDisable(GL_CULL_FACE);
  }
}

static void viewport(const GxViewport *v) {
  glViewport((GLint)(v->x * EFB_SCALE), (GLint)((EFB_HEIGHT - v->y - v->height) * EFB_SCALE),
             (GLsizei)(v->width * EFB_SCALE), (GLsizei)(v->height * EFB_SCALE));
  glDepthRange(v->depth_near, v->depth_far);
}

static void set_scissor(const GxRectangle *area) {
  glEnable(GL_SCISSOR_TEST);
  glScissor(area->x * EFB_SCALE, (EFB_HEIGHT - area->y - area->height) * EFB_SCALE, area->width * EFB_SCALE, area->height * EFB_SCALE);
}

static GxTexture *texture_create(void) {
  GxTexture *texture = calloc(1, sizeof *texture);
  glGenTextures(1, &texture->name);
  return texture;
}

static void texture_upload(GxTexture *texture, uint32_t width, uint32_t height, uint32_t levels, const uint8_t *rgba) {
  glActiveTexture(GL_TEXTURE0 + SETUP_UNIT);
  glBindTexture(GL_TEXTURE_2D, texture->name);
  for (uint32_t level = 0; level < levels; level++) {
    GLsizei level_width = width >> level ? (GLsizei)(width >> level) : 1, level_height = height >> level ? (GLsizei)(height >> level) : 1;
    glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RGBA, level_width, level_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    rgba += (size_t)level_width * (size_t)level_height * 4;
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
}

static GLint minify_filter(const GxSampling *sampling) {
  if (sampling->mip_mode == GX_MIP_NONE) return sampling->minify_linear ? GL_LINEAR : GL_NEAREST;
  if (sampling->mip_mode == GX_MIP_LINEAR) return sampling->minify_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
  return sampling->minify_linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
}

// Wrapping and filtering live in sampler objects, one per combination the game uses,
// so a draw changes nothing the driver would have to check again.
static GLuint sampler_for(const GxSampling *sampling) {
  static const GLint wraps[] = {[GX_WRAP_CLAMP] = GL_CLAMP_TO_EDGE, [GX_WRAP_REPEAT] = GL_REPEAT, [GX_WRAP_MIRROR] = GL_MIRRORED_REPEAT};
  static uint32_t count;
  GxSampling keyed = *sampling;
  keyed.unused = 1;  // So that no key is ever 0, which marks a free slot.
  uint64_t key;
  memcpy(&key, &keyed, sizeof key);
  uint32_t slot = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 40) & (SAMPLER_SLOTS - 1);
  while (samplers[slot].key && samplers[slot].key != key) slot = (slot + 1) & (SAMPLER_SLOTS - 1);
  if (samplers[slot].key) return samplers[slot].sampler;
  if (count >= SAMPLER_SLOTS / 2) {
    debug_unsupported("more texture samplers than the table holds", SAMPLER_SLOTS / 2);
    return 0;  // The texture's own settings.
  }
  count++;
  samplers[slot].key = key;
  GLuint sampler;
  glGenSamplers(1, &sampler);
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, wraps[sampling->wrap_s]);
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, wraps[sampling->wrap_t]);
  glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, sampling->magnify_linear ? GL_LINEAR : GL_NEAREST);
  glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, minify_filter(sampling));
  glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, (float)sampling->min_level / LEVEL_STEPS);
  glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, (float)sampling->max_level / LEVEL_STEPS);
  samplers[slot].sampler = sampler;
  return sampler;
}

static void texture_bind(int map, GxTexture *texture, const GxSampling *sampling) {
  GLuint sampler = sampler_for(sampling);
  if (map_texture[map] != texture->name) {
    glActiveTexture(GL_TEXTURE0 + (GLenum)map);
    glBindTexture(GL_TEXTURE_2D, texture->name);
    map_texture[map] = texture->name;
  }
  if (map_sampler[map] != sampler) {
    glBindSampler((GLuint)map, sampler);
    map_sampler[map] = sampler;
  }
}

// Vertices go into one large buffer in turn. Only when it fills is a fresh one
// started, so the driver never has to wait for earlier draws to finish.
static void *vertices_reserve(int count) {
  size_t bytes = (size_t)count * vertex_bytes;
  if (stream_at + bytes > VERTEX_BUFFER_BYTES) {
    glBufferData(GL_ARRAY_BUFFER, VERTEX_BUFFER_BYTES, 0, GL_STREAM_DRAW);
    stream_at = 0;
  }
  return glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)stream_at, (GLsizeiptr)bytes,
                          GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
}

static void draw(int primitive, int count) {
  glUnmapBuffer(GL_ARRAY_BUFFER);
  GLint first = (GLint)(stream_at / vertex_bytes);
  stream_at += (size_t)count * vertex_bytes;
  if (primitive == PRIM_QUADS || primitive == PRIM_QUADS2) {
    glDrawElementsBaseVertex(GL_TRIANGLES, count / VERTICES_PER_QUAD * INDICES_PER_QUAD, GL_UNSIGNED_SHORT, 0, first);
  } else {
    glDrawArrays(primitive == PRIM_STRIP ? GL_TRIANGLE_STRIP : primitive == PRIM_FAN ? GL_TRIANGLE_FAN : GL_TRIANGLES, first, count);
  }
}

static void clear(const GxClear *c) {
  glColorMask((GLboolean)c->color_write, (GLboolean)c->color_write, (GLboolean)c->color_write, (GLboolean)c->alpha_write);
  glDepthMask((GLboolean)c->depth_write);
  set_scissor(&c->area);
  glClearColor(c->color[0], c->color[1], c->color[2], c->color[3]);
  glClearDepth(c->depth);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Makes `to` the size a copy needs and points the copy framebuffer's reading and drawing the right way.
static void prepare_copy(GxTexture *to, int width, int height, GLint format, GLenum layout, GLenum type) {
  glActiveTexture(GL_TEXTURE0 + SETUP_UNIT);
  glBindTexture(GL_TEXTURE_2D, to->name);
  if (to->width != width || to->height != height) {
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, layout, type, 0);
    to->width = width;
    to->height = height;
  }
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, efb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_framebuffer);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

static void set_channels(GxChannels channels) {
  static const GLint names[] = {[GX_CHANNEL_RED] = GL_RED, [GX_CHANNEL_GREEN] = GL_GREEN, [GX_CHANNEL_BLUE] = GL_BLUE,
                                [GX_CHANNEL_ALPHA] = GL_ALPHA, [GX_CHANNEL_ONE] = GL_ONE};
  GLint swizzle[4] = {names[channels.red], names[channels.green], names[channels.blue], names[channels.alpha]};
  glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
}

// Textures start at their top row and OpenGL's buffers at the bottom, so copies are
// turned over: the source's top comes first.
static void blit_from(const GxRectangle *from, int width, int height, GLbitfield what, GLenum filter) {
  glBlitFramebuffer(from->x * EFB_SCALE, (EFB_HEIGHT - from->y) * EFB_SCALE, (from->x + from->width) * EFB_SCALE,
                    (EFB_HEIGHT - from->y - from->height) * EFB_SCALE, 0, 0, width, height, what, filter);
}

static void copy_color(GxTexture *to, const GxRectangle *from, int width, int height, GxChannels channels, int smooth) {
  prepare_copy(to, width, height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
  set_channels(channels);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, to->name, 0);
  blit_from(from, width, height, GL_COLOR_BUFFER_BIT, smooth ? GL_LINEAR : GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, efb);
}

static void copy_depth(GxTexture *to, const GxRectangle *from, int width, int height) {
  prepare_copy(to, width, height, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);
  GxChannels channels = {GX_CHANNEL_RED, GX_CHANNEL_RED, GX_CHANNEL_RED, GX_CHANNEL_ONE};
  set_channels(channels);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, to->name, 0);
  glDrawBuffer(GL_NONE);
  blit_from(from, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
  glDrawBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_FRAMEBUFFER, efb);
}

static void present(const GxRectangle *from) {
  int window_width, window_height;
  window_size(&window_width, &window_height);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, efb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(from->x * EFB_SCALE, (EFB_HEIGHT - from->y - from->height) * EFB_SCALE, (from->x + from->width) * EFB_SCALE,
                    (EFB_HEIGHT - from->y) * EFB_SCALE, 0, 0, window_width, window_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glfwSwapBuffers(window_handle());
  window_refresh_size();
  glBindFramebuffer(GL_FRAMEBUFFER, efb);
}

const GxBackend gx_backend_opengl = {
  .name = "opengl",
  .window_prepare = window_prepare,
  .window_ready = window_ready,
  .start = start,
  .stage_compile = stage_compile,
  .shader_link = shader_link,
  .shader_use = shader_use,
  .constants_send = constants_send,
  .render_state = render_state,
  .viewport = viewport,
  .scissor = set_scissor,
  .texture_create = texture_create,
  .texture_upload = texture_upload,
  .texture_bind = texture_bind,
  .vertices_reserve = vertices_reserve,
  .draw = draw,
  .clear = clear,
  .copy_color = copy_color,
  .copy_depth = copy_depth,
  .present = present,
};
