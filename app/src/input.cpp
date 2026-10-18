#include "input.hpp"

#include <android/input.h>
#include <linux/input-event-codes.h>

#include "yabe-compositor.h"

namespace yabe_openxr {
namespace {
const std::atomic_bool* compositor_ready_ = nullptr;  // NOLINT

int32_t send_mouse_button_event(int32_t button, bool is_pressed) {
  static int32_t prev_button = 0;
  const auto     diff        = button ^ prev_button;
  prev_button                = button;
  if (diff == AMOTION_EVENT_BUTTON_PRIMARY) {
    yabe_compositor_button(BTN_LEFT, is_pressed);
    return 1;
  }
  if (diff == AMOTION_EVENT_BUTTON_SECONDARY) {
    yabe_compositor_button(BTN_RIGHT, is_pressed);
    return 1;
  }
  if (diff == AMOTION_EVENT_BUTTON_TERTIARY) {
    yabe_compositor_button(BTN_MIDDLE, is_pressed);
    return 1;
  }
  return 0;
}
int32_t handle_mouse_input(AInputEvent* event) {
  const auto action = AMotionEvent_getAction(event);
  switch (action) {
    case AMOTION_EVENT_ACTION_BUTTON_PRESS:
      return send_mouse_button_event(AMotionEvent_getButtonState(event), true);
    case AMOTION_EVENT_ACTION_BUTTON_RELEASE:
      return send_mouse_button_event(AMotionEvent_getButtonState(event), false);
    case AMOTION_EVENT_ACTION_SCROLL:
      yabe_compositor_axis(
        0, AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0));
      return 1;
    case AMOTION_EVENT_ACTION_MOVE:
    case AMOTION_EVENT_ACTION_HOVER_MOVE: {
      const auto dx =
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RELATIVE_X, 0);
      const auto dy =
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RELATIVE_Y, 0);
      constexpr float kScale = 0.002F;
      yabe_compositor_move_pointer(dx * kScale, -dy * kScale);
      return 1;
    }
    default:
      return 0;
  }

  return 0;
}

int32_t handle_keyboard_input(AInputEvent* event) {
  const auto action = AKeyEvent_getAction(event);
  if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) {
    return 0;
  }
  const uint32_t key = AKeyEvent_getScanCode(event);
  if (key == 0) {
    return 0;
  }
  const bool is_pressed = (action == AKEY_EVENT_ACTION_DOWN);
  if (is_pressed && AKeyEvent_getRepeatCount(event) > 0) {
    return 1;
  }
  yabe_compositor_key(key, is_pressed);
  return 1;
}

int32_t handle_input_event(android_app* /*app*/, AInputEvent* event) {
  if (!compositor_ready_ || !compositor_ready_->load()) {
    return 0;
  }
  if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
    handle_mouse_input(event);
  } else if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
    handle_keyboard_input(event);
  }
  return 0;
}
}  // namespace

void init_input(android_app* app, const std::atomic_bool& compositor_ready) {
  compositor_ready_ = &compositor_ready;
  app->onInputEvent = handle_input_event;
}
}  // namespace yabe_openxr
