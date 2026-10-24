#include "graphic.hpp"

#include <GLES3/gl3.h>

#include <glm/ext.hpp>
#include <glm/glm.hpp>
#include <stdexcept>
#include <vector>

#include "log.hpp"
#include "openxr.hpp"
#include "yabe-compositor.h"

namespace yabe_openxr {

Graphic::Graphic(const std::atomic_bool& compositor_ready)
    : compositor_ready_(compositor_ready) {
  init_egl();
}

Graphic::~Graphic() {
  LOG_INFO("Destroying Graphic");
  if (this->egl_display_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(
      this->egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (this->egl_context_ != EGL_NO_CONTEXT) {
      eglDestroyContext(this->egl_display_, this->egl_context_);
    }
    if (this->egl_surface_ != EGL_NO_SURFACE) {
      eglDestroySurface(this->egl_display_, this->egl_surface_);
    }
    eglTerminate(this->egl_display_);
  }
}

void Graphic::init_egl() {
  this->egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (this->egl_display_ == EGL_NO_DISPLAY) {
    throw std::runtime_error("eglGetDisplay failed");
  }

  if (!eglInitialize(this->egl_display_, nullptr, nullptr)) {
    throw std::runtime_error("eglInitialize failed");
  }

  const EGLint config_attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};

  int num_configs = 0;
  if (!eglChooseConfig(this->egl_display_, config_attribs, &this->egl_config_,
        1, &num_configs) ||
      num_configs == 0) {
    throw std::runtime_error("eglChooseConfig failed");
  }

  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};

  this->egl_context_ = eglCreateContext(
    this->egl_display_, this->egl_config_, EGL_NO_CONTEXT, context_attribs);
  if (this->egl_context_ == EGL_NO_CONTEXT) {
    throw std::runtime_error("eglCreateContext failed");
  }

  const EGLint surface_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  this->egl_surface_             = eglCreatePbufferSurface(
    this->egl_display_, this->egl_config_, surface_attribs);
  if (this->egl_surface_ == EGL_NO_SURFACE) {
    throw std::runtime_error("eglCreatePbufferSurface failed");
  }

  if (!eglMakeCurrent(this->egl_display_, this->egl_surface_,
        this->egl_surface_, this->egl_context_)) {
    throw std::runtime_error("eglMakeCurrent failed");
  }
  glGenFramebuffers(1, &this->frame_buffer_);
}

XrGraphicsBindingOpenGLESAndroidKHR Graphic::get_graphics_binding() const {
  return {
    .type    = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR,
    .next    = nullptr,
    .display = this->egl_display_,
    .config  = this->egl_config_,
    .context = this->egl_context_,
  };
}

XrSwapchainImageBaseHeader* Graphic::acquire_swapchain_image(
  XrSwapchain swapchain, SwapchainType type, uint32_t count) {
  auto& info = this->swapchains_[swapchain];
  info.type  = type;
  info.images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
  return reinterpret_cast<XrSwapchainImageBaseHeader*>(info.images.data());
}

uint32_t Graphic::get_swapchain_image(
  XrSwapchain swapchain, uint32_t index, SwapchainType /*type*/) {
  return this->swapchains_[swapchain].images[index].image;
}

void Graphic::draw(uint32_t color_texture, uint32_t depth_texture,
  int32_t width, int32_t height, const glm::mat4& proj_mat,
  const glm::mat4& view_mat) {
  glBindFramebuffer(GL_FRAMEBUFFER, this->frame_buffer_);
  glFramebufferTexture2D(
    GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture, 0);
  glFramebufferTexture2D(
    GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_texture, 0);
  glViewport(0, 0, width, height);
  glClearColor(0.F, 0.F, 0.F, 0.F);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (!this->compositor_ready_.load()) {
    return;
  }
  yabe_compositor_draw(glm::value_ptr(proj_mat), glm::value_ptr(view_mat));
}
}  // namespace yabe_openxr
