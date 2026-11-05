// Vulkan's start-up, memory, window images and frames.
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include "timeline.h"
#include "gx_vulkan.h"

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERTEX_RING_BYTES (32u * 1024u * 1024u)
#define CONSTANT_RING_BYTES (16u * 1024u * 1024u)
#define STAGING_RING_BYTES (64u * 1024u * 1024u)
#define VALIDATION_LAYER "VK_LAYER_KHRONOS_validation"
#define PORTABILITY_ENUMERATION "VK_KHR_portability_enumeration"
#define PORTABILITY_SUBSET "VK_KHR_portability_subset"

enum { MAX_EXTENSIONS = 16, WANTED_SWAP_IMAGES = 3 };

Vulkan vk;

void vk_check(VkResult result, const char *what) {
  if (result == VK_SUCCESS) return;
  fprintf(stderr, "stop: vulkan: %s failed with %d\n", what, result);
  exit(10);
}

static int instance_has_extension(const char *name) {
  uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(0, &count, 0);
  VkExtensionProperties *found = calloc(count, sizeof *found);
  vkEnumerateInstanceExtensionProperties(0, &count, found);
  int has = 0;
  for (uint32_t i = 0; i < count; i++) has |= !strcmp(found[i].extensionName, name);
  free(found);
  return has;
}

static int instance_has_layer(const char *name) {
  uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, 0);
  VkLayerProperties *found = calloc(count, sizeof *found);
  vkEnumerateInstanceLayerProperties(&count, found);
  int has = 0;
  for (uint32_t i = 0; i < count; i++) has |= !strcmp(found[i].layerName, name);
  free(found);
  return has;
}

// The window's surface must be made on the window's thread on the Mac. With no window,
// there is no surface, and pictures only reach vk_picture_sink.
void vk_create_instance_and_surface(struct GLFWwindow *window) {
  if (window && !glfwVulkanSupported()) { fprintf(stderr, "stop: no Vulkan here\n"); exit(10); }
  const char *extensions[MAX_EXTENSIONS];
  uint32_t window_extensions = 0, extension_count = 0;
  const char **needed = window ? glfwGetRequiredInstanceExtensions(&window_extensions) : 0;
  for (uint32_t i = 0; i < window_extensions; i++) extensions[extension_count++] = needed[i];
  VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Wii Fit Plus", .apiVersion = VK_API_VERSION_1_1};
  VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application};
  if (instance_has_extension(PORTABILITY_ENUMERATION)) {
    extensions[extension_count++] = PORTABILITY_ENUMERATION;
    info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
  if (window && instance_has_extension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) extensions[extension_count++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
  const char *layers[] = {VALIDATION_LAYER};
  // The layer prints what it finds by itself.
  if (getenv("WII_RECOMP_VULKAN_VALIDATION")) {
    if (instance_has_layer(VALIDATION_LAYER)) {
      info.enabledLayerCount = 1;
      info.ppEnabledLayerNames = layers;
    } else {
      fprintf(stderr, "vulkan: validation asked for, but the layer isn't installed\n");
    }
  }
  info.enabledExtensionCount = extension_count;
  info.ppEnabledExtensionNames = extensions;
  vk_check(vkCreateInstance(&info, 0, &vk.instance), "creating an instance");
  if (window) vk_check(glfwCreateWindowSurface(vk.instance, window, 0, &vk.surface), "creating the window's surface");
}

static int device_has_extension(VkPhysicalDevice device, const char *name) {
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(device, 0, &count, 0);
  VkExtensionProperties *found = calloc(count, sizeof *found);
  vkEnumerateDeviceExtensionProperties(device, 0, &count, found);
  int has = 0;
  for (uint32_t i = 0; i < count; i++) has |= !strcmp(found[i].extensionName, name);
  free(found);
  return has;
}

// A queue that both draws and shows pictures, or -1.
static int find_queue_family(VkPhysicalDevice device) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, 0);
  VkQueueFamilyProperties *families = calloc(count, sizeof *families);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);
  int found = -1;
  for (uint32_t i = 0; i < count && found < 0; i++) {
    VkBool32 presents = vk.surface ? VK_FALSE : VK_TRUE;
    if (vk.surface) vkGetPhysicalDeviceSurfaceSupportKHR(device, i, vk.surface, &presents);
    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presents) found = (int)i;
  }
  free(families);
  return found;
}

