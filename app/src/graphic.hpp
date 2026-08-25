#pragma once

// must be included before openxr_platform.h
#include <android_native_app_glue.h>
// this is a comment to disable reorder include by clang-format

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <android/asset_manager.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <glm/glm.hpp>
#include <map>
#include <vector>

namespace yabe_openxr {
class Graphic {
 public:
  explicit Graphic(const std::atomic_bool& compositor_ready);
  ~Graphic();

  [[nodiscard]] XrGraphicsBindingOpenGLESAndroidKHR get_graphics_binding()
    const;

  enum class SwapchainType : uint8_t {
    COLOR,
    DEPTH,
  };

  XrSwapchainImageBaseHeader* acquire_swapchain_image(
    XrSwapchain swapchain, SwapchainType type, uint32_t count);
  uint32_t get_swapchain_image(
    XrSwapchain swapchain, uint32_t index, SwapchainType type);
  void draw(uint32_t color_texture, uint32_t depth_texture, int32_t width,
    int32_t height, const glm::mat4& vp_mat);

 private:
  void init_egl();
  void init_shader(AAssetManager* asset_manager);

  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLContext egl_context_ = EGL_NO_CONTEXT;
  EGLSurface egl_surface_ = EGL_NO_SURFACE;
  EGLConfig  egl_config_  = nullptr;

  struct SwapchainInfo {
    SwapchainType                            type;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
  };
  std::map<XrSwapchain, SwapchainInfo> swapchains_;
  uint32_t                             frame_buffer_ = 0;
  const std::atomic_bool&              compositor_ready_;
};
}  // namespace yabe_openxr
