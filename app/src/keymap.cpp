#include "keymap.hpp"

#include <xkbcommon/xkbcommon.h>

#include <cstdlib>
#include <format>
#include <stdexcept>

#include "andlify_env.hpp"
#include "log.hpp"

namespace yabe_openxr {
namespace {
xkb_context* xkb_context_ = nullptr;  // NOLINT(readability-identifier-naming)
char*        keymap_str   = nullptr;
}  // namespace

void init_xkbcommon() {
  xkb_context_ = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
  if (!xkb_context_) {
    throw std::runtime_error("Failed to create xkb context");
  }
  const auto include_path =
    std::format("{}/usr/share/xkeyboard-config-2", rootfs_path());
  if (xkb_context_include_path_append(xkb_context_, include_path.c_str()) !=
      1) {
    throw std::runtime_error(
      std::format("Failed to add xkb include path: {}", include_path));
  }
}

const char* create_keymap(const char* rules, const char* model,
  const char* layout, const char* variant, const char* options) {
  free(keymap_str);
  keymap_str                 = nullptr;
  const xkb_rule_names names = {
    .rules   = rules,
    .model   = model,
    .layout  = layout,
    .variant = variant,
    .options = options,
  };
  auto* keymap = xkb_keymap_new_from_names2(xkb_context_, &names,
    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap) {
    LOG_ERROR("Failed to create xkb keymap");
    return nullptr;
  }
  keymap_str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  xkb_keymap_unref(keymap);
  return keymap_str;
}

void destroy_xkbcommon() {
  free(keymap_str);
  keymap_str = nullptr;
  if (xkb_context_) {
    xkb_context_unref(xkb_context_);
    xkb_context_ = nullptr;
  }
}
}  // namespace yabe_openxr
