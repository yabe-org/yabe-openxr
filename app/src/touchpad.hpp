#pragma once
#include <android/input.h>
#include <android_native_app_glue.h>

namespace yabe_openxr {
/// accumulate a touchpad event; a touchpad reports raw touches only while the
/// pointer is captured
void handle_touchpad_input(AInputEvent* event);
/// interpret accumulated touchpad events as gestures (tap, long tap, drag and
/// two-finger scroll) and forward them to the compositor;
/// must be called every frame from the thread processing input events
void update_touchpad(android_app* app, float delta_sec);
}  // namespace yabe_openxr
