#pragma once
#include <android_native_app_glue.h>

#include <atomic>
#include <string>

namespace yabe_openxr {
void               init_rootfs_path(const char* internal_data_path);
const std::string& rootfs_path();

/// extract rootfs if needed and clear runtime directories.
/// return false if fails
bool prepare_rootfs(android_app* app);

/// execute argv inside andlify environment and return its result
int exec_in_andlify(const char* const * argv);

/// start andlify environment and forward its stdout/stderr to log
/// until is_finished becomes true
void run_andlify(
  std::atomic_int& tracer_pid, const std::atomic_bool& is_finished);
}  // namespace yabe_openxr
