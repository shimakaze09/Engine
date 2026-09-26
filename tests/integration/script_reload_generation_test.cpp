// Verifies that both Lua hot-reload executors record a committed reload on
// the asset catalog and a failed one not at all (issue #682): the main
// script's reload and an entity module's each move the script's reload
// generation by exactly one when the new file commits, and leave it where
// it was when the new file fails. Runs through the production watch and
// dispatch paths against the production scripting bridge, with the
// engine's asset service published so the bridge reaches the catalog.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <system_error>

#include "../test_harness.h"
#include "engine/content/asset_catalog.h"
#include "engine/core/service_locator.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

engine::tests::TestContext g_tests{};

constexpr const char *kMainPath = "script_reload_generation_main.lua";
constexpr const char *kModulePath = "script_reload_generation_module.lua";

/// Writes `text` to `path` and moves its time a whole second past the
/// previous one, so the watcher sees the save however coarse the
/// filesystem's clock.
bool save(const char *path, const char *text) {
  std::error_code ec{};
  const auto before = std::filesystem::last_write_time(path, ec);
  const bool existed = !ec;
  ec.clear();
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t size = std::strlen(text);
  const bool wrote = std::fwrite(text, 1U, size, file) == size;
  const bool closed = std::fclose(file) == 0;
  if (existed) {
    std::filesystem::last_write_time(path, before + std::chrono::seconds(1),
                                     ec);
  }
  return wrote && closed && !ec;
}

/// Catalogues a script under the path the executors name it by.
bool catalogue(engine::content::AssetCatalog *catalog, const char *path) {
  engine::content::AssetMetadata record{};
  record.assetId = engine::content::make_asset_id_from_path(path);
  record.typeTag = engine::content::AssetTypeTag::Script;
  engine::content::write_metadata_path(&record.filePath, path);
  return engine::content::register_asset_metadata(catalog, record);
}

std::uint32_t generation(const engine::content::AssetCatalog *catalog,
                         const char *path) {
  return engine::content::asset_reload_generation(
      catalog, engine::content::make_asset_id_from_path(path));
}

void check_main_script(engine::content::AssetCatalog *catalog) {
  g_tests.check(save(kMainPath, "main_version = 1\n"), "save the main");
  g_tests.check(engine::scripting::load_script(kMainPath),
                "the main script loads");
  engine::scripting::watch_script_file(kMainPath);

  g_tests.check(save(kMainPath, "main_version = 2\n"), "save a good main");
  engine::scripting::check_script_reload();
  g_tests.check(generation(catalog, kMainPath) == 1U,
                "a committed main-script reload moves its generation once");

  g_tests.check(save(kMainPath, "main_version = 3\nerror('broken')\n"),
                "save a broken main");
  engine::scripting::check_script_reload();
  g_tests.check(generation(catalog, kMainPath) == 1U,
                "a failed main-script reload leaves its generation");
}

void check_entity_module(engine::runtime::World *world,
                         engine::content::AssetCatalog *catalog) {
  const char *v1 = "local M = {}\n"
                   "function M.on_tick(self, dt) end\n"
                   "return M\n";
  const engine::runtime::Entity entity = world->create_entity();
  engine::runtime::ScriptComponent script{};
  std::snprintf(script.scriptPath, sizeof(script.scriptPath), "%s",
                kModulePath);
  g_tests.check(save(kModulePath, v1) &&
                    (entity != engine::runtime::kInvalidEntity) &&
                    world->add_script_component(entity, script),
                "a scripted entity");
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  g_tests.check(generation(catalog, kModulePath) == 0U,
                "a first load is not a reload");

  g_tests.check(save(kModulePath, "local M = {}\n"
                                  "function M.on_tick(self, dt) end\n"
                                  "M.version = 2\n"
                                  "return M\n"),
                "save a good module");
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  g_tests.check(generation(catalog, kModulePath) == 1U,
                "a committed module reload moves its generation once");

  g_tests.check(save(kModulePath, "error('broken module')\n"),
                "save a broken module");
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  g_tests.check(generation(catalog, kModulePath) == 1U,
                "a failed module reload leaves its generation");
}

} // namespace

int main() {
  if (!engine::scripting::initialize_scripting()) {
    g_tests.fail("initialize scripting");
    return g_tests.finish("script reload generation tests");
  }
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::AssetManager> manager(
      new (std::nothrow) engine::renderer::AssetManager());
  std::unique_ptr<engine::content::AssetCatalog> catalog(
      new (std::nothrow) engine::content::AssetCatalog());
  if ((world == nullptr) || (database == nullptr) || (manager == nullptr) ||
      (catalog == nullptr)) {
    g_tests.fail("allocate the session");
    engine::scripting::shutdown_scripting();
    return g_tests.finish("script reload generation tests");
  }
  engine::renderer::clear_asset_database(database.get());
  engine::renderer::clear_asset_manager(manager.get());
  engine::core::ServiceLocator locator{};
  engine::runtime::EngineAssetDatabaseService assetService{};
  assetService.catalog = catalog.get();
  assetService.database = database.get();
  assetService.manager = manager.get();
  g_tests.check(
      locator.register_service<engine::runtime::EngineAssetDatabaseService>(
          &assetService),
      "publish the asset service");
  engine::runtime::bind_scripting_runtime(world.get(), locator);

  g_tests.check(catalogue(catalog.get(), kMainPath) &&
                    catalogue(catalog.get(), kModulePath),
                "catalogue the scripts");
  check_main_script(catalog.get());
  check_entity_module(world.get(), catalog.get());

  engine::runtime::unbind_scripting_runtime(locator);
  engine::scripting::clear_entity_script_modules();
  engine::scripting::shutdown_scripting();
  std::remove(kMainPath);
  std::remove(kModulePath);
  return g_tests.finish("script reload generation tests");
}
