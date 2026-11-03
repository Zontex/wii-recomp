// The Vulkan backend. Its images count rows from the top as the game does, so the
// picture is stored the right way up and copies aren't turned over. Shaders still
// see OpenGL's clip space, as the viewport's height is negative.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_vulkan.h"

enum {
  EFB_PIXEL_WIDTH = EFB_WIDTH * EFB_SCALE, EFB_PIXEL_HEIGHT = EFB_HEIGHT * EFB_SCALE, BYTES_PER_PIXEL = 4,
  // Dropping a picture beats holding up the game, which OpenGL never did either.
  ACQUIRE_TIMEOUT_NS = 0,
  VERTICES_PER_QUAD = 4, INDICES_PER_QUAD = 6, INDICES_PER_TRIANGLE = 3, QUADS = MAX_VERTICES / VERTICES_PER_QUAD,
  QUAD_INDICES = QUADS * INDICES_PER_QUAD, SHAPE_INDICES = MAX_VERTICES * INDICES_PER_TRIANGLE,
  FIRST_QUAD_INDEX = 0, FIRST_STRIP_INDEX = QUAD_INDICES, FIRST_FAN_INDEX = QUAD_INDICES + SHAPE_INDICES,
  INDEX_COUNT = QUAD_INDICES + SHAPE_INDICES * 2,
};

static const char *const clear_vertex_source =
  "layout(push_constant) uniform Clear { vec4 color; float depth; };\n"
  "void main() {\n"
  "  vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
  "  gl_Position = vec4(corner * 2.0 - 1.0, depth, 1.0);\n"
  "}\n";
static const char *const clear_fragment_source =
  "layout(push_constant) uniform Clear { vec4 color; float depth; };\n"
  "GX_VARYING(0) out vec4 out_color;\n"
  "void main() { out_color = color; }\n";

typedef struct {
  float color[4], depth;
} ClearValues;

struct GxTexture {
  VkImage image;
  VkImageView view;
  VkDeviceMemory memory;
  int width, height, depth, written;
  GxChannels channels;
  uint32_t copied_frame, strip_frame;
  VkImageView attach_view;  // With framebuffer, when its image can be drawn into: see the side picture.
  VkFramebuffer framebuffer;
  int owed;  // Its copy was skipped: 1 the picture, 2 an empty layer, 3 depth. 4 adds: given up late, 8: drawn apart.
  uint32_t owed_frame;
};
static uint32_t frame_serial;
static int in_strip, strip_rest;
enum { LAYER_IDLE, LAYER_PICTURE_COPIED, LAYER_CLEARED, LAYER_EMPTY_COPIED, LAYER_CLEARED_AGAIN, LAYER_PICTURE_BACK, LAYER_DEPTH_COPIED, LAYER_DEPTH_CLEARED,
       LAYER_SIDE, LAYER_SIDE_COPIED, LAYER_SIDE_CLEARED };
static struct {
  int step, off, real;  // Real: the layer has something in it, drawn into the side picture.
  GxTexture *picture, *layer, *depth;
  GxRectangle from;
  int width, height, depth_width, depth_height;
  GxChannels picture_channels, layer_channels;
  GxClear clears[3];
} empty_layer;
static struct {
  int bands;
  float bottom, top;
  GxShader first_shader, next_shader;
  float next_box[4];
  float first_box[4], candidate_box[4];
  GxShader candidate_shader;
  int recorded_anything_lasting;
} cover;
static struct {
  int number, last_in_frame, read_depth, saw_depth_clear, frame_began;
  uint32_t shape, last_shape;
  char no_load[32], no_store[32];
  float clear_depth;
} depth_use;

typedef struct {
  GxTexture *texture;
  GxSampling sampling;
} Binding;

static GxShader shader_in_use, clear_shader;
static GxRenderState state_in_use;
static VkViewport viewport_wanted, viewport_set;
static VkRect2D scissor_wanted, scissor_set;
static VkPipeline pipeline_set;
static GxTexture *blank_texture;
static Binding bindings[MAX_TEXTURE_MAPS];
static VkDescriptorImageInfo maps_set[MAX_TEXTURE_MAPS];
static Ring shape_indices;  // Strips, fans and quads as triangles: Metal has no fans, and one kind of pipeline serves all.
static VkDeviceSize vertex_bytes, reserved_at;
static int pass_open, indices_bound;
static VkRect2D pass_area;
static unsigned restarts, reserved_restarts;  // How often the frame's memory has started over.

// The latest value of each constant block. A new frame has its own memory, and a block
// sent long ago must still reach it, so values wait here until a draw needs them.
static uint8_t blocks[GX_CONSTANT_BLOCKS][GX_CONSTANT_BYTES];
static uint32_t block_bytes[GX_CONSTANT_BLOCKS], block_offsets[GX_CONSTANT_BLOCKS];
static unsigned blocks_waiting;

static void window_prepare(void) { glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); }

static void window_ready(struct GLFWwindow *window) { vk_create_instance_and_surface(window); }

static void barrier(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, uint32_t level, uint32_t levels, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags from_stage, VkAccessFlags from_access, VkPipelineStageFlags to_stage, VkAccessFlags to_access) {
  VkImageMemoryBarrier info = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = from_access, .dstAccessMask = to_access, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                               .subresourceRange = {aspect, level, levels, 0, 1}};
  vkCmdPipelineBarrier(commands, from_stage, to_stage, 0, 0, 0, 0, 0, 1, &info);
}

#define COLOR_ATTACHMENT_STAGE VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
#define COLOR_ATTACHMENT_ACCESS (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
#define DEPTH_ATTACHMENT_STAGE (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
#define DEPTH_ATTACHMENT_ACCESS (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)

// The picture's two images between drawing into them and being read as a copy's source.
static void efb_color_to_source(VkCommandBuffer commands) {
  barrier(commands, vk.efb_color, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          COLOR_ATTACHMENT_STAGE, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
}

static void efb_color_to_target(VkCommandBuffer commands) {
  barrier(commands, vk.efb_color, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, COLOR_ATTACHMENT_STAGE, COLOR_ATTACHMENT_ACCESS);
}

enum { CLEARS_COLOR = 1, CLEARS_DEPTH = 2, DROPS_DEPTH = 4, VISIBLE_HEIGHT = 456, MAX_PASSES = 32 };
static VkRenderPass render_passes[8];
static VkFramebuffer visible_framebuffer;  // Same memory, ending where the visible picture does.

// A clear waits for the next pass and becomes how that pass starts, which spares
// the chip loading and storing a picture only to overwrite it.
static struct { int kind; VkRect2D area; VkClearValue values[2]; } waiting_clear;

static void create_render_pass(void) {
  VkAttachmentDescription attachments[2] = {
    {.format = COLOR_FORMAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
     .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
     .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    {.format = DEPTH_FORMAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
     .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
     .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
  };
  VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &color, .pDepthStencilAttachment = &depth};
  VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = attachments, .subpassCount = 1, .pSubpasses = &subpass};
  for (int kind = 0; kind < 8; kind++) {
    attachments[1].storeOp = kind & DROPS_DEPTH ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].loadOp = kind & CLEARS_COLOR ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].loadOp = kind & CLEARS_DEPTH ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    vk_check(vkCreateRenderPass(vk.device, &info, 0, &render_passes[kind]), "creating the render pass");
  }
  vk.render_pass = render_passes[0];
}

static void create_efb(void) {
  static const VkComponentMapping plain = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  VkDeviceMemory color_memory, depth_memory;
  vk_create_image(COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  EFB_PIXEL_WIDTH, EFB_PIXEL_HEIGHT, 1, &vk.efb_color, &color_memory);
  vk_create_image(DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  EFB_PIXEL_WIDTH, EFB_PIXEL_HEIGHT, 1, &vk.efb_depth, &depth_memory);
  vk.efb_color_view = vk_create_view(vk.efb_color, COLOR_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, plain);
  vk.efb_depth_view = vk_create_view(vk.efb_depth, DEPTH_FORMAT, VK_IMAGE_ASPECT_DEPTH_BIT, 1, plain);
  VkImageView views[2] = {vk.efb_color_view, vk.efb_depth_view};
  VkFramebufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = vk.render_pass, .attachmentCount = 2, .pAttachments = views, .layers = 1};
  info.width = EFB_PIXEL_WIDTH;
  info.height = EFB_PIXEL_HEIGHT;
  vk_check(vkCreateFramebuffer(vk.device, &info, 0, &vk.efb_framebuffer), "creating the framebuffer");
  info.height = VISIBLE_HEIGHT * EFB_SCALE;
  vk_check(vkCreateFramebuffer(vk.device, &info, 0, &visible_framebuffer), "creating the framebuffer");
}

