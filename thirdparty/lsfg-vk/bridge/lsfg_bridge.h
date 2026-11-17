/**
 * The C interface of liblsfg_framegen.so: Lossless Scaling frame generation
 * (lsfg-vk's framegen) on the caller's own Vulkan device and queue, with the
 * shaders taken from the player's Lossless.dll. The presenter loads the
 * library by name and looks these functions up.
 *
 * Every function runs on the caller's thread and submits to the caller's
 * queue, which the caller must hold for the call. Errors are written to
 * `error` (at most `error_size` bytes with the terminator).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LsfgBridgeInit {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr;
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  uint32_t queue_family;
  VkQueue queue;
  // LSFG 3.1 performance mode (lighter networks) instead of quality.
  int performance;
  int hdr;
  // The optical flow's resolution scale, 0.25 to 1.
  float flow_scale;
  // Frames generated between two rendered ones: the multiplier minus one.
  uint32_t generations;
  const char* dll_path;
} LsfgBridgeInit;

// 1 on success.
typedef int (*PFN_lsfg_bridge_init)(const LsfgBridgeInit* init, char* error, size_t error_size);
// A context on the two input images and `output_count` output images (all of
// `format`, in the GENERAL layout); -1 on failure.
typedef int32_t (*PFN_lsfg_bridge_create)(VkImage input_0, VkImage input_1,
                                          const VkImage* outputs, uint32_t output_count,
                                          uint32_t width, uint32_t height, VkFormat format,
                                          char* error, size_t error_size);
// Submits the generation of every output from the inputs: the input written
// last (alternating, input_0 first) is the newest frame. 1 on success.
typedef int (*PFN_lsfg_bridge_present)(int32_t context, char* error, size_t error_size);
typedef void (*PFN_lsfg_bridge_delete)(int32_t context);
typedef void (*PFN_lsfg_bridge_finalize)(void);

#ifdef __cplusplus
}
#endif
