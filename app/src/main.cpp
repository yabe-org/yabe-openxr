#include <android/asset_manager.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android_native_app_glue.h>
#include <bits/wait.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <ptrace_engine.h>
#include <rootfs_extractor.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "graphic.hpp"
#include "log.hpp"
#include "openxr.hpp"
#include "pointer_capture.hpp"
#include "yabe-compositor.h"

namespace {
std::string      rootfs_path;
std::atomic_bool compositor_ready = false;

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
  setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/0/bus", true);
}
int andlify_entrypoint() {
  LOG_DEBUG("chroot started!");
  initialize_environment_variables();
  for (const auto& path : {"/run/dbus", "/run/user/0"}) {
    try {
      if (!std::filesystem::create_directories(path)) {
        LOG_ERROR("Tried to create existing directory: {}s", path);
      }
    } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("Failed to create {}: {}", path, e.what());
    }
  }
  while (true) {
    int child = fork();
    if (child < 0) {
      LOG_ERROR("fork(): {}", std::strerror(errno));
    }
    if (child == 0) {
      chdir("/tmp");
      setenv("PWD", "/tmp", 1);
      setenv("PS1", R"(\u:\w\$ )", 1);

      execl("/usr/bin/sh", "sh", "-c",
        "dbus-daemon --system  --fork &&"
        "dbus-daemon --session --nofork --address=unix:path=/run/user/0/bus",
        nullptr);
      std::exit(0);
    } else {
      LOG_DEBUG("child {} started", child);
      int status = 0;
      if (waitpid(child, &status, 0) < 0) {
        LOG_ERROR("wait(): {}", std::strerror(errno));
        continue;
      }
      int exit_status = WEXITSTATUS(status);
      LOG_INFO("DBus process ({}) finished with {}", child, exit_status);
      break;
    }
  }
  return 0;
}

int32_t send_mouse_button_event(int32_t button, bool is_pressed) {
  static int32_t prev_button = 0;
  const auto     diff        = button ^ prev_button;
  prev_button                = button;
  if (diff == AMOTION_EVENT_BUTTON_PRIMARY) {
    yabe_compositor_button(BTN_LEFT, is_pressed);
    return 1;
  }
  if (diff == AMOTION_EVENT_BUTTON_SECONDARY) {
    yabe_compositor_button(BTN_RIGHT, is_pressed);
    return 1;
  }
  if (diff == AMOTION_EVENT_BUTTON_TERTIARY) {
    yabe_compositor_button(BTN_MIDDLE, is_pressed);
    return 1;
  }
  return 0;
}
int32_t handle_mouse_input(AInputEvent* event) {
  const auto action = AMotionEvent_getAction(event);
  switch (action) {
    case AMOTION_EVENT_ACTION_BUTTON_PRESS:
      return send_mouse_button_event(AMotionEvent_getButtonState(event), true);
    case AMOTION_EVENT_ACTION_BUTTON_RELEASE:
      return send_mouse_button_event(AMotionEvent_getButtonState(event), false);
    case AMOTION_EVENT_ACTION_SCROLL:
      yabe_compositor_axis(
        0, AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0));
      return 1;
    case AMOTION_EVENT_ACTION_MOVE:
    case AMOTION_EVENT_ACTION_HOVER_MOVE: {
      const auto dx =
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RELATIVE_X, 0);
      const auto dy =
        AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RELATIVE_Y, 0);
      constexpr float kScale = 0.002F;
      yabe_compositor_move_pointer(dx * kScale, -dy * kScale);
      return 1;
    }
    default:
      return 0;
  }

  return 0;
}

int32_t handle_keyboard_input(AInputEvent* event) {
  const auto action = AKeyEvent_getAction(event);
  if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) {
    return 0;
  }
  const uint32_t key = AKeyEvent_getScanCode(event);
  if (key == 0) {
    return 0;
  }
  const bool is_pressed = (action == AKEY_EVENT_ACTION_DOWN);
  yabe_compositor_key(key, is_pressed);
  return 1;
}

