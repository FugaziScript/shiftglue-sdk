/**
 * Snapdragon Game Super Resolution 2 (Qualcomm, BSD-3-Clause), the 2-pass compute variant, as a
 * temporal upscaler provider (rex/ui/vulkan/upscaler.h).
 *
 * Qualcomm made it for Adreno GPUs, where FSR 2 and 3's many full-resolution compute passes are
 * slow: Convert writes the render-size motion, depth clip and YCoCg colour, Upscale blends them
 * with the reprojected history into the output and the next history. The shaders
 * (ui/shaders/sgsr2_*.cs.glsl) take the presenter's colour, depth, motion vectors and jitter.
 *
 * sgsr2_fast runs the algorithm of Qualcomm's 2-pass fragment variant instead, made for mobile
 * GPUs: five input taps instead of nine and the colour read straight from the input, no YCoCg.
 */

#include <rex/ui/vulkan/upscaler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(sgsr2_fast, false, "UI/Presenter",
                    "GSR 2: the lighter algorithm of Qualcomm's fragment variant (five input "
                    "taps, no YCoCg pass) instead of the compute variant's");

namespace rex {
namespace ui {
namespace vulkan {

namespace {

#include "../shaders/vulkan_spirv/sgsr2_convert_cs.h"
#include "../shaders/vulkan_spirv/sgsr2_upscale_cs.h"
#include "../shaders/vulkan_spirv/sgsr2_fast_convert_cs.h"
#include "../shaders/vulkan_spirv/sgsr2_fast_upscale_cs.h"

// The shaders' push constants.
struct Sgsr2Constants {
  uint32_t render_size[2];
  uint32_t display_size[2];
  float render_size_rcp[2];
  float display_size_rcp[2];
  float jitter_offset[2];
  float pre_exposure;
  float camera_fov_angle_hor;
  float min_lerp_contribution;
  uint32_t same_camera_frame_num;
  uint32_t reset;
  uint32_t depth_inverted;
};
static_assert(sizeof(Sgsr2Constants) == 64);

constexpr uint32_t kBindings = 5;
// Descriptor sets per pass, used in turn: a set is rewritten only after the frames in flight
// that read it are done.
constexpr uint32_t kSetRing = 8;

class VulkanSgsr2Upscaler final : public VulkanUpscaler {
 public:
  explicit VulkanSgsr2Upscaler(const VulkanDevice* device) : device_(device) {
    info_.id = "gsr2";
    info_.name = "Snapdragon Game Super Resolution 2";
    info_.version = "2 (2-pass compute)";
    info_.kind = UpscalerKind::kTemporal;
    info_.mode_mask = (1u << uint32_t(UpscalerMode::kCount)) - 1;
    info_.uses_depth = true;
    info_.uses_motion_vectors = true;
    info_.uses_jitter = true;
    info_.available = device != nullptr;
    if (!info_.available) info_.unavailable_reason = "no Vulkan device";
  }
  ~VulkanSgsr2Upscaler() override {
    Release();
    DestroyPipelines();
  }

  const UpscalerInfo& info() const override { return info_; }

  bool Prepare(uint32_t max_render_width, uint32_t max_render_height, uint32_t max_output_width,
               uint32_t max_output_height, bool) override {
    if (!info_.available || !EnsurePipelines()) return false;
    if (max_render_width <= render_width_ && max_render_height <= render_height_ &&
        max_output_width == output_width_ && max_output_height == output_height_ &&
        history_[0].image) {
      return true;
    }
    Release();
    render_width_ = max_render_width;
    render_height_ = max_render_height;
    output_width_ = max_output_width;
    output_height_ = max_output_height;
    if (!CreateImage(motion_depth_clip_, VK_FORMAT_R16G16B16A16_SFLOAT, render_width_,
                     render_height_) ||
        !CreateImage(ycocg_, VK_FORMAT_R32_UINT, render_width_, render_height_) ||
        !CreateImage(history_[0], VK_FORMAT_R16G16B16A16_SFLOAT, output_width_, output_height_) ||
        !CreateImage(history_[1], VK_FORMAT_R16G16B16A16_SFLOAT, output_width_, output_height_)) {
      REXLOG_ERROR("GSR 2: cannot create the images for {}x{} to {}x{}", render_width_,
                   render_height_, output_width_, output_height_);
      Release();
      return false;
    }
    images_initialized_ = false;
    REXLOG_INFO("GSR 2: {}x{} to {}x{}", render_width_, render_height_, output_width_,
                output_height_);
    return true;
  }

