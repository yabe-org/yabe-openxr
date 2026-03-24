#pragma once

#include <android_native_app_glue.h>
#include <openxr/openxr.h>

#include <string>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

#include "graphic.hpp"

namespace handover {
class OpenXrApp {
 public:
  explicit OpenXrApp(android_app* app);
  ~OpenXrApp();

  /// must be called after initializing Graphic instance,
  /// which depends on OpenXR instance, is initialized
  void init_later(std::unique_ptr<Graphic>& graphic);
  /// @return false if the app is exited
  bool pool(std::unique_ptr<Graphic>& graphic);

  [[nodiscard]] std::vector<std::string> get_vulkan_instance_extensions() const;
  [[nodiscard]] std::vector<std::string> get_vulkan_device_extensions() const;
  [[nodiscard]] VkPhysicalDevice         get_vulkan_physical_device(
            const vk::raii::Instance& vk_instance) const;

 private:
  void init_instance(android_app* app);
  void init_session(std::unique_ptr<Graphic>& graphic);
  void init_formats();
  void init_views(std::unique_ptr<Graphic>& graphic);
  void init_buffers(std::unique_ptr<Graphic>& graphic);

  void handle_session_state_changed(
    const XrEventDataSessionStateChanged& event);

  struct RenderLayerInfo {
    XrTime                                     predicted_display_time;
    std::vector<XrCompositionLayerProjection*> layers;
    XrCompositionLayerProjection               layer_projection{
      XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerProjectionView> layer_projection_views;
  };
  void render(std::unique_ptr<Graphic>& graphic);
  void render_layer(
    std::unique_ptr<Graphic>& graphic, RenderLayerInfo& layer_info);

  bool is_session_running_ = false;
  bool is_app_running_     = true;

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId system_id_;
  XrSession  session_;

  uint32_t color_format_ = 0;
  uint32_t depth_format_ = 0;

  std::vector<XrViewConfigurationView> view_config_views_;
  XrViewConfigurationType              view_config_type_ =
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  struct SwapchainInfo {
    XrSwapchain                      swapchain        = XR_NULL_HANDLE;
    int64_t                          swapchain_format = 0;
    std::vector<vk::raii::ImageView> image_views;
  };
  std::vector<SwapchainInfo> color_swapchains_;
  std::vector<SwapchainInfo> depth_swapchains_;

  XrSpace local_space_;

  std::unique_ptr<Graphic::Buffer> vertex_buffer_ = nullptr;
  std::unique_ptr<Graphic::Buffer> index_buffer_  = nullptr;
};
}  // namespace handover
