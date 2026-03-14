cmake_minimum_required(VERSION 3.25) # for FetchContent_Declare(SYSTEM)
project(handover LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON) # for CMake >= 3.24
set(CMAKE_CXX_STANDARD 23)

find_package(OpenXR REQUIRED)
include(FetchContent)
FetchContent_Declare(
  VulkanHeaders SYSTEM
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG v1.4.345
)
FetchContent_MakeAvailable(VulkanHeaders)

include(AndroidNdkModules)
android_ndk_import_module_native_app_glue()

file(GLOB_RECURSE lib_sources src/*.cpp)
add_library(${PROJECT_NAME} MODULE ${lib_sources})

target_compile_options(${PROJECT_NAME} PRIVATE
  -Wall -Wextra -Wpedantic
  -Wno-gnu-zero-variadic-macro-arguments
  -Wno-missing-field-initializers # avoid adding `.next = nullptr` everywhere
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  XR_USE_GRAPHICS_API_VULKAN
  XR_USE_PLATFORM_ANDROID
)

target_include_directories(${PROJECT_NAME}
  PRIVATE src
  SYSTEM PRIVATE
  ${ANDROID_NDK}/sources/android/native_app_glue
)

target_link_libraries(${PROJECT_NAME} PRIVATE
  OpenXR::openxr_loader
  Vulkan::Headers
  android
  native_app_glue
  log
)

set_property(
  TARGET ${PROJECT_NAME}
  APPEND_STRING
  PROPERTY LINK_FLAGS " -u ANativeActivity_onCreate"
)

