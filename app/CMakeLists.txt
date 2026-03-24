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
FetchContent_Declare(
	glm
	GIT_REPOSITORY	https://github.com/g-truc/glm.git
	GIT_TAG 	      1.0.3
)
FetchContent_MakeAvailable(glm)

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
  glm::glm
  android
  native_app_glue
  log
)

set_property(
  TARGET ${PROJECT_NAME}
  APPEND_STRING
  PROPERTY LINK_FLAGS " -u ANativeActivity_onCreate"
)

find_program(GLSLC glslc REQUIRED)
find_program(SPIRV_LINK spirv-link REQUIRED)
function(compile_hlsl SHADER_SRC)
  get_filename_component(FILE_NAME ${SHADER_SRC} NAME_WE)
  # `ASSETS_DIR` is given by `build.gradle`
  set(SHADER "${ASSETS_DIR}/shaders/${FILE_NAME}.spv")
  set(VERT_SHADER "${CMAKE_CURRENT_BINARY_DIR}/shaders/${FILE_NAME}_v.spv")
  set(FRAG_SHADER "${CMAKE_CURRENT_BINARY_DIR}/shaders/${FILE_NAME}_f.spv")
  add_custom_command(
    OUTPUT "${VERT_SHADER}"
    COMMAND
      "${GLSLC}"
      "-o"
      "${VERT_SHADER}"
      "-fshader-stage=vert"
      "-fentry-point=vert_main"
      "--target-env=vulkan1.0"
      $<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>,-g,>
      "${SHADER_SRC}"
    DEPENDS "${SHADER_SRC}"
    VERBATIM
  )
  add_custom_command(
    OUTPUT "${FRAG_SHADER}"
    COMMAND
      "${GLSLC}"
      "-o"
      "${FRAG_SHADER}"
      "-fshader-stage=frag"
      "-fentry-point=frag_main"
      "--target-env=vulkan1.0"
      $<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>,-g,>
      "${SHADER_SRC}"
    DEPENDS "${SHADER_SRC}"
    VERBATIM
  )
  add_custom_command(
    OUTPUT "${SHADER}"
    COMMAND
      "${SPIRV_LINK}"
      "-o"
      "${SHADER}"
      "${VERT_SHADER}"
      "${FRAG_SHADER}"
    DEPENDS "${SHADER_SRC}" "${VERT_SHADER}" "${FRAG_SHADER}"
    VERBATIM
  )
  set_source_files_properties("${SHADER}" GENERATED)
  target_sources(${PROJECT_NAME} PRIVATE "${SHADER}")
endfunction()
file(GLOB_RECURSE shaders shaders/*.hlsl)
foreach(SHADER_SRC ${shaders})
  compile_hlsl(${SHADER_SRC})
endforeach()

