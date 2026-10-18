#include "andlify_env.hpp"

#include <android/asset_manager.h>
#include <grp.h>
#include <poll.h>
#include <ptrace_engine.h>
#include <rootfs_extractor.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>

#include "log.hpp"

namespace yabe_openxr {
namespace {
std::string rootfs_path_;  // NOLINT(readability-identifier-naming)

/// return false if fails
bool ensure_rootfs_extracted(
  android_app* app, const std::string& extract_dst_path) {
  if (IsRootfsExtracted(extract_dst_path)) {
    return true;
  }
  const auto archive_path =
    std::format("{}/image.tar.zst", app->activity->internalDataPath);
  {
    auto* asset = AAssetManager_open(
      app->activity->assetManager, "image.tar.zst", AASSET_MODE_STREAMING);
    if (!asset) {
      LOG_ERROR("Failed to open asset image.tar.zst");
      return false;
    }
    std::ofstream out(archive_path, std::ios::binary);
    const void*   buf  = AAsset_getBuffer(asset);
    const auto    size = AAsset_getLength(asset);
    out.write(static_cast<const char*>(buf), size);
    AAsset_close(asset);
  }

  LOG_INFO("Extracting rootfs ({}) to {}", archive_path, extract_dst_path);
  if (ExtractRootfs(archive_path, extract_dst_path)) {
    return true;
  }
  LOG_ERROR("Failed to extract rootfs");
  return false;
}

void initialize_environment_variables() {
  setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", true);
  setenv("XDG_RUNTIME_DIR", "/tmp", true);
  setenv("WAYLAND_DISPLAY", "wayland-1", true);
  setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus", true);
}
constexpr uid_t kUserUid = 1000;
constexpr gid_t kUserGid = 1000;

/// returns exit status of the child, or -1 if it cannot be waited
int run_child(const std::function<void()>& child_func) {
  const int child = fork();
  if (child < 0) {
    LOG_ERROR("fork(): {}", std::strerror(errno));
    return -1;
  }
  if (child == 0) {
    chdir("/tmp");
    setenv("PWD", "/tmp", 1);
    child_func();
    std::_Exit(126);
  }
  LOG_DEBUG("child {} started", child);
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      LOG_ERROR("wait(): {}", std::strerror(errno));
      return -1;
    }
  }
  if (!WIFEXITED(status)) {
    LOG_ERROR("child {} terminated by signal {}", child, WTERMSIG(status));
    return -1;
  }
  return WEXITSTATUS(status);
}

bool drop_to_user() {
  const gid_t groups[] = {kUserGid};
  if (setgroups(1, groups) != 0 || setgid(kUserGid) != 0 ||
      setuid(kUserUid) != 0) {
    LOG_ERROR("Failed to drop privileges: {}", std::strerror(errno));
    return false;
  }
  setenv("HOME", "/home/user", 1);
  setenv("USER", "user", 1);
  setenv("LOGNAME", "user", 1);
  return true;
}

/// executed as root, like an init process
int andlify_entrypoint() {
  LOG_DEBUG("chroot started!");
  initialize_environment_variables();
  std::error_code error;
  std::filesystem::create_directories("/run/dbus", error);
  if (error) {
    LOG_ERROR("Failed to create /run/dbus: {}", error.message());
  }

  // system bus drops its privileges by itself according to system.conf.
  // /run persists unlike tmpfs, so a pid file would remain after the app exits
  const int system_bus_status = run_child([]() {
    execl("/usr/bin/dbus-daemon", "dbus-daemon", "--system", "--fork",
      "--nopidfile", nullptr);
  });
  LOG_INFO("DBus system bus launcher finished with {}", system_bus_status);

  const int session_bus_status = run_child([]() {
    if (!drop_to_user()) {
      return;
    }
    setenv("PS1", R"(\u:\w\$ )", 1);
    execl("/usr/bin/dbus-daemon", "dbus-daemon", "--session", "--nofork",
      "--address=unix:path=/run/user/1000/bus", nullptr);
  });
  LOG_INFO("DBus session bus finished with {}", session_bus_status);
  return 0;
}
}  // namespace