// Fresh memory holds anything, so the picture starts as blank as OpenGL's did in practice.
static void blank_efb(VkCommandBuffer commands) {
  VkClearColorValue color = {{0, 0, 0, 0}};
  VkClearDepthStencilValue depth = {0, 0};
  VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, depth_range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  barrier(commands, vk.efb_color, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  barrier(commands, vk.efb_depth, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdClearColorImage(commands, vk.efb_color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &color_range);
  vkCmdClearDepthStencilImage(commands, vk.efb_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth, 1, &depth_range);
  barrier(commands, vk.efb_color, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, COLOR_ATTACHMENT_STAGE, COLOR_ATTACHMENT_ACCESS);
  barrier(commands, vk.efb_depth, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, DEPTH_ATTACHMENT_STAGE, DEPTH_ATTACHMENT_ACCESS);
}

static GxTexture *texture_create(void) { return calloc(1, sizeof(GxTexture)); }
static void retire_attachment(GxTexture *t);

static void upload_pixels(GxTexture *texture, uint32_t width, uint32_t height, uint32_t levels, const uint8_t *rgba);

// A new commands buffer knows nothing of what the last one had set.
static void forget_set_state(void) {
  pipeline_set = VK_NULL_HANDLE;
  memset(&viewport_set, 0, sizeof viewport_set);
  memset(&scissor_set, 0, sizeof scissor_set);
  memset(maps_set, 0, sizeof maps_set);
  indices_bound = 0;
  for (int i = 0; i < GX_CONSTANT_BLOCKS; i++) {
    if (block_bytes[i]) blocks_waiting |= 1u << i;
  }
}

static void mark_frame_start(void);
static void begin_frame(Frame *frame) {
  vk_frame_begin(frame);
  pass_open = 0;
  restarts++;
  frame_serial++;
  if (depth_use.shape != depth_use.last_shape) {
    memset(depth_use.no_load, 0, sizeof depth_use.no_load);
    memset(depth_use.no_store, 0, sizeof depth_use.no_store);
  }
  depth_use.last_shape = depth_use.shape;
  depth_use.shape = 0;
  cover.recorded_anything_lasting = 0;
  cover.bands = 0;
  cover.first_shader = cover.next_shader;
  memcpy(cover.first_box, cover.next_box, sizeof cover.first_box);
  cover.next_shader = 0;
  depth_use.number = 0;
  depth_use.frame_began = 0;
  if (strip_rest) strip_rest--;
  mark_frame_start();
  forget_set_state();
}

// Timing of one frame in every 300, on when WII_RECOMP_GPU_TIMES is set.
enum { MARKS = 96 };
static VkQueryPool mark_pool; static int marks, mark_on = -1, mark_frame; static int capturing; static char mark_names[MARKS][48];
static unsigned mark_draws;
static void mark(const char *name, int w, int h) {
  if (!capturing || marks >= MARKS || in_strip) return;
  vkCmdWriteTimestamp(vk.frame->draw, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mark_pool, (uint32_t)marks);
  snprintf(mark_names[marks], 48, "%s %dx%d draws %u", name, w, h, gx_stats.draws - mark_draws);
  mark_draws = gx_stats.draws;
  marks++;
}
static void mark_frame_start(void) {
  if (mark_on < 0) { mark_on = getenv("WII_RECOMP_GPU_TIMES") != 0;
    if (mark_on) { VkQueryPoolCreateInfo q = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = MARKS};
      vk_check(vkCreateQueryPool(vk.device, &q, 0, &mark_pool), "query pool"); } }
  capturing = 0;
  if (!mark_on || ++mark_frame % 300) return;
  vkCmdResetQueryPool(vk.frame->draw, mark_pool, 0, MARKS);
  marks = 0; capturing = 1; mark("start", 0, 0);
}
static void mark_report(void) {
  if (!capturing) return;
  vkQueueWaitIdle(vk.queue);
  static uint64_t t[MARKS];
  vkGetQueryPoolResults(vk.device, mark_pool, 0, (uint32_t)marks, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  double period = vk.limits.timestampPeriod;
  fprintf(stderr, "gpu frame %.2f ms\n", (t[marks - 1] - t[0]) * period / 1e6);
  for (int i = 1; i < marks; i++) fprintf(stderr, "  %6.2f ms %s\n", (t[i] - t[i - 1]) * period / 1e6, mark_names[i]);
  capturing = 0;
}
// The game draws small pictures into a strip below the visible one and copies them out,
// between chunks of its scene. Each would split the scene's pass, and a tile-based chip
// then stores and reloads everything. So that work goes in the commands that run first,
// and the scene's pass stays open. A draw there can't read what this frame's scene made.
static int in_strip, strip_off = -1, strip_rest;
static uint32_t frame_serial;
static VkCommandBuffer scene_commands;
static struct { int open; VkRect2D area; int kind; VkRect2D clear_area; VkClearValue values[2]; } scene_state;
static void forget_set_state(void);
static void give_up_layer(void);
static int layer_draw(int count);
static void start_pass_with_clear(void);
static void end_pass(void);

static void enter_strip(void) {
  scene_state.open = pass_open;
  scene_state.area = pass_area;
  scene_state.kind = waiting_clear.kind;
  scene_state.clear_area = waiting_clear.area;
  memcpy(scene_state.values, waiting_clear.values, sizeof scene_state.values);
  pass_open = 0;
  waiting_clear.kind = 0;
  scene_commands = vk.frame->draw;
  vk.frame->draw = vk_upload_commands();
  in_strip = 1;
  forget_set_state();
}

static void leave_strip(void) {
  if (!in_strip) return;
  end_pass();
  in_strip = 0;
  vk.frame->draw = scene_commands;
  pass_open = scene_state.open;
  pass_area = scene_state.area;
  waiting_clear.kind = scene_state.kind;
  waiting_clear.area = scene_state.clear_area;
  memcpy(waiting_clear.values, scene_state.values, sizeof scene_state.values);
  forget_set_state();
}

// Called before any work, with whether that work is in the strip.
static void route_work(int strip) {
  if (strip_off < 0) strip_off = getenv("WII_RECOMP_NO_STRIP") != 0;
  if (strip && !in_strip && !strip_off && !strip_rest) enter_strip();
  else if (!strip) leave_strip();
}

// Most frames repeat. A pass that clears depth before reading it needn't load it, and
// the last pass needn't store it if the next frame clears it first. Each is learned from
// the frame before and forgotten the moment the game does otherwise.

static void forget_depth_use(void) {
  memset(depth_use.no_load, 0, sizeof depth_use.no_load);
  memset(depth_use.no_store, 0, sizeof depth_use.no_store);
}

static int pass_kind_learned(void) {
  static int off = -1;
  if (off < 0) off = getenv("WII_RECOMP_KEEP_DEPTH") != 0;
  int n = depth_use.number;
  if (off || in_strip || n >= MAX_PASSES) return 0;
  return (depth_use.no_load[n] ? CLEARS_DEPTH : 0) | (depth_use.no_store[n] ? DROPS_DEPTH : 0);
}

// A layer's color can go to a second picture while the first stays where it is, so the
// first needn't be copied out and drawn back. Depth is shared. See the layers below.
static struct { VkImage image; VkDeviceMemory memory; VkImageView view; VkFramebuffer framebuffer; int on; } side;

static VkFramebuffer framebuffer_for(int visible) {
  if (side.on && !in_strip) return side.framebuffer;
  return visible ? visible_framebuffer : vk.efb_framebuffer;
}

static void start_pass_with_clear(void) {
  int learned = pass_kind_learned();
  if (learned & CLEARS_DEPTH) waiting_clear.values[1].depthStencil.depth = depth_use.clear_depth;
  waiting_clear.kind |= learned;
  depth_use.read_depth = depth_use.saw_depth_clear = 0;
  VkRect2D area = waiting_clear.area;
  int visible = area.offset.y + (int64_t)area.extent.height == VISIBLE_HEIGHT * EFB_SCALE;
  VkRenderPassBeginInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = render_passes[waiting_clear.kind],
                                .framebuffer = framebuffer_for(visible), .renderArea = area,
                                .clearValueCount = 2, .pClearValues = waiting_clear.values};
  vkCmdBeginRenderPass(vk.frame->draw, &info, VK_SUBPASS_CONTENTS_INLINE);
  waiting_clear.kind = 0;
  pass_area = area;
  pass_open = 1;
  gx_stats.passes++;
}

static void end_pass(void) {
  if (!pass_open && waiting_clear.kind) start_pass_with_clear();
  if (!pass_open) return;
  vkCmdEndRenderPass(vk.frame->draw);
  pass_open = 0;
  if (!in_strip) {
    depth_use.last_in_frame = depth_use.number;
    depth_use.number++;
    // What's learned is by a pass's place in the frame, so the frames must have one shape.
    depth_use.shape = depth_use.shape * 31 + pass_area.extent.width * 7 + pass_area.extent.height + pass_area.offset.y * 3;
  }
  mark("pass", (int)pass_area.extent.width, (int)pass_area.extent.height);
}

static int rectangle_holds(const VkRect2D *outer, const VkRect2D *inner) {
  return inner->offset.x >= outer->offset.x && inner->offset.y >= outer->offset.y
      && inner->offset.x + (int64_t)inner->extent.width <= outer->offset.x + (int64_t)outer->extent.width
      && inner->offset.y + (int64_t)inner->extent.height <= outer->offset.y + (int64_t)outer->extent.height;
}

static VkRect2D rectangle_union(const VkRect2D *a, const VkRect2D *b) {
  int32_t left = a->offset.x < b->offset.x ? a->offset.x : b->offset.x, top = a->offset.y < b->offset.y ? a->offset.y : b->offset.y;
  int32_t right_a = a->offset.x + (int32_t)a->extent.width, right_b = b->offset.x + (int32_t)b->extent.width;
  int32_t bottom_a = a->offset.y + (int32_t)a->extent.height, bottom_b = b->offset.y + (int32_t)b->extent.height;
  int32_t right = right_a > right_b ? right_a : right_b, bottom = bottom_a > bottom_b ? bottom_a : bottom_b;
  return (VkRect2D){{left, top}, {(uint32_t)(right - left), (uint32_t)(bottom - top)}};
}

// A tile-based chip loads and stores everything in a pass's area, so a pass covers only
// what it draws. It starts with the first draw's rectangle, and a later draw outside
// that reopens it wider, which keeps every pixel but costs one more pass.
static void begin_pass(const VkRect2D *area) {
  if (pass_open && rectangle_holds(&pass_area, area)) return;
  // Work below the picture can't touch a clear waiting in it, which can go on waiting.
  int apart = area->offset.y >= (int64_t)waiting_clear.area.offset.y + waiting_clear.area.extent.height;
  if (!pass_open && waiting_clear.kind && !apart) {
    int fits = rectangle_holds(&waiting_clear.area, area);
    start_pass_with_clear();
    if (fits) return;
  }
  depth_use.read_depth = depth_use.saw_depth_clear = 0;
  VkRect2D wanted = *area;
  if (pass_open) {
    // Below the picture is apart from it, and joining them would reload the whole picture.
    int apart = area->offset.y >= (int64_t)pass_area.offset.y + pass_area.extent.height;
    if (!apart) wanted = rectangle_union(&pass_area, area);
    end_pass();
  }
  VkRenderPassBeginInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = vk.render_pass, .framebuffer = framebuffer_for(0),
                                .renderArea = wanted};
  vkCmdBeginRenderPass(vk.frame->draw, &info, VK_SUBPASS_CONTENTS_INLINE);
  pass_open = 1;
  pass_area = wanted;
  gx_stats.passes++;
}

// Sends what's recorded so far and waits for all of it, for when a frame's memory runs out.
static void finish_commands(void) {
  give_up_layer();
  leave_strip();
  Frame *frame = vk.frame;
  end_pass();
  vk_frame_submit(frame, VK_NULL_HANDLE, VK_NULL_HANDLE);
  begin_frame(frame);
}

