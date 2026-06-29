#include "pointer_capture.hpp"
#include <jni.h>
#include "log.hpp"

void capture_pointer(android_app* app) {
  JNIEnv* env = nullptr;
  app->activity->vm->AttachCurrentThread(&env, nullptr);
  if (!env) {
    LOG_ERROR("capture_pointer: Failed to attach current thread");
    return;
  }

  // Get Window from Activity
  jclass activity_class = env->GetObjectClass(app->activity->clazz);
  jmethodID get_window_method = env->GetMethodID(activity_class, "getWindow", "()Landroid/view/Window;");
  if (!get_window_method) {
    LOG_ERROR("capture_pointer: getWindow method not found");
    env->DeleteLocalRef(activity_class);
    return;
  }
  jobject window = env->CallObjectMethod(app->activity->clazz, get_window_method);
  if (!window) {
    LOG_ERROR("capture_pointer: Failed to get Window");
    env->DeleteLocalRef(activity_class);
    return;
  }

  // Get DecorView from Window
  jclass window_class = env->GetObjectClass(window);
  jmethodID get_decor_view_method = env->GetMethodID(window_class, "getDecorView", "()Landroid/view/View;");
  if (!get_decor_view_method) {
    LOG_ERROR("capture_pointer: getDecorView method not found");
    env->DeleteLocalRef(window_class);
    env->DeleteLocalRef(window);
    env->DeleteLocalRef(activity_class);
    return;
  }
  jobject decor_view = env->CallObjectMethod(window, get_decor_view_method);
  if (!decor_view) {
    LOG_ERROR("capture_pointer: Failed to get DecorView");
    env->DeleteLocalRef(window_class);
    env->DeleteLocalRef(window);
    env->DeleteLocalRef(activity_class);
    return;
  }

  // Request pointer capture
  jclass view_class = env->GetObjectClass(decor_view);
  jmethodID request_pointer_capture_method = env->GetMethodID(view_class, "requestPointerCapture", "()V");
  if (request_pointer_capture_method) {
    env->CallVoidMethod(decor_view, request_pointer_capture_method);
    LOG_INFO("capture_pointer: Pointer capture requested");
  } else {
    LOG_ERROR("capture_pointer: Failed to get requestPointerCapture method");
    env->ExceptionClear(); // Clear NoSuchMethodError if API < 26
  }

  env->DeleteLocalRef(view_class);
  env->DeleteLocalRef(decor_view);
  env->DeleteLocalRef(window_class);
  env->DeleteLocalRef(window);
  env->DeleteLocalRef(activity_class);
}
