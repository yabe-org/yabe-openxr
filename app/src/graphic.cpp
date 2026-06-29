#include "graphic.hpp"

#include <glm/ext.hpp>
#include <glm/glm.hpp>
#include <stdexcept>
#include <vector>

#include "log.hpp"
#include "openxr.hpp"

namespace yabe_openxr {

Graphic::Graphic(
  std::unique_ptr<OpenXrApp>& /*openxr_app*/, AAssetManager* asset_manager) {
  init_egl();
  init_shader(asset_manager);
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
  glGenVertexArrays(1, &this->vao_);
}

void Graphic::init_pipeline(
  uint32_t /*color_format*/, uint32_t /*depth_format*/) {
  // Not strictly needed in GL like it is in Vulkan
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

Graphic::Buffer Graphic::create_buffer(
  const void* data, size_t size, uint32_t target) {
  uint32_t buffer = 0;
  glGenBuffers(1, &buffer);
  glBindBuffer(target, buffer);
  glBufferData(target, size, data, GL_STATIC_DRAW);
  glBindBuffer(target, 0);
  return Buffer(buffer);
}

void Graphic::init_shader(AAssetManager* /*asset_manager*/) {
  const char* vertex_shader_source   = R"(#version 320 es
    uniform mat4 vp;
    layout(location = 0) in vec3 inPosition;
    void main() {
        gl_Position = vp * vec4(inPosition, 1.0);
    }
  )";
  const char* fragment_shader_source = R"(#version 320 es
    precision mediump float;
    out vec4 FragColor;
    void main() {
        FragColor = vec4(1.0, 0.5, 0.2, 1.0);
    }
  )";

  auto compile_shader = [](const char* source, GLenum type) -> uint32_t {
    uint32_t shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
      char infoLog[512];
      glGetShaderInfoLog(shader, 512, nullptr, infoLog);
      LOG_ERROR("Shader compilation failed: {}", infoLog);
    }
    return shader;
  };

  uint32_t vertex_shader =
    compile_shader(vertex_shader_source, GL_VERTEX_SHADER);
  uint32_t fragment_shader =
    compile_shader(fragment_shader_source, GL_FRAGMENT_SHADER);

  this->shader_program_ = glCreateProgram();
  glAttachShader(this->shader_program_, vertex_shader);
  glAttachShader(this->shader_program_, fragment_shader);
  glLinkProgram(this->shader_program_);

  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);
}

void Graphic::rendering_scope(const std::function<void()>& func) {
  glUseProgram(this->shader_program_);
  glBindVertexArray(this->vao_);
  func();
  glBindVertexArray(0);
}

void Graphic::set_render_attachments(uint32_t color_texture,
  uint32_t depth_texture, uint32_t width, uint32_t height, float clear_r,
  float clear_g, float clear_b, float clear_a, float clear_depth) {
  glBindFramebuffer(GL_FRAMEBUFFER, this->frame_buffer_);
  glFramebufferTexture2D(
    GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture, 0);
  glFramebufferTexture2D(
    GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_texture, 0);

  glViewport(0, 0, width, height);
  glScissor(0, 0, width, height);

  glClearColor(clear_r, clear_g, clear_b, clear_a);
  glClearDepthf(clear_depth);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Graphic::set_viewport(float x, float y, float width, float height,
  float /*min_depth*/, float /*max_depth*/) {
  glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
    static_cast<GLsizei>(width), static_cast<GLsizei>(height));
}

void Graphic::set_scissor(
  int32_t offset_x, int32_t offset_y, uint32_t width, uint32_t height) {
  glScissor(offset_x, offset_y, width, height);
}

void Graphic::set_view_projection(
  const glm::mat4& view, const glm::mat4& projection) {
  glm::mat4 vp          = projection * view;
  GLint     vp_location = glGetUniformLocation(this->shader_program_, "vp");
  glUniformMatrix4fv(vp_location, 1, GL_FALSE, glm::value_ptr(vp));
}

void Graphic::set_buffers(
  const Graphic::Buffer& vertex_buffer, const Graphic::Buffer& index_buffer) {
  glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer.buffer());
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer.buffer());
}

void Graphic::draw_indexed(uint32_t index_count) {
  glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT, 0);
}

}  // namespace yabe_openxr
