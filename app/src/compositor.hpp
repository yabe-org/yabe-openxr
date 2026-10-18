#pragma once
#include <atomic>

namespace yabe_openxr {
/// initialize and run the compositor until it is terminated.
/// set startup_failed to true if initialization fails
void run_compositor(
  std::atomic_bool& compositor_ready, std::atomic_bool& startup_failed);

/// watch config file and reload it while compositor_ready is true,
/// until is_finished becomes true
void watch_config(const std::atomic_bool& compositor_ready,
  const std::atomic_bool&                 is_finished);
}  // namespace yabe_openxr
