cmake_minimum_required(VERSION 3.5) # for CMAKE_EXPORT_COMPILE_COMMANDS
project(handover LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON) # for CMake >= 3.24
set(CMAKE_CXX_STANDARD 23)

find_package(OpenXR REQUIRED)
file(GLOB_RECURSE lib_sources src/*.cpp)
add_library(${PROJECT_NAME} MODULE ${lib_sources})

target_compile_options(${PROJECT_NAME} PRIVATE
  -Wall -Wextra -Wpedantic)

target_include_directories(${PROJECT_NAME}
  PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue
  PRIVATE src)

target_link_libraries(${PROJECT_NAME} PRIVATE
  OpenXR::openxr_loader
  android
  EGL
  GLESv3
  log
)
