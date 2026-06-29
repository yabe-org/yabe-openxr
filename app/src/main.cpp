#include <android/looper.h>
#include <android_native_app_glue.h>

#include <memory>

#include "graphic.hpp"
#include "log.hpp"
#include "openxr.hpp"

namespace {
int android_pool(android_app* app) {
  android_poll_source* src    = nullptr;
  int                  events = 0;
  const int            pool_result =
    ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&src));
  if (pool_result >= 0 && src) {
    src->process(app, src);
  }
  return pool_result;
}
}  // namespace

void android_main(android_app* app) {
  JNIEnv* env = nullptr;
  app->activity->vm->AttachCurrentThread(&env, nullptr);

  std::unique_ptr<yabe_openxr::OpenXrApp> openxr_app = nullptr;
  std::unique_ptr<yabe_openxr::Graphic>   graphic    = nullptr;
  try {
    openxr_app = std::make_unique<yabe_openxr::OpenXrApp>(app);
    graphic    = std::make_unique<yabe_openxr::Graphic>(
      openxr_app, app->activity->assetManager);
    openxr_app->init_later(graphic);
    LOG_INFO("Initialization completed");

    while (true) {
      if (auto ret = android_pool(app); ret < 0) {
        LOG_WARN("ALooper_pollOnce() failed with result {}", ret);
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
