// What the parts of the Vulkan backend share. gx_vulkan_device.c owns the device,
// the frames and their memory, gx_vulkan_pipeline.c owns shaders, pipelines and
// samplers, and gx_vulkan.c turns the backend's calls into commands.
//
// Frames: FRAMES_IN_FLIGHT sets of commands and memory take turns, so the game runs
// ahead of the graphics card without touching anything the card still reads. Waiting
// happens only when a turn comes round and the card hasn't finished that set yet. Three
// give a card that sometimes takes longer than a frame room to catch up: the wait comes
// in the copy to the screen, before the game hears the frame is done, so with two a
// game that waits for that before the next retrace lost a whole retrace each time.
#pragma once
#define VK_ENABLE_BETA_EXTENSIONS  // VK_KHR_portability_subset, for running on Metal.
#include <vulkan/vulkan.h>

#include "gx_backend.h"

enum {
  FRAMES_IN_FLIGHT = 3,
  MAX_TEXTURE_POOLS = 16,
  TEXTURE_SETS_PER_POOL = 1024,
  COLOR_FORMAT = VK_FORMAT_R8G8B8A8_UNORM,
  DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT,  // Available everywhere, unlike 24 bits. The picture only ever compares it.
  SET_CONSTANTS = 0, SET_TEXTURES = 1, SET_BOUNDING_BOX = 2, SET_COUNT = 3,
};

// One share of a buffer the processor writes and the card reads, used from the start each frame.
typedef struct {
  VkBuffer buffer;
  VkDeviceMemory memory;
  uint8_t *mapped;
  VkDeviceSize size, used;
} Ring;

// An image whose last use may still be on its way through the card.
typedef struct {
  VkImage image;
  VkImageView view;
  VkDeviceMemory memory;
} Retired;

typedef struct {
  VkCommandPool pool;
  VkCommandBuffer draw;    // Draws and picture copies, in the game's order.
  VkCommandBuffer upload;  // Runs first, so new textures needn't break up draws.
  int upload_started, submitted;
  VkFence fence;
  VkSemaphore image_ready;
  VkDescriptorSet constant_set;
  VkDescriptorPool texture_pools[MAX_TEXTURE_POOLS];
  int texture_pool_count, texture_sets_used;
  Ring vertices, constants, staging, indices;  // Indices: strips joined into one draw (gx_vulkan.c).
  Retired *retired;
  int retired_count, retired_capacity;
} Frame;

typedef struct {
  VkInstance instance;
  VkSurfaceKHR surface;
  VkPhysicalDevice physical;
  VkDevice device;
  uint32_t queue_family;
  VkQueue queue;
  VkQueue show_queue;  // Shows pictures in the window, where it has its own. See gx_vulkan_window.c.
  uint32_t show_family;
  VkPhysicalDeviceLimits limits;
  PFN_vkCmdPushDescriptorSetKHR push_descriptors;  // Null where the driver lacks them.
  int depth_filters_linear, dual_source;
  int bounding_box;  // Shaders can write to a buffer, which the bounding box needs.
  float max_anisotropy;  // 0 when the device can't filter along a slope.
  int counting;  // WII_RECOMP_GPU_COUNTERS: the card counts its own work. See gx_vulkan.c.

  VkSwapchainKHR swapchain;
  VkExtent2D swap_extent;
  int swap_window_width, swap_window_height;  // GLFW's count, which on a Retina screen isn't the extent's.
  VkImage *swap_images;
  VkSemaphore *swap_done;  // One per image: an image's present can outlast our frame.
  uint32_t swap_count;

  VkImage efb_color, efb_depth;
  VkImageView efb_color_view, efb_depth_view;
  VkFramebuffer efb_framebuffer;
  VkRenderPass render_pass;

  VkDescriptorSetLayout set_layouts[SET_COUNT];
  VkPipelineLayout pipeline_layout;
  VkDescriptorPool descriptor_pool;

  Frame frames[FRAMES_IN_FLIGHT];
  Frame *frame;
} Vulkan;

extern Vulkan vk;

// With no window, window_ready(0) makes an instance with no surface, and nothing is shown.
// A tool that sets this gets each picture as it is presented: top row first, four bytes a pixel.
typedef void (*VkPictureSink)(const uint8_t *rgba, int width, int height);
extern VkPictureSink vk_picture_sink;

void vk_check(VkResult result, const char *what);

// gx_vulkan_device.c
VkDescriptorSet vk_texture_set(Frame *frame);
void vk_create_instance_and_surface(struct GLFWwindow *window);
void vk_create_device(void);
void vk_create_frames(void);
Ring vk_create_ring(VkDeviceSize size, VkBufferUsageFlags usage);
Ring vk_create_read_ring(VkDeviceSize size);  // For the card to write and the processor to read.
void vk_create_swapchain(void);
int vk_swapchain_matches_window(void);
void vk_create_image(VkFormat format, VkImageUsageFlags usage, uint32_t width, uint32_t height, uint32_t levels,
                     VkImage *image, VkDeviceMemory *memory);
// The same, readable from the window's queue as well when that's in another family.
void vk_create_shared_image(VkFormat format, VkImageUsageFlags usage, uint32_t width, uint32_t height, uint32_t levels, int shared,
                            VkImage *image, VkDeviceMemory *memory);
VkImageView vk_create_view(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t levels, VkComponentMapping channels);
void vk_retire(VkImage image, VkImageView view, VkDeviceMemory memory);
// Space in a ring, or -1 if this frame's share has run out.
VkDeviceSize vk_ring_take(Ring *ring, VkDeviceSize bytes, VkDeviceSize alignment);
void vk_frame_begin(Frame *frame);
void vk_frame_submit(Frame *frame, VkSemaphore wait, VkSemaphore signal);
VkCommandBuffer vk_upload_commands(void);

// gx_vulkan_window.c
int vk_window_wants_own_queue(void);
void vk_window_test_stall(VkCommandBuffer commands);
void vk_window_start(uint32_t picture_width, uint32_t picture_height);  // Does nothing without a show queue.
int vk_window_take_slot(void);  // -1 while the window is still busy with earlier pictures.
void vk_window_copy_picture(VkCommandBuffer commands, int slot, VkImage picture);
VkSemaphore vk_window_copied(int slot);
int vk_window_show(int slot, const VkOffset3D from[2], const VkOffset3D to[2]);

// gx_vulkan_pipeline.c
void vk_pipelines_start(const GxAttribute *attributes, int attribute_count, size_t vertex_bytes);
GxStage vk_stage_compile(int kind, const char *text);
GxShader vk_shader_link(GxStage vertex, GxStage fragment);
// A program's vertices hold only what its vertex shader reads of a Vertex, packed in the
// order of their locations: each run copies `words` words from word `from` of a Vertex.
enum { MOST_VERTEX_RUNS = 16 };
typedef struct { uint8_t from, words; } VkVertexRun;
typedef struct { uint32_t stride, run_count; VkVertexRun runs[MOST_VERTEX_RUNS]; } VkVertexPacking;
const VkVertexPacking *vk_shader_packing(GxShader shader);
VkPipeline vk_pipeline_find(GxShader shader, const GxRenderState *state);
void vk_stages_prepare(int count, const int *kinds, const char *const *texts);  // On every core.
void vk_pipeline_prepare(GxShader shader, const GxRenderState *state);
void vk_pipelines_prepare(int count, const GxShader *shaders, const GxRenderState *states);
VkSampler vk_sampler_find(const GxSampling *sampling, int depth);
