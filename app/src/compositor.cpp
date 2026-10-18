#include "compositor.hpp"

#include <android/log.h>
#include <ownership/ownership_store.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>

#include "andlify_env.hpp"
#include "keymap.hpp"
#include "log.hpp"
#include "yabe-compositor.h"

namespace yabe_openxr {
namespace {
std::filesystem::path config_path() {
  return std::filesystem::path(rootfs_path()) / "etc/yabe/config.lua";
}

std::optional<std::string> read_config() {
  const auto    path = config_path();
  std::ifstream config(path);
  if (!config) {
    LOG_ERROR("Failed to open config: {}", path.string());
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(config), {});
}
}  // namespace

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
  const auto config_contents = read_config();
  if (!config_contents) {
    startup_failed.store(true);
    return;
  }
  yabe_compositor_load_config(config_contents->c_str());
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

void watch_config(const std::atomic_bool& compositor_ready,
  const std::atomic_bool&                 is_finished) {
  const int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotify_fd < 0) {
    LOG_ERROR("Failed to initialize inotify: {}", std::strerror(errno));
    return;
  }
  // watch parent directory; some editor does not write the file directly
  // but replace it with another temporal file
  const auto path = config_path();
  if (inotify_add_watch(inotify_fd, path.parent_path().c_str(),
        IN_CLOSE_WRITE | IN_MOVED_TO) == -1) {
    LOG_ERROR(
      "Failed to add config directory to watch: {}", std::strerror(errno));
    close(inotify_fd);
    return;
  }
  const auto config_name = path.filename().string();

  alignas(inotify_event) std::array<uint8_t, 4096> buffer;
  pollfd fds = {.fd = inotify_fd, .events = POLLIN};
  while (!is_finished.load()) {
    if (poll(&fds, 1, 100) <= 0) {
      continue;
    }
    const auto len = read(inotify_fd, buffer.data(), buffer.size());
    if (len < 0) {
      if (errno != EAGAIN && errno != EINTR) {
        LOG_ERROR("Failed to read inotify_fd: {}", std::strerror(errno));
      }
      continue;
    }
    bool changed = false;
    for (uint8_t* p = buffer.data(); p < buffer.data() + len;) {
      auto* event = reinterpret_cast<inotify_event*>(p);
      if (event->len > 0 && config_name == event->name &&
          (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))) {
        changed = true;
      }
      p += sizeof(inotify_event) + event->len;
    }
    if (!changed || !compositor_ready.load()) {
      continue;
    }
    if (const auto contents = read_config()) {
      LOG_INFO("Reloading config: {}", path.string());
      yabe_compositor_load_config(contents->c_str());
    }
  }
  close(inotify_fd);
}
}  // namespace yabe_openxr
