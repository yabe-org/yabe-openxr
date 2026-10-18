#pragma once

namespace yabe_openxr {
/// throws std::runtime_error if fails
void        init_xkbcommon();
const char* create_keymap(const char* rules, const char* model,
  const char* layout, const char* variant, const char* options);
void        destroy_xkbcommon();
}  // namespace yabe_openxr
