#include "graphic.hpp"

#include "log.hpp"
#include "openxr.hpp"

namespace yabe_quest {
Graphic::Graphic(
  std::unique_ptr<OpenXrApp>& openxr_app, AAssetManager* asset_manager) {
  init_shader(asset_manager);
}
Graphic::~Graphic() {
  LOG_INFO("Destroying Graphic");
}
}  // namespace yabe_quest
