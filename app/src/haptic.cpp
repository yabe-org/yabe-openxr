#include "haptic.hpp"

#include <android/api-level.h>
#include <jni.h>

#include "log.hpp"

namespace {
bool check_exception(JNIEnv* env, const char* what) {
  if (!env->ExceptionCheck()) {
    return false;
  }
  LOG_ERROR("perform_haptic_feedback: {} threw", what);
  env->ExceptionClear();
  return true;
}

/// vibrate the default vibrator with VibrationAttributes (API 33);
/// View.performHapticFeedback() always uses USAGE_TOUCH, which is turned off
/// by default on some devices, whereas USAGE_HARDWARE_FEEDBACK represents
/// feedback for interaction with hardware such as a touchpad
bool vibrate_as_hardware_feedback(JNIEnv* env, jobject activity) {
  // predefined effects such as EFFECT_HEAVY_CLICK are played with medium
  // strength, which is too weak to notice; use the maximum amplitude instead
  constexpr jlong kDurationMs            = 20;
  constexpr jint  kMaxAmplitude          = 255;
  // VibrationAttributes.USAGE_HARDWARE_FEEDBACK
  constexpr jint  kUsageHardwareFeedback = 0x32;

  jclass    context_class = env->FindClass("android/content/Context");
  jmethodID get_system_service_method = env->GetMethodID(context_class,
    "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
  jstring   service_name              = env->NewStringUTF("vibrator");
  jobject   vibrator =
    env->CallObjectMethod(activity, get_system_service_method, service_name);
  if (check_exception(env, "getSystemService") || !vibrator) {
    return false;
  }

  jclass    vibrator_class = env->FindClass("android/os/Vibrator");
  jmethodID has_vibrator_method =
    env->GetMethodID(vibrator_class, "hasVibrator", "()Z");
  if (!env->CallBooleanMethod(vibrator, has_vibrator_method)) {
    check_exception(env, "hasVibrator");
    return false;
  }

  jclass    effect_class = env->FindClass("android/os/VibrationEffect");
  jmethodID create_one_shot_method = env->GetStaticMethodID(
    effect_class, "createOneShot", "(JI)Landroid/os/VibrationEffect;");
  jobject effect = env->CallStaticObjectMethod(
    effect_class, create_one_shot_method, kDurationMs, kMaxAmplitude);
  if (check_exception(env, "createOneShot") || !effect) {
    return false;
  }

  jclass    attributes_class = env->FindClass("android/os/VibrationAttributes");
  jmethodID create_for_usage_method = env->GetStaticMethodID(
    attributes_class, "createForUsage", "(I)Landroid/os/VibrationAttributes;");
  jobject attributes = env->CallStaticObjectMethod(
    attributes_class, create_for_usage_method, kUsageHardwareFeedback);
  if (check_exception(env, "createForUsage") || !attributes) {
    return false;
  }

  jmethodID vibrate_method = env->GetMethodID(vibrator_class, "vibrate",
    "(Landroid/os/VibrationEffect;Landroid/os/VibrationAttributes;)V");
  env->CallVoidMethod(vibrator, vibrate_method, effect, attributes);
  return !check_exception(env, "vibrate");
}

void perform_view_haptic_feedback(JNIEnv* env, jobject activity) {
  // HapticFeedbackConstants.LONG_PRESS
  constexpr jint kLongPress = 0;

  jclass    activity_class = env->GetObjectClass(activity);
  jmethodID get_window_method =
    env->GetMethodID(activity_class, "getWindow", "()Landroid/view/Window;");
  jobject   window       = env->CallObjectMethod(activity, get_window_method);
  jclass    window_class = env->GetObjectClass(window);
  jmethodID get_decor_view_method =
    env->GetMethodID(window_class, "getDecorView", "()Landroid/view/View;");
  jobject   decor_view = env->CallObjectMethod(window, get_decor_view_method);
  jclass    view_class = env->GetObjectClass(decor_view);
  jmethodID perform_haptic_feedback_method =
    env->GetMethodID(view_class, "performHapticFeedback", "(I)Z");
  env->CallBooleanMethod(
    decor_view, perform_haptic_feedback_method, kLongPress);
  check_exception(env, "performHapticFeedback");
}
}  // namespace

void perform_haptic_feedback(android_app* app) {
  JNIEnv* env = nullptr;
  if (app->activity->vm->GetEnv(
        reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK ||
      !env) {
    LOG_ERROR("perform_haptic_feedback: current thread is not attached");
    return;
  }
  // local references are released by PopLocalFrame()
  if (env->PushLocalFrame(16) != JNI_OK) {
    LOG_ERROR("perform_haptic_feedback: PushLocalFrame failed");
    env->ExceptionClear();
    return;
  }

  // VibrationAttributes is available since API 33
  constexpr int kApiLevelVibrationAttributes = 33;
  if (android_get_device_api_level() < kApiLevelVibrationAttributes ||
      !vibrate_as_hardware_feedback(env, app->activity->clazz)) {
    perform_view_haptic_feedback(env, app->activity->clazz);
  }

  env->PopLocalFrame(nullptr);
}
