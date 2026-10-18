#include "compositor.hpp"

#include <android/log.h>
#include <ownership/ownership_store.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <string>

#include "andlify_env.hpp"
#include "keymap.hpp"
#include "log.hpp"
#include "yabe-compositor.h"

namespace yabe_openxr {
void run_compositor(
  std::atomic_bool& compositor_ready, std::atomic_bool& startup_failed) {
  const auto runtime_dir = std::format("{}/tmp", rootfs_path());
  setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1);
  setenv("WAYLAND_DISPLAY", "wayland-1", 1);
  LOG_INFO("Starting YabeCompositor");
  yabe_compositor_init(
    [](const char* s) {
      // use raw log function instead log macros to use different tag
      __android_log_print(ANDROID_LOG_DEBUG, "YabeCompositor", "%s", s);
    },
    create_keymap, exec_in_andlify);
  const auto config_path = std::format("{}/etc/yabe/config.lua", rootfs_path());
  std::ifstream config(config_path);
  if (!config) {
    LOG_ERROR("Failed to open config: {}", config_path);
    startup_failed.store(true);
    return;
  }
  const std::string config_contents(std::istreambuf_iterator<char>(config), {});
  yabe_compositor_load_config(config_contents.c_str());
  const auto              display_id = yabe_compositor_register_socket_fd(-1);
  andlify::OwnershipStore ownership;
  const auto              socket_path =
    std::format("{}/wayland-{}", runtime_dir, display_id);
  // Host-created sockets otherwise inherit virtual root ownership.
  if (!ownership.Open(rootfs_path(), true) ||
      !ownership.SetPath(socket_path, 1000, 1000, 0600)) {
    LOG_ERROR(
      "Failed to assign Wayland socket ownership: {}", std::strerror(errno));
    startup_failed.store(true);
    return;
  }
  compositor_ready.store(true);
  yabe_compositor_start();
  compositor_ready.store(false);
  LOG_INFO("YabeCompositor finished");
}
}  // namespace yabe_openxr