static void write_shape_indices(void) {
  shape_indices = vk_create_ring(INDEX_COUNT * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  uint16_t *indices = (uint16_t *)shape_indices.mapped;
  static const int corners[INDICES_PER_QUAD] = {0, 1, 2, 0, 2, 3};
  for (int quad = 0; quad < QUADS; quad++) {
    for (int i = 0; i < INDICES_PER_QUAD; i++) indices[FIRST_QUAD_INDEX + quad * INDICES_PER_QUAD + i] = (uint16_t)(quad * VERTICES_PER_QUAD + corners[i]);
  }
  for (int i = 0; i + 2 < MAX_VERTICES; i++) {
    uint16_t *strip = &indices[FIRST_STRIP_INDEX + i * INDICES_PER_TRIANGLE], *fan = &indices[FIRST_FAN_INDEX + i * INDICES_PER_TRIANGLE];
    // Every other triangle of a strip turns the other way, so swap two corners to keep its face.
    strip[0] = (uint16_t)(i + (i & 1)); strip[1] = (uint16_t)(i + 1 - (i & 1)); strip[2] = (uint16_t)(i + 2);
    fan[0] = 0; fan[1] = (uint16_t)(i + 1); fan[2] = (uint16_t)(i + 2);
  }
}

static void start(const GxAttribute *attributes, int attribute_count, size_t bytes) {
  vertex_bytes = bytes;
  viewport_wanted = (VkViewport){0, EFB_PIXEL_HEIGHT, EFB_PIXEL_WIDTH, -(float)EFB_PIXEL_HEIGHT, 0, 1};
  vk_create_device();
  create_render_pass();
  vk_pipelines_start(attributes, attribute_count, bytes);
  vk_create_frames();
  create_efb();
  if (vk.surface) vk_create_swapchain();
  write_shape_indices();
  begin_frame(&vk.frames[0]);
  blank_efb(vk.frame->draw);
  clear_shader = vk_shader_link(vk_stage_compile(GX_STAGE_VERTEX, clear_vertex_source), vk_stage_compile(GX_STAGE_FRAGMENT, clear_fragment_source));
  static const uint8_t black[BYTES_PER_PIXEL] = {0, 0, 0, 255};  // What OpenGL samples from an empty texture.
  blank_texture = texture_create();
  upload_pixels(blank_texture, 1, 1, 1, black);
}

static void shader_use(GxShader shader) { shader_in_use = shader; }

static void constants_send(int block, const void *data, size_t bytes) {
  if (bytes > GX_CONSTANT_BYTES) { fprintf(stderr, "stop: vulkan: constant block too large\n"); exit(10); }
  if (block_bytes[block] == bytes && !memcmp(blocks[block], data, bytes)) return;
  memcpy(blocks[block], data, bytes);
  block_bytes[block] = (uint32_t)bytes;
  blocks_waiting |= 1u << block;
}

static void render_state(const GxRenderState *state) { state_in_use = *state; }

// Keeps shaders in OpenGL's clip space, where up is positive. Depth runs 0 to 1, as
// the shaders' GX_DEPTH sees to.
static void viewport(const GxViewport *v) {
  float width = v->width * EFB_SCALE, height = v->height * EFB_SCALE;
  if (width <= 0 || height <= 0) return;
  viewport_wanted = (VkViewport){v->x * EFB_SCALE, (v->y + v->height) * EFB_SCALE, width, -height, v->depth_near, v->depth_far};
}

// The game's rectangles can go past the picture, which Vulkan refuses.
static VkRect2D clip_to_efb(const GxRectangle *area) {
  int left = area->x * EFB_SCALE, top = area->y * EFB_SCALE;
  int right = (area->x + area->width) * EFB_SCALE, bottom = (area->y + area->height) * EFB_SCALE;
  if (left < 0) left = 0;
  if (top < 0) top = 0;
  if (right > EFB_PIXEL_WIDTH) right = EFB_PIXEL_WIDTH;
  if (bottom > EFB_PIXEL_HEIGHT) bottom = EFB_PIXEL_HEIGHT;
  if (right < left) right = left;
  if (bottom < top) bottom = top;
  return (VkRect2D){{left, top}, {(uint32_t)(right - left), (uint32_t)(bottom - top)}};
}

static void scissor(const GxRectangle *area) { scissor_wanted = clip_to_efb(area); }

// The game often leaves the scissor wide and narrows only the viewport, and nothing
// lands outside either. Scissoring to this keeps every draw inside its pass's area.
static VkRect2D drawn_area(void) {
  int32_t left = (int32_t)viewport_wanted.x, right = (int32_t)ceilf(viewport_wanted.x + viewport_wanted.width);
  int32_t bottom = (int32_t)ceilf(viewport_wanted.y), top = (int32_t)(viewport_wanted.y + viewport_wanted.height);
  int32_t scissor_right = scissor_wanted.offset.x + (int32_t)scissor_wanted.extent.width;
  int32_t scissor_bottom = scissor_wanted.offset.y + (int32_t)scissor_wanted.extent.height;
  if (left < scissor_wanted.offset.x) left = scissor_wanted.offset.x;
  if (top < scissor_wanted.offset.y) top = scissor_wanted.offset.y;
  if (right > scissor_right) right = scissor_right;
  if (bottom > scissor_bottom) bottom = scissor_bottom;
  if (gx_bounds_known) {
    enum { MARGIN = 2 * EFB_SCALE };
    float x = viewport_wanted.x, y = viewport_wanted.y, w = viewport_wanted.width / 2, h = viewport_wanted.height / 2;
    float x0 = x + (gx_bounds[0] + 1) * w, x1 = x + (gx_bounds[2] + 1) * w;
    float y0 = y + (gx_bounds[1] + 1) * h, y1 = y + (gx_bounds[3] + 1) * h;
    if (y0 > y1) { float swap = y0; y0 = y1; y1 = swap; }
    if (x0 > left - MARGIN) left = (int32_t)x0 - MARGIN > left ? (int32_t)x0 - MARGIN : left;
    if (y0 > top - MARGIN) top = (int32_t)y0 - MARGIN > top ? (int32_t)y0 - MARGIN : top;
    if (x1 < right + MARGIN) right = (int32_t)ceilf(x1) + MARGIN < right ? (int32_t)ceilf(x1) + MARGIN : right;
    if (y1 < bottom + MARGIN) bottom = (int32_t)ceilf(y1) + MARGIN < bottom ? (int32_t)ceilf(y1) + MARGIN : bottom;
  }
  if (right <= left || bottom <= top) return scissor_wanted;
  return (VkRect2D){{left, top}, {(uint32_t)(right - left), (uint32_t)(bottom - top)}};
}

static uint32_t level_side(uint32_t size, uint32_t level) { return size >> level ? size >> level : 1; }

static void upload_pixels(GxTexture *texture, uint32_t width, uint32_t height, uint32_t levels, const uint8_t *rgba) {
  VkDeviceSize bytes = 0;
  for (uint32_t level = 0; level < levels; level++) bytes += (VkDeviceSize)level_side(width, level) * level_side(height, level) * BYTES_PER_PIXEL;
  VkDeviceSize at = vk_ring_take(&vk.frame->staging, bytes, BYTES_PER_PIXEL);
  if (at == (VkDeviceSize)-1) {
    finish_commands();
    at = vk_ring_take(&vk.frame->staging, bytes, BYTES_PER_PIXEL);
    if (at == (VkDeviceSize)-1) { fprintf(stderr, "stop: vulkan: texture larger than the staging memory\n"); exit(10); }
  }
  memcpy(vk.frame->staging.mapped + at, rgba, bytes);

  // Always a new image: draws already recorded this frame run after the upload
  // commands, and must still see the old picture, as may a frame on the card.
  if (texture->image) vk_retire(texture->image, texture->view, texture->memory);
  retire_attachment(texture);
  vk_create_image(COLOR_FORMAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, width, height, levels, &texture->image, &texture->memory);
  VkComponentMapping plain = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  texture->view = vk_create_view(texture->image, COLOR_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, levels, plain);
  texture->width = (int)width;
  texture->height = (int)height;
  texture->written = 1;

  if (in_strip) end_pass();
  VkCommandBuffer commands = vk_upload_commands();
  barrier(commands, texture->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  for (uint32_t level = 0; level < levels; level++) {
    uint32_t level_width = level_side(width, level), level_height = level_side(height, level);
    VkBufferImageCopy region = {.bufferOffset = at, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1}, .imageExtent = {level_width, level_height, 1}};
    vkCmdCopyBufferToImage(commands, vk.frame->staging.buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    at += (VkDeviceSize)level_width * level_height * BYTES_PER_PIXEL;
  }
  barrier(commands, texture->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

static void texture_bind(int map, GxTexture *texture, const GxSampling *sampling) {
  bindings[map].texture = texture;
  bindings[map].sampling = *sampling;
}

// Bindings are looked at only now, so a texture's newest view is the one used.
static void bind_maps(VkCommandBuffer commands) {
  VkDescriptorImageInfo maps[MAX_TEXTURE_MAPS];
  memset(maps, 0, sizeof maps);
  for (int map = 0; map < MAX_TEXTURE_MAPS; map++) {
    const GxTexture *texture = bindings[map].texture && bindings[map].texture->written ? bindings[map].texture : blank_texture;
    maps[map].sampler = vk_sampler_find(&bindings[map].sampling, texture->depth);
    maps[map].imageView = texture->view;
    maps[map].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  if (!memcmp(maps, maps_set, sizeof maps)) return;
  VkWriteDescriptorSet writes[MAX_TEXTURE_MAPS];
  for (uint32_t map = 0; map < MAX_TEXTURE_MAPS; map++) {
    writes[map] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = map, .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &maps[map]};
  }
  if (vk.push_descriptors) {
    vk.push_descriptors(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout, SET_TEXTURES, MAX_TEXTURE_MAPS, writes);
  } else {
    VkDescriptorSet set = vk_texture_set(vk.frame);
    for (uint32_t map = 0; map < MAX_TEXTURE_MAPS; map++) writes[map].dstSet = set;
    vkUpdateDescriptorSets(vk.device, MAX_TEXTURE_MAPS, writes, 0, 0);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout, SET_TEXTURES, 1, &set, 0, 0);
  }
  memcpy(maps_set, maps, sizeof maps);
}

// Places waiting constant blocks in this frame's memory. Says if there wasn't room.
static int place_constants(VkCommandBuffer commands) {
  Ring *ring = &vk.frame->constants;
  VkDeviceSize alignment = vk.limits.minUniformBufferOffsetAlignment;
  if (!blocks_waiting) return 1;
  for (int i = 0; i < GX_CONSTANT_BLOCKS; i++) {
    if (!(blocks_waiting & (1u << i))) continue;
    VkDeviceSize at = vk_ring_take(ring, GX_CONSTANT_BYTES, alignment);
    if (at == (VkDeviceSize)-1) return 0;
    // Only what was sent is copied, and later blocks may overlap the rest of this range.
    ring->used = at + block_bytes[i];
    memcpy(ring->mapped + at, blocks[i], block_bytes[i]);
    block_offsets[i] = (uint32_t)at;
    blocks_waiting &= ~(1u << i);
  }
  vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout, SET_CONSTANTS, 1, &vk.frame->constant_set,
                          GX_CONSTANT_BLOCKS, block_offsets);
  return 1;
}

static void set_pipeline(VkCommandBuffer commands, VkPipeline pipeline) {
  if (pipeline == pipeline_set) return;
  vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  pipeline_set = pipeline;
}

static void set_viewport_and_scissor(VkCommandBuffer commands, const VkViewport *viewport_now, const VkRect2D *scissor_now) {
  if (memcmp(viewport_now, &viewport_set, sizeof viewport_set)) {
    vkCmdSetViewport(commands, 0, 1, viewport_now);
    viewport_set = *viewport_now;
  }
  if (memcmp(scissor_now, &scissor_set, sizeof scissor_set)) {
    vkCmdSetScissor(commands, 0, 1, scissor_now);
    scissor_set = *scissor_now;
  }
}

static void *vertices_reserve(int count) {
  VkDeviceSize bytes = (VkDeviceSize)count * vertex_bytes;
  reserved_at = vk_ring_take(&vk.frame->vertices, bytes, vertex_bytes);
  if (reserved_at == (VkDeviceSize)-1) {
    finish_commands();
    reserved_at = vk_ring_take(&vk.frame->vertices, bytes, vertex_bytes);
  }
  reserved_restarts = restarts;
  return vk.frame->vertices.mapped + reserved_at;
}

static void early_depth_changed(void);
// A page that covers the whole picture makes all drawing before it wasted, however much
// there was. The commands recorded so far haven't run, so they can simply be thrown away.
// If the page never comes, nothing was lost. It's known from the frame before, because
// a page is often two bands, and the first must survive when the rest arrive.

static int cover_band(int count, float box[4]) {
  if (count > 4 || !state_in_use.color_write || state_in_use.depth_test_on) return 0;
  int plain = state_in_use.blend_source == GX_FACTOR_SOURCE_ALPHA && state_in_use.blend_target == GX_FACTOR_INVERSE_SOURCE_ALPHA;
  int second = state_in_use.blend_source == GX_FACTOR_SOURCE1_ALPHA && state_in_use.blend_target == GX_FACTOR_INVERSE_SOURCE1_ALPHA;
  if (state_in_use.blend_on && !plain && !second) return 0;
  if (viewport_wanted.width < EFB_WIDTH * EFB_SCALE || -viewport_wanted.height < VISIBLE_HEIGHT * EFB_SCALE) return 0;
  if (viewport_wanted.x != 0 || scissor_wanted.extent.width < (uint32_t)(EFB_WIDTH * EFB_SCALE)) return 0;
  if (!gx_transform_bounds(gx_reserved, count, box) || box[0] > -1 || box[2] < 1) return 0;
  return gx_output_is_opaque();
}

static void drop_recorded_commands(void) {
  Frame *frame = vk.frame;
  vk_check(vkResetCommandBuffer(frame->draw, 0), "resetting commands");
  VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  vk_check(vkBeginCommandBuffer(frame->draw, &begin), "starting commands");
  pass_open = 0;
  depth_use.number = 0;
  forget_set_state();
  // The page covers everything, so the old picture needn't be loaded.
  waiting_clear.kind = CLEARS_COLOR | CLEARS_DEPTH;
  waiting_clear.area = (VkRect2D){{0, 0}, {(uint32_t)(EFB_WIDTH * EFB_SCALE), (uint32_t)(VISIBLE_HEIGHT * EFB_SCALE)}};
  memset(waiting_clear.values, 0, sizeof waiting_clear.values);
  waiting_clear.values[1].depthStencil.depth = 0;  // What fell behind the page stays behind it.
  if (capturing) { vkCmdResetQueryPool(vk.frame->draw, mark_pool, 0, MARKS); marks = 0; mark_draws = gx_stats.draws; mark("start", 0, 0); }
}

// Games copy the picture out, clear, and draw the copy straight back, ready for effects.
// Drawing it back gives what the picture already held, so the clear and draw can go.
// Every corner must map onto itself, in white, from the newest copy of the whole picture.
static struct { GxTexture *texture; uint32_t frame; } newest_copy;

// Whether the color stages hand on the first texture exactly, color and alpha alike: one
// plain stage times a white vertex, with nothing after it that changes a pixel, and no
// fog unless it's proven to add nothing.
static int passes_texture_through(const TevKey *key, const GxTexture *texture) {
  enum { SWAP_IDENTITY = 0 | 1 << 2 | 2 << 4 | 3 << 6 };
  const TevStage *s = &key->stage[0];
  if (key->stages != 1 || key->alpha_test || key->constant_alpha || key->logic_help || key->wrap_output || key->bump_alpha) return 0;
  if ((key->fog_type && !key->fog_none) || key->depth_mode || key->no_coordinates || (key->projected & 1)) return 0;
  if (s->color_in[0] != 15 || s->color_in[1] != 8 || s->color_in[2] != 10 || s->color_in[3] != 15) return 0;
  if (s->alpha_in[0] != 7 || s->alpha_in[1] != 4 || s->alpha_in[2] != 5 || s->alpha_in[3] != 7) return 0;
  if (s->color_bias || s->color_subtract || s->color_scale || s->color_target) return 0;
  if (s->alpha_bias || s->alpha_subtract || s->alpha_scale || s->alpha_target || s->map != 0 || s->coordinate != 0) return 0;
  if (s->indirect || !s->texture_on || s->texture_swap != SWAP_IDENTITY) return 0;
  const GxChannels *c = &texture->channels;
  return c->red == GX_CHANNEL_RED && c->green == GX_CHANNEL_GREEN && c->blue == GX_CHANNEL_BLUE && c->alpha == GX_CHANNEL_ALPHA;
}

// Whether four vertices are the whole picture's corners, each reading the texture where it
// lands, in a viewport that is the whole picture: a copy drawn back where it came from.
static int draws_copy_where_it_was(int count) {
  if (count != 4 || viewport_wanted.x != 0 || viewport_wanted.width != EFB_WIDTH * EFB_SCALE || -viewport_wanted.height != VISIBLE_HEIGHT * EFB_SCALE) return 0;
  if (scissor_wanted.offset.x || scissor_wanted.offset.y || scissor_wanted.extent.width < (uint32_t)(EFB_WIDTH * EFB_SCALE)
      || scissor_wanted.extent.height < (uint32_t)(VISIBLE_HEIGHT * EFB_SCALE)) return 0;
  int corners = 0;
  for (int i = 0; i < 4; i++) {
    const Vertex *v = &gx_reserved[i];
    float x, y;
    if (!gx_transform_screen(v, &x, &y)) return 0;
    for (int channel = 0; channel < 4; channel++) if (v->color[0][channel] != 1) return 0;
    if (fabsf((x + 1) / 2 - v->uv[0][0]) > 0.0005f || fabsf((1 - y) / 2 - v->uv[0][1]) > 0.0005f) return 0;
    if (fabsf(fabsf(x) - 1) > 0.0001f || fabsf(fabsf(y) - 1) > 0.0001f) return 0;
    corners |= 1 << ((x > 0) + 2 * (y > 0));
  }
  return corners == 15;
}

static int redraws_newest_copy(int count) {
  static int off = -1;
  if (off < 0) off = getenv("WII_RECOMP_KEEP_COPY_BACK") != 0;
  extern TevKey gx_last_tev;
  const TevKey *key = &gx_last_tev;
  if (off || in_strip || count != 4 || newest_copy.frame != frame_serial || bindings[0].texture != newest_copy.texture) return 0;
  if (!(waiting_clear.kind & CLEARS_COLOR) || pass_open || state_in_use.blend_on || !state_in_use.color_write || !state_in_use.alpha_write) return 0;
  if (state_in_use.depth_write || state_in_use.depth_test_on || !passes_texture_through(key, newest_copy.texture)) return 0;
  return draws_copy_where_it_was(count);
}

static void watch_for_cover(int count) {
  static int off = -1;
  if (off < 0) off = getenv("WII_RECOMP_COVER") == 0;  // Proven on one screen so far, so asked for.
  if (off || in_strip) return;
  float box[4];
  if (!cover_band(count, box)) { cover.bands = 0; return; }
  if (cover.first_shader == shader_in_use && !cover.recorded_anything_lasting && !memcmp(box, cover.first_box, sizeof box)) drop_recorded_commands();
  if (cover.bands && box[3] >= cover.bottom - 0.001f && box[1] <= cover.bottom + 0.001f) {
    cover.bottom = box[1] < cover.bottom ? box[1] : cover.bottom;
    cover.top = box[3] > cover.top ? box[3] : cover.top;
  } else if (cover.bands && box[1] <= cover.top + 0.001f && box[3] >= cover.top - 0.001f) {
    cover.bottom = box[1] < cover.bottom ? box[1] : cover.bottom;
    cover.top = box[3] > cover.top ? box[3] : cover.top;
  } else {
    cover.bands = 0;
  }
  if (!cover.bands) {
    cover.bands = 1;
    cover.bottom = box[1];
    cover.top = box[3];
    cover.candidate_shader = shader_in_use;
    memcpy(cover.candidate_box, box, sizeof box);
  }
  if (cover.bottom <= -1 && cover.top >= 1) {
    cover.next_shader = cover.candidate_shader;
    memcpy(cover.next_box, cover.candidate_box, sizeof cover.next_box);
  }
}

// Only the maps this draw reads: the others still hold whatever earlier draws bound.
static int reads_this_frames_copy(void) {
  extern unsigned gx_used_maps;
  for (int map = 0; map < MAX_TEXTURE_MAPS; map++) {
    if ((gx_used_maps & (1u << map)) && bindings[map].texture && bindings[map].texture->copied_frame == frame_serial) return 1;
  }
  return 0;
}

static void draw(int primitive, int count) {
  if (!count) return;
  {
    // In the strip, a pass should touch only the small pictures being made, and those
    // draws are few and tiny, so it's worth working out exactly where they land.
    gx_bounds_known = viewport_wanted.y + viewport_wanted.height >= VISIBLE_HEIGHT * EFB_SCALE && count <= 64
                      && gx_transform_bounds(gx_reserved, count, gx_bounds);
    VkRect2D where = drawn_area();
    int strip = where.offset.y >= VISIBLE_HEIGHT * EFB_SCALE;
    if (strip && reads_this_frames_copy()) {
      static int told;
      if (!told++) fprintf(stderr, "strip: a draw reads this frame's copy, so it stays in order\n");
      strip = 0;
      strip_rest = 120;
    }
    route_work(strip);
  }
  if (layer_draw(count)) return;
  { extern unsigned gx_used_maps;
    for (int m = 0; m < MAX_TEXTURE_MAPS; m++) {
      if ((gx_used_maps & (1u << m)) && bindings[m].texture && bindings[m].texture->owed) {
        static int told;
        GxTexture *t = bindings[m].texture;
        static const char *const roles[] = {"", "picture", "layer", "depth"};
        if (!told++) fprintf(stderr, "layer: a skipped %s copy%s%s was read %u frames later by %d vertices with shader %u, so skipping stops\n",
                             roles[t->owed & 3], t->owed & 4 ? " (given up late)" : "", t->owed & 8 ? " (drawn apart)" : "", frame_serial - t->owed_frame, count, shader_in_use);
        empty_layer.off = 1;
      }
    }
  }
  if (redraws_newest_copy(count)) {
    waiting_clear.kind &= ~CLEARS_COLOR;
    newest_copy.frame = 0;
    return;
  }
  if (!in_strip) newest_copy.frame = 0;
  watch_for_cover(count);
  if (!place_constants(vk.frame->draw)) {
    finish_commands();
    place_constants(vk.frame->draw);
  }
  // Starting over since the reservation, for a large texture say, freed its room.
  // The vertices are still there, but must be claimed again before anything lands on them.
  Frame *frame = vk.frame;
  if (restarts != reserved_restarts) {
    VkDeviceSize bytes = (VkDeviceSize)count * vertex_bytes, at = vk_ring_take(&frame->vertices, bytes, vertex_bytes);
    memmove(frame->vertices.mapped + at, frame->vertices.mapped + reserved_at, bytes);
    reserved_at = at;
  }
  VkCommandBuffer commands = frame->draw;
  if (state_in_use.depth_write) early_depth_changed();
  if (!depth_use.frame_began && !in_strip && state_in_use.depth_test_on) { depth_use.frame_began = 1; memset(depth_use.no_store, 0, sizeof depth_use.no_store); }
  if (state_in_use.depth_test_on && !in_strip && !depth_use.saw_depth_clear) {
    depth_use.read_depth = 1;
    if (depth_use.number < MAX_PASSES && depth_use.no_load[depth_use.number]) forget_depth_use();
  }
  VkRect2D area = drawn_area();
  begin_pass(&area);
  set_pipeline(commands, vk_pipeline_find(shader_in_use, &state_in_use));
  set_viewport_and_scissor(commands, &viewport_wanted, &area);
  bind_maps(commands);
  VkDeviceSize start_of_buffer = 0;
  vkCmdBindVertexBuffers(commands, 0, 1, &frame->vertices.buffer, &start_of_buffer);
  int32_t first = (int32_t)(reserved_at / vertex_bytes);
  if (primitive == PRIM_TRIANGLES) {
    vkCmdDraw(commands, (uint32_t)count, 1, (uint32_t)first, 0);
    return;
  }
  if (!indices_bound) {
    vkCmdBindIndexBuffer(commands, shape_indices.buffer, 0, VK_INDEX_TYPE_UINT16);
    indices_bound = 1;
  }
  if (primitive == PRIM_STRIP || primitive == PRIM_FAN) {
    uint32_t triangles = (uint32_t)(count - 2);
    vkCmdDrawIndexed(commands, triangles * INDICES_PER_TRIANGLE, 1, primitive == PRIM_STRIP ? FIRST_STRIP_INDEX : FIRST_FAN_INDEX, first, 0);
  } else {
    vkCmdDrawIndexed(commands, (uint32_t)(count / VERTICES_PER_QUAD) * INDICES_PER_QUAD, 1, FIRST_QUAD_INDEX, first, 0);
  }
}

// Vulkan's own clears ignore the write flags, so this draws over the rectangle instead.
static void clear_now(const GxClear *c) {
  if (!c->color_write && !c->alpha_write && !c->depth_write) return;
  route_work(clip_to_efb(&c->area).offset.y >= VISIBLE_HEIGHT * EFB_SCALE);
  if (c->depth_write) early_depth_changed();
  if (c->depth_write && !in_strip) {
    VkRect2D wanted = clip_to_efb(&c->area);
    VkRect2D visible = {{0, 0}, {EFB_PIXEL_WIDTH, VISIBLE_HEIGHT * EFB_SCALE}};
    int all = rectangle_holds(&wanted, &visible), n = depth_use.number;
    // The frame opens with a whole clear, so the last pass of the one before kept nothing.
    if (all && !depth_use.frame_began && n == 0) depth_use.no_store[depth_use.last_in_frame] = 1;
    depth_use.frame_began = 1;
    if (pass_open && !depth_use.read_depth && !depth_use.saw_depth_clear && all && n < MAX_PASSES) {
      int learned = depth_use.no_load[n] && depth_use.clear_depth == (float)c->depth;
      depth_use.no_load[n] = 1;
      depth_use.clear_depth = (float)c->depth;
      depth_use.saw_depth_clear = 1;
      if (learned && !c->color_write && !c->alpha_write) return;  // The pass began with it.
    }
  }
  static int off = -1;
  if (off < 0) off = getenv("WII_RECOMP_DRAW_CLEARS") != 0;
  if (!off && !pass_open && c->color_write == c->alpha_write) {
    VkRect2D wanted = clip_to_efb(&c->area);
    int replaces = rectangle_holds(&wanted, &waiting_clear.area) && !(waiting_clear.kind & ~((c->color_write ? CLEARS_COLOR : 0) | (c->depth_write ? CLEARS_DEPTH : 0)));
    if (waiting_clear.kind && replaces) waiting_clear.kind = 0;  // Nothing could have seen it.
    end_pass();  // Any other clear already waiting.
    waiting_clear.kind = (c->color_write ? CLEARS_COLOR : 0) | (c->depth_write ? CLEARS_DEPTH : 0);
    waiting_clear.area = clip_to_efb(&c->area);
    memcpy(waiting_clear.values[0].color.float32, c->color, sizeof c->color);
    waiting_clear.values[1].depthStencil.depth = (float)c->depth;
    return;
  }
  GxRenderState state;
  memset(&state, 0, sizeof state);
  state.color_write = (uint8_t)c->color_write;
  state.alpha_write = (uint8_t)c->alpha_write;
  state.depth_test_on = state.depth_write = (uint8_t)c->depth_write;  // Depth is only written while testing is on.
  state.depth_test = GX_TEST_ALWAYS;
  ClearValues values;
  memcpy(values.color, c->color, sizeof values.color);
  values.depth = (float)c->depth;
  VkCommandBuffer commands = vk.frame->draw;
  VkRect2D area = clip_to_efb(&c->area);
  begin_pass(&area);
  set_pipeline(commands, vk_pipeline_find(clear_shader, &state));
  VkViewport whole = {0, 0, EFB_PIXEL_WIDTH, EFB_PIXEL_HEIGHT, 0, 1};
  set_viewport_and_scissor(commands, &whole, &area);
  vkCmdPushConstants(commands, vk.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof values, &values);
  vkCmdDraw(commands, 3, 1, 0, 0);
}

// Clips a copy's source to the picture, moving the target's edges to keep the scale.
static void blit_areas(const GxRectangle *from, int width, int height, VkOffset3D source[2], VkOffset3D target[2]) {
  VkRect2D clipped = clip_to_efb(from);
  int full_width = from->width * EFB_SCALE, full_height = from->height * EFB_SCALE;
  int skipped_x = clipped.offset.x - from->x * EFB_SCALE, skipped_y = clipped.offset.y - from->y * EFB_SCALE;
  source[0] = (VkOffset3D){clipped.offset.x, clipped.offset.y, 0};
  source[1] = (VkOffset3D){clipped.offset.x + (int32_t)clipped.extent.width, clipped.offset.y + (int32_t)clipped.extent.height, 1};
  target[0] = (VkOffset3D){skipped_x * width / full_width, skipped_y * height / full_height, 0};
  target[1] = (VkOffset3D){(skipped_x + (int)clipped.extent.width) * width / full_width, (skipped_y + (int)clipped.extent.height) * height / full_height, 1};
}

static VkExtent3D blit_extent(const VkImageBlit *blit) {
  return (VkExtent3D){(uint32_t)(blit->srcOffsets[1].x - blit->srcOffsets[0].x), (uint32_t)(blit->srcOffsets[1].y - blit->srcOffsets[0].y), 1};
}

// A plain copy is much cheaper than a blit on some chips, when nothing needs scaling.
static int same_size(const VkImageBlit *blit) {
  return blit->srcOffsets[1].x - blit->srcOffsets[0].x == blit->dstOffsets[1].x - blit->dstOffsets[0].x
      && blit->srcOffsets[1].y - blit->srcOffsets[0].y == blit->dstOffsets[1].y - blit->dstOffsets[0].y;
}

static VkComponentSwizzle swizzle(uint8_t channel) {
  static const VkComponentSwizzle names[] = {[GX_CHANNEL_RED] = VK_COMPONENT_SWIZZLE_R, [GX_CHANNEL_GREEN] = VK_COMPONENT_SWIZZLE_G, [GX_CHANNEL_BLUE] = VK_COMPONENT_SWIZZLE_B,
                                             [GX_CHANNEL_ALPHA] = VK_COMPONENT_SWIZZLE_A, [GX_CHANNEL_ONE] = VK_COMPONENT_SWIZZLE_ONE};
  return names[channel];
}

// Gets `to` ready as a copy's target. A new size means a new image; channels are part
// of the view, so new channels mean a new view of the same image.
static void prepare_copy(GxTexture *to, int width, int height, VkFormat format, VkImageAspectFlags aspect, GxChannels channels) {
  VkComponentMapping mapping = {swizzle(channels.red), swizzle(channels.green), swizzle(channels.blue), swizzle(channels.alpha)};
  if (!to->image || to->width != width || to->height != height) {
    if (to->image) vk_retire(to->image, to->view, to->memory);
    retire_attachment(to);
    vk_create_image(format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, (uint32_t)width, (uint32_t)height, 1, &to->image, &to->memory);
    to->view = vk_create_view(to->image, format, aspect, 1, mapping);
    to->width = width;
    to->height = height;
    to->written = 0;
  } else if (memcmp(&to->channels, &channels, sizeof channels)) {
    vk_retire(VK_NULL_HANDLE, to->view, VK_NULL_HANDLE);
    to->view = vk_create_view(to->image, format, aspect, 1, mapping);
  }
  to->channels = channels;
  to->owed = 0;
  to->depth = aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
  VkCommandBuffer commands = vk.frame->draw;
  barrier(commands, to->image, aspect, 0, 1, to->written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
}

static void finish_copy(GxTexture *to, VkImageAspectFlags aspect) {
  barrier(vk.frame->draw, to->image, aspect, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  to->written = 1;
}

static void copy_depth_early(const GxRectangle *from);
static void copy_color_now(GxTexture *to, const GxRectangle *from, int width, int height, GxChannels channels, int smooth) {
  route_work(from->y >= VISIBLE_HEIGHT);
  if (!in_strip) { to->copied_frame = frame_serial; cover.recorded_anything_lasting = 1; }
  else if (to->strip_frame == frame_serial) { static int told; if (!told++) fprintf(stderr, "strip: a target was copied to twice in a frame\n"); }
  to->strip_frame = frame_serial;
  if (width <= 0 || height <= 0) return;
  // A copy of a clear that's still waiting is one flat color, so nothing need be stored.
  VkRect2D source = clip_to_efb(from);
  if (!pass_open && (waiting_clear.kind & CLEARS_COLOR) && rectangle_holds(&waiting_clear.area, &source)
      && source.extent.width == (uint32_t)(from->width * EFB_SCALE) && source.extent.height == (uint32_t)(from->height * EFB_SCALE)) {
    prepare_copy(to, width, height, COLOR_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, channels);
    VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(vk.frame->draw, to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &waiting_clear.values[0].color, 1, &whole);
    finish_copy(to, VK_IMAGE_ASPECT_COLOR_BIT);
    mark("clear instead of copy", width, height);
    return;
  }
  if (pass_open || from->y < VISIBLE_HEIGHT) end_pass();
  VkCommandBuffer commands = vk.frame->draw;
  prepare_copy(to, width, height, COLOR_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, channels);
  efb_color_to_source(commands);
  VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}};
  blit_areas(from, width, height, blit.srcOffsets, blit.dstOffsets);
  if (same_size(&blit)) vkCmdCopyImage(commands, vk.efb_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkImageCopy){blit.srcSubresource, blit.srcOffsets[0], blit.dstSubresource, blit.dstOffsets[0], blit_extent(&blit)});
  else vkCmdBlitImage(commands, vk.efb_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  efb_color_to_target(commands);
  finish_copy(to, VK_IMAGE_ASPECT_COLOR_BIT);
  mark("copy color", width, height);
  if (from->x == 0 && from->y == 0 && from->width == EFB_WIDTH && from->height == VISIBLE_HEIGHT && width == EFB_WIDTH * EFB_SCALE && !in_strip) {
    newest_copy.texture = to;
    newest_copy.frame = frame_serial;
  }
  copy_depth_early(from);
}

static void copy_depth_now(GxTexture *to, const GxRectangle *from, int width, int height) {
  if (width <= 0 || height <= 0) return;
  end_pass();
  VkCommandBuffer commands = vk.frame->draw;
  GxChannels channels = {GX_CHANNEL_RED, GX_CHANNEL_RED, GX_CHANNEL_RED, GX_CHANNEL_ONE};
  prepare_copy(to, width, height, DEPTH_FORMAT, VK_IMAGE_ASPECT_DEPTH_BIT, channels);
  barrier(commands, vk.efb_depth, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          DEPTH_ATTACHMENT_STAGE, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}};
  blit_areas(from, width, height, blit.srcOffsets, blit.dstOffsets);
  if (same_size(&blit)) vkCmdCopyImage(commands, vk.efb_depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkImageCopy){blit.srcSubresource, blit.srcOffsets[0], blit.dstSubresource, blit.dstOffsets[0], blit_extent(&blit)});
  else vkCmdBlitImage(commands, vk.efb_depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
  barrier(commands, vk.efb_depth, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, DEPTH_ATTACHMENT_STAGE, DEPTH_ATTACHMENT_ACCESS);
  finish_copy(to, VK_IMAGE_ASPECT_DEPTH_BIT);
  mark("copy depth", width, height);
}

// The game copies depth after drawing on top of the scene, which would split that work
// in two for a tile-based chip. Depth is usually unchanged by then, so last frame's copy
// is made as soon as the scene ends, and the game's own request finds it done.
static struct { GxTexture *to; GxRectangle from; int width, height; uint32_t done_frame; int changed; } early_depth;

static void copy_depth_early(const GxRectangle *from) {
  static int off = -1;
  if (off < 0) off = getenv("WII_RECOMP_NO_EARLY_DEPTH") != 0;
  if (off || !early_depth.to || in_strip || early_depth.done_frame == frame_serial || memcmp(from, &early_depth.from, sizeof *from)) return;
  copy_depth_now(early_depth.to, from, early_depth.width, early_depth.height);
  early_depth.done_frame = frame_serial;
  early_depth.changed = 0;
}

static void early_depth_changed(void) { early_depth.changed = 1; }

static void copy_depth_requested(GxTexture *to, const GxRectangle *from, int width, int height) {
  route_work(from->y >= VISIBLE_HEIGHT);
  if (!in_strip) { to->copied_frame = frame_serial; cover.recorded_anything_lasting = 1; }
  if (early_depth.done_frame == frame_serial && !early_depth.changed && early_depth.to == to && early_depth.width == width
      && early_depth.height == height && !memcmp(from, &early_depth.from, sizeof *from)) return;
  copy_depth_now(to, from, width, height);
  if (!in_strip) {
    early_depth.to = to;
    early_depth.from = *from;
    early_depth.width = width;
    early_depth.height = height;
  }
}

static void blit_to_window(VkCommandBuffer commands, uint32_t image_index, const GxRectangle *from) {
  VkImage image = vk.swap_images[image_index];
  barrier(commands, image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  efb_color_to_source(commands);
  VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}};
  blit_areas(from, (int)vk.swap_extent.width, (int)vk.swap_extent.height, blit.srcOffsets, blit.dstOffsets);
  vkCmdBlitImage(commands, vk.efb_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
  efb_color_to_target(commands);
  barrier(commands, image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
}

VkPictureSink vk_picture_sink;
static Ring picture_ring;
static VkExtent2D picture_size;

static void read_picture_back(VkCommandBuffer commands, const GxRectangle *from) {
  VkRect2D area = clip_to_efb(from);
  picture_size = area.extent;
  if (!area.extent.width || !area.extent.height) return;
  if (!picture_ring.buffer) picture_ring = vk_create_ring((VkDeviceSize)EFB_PIXEL_WIDTH * EFB_PIXEL_HEIGHT * BYTES_PER_PIXEL, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  efb_color_to_source(commands);
  VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {area.offset.x, area.offset.y, 0},
                              .imageExtent = {area.extent.width, area.extent.height, 1}};
  vkCmdCopyImageToBuffer(commands, vk.efb_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, picture_ring.buffer, 1, &region);
  efb_color_to_target(commands);
  VkMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, 0, 0, 0);
}

static void hand_picture_over(Frame *frame) {
  if (!picture_size.width || !picture_size.height) return;
  vk_check(vkWaitForFences(vk.device, 1, &frame->fence, VK_TRUE, UINT64_MAX), "waiting for the picture");
  vk_picture_sink(picture_ring.mapped, (int)picture_size.width, (int)picture_size.height);
}

static void present(const GxRectangle *from) {
  give_up_layer();
  leave_strip();
  Frame *frame = vk.frame;
  end_pass();
  if (vk_picture_sink) read_picture_back(frame->draw, from);
  uint32_t image_index = 0;
  VkResult acquired = vk.swapchain ? vkAcquireNextImageKHR(vk.device, vk.swapchain, ACQUIRE_TIMEOUT_NS, frame->image_ready, VK_NULL_HANDLE, &image_index) : VK_NOT_READY;
  int showing = acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR;
  int outdated = acquired == VK_SUBOPTIMAL_KHR || acquired == VK_ERROR_OUT_OF_DATE_KHR;
  if (showing) {
    blit_to_window(frame->draw, image_index, from);
    mark("to window", 0, 0);
    vk_frame_submit(frame, frame->image_ready, vk.swap_done[image_index]);
    VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &vk.swap_done[image_index],
                             .swapchainCount = 1, .pSwapchains = &vk.swapchain, .pImageIndices = &image_index};
    VkResult presented = vkQueuePresentKHR(vk.queue, &info);
    outdated |= presented == VK_SUBOPTIMAL_KHR || presented == VK_ERROR_OUT_OF_DATE_KHR;
  } else {
    vk_frame_submit(frame, VK_NULL_HANDLE, VK_NULL_HANDLE);
  }
  mark_report();
  if (vk_picture_sink) hand_picture_over(frame);
  if (vk.swapchain) {
    window_refresh_size();
    if (outdated || !vk_swapchain_matches_window()) vk_create_swapchain();
  }
  begin_frame(&vk.frames[(frame - vk.frames + 1) % FRAMES_IN_FLIGHT]);
}

