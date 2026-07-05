cmake_minimum_required(VERSION 3.25) # for FetchContent_Declare(SYSTEM)
project(yabe_openxr LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON) # for CMake >= 3.24
set(CMAKE_CXX_STANDARD 23)

find_package(OpenXR REQUIRED)
include(FetchContent)
FetchContent_Declare(
	glm
	GIT_REPOSITORY	https://github.com/g-truc/glm.git
	GIT_TAG 	      1.0.3
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
  andlify
  GIT_REPOSITORY https://github.com/watasuke102/andlify.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(andlify)

add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

file(GLOB_RECURSE lib_sources src/*.cpp)
add_library(${PROJECT_NAME} MODULE ${lib_sources})

target_compile_options(${PROJECT_NAME} PRIVATE
  -Wall -Wextra -Wpedantic
  -Wno-gnu-zero-variadic-macro-arguments
  -Wno-missing-field-initializers # avoid adding `.next = nullptr` everywhere
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  XR_USE_GRAPHICS_API_OPENGL_ES
  XR_USE_PLATFORM_ANDROID
  GLM_ENABLE_EXPERIMENTAL
)

add_library(yabe-compositor STATIC IMPORTED)
set_target_properties(yabe-compositor PROPERTIES
  IMPORTED_LOCATION ${CMAKE_CURRENT_LIST_DIR}/../../yabe-compositor/android-build/libyabe_compositor.a)

target_include_directories(${PROJECT_NAME}
  PRIVATE src
  SYSTEM PRIVATE
  ${ANDROID_NDK}/sources/android/native_app_glue
  ${CMAKE_CURRENT_LIST_DIR}/../../yabe-compositor
)

target_link_libraries(${PROJECT_NAME} PRIVATE
  OpenXR::openxr_loader
  EGL
  GLESv3
  glm::glm
  android
  native_app_glue
  log
  andlify_core
  yabe-compositor
)

set_property(
  TARGET ${PROJECT_NAME}
  APPEND_STRING
  PROPERTY LINK_FLAGS " -u ANativeActivity_onCreate"
)

