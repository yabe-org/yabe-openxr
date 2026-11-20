#include "openxr.hpp"

#include <android/asset_manager.h>
#include <android/log.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <yabe-compositor.h>

#include <array>
#include <cstring>
#include <format>
#include <stdexcept>

#include "glm/ext.hpp"
#include "glm/gtc/type_ptr.hpp"
#include "graphic.hpp"
#include "log.hpp"

namespace {
inline void _ensure(
  XrInstance instance, XrResult result, const char* function) {
  if (XR_SUCCEEDED(result)) {
    return;
  }

  if (instance == XR_NULL_HANDLE) {
    throw std::runtime_error(
      std::format("[OpenXR] {} failed before initializing instance: {}",
        function, static_cast<int32_t>(result)));
  }

  std::array<char, XR_MAX_RESULT_STRING_SIZE> buf;
  xrResultToString(instance, result, buf.data());
  throw std::runtime_error(
    std::format("[OpenXR] {} failed: {}", function, buf.data()));
}

#define ENSURE(func) _ensure(this->instance_, func, #func)
}  // namespace

namespace yabe_openxr {
OpenXrApp::OpenXrApp(android_app* app, std::unique_ptr<Graphic>& graphic) {
  this->init_instance(app);
  this->init_session(graphic);
  this->init_formats();
  this->init_views(graphic);
}

OpenXrApp::~OpenXrApp() {
  LOG_INFO("Destroying OpenXrApp");
  if (this->session_ != XR_NULL_HANDLE) {
    xrDestroySession(this->session_);
  }
  if (this->instance_ != XR_NULL_HANDLE) {
    xrDestroyInstance(this->instance_);
  }
}

bool OpenXrApp::pool(std::unique_ptr<Graphic>& graphic) {
  if (!this->is_app_running_) {
    LOG_INFO("OpenXR is_app_running_ became false; exiting");
    return false;
  }

  XrEventDataBuffer event_data{
    .type    = XR_TYPE_EVENT_DATA_BUFFER,
    .next    = nullptr,
    .varying = {0},
  };
  while (xrPollEvent(this->instance_, &event_data) == XR_SUCCESS) {
    switch (event_data.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
        this->handle_session_state_changed(
          *reinterpret_cast<XrEventDataSessionStateChanged*>(&event_data));
        break;
      default:
        break;
    }
    event_data.type = XR_TYPE_EVENT_DATA_BUFFER;
  }

  if (this->is_session_running_) {
    this->render(graphic);
  }

  return true;
}

void OpenXrApp::handle_session_state_changed(
  const XrEventDataSessionStateChanged& event) {
  switch (event.state) {
    case XR_SESSION_STATE_READY: {
      XrSessionBeginInfo sbi{
        .type                         = XR_TYPE_SESSION_BEGIN_INFO,
        .next                         = nullptr,
        .primaryViewConfigurationType = this->view_config_type_,
      };
      ENSURE(xrBeginSession(this->session_, &sbi));
      this->is_session_running_ = true;
      break;
    }
    case XR_SESSION_STATE_STOPPING:
      ENSURE(xrEndSession(this->session_));
      this->is_session_running_ = false;
      break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
      this->is_app_running_     = false;
      this->is_session_running_ = false;
      break;
    default:
      break;
  }
}

void OpenXrApp::render(std::unique_ptr<Graphic>& graphic) {
  XrFrameState    frame_state{XR_TYPE_FRAME_STATE};
  XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
  ENSURE(xrWaitFrame(this->session_, &fwi, &frame_state));
  XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
  ENSURE(xrBeginFrame(this->session_, &fbi));

  RenderLayerInfo layer_info{
    .predicted_display_time = frame_state.predictedDisplayTime,
  };
  if (frame_state.shouldRender) {
    render_layer(graphic, layer_info);
  }

  XrFrameEndInfo fei{
    .type                 = XR_TYPE_FRAME_END_INFO,
    .next                 = nullptr,
    .displayTime          = frame_state.predictedDisplayTime,
    .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
    .layerCount           = static_cast<uint32_t>(layer_info.layers.size()),
    .layers = reinterpret_cast<const XrCompositionLayerBaseHeader* const *>(
      layer_info.layers.data()),
  };
  ENSURE(xrEndFrame(this->session_, &fei));
}
void OpenXrApp::render_layer(
  std::unique_ptr<Graphic>& graphic, RenderLayerInfo& layer_info) {
  XrViewState      view_state{XR_TYPE_VIEW_STATE};
  XrViewLocateInfo vli{
    .type                  = XR_TYPE_VIEW_LOCATE_INFO,
    .next                  = nullptr,
    .viewConfigurationType = this->view_config_type_,
    .displayTime           = layer_info.predicted_display_time,
    .space                 = this->local_space_,
  };
  uint32_t            view_count = 0;
  std::vector<XrView> views(this->view_config_views_.size(), {XR_TYPE_VIEW});
  auto                result = xrLocateViews(this->session_, &vli, &view_state,
    static_cast<uint32_t>(views.size()), &view_count, views.data());
  if (result != XR_SUCCESS) {
    return;
  }
  // runtimes may report no valid orientation (e.g. before tracking starts),
  // and submitting such pose makes xrEndFrame() fail
  if ((view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
    return;
  }
  layer_info.layer_projection_views.resize(view_count);

  for (uint32_t i = 0; i < view_count; i++) {
    auto&    color_swapchain_info = this->color_swapchains_[i];
    auto&    depth_swapchain_info = this->depth_swapchains_[i];
    uint32_t color_image_index    = 0;
    uint32_t depth_image_index    = 0;
    ENSURE(xrAcquireSwapchainImage(
      color_swapchain_info.swapchain, nullptr, &color_image_index));
    ENSURE(xrAcquireSwapchainImage(
      depth_swapchain_info.swapchain, nullptr, &depth_image_index));
    XrSwapchainImageWaitInfo swi{
      .type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
      .timeout = XR_INFINITE_DURATION,
    };
    ENSURE(xrWaitSwapchainImage(color_swapchain_info.swapchain, &swi));
    ENSURE(xrWaitSwapchainImage(depth_swapchain_info.swapchain, &swi));
    const auto width = static_cast<int32_t>(
      this->view_config_views_[i].recommendedImageRectWidth);
    const auto height = static_cast<int32_t>(
      this->view_config_views_[i].recommendedImageRectHeight);
    layer_info.layer_projection_views[i] = {
      .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
      .next = nullptr,
      .pose = views[i].pose,
      .fov  = views[i].fov,
      .subImage =
        {
          .swapchain = color_swapchain_info.swapchain,
          .imageRect =
            {
              .offset = {0, 0},
              .extent = {.width = width, .height = height},
            },
          .imageArrayIndex = 0,
        },
    };

    const glm::quat view_quat{
      views[i].pose.orientation.w,
      views[i].pose.orientation.x,
      views[i].pose.orientation.y,
      views[i].pose.orientation.z,
    };
    const glm::vec3 view_pos{
      views[i].pose.position.x,
      views[i].pose.position.y,
      views[i].pose.position.z,
    };
    const auto view = glm::toMat4(glm::inverse(view_quat)) *
                      glm::translate(glm::mat4(1.F), -view_pos);

    const auto tan_left   = std::tan(views[i].fov.angleLeft);
    const auto tan_right  = std::tan(views[i].fov.angleRight);
    const auto tan_up     = std::tan(views[i].fov.angleUp);
    const auto tan_down   = std::tan(views[i].fov.angleDown);
    const auto tan_width  = tan_right - tan_left;
    const auto tan_height = tan_up - tan_down;
    const auto proj_near  = 0.01F;
    const auto proj_far   = 100.F;
    auto       proj       = glm::mat4(0.F);
    proj[0][0]            = 2.F / tan_width;
    proj[2][0]            = (tan_right + tan_left) / tan_width;
    proj[1][1]            = 2.F / tan_height;
    proj[2][1]            = (tan_up + tan_down) / tan_height;
    proj[2][2]            = -proj_far / (proj_far - proj_near);
    proj[3][2]            = -(proj_far * proj_near) / (proj_far - proj_near);
    proj[2][3]            = -1.F;
    graphic->draw(color_swapchain_info.image_views[color_image_index],
      depth_swapchain_info.image_views[depth_image_index], width, height, proj,
      view);

    XrSwapchainImageReleaseInfo sri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    ENSURE(xrReleaseSwapchainImage(color_swapchain_info.swapchain, &sri));
    ENSURE(xrReleaseSwapchainImage(depth_swapchain_info.swapchain, &sri));
  }

  layer_info.layer_projection = {
    .type      = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
    .next      = nullptr,
    .space     = this->local_space_,
    .viewCount = view_count,
    .views     = layer_info.layer_projection_views.data(),
  };
  layer_info.layers.emplace_back(&layer_info.layer_projection);
}

void OpenXrApp::init_instance(android_app* app) {
  PFN_xrInitializeLoaderKHR xr_loader = nullptr;
  ENSURE(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
    reinterpret_cast<PFN_xrVoidFunction*>(&xr_loader)));
  XrLoaderInitInfoAndroidKHR liia{
    .type               = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR,
    .next               = nullptr,
    .applicationVM      = app->activity->vm,
    .applicationContext = app->activity->clazz,
  };
  ENSURE(xr_loader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&liia)));

  XrApplicationInfo ai{
    .applicationName    = "",
    .applicationVersion = 1,
    .engineName         = "YabeOpenXR",
    .engineVersion      = 1,
    .apiVersion         = XR_API_VERSION_1_1,
  };
  std::strncpy(ai.applicationName, "YabeOpenXR", XR_MAX_APPLICATION_NAME_SIZE);

  const std::vector<const char*> required_extensions = {
    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
  };
  const std::vector<const char*> optional_extensions = {
    XR_EXT_DEBUG_UTILS_EXTENSION_NAME,
  };

  uint32_t extension_count = 0;
  ENSURE(xrEnumerateInstanceExtensionProperties(
    nullptr, 0, &extension_count, nullptr));
  std::vector<XrExtensionProperties> available_extensions(
    extension_count, {XR_TYPE_EXTENSION_PROPERTIES});
  ENSURE(xrEnumerateInstanceExtensionProperties(
    nullptr, extension_count, &extension_count, available_extensions.data()));
  const auto find_extension = [&available_extensions](const char* name) {
    return std::find_if(available_extensions.begin(),
      available_extensions.end(), [name](const XrExtensionProperties& e) {
        return strcmp(e.extensionName, name) == 0;
      });
  };

  std::vector<const char*> extensions;
  for (const auto& required_extension : required_extensions) {
    auto it = find_extension(required_extension);
    if (it == available_extensions.end()) {
      throw std::runtime_error(std::format(
        "OpenXR instance extension '{}' is not supported", required_extension));
    }
    extensions.push_back(it->extensionName);
  }
  for (const auto& optional_extension : optional_extensions) {
    auto it = find_extension(optional_extension);
    if (it == available_extensions.end()) {
      LOG_INFO("Optional OpenXR instance extension '{}' is not supported",
        optional_extension);
      continue;
    }
    extensions.push_back(it->extensionName);
  }

  XrInstanceCreateInfoAndroidKHR icia{
    .type                = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR,
    .next                = nullptr,
    .applicationVM       = app->activity->vm,
    .applicationActivity = app->activity->clazz,
  };
  XrInstanceCreateInfo ici{
    .type                  = XR_TYPE_INSTANCE_CREATE_INFO,
    .next                  = &icia,
    .createFlags           = 0,
    .applicationInfo       = ai,
    .enabledApiLayerCount  = 0,
    .enabledApiLayerNames  = nullptr,
    .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
    .enabledExtensionNames = extensions.data(),
  };
  auto result = xrCreateInstance(&ici, &this->instance_);
  if (result == XR_ERROR_API_VERSION_UNSUPPORTED) {
    // this app uses no OpenXR 1.1 feature, so 1.0 runtimes are also usable
    LOG_INFO("OpenXR 1.1 is not supported by the runtime; retrying with 1.0");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    result                         = xrCreateInstance(&ici, &this->instance_);
  }
  ENSURE(result);

  XrInstanceProperties instance_properties{XR_TYPE_INSTANCE_PROPERTIES};
  ENSURE(xrGetInstanceProperties(this->instance_, &instance_properties));
  LOG_INFO("OpenXR runtime: {} {}.{}.{}", instance_properties.runtimeName,
    XR_VERSION_MAJOR(instance_properties.runtimeVersion),
    XR_VERSION_MINOR(instance_properties.runtimeVersion),
    XR_VERSION_PATCH(instance_properties.runtimeVersion));

  XrSystemGetInfo sgi{
    .type       = XR_TYPE_SYSTEM_GET_INFO,
    .next       = nullptr,
    .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
  };
  ENSURE(xrGetSystem(this->instance_, &sgi, &this->system_id_));

  LOG_DEBUG("OpenXR instance was created; system ID: {}", this->system_id_);
}