// Each translucent layer goes: copy the picture out, clear to clear, draw the layer, copy
// it out, clear, draw the picture back, copy depth, clear depth, then lay the layer over
// and restore depth. The game does that even when the layer is empty, sometimes four
// times a frame, and on a tile-based chip each one stores and reloads everything. With
// nothing drawn and a clear of no alpha, it changes nothing. So the steps are held back
// until that's certain. If anything else happens they run in order, as asked.

static int is_whole_picture(const GxRectangle *r) {
  return r->x == 0 && r->y == 0 && r->width == EFB_WIDTH && r->height == VISIBLE_HEIGHT;
}

// Fills a copy with the flat color it would have taken from a cleared picture.
static void fill_copy(GxTexture *to, int width, int height, GxChannels channels, const float color[4]) {
  route_work(0);
  if (!in_strip) { to->copied_frame = frame_serial; cover.recorded_anything_lasting = 1; }
  to->strip_frame = frame_serial;
  end_pass();
  prepare_copy(to, width, height, COLOR_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, channels);
  VkClearColorValue value;
  memcpy(value.float32, color, sizeof value.float32);
  VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(vk.frame->draw, to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &whole);
  finish_copy(to, VK_IMAGE_ASPECT_COLOR_BIT);
  mark("fill instead of copy", width, height);
}

