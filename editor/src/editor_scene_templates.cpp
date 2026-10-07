// Implements the scene template list: the engine's Basic scene, found
// under the engine content root, and the project's templates/ folder,
// read once per call and sorted by name so the menu is stable.

#include "editor_scene_templates.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "engine/engine.h"

namespace engine::editor {

namespace {

namespace fs = std::filesystem;

/// Copies `path` into `out` with `label`; false when the path does not
/// fit, so it is not listed.
bool set_template(SceneTemplate *out, const std::string &label,
                  const std::string &path, bool builtIn) noexcept {
  if (path.size() >= sizeof(out->path)) {
    return false;
  }
  std::snprintf(out->path, sizeof(out->path), "%s", path.c_str());
  std::snprintf(out->label, sizeof(out->label), "%s", label.c_str());
  out->builtIn = builtIn;
  return true;
}

} // namespace

std::size_t list_scene_templates_in(const char *engineRoot,
                                    const char *assetRoot, SceneTemplate *out,
                                    std::size_t capacity) noexcept {
  if ((out == nullptr) || (capacity == 0U)) {
    return 0U;
  }
  std::size_t count = 0U;
  std::error_code ec{};
  if ((engineRoot != nullptr) && (engineRoot[0] != '\0')) {
    const fs::path basic = fs::path(engineRoot) / kBasicSceneTemplate;
    if (fs::is_regular_file(basic, ec) &&
        set_template(&out[count], "Basic", basic.generic_string(), true)) {
      ++count;
    }
  }

  if ((assetRoot == nullptr) || (assetRoot[0] == '\0')) {
    return count;
  }
  const fs::path folder = fs::path(assetRoot) / kProjectSceneTemplatesFolder;
  std::vector<fs::path> scenes{};
  for (fs::directory_iterator it(folder, ec), end; !ec && (it != end);
       it.increment(ec)) {
    std::error_code fileEc{};
    if (it->is_regular_file(fileEc) && !fileEc &&
        (it->path().extension() == ".scene")) {
      scenes.push_back(it->path());
    }
  }
  std::sort(scenes.begin(), scenes.end());
  for (const fs::path &scene : scenes) {
    if (count >= capacity) {
      break;
    }
    if (set_template(&out[count], scene.stem().string(),
                     scene.generic_string(), false)) {
      ++count;
    }
  }
  return count;
}

std::size_t list_scene_templates(SceneTemplate *out,
                                 std::size_t capacity) noexcept {
  const EngineConfig &config = active_config();
  return list_scene_templates_in(config.engineRoot, config.editorAssetRoot,
                                 out, capacity);
}

} // namespace engine::editor