void init_rootfs_path(const char* internal_data_path) {
  rootfs_path_ = std::format("{}/rootfs", internal_data_path);
}

const std::string& rootfs_path() {
  return rootfs_path_;
}

bool prepare_rootfs(android_app* app) {
  if (!ensure_rootfs_extracted(app, rootfs_path_)) {
    return false;
  }
  // /run directory created by `filesystem` package in Arch Linux
  // may creates it as 0555, so fix it
  chmod(std::format("{}/run", rootfs_path_).c_str(), 0755);
  for (const auto* directory : {"run", "tmp"}) {
    const auto      path = std::filesystem::path(rootfs_path_) / directory;
    std::error_code error;
    const auto      status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_directory(status)) {
      LOG_ERROR("Invalid rootfs runtime directory {}: {}", path.string(),
        error ? error.message() : "not a directory");
      return false;
    }
    for (std::filesystem::directory_iterator entries(path, error), end;
      !error && entries != end; entries.increment(error)) {
      std::filesystem::remove_all(entries->path(), error);
    }
    if (error) {
      LOG_ERROR("Failed to clear rootfs runtime directory {}: {}",
        path.string(), error.message());
      return false;
    }
  }
  return true;
}

int exec_in_andlify(const char* const * argv) {
  return StartChrootFunc(
    rootfs_path_,
    [argv]() {
      initialize_environment_variables();
      execvp(argv[0], const_cast<char* const *>(argv));
      return 126;
    },
    STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
}

void run_andlify(
  std::atomic_int& tracer_pid, const std::atomic_bool& is_finished) {
  int pipe_out[2];
  int pipe_err[2];
  if (pipe(pipe_out) != 0 || pipe(pipe_err) != 0) {
    LOG_ERROR("Failed to create pipes");
    return;
  }

  LOG_INFO("Starting chroot to {}", rootfs_path_);
  auto res = StartChrootFunc(rootfs_path_, andlify_entrypoint, STDIN_FILENO,
    pipe_out[1], pipe_err[1], 0, 0);

  close(pipe_out[1]);
  close(pipe_err[1]);

  if (res < 0) {
    close(pipe_out[0]);
    close(pipe_err[0]);
    return;
  }
  tracer_pid.store(res);

  struct pollfd fds[2];
  fds[0].fd     = pipe_out[0];
  fds[0].events = POLLIN;
  fds[1].fd     = pipe_err[0];
  fds[1].events = POLLIN;

  char        buffer[1024];
  std::string out_buf;
  std::string err_buf;

  auto process_buffer = [](std::string& buf, const char* prefix) {
    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) {
      LOG_DEBUG("{} {}", prefix, buf.substr(0, pos));
      buf.erase(0, pos + 1);
    }
  };

  while (!is_finished.load()) {
    int ret = poll(fds, 2, 100);
    if (ret > 0) {
      if (fds[0].revents & (POLLIN | POLLHUP)) {
        ssize_t n = read(pipe_out[0], buffer, sizeof(buffer) - 1);
        if (n > 0) {
          buffer[n] = '\0';
          out_buf += buffer;
          process_buffer(out_buf, "[STDOUT]");
        } else if (n == 0) {
          fds[0].events = 0;
          fds[0].fd     = -1;
        }
      }
      if (fds[1].revents & (POLLIN | POLLHUP)) {
        ssize_t n = read(pipe_err[0], buffer, sizeof(buffer) - 1);
        if (n > 0) {
          buffer[n] = '\0';
          err_buf += buffer;
          process_buffer(err_buf, "[STDERR]");
        } else if (n == 0) {
          fds[1].events = 0;
          fds[1].fd     = -1;
        }
      }
    }
  }

  if (!out_buf.empty()) {
    LOG_DEBUG("[STDOUT] {}", out_buf);
  }
  if (!err_buf.empty()) {
    LOG_DEBUG("[STDERR] {}", err_buf);
  }

  close(pipe_out[0]);
  close(pipe_err[0]);
}
}  // namespace yabe_openxr
