#include "mouse_interaction.hpp"

#include <linux/input-event-codes.h>
#include <yabe-compositor.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <stdexcept>

#include "glm/ext.hpp"
#include "log.hpp"

namespace {
inline void _ensure(
  XrInstance instance, XrResult result, const char* function) {
  if (XR_SUCCEEDED(result)) {
    return;
  }
  std::array<char, XR_MAX_RESULT_STRING_SIZE> buf;
  xrResultToString(instance, result, buf.data());
  throw std::runtime_error(
    std::format("[OpenXR] {} failed: {}", function, buf.data()));
}

#define ENSURE(func) _ensure(this->instance_, func, #func)

constexpr const char* kInteractionProfile =
  "/interaction_profiles/android/mouse_interaction_android";
}  // namespace

namespace yabe_openxr {
MouseInteraction::MouseInteraction(XrInstance instance, XrSession session,
  const std::atomic_bool& compositor_ready)
    : instance_(instance)
    , session_(session)
    , compositor_ready_(compositor_ready) {
  XrActionSetCreateInfo asci{
    .type                   = XR_TYPE_ACTION_SET_CREATE_INFO,
    .next                   = nullptr,
    .actionSetName          = "",
    .localizedActionSetName = "",
    .priority               = 0,
  };
  std::strncpy(asci.actionSetName, "mouse", XR_MAX_ACTION_SET_NAME_SIZE);
  std::strncpy(asci.localizedActionSetName, "Mouse",
    XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
  ENSURE(xrCreateActionSet(this->instance_, &asci, &this->action_set_));

  this->aim_action_ =
    this->create_action(XR_ACTION_TYPE_POSE_INPUT, "aim", "Aim");
  this->select_action_ =
    this->create_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "select", "Select");
  this->secondary_action_ =
    this->create_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "Secondary");
  this->tertiary_action_ =
    this->create_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "tertiary", "Tertiary");
  this->scroll_action_ =
    this->create_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "scroll", "Scroll");

  const auto to_path = [this](const char* str) {
    XrPath path = XR_NULL_PATH;
    ENSURE(xrStringToPath(this->instance_, str, &path));
    return path;
  };
  const std::array<XrActionSuggestedBinding, 5> bindings = {{
    {this->aim_action_, to_path("/user/mouse/input/aim/pose")},
    {this->select_action_, to_path("/user/mouse/input/select/click")},
    {this->secondary_action_,
      to_path("/user/mouse/input/secondary_android/click")},
    {this->tertiary_action_,
      to_path("/user/mouse/input/tertiary_android/click")},
    {this->scroll_action_, to_path("/user/mouse/input/scroll_android/value")},
  }};
  XrInteractionProfileSuggestedBinding          ipsb{
    .type                   = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
    .next                   = nullptr,
    .interactionProfile     = to_path(kInteractionProfile),
    .countSuggestedBindings = static_cast<uint32_t>(bindings.size()),
    .suggestedBindings      = bindings.data(),
  };
  ENSURE(xrSuggestInteractionProfileBindings(this->instance_, &ipsb));

  XrSessionActionSetsAttachInfo sasai{
    .type            = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
    .next            = nullptr,
    .countActionSets = 1,
    .actionSets      = &this->action_set_,
  };
  ENSURE(xrAttachSessionActionSets(this->session_, &sasai));

  XrActionSpaceCreateInfo asci_space{
    .type          = XR_TYPE_ACTION_SPACE_CREATE_INFO,
    .next          = nullptr,
    .action        = this->aim_action_,
    .subactionPath = XR_NULL_PATH,
    .poseInActionSpace =
      {
        .orientation = {0.F, 0.F, 0.F, 1.F},
        .position    = {0.F, 0.F, 0.F},
      },
  };
  ENSURE(xrCreateActionSpace(this->session_, &asci_space, &this->aim_space_));
  LOG_INFO("{} is enabled", kExtensionName);
}

MouseInteraction::~MouseInteraction() {
  if (this->aim_space_ != XR_NULL_HANDLE) {
    xrDestroySpace(this->aim_space_);
  }
  // actions are destroyed with the action set
  if (this->action_set_ != XR_NULL_HANDLE) {
    xrDestroyActionSet(this->action_set_);
  }
}

