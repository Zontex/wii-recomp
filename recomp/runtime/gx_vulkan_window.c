// Showing pictures in the window on a queue of their own, where the system can make
// that wait.
//
// On the Mac, Vulkan runs on Metal, which only hands out a window picture when the
// window server has given one back, and waits up to a second for it otherwise. It
// does that wait while encoding the first command that writes to the picture, on the
// queue's own thread. If that command is in a frame's draw commands, the frame's
// fence waits too, and the game stops with it. Here each finished picture is copied
// aside in the frame, and a second queue copies it into the window and shows it. A
// slow window then holds back only that queue, and while it's busy, finished
// pictures simply aren't shown. The game never waits for the window.
//
// Elsewhere, and wherever the device has no second queue that can show pictures,
// they are shown in the frame as before. WII_RECOMP_SHOW_IN_FRAME=1 does that on the Mac too, for comparing.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gx_vulkan.h"

enum { SHOWN_SLOTS = 2 };

typedef struct {
  VkImage image;
  VkDeviceMemory memory;
  VkCommandBuffer commands;
  VkFence done;           // Signalled when the slot's picture is no longer read.
  VkSemaphore copied;     // The frame's copy into the image is finished.
  VkSemaphore acquired;   // The window's picture is ready to be written.
} Slot;

static VkCommandPool pool;
static Slot slots[SHOWN_SLOTS];
static int next_slot;
static uint32_t picture_width, picture_height;
static int skipped;  // Pictures not shown since the window was last free.

// For testing: WII_RECOMP_TEST_WINDOW_STALL=<n> makes the n-th write to the window wait
// a second on the card, as when the window server keeps every picture, to check that
// the game keeps running through it.
static VkEvent stall_event;

static void *release_stall(void *unused) {
  (void)unused;
  usleep(1000 * 1000);
  vkSetEvent(vk.device, stall_event);
  return 0;
}

void vk_window_test_stall(VkCommandBuffer commands) {
  static long at = -1, count;
  if (at < 0) { const char *text = getenv("WII_RECOMP_TEST_WINDOW_STALL"); at = text ? atol(text) : 0; }
  if (!at || ++count != at) return;
  VkEventCreateInfo info = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
  vk_check(vkCreateEvent(vk.device, &info, 0, &stall_event), "creating an event");
  vkCmdWaitEvents(commands, 1, &stall_event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 0);
  pthread_t thread;
  pthread_create(&thread, 0, release_stall, 0);
  pthread_detach(thread);
  fprintf(stderr, "window: test stall on picture %ld\n", count);
}

int vk_window_wants_own_queue(void) {
#ifdef __APPLE__
  const char *in_frame = getenv("WII_RECOMP_SHOW_IN_FRAME");
  return !(in_frame && *in_frame && strcmp(in_frame, "0"));
#else
  return 0;
#endif
}

void vk_window_start(uint32_t width, uint32_t height) {
  if (!vk.show_queue) return;
  picture_width = width;
  picture_height = height;
  VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                       .queueFamilyIndex = vk.show_family};
  vk_check(vkCreateCommandPool(vk.device, &pool_info, 0, &pool), "creating a command pool");
  for (int i = 0; i < SHOWN_SLOTS; i++) {
    Slot *slot = &slots[i];
    vk_create_shared_image(COLOR_FORMAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, picture_width, picture_height, 1, 1,
                    &slot->image, &slot->memory);
    VkCommandBufferAllocateInfo buffer = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    vk_check(vkAllocateCommandBuffers(vk.device, &buffer, &slot->commands), "creating command buffers");
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    vk_check(vkCreateFence(vk.device, &fence, 0, &slot->done), "creating a fence");
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vk_check(vkCreateSemaphore(vk.device, &semaphore, 0, &slot->copied), "creating a semaphore");
    vk_check(vkCreateSemaphore(vk.device, &semaphore, 0, &slot->acquired), "creating a semaphore");
  }
}

