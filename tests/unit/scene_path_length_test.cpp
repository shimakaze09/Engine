// Verifies that over-long paths in scene/prefab data and streaming
// requests are rejected with a diagnostic instead of silently truncating
// into a different path (audit H-16 / issue #85): load failure must leave
// the destination world unchanged, and max-length paths must round-trip
// exactly.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "engine/core/logging.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_streaming.h"
#include "engine/runtime/prefab_serializer.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

constexpr const char *kPrefabPath = "scene_path_length_prefab.json";

/// Builds a scene JSON buffer with one entity carrying the given field.
std::string make_scene_json(const char *componentKey,
                            const std::string &pathValue) {
  std::string json = "{\"version\":2,\"entities\":[{\"components\":{\"";
  json += componentKey;
  json += "\":\"";
  json += pathValue;
  json += "\"}}]}";
  return json;
}

/// Writes one prefab fixture file.
bool write_prefab_file(const std::string &contents) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kPrefabPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kPrefabPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const bool ok =
      std::fwrite(contents.data(), 1U, contents.size(), file) ==
      contents.size();
  std::fclose(file);
  return ok;
}

/// An over-long ScriptComponent path must fail the scene load and leave
/// the destination world unchanged; a max-length path must survive
/// exactly.
int check_scene_script_path_rejection() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 10;
  }
  const engine::runtime::Entity keeper = world->create_scene_object();
  if (keeper == engine::runtime::kInvalidEntity) {
    return 11;
  }

  const std::string longPath(
      sizeof(engine::runtime::ScriptComponent::scriptPath), 'a');
  const std::string overlongScene =
      make_scene_json("ScriptComponent", longPath);
  if (engine::runtime::load_scene(*world, overlongScene.c_str(),
                                  overlongScene.size())) {
    return 12;
  }
  if ((world->alive_entity_count() != 1U) || !world->is_alive(keeper)) {
    return 13;
  }

  const std::string maxPath(
      sizeof(engine::runtime::ScriptComponent::scriptPath) - 1U, 'b');
  const std::string maxScene = make_scene_json("ScriptComponent", maxPath);
  if (!engine::runtime::load_scene(*world, maxScene.c_str(),
                                   maxScene.size())) {
    return 14;
  }
  bool found = false;
  world->for_each_alive([&](engine::runtime::Entity entity) noexcept {
    const engine::runtime::ScriptComponent *script =
        world->get_script_component_ptr(entity);
    if ((script != nullptr) &&
        (std::strcmp(script->scriptPath, maxPath.c_str()) == 0)) {
      found = true;
    }
  });
  return found ? 0 : 15;
}

/// An over-long AnimationComponent controller path must fail the scene
/// load and leave the destination world unchanged.
int check_scene_controller_path_rejection() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 20;
  }

  const std::string longPath(
      sizeof(engine::runtime::AnimationComponent::controllerPath), 'c');
  const std::string overlongScene =
      make_scene_json("AnimationComponent", longPath);
  if (engine::runtime::load_scene(*world, overlongScene.c_str(),
                                  overlongScene.size())) {
    return 21;
  }
  return (world->alive_entity_count() == 0U) ? 0 : 22;
}

/// An over-long prefab script path must fail instantiation and leave the
/// world unchanged.
int check_prefab_path_rejection() {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 30;
  }

  const std::string longPath(
      sizeof(engine::runtime::ScriptComponent::scriptPath), 'd');
  std::string prefab =
      "{\"version\":1,\"components\":{\"ScriptComponent\":\"";
  prefab += longPath;
  prefab += "\"}}";
  if (!write_prefab_file(prefab)) {
    return 31;
  }

  const engine::runtime::Entity entity =
      engine::runtime::instantiate_prefab(*world, kPrefabPath);
  static_cast<void>(std::remove(kPrefabPath));
  if (entity != engine::runtime::kInvalidEntity) {
    return 32;
  }
  return (world->alive_entity_count() == 0U) ? 0 : 33;
}

/// An over-long streaming source path must be rejected by the queue and
/// the asset database instead of being truncated into a different path.
int check_streaming_path_rejection() {
  std::unique_ptr<engine::renderer::AssetStreamingQueue> queue(
      new (std::nothrow) engine::renderer::AssetStreamingQueue());
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if ((queue == nullptr) || (database == nullptr)) {
    return 40;
  }
  engine::renderer::clear_asset_database(database.get());

  const std::size_t capacity =
      sizeof(engine::renderer::LoadRequest::sourcePath);
  const std::string overlong(capacity + 40U, 'e');
  const std::string maxFit(capacity - 1U, 'f');

  const engine::renderer::LoadHandle rejected =
      engine::renderer::load_asset_async(queue.get(), 7ULL, overlong.c_str(),
                                         engine::renderer::LoadPriority::Low);
  if (rejected.valid()) {
    return 41;
  }
  const engine::renderer::LoadHandle accepted =
      engine::renderer::load_asset_async(queue.get(), 7ULL, maxFit.c_str(),
                                         engine::renderer::LoadPriority::Low);
  if (!accepted.valid()) {
    return 42;
  }

  if (engine::renderer::request_mesh_asset_streaming_load(
          database.get(), 9ULL, overlong.c_str())) {
    return 43;
  }
  if (engine::renderer::mesh_asset_state(database.get(), 9ULL) !=
      engine::renderer::AssetState::Unloaded) {
    return 44;
  }
  if (!engine::renderer::request_mesh_asset_streaming_load(
          database.get(), 9ULL, maxFit.c_str())) {
    return 45;
  }
  return 0;
}

} // namespace

int main() {
  static_cast<void>(engine::core::initialize_logging());

  struct Check final {
    const char *name;
    int (*fn)();
  };
  const Check checks[] = {
      {"scene script path", &check_scene_script_path_rejection},
      {"scene controller path", &check_scene_controller_path_rejection},
      {"prefab path", &check_prefab_path_rejection},
      {"streaming path", &check_streaming_path_rejection},
  };

  for (const Check &check : checks) {
    const int result = check.fn();
    if (result != 0) {
      std::fprintf(stderr, "scene_path_length_test failed (%s): %d\n",
                   check.name, result);
      return result;
    }
  }

  std::printf("scene_path_length_test: all tests passed\n");
  return 0;
}
