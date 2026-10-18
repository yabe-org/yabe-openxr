#pragma once
#include <atomic>

namespace yabe_openxr {
/// initialize and run the compositor until it is terminated.
/// set startup_failed to true if initialization fails
void run_compositor(
  std::atomic_bool& compositor_ready, std::atomic_bool& startup_failed);
}  // namespace yabe_openxr