static void choose_physical_device(void) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(vk.instance, &count, 0);
  VkPhysicalDevice *devices = calloc(count, sizeof *devices);
  vkEnumeratePhysicalDevices(vk.instance, &count, devices);
  VkPhysicalDeviceProperties chosen_properties;
  for (uint32_t i = 0; i < count; i++) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(devices[i], &properties);
    if (find_queue_family(devices[i]) < 0 || (vk.surface && !device_has_extension(devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME))) continue;
    // Prefer a real graphics card to one built into the processor, and either to software.
    if (!vk.physical || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
        chosen_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
      vk.physical = devices[i];
      chosen_properties = properties;
    }
  }
  free(devices);
  if (!vk.physical) { fprintf(stderr, "stop: no suitable graphics card for Vulkan\n"); exit(10); }
  vk.limits = chosen_properties.limits;
  fprintf(stderr, "vulkan: %s\n", chosen_properties.deviceName);
}

void vk_create_device(void) {
  choose_physical_device();
  vk.queue_family = (uint32_t)find_queue_family(vk.physical);
  // The window's queue: a second one in the same family, or else another family that
  // draws and shows. On the Mac, main.c asks for the extra families, each its own Metal queue.
  uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &family_count, 0);
  VkQueueFamilyProperties *families = calloc(family_count, sizeof *families);
  vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &family_count, families);
  int own_show_queue = vk.surface && vk_window_wants_own_queue();
  uint32_t show_family = vk.queue_family;
  if (own_show_queue && families[vk.queue_family].queueCount < 2) {
    own_show_queue = 0;
    for (uint32_t i = 0; i < family_count && !own_show_queue; i++) {
      VkBool32 presents = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical, i, vk.surface, &presents);
      if (i != vk.queue_family && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presents) {
        show_family = i;
        own_show_queue = 1;
      }
    }
  }
  free(families);
  float priorities[2] = {1, 1};
  int same_family = own_show_queue && show_family == vk.queue_family;
  VkDeviceQueueCreateInfo queues[2] = {
    {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk.queue_family, .queueCount = same_family ? 2 : 1, .pQueuePriorities = priorities},
    {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = show_family, .queueCount = 1, .pQueuePriorities = priorities},
  };
  const char *extensions[3];
  uint32_t extension_count = 0;
  if (vk.surface) extensions[extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  int pushes_textures = device_has_extension(vk.physical, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
  if (pushes_textures) extensions[extension_count++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
  VkPhysicalDeviceFeatures available, wanted = {0};
  vkGetPhysicalDeviceFeatures(vk.physical, &available);
  // Blending reads a second shader output. Without it, a constant output alpha also
  // changes the blend, which only a few effects use.
  wanted.dualSrcBlend = available.dualSrcBlend;
  vk.dual_source = available.dualSrcBlend == VK_TRUE;
  if (!vk.dual_source) fprintf(stderr, "vulkan: no dual-source blending, so constant alpha will blend slightly wrong\n");
  wanted.samplerAnisotropy = available.samplerAnisotropy;
  vk.max_anisotropy = available.samplerAnisotropy ? vk.limits.maxSamplerAnisotropy : 0;
  if (!vk.max_anisotropy) fprintf(stderr, "vulkan: no anisotropic filtering, so textures seen at a slope will blur more\n");
  VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = own_show_queue && !same_family ? 2 : 1, .pQueueCreateInfos = queues,
                             .enabledExtensionCount = extension_count, .ppEnabledExtensionNames = extensions, .pEnabledFeatures = &wanted};
  // Vulkan on Metal leaves some things out, and using what's left must be declared.
  VkPhysicalDevicePortabilitySubsetFeaturesKHR portability = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR};
  if (device_has_extension(vk.physical, PORTABILITY_SUBSET)) {
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &portability};
    vkGetPhysicalDeviceFeatures2(vk.physical, &features);
    if (!portability.imageViewFormatSwizzle) fprintf(stderr, "vulkan: this device can't swizzle, so one-channel copies will look wrong\n");
    extensions[info.enabledExtensionCount++] = PORTABILITY_SUBSET;
    info.pNext = &portability;
  }
  vk_check(vkCreateDevice(vk.physical, &info, 0, &vk.device), "creating the device");
  vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);
  if (own_show_queue) vkGetDeviceQueue(vk.device, show_family, same_family ? 1 : 0, &vk.show_queue);
  vk.show_family = show_family;
  if (own_show_queue) fprintf(stderr, "vulkan: pictures shown on a queue of their own\n");
  if (pushes_textures) vk.push_descriptors = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(vk.device, "vkCmdPushDescriptorSetKHR");
  VkFormatProperties depth;
  vkGetPhysicalDeviceFormatProperties(vk.physical, DEPTH_FORMAT, &depth);
  vk.depth_filters_linear = (depth.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  VkFormatFeatureFlags blits = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
  if ((depth.optimalTilingFeatures & blits) != blits) fprintf(stderr, "vulkan: this device can't blit depth, so depth copies will fail\n");
}

