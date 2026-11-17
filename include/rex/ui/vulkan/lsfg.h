/**
 * Lossless Scaling frame generation on the Vulkan presenter: lsfg-vk's framegen
 * (liblsfg_framegen.so, loaded by name) on the presenter's device and queue,
 * with the shaders from the player's own Lossless.dll.
 *
 * For each new guest frame the presenter copies the presented image into the
 * next of two inputs, has the library generate the frames between the
 * previous one and it, and presents those, each copied into a swapchain image
 * of its own, before the rendered frame. A FIFO swapchain paces them: 40 fps
 * times 3 on a 120 Hz screen gives one frame each refresh.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanDevice;

class VulkanLsfg {
 public:
  struct Settings {
    // Presented frames per rendered one; 1 is off.
    uint32_t multiplier = 1;
    // LSFG 3.1's performance networks instead of quality.
    bool performance = false;
    // The optical flow's resolution scale, 0.25 to 1.
    float flow_scale = 1.0f;
    std::string dll_path;
    bool operator==(const Settings& other) const {
      return multiplier == other.multiplier && performance == other.performance &&
             flow_scale == other.flow_scale && dll_path == other.dll_path;
    }
  };

  // lsfg_* cvars; dll_path falls back to REX_LSFG_DLL from the environment.
  static Settings SettingsFromCvars();

  explicit VulkanLsfg(const VulkanDevice* device);
  ~VulkanLsfg();
  VulkanLsfg(const VulkanLsfg&) = delete;
  VulkanLsfg& operator=(const VulkanLsfg&) = delete;

  // With `queue` held: the library and the context for `settings` at
  // `width`x`height`, (re)created as needed; false when frames can't be
  // generated (the reason is logged once per settings).
  bool Prepare(VkQueue queue, const Settings& settings, uint32_t width, uint32_t height);
  uint32_t generations() const { return generations_; }

  // Records the copy of the presented `source` (in the PRESENT_SRC layout, left
  // in it) into the next input.
  void RecordInput(VkCommandBuffer command_buffer, VkImage source);
  // With `queue` held, after the submission holding RecordInput: submits the
  // generation of every frame between the previous input and this one.
  bool Generate(VkQueue queue);

  // A semaphore for acquiring the swapchain image for the next generated frame.
  VkSemaphore NextAcquireSemaphore();
  // With `queue` held: copies generated frame `index` into `swapchain_image`
  // once the `acquired` semaphore (NextAcquireSemaphore's) is signaled; returns
  // the semaphore the present waits for, or null on failure.
  VkSemaphore SubmitOutput(VkQueue queue, uint32_t index, VkImage swapchain_image);

  // Drops the context and the images (after the GPU is done with them).
  void Release();

 private:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
  };
  struct Slot {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore copied = VK_NULL_HANDLE;
    bool submitted = false;
  };
  static constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  static constexpr size_t kSlotCount = 12;

  bool LoadLibrary();
  bool CreateImage(Image& image);
  void DestroyImage(Image& image);
  void DestroyContext();
  void Finalize();
  void ReportOnce(const std::string& reason);

  const VulkanDevice* device_;
  void* library_ = nullptr;
  bool library_failed_ = false;
  void* init_ = nullptr;
  void* create_ = nullptr;
  void* present_ = nullptr;
  void* delete_ = nullptr;
  void* finalize_ = nullptr;

  bool initialized_ = false;
  Settings initialized_settings_;
  int32_t context_ = -1;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t generations_ = 0;
  std::array<Image, 2> inputs_;
  std::vector<Image> outputs_;
  uint64_t input_count_ = 0;
  bool layouts_initialized_ = false;

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  std::array<Slot, kSlotCount> slots_;
  size_t slot_next_ = 0;
  size_t slot_current_ = 0;

  std::string reported_;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