void OpenXrApp::init_session(std::unique_ptr<Graphic>& graphic) {
  XrGraphicsRequirementsOpenGLESKHR reqs{
    .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR,
    .next = nullptr,
  };
  PFN_xrGetOpenGLESGraphicsRequirementsKHR
    pfnGetOpenGLESGraphicsRequirementsKHR = nullptr;
  ENSURE(xrGetInstanceProcAddr(this->instance_,
    "xrGetOpenGLESGraphicsRequirementsKHR",
    reinterpret_cast<PFN_xrVoidFunction*>(
      &pfnGetOpenGLESGraphicsRequirementsKHR)));
  ENSURE(pfnGetOpenGLESGraphicsRequirementsKHR(
    this->instance_, this->system_id_, &reqs));

  const auto          graphics_binding = graphic->get_graphics_binding();
  XrSessionCreateInfo sci{
    .type        = XR_TYPE_SESSION_CREATE_INFO,
    .next        = &graphics_binding,
    .createFlags = 0,
    .systemId    = this->system_id_,
  };
  ENSURE(xrCreateSession(this->instance_, &sci, &this->session_));
}

void OpenXrApp::init_formats() {
  uint32_t swapchain_fmt_count = 0;
  ENSURE(xrEnumerateSwapchainFormats(
    this->session_, 0, &swapchain_fmt_count, nullptr));
  std::vector<int64_t> swapchain_fmts(swapchain_fmt_count);
  ENSURE(xrEnumerateSwapchainFormats(this->session_, swapchain_fmt_count,
    &swapchain_fmt_count, swapchain_fmts.data()));
  // select color format
  const std::array<uint64_t, 2> expected_color_formats = {
    GL_SRGB8_ALPHA8,
    GL_RGBA8,
  };
  auto it = std::find_first_of(swapchain_fmts.begin(), swapchain_fmts.end(),
    expected_color_formats.begin(), expected_color_formats.end());
  if (it == swapchain_fmts.end()) {
    throw std::runtime_error("expected color formats are not supported");
  }
  this->color_format_                                  = *it;
  // select depth format
  const std::array<uint64_t, 5> expected_depth_formats = {
    GL_DEPTH_COMPONENT24,
    GL_DEPTH24_STENCIL8,
    GL_DEPTH_COMPONENT32F,
    GL_DEPTH32F_STENCIL8,
    GL_DEPTH_COMPONENT16,
  };
  it = std::find_first_of(swapchain_fmts.begin(), swapchain_fmts.end(),
    expected_depth_formats.begin(), expected_depth_formats.end());
  if (it == swapchain_fmts.end()) {
    throw std::runtime_error("expected depth formats are not supported");
  }
  this->depth_format_ = *it;
}