  bool Dispatch(VkCommandBuffer command_buffer, const VulkanUpscalerInputs& inputs) override {
    const UpscalerFrame& frame = inputs.frame;
    if (!history_[0].image || !inputs.color.view || !inputs.depth.view ||
        !inputs.motion_vectors.view || !inputs.output.view ||
        frame.render_width > render_width_ || frame.render_height > render_height_ ||
        frame.output_width != output_width_ || frame.output_height != output_height_) {
      return false;
    }
    const VulkanDevice::Functions& dfn = device_->functions();
    const VkDevice device = device_->device();

    // Every internal image stays in GENERAL; after a Prepare they start there, the history
    // dropped (reset) on that frame.
    bool reset = frame.reset;
    // The two variants keep different histories.
    const bool fast = REXCVAR_GET(sgsr2_fast);
    if (fast != fast_) {
      fast_ = fast;
      reset = true;
      REXLOG_INFO("GSR 2: {} variant", fast ? "fast" : "compute");
    }
    if (!images_initialized_) {
      std::array<VkImageMemoryBarrier, 4> barriers{};
      const VkImage images[4] = {motion_depth_clip_.image, ycocg_.image, history_[0].image,
                                 history_[1].image};
      for (uint32_t i = 0; i < 4; ++i) {
        VkImageMemoryBarrier& barrier = barriers[i];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[i];
        barrier.subresourceRange = util::InitializeSubresourceRange();
      }
      dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                               uint32_t(barriers.size()), barriers.data());
      images_initialized_ = true;
      reset = true;
    } else {
      // The previous frame's passes wrote the history this one reads.
      ComputeBarrier(command_buffer);
    }

    Sgsr2Constants constants = {};
    constants.render_size[0] = frame.render_width;
    constants.render_size[1] = frame.render_height;
    constants.display_size[0] = frame.output_width;
    constants.display_size[1] = frame.output_height;
    constants.render_size_rcp[0] = 1.0f / float(frame.render_width);
    constants.render_size_rcp[1] = 1.0f / float(frame.render_height);
    constants.display_size_rcp[0] = 1.0f / float(frame.output_width);
    constants.display_size_rcp[1] = 1.0f / float(frame.output_height);
    constants.jitter_offset[0] = frame.jitter_x;
    constants.jitter_offset[1] = frame.jitter_y;
    // SDR guest output: the passes' simple tonemap and its inverse at an exposure of 1.
    constants.pre_exposure = 1.0f;
    constants.camera_fov_angle_hor = std::tan(frame.camera_fov_y * 0.5f) *
                                     float(frame.render_width) / float(frame.render_height);
    constants.min_lerp_contribution = 0.0f;
    // The game moves its camera every frame in a race; without the view matrices the
    // still-camera taps are not used.
    constants.same_camera_frame_num = 0;
    constants.reset = reset ? 1 : 0;
    constants.depth_inverted = frame.depth_inverted ? 1 : 0;

    const uint32_t set_index = frame_counter_ % kSetRing;
    const uint32_t read_history = frame_counter_ & 1;
    const uint32_t write_history = read_history ^ 1;
    ++frame_counter_;

