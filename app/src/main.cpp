#include <android/asset_manager.h>
#include <android/looper.h>
#include <android_native_app_glue.h>
#include <bits/wait.h>
#include <fcntl.h>
#include <ptrace_engine.h>
#include <rootfs_extractor.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>

#include "graphic.hpp"
#include "log.hpp"
#include "openxr.hpp"
#include "yabe-compositor.h"

namespace {
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
}  // namespace

void android_main(android_app* app) {
  JNIEnv* env = nullptr;
  app->activity->vm->AttachCurrentThread(&env, nullptr);

  std::atomic<bool> is_finished(false);
  std::atomic<int>  tracer_pid(-1);
  std::thread       ptrace_thread([&app, &tracer_pid, &is_finished]() {
    const auto extract_dst_path =
      std::format("{}/rootfs", app->activity->internalDataPath);
    ensure_rootfs_extracted(app, extract_dst_path);

    LOG_INFO("Starting chroot to {}", extract_dst_path);
    auto res = StartChrootFunc(
      extract_dst_path,
      []() -> int {
        LOG_DEBUG("chroot started!");
        setenv("XDG_RUNTIME_DIR", "/tmp", 1);
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        setenv("WAYLAND_DEBUG", "1", 1);

        std::thread client_thread([]() {
          LOG_DEBUG("weston-terminal thread start!");
          // while (true) {
          //   int res = access("/tmp/wayland-0", F_OK);
          //   if (res >= 0) {
          //     break;
          //   }
          // }
          while (true) {
            int child = fork();
            if (child < 0) {
              LOG_ERROR("fork(): {}", std::strerror(errno));
            }
            if (child == 0) {
              execl("/usr/bin/weston-terminal", "weston-terminal", nullptr);
              std::exit(0);
            } else {
              LOG_DEBUG("child {} started", child);
              int status = 0;
              if (waitpid(child, &status, 0) < 0) {
                LOG_ERROR("wait(): {}", std::strerror(errno));
              }
              int exit_status = WEXITSTATUS(status);
              LOG_INFO(
                "weston-terminal ({}) finished with {}", child, exit_status);
              if (exit_status != 255) {
                break;
              }
            }
          }
        });

        LOG_INFO("Starting YabeCompositor");
        yabe_compositor_start(
          [](const char* s) {
            LOG_DEBUG("YabeCompositor log> {}", s);
          },
          -1);
        LOG_INFO("YabeCompositor finished");
        return 0;
      },
      STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
    if (res < 0) {
      return 0;
    }
    tracer_pid.store(res);
    while (!is_finished.load()) {}
    return 0;
  });

  std::unique_ptr<yabe_openxr::OpenXrApp> openxr_app = nullptr;
  std::unique_ptr<yabe_openxr::Graphic>   graphic    = nullptr;
  try {
    graphic    = std::make_unique<yabe_openxr::Graphic>();
    openxr_app = std::make_unique<yabe_openxr::OpenXrApp>(app, graphic);
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
  } catch (const std::exception& e) {
    LOG_ERROR("[Exception] {}", e.what());
  }
  // OpenXR app should be destroyed before Graphic
  openxr_app.reset();
  graphic.reset();

  StopChroot(tracer_pid);
  is_finished.store(true);
  ptrace_thread.join();

  app->activity->vm->DetachCurrentThread();
  LOG_INFO("Exiting");
}
