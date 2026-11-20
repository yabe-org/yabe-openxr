#pragma once
#include <android_native_app_glue.h>

/// must be called from a thread attached to the JavaVM
void perform_haptic_feedback(android_app* app);