    // Convert.
    {
      const VkDescriptorSet set = convert_sets_[set_index];
      const VkDescriptorImageInfo infos[kBindings] = {
          {nearest_sampler_, inputs.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {nearest_sampler_, inputs.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {nearest_sampler_, inputs.motion_vectors.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {VK_NULL_HANDLE, motion_depth_clip_.view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, ycocg_.view, VK_IMAGE_LAYOUT_GENERAL},
      };
      WriteSet(set, infos, 3);
      dfn.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            fast ? fast_convert_pipeline_ : convert_pipeline_);
      dfn.vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  pipeline_layout_, 0, 1, &set, 0, nullptr);
      dfn.vkCmdPushConstants(command_buffer, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             sizeof(constants), &constants);
      dfn.vkCmdDispatch(command_buffer, (frame.render_width + 7) / 8,
                        (frame.render_height + 7) / 8, 1);
    }
    ComputeBarrier(command_buffer);
    // Upscale.
    {
      const VkDescriptorSet set = upscale_sets_[set_index];
      const VkDescriptorImageInfo infos[kBindings] = {
          {linear_sampler_, history_[read_history].view, VK_IMAGE_LAYOUT_GENERAL},
          {linear_sampler_, motion_depth_clip_.view, VK_IMAGE_LAYOUT_GENERAL},
          fast ? VkDescriptorImageInfo{nearest_sampler_, inputs.color.view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
               : VkDescriptorImageInfo{nearest_sampler_, ycocg_.view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, inputs.output.view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, history_[write_history].view, VK_IMAGE_LAYOUT_GENERAL},
      };
      WriteSet(set, infos, 3);
      dfn.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            fast ? fast_upscale_pipeline_ : upscale_pipeline_);
      dfn.vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  pipeline_layout_, 0, 1, &set, 0, nullptr);
      dfn.vkCmdPushConstants(command_buffer, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             sizeof(constants), &constants);
      dfn.vkCmdDispatch(command_buffer, (frame.output_width + 7) / 8,
                        (frame.output_height + 7) / 8, 1);
    }
    (void)device;
    return true;
  }

  void Release() override {
    for (Image* image : {&motion_depth_clip_, &ycocg_, &history_[0], &history_[1]}) {
      DestroyImage(*image);
    }
    render_width_ = render_height_ = output_width_ = output_height_ = 0;
    images_initialized_ = false;
  }

 private:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  bool CreateImage(Image& out, VkFormat format, uint32_t width, uint32_t height) {
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!util::CreateDedicatedAllocationImage(device_, image_info,
                                              util::MemoryPurpose::kDeviceLocal, out.image,
                                              out.memory)) {
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = out.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = util::InitializeSubresourceRange();
    return device_->functions().vkCreateImageView(device_->device(), &view_info, nullptr,
                                                  &out.view) == VK_SUCCESS;
  }

  void DestroyImage(Image& image) {
    const VulkanDevice::Functions& dfn = device_->functions();
    const VkDevice device = device_->device();
    util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, image.view);
    util::DestroyAndNullHandle(dfn.vkDestroyImage, device, image.image);
    util::DestroyAndNullHandle(dfn.vkFreeMemory, device, image.memory);
  }

  void ComputeBarrier(VkCommandBuffer command_buffer) {
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    device_->functions().vkCmdPipelineBarrier(
        command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
  }

  // Bindings below storage_first are combined image samplers, the rest storage images.
  void WriteSet(VkDescriptorSet set, const VkDescriptorImageInfo* infos, uint32_t storage_first) {
    std::array<VkWriteDescriptorSet, kBindings> writes{};
    for (uint32_t i = 0; i < kBindings; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = i < storage_first ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                   : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &infos[i];
    }
    device_->functions().vkUpdateDescriptorSets(device_->device(), kBindings, writes.data(), 0,
                                                nullptr);
  }

  bool EnsurePipelines() {
    if (upscale_pipeline_) return true;
    if (pipelines_failed_) return false;
    pipelines_failed_ = true;
    const VulkanDevice::Functions& dfn = device_->functions();
    const VkDevice device = device_->device();

    VkSamplerCreateInfo sampler_info = {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &nearest_sampler_) != VK_SUCCESS) {
      return false;
    }
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &linear_sampler_) != VK_SUCCESS) {
      return false;
    }