int32_t handle_input_event(android_app* /*app*/, AInputEvent* event) {
  if (!compositor_ready.load()) {
    return 0;
  }
  if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
    handle_mouse_input(event);
  } else if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
    handle_keyboard_input(event);
  }
  return 0;
}

void process_android_events(android_app* app, bool is_session_running) {
  android_poll_source* src     = nullptr;
  int                  events  = 0;
  int                  timeout = is_session_running ? 0 : -1;
  while (ALooper_pollOnce(
           timeout, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
    if (src) {
      src->process(app, src);
    }
    timeout = 0;
  }
}

xkb_context* xkb_context_ = nullptr;  // NOLINT(readability-identifier-naming)
char*        keymap_str   = nullptr;
void         init_xkbcommon() {
  xkb_context_ = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
  if (!xkb_context_) {
    throw std::runtime_error("Failed to create xkb context");
  }
  const auto include_path =
    std::format("{}/usr/share/xkeyboard-config-2", rootfs_path);
  if (xkb_context_include_path_append(xkb_context_, include_path.c_str()) !=
      1) {
    throw std::runtime_error(
      std::format("Failed to add xkb include path: {}", include_path));
  }
}
const char* create_keymap(const char* rules, const char* model,
  const char* layout, const char* variant, const char* options) {
  free(keymap_str);
  keymap_str                 = nullptr;
  const xkb_rule_names names = {
    .rules   = rules,
    .model   = model,
    .layout  = layout,
    .variant = variant,
    .options = options,
  };
  auto* keymap = xkb_keymap_new_from_names2(xkb_context_, &names,
    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap) {
    LOG_ERROR("Failed to create xkb keymap");
    return nullptr;
  }
  keymap_str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  xkb_keymap_unref(keymap);
  return keymap_str;
}

int exec_in_andlify(const char* const * argv) {
  return StartChrootFunc(
    rootfs_path,
    [argv]() {
      initialize_environment_variables();
      execvp(argv[0], const_cast<char* const *>(argv));
      return 126;
    },
    STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
}

}  // namespace

void android_main(android_app* app) {
  JNIEnv* env = nullptr;
  app->activity->vm->AttachCurrentThread(&env, nullptr);

  rootfs_path = std::format("{}/rootfs", app->activity->internalDataPath);

  try {
    app->onInputEvent = handle_input_event;
    capture_pointer(app);

    auto graphic    = std::make_unique<yabe_openxr::Graphic>(compositor_ready);
    auto openxr_app = std::make_unique<yabe_openxr::OpenXrApp>(app, graphic);

    std::atomic<bool> is_finished(false);
    std::atomic<bool> startup_failed(false);
    std::atomic<int>  tracer_pid(-1);
    std::thread       andlify_thread;
    std::thread       compositor_thread;
    std::thread       initialization_thread(
      [app, &andlify_thread, &compositor_thread, &is_finished, &startup_failed,
        &tracer_pid]() {
        if (!ensure_rootfs_extracted(app, rootfs_path)) {
          startup_failed.store(true);
          return;
        }
        // /run directory created by `filesystem` package in Arch Linux
        // may creates it as 0555, so fix it
        chmod(std::format("{}/run", rootfs_path).c_str(), 0755);
        for (const auto* directory : {"run", "tmp"}) {
          const auto      path = std::filesystem::path(rootfs_path) / directory;
          std::error_code error;
          const auto      status = std::filesystem::symlink_status(path, error);
          if (error || !std::filesystem::is_directory(status)) {
            LOG_ERROR("Invalid rootfs runtime directory {}: {}", path.string(),
              error ? error.message() : "not a directory");
            startup_failed.store(true);
            return;
          }
          for (std::filesystem::directory_iterator entries(path, error), end;
            !error && entries != end; entries.increment(error)) {
            std::filesystem::remove_all(entries->path(), error);
          }
          if (error) {
            LOG_ERROR("Failed to clear rootfs runtime directory {}: {}",
              path.string(), error.message());
            startup_failed.store(true);
            return;
          }
        }
        if (is_finished.load()) {
          return;
        }
        try {
          init_xkbcommon();
        } catch (const std::exception& e) {
          LOG_ERROR("[Exception] {}", e.what());
          startup_failed.store(true);
          return;
        }

        andlify_thread    = std::thread([&tracer_pid, &is_finished]() {
          int pipe_out[2];
          int pipe_err[2];
          if (pipe(pipe_out) != 0 || pipe(pipe_err) != 0) {
            LOG_ERROR("Failed to create pipes");
            return 0;
          }

          LOG_INFO("Starting chroot to {}", rootfs_path);
          auto res = StartChrootFunc(rootfs_path, andlify_entrypoint,
            STDIN_FILENO, pipe_out[1], pipe_err[1]);

          close(pipe_out[1]);
          close(pipe_err[1]);

          if (res < 0) {
            close(pipe_out[0]);
            close(pipe_err[0]);
            return 0;
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

          if (!out_buf.empty())
            LOG_DEBUG("[STDOUT] {}", out_buf);
          if (!err_buf.empty())
            LOG_DEBUG("[STDERR] {}", err_buf);

          close(pipe_out[0]);
          close(pipe_err[0]);

          return 0;
        });
        compositor_thread = std::thread([&startup_failed]() {
          const auto runtime_dir = std::format("{}/tmp", rootfs_path);
          setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1);
          setenv("WAYLAND_DISPLAY", "wayland-1", 1);
          LOG_INFO("Starting YabeCompositor");
          yabe_compositor_init(
            [](const char* s) {
              // use raw log function instead log macros to use different tag
              __android_log_print(ANDROID_LOG_DEBUG, "YabeCompositor", "%s", s);
            },
            create_keymap, exec_in_andlify);
          const auto config_path =
            std::format("{}/etc/yabe/config.lua", rootfs_path);
          std::ifstream config(config_path);
          if (!config) {
            LOG_ERROR("Failed to open config: {}", config_path);
            startup_failed.store(true);
            return;
          }
          const std::string config_contents(
            std::istreambuf_iterator<char>(config), {});
          yabe_compositor_load_config(config_contents.c_str());
          yabe_compositor_register_socket_fd(-1);
          compositor_ready.store(true);
          yabe_compositor_start();
          compositor_ready.store(false);
          LOG_INFO("YabeCompositor finished");
        });
      });

    LOG_INFO("main loop will begin");
    while (true) {
      process_android_events(app, openxr_app->is_session_running());
      if (app->destroyRequested != 0) {
        LOG_INFO("Android app destroy requested");
        break;
      }
      if (startup_failed.load()) {
        LOG_ERROR("Initialization failed");
        break;
      }
      if (!openxr_app->pool(graphic)) {
        LOG_WARN("OpenXR app is exiting");
        break;
      }
    }

    is_finished.store(true);
    initialization_thread.join();
    if (compositor_thread.joinable()) {
      // Clients must stop before the chroot environment that owns them.
      compositor_ready.store(false);
      yabe_compositor_terminate();
      compositor_thread.join();
    }
    free(keymap_str);
    keymap_str = nullptr;
    if (xkb_context_) {
      xkb_context_unref(xkb_context_);
      xkb_context_ = nullptr;
    }

    StopChroot(tracer_pid);
    if (andlify_thread.joinable()) {
      andlify_thread.join();
    }
    // OpenXR app should be destroyed before Graphic
    openxr_app.reset();
    graphic.reset();
  } catch (const std::exception& e) {
    LOG_ERROR("[Exception] {}", e.what());
  }

  app->activity->vm->DetachCurrentThread();
  LOG_INFO("Exiting");
}
