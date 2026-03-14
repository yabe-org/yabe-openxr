#pragma once
#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS 1

// must be included before openxr_platform.h
#include <android_native_app_glue.h>
#include <vulkan/vulkan.h>
// this is a comment to disable reorder include by clang-format

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <functional>
#include <map>
#include <vector>

#include "openxr.hpp"

// Note: clangd adds this; surrounded by quote, not like `<vulkan/vulkan.hpp>`
// this may because CMake uses FetchContent
#include "vulkan/vulkan_raii.hpp"

namespace handover {
class Graphic {
 public:
  explicit Graphic(std::unique_ptr<OpenXrApp>& openxr_app);
  ~Graphic();

  [[nodiscard]] XrGraphicsBindingVulkanKHR get_graphics_binding() const;

  enum class SwapchainType : uint8_t {
    COLOR,
    DEPTH,
  };
  XrSwapchainImageBaseHeader* acquire_swapchain_image(
    XrSwapchain swapchain, SwapchainType type, uint32_t count);
  vk::raii::ImageView create_swapchain_image_views(
    XrSwapchain swapchain, uint32_t index, SwapchainType type, uint32_t format);

  void rendering_scope(vk::ImageView image_view, uint32_t width,
    uint32_t height, const std::function<void()>& func);
  void clear_color(
    vk::raii::ImageView& image_view, float r, float g, float b, float a);
  void clear_depth(vk::raii::ImageView& image_view, float depth);

 private:
  void init_instance(std::unique_ptr<OpenXrApp>& openxr_app);
  void init_device(std::unique_ptr<OpenXrApp>& openxr_app);

  uint32_t queue_family_index_ = UINT32_MAX;
  uint32_t queue_index_        = UINT32_MAX;

  struct SwapchainInfo {
    SwapchainType                          type;
    std::vector<XrSwapchainImageVulkanKHR> images;
    std::vector<vk::raii::ImageView>       image_views;
  };
  std::map<XrSwapchain, SwapchainInfo> swapchains_;

  // avoid using vk::raii::ImageView as key to suppress compile error
  // caused at .emplace()
  std::map<VkImageView, vk::ImageViewCreateInfo> image_views_;

  vk::raii::Context        context_;
  vk::raii::Instance       instance_        = nullptr;
  vk::raii::PhysicalDevice physical_device_ = nullptr;
  vk::raii::Device         device_          = nullptr;
  vk::raii::Queue          device_queue_    = nullptr;
  vk::raii::CommandPool    command_pool_    = nullptr;
  vk::raii::CommandBuffers command_buffers_ = nullptr;
  vk::raii::Fence          fence_           = nullptr;
};
}  // namespace handover