// Framebuffers outlive their last use by the frames still on the chip.
typedef struct { VkFramebuffer framebuffer; uint32_t frame; } DeadFramebuffer;
static DeadFramebuffer dead_framebuffers[16];
static int dead_count;

static void retire_framebuffer(VkFramebuffer framebuffer) {
  if (!framebuffer) return;
  for (int i = 0; i < dead_count;) {
    if (frame_serial - dead_framebuffers[i].frame > FRAMES_IN_FLIGHT + 1) {
      vkDestroyFramebuffer(vk.device, dead_framebuffers[i].framebuffer, 0);
      dead_framebuffers[i] = dead_framebuffers[--dead_count];
    } else {
      i++;
    }
  }
  if (dead_count == (int)(sizeof dead_framebuffers / sizeof dead_framebuffers[0])) {
    vkQueueWaitIdle(vk.queue);
    for (int i = 0; i < dead_count; i++) vkDestroyFramebuffer(vk.device, dead_framebuffers[i].framebuffer, 0);
    dead_count = 0;
  }
  dead_framebuffers[dead_count++] = (DeadFramebuffer){framebuffer, frame_serial};
}

// A texture that took the side picture's image keeps a view and framebuffer for drawing into
// it, and they go when its image does.
static void retire_attachment(GxTexture *t) {
  if (t->attach_view) vk_retire(VK_NULL_HANDLE, t->attach_view, VK_NULL_HANDLE);
  retire_framebuffer(t->framebuffer);
  t->attach_view = VK_NULL_HANDLE;
  t->framebuffer = VK_NULL_HANDLE;
}

