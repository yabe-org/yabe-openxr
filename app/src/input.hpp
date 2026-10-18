#pragma once
#include <android_native_app_glue.h>

#include <atomic>

namespace yabe_openxr {
/// forward input events to the compositor while compositor_ready is true
void init_input(android_app* app, const std::atomic_bool& compositor_ready);
}  // namespace yabe_openxr
