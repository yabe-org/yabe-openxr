#pragma once
#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS 1

// must be included before openxr_platform.h
#include <android_native_app_glue.h>
#include <vulkan/vulkan.h>
// this is a comment to disable reorder include by clang-format

#include <android/asset_manager.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <functional>
#include <map>
#include <vector>

// Note: clangd adds this; surrounded by quote, not like `<vulkan/vulkan.hpp>`
// this may because CMake uses FetchContent
#include "vulkan/vulkan_raii.hpp"

namespace handover {
class OpenXrApp;

// Vulkan instance needs OpenXrApp, who tells what Vulkan instance extensions
// are required by OpenXR, so OpenXrApp must be created before Graphic
class Graphic {
 public:
  Graphic(std::unique_ptr<OpenXrApp>& openxr_app, AAssetManager* asset_manager);
  ~Graphic();

  // pipeline creation depends on OpenXR swapchain formats,
  // which can be obtain only after OpenXR session is created
  // (xrEnumerateSwapchainFormats() requires the session as an argument)
  void init_pipeline(uint32_t color_format, uint32_t depth_format);

  [[nodiscard]] XrGraphicsBindingVulkanKHR get_graphics_binding() const;

  enum class SwapchainType : uint8_t {
    COLOR,
    DEPTH,
  };
  XrSwapchainImageBaseHeader* acquire_swapchain_image(
    XrSwapchain swapchain, SwapchainType type, uint32_t count);
  vk::raii::ImageView create_swapchain_image_views(
    XrSwapchain swapchain, uint32_t index, SwapchainType type, uint32_t format);

  class Buffer {
   public:
    Buffer(vk::raii::Buffer&& buffer, vk::raii::DeviceMemory&& memory)
        : buffer_(std::move(buffer)), memory_(std::move(memory)) {
    }
    [[nodiscard]]
    vk::Buffer buffer() const {
      return this->buffer_;
    }

   private:
    vk::raii::Buffer       buffer_;
    vk::raii::DeviceMemory memory_;
  };
  Graphic::Buffer create_buffer(
    const void* data, vk::DeviceSize size, vk::BufferUsageFlags usage);

  void rendering_scope(
    const std::function<void(vk::raii::CommandBuffer&)>& func);
  void set_render_attachments(vk::raii::CommandBuffer& cmd,
    vk::raii::ImageView& color_view, vk::raii::ImageView& depth_view,
    uint32_t width, uint32_t height, float clear_r, float clear_g,
    float clear_b, float clear_a, float clear_depth);
  void set_viewport(vk::raii::CommandBuffer& cmd, float x, float y, float width,
    float height, float min_depth, float max_depth);
  void set_scissor(vk::raii::CommandBuffer& cmd, float offset_x, float offset_y,
    uint32_t width, uint32_t height);
  void set_buffers(vk::raii::CommandBuffer& cmd,
    const Graphic::Buffer& vertex_buffer, const Graphic::Buffer& index_buffer);
  void draw_indexed(vk::raii::CommandBuffer& cmd, uint32_t index_count);

 private:
  void init_instance(std::unique_ptr<OpenXrApp>& openxr_app);
  void init_device(std::unique_ptr<OpenXrApp>& openxr_app);
  void init_shader(AAssetManager* asset_manager);

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

  bool                          is_render_pass_started_ = false;
  vk::raii::ShaderModule        shader_module_          = nullptr;
  vk::raii::DescriptorSetLayout descriptor_set_layout_  = nullptr;
  vk::raii::PipelineLayout      pipeline_layout_        = nullptr;
  vk::raii::Pipeline            pipeline_               = nullptr;
  vk::raii::RenderPass          render_pass_            = nullptr;
  vk::raii::Framebuffer         frame_buffer_           = nullptr;
};
}  // namespace handover