static VkFramebuffer side_framebuffer_for(VkImageView view) {
  VkImageView views[2] = {view, vk.efb_depth_view};
  VkFramebufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = vk.render_pass, .attachmentCount = 2, .pAttachments = views,
                                  .width = EFB_PIXEL_WIDTH, .height = VISIBLE_HEIGHT * EFB_SCALE, .layers = 1};
  VkFramebuffer framebuffer;
  vk_check(vkCreateFramebuffer(vk.device, &info, 0, &framebuffer), "creating the side framebuffer");
  return framebuffer;
}

static void make_side(void) {
  if (side.image) return;
  static const VkComponentMapping plain = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  vk_create_image(COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  EFB_PIXEL_WIDTH, VISIBLE_HEIGHT * EFB_SCALE, 1, &side.image, &side.memory);
  side.view = vk_create_view(side.image, COLOR_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, plain);
  side.framebuffer = side_framebuffer_for(side.view);
  barrier(vk.frame->draw, side.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, COLOR_ATTACHMENT_STAGE, COLOR_ATTACHMENT_ACCESS);
}

static void side_to_source(VkCommandBuffer commands) {
  barrier(commands, side.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          COLOR_ATTACHMENT_STAGE, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
}

static void side_to_target(VkCommandBuffer commands) {
  barrier(commands, side.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, COLOR_ATTACHMENT_STAGE, COLOR_ATTACHMENT_ACCESS);
}

// Ends drawing into the side picture. Anything still waiting to clear it happens first.
static void leave_side(void) {
  if (!side.on) return;
  end_pass();
  side.on = 0;
}

// A copy of the picture while the layer is being drawn is a copy of the side picture.
static void copy_from_side(GxTexture *to, const GxRectangle *from, int width, int height, GxChannels channels) {
  route_work(0);
  leave_side();
  to->copied_frame = frame_serial;
  to->strip_frame = frame_serial;
  cover.recorded_anything_lasting = 1;
  VkCommandBuffer commands = vk.frame->draw;
  static int swap_off = -1;
  if (swap_off < 0) swap_off = getenv("WII_RECOMP_NO_SWAP") != 0;
  // The whole picture at its own size: the texture can take the side picture's image as
  // it is, and give its own for the next layer. Only an image made for drawing into will do.
  if (!swap_off && width == EFB_PIXEL_WIDTH && height == VISIBLE_HEIGHT * EFB_SCALE && !to->depth) {
    static const VkComponentMapping plain = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkComponentMapping mapping = {swizzle(channels.red), swizzle(channels.green), swizzle(channels.blue), swizzle(channels.alpha)};
    VkImage image = to->image;
    VkDeviceMemory memory = to->memory;
    VkImageView attach_view = to->attach_view;
    VkFramebuffer framebuffer = to->framebuffer;
    int was_written = to->written;
    if (to->view) vk_retire(VK_NULL_HANDLE, to->view, VK_NULL_HANDLE);
    to->image = side.image;
    to->memory = side.memory;
    to->attach_view = side.view;
    to->framebuffer = side.framebuffer;
    to->view = vk_create_view(to->image, COLOR_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, mapping);
    to->width = width;
    to->height = height;
    to->channels = channels;
    to->owed = 0;
    barrier(commands, to->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            COLOR_ATTACHMENT_STAGE, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    to->written = 1;
    if (attach_view) {
      side.image = image;
      side.memory = memory;
      side.view = attach_view;
      side.framebuffer = framebuffer;
    } else {
      // Its image wasn't made for drawing into, so it goes, and the side picture gets a new one.
      if (image) vk_retire(image, VK_NULL_HANDLE, memory);
      vk_create_image(COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      EFB_PIXEL_WIDTH, VISIBLE_HEIGHT * EFB_SCALE, 1, &side.image, &side.memory);
      side.view = vk_create_view(side.image, COLOR_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, plain);
      side.framebuffer = side_framebuffer_for(side.view);
      was_written = 0;
    }
    barrier(commands, side.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, was_written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, COLOR_ATTACHMENT_STAGE, COLOR_ATTACHMENT_ACCESS);
    mark("swap with side", width, height);
    return;
  }
  prepare_copy(to, width, height, COLOR_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, channels);
  side_to_source(commands);
  VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}};
  blit_areas(from, width, height, blit.srcOffsets, blit.dstOffsets);
  if (same_size(&blit)) vkCmdCopyImage(commands, side.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkImageCopy){blit.srcSubresource, blit.srcOffsets[0], blit.dstSubresource, blit.dstOffsets[0], blit_extent(&blit)});
  else vkCmdBlitImage(commands, side.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
  side_to_target(commands);
  finish_copy(to, VK_IMAGE_ASPECT_COLOR_BIT);
  mark("copy color from side", width, height);
}

