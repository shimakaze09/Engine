// Verifies the startup scene is the empty 3D template: what the engine
// builds at startup (create_bootstrap_scene) is a "Main Camera", a
// "Directional Light" and a "Scene Controller" running the configured
// main script, and nothing else, and assets/main.scene, which player mode
// boots, loads to that same scene. The two cannot drift apart:
// changing either without the other fails here, and a mismatch writes the
// scene the engine builds to the temp directory for comparison.

#include "engine_bootstrap_content.h"

#include "../asset_root.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char *name) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", name);
    ++g_failures;
  }
}

} // namespace

int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: assets/main.scene not found\n");
    return 1;
  }
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 2;
  }
  engine::create_bootstrap_scene(world.get());

  check(world->alive_entity_count() == 3U, "the template holds three entities");
  const engine::runtime::Entity camera =
      world->find_entity_by_name("Main Camera");
  const engine::runtime::Entity light =
      world->find_entity_by_name("Directional Light");
  const engine::runtime::Entity controller =
      world->find_entity_by_name("Scene Controller");
  check(world->has_camera_component(camera), "a Main Camera with a camera");
  engine::runtime::LightComponent sun{};
  check(world->get_light_component(light, &sun) &&
            (sun.type == engine::runtime::LightType::Directional),
        "a Directional Light");
  engine::runtime::ScriptComponent script{};
  check(world->get_script_component(controller, &script) &&
            (std::strcmp(script.scriptPath, "assets/main.lua") == 0),
        "a Scene Controller running the default main script");

  // The shipped file is authored JSON; the comparison is semantic: it
  // loads through the production loader and must save exactly as the
  // built scene saves.
  std::unique_ptr<engine::runtime::World> shipped(new (std::nothrow)
                                                      engine::runtime::World());
  if (shipped == nullptr) {
    return 3;
  }
  check(engine::runtime::load_scene(*shipped, "assets/main.scene"),
        "assets/main.scene loads");
  std::vector<char> builtBytes(64U * 1024U, '\0');
  std::vector<char> shippedBytes(64U * 1024U, '\0');
  std::size_t builtSize = 0U;
  std::size_t shippedSize = 0U;
  check(engine::runtime::save_scene(*world, builtBytes.data(),
                                    builtBytes.size(), &builtSize) &&
            engine::runtime::save_scene(*shipped, shippedBytes.data(),
                                        shippedBytes.size(), &shippedSize),
        "both scenes save");
  const std::string built(builtBytes.data(), builtSize);
  if (built != std::string(shippedBytes.data(), shippedSize)) {
    std::error_code ec{};
    const std::filesystem::path dump =
        std::filesystem::temp_directory_path(ec) /
        "engine_startup_template.scene";
    std::ofstream out(dump, std::ios::binary);
    out.write(built.data(), static_cast<std::streamsize>(built.size()));
    std::fprintf(stderr,
                 "FAIL: assets/main.scene is not the startup scene; the scene "
                 "the engine builds is at %s\n",
                 dump.string().c_str());
    ++g_failures;
  }

  if (g_failures == 0) {
    std::printf("startup_template: all checks passed\n");
    return 0;
  }
  return 1;
}
