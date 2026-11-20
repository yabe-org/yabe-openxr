#include "touchpad.hpp"

#include <linux/input-event-codes.h>

#include <chrono>
#include <vector>

#include "haptic.hpp"
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

}  // namespace

void handle_touchpad_input(AInputEvent* event) {
  const auto action = AMotionEvent_getAction(event);
  const auto index =
    static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
  const auto pointer_count =
    static_cast<int32_t>(AMotionEvent_getPointerCount(event));
  switch (action & AMOTION_EVENT_ACTION_MASK) {
    case AMOTION_EVENT_ACTION_DOWN:
      set_primary_pointer(event, 0);
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
  constexpr float kTapThresholdSec            = 0.2F;

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
      if (pointer_count >= 2) {
        tap_state = TapState::SCROLL;
        break;
      }
      switch (phase) {
        case TouchPhase::MOVED:
          tap_state = TapState::MOVE_ONLY;
          break;
        case TouchPhase::STATIONARY:
          passed_sec_from_began += delta_sec;
          if (passed_sec_from_began > kTapThresholdSec) {
            tap_state = TapState::LONG_TAP;
            perform_haptic_feedback(app);
          }
          break;
        case TouchPhase::ENDED:
          // tap duration is less than kTapThresholdSec
          click(BTN_LEFT, passed_sec_from_began);
          tap_state = TapState::IDLE;
          break;
        default:
          break;
      }
      break;

    case TapState::LONG_TAP:
      if (phase == TouchPhase::MOVED) {
        yabe_compositor_button(BTN_LEFT, true);
        tap_state = TapState::DRAG;
      } else if (phase == TouchPhase::ENDED) {
        click(BTN_RIGHT, 0.2F);
        tap_state = TapState::IDLE;
      } else if (pointer_count >= 2) {
        tap_state = TapState::SCROLL;
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
