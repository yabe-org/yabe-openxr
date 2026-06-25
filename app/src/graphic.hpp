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

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace yabe_quest {
class OpenXrApp;

class Graphic {
 public:
  Graphic(std::unique_ptr<OpenXrApp>& openxr_app, AAssetManager* asset_manager);
  ~Graphic();

  void init_pipeline(uint32_t color_format, uint32_t depth_format);

  [[nodiscard]] XrGraphicsBindingOpenGLESAndroidKHR get_graphics_binding() const;

  enum class SwapchainType : uint8_t {
    COLOR,
    DEPTH,
  };

  XrSwapchainImageBaseHeader* acquire_swapchain_image(
    XrSwapchain swapchain, SwapchainType type, uint32_t count);
  uint32_t get_swapchain_image(XrSwapchain swapchain, uint32_t index, SwapchainType type);

  class Buffer {
   public:
    Buffer(uint32_t buffer) : buffer_(buffer) {}
    ~Buffer() {}
    [[nodiscard]] uint32_t buffer() const { return this->buffer_; }

   private:
    uint32_t buffer_;
  };

  Graphic::Buffer create_buffer(const void* data, size_t size, uint32_t target);

  void rendering_scope(const std::function<void()>& func);
  void set_render_attachments(uint32_t color_texture, uint32_t depth_texture,
    uint32_t width, uint32_t height, float clear_r, float clear_g,
    float clear_b, float clear_a, float clear_depth);
  void set_viewport(float x, float y, float width, float height, float min_depth, float max_depth);
  void set_scissor(int32_t offset_x, int32_t offset_y, uint32_t width, uint32_t height);
  void set_buffers(const Graphic::Buffer& vertex_buffer, const Graphic::Buffer& index_buffer);
  void draw_indexed(uint32_t index_count);

 private:
  void init_egl();
  void init_shader(AAssetManager* asset_manager);

  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLContext egl_context_ = EGL_NO_CONTEXT;
  EGLSurface egl_surface_ = EGL_NO_SURFACE;
  EGLConfig  egl_config_  = nullptr;

  struct SwapchainInfo {
    SwapchainType type;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
  };
  std::map<XrSwapchain, SwapchainInfo> swapchains_;

  uint32_t shader_program_ = 0;
  uint32_t vao_ = 0;
  uint32_t frame_buffer_ = 0;
};
}  // namespace yabe_quest