XrAction MouseInteraction::create_action(
  XrActionType type, const char* name, const char* localized_name) {
  XrActionCreateInfo aci{
    .type                = XR_TYPE_ACTION_CREATE_INFO,
    .next                = nullptr,
    .actionName          = "",
    .actionType          = type,
    .countSubactionPaths = 0,
    .subactionPaths      = nullptr,
    .localizedActionName = "",
  };
  std::strncpy(aci.actionName, name, XR_MAX_ACTION_NAME_SIZE);
  std::strncpy(
    aci.localizedActionName, localized_name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
  XrAction action = XR_NULL_HANDLE;
  ENSURE(xrCreateAction(this->action_set_, &aci, &action));
  return action;
}

void MouseInteraction::update(XrTime time, XrSpace base_space) {
  const XrActiveActionSet active_action_set{
    .actionSet     = this->action_set_,
    .subactionPath = XR_NULL_PATH,
  };
  XrActionsSyncInfo asi{
    .type                  = XR_TYPE_ACTIONS_SYNC_INFO,
    .next                  = nullptr,
    .countActiveActionSets = 1,
    .activeActionSets      = &active_action_set,
  };
  // returns XR_SESSION_NOT_FOCUSED (success code) while unfocused,
  // and then all actions become inactive
  ENSURE(xrSyncActions(this->session_, &asi));

  this->update_aim(time, base_space);
  this->update_button(this->select_action_, BTN_LEFT);
  this->update_button(this->secondary_action_, BTN_RIGHT);
  this->update_button(this->tertiary_action_, BTN_MIDDLE);
  this->update_scroll();
}

void MouseInteraction::update_button(XrAction action, uint32_t button) {
  XrActionStateGetInfo asgi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action};
  XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
  ENSURE(xrGetActionStateBoolean(this->session_, &asgi, &state));
  if (!state.isActive || !state.changedSinceLastSync ||
      !this->compositor_ready_.load()) {
    return;
  }
  yabe_compositor_button(button, state.currentState == XR_TRUE);
}

void MouseInteraction::update_scroll() {
  XrActionStateGetInfo asgi{
    XR_TYPE_ACTION_STATE_GET_INFO, nullptr, this->scroll_action_};
  XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
  ENSURE(xrGetActionStateVector2f(this->session_, &asgi, &state));
  if (!state.isActive || !this->compositor_ready_.load()) {
    return;
  }
  // the value is in [-1, 1] and up/right is positive
  if (state.currentState.y != 0.F) {
    yabe_compositor_axis(0, state.currentState.y);
  }
  if (state.currentState.x != 0.F) {
    yabe_compositor_axis(1, state.currentState.x);
  }
}

void MouseInteraction::update_aim(XrTime time, XrSpace base_space) {
  XrActionStateGetInfo asgi{
    XR_TYPE_ACTION_STATE_GET_INFO, nullptr, this->aim_action_};
  XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
  ENSURE(xrGetActionStatePose(this->session_, &asgi, &state));
  if (!state.isActive) {
    this->prev_aim_angle_.reset();
    return;
  }
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  ENSURE(xrLocateSpace(this->aim_space_, base_space, time, &location));
  if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
    this->prev_aim_angle_.reset();
    return;
  }

  // the runtime moves the aim ray along a sphere around the user's head by
  // relative movement of the device, so forward the change of its direction
  const glm::quat orientation{
    location.pose.orientation.w,
    location.pose.orientation.x,
    location.pose.orientation.y,
    location.pose.orientation.z,
  };
  const auto     forward = orientation * glm::vec3(0.F, 0.F, -1.F);
  const AimAngle angle{
    .azimuth = std::atan2(forward.x, -forward.z),
    .polar   = std::asin(std::clamp(forward.y, -1.F, 1.F)),
  };
  if (this->prev_aim_angle_ && this->compositor_ready_.load()) {
    auto d_azimuth = angle.azimuth - this->prev_aim_angle_->azimuth;
    // the azimuth wraps around at +-pi
    if (d_azimuth > std::numbers::pi_v<float>) {
      d_azimuth -= 2.F * std::numbers::pi_v<float>;
    } else if (d_azimuth < -std::numbers::pi_v<float>) {
      d_azimuth += 2.F * std::numbers::pi_v<float>;
    }
    const auto d_polar = angle.polar - this->prev_aim_angle_->polar;
    if (d_azimuth != 0.F || d_polar != 0.F) {
      yabe_compositor_move_pointer(d_azimuth / YABE_COMPOSITOR_RADIAN_PER_PIXEL,
        d_polar / YABE_COMPOSITOR_RADIAN_PER_PIXEL);
    }
  }
  this->prev_aim_angle_ = angle;
}
}  // namespace yabe_openxr