static void image_barrier(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags before, VkAccessFlags after,
                          VkPipelineStageFlags wait, VkPipelineStageFlags block) {
  VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = before, .dstAccessMask = after,
                                  .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  vkCmdPipelineBarrier(commands, wait, block, 0, 0, 0, 0, 0, 1, &barrier);
}

// One line when the window is free again, not one per picture: a window that's being
// dragged or is hidden can skip many.
static void note_shown(void) {
  if (skipped) fprintf(stderr, "window: %d pictures not shown while it was busy\n", skipped);
  skipped = 0;
}

int vk_window_take_slot(void) {
  Slot *slot = &slots[next_slot];
  if (vkGetFenceStatus(vk.device, slot->done) != VK_SUCCESS) {
    skipped++;
    return -1;
  }
  int taken = next_slot;
  next_slot = (next_slot + 1) % SHOWN_SLOTS;
  return taken;
}

// In the frame's draw commands. The picture must be ready to be read from: the caller
// moves it to the transfer source layout and back.
void vk_window_copy_picture(VkCommandBuffer commands, int slot_index, VkImage picture) {
  Slot *slot = &slots[slot_index];
  image_barrier(commands, slot->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {picture_width, picture_height, 1}};
  vkCmdCopyImage(commands, picture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  image_barrier(commands, slot->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

VkSemaphore vk_window_copied(int slot_index) { return slots[slot_index].copied; }

// After the frame that copied the picture has been submitted, signalling copied. from
// is the area of the picture to show and to where it goes in the window.
// Returns 1 if the window needs a new swapchain.
int vk_window_show(int slot_index, const VkOffset3D from[2], const VkOffset3D to[2]) {
  Slot *slot = &slots[slot_index];
  vk_check(vkResetFences(vk.device, 1, &slot->done), "resetting a fence");
  uint32_t image_index = 0;
  VkResult acquired = vkAcquireNextImageKHR(vk.device, vk.swapchain, 0, slot->acquired, VK_NULL_HANDLE, &image_index);
  int outdated = acquired == VK_SUBOPTIMAL_KHR || acquired == VK_ERROR_OUT_OF_DATE_KHR;
  VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
  if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
    // No window picture free. The copied signal still has to be waited on before it's
    // used again, and the slot's fence marks when that's done.
    skipped++;
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &slot->copied, .pWaitDstStageMask = stages};
    vk_check(vkQueueSubmit(vk.show_queue, 1, &info, slot->done), "submitting commands");
    return outdated;
  }
  VkImage window = vk.swap_images[image_index];
  VkCommandBuffer commands = slot->commands;
  vk_check(vkResetCommandBuffer(commands, 0), "resetting commands");
  VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  vk_check(vkBeginCommandBuffer(commands, &begin), "starting commands");
  image_barrier(commands, window, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .srcOffsets = {from[0], from[1]}, .dstOffsets = {to[0], to[1]}};
  vk_window_test_stall(commands);
  vkCmdBlitImage(commands, slot->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, window, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
  image_barrier(commands, window, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  vk_check(vkEndCommandBuffer(commands), "finishing commands");
  VkSemaphore waits[2] = {slot->copied, slot->acquired};
  VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 2, .pWaitSemaphores = waits, .pWaitDstStageMask = stages,
                       .commandBufferCount = 1, .pCommandBuffers = &commands, .signalSemaphoreCount = 1, .pSignalSemaphores = &vk.swap_done[image_index]};
  vk_check(vkQueueSubmit(vk.show_queue, 1, &info, slot->done), "submitting commands");
  VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &vk.swap_done[image_index],
                              .swapchainCount = 1, .pSwapchains = &vk.swapchain, .pImageIndices = &image_index};
  VkResult presented = vkQueuePresentKHR(vk.show_queue, &present);
  note_shown();
  return outdated || presented == VK_SUBOPTIMAL_KHR || presented == VK_ERROR_OUT_OF_DATE_KHR;
}
