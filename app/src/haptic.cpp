#include "haptic.hpp"

#include <jni.h>

#include "log.hpp"

void perform_haptic_feedback(android_app* app) {
  JNIEnv* env = nullptr;
  if (app->activity->vm->GetEnv(
        reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK ||
      !env) {
    LOG_ERROR("perform_haptic_feedback: current thread is not attached");
    return;
  }

  jclass    activity_class = env->GetObjectClass(app->activity->clazz);
  jmethodID get_window_method =
    env->GetMethodID(activity_class, "getWindow", "()Landroid/view/Window;");
  jobject window =
    env->CallObjectMethod(app->activity->clazz, get_window_method);
  jclass    window_class = env->GetObjectClass(window);
  jmethodID get_decor_view_method =
    env->GetMethodID(window_class, "getDecorView", "()Landroid/view/View;");
  jobject   decor_view = env->CallObjectMethod(window, get_decor_view_method);
  jclass    view_class = env->GetObjectClass(decor_view);
  jmethodID perform_haptic_feedback_method =
    env->GetMethodID(view_class, "performHapticFeedback", "(I)Z");
  // HapticFeedbackConstants.LONG_PRESS
  constexpr jint kLongPress = 0;
  env->CallBooleanMethod(
    decor_view, perform_haptic_feedback_method, kLongPress);
  if (env->ExceptionCheck()) {
    LOG_ERROR("perform_haptic_feedback: performHapticFeedback threw");
    env->ExceptionClear();
  }

  env->DeleteLocalRef(view_class);
  env->DeleteLocalRef(decor_view);
  env->DeleteLocalRef(window_class);
  env->DeleteLocalRef(window);
  env->DeleteLocalRef(activity_class);
}
