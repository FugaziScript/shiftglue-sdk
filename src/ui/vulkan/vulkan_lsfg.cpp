/**
 * Lossless Scaling frame generation on the Vulkan presenter (rex/ui/vulkan/lsfg.h), through
 * liblsfg_framegen.so's C interface (thirdparty/lsfg-vk/bridge/lsfg_bridge.h).
 */

#include <rex/ui/vulkan/lsfg.h>

#include <algorithm>
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

#include "lsfg_bridge.h"

#if REX_PLATFORM_LINUX || defined(__ANDROID__)
#include <dlfcn.h>
#endif

REXCVAR_DEFINE_INT32(lsfg_multiplier, 1, "UI/Presenter",
                     "Lossless Scaling frame generation: frames presented per rendered one, 2 to "
                     "4 (1: off); needs the player's Lossless.dll (lsfg_dll)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(lsfg_performance, false, "UI/Presenter",
                    "Lossless Scaling frame generation: the lighter performance networks")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(lsfg_flow_scale, 100, "UI/Presenter",
                     "Lossless Scaling frame generation: the optical flow's resolution, 25 to "
                     "100 % of the frame's; lower is faster")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(lsfg_dll, "", "UI/Presenter",
                      "Lossless Scaling frame generation: the path of Lossless.dll, whose "
                      "shaders it runs (empty: REX_LSFG_DLL from the environment)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex {
namespace ui {
namespace vulkan {

namespace {

void Barrier(const VulkanDevice::Functions& dfn, VkCommandBuffer command_buffer, VkImage image,
             VkPipelineStageFlags src_stage, VkAccessFlags src_access,
             VkPipelineStageFlags dst_stage, VkAccessFlags dst_access, VkImageLayout old_layout,
             VkImageLayout new_layout) {
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = util::InitializeSubresourceRange();
  dfn.vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
}

void Blit(const VulkanDevice::Functions& dfn, VkCommandBuffer command_buffer, VkImage source,
          VkImageLayout source_layout, VkImage target, VkImageLayout target_layout,
          uint32_t width, uint32_t height) {
  VkImageBlit region = {};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
  region.dstSubresource = region.srcSubresource;
  region.dstOffsets[1] = region.srcOffsets[1];
  dfn.vkCmdBlitImage(command_buffer, source, source_layout, target, target_layout, 1, &region,
                     VK_FILTER_NEAREST);
}

}  // namespace

VulkanLsfg::Settings VulkanLsfg::SettingsFromCvars() {
  Settings settings;
  settings.multiplier = uint32_t(std::clamp(REXCVAR_GET(lsfg_multiplier), 1, 4));
  settings.performance = REXCVAR_GET(lsfg_performance);
  settings.flow_scale = float(std::clamp(REXCVAR_GET(lsfg_flow_scale), 25, 100)) / 100.0f;
  settings.dll_path = REXCVAR_GET(lsfg_dll);
  if (settings.dll_path.empty()) {
    if (const char* path = std::getenv("REX_LSFG_DLL")) {
      settings.dll_path = path;
    }
  }
  return settings;
}

VulkanLsfg::VulkanLsfg(const VulkanDevice* device) : device_(device) {}

VulkanLsfg::~VulkanLsfg() {
  Release();
  Finalize();
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  for (Slot& slot : slots_) {
    util::DestroyAndNullHandle(dfn.vkDestroyFence, device, slot.fence);
    util::DestroyAndNullHandle(dfn.vkDestroySemaphore, device, slot.acquired);
    util::DestroyAndNullHandle(dfn.vkDestroySemaphore, device, slot.copied);
  }
  util::DestroyAndNullHandle(dfn.vkDestroyCommandPool, device, command_pool_);
#if REX_PLATFORM_LINUX || defined(__ANDROID__)
  if (library_) {
    dlclose(library_);
  }
#endif
}

void VulkanLsfg::ReportOnce(const std::string& reason) {
  if (reason != reported_) {
    REXLOG_WARN("Lossless Scaling frame generation off: {}", reason);
    reported_ = reason;
  }
}

bool VulkanLsfg::LoadLibrary() {
  if (library_) {
    return true;
  }
  if (library_failed_) {
    return false;
  }
#if REX_PLATFORM_LINUX || defined(__ANDROID__)
  library_ = dlopen("liblsfg_framegen.so", RTLD_NOW | RTLD_LOCAL);
  if (library_) {
    init_ = dlsym(library_, "lsfg_bridge_init");
    create_ = dlsym(library_, "lsfg_bridge_create");
    present_ = dlsym(library_, "lsfg_bridge_present");
    delete_ = dlsym(library_, "lsfg_bridge_delete");
    finalize_ = dlsym(library_, "lsfg_bridge_finalize");
  }
  if (!library_ || !init_ || !create_ || !present_ || !delete_ || !finalize_) {
    const char* error = dlerror();
    ReportOnce(std::string("liblsfg_framegen.so not loaded (") + (error ? error : "") + ")");
    library_failed_ = true;
    return false;
  }
  return true;
#else
  ReportOnce("not built for this platform");
  library_failed_ = true;
  return false;
#endif
}

bool VulkanLsfg::CreateImage(Image& image) {
  VkImageCreateInfo create_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  create_info.imageType = VK_IMAGE_TYPE_2D;
  create_info.format = kFormat;
  create_info.extent = {width_, height_, 1};
  create_info.mipLevels = 1;
  create_info.arrayLayers = 1;
  create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  create_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return util::CreateDedicatedAllocationImage(device_, create_info,
                                              util::MemoryPurpose::kDeviceLocal, image.image,
                                              image.memory);
}

void VulkanLsfg::DestroyImage(Image& image) {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  util::DestroyAndNullHandle(dfn.vkDestroyImage, device, image.image);
  util::DestroyAndNullHandle(dfn.vkFreeMemory, device, image.memory);
}

void VulkanLsfg::DestroyContext() {
  if (context_ >= 0) {
    reinterpret_cast<PFN_lsfg_bridge_delete>(delete_)(context_);
    context_ = -1;
  }
}

void VulkanLsfg::Release() {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  for (Slot& slot : slots_) {
    if (slot.submitted) {
      dfn.vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_C(2000000000));
      slot.submitted = false;
    }
  }
  DestroyContext();
  for (Image& input : inputs_) {
    DestroyImage(input);
  }
  for (Image& output : outputs_) {
    DestroyImage(output);
  }
  outputs_.clear();
  width_ = height_ = 0;
  generations_ = 0;
  layouts_initialized_ = false;
}

void VulkanLsfg::Finalize() {
  if (initialized_) {
    reinterpret_cast<PFN_lsfg_bridge_finalize>(finalize_)();
    initialized_ = false;
  }
}

bool VulkanLsfg::Prepare(VkQueue queue, const Settings& settings, uint32_t width,
                         uint32_t height) {
  if (settings.multiplier < 2) {
    return false;
  }
  if (!LoadLibrary()) {
    return false;
  }
  if (settings.dll_path.empty()) {
    ReportOnce("no Lossless.dll (lsfg_dll)");
    return false;
  }
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  // The library's networks and shaders depend on every setting: start over on a change.
  if (initialized_ && !(settings == initialized_settings_)) {
    Release();
    Finalize();
  }
  if (!initialized_) {
    const VulkanInstance* instance = device_->vulkan_instance();
    LsfgBridgeInit init = {};
    init.get_instance_proc_addr = instance->functions().vkGetInstanceProcAddr;
    init.instance = instance->instance();
    init.physical_device = device_->physical_device();
    init.device = device;
    init.queue_family = device_->queue_family_graphics_compute();
    init.queue = queue;
    init.performance = settings.performance;
    init.hdr = 0;
    init.flow_scale = settings.flow_scale;
    init.generations = settings.multiplier - 1;
    init.dll_path = settings.dll_path.c_str();
    char error[512] = {};
    if (!reinterpret_cast<PFN_lsfg_bridge_init>(init_)(&init, error, sizeof(error))) {
      ReportOnce(std::string("the library could not start: ") + error);
      initialized_settings_ = settings;
      return false;
    }
    initialized_ = true;
    initialized_settings_ = settings;
    REXLOG_INFO("Lossless Scaling frame generation: {}x, {} mode, flow scale {}",
                settings.multiplier, settings.performance ? "performance" : "quality",
                settings.flow_scale);
  }
  if (context_ >= 0 && width == width_ && height == height_) {
    return true;
  }
  Release();
  width_ = width;
  height_ = height;
  generations_ = settings.multiplier - 1;
  outputs_.resize(generations_);
  bool created = CreateImage(inputs_[0]) && CreateImage(inputs_[1]);
  for (Image& output : outputs_) {
    created = created && CreateImage(output);
  }
  if (!created) {
    ReportOnce("its images could not be created");
    Release();
    return false;
  }
  std::vector<VkImage> output_images;
  for (const Image& output : outputs_) {
    output_images.push_back(output.image);
  }
  // The library records the inputs' and outputs' layouts as GENERAL: transition them before
  // it uses them.
  if (command_pool_ == VK_NULL_HANDLE) {
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device_->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
      ReportOnce("no command pool");
      Release();
      return false;
    }
    std::array<VkCommandBuffer, kSlotCount> command_buffers;
    VkCommandBufferAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = command_pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = uint32_t(kSlotCount);
    dfn.vkAllocateCommandBuffers(device, &allocate_info, command_buffers.data());
    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (size_t i = 0; i < kSlotCount; ++i) {
      slots_[i].command_buffer = command_buffers[i];
      dfn.vkCreateFence(device, &fence_info, nullptr, &slots_[i].fence);
      dfn.vkCreateSemaphore(device, &semaphore_info, nullptr, &slots_[i].acquired);
      dfn.vkCreateSemaphore(device, &semaphore_info, nullptr, &slots_[i].copied);
    }
  }
  {
    Slot& slot = slots_[slot_next_];
    if (slot.submitted) {
      dfn.vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
      slot.submitted = false;
    }
    dfn.vkResetFences(device, 1, &slot.fence);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn.vkBeginCommandBuffer(slot.command_buffer, &begin_info);
    for (Image* image : {&inputs_[0], &inputs_[1]}) {
      Barrier(dfn, slot.command_buffer, image->image, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_GENERAL);
    }
    for (Image& output : outputs_) {
      Barrier(dfn, slot.command_buffer, output.image, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_GENERAL);
    }
    dfn.vkEndCommandBuffer(slot.command_buffer);
    VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &slot.command_buffer;
    if (dfn.vkQueueSubmit(queue, 1, &submit_info, slot.fence) == VK_SUCCESS) {
      slot.submitted = true;
    }
    slot_next_ = (slot_next_ + 1) % kSlotCount;
  }
  char error[512] = {};
  context_ = reinterpret_cast<PFN_lsfg_bridge_create>(create_)(
      inputs_[0].image, inputs_[1].image, output_images.data(), uint32_t(output_images.size()),
      width, height, kFormat, error, sizeof(error));
  if (context_ < 0) {
    ReportOnce(std::string("no context: ") + error);
    Release();
    return false;
  }
  input_count_ = 0;
  reported_.clear();
  REXLOG_INFO("Lossless Scaling frame generation: {}x{} context, {} generated per frame",
              width, height, generations_);
  return true;
}

void VulkanLsfg::RecordInput(VkCommandBuffer command_buffer, VkImage source) {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkImage input = inputs_[input_count_ % 2].image;
  Barrier(dfn, command_buffer, source, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Barrier(dfn, command_buffer, input, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
          VK_IMAGE_LAYOUT_GENERAL);
  Blit(dfn, command_buffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, input,
       VK_IMAGE_LAYOUT_GENERAL, width_, height_);
  Barrier(dfn, command_buffer, source, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  Barrier(dfn, command_buffer, input, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
}

bool VulkanLsfg::Generate(VkQueue queue) {
  (void)queue;
  char error[512] = {};
  if (!reinterpret_cast<PFN_lsfg_bridge_present>(present_)(context_, error, sizeof(error))) {
    ReportOnce(std::string("generation failed: ") + error);
    Release();
    return false;
  }
  ++input_count_;
  // The first frame has no previous one to interpolate from.
  return input_count_ >= 2;
}

VkSemaphore VulkanLsfg::NextAcquireSemaphore() {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  Slot& slot = slots_[slot_next_];
  if (slot.submitted) {
    dfn.vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    slot.submitted = false;
  }
  slot_current_ = slot_next_;
  slot_next_ = (slot_next_ + 1) % kSlotCount;
  return slot.acquired;
}

VkSemaphore VulkanLsfg::SubmitOutput(VkQueue queue, uint32_t index, VkImage swapchain_image) {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  Slot& slot = slots_[slot_current_];
  const VkImage output = outputs_[index].image;
  dfn.vkResetFences(device, 1, &slot.fence);
  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(slot.command_buffer, &begin_info);
  Barrier(dfn, slot.command_buffer, output, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
          VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
  Barrier(dfn, slot.command_buffer, swapchain_image, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  Blit(dfn, slot.command_buffer, output, VK_IMAGE_LAYOUT_GENERAL, swapchain_image,
       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, width_, height_);
  Barrier(dfn, slot.command_buffer, swapchain_image, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  Barrier(dfn, slot.command_buffer, output, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
          VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
  dfn.vkEndCommandBuffer(slot.command_buffer);
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores = &slot.acquired;
  submit_info.pWaitDstStageMask = &wait_stage;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &slot.command_buffer;
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &slot.copied;
  if (dfn.vkQueueSubmit(queue, 1, &submit_info, slot.fence) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  slot.submitted = true;
  return slot.copied;
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