void OpenXrApp::init_views(std::unique_ptr<Graphic>& graphic) {
  const std::vector<XrViewConfigurationType> expected_view_config_types = {
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO,
  };
  uint32_t view_config_type_count = 0;
  ENSURE(xrEnumerateViewConfigurations(
    this->instance_, this->system_id_, 0, &view_config_type_count, nullptr));
  std::vector<XrViewConfigurationType> view_config_types(
    view_config_type_count);
  ENSURE(xrEnumerateViewConfigurations(this->instance_, this->system_id_,
    view_config_type_count, &view_config_type_count, view_config_types.data()));
  for (const auto& expected_type : expected_view_config_types) {
    if (std::find(view_config_types.begin(), view_config_types.end(),
          expected_type) != view_config_types.end()) {
      this->view_config_type_ = expected_type;
      break;
    }
  }

  uint32_t config_views_count = 0;
  ENSURE(xrEnumerateViewConfigurationViews(this->instance_, this->system_id_,
    this->view_config_type_, 0, &config_views_count, nullptr));
  this->view_config_views_.resize(
    config_views_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  ENSURE(xrEnumerateViewConfigurationViews(this->instance_, this->system_id_,
    this->view_config_type_, config_views_count, &config_views_count,
    this->view_config_views_.data()));

  this->color_swapchains_.resize(config_views_count);
  this->depth_swapchains_.resize(config_views_count);
  for (uint32_t i = 0; i < config_views_count; i++) {
    auto&                 config_view = this->view_config_views_[i];
    // create XrSwapchain
    XrSwapchainCreateInfo sci{
      .type        = XR_TYPE_SWAPCHAIN_CREATE_INFO,
      .next        = nullptr,
      .createFlags = 0,
      .usageFlags  = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
      .format      = this->color_format_,
      // Graphic attaches swapchain images as GL_TEXTURE_2D
      .sampleCount = 1,
      .width       = config_view.recommendedImageRectWidth,
      .height      = config_view.recommendedImageRectHeight,
      .faceCount   = 1,
      .arraySize   = 1,
      .mipCount    = 1,
    };
    auto& color_swapchain_info = this->color_swapchains_[i];
    ENSURE(
      xrCreateSwapchain(this->session_, &sci, &color_swapchain_info.swapchain));
    color_swapchain_info.swapchain_format = sci.format;

    sci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    sci.format     = this->depth_format_;
    auto& depth_swapchain_info = this->depth_swapchains_[i];
    ENSURE(
      xrCreateSwapchain(this->session_, &sci, &depth_swapchain_info.swapchain));
    depth_swapchain_info.swapchain_format = sci.format;

    // create XrSwapchainImage
    uint32_t color_swapchain_image_count = 0;
    ENSURE(xrEnumerateSwapchainImages(color_swapchain_info.swapchain, 0,
      &color_swapchain_image_count, nullptr));
    auto* color_swapchain_images =
      graphic->acquire_swapchain_image(color_swapchain_info.swapchain,
        Graphic::SwapchainType::COLOR, color_swapchain_image_count);
    ENSURE(xrEnumerateSwapchainImages(color_swapchain_info.swapchain,
      color_swapchain_image_count, &color_swapchain_image_count,
      color_swapchain_images));

    uint32_t depth_swapchain_image_count = 0;
    ENSURE(xrEnumerateSwapchainImages(depth_swapchain_info.swapchain, 0,
      &depth_swapchain_image_count, nullptr));
    auto* depth_swapchain_images =
      graphic->acquire_swapchain_image(depth_swapchain_info.swapchain,
        Graphic::SwapchainType::DEPTH, depth_swapchain_image_count);
    ENSURE(xrEnumerateSwapchainImages(depth_swapchain_info.swapchain,
      depth_swapchain_image_count, &depth_swapchain_image_count,
      depth_swapchain_images));

    // create Swapchain Image Views
    for (uint32_t j = 0; j < color_swapchain_image_count; j++) {
      color_swapchain_info.image_views.emplace_back(
        graphic->get_swapchain_image(
          color_swapchain_info.swapchain, j, Graphic::SwapchainType::COLOR));
    }
    for (uint32_t j = 0; j < depth_swapchain_image_count; j++) {
      depth_swapchain_info.image_views.emplace_back(
        graphic->get_swapchain_image(
          depth_swapchain_info.swapchain, j, Graphic::SwapchainType::DEPTH));
    }
  }

  // Space
  XrReferenceSpaceCreateInfo rsci{
    .type               = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
    .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
    .poseInReferenceSpace =
      {
        .orientation = {0.0F, 0.0F, 0.0F, 1.0F},
        .position    = {0.0F, 0.0F, 0.0F},
      },
  };
  ENSURE(xrCreateReferenceSpace(this->session_, &rsci, &this->local_space_));
}
}  // namespace yabe_openxr
