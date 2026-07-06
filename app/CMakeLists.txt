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

FetchContent_Declare(
  xkbcommon_src
  GIT_REPOSITORY https://github.com/xkbcommon/libxkbcommon.git
  GIT_TAG xkbcommon-1.13.2
  GIT_SHALLOW true
)
FetchContent_GetProperties(xkbcommon_src)
if(NOT xkbcommon_src_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(xkbcommon_src)
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/meson_cross.txt.in" ${CMAKE_CURRENT_BINARY_DIR}/meson_cross.txt)
include(ExternalProject)
ExternalProject_Add(
  xkbcommon_project
  SOURCE_DIR ${xkbcommon_src_SOURCE_DIR}
  BUILD_ALWAYS 0
  CONFIGURE_COMMAND meson setup <BINARY_DIR> <SOURCE_DIR> --cross-file ${CMAKE_CURRENT_BINARY_DIR}/meson_cross.txt --default-library=static -Denable-tools=false -Denable-x11=false -Denable-docs=false -Denable-wayland=false -Denable-xkbregistry=false -Denable-bash-completion=false
  BUILD_COMMAND meson compile -C <BINARY_DIR>
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS <BINARY_DIR>/libxkbcommon.a
)
ExternalProject_Get_Property(xkbcommon_project BINARY_DIR)
ExternalProject_Get_Property(xkbcommon_project SOURCE_DIR)
add_library(xkbcommon STATIC IMPORTED)
set_target_properties(xkbcommon PROPERTIES
  IMPORTED_LOCATION ${BINARY_DIR}/libxkbcommon.a
  INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${SOURCE_DIR}/include>"
)
add_dependencies(xkbcommon xkbcommon_project)

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

add_dependencies(${PROJECT_NAME} xkbcommon_project)

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
  xkbcommon
)

set_property(
  TARGET ${PROJECT_NAME}
  APPEND_STRING
  PROPERTY LINK_FLAGS " -u ANativeActivity_onCreate"
)