static uint32_t find_memory(uint32_t allowed, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags needed) {
  VkPhysicalDeviceMemoryProperties memory;
  vkGetPhysicalDeviceMemoryProperties(vk.physical, &memory);
  VkMemoryPropertyFlags tries[2] = {wanted | needed, needed};
  for (int attempt = 0; attempt < 2; attempt++) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
      if ((allowed & (1u << i)) && (memory.memoryTypes[i].propertyFlags & tries[attempt]) == tries[attempt]) return i;
    }
  }
  fprintf(stderr, "stop: vulkan: no suitable memory\n");
  exit(10);
}

static VkDeviceMemory allocate(VkMemoryRequirements needs, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags needed) {
  VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = needs.size,
                               .memoryTypeIndex = find_memory(needs.memoryTypeBits, wanted, needed)};
  VkDeviceMemory memory;
  vk_check(vkAllocateMemory(vk.device, &info, 0, &memory), "allocating memory");
  return memory;
}

void vk_create_image(VkFormat format, VkImageUsageFlags usage, uint32_t width, uint32_t height, uint32_t levels,
                     VkImage *image, VkDeviceMemory *memory) {
  vk_create_shared_image(format, usage, width, height, levels, 0, image, memory);
}

void vk_create_shared_image(VkFormat format, VkImageUsageFlags usage, uint32_t width, uint32_t height, uint32_t levels, int shared,
                            VkImage *image, VkDeviceMemory *memory) {
  uint32_t both[2] = {vk.queue_family, vk.show_family};
  int concurrent = shared && vk.show_family != vk.queue_family;
  VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
                            .extent = {width, height, 1}, .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .sharingMode = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
                            .queueFamilyIndexCount = concurrent ? 2 : 0, .pQueueFamilyIndices = both};
  vk_check(vkCreateImage(vk.device, &info, 0, image), "creating an image");
  VkMemoryRequirements needs;
  vkGetImageMemoryRequirements(vk.device, *image, &needs);
  *memory = allocate(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
  vk_check(vkBindImageMemory(vk.device, *image, *memory, 0), "binding image memory");
}

VkImageView vk_create_view(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t levels, VkComponentMapping channels) {
  VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format, .components = channels, .subresourceRange = {aspect, 0, levels, 0, 1}};
  VkImageView view;
  vk_check(vkCreateImageView(vk.device, &info, 0, &view), "creating an image view");
  return view;
}

void vk_retire(VkImage image, VkImageView view, VkDeviceMemory memory) {
  Frame *frame = vk.frame;
  if (frame->retired_count == frame->retired_capacity) {
    frame->retired_capacity = frame->retired_capacity ? frame->retired_capacity * 2 : 64;
    frame->retired = realloc(frame->retired, (size_t)frame->retired_capacity * sizeof *frame->retired);
  }
  frame->retired[frame->retired_count++] = (Retired){image, view, memory};
}

Ring vk_create_ring(VkDeviceSize size, VkBufferUsageFlags usage) {
  Ring ring = {.size = size};
  VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
  vk_check(vkCreateBuffer(vk.device, &info, 0, &ring.buffer), "creating a buffer");
  VkMemoryRequirements needs;
  vkGetBufferMemoryRequirements(vk.device, ring.buffer, &needs);
  ring.memory = allocate(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vk_check(vkBindBufferMemory(vk.device, ring.buffer, ring.memory, 0), "binding buffer memory");
  vk_check(vkMapMemory(vk.device, ring.memory, 0, VK_WHOLE_SIZE, 0, (void **)&ring.mapped), "mapping a buffer");
  return ring;
}

VkDeviceSize vk_ring_take(Ring *ring, VkDeviceSize bytes, VkDeviceSize alignment) {
  VkDeviceSize at = (ring->used + alignment - 1) / alignment * alignment;
  if (at + bytes > ring->size) return (VkDeviceSize)-1;
  ring->used = at + bytes;
  return at;
}

void vk_create_frames(void) {
  for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
    Frame *frame = &vk.frames[i];
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk.queue_family};
    vk_check(vkCreateCommandPool(vk.device, &pool, 0, &frame->pool), "creating a command pool");
    VkCommandBufferAllocateInfo buffers = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = frame->pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    VkCommandBuffer both[2];
    vk_check(vkAllocateCommandBuffers(vk.device, &buffers, both), "creating command buffers");
    frame->draw = both[0];
    frame->upload = both[1];
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vk_check(vkCreateFence(vk.device, &fence, 0, &frame->fence), "creating a fence");
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vk_check(vkCreateSemaphore(vk.device, &semaphore, 0, &frame->image_ready), "creating a semaphore");
    frame->vertices = vk_create_ring(VERTEX_RING_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    frame->constants = vk_create_ring(CONSTANT_RING_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    frame->staging = vk_create_ring(STAGING_RING_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkDescriptorSetAllocateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = vk.descriptor_pool,
                                       .descriptorSetCount = 1, .pSetLayouts = &vk.set_layouts[SET_CONSTANTS]};
    vk_check(vkAllocateDescriptorSets(vk.device, &set, &frame->constant_set), "allocating a descriptor set");
    VkDescriptorBufferInfo blocks[GX_CONSTANT_BLOCKS];
    VkWriteDescriptorSet writes[GX_CONSTANT_BLOCKS];
    for (uint32_t block = 0; block < GX_CONSTANT_BLOCKS; block++) {
      blocks[block] = (VkDescriptorBufferInfo){frame->constants.buffer, 0, GX_CONSTANT_BYTES};
      writes[block] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame->constant_set, .dstBinding = block,
                                             .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &blocks[block]};
    }
    vkUpdateDescriptorSets(vk.device, GX_CONSTANT_BLOCKS, writes, 0, 0);
  }
}

