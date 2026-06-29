#include <android/asset_manager.h>
#include <android/looper.h>
#include <android_native_app_glue.h>
#include <fcntl.h>
#include <ptrace_engine.h>
#include <rootfs_extractor.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <thread>

#include "graphic.hpp"
#include "log.hpp"
#include "openxr.hpp"

namespace {
void process_android_events(android_app* app, bool is_session_running) {
  android_poll_source* src    = nullptr;
  int                  events = 0;
  int                  timeout = is_session_running ? 0 : -1;
  while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
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

  std::thread ptrace_thread([&app]() {
    const auto extract_dst_path =
      std::format("{}/rootfs", app->activity->internalDataPath);
    ensure_rootfs_extracted(app, extract_dst_path);

    int stdin_fd  = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC);
    int stdout_fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC);
    int stderr_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC);

    LOG_INFO("Starting chroot to {}", extract_dst_path);
    StartChrootFunc(
      extract_dst_path,
      []() -> int {
        std::thread([]() {
          setenv("XDG_RUNTIME_DIR", "/tmp", 1);
          setenv("WAYLAND_DISPLAY", "wayland-0", 1);
          setenv("WAYLAND_DEBUG", "1", 1);
          // wait 3s for compositor start
          std::this_thread::sleep_for(std::chrono::seconds(3));
          execv("/usr/bin/weston-terminal", nullptr);
        });
        return 0;
      },
      stdin_fd, stdout_fd, stderr_fd);
  });

  std::unique_ptr<yabe_openxr::OpenXrApp> openxr_app = nullptr;
  std::unique_ptr<yabe_openxr::Graphic>   graphic    = nullptr;
  try {
    openxr_app = std::make_unique<yabe_openxr::OpenXrApp>(app);
    graphic    = std::make_unique<yabe_openxr::Graphic>(
      openxr_app, app->activity->assetManager);
    openxr_app->init_later(graphic);
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

  app->activity->vm->DetachCurrentThread();
  LOG_INFO("Exiting");
}
