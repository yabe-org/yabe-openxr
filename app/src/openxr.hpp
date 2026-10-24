#pragma once

#include <android_native_app_glue.h>
#include <openxr/openxr.h>

#include <memory>
#include <vector>

#include "graphic.hpp"

namespace yabe_openxr {
class OpenXrApp {
 public:
  OpenXrApp(android_app* app, std::unique_ptr<Graphic>& graphic);
  ~OpenXrApp();

  /// @return false if the app is exited
  bool pool(std::unique_ptr<Graphic>& graphic);

  [[nodiscard]] bool is_session_running() const {
    return is_session_running_;
  }

 private:
  void init_instance(android_app* app);
  void init_session(std::unique_ptr<Graphic>& graphic);
  void init_formats();
  void init_views(std::unique_ptr<Graphic>& graphic);
  void init_buffers(std::unique_ptr<Graphic>& graphic);
  void init_environment_blend_mode();
  /// @return false if the passthrough cannot be started
  bool init_fb_passthrough();

  void handle_session_state_changed(
    const XrEventDataSessionStateChanged& event);

  struct RenderLayerInfo {
    XrTime                                     predicted_display_time;
    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerPassthroughFB            layer_passthrough{
      XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    XrCompositionLayerProjection layer_projection{
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
    XrSwapchain           swapchain        = XR_NULL_HANDLE;
    int64_t               swapchain_format = 0;
    std::vector<uint32_t> image_views;
  };
  std::vector<SwapchainInfo> color_swapchains_;
  std::vector<SwapchainInfo> depth_swapchains_;

  XrSpace local_space_;

  XrEnvironmentBlendMode environment_blend_mode_ =
    XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  bool                 is_fb_passthrough_enabled_ = false;
  XrPassthroughFB      passthrough_               = XR_NULL_HANDLE;
  XrPassthroughLayerFB passthrough_layer_         = XR_NULL_HANDLE;
};
}  // namespace yabe_openxr