static VkPresentModeKHR choose_present_mode(void) {
  // In order of preference. The first matches OpenGL with its swap interval at 0.
  static const VkPresentModeKHR wanted[] = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
  uint32_t count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, vk.surface, &count, 0);
  VkPresentModeKHR *modes = calloc(count, sizeof *modes);
  vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, vk.surface, &count, modes);
  VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
  for (int i = (int)(sizeof wanted / sizeof wanted[0]) - 1; i >= 0; i--) {
    for (uint32_t m = 0; m < count; m++) {
      if (modes[m] == wanted[i]) chosen = wanted[i];
    }
  }
  free(modes);
  return chosen;
}

// Plain 8-bit output, as the picture's values are already what the screen should show.
// Where the system would convert colors for the display, as the Mac does for anything
// labelled sRGB, passing them through unchanged matches what OpenGL shows.
static VkSurfaceFormatKHR choose_surface_format(void) {
  uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &count, 0);
  VkSurfaceFormatKHR *formats = calloc(count, sizeof *formats);
  vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &count, formats);
  VkSurfaceFormatKHR chosen = formats[0];
  int score = 0;
  for (uint32_t i = 0; i < count; i++) {
    int plain = formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM;
    int found = plain ? (formats[i].colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT ? 2 : 1) : 0;
    if (found > score) {
      chosen = formats[i];
      score = found;
    }
  }
  free(formats);
  return chosen;
}

int vk_swapchain_matches_window(void) {
  int width, height;
  window_size(&width, &height);
  return !width || !height || (vk.swap_window_width == width && vk.swap_window_height == height);
}

void vk_create_swapchain(void) {
  vkDeviceWaitIdle(vk.device);
  VkSurfaceCapabilitiesKHR capabilities;
  vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &capabilities), "reading the surface");
  int width, height;
  window_size(&width, &height);
  VkExtent2D extent = capabilities.currentExtent;
  if (extent.width == UINT32_MAX) extent = (VkExtent2D){(uint32_t)width, (uint32_t)height};
  vk.swap_window_width = width;
  vk.swap_window_height = height;
  uint32_t images = capabilities.minImageCount > WANTED_SWAP_IMAGES ? capabilities.minImageCount : WANTED_SWAP_IMAGES;
  if (capabilities.maxImageCount && images > capabilities.maxImageCount) images = capabilities.maxImageCount;
  VkSurfaceFormatKHR format = choose_surface_format();
  VkSwapchainKHR old = vk.swapchain;
  VkSwapchainCreateInfoKHR info = {
    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vk.surface, .minImageCount = images,
    .imageFormat = format.format, .imageColorSpace = format.colorSpace, .imageExtent = extent, .imageArrayLayers = 1,
    .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = capabilities.currentTransform,
    .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = choose_present_mode(), .clipped = VK_TRUE, .oldSwapchain = old,
  };
  vk_check(vkCreateSwapchainKHR(vk.device, &info, 0, &vk.swapchain), "creating the swapchain");
  for (uint32_t i = 0; i < vk.swap_count; i++) vkDestroySemaphore(vk.device, vk.swap_done[i], 0);
  if (old) vkDestroySwapchainKHR(vk.device, old, 0);
  vk.swap_extent = extent;
  vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.swap_count, 0);
  vk.swap_images = realloc(vk.swap_images, vk.swap_count * sizeof *vk.swap_images);
  vk.swap_done = realloc(vk.swap_done, vk.swap_count * sizeof *vk.swap_done);
  vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.swap_count, vk.swap_images);
  VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (uint32_t i = 0; i < vk.swap_count; i++) vk_check(vkCreateSemaphore(vk.device, &semaphore, 0, &vk.swap_done[i]), "creating a semaphore");
}