    // One layout for both passes: three sampled images, then two storage images.
    std::array<VkDescriptorSetLayoutBinding, kBindings> bindings{};
    for (uint32_t i = 0; i < kBindings; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType =
          i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info = {};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = kBindings;
    set_layout_info.pBindings = bindings.data();
    if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout_) !=
        VK_SUCCESS) {
      return false;
    }
    const VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                            sizeof(Sgsr2Constants)};
    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout_) !=
        VK_SUCCESS) {
      return false;
    }
    const VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * 2 * kSetRing},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * 2 * kSetRing},
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 2 * kSetRing;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_) !=
        VK_SUCCESS) {
      return false;
    }
    std::array<VkDescriptorSetLayout, 2 * kSetRing> layouts;
    layouts.fill(set_layout_);
    std::array<VkDescriptorSet, 2 * kSetRing> sets{};
    VkDescriptorSetAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = descriptor_pool_;
    allocate_info.descriptorSetCount = uint32_t(layouts.size());
    allocate_info.pSetLayouts = layouts.data();
    if (dfn.vkAllocateDescriptorSets(device, &allocate_info, sets.data()) != VK_SUCCESS) {
      return false;
    }
    std::copy_n(sets.begin(), kSetRing, convert_sets_.begin());
    std::copy_n(sets.begin() + kSetRing, kSetRing, upscale_sets_.begin());

    convert_pipeline_ = util::CreateComputePipeline(device_, pipeline_layout_, sgsr2_convert_cs,
                                                    sizeof(sgsr2_convert_cs));
    upscale_pipeline_ = util::CreateComputePipeline(device_, pipeline_layout_, sgsr2_upscale_cs,
                                                    sizeof(sgsr2_upscale_cs));
    fast_convert_pipeline_ = util::CreateComputePipeline(
        device_, pipeline_layout_, sgsr2_fast_convert_cs, sizeof(sgsr2_fast_convert_cs));
    fast_upscale_pipeline_ = util::CreateComputePipeline(
        device_, pipeline_layout_, sgsr2_fast_upscale_cs, sizeof(sgsr2_fast_upscale_cs));
    if (!convert_pipeline_ || !upscale_pipeline_ || !fast_convert_pipeline_ ||
        !fast_upscale_pipeline_) {
      REXLOG_ERROR("GSR 2: cannot create the compute pipelines");
      return false;
    }
    pipelines_failed_ = false;
    return true;
  }

  void DestroyPipelines() {
    const VulkanDevice::Functions& dfn = device_->functions();
    const VkDevice device = device_->device();
    util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, convert_pipeline_);
    util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, upscale_pipeline_);
    util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, fast_convert_pipeline_);
    util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, fast_upscale_pipeline_);
    util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, pipeline_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, descriptor_pool_);
    util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, set_layout_);
    util::DestroyAndNullHandle(dfn.vkDestroySampler, device, nearest_sampler_);
    util::DestroyAndNullHandle(dfn.vkDestroySampler, device, linear_sampler_);
  }

  const VulkanDevice* device_;
  UpscalerInfo info_;
  bool pipelines_failed_ = false;
  VkSampler nearest_sampler_ = VK_NULL_HANDLE;
  VkSampler linear_sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSetRing> convert_sets_{};
  std::array<VkDescriptorSet, kSetRing> upscale_sets_{};
  VkPipeline convert_pipeline_ = VK_NULL_HANDLE;
  VkPipeline upscale_pipeline_ = VK_NULL_HANDLE;
  VkPipeline fast_convert_pipeline_ = VK_NULL_HANDLE;
  VkPipeline fast_upscale_pipeline_ = VK_NULL_HANDLE;
  bool fast_ = false;

  uint32_t render_width_ = 0, render_height_ = 0, output_width_ = 0, output_height_ = 0;
  Image motion_depth_clip_, ycocg_;
  std::array<Image, 2> history_;
  bool images_initialized_ = false;
  uint32_t frame_counter_ = 0;
};

}  // namespace

std::unique_ptr<VulkanUpscaler> CreateVulkanSgsr2Upscaler(const VulkanDevice* device) {
  return std::make_unique<VulkanSgsr2Upscaler>(device);
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