// Puts what was drawn apart where the game expects it, when the layer ends some other way:
// from the side picture, or once copied, from the layer, which may have taken its image.
static void side_to_picture(GxTexture *copied) {
  leave_side();
  VkCommandBuffer commands = vk.frame->draw;
  VkImage source = copied ? copied->image : side.image;
  if (copied) {
    barrier(commands, source, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  } else {
    side_to_source(commands);
  }
  barrier(commands, vk.efb_color, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          COLOR_ATTACHMENT_STAGE, COLOR_ATTACHMENT_ACCESS, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkImageCopy region = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                        {EFB_PIXEL_WIDTH, VISIBLE_HEIGHT * EFB_SCALE, 1}};
  vkCmdCopyImage(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.efb_color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  barrier(commands, vk.efb_color, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, COLOR_ATTACHMENT_STAGE, COLOR_ATTACHMENT_ACCESS);
  if (copied) {
    barrier(commands, source, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  } else {
    side_to_target(commands);
  }
  mark("side back to picture", EFB_PIXEL_WIDTH, VISIBLE_HEIGHT * EFB_SCALE);
}

static void owe(GxTexture *t, int why) { t->owed = why; t->owed_frame = frame_serial; }

// Runs what was held back. The picture's copy can be left out when it's about to be
// copied again with nothing read in between.
static void settle_layer(int picture_needed) {
  int step = empty_layer.step, real = empty_layer.real;
  empty_layer.step = LAYER_IDLE;
  empty_layer.real = 0;
  if (step == LAYER_IDLE) return;
  // The picture never left. What the game thinks it holds is in the side picture, or a clear.
  if (step >= LAYER_SIDE) {
    leave_side();
    copy_color_now(empty_layer.picture, &empty_layer.from, empty_layer.width, empty_layer.height, empty_layer.picture_channels, 0);
    if (step == LAYER_SIDE_CLEARED) clear_now(&empty_layer.clears[1]);
    else side_to_picture(step == LAYER_SIDE_COPIED ? empty_layer.layer : 0);
    return;
  }
  if (step < LAYER_PICTURE_BACK) {
    copy_color_now(empty_layer.picture, &empty_layer.from, empty_layer.width, empty_layer.height, empty_layer.picture_channels, 0);
    if (step >= LAYER_CLEARED) clear_now(&empty_layer.clears[0]);
    if (step >= LAYER_EMPTY_COPIED) copy_color_now(empty_layer.layer, &empty_layer.from, empty_layer.width, empty_layer.height, empty_layer.layer_channels, 0);
    if (step >= LAYER_CLEARED_AGAIN) clear_now(&empty_layer.clears[1]);
    return;
  }
  // The picture is back and depth is untouched, so the copies can still be made now. The
  // layer had nothing drawn into it, so its copy is the clear before it.
  // A layer drawn apart was copied for real. Its picture copy was only ever drawn back, so
  // like a skipped layer's it's left out, and a read of it is caught.
  if (real) {
    if (picture_needed) owe(empty_layer.picture, 1 | 8);
  } else {
    if (picture_needed) copy_color_now(empty_layer.picture, &empty_layer.from, empty_layer.width, empty_layer.height, empty_layer.picture_channels, 0);
    fill_copy(empty_layer.layer, empty_layer.width, empty_layer.height, empty_layer.layer_channels, empty_layer.clears[0].color);
  }
  if (step >= LAYER_DEPTH_COPIED) copy_depth_requested(empty_layer.depth, &empty_layer.from, empty_layer.depth_width, empty_layer.depth_height);
  if (step == LAYER_DEPTH_CLEARED) clear_now(&empty_layer.clears[2]);
}

static void give_up_layer(void) { settle_layer(1); }

// Work in the strip below the picture neither changes the picture nor, unless it reads
// one of them, needs the copies held back, so it doesn't end a layer.
static int holds(const GxTexture *t) {
  int step = empty_layer.step;
  if (!t || step == LAYER_IDLE) return 0;
  if (t == empty_layer.picture) return 1;
  if (step >= LAYER_SIDE) return 0;
  return (step >= LAYER_EMPTY_COPIED && !empty_layer.real && t == empty_layer.layer) || (step >= LAYER_DEPTH_COPIED && t == empty_layer.depth);
}

static int reads_held(void) {
  extern unsigned gx_used_maps;
  for (int m = 0; m < MAX_TEXTURE_MAPS; m++) if ((gx_used_maps & (1u << m)) && holds(bindings[m].texture)) return 1;
  return 0;
}

static int draws_straight_back(int count, GxTexture *texture) {
  extern TevKey gx_last_tev;
  const TevKey *key = &gx_last_tev;
  if (count != 4 || bindings[0].texture != texture || state_in_use.blend_on || !state_in_use.color_write || !state_in_use.alpha_write) return 0;
  if (state_in_use.depth_write || state_in_use.depth_test_on || !passes_texture_through(key, texture)) return 0;
  return draws_copy_where_it_was(count);
}

// The layer's alpha decides the color, and an empty layer has none: the first stage
// gives the raster's alpha only where the layer's alpha is above a constant, and the
// second passes that on. Depth goes back to the copy, which is what it was before the
// clear, as long as nothing is added to it and the clear was to the far end, so every
// test passes or keeps the same far value.
static int refused_at;
static int refuse(int line) { refused_at = line; return 0; }

static int lays_empty_layer_over(int count) {
  extern TevKey gx_last_tev;
  extern unsigned gx_used_maps;
  const TevKey *key = &gx_last_tev;
  const TevStage *s = &key->stage[0], *t = &key->stage[1];
  enum { TEST_LESS = 1, TEST_LEQUAL = 3, TEST_ALWAYS = 7, BIAS_COMPARE = 3, DEPTH_REPLACE = 2, BP_Z_TEXTURE_BIAS = 0xf4 };
  if (count != 4 || gx_used_maps != 3 || bindings[0].texture != empty_layer.layer || bindings[1].texture != empty_layer.depth) return refuse(__LINE__);
  if (key->stages != 2 || key->alpha_test || key->constant_alpha || key->depth_mode != DEPTH_REPLACE || !key->depth_from_copy) return refuse(__LINE__);
  if (s->alpha_in[0] != 4 || s->alpha_in[1] != 6 || s->alpha_in[2] != 5 || s->alpha_in[3] != 7) return refuse(__LINE__);
  if (s->alpha_bias != BIAS_COMPARE || s->alpha_subtract || s->alpha_target || s->map != 0 || s->indirect) return refuse(__LINE__);
  if (t->alpha_in[0] != 7 || t->alpha_in[1] != 7 || t->alpha_in[2] != 7 || t->alpha_in[3] != 0) return refuse(__LINE__);
  if (t->alpha_bias || t->alpha_subtract || t->alpha_scale || t->alpha_target) return refuse(__LINE__);
  if (gx.bp[BP_Z_TEXTURE_BIAS] & 0xffffff) return refuse(__LINE__);
  if (!state_in_use.blend_on || state_in_use.blend_subtract || state_in_use.alpha_write) return refuse(__LINE__);
  // The stages' alpha, read through the second output or, with none, the first.
  int second = state_in_use.blend_source == GX_FACTOR_SOURCE1_ALPHA && state_in_use.blend_target == GX_FACTOR_INVERSE_SOURCE1_ALPHA;
  int plain = state_in_use.blend_source == GX_FACTOR_SOURCE_ALPHA && state_in_use.blend_target == GX_FACTOR_INVERSE_SOURCE_ALPHA;
  if (!second && !(plain && !gx_blend_needs_second_output(key))) return refuse(__LINE__);
  if (!state_in_use.depth_write || empty_layer.clears[2].depth != 1) return refuse(__LINE__);
  int test = state_in_use.depth_test;
  return !state_in_use.depth_test_on || test == TEST_LESS || test == TEST_LEQUAL || test == TEST_ALWAYS;
}

static int layer_draw(int count) {
  if (empty_layer.step == LAYER_IDLE || (in_strip && !reads_held())) return 0;
  static int side_off = -1;
  if (side_off < 0) side_off = getenv("WII_RECOMP_NO_SIDE") != 0;
  VkRect2D where = drawn_area();
  int fits = !reads_held() && where.offset.y + (int64_t)where.extent.height <= VISIBLE_HEIGHT * EFB_SCALE;
  // Something is drawn into the cleared picture: a layer with something in it. It goes to
  // the side picture, cleared the same way, and the picture stays.
  if (empty_layer.step == LAYER_CLEARED && fits && !side_off) {
    end_pass();
    make_side();
    newest_copy.frame = 0;
    side.on = 1;
    empty_layer.step = LAYER_SIDE;
    clear_now(&empty_layer.clears[0]);
    return 0;
  }
  if (empty_layer.step == LAYER_SIDE && fits) return 0;
  if (empty_layer.step == LAYER_SIDE_CLEARED && draws_straight_back(count, empty_layer.picture)) {
    empty_layer.step = LAYER_PICTURE_BACK;
    empty_layer.real = 1;
    return 1;
  }
  if (empty_layer.step == LAYER_CLEARED_AGAIN && draws_straight_back(count, empty_layer.picture)) {
    empty_layer.step = LAYER_PICTURE_BACK;
    return 1;
  }
  if (empty_layer.step == LAYER_DEPTH_CLEARED && !empty_layer.real && lays_empty_layer_over(count)) {
    owe(empty_layer.picture, 1);
    owe(empty_layer.layer, 2);
    owe(empty_layer.depth, 3);
    empty_layer.step = LAYER_IDLE;
    return 1;
  }
  if (empty_layer.step == LAYER_DEPTH_CLEARED && !empty_layer.real) {
    static int told;
    if (!told++) fprintf(stderr, "layer: laid over in a way not proven to change nothing (gx_vulkan.c:%d), so it is drawn\n", refused_at);
  }
  give_up_layer();
  return 0;
}

static void copy_color(GxTexture *to, const GxRectangle *from, int width, int height, GxChannels channels, int smooth) {
  static int off = -1;
  if (off < 0) off = getenv("WII_RECOMP_KEEP_EMPTY_LAYERS") != 0;
  int step = empty_layer.step, usable = !off && !empty_layer.off && is_whole_picture(from) && width == EFB_WIDTH * EFB_SCALE && !smooth;
  if (step == LAYER_SIDE && usable && to != empty_layer.picture) {
    copy_from_side(to, from, width, height, channels);
    empty_layer.step = LAYER_SIDE_COPIED;
    empty_layer.layer = to;
    empty_layer.layer_channels = channels;
    return;
  }
  // The game sometimes ends a layer without laying it over, then copies the picture again.
  int back = step == LAYER_PICTURE_BACK || step == LAYER_DEPTH_COPIED || step == LAYER_DEPTH_CLEARED;
  if (usable && back && to == empty_layer.picture) {
    settle_layer(0);
    step = LAYER_IDLE;
  }
  if (usable && step == LAYER_IDLE) {
    empty_layer.step = LAYER_PICTURE_COPIED;
    empty_layer.picture = to;
    empty_layer.picture_channels = channels;
    empty_layer.from = *from;
    empty_layer.width = width;
    empty_layer.height = height;
    return;
  }
  if (usable && step == LAYER_CLEARED) {
    empty_layer.step = LAYER_EMPTY_COPIED;
    empty_layer.layer = to;
    empty_layer.layer_channels = channels;
    return;
  }
  if (from->y < VISIBLE_HEIGHT || holds(to)) give_up_layer();
  copy_color_now(to, from, width, height, channels, smooth);
}

static void copy_depth(GxTexture *to, const GxRectangle *from, int width, int height) {
  // Depth is never drawn apart, so its copies go on as asked.
  if (empty_layer.step == LAYER_SIDE && !holds(to)) {
    copy_depth_requested(to, from, width, height);
    return;
  }
  if (empty_layer.step == LAYER_PICTURE_BACK && is_whole_picture(from)) {
    empty_layer.step = LAYER_DEPTH_COPIED;
    empty_layer.depth = to;
    empty_layer.depth_width = width;
    empty_layer.depth_height = height;
    return;
  }
  if (from->y < VISIBLE_HEIGHT || holds(to)) give_up_layer();
  copy_depth_requested(to, from, width, height);
}

static void clear(const GxClear *c) {
  int step = empty_layer.step, whole = is_whole_picture(&c->area);
  int color = whole && c->color_write && c->alpha_write && !c->depth_write && c->color[3] == 0;
  if (color && (step == LAYER_PICTURE_COPIED || step == LAYER_EMPTY_COPIED)) {
    empty_layer.clears[step == LAYER_EMPTY_COPIED] = *c;
    empty_layer.step = step + 1;
    return;
  }
  // While drawing apart, a clear in the picture clears the side picture and shared depth.
  if (step == LAYER_SIDE && c->area.y + c->area.height <= VISIBLE_HEIGHT) {
    clear_now(c);
    return;
  }
  if (step == LAYER_SIDE_COPIED && whole && c->color_write && c->alpha_write && !c->depth_write) {
    empty_layer.clears[1] = *c;
    empty_layer.step = LAYER_SIDE_CLEARED;
    return;
  }
  if (step == LAYER_DEPTH_COPIED && whole && c->depth_write && !c->color_write && !c->alpha_write) {
    empty_layer.clears[2] = *c;
    empty_layer.step = LAYER_DEPTH_CLEARED;
    return;
  }
  if (c->area.y < VISIBLE_HEIGHT) give_up_layer();
  clear_now(c);
}

const GxBackend gx_backend_vulkan = {
  .name = "vulkan",
  .window_prepare = window_prepare,
  .window_ready = window_ready,
  .start = start,
  .stage_compile = vk_stage_compile,
  .shader_link = vk_shader_link,
  .shader_use = shader_use,
  .constants_send = constants_send,
  .render_state = render_state,
  .viewport = viewport,
  .scissor = scissor,
  .texture_create = texture_create,
  .texture_upload = upload_pixels,
  .texture_bind = texture_bind,
  .vertices_reserve = vertices_reserve,
  .draw = draw,
  .clear = clear,
  .copy_color = copy_color,
  .copy_depth = copy_depth,
  .present = present,
};
