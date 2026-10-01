// Host for the asset-spawn entity-name regressions: drags a short, a
// 50-character and a 150-character virtual asset path through the
// production placement entry point (execute_asset_instantiate) so the
// driving CMake script can check that a long asset name is kept whole and
// that only one past NameComponent's 127-byte capacity is cut, with a
// diagnostic on stdout rather than silently.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "editor_asset_place.h"
#include "editor_commands.h"
#include "editor_session.h"
#include "engine/content/asset_catalog.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"

namespace {

constexpr const char *kMountPrefix = "spawntest";

/// Spawns one asset entity and prints its resulting name for the driving
/// CMake script to inspect alongside the stdout warning it may have logged.
bool spawn_and_report(const char *virtualPath) noexcept {
  engine::editor::AssetPlacePayload asset{};
  std::snprintf(asset.virtualPath, sizeof(asset.virtualPath), "%s",
                virtualPath);
  asset.kind = engine::content::AssetTypeTag::Mesh;
  const engine::runtime::Entity entity =
      engine::editor::execute_asset_instantiate(
          asset, engine::editor::EntitySpawnPlacement{});
  if (entity == engine::runtime::kInvalidEntity) {
    std::fprintf(stderr, "error: spawn failed for %s\n", virtualPath);
    return false;
  }

  engine::runtime::World *const world = engine::editor::editor_session().world;
  engine::runtime::NameComponent name{};
  if ((world == nullptr) || !world->get_name_component(entity, &name)) {
    std::fprintf(stderr, "error: spawned entity has no name component\n");
    return false;
  }
  std::printf("SPAWN_NAME len=%zu name=%s\n", std::strlen(name.name),
             name.name);
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_logging()) {
    return 2;
  }
  if (!engine::core::initialize_vfs()) {
    return 2;
  }
  if (!engine::core::mount(kMountPrefix, ".")) {
    std::fprintf(stderr, "error: failed to mount .\n");
    return 2;
  }

  // Both are fixed-capacity/entity-count-sized structs, too large for the
  // stack: heap-allocate like the other test hosts that construct a World.
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  if ((database == nullptr) || (world == nullptr)) {
    return 2;
  }

  engine::runtime::EngineAssetDatabaseService service{};
  std::unique_ptr<engine::content::AssetCatalog> serviceCatalog(
      new (std::nothrow) engine::content::AssetCatalog());
  service.catalog = serviceCatalog.get();
  service.database = database.get();
  engine::runtime::set_editor_asset_service(&service);

  auto &session = engine::editor::editor_session();
  session.world = world.get();

  char shortPath[128] = {};
  std::snprintf(shortPath, sizeof(shortPath), "%s/short_name.mesh",
               kMountPrefix);
  char mediumPath[128] = {};
  std::snprintf(mediumPath, sizeof(mediumPath),
                "%s/xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.mesh",
                kMountPrefix);
  char longStem[151] = {};
  std::memset(longStem, 'y', sizeof(longStem) - 1U);
  char longPath[256] = {};
  std::snprintf(longPath, sizeof(longPath), "%s/%s.mesh", kMountPrefix,
                longStem);

  bool ok = spawn_and_report(shortPath);
  ok = spawn_and_report(mediumPath) && ok;
  ok = spawn_and_report(longPath) && ok;

  session.world = nullptr;
  engine::runtime::set_editor_asset_service(nullptr);
  engine::core::shutdown_vfs();
  engine::core::shutdown_logging();
  return ok ? 0 : 3;
}
