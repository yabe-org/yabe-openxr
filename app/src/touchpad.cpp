#include "touchpad.hpp"

#include <jni.h>
#include <linux/input-event-codes.h>

#include <chrono>
#include <vector>

#include "haptic.hpp"
#include "log.hpp"
#include "yabe-compositor.h"

namespace yabe_openxr {
namespace {
// Touchpad events are accumulated by handle_touchpad_input() and interpreted
// as gestures once per frame by update_touchpad(), because gestures
// such as long tap depend on elapsed time while no event arrives
struct TouchFrame {
  int32_t pointer_count = 0;
  // the first touching pointer; its movement is used for gestures
  int32_t primary_id    = -1;
  float   last_x        = 0.F;
  float   last_y        = 0.F;
  // position of the primary pointer when the touch began
  float   start_x       = 0.F;
  float   start_y       = 0.F;
  // accumulated since the last update_touchpad()
  float   delta_x       = 0.F;
  float   delta_y       = 0.F;
  bool    began         = false;
  bool    ended         = false;
};
TouchFrame touch_frame;  // NOLINT

void set_primary_pointer(AInputEvent* event, size_t index) {
  touch_frame.primary_id = AMotionEvent_getPointerId(event, index);
  touch_frame.last_x     = AMotionEvent_getX(event, index);
  touch_frame.last_y     = AMotionEvent_getY(event, index);
}

// A finger moves slightly even while tapping, and how much it moves in
// touchpad units depends on the touchpad's resolution, so a touch is regarded
// as a tap while it stays within kTapSlopMm from where it began.
// The same value as "Tap Move Distance" of Android's touchpad gesture library
constexpr float kTapSlopMm                 = 2.F;
// used if the touchpad does not report its resolution
constexpr float kFallbackTapSlopRangeRatio = 0.03F;

struct TapSlop {
  int32_t device_id = -1;
  // in touchpad units
  float   x         = 0.F;
  float   y         = 0.F;
};
TapSlop tap_slop;  // NOLINT

/// returns the tap slop along the axis in touchpad units, or 0 on failure
float query_tap_slop(JNIEnv* env, jobject device, jint axis) {
  jclass    device_class            = env->GetObjectClass(device);
  jmethodID get_motion_range_method = env->GetMethodID(device_class,
    "getMotionRange", "(II)Landroid/view/InputDevice$MotionRange;");
  jobject   range                   = env->CallObjectMethod(
    device, get_motion_range_method, axis, AINPUT_SOURCE_TOUCHPAD);
  if (env->ExceptionCheck() || !range) {
    env->ExceptionClear();
    return 0.F;
  }
  jclass    range_class = env->GetObjectClass(range);
  jmethodID get_resolution_method =
    env->GetMethodID(range_class, "getResolution", "()F");
  jmethodID get_range_method = env->GetMethodID(range_class, "getRange", "()F");
  // units per mm
  const auto resolution = env->CallFloatMethod(range, get_resolution_method);
  const auto size       = env->CallFloatMethod(range, get_range_method);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return 0.F;
  }
  if (resolution > 0.F) {
    return resolution * kTapSlopMm;
  }
  LOG_WARN("touchpad: resolution of axis {} is unknown", axis);
  return size * kFallbackTapSlopRangeRatio;
}

void update_tap_slop(android_app* app, int32_t device_id) {
  if (tap_slop.device_id == device_id) {
    return;
  }
  tap_slop = {.device_id = device_id};

  JNIEnv* env = nullptr;
  if (app->activity->vm->GetEnv(
        reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK ||
      !env) {
    LOG_ERROR("update_tap_slop: current thread is not attached");
    return;
  }
  // local references are released by PopLocalFrame()
  if (env->PushLocalFrame(8) != JNI_OK) {
    LOG_ERROR("update_tap_slop: PushLocalFrame failed");
    env->ExceptionClear();
    return;
  }
  jclass    device_class      = env->FindClass("android/view/InputDevice");
  jmethodID get_device_method = env->GetStaticMethodID(
    device_class, "getDevice", "(I)Landroid/view/InputDevice;");
  jobject device =
    env->CallStaticObjectMethod(device_class, get_device_method, device_id);
  if (env->ExceptionCheck() || !device) {
    env->ExceptionClear();
  } else {
    tap_slop.x = query_tap_slop(env, device, AMOTION_EVENT_AXIS_X);
    tap_slop.y = query_tap_slop(env, device, AMOTION_EVENT_AXIS_Y);
  }
  env->PopLocalFrame(nullptr);
  LOG_INFO(
    "touchpad {}: tap slop is ({}, {})", device_id, tap_slop.x, tap_slop.y);
}

bool exceeds_tap_slop() {
  const auto dx = touch_frame.last_x - touch_frame.start_x;
  const auto dy = touch_frame.last_y - touch_frame.start_y;
  if (tap_slop.x <= 0.F || tap_slop.y <= 0.F) {
    // the slop is unavailable; regard any movement as exceeding it
    return dx != 0.F || dy != 0.F;
  }
  const auto rx = dx / tap_slop.x;
  const auto ry = dy / tap_slop.y;
  return (rx * rx) + (ry * ry) > 1.F;
}
}  // namespace

void handle_touchpad_input(android_app* app, AInputEvent* event) {
  const auto action = AMotionEvent_getAction(event);
  const auto index =
    static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
  const auto pointer_count =
    static_cast<int32_t>(AMotionEvent_getPointerCount(event));
  switch (action & AMOTION_EVENT_ACTION_MASK) {
    case AMOTION_EVENT_ACTION_DOWN:
      update_tap_slop(app, AInputEvent_getDeviceId(event));
      set_primary_pointer(event, 0);
      touch_frame.start_x       = touch_frame.last_x;
      touch_frame.start_y       = touch_frame.last_y;
      touch_frame.pointer_count = pointer_count;
      touch_frame.began         = true;
      return;
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
      touch_frame.pointer_count = pointer_count;
      return;
    case AMOTION_EVENT_ACTION_MOVE:
      touch_frame.pointer_count = pointer_count;
      for (size_t i = 0; i < static_cast<size_t>(pointer_count); i++) {
        if (AMotionEvent_getPointerId(event, i) != touch_frame.primary_id) {
          continue;
        }
        const auto x = AMotionEvent_getX(event, i);
        const auto y = AMotionEvent_getY(event, i);
        touch_frame.delta_x += x - touch_frame.last_x;
        touch_frame.delta_y += y - touch_frame.last_y;
        touch_frame.last_x = x;
        touch_frame.last_y = y;
      }
      return;
    case AMOTION_EVENT_ACTION_POINTER_UP:
      touch_frame.pointer_count = pointer_count - 1;
      if (AMotionEvent_getPointerId(event, index) == touch_frame.primary_id) {
        // continue the gesture with one of the remaining pointers
        set_primary_pointer(event, index == 0 ? 1 : 0);
      }
      return;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
      touch_frame.pointer_count = 0;
      touch_frame.primary_id    = -1;
      touch_frame.ended         = true;
      return;
    default:
      return;
  }
}

namespace {
enum class TouchPhase : uint8_t {
  BEGAN,
  MOVED,
  STATIONARY,
  ENDED,
};
enum class TapState : uint8_t {
  IDLE,
  PENDING,  // until kTapThresholdSec passed from BEGAN
  LONG_TAP,
  MOVE_ONLY,  // button down is not sent at the beginning
  DRAG,
  SCROLL,
};
TapState tap_state             = TapState::IDLE;  // NOLINT
float    passed_sec_from_began = 0.F;             // NOLINT

struct PendingButtonRelease {
  uint32_t                              button;
  std::chrono::steady_clock::time_point time;
};
std::vector<PendingButtonRelease> pending_button_releases;  // NOLINT

void click(uint32_t button, float release_delay_sec) {
  yabe_compositor_button(button, true);
  pending_button_releases.push_back({
    .button = button,
    .time   = std::chrono::steady_clock::now() +
              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(release_delay_sec)),
  });
}

void release_pending_buttons() {
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(pending_button_releases, [now](const auto& release) {
    if (release.time > now) {
      return false;
    }
    yabe_compositor_button(release.button, false);
    return true;
  });
}

TouchPhase take_touch_phase() {
  TouchPhase phase = TouchPhase::ENDED;
  if (touch_frame.began) {
    // ended is kept for the next frame if the touch is released immediately
    phase = TouchPhase::BEGAN;
  } else if (touch_frame.ended) {
    phase = TouchPhase::ENDED;
  } else if (touch_frame.pointer_count > 0) {
    const auto sq_magnitude = (touch_frame.delta_x * touch_frame.delta_x) +
                              (touch_frame.delta_y * touch_frame.delta_y);
    // discard small movement
    phase = sq_magnitude > 2.F ? TouchPhase::MOVED : TouchPhase::STATIONARY;
  }
  if (phase == TouchPhase::BEGAN) {
    touch_frame.began = false;
  } else {
    touch_frame.ended = false;
  }
  return phase;
}

void update_gesture(android_app* app, float delta_sec) {
  constexpr float kTapMovementDeltaMultiplier = 0.4F;
  constexpr float kTapScrollDeltaMultiplier   = 0.006F;
  constexpr float kTapThresholdSec            = 0.3F;

  const auto phase         = take_touch_phase();
  const auto pointer_count = touch_frame.pointer_count;
  // Android's Y axis points downward
  const auto dx            = touch_frame.delta_x;
  const auto dy            = -touch_frame.delta_y;
  touch_frame.delta_x      = 0.F;
  touch_frame.delta_y      = 0.F;

  switch (tap_state) {
    case TapState::IDLE:
      if (phase == TouchPhase::BEGAN) {
        tap_state             = TapState::PENDING;
        passed_sec_from_began = 0.F;
      }
      if (phase == TouchPhase::MOVED) {
        tap_state = TapState::MOVE_ONLY;
      }
      break;

    case TapState::PENDING:
      if (phase == TouchPhase::ENDED) {
        // tap duration is less than kTapThresholdSec
        if (!exceeds_tap_slop()) {
          click(BTN_LEFT, passed_sec_from_began);
        }
        tap_state = TapState::IDLE;
      } else if (pointer_count >= 2) {
        tap_state = TapState::SCROLL;
      } else if (exceeds_tap_slop()) {
        tap_state = TapState::MOVE_ONLY;
      } else if (phase != TouchPhase::BEGAN) {
        passed_sec_from_began += delta_sec;
        if (passed_sec_from_began > kTapThresholdSec) {
          tap_state = TapState::LONG_TAP;
          perform_haptic_feedback(app);
        }
      }
      break;

    case TapState::LONG_TAP:
      if (phase == TouchPhase::ENDED) {
        click(BTN_RIGHT, 0.2F);
        tap_state = TapState::IDLE;
      } else if (pointer_count >= 2) {
        tap_state = TapState::SCROLL;
      } else if (exceeds_tap_slop()) {
        yabe_compositor_button(BTN_LEFT, true);
        tap_state = TapState::DRAG;
      }
      break;

    case TapState::MOVE_ONLY:
      if (phase == TouchPhase::MOVED) {
        yabe_compositor_move_pointer(
          dx * kTapMovementDeltaMultiplier, dy * kTapMovementDeltaMultiplier);
      } else if (phase == TouchPhase::ENDED) {
        tap_state = TapState::IDLE;
      } else if (pointer_count >= 2) {
        tap_state = TapState::SCROLL;
      }
      break;

    case TapState::DRAG:
      if (phase == TouchPhase::MOVED) {
        yabe_compositor_move_pointer(
          dx * kTapMovementDeltaMultiplier, dy * kTapMovementDeltaMultiplier);
      }
      if (phase == TouchPhase::ENDED) {
        yabe_compositor_button(BTN_LEFT, false);
        tap_state = TapState::IDLE;
      }
      break;

    case TapState::SCROLL:
      if (phase == TouchPhase::MOVED) {
        yabe_compositor_axis(0, ((dx * dx) + (dy * dy)) *
                                  kTapScrollDeltaMultiplier *
                                  (dy < 0 ? -1.F : 1.F));
      } else if (pointer_count < 2) {
        tap_state = TapState::IDLE;
      }
      break;
  }
}
}  // namespace

void update_touchpad(android_app* app, float delta_sec) {
  update_gesture(app, delta_sec);
  release_pending_buttons();
}
}  // namespace yabe_openxr