VkCommandBuffer vk_upload_commands(void) {
  Frame *frame = vk.frame;
  if (!frame->upload_started) {
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk_check(vkBeginCommandBuffer(frame->upload, &begin), "starting commands");
    frame->upload_started = 1;
  }
  return frame->upload;
}

// Where the driver can't push textures, each change takes a set from this frame's pools.
static void add_texture_pool(Frame *frame) {
  if (frame->texture_pool_count == MAX_TEXTURE_POOLS) { fprintf(stderr, "stop: vulkan: too many texture changes in one frame\n"); exit(10); }
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TEXTURE_SETS_PER_POOL * MAX_TEXTURE_MAPS};
  VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = TEXTURE_SETS_PER_POOL, .poolSizeCount = 1, .pPoolSizes = &size};
  vk_check(vkCreateDescriptorPool(vk.device, &pool, 0, &frame->texture_pools[frame->texture_pool_count++]), "creating a descriptor pool");
}

VkDescriptorSet vk_texture_set(Frame *frame) {
  if (frame->texture_sets_used % TEXTURE_SETS_PER_POOL == 0 && frame->texture_sets_used / TEXTURE_SETS_PER_POOL == frame->texture_pool_count) {
    add_texture_pool(frame);
  }
  VkDescriptorSetAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = frame->texture_pools[frame->texture_sets_used / TEXTURE_SETS_PER_POOL],
                                      .descriptorSetCount = 1, .pSetLayouts = &vk.set_layouts[SET_TEXTURES]};
  VkDescriptorSet set;
  vk_check(vkAllocateDescriptorSets(vk.device, &info, &set), "allocating a descriptor set");
  frame->texture_sets_used++;
  return set;
}

// Waits for this frame's last turn on the card, then readies it for another.
void vk_frame_begin(Frame *frame) {
  if (frame->submitted) {
    TIMELINE_BEGIN("waiting for the chip");
    vk_check(vkWaitForFences(vk.device, 1, &frame->fence, VK_TRUE, UINT64_MAX), "waiting for the card");
    TIMELINE_END("waiting for the chip");
    vk_check(vkResetFences(vk.device, 1, &frame->fence), "resetting a fence");
    frame->submitted = 0;
  }
  for (int i = 0; i < frame->retired_count; i++) {
    Retired *old = &frame->retired[i];
    vkDestroyImageView(vk.device, old->view, 0);
    vkDestroyImage(vk.device, old->image, 0);
    vkFreeMemory(vk.device, old->memory, 0);
  }
  frame->retired_count = 0;
  for (int i = 0; i < frame->texture_pool_count; i++) vk_check(vkResetDescriptorPool(vk.device, frame->texture_pools[i], 0), "resetting a descriptor pool");
  frame->texture_sets_used = 0;
  frame->vertices.used = frame->constants.used = frame->staging.used = 0;
  frame->upload_started = 0;
  vk_check(vkResetCommandPool(vk.device, frame->pool, 0), "resetting commands");
  VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  vk_check(vkBeginCommandBuffer(frame->draw, &begin), "starting commands");
  vk.frame = frame;
}

void vk_frame_submit(Frame *frame, VkSemaphore wait, VkSemaphore signal) {
  VkCommandBuffer buffers[2];
  uint32_t count = 0;
  if (frame->upload_started) {
    vk_check(vkEndCommandBuffer(frame->upload), "finishing commands");
    buffers[count++] = frame->upload;
  }
  vk_check(vkEndCommandBuffer(frame->draw), "finishing commands");
  buffers[count++] = frame->draw;
  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = wait ? 1 : 0, .pWaitSemaphores = &wait,
                       .pWaitDstStageMask = &wait_stage, .commandBufferCount = count, .pCommandBuffers = buffers,
                       .signalSemaphoreCount = signal ? 1 : 0, .pSignalSemaphores = &signal};
  vk_check(vkQueueSubmit(vk.queue, 1, &info, frame->fence), "submitting commands");
  frame->submitted = 1;
}
