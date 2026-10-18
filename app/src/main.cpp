#include <android/looper.h>
#include <android_native_app_glue.h>
#include <ptrace_engine.h>

#include <atomic>
#include <exception>
#include <memory>
#include <thread>

#include "andlify_env.hpp"
#include "compositor.hpp"
#include "graphic.hpp"
#include "input.hpp"
#include "keymap.hpp"
#include "log.hpp"
#include "openxr.hpp"
#include "pointer_capture.hpp"
#include "yabe-compositor.h"

namespace {
bool is_app_resumed = false;
void handle_app_cmd(android_app* /*app*/, int32_t cmd) {
  switch (cmd) {
    case APP_CMD_RESUME:
      is_app_resumed = true;
      break;
    case APP_CMD_PAUSE:
      is_app_resumed = false;
      break;
    default:
      break;
  }
}

void process_android_events(android_app* app, bool is_session_running) {
  android_poll_source* src    = nullptr;
  int                  events = 0;
  // OpenXR events (e.g. session state change to READY) may arrive without
  // Android events, so block only while the app is not visible
  int                  timeout =
    (is_session_running || is_app_resumed || app->destroyRequested != 0) ? 0 :
                                                                           -1;
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

  yabe_openxr::init_rootfs_path(app->activity->internalDataPath);

  try {
    std::atomic_bool compositor_ready(false);
    app->onAppCmd = handle_app_cmd;
    yabe_openxr::init_input(app, compositor_ready);
    capture_pointer(app);

    auto graphic    = std::make_unique<yabe_openxr::Graphic>(compositor_ready);
    auto openxr_app = std::make_unique<yabe_openxr::OpenXrApp>(app, graphic);

    std::atomic<bool> is_finished(false);
    std::atomic<bool> startup_failed(false);
    std::atomic<int>  tracer_pid(-1);
    std::thread       andlify_thread;
    std::thread       compositor_thread;
    std::thread       config_watcher_thread;
    std::thread       initialization_thread(
      [app, &andlify_thread, &compositor_thread, &config_watcher_thread,
        &is_finished, &startup_failed, &tracer_pid, &compositor_ready]() {
        if (!yabe_openxr::prepare_rootfs(app)) {
          startup_failed.store(true);
          return;
        }
        if (is_finished.load()) {
          return;
        }
        try {
          yabe_openxr::init_xkbcommon();
        } catch (const std::exception& e) {
          LOG_ERROR("[Exception] {}", e.what());
          startup_failed.store(true);
          return;
        }

        andlify_thread    = std::thread([&tracer_pid, &is_finished]() {
          yabe_openxr::run_andlify(tracer_pid, is_finished);
        });
        compositor_thread = std::thread([&compositor_ready, &startup_failed]() {
          yabe_openxr::run_compositor(compositor_ready, startup_failed);
        });
        config_watcher_thread =
          std::thread([&compositor_ready, &is_finished]() {
            yabe_openxr::watch_config(compositor_ready, is_finished);
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
    // stop reloading config before terminating the compositor
    if (config_watcher_thread.joinable()) {
      config_watcher_thread.join();
    }
    if (compositor_thread.joinable()) {
      // Clients must stop before the chroot environment that owns them.
      compositor_ready.store(false);
      yabe_compositor_terminate();
      compositor_thread.join();
    }
    yabe_openxr::destroy_xkbcommon();

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
