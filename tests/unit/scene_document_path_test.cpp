// Pins where scene files are read and written: a path under a mounted
// virtual prefix ("assets/level.scene") names the project's file through
// the VFS wherever the process was started, as a project opened by path
// needs (the player, Lua's engine.load_scene), and never a same-named file
// under the working directory; an OS path (a file dialog's) and a path
// under no mount are used as they are.

#include "../test_harness.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;
namespace rt = engine::runtime;

constexpr const char *kScratch = "engine_scene_document_path_test";

std::unique_ptr<rt::World> world_with(std::size_t objects) noexcept {
  std::unique_ptr<rt::World> world(new (std::nothrow) rt::World());
  for (std::size_t i = 0U; (world != nullptr) && (i < objects); ++i) {
    if (world->create_scene_object() == rt::kInvalidEntity) {
      return nullptr;
    }
  }
  return world;
}

} // namespace

int main() {
  engine::tests::TestContext t;
  std::error_code ec{};
  fs::remove_all(kScratch, ec);
  const fs::path content = fs::path(kScratch) / "project_content";
  const fs::path decoyDir = fs::path(kScratch) / "assets";
  fs::create_directories(content, ec);
  fs::create_directories(decoyDir, ec);
  fs::current_path(kScratch, ec);
  if (ec || !engine::core::initialize_logging() ||
      !engine::core::initialize_vfs() ||
      !engine::core::mount("assets", "project_content")) {
    return 2;
  }

  std::unique_ptr<rt::World> authored = world_with(3U);
  t.check((authored != nullptr) &&
              rt::save_scene(*authored, "assets/level.scene"),
          "a scene saves to a virtual path");
  t.check(fs::exists("project_content/level.scene", ec) &&
              !fs::exists("assets/level.scene", ec),
          "it lands in the mounted content root, not the working "
          "directory's same-named folder");

  std::unique_ptr<rt::World> loaded = world_with(0U);
  t.check((loaded != nullptr) &&
              rt::load_scene(*loaded, "assets/level.scene", nullptr, nullptr) &&
              (loaded->alive_entity_count() == 3U),
          "the virtual path loads it back through the mount");

  std::unique_ptr<rt::World> decoy = world_with(1U);
  t.check((decoy != nullptr) && rt::save_scene(*decoy, "unmounted.scene") &&
              fs::exists("unmounted.scene", ec),
          "a path under no mount is an OS path, relative to the working "
          "directory");
  const std::string absolute =
      (fs::current_path(ec) / "project_content" / "level.scene").string();
  std::unique_ptr<rt::World> dialog = world_with(0U);
  t.check((dialog != nullptr) &&
              rt::load_scene(*dialog, absolute.c_str(), nullptr, nullptr) &&
              (dialog->alive_entity_count() == 3U),
          "an absolute OS path, as a file dialog gives, loads as it is");

  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
  fs::current_path("..", ec);
  fs::remove_all(kScratch, ec);
  return t.finish("scene_document_path");
}
