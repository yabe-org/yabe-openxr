#pragma once

#include <android/log.h>

#include <format>

#define _LOG(severity, fmt, ...)                                               \
  __android_log_print(                                                         \
    severity, "YabeOpenXR", "%s", std::format(fmt, ##__VA_ARGS__).c_str());

#define LOG_ERROR(fmt, ...) _LOG(ANDROID_LOG_ERROR, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  _LOG(ANDROID_LOG_WARN, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  _LOG(ANDROID_LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) _LOG(ANDROID_LOG_DEBUG, fmt, ##__VA_ARGS__)
