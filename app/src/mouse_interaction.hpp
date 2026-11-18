#pragma once

#include <openxr/openxr.h>

#include <atomic>
#include <cstdint>
#include <optional>

namespace yabe_openxr {
/// forwards input of XR_ANDROID_mouse_interaction to the compositor;
/// the runtime reports mouse devices and trackpad devices (e.g. the touchpad
/// on a compute puck) through this interaction profile instead of Android
/// input events
class MouseInteraction {
 public:
  static constexpr const char* kExtensionName = "XR_ANDROID_mouse_interaction";

  MouseInteraction(XrInstance instance, XrSession session,
    const std::atomic_bool& compositor_ready);
  ~MouseInteraction();
  MouseInteraction(const MouseInteraction&)            = delete;
  MouseInteraction& operator=(const MouseInteraction&) = delete;
  MouseInteraction(MouseInteraction&&)                 = delete;
  MouseInteraction& operator=(MouseInteraction&&)      = delete;

  /// must be called every frame while the session is running
  /// @param time       predicted display time of the frame
  /// @param base_space space in which the compositor scene is rendered
  void update(XrTime time, XrSpace base_space);

 private:
  XrAction create_action(
    XrActionType type, const char* name, const char* localized_name);
  void update_button(XrAction action, uint32_t button);
  void update_scroll();
  void update_aim(XrTime time, XrSpace base_space);

  XrInstance              instance_;
  XrSession               session_;
  const std::atomic_bool& compositor_ready_;

  XrActionSet action_set_       = XR_NULL_HANDLE;
  XrAction    aim_action_       = XR_NULL_HANDLE;
  XrAction    select_action_    = XR_NULL_HANDLE;
  XrAction    secondary_action_ = XR_NULL_HANDLE;
  XrAction    tertiary_action_  = XR_NULL_HANDLE;
  XrAction    scroll_action_    = XR_NULL_HANDLE;
  XrSpace     aim_space_        = XR_NULL_HANDLE;

  struct AimAngle {
    float azimuth;  // right is positive
    float polar;    // up is positive
  };
  std::optional<AimAngle> prev_aim_angle_;
};
}  // namespace yabe_openxr
