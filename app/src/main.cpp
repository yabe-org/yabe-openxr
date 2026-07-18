#include <android/asset_manager.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android_native_app_glue.h>
#include <bits/wait.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/memfd.h>
#include <poll.h>
#include <ptrace_engine.h>
#include <rootfs_extractor.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>

#include "graphic.hpp"
#include "log.hpp"
#include "openxr.hpp"
#include "pointer_capture.hpp"
#include "yabe-compositor.h"

namespace {
std::string rootfs_path;

inline int sys_memfd_create(const char* name, unsigned int flags) {
  return static_cast<int>(syscall(SYS_memfd_create, name, flags));
}

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

int andlify_entrypoint() {
  LOG_DEBUG("chroot started!");

  setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
  setenv("XDG_RUNTIME_DIR", "/tmp", 1);
  setenv("WAYLAND_DISPLAY", "wayland-1", 1);
  setenv("WAYLAND_DEBUG", "1", 1);

  LOG_DEBUG("weston-terminal thread start!");
  while (true) {
    int child = fork();
    if (child < 0) {
      LOG_ERROR("fork(): {}", std::strerror(errno));
    }
    if (child == 0) {
      chdir("/tmp");
      setenv("PS1", R"(\u:\w\$ )", 1);
      execl("/usr/bin/weston-terminal", "weston-terminal",
        "--shell=/usr/bin/bash", nullptr);
      std::exit(0);
    } else {
      LOG_DEBUG("child {} started", child);
      int status = 0;
      if (waitpid(child, &status, 0) < 0) {
        LOG_ERROR("wait(): {}", std::strerror(errno));
      }
      int exit_status = WEXITSTATUS(status);
      LOG_INFO("weston-terminal ({}) finished with {}", child, exit_status);
      if (exit_status != 255) {
        break;
      }
    }
  }
  return 0;
}

xkb_state* g_xkb_state = nullptr;

void init_keyboard_keymap() {
  if (g_xkb_state) {
    return;
  }
  xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
  if (!context) {
    LOG_ERROR("Failed to create xkb context");
    return;
  }
  xkb_context_set_log_fn(context,
    [](xkb_context*, enum xkb_log_level, const char* format, va_list args) {
      static std::array<char, 1024> buffer;
      vsnprintf(buffer.data(), buffer.size(), format, args);
      LOG_DEBUG("xkb: {}", buffer.data());
    });
  xkb_context_include_path_append(context,
    std::format("{}/usr/share/xkeyboard-config-2", rootfs_path).c_str());
  xkb_keymap* keymap =
    xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap) {
    LOG_ERROR("Failed to create xkb keymap");
    xkb_context_unref(context);
    return;
  }
  g_xkb_state = xkb_state_new(keymap);

  char* keymap_str =
    xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  uint32_t size = std::strlen(keymap_str) + 1;  // '\0'

  int fd = sys_memfd_create("yabe-xkb-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd >= 0) {
    ftruncate(fd, size);
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr != MAP_FAILED) {
      LOG_DEBUG("keymap fd: {}, size: {}, ptr: {}", fd, size, (void*)ptr);
      std::memcpy(ptr, keymap_str, size);
      munmap(ptr, size);
      yabe_compositor_keymap(1, fd, size);
    }
    // we can close because it will be duped
    close(fd);
  }

  free(keymap_str);
  xkb_keymap_unref(keymap);
  xkb_context_unref(context);

  yabe_compositor_repeat_info(33, 500);
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
  init_keyboard_keymap();
  const bool is_pressed = (action == AKEY_EVENT_ACTION_DOWN);
  yabe_compositor_key(key, is_pressed);

  if (g_xkb_state) {
    xkb_state_update_key(
      g_xkb_state, key + 8, is_pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    const uint32_t depressed =
      xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_DEPRESSED);
    const uint32_t latched =
      xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LATCHED);
    const uint32_t locked =
      xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LOCKED);
    const uint32_t group =
      xkb_state_serialize_layout(g_xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);

    yabe_compositor_modifiers(depressed, latched, locked, group);
  }
  return 1;
}

int32_t handle_input_event(android_app* /*app*/, AInputEvent* event) {
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
}  // namespace

void android_main(android_app* app) {
  JNIEnv* env = nullptr;
  app->activity->vm->AttachCurrentThread(&env, nullptr);

  rootfs_path = std::format("{}/rootfs", app->activity->internalDataPath);
  {
    // if socket file exists, remove it
    const auto socket_path = std::format("{}/tmp/wayland-1", rootfs_path);
    if (access(socket_path.c_str(), F_OK) == 0) {
      unlink(socket_path.c_str());
    }
    const auto lock_path = socket_path + ".lock";
    if (access(lock_path.c_str(), F_OK) == 0) {
      unlink(lock_path.c_str());
    }
  }

  try {
    app->onInputEvent = handle_input_event;
    capture_pointer(app);

    auto graphic    = std::make_unique<yabe_openxr::Graphic>();
    auto openxr_app = std::make_unique<yabe_openxr::OpenXrApp>(app, graphic);

    std::atomic<bool> is_finished(false);
    std::atomic<int>  tracer_pid(-1);
    std::thread       andlify_thread([&app, &tracer_pid, &is_finished]() {
      ensure_rootfs_extracted(app, rootfs_path);

      int pipe_out[2];
      int pipe_err[2];
      if (pipe(pipe_out) != 0 || pipe(pipe_err) != 0) {
        LOG_ERROR("Failed to create pipes");
        return 0;
      }

      LOG_INFO("Starting chroot to {}", rootfs_path);
      auto res = StartChrootFunc(rootfs_path, andlify_entrypoint, STDIN_FILENO,
        pipe_out[1], pipe_err[1]);

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
    std::thread       compositor_thread([]() {
      const auto runtime_dir = std::format("{}/tmp", rootfs_path);
      setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1);
      setenv("WAYLAND_DISPLAY", "wayland-1", 1);
      setenv("WAYLAND_DEBUG", "1", 1);
      LOG_INFO("Starting YabeCompositor");
      yabe_compositor_init([](const char* s) {
        // use raw log function instead log macros to use different tag
        __android_log_print(ANDROID_LOG_DEBUG, "YabeCompositor", "%s", s);
      });
      yabe_compositor_register_socket_fd(-1);
      yabe_compositor_start();
      LOG_INFO("YabeCompositor finished");
    });

    LOG_INFO("Initialization completed");
    while (true) {
      process_android_events(app, openxr_app->is_session_running());
      if (app->destroyRequested != 0) {
        LOG_INFO("Android app destroy requested");
        break;
      }
      if (!openxr_app->pool(graphic)) {
        LOG_WARN("OpenXR app is exiting");
        break;
      }
    }

    // stop compositor at first (clients die at the same time)
    yabe_compositor_terminate();
    compositor_thread.join();
    if (g_xkb_state) {
      xkb_state_unref(g_xkb_state);
      g_xkb_state = nullptr;
    }

    StopChroot(tracer_pid);
    is_finished.store(true);
    andlify_thread.join();
    // OpenXR app should be destroyed before Graphic
    openxr_app.reset();
    graphic.reset();
  } catch (const std::exception& e) {
    LOG_ERROR("[Exception] {}", e.what());
  }

  app->activity->vm->DetachCurrentThread();
  LOG_INFO("Exiting");
}
