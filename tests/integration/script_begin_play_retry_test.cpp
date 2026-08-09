// Verifies on_begin_play delivery survives transient module-load failure:
// a failed load leaves the entity pending (no permanent latch), retries are
// gated on the file's mtime changing (no per-frame retry storm), delivery
// stays exactly-once per entity, and one cached load serves many entities.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <system_error>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

/// Writes contents to a relative path.
bool write_file_at(const char *path, const char *contents) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, path, "wb") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(path, "wb");
  if (f == nullptr) {
    return false;
  }
#endif
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, f) == len);
  std::fclose(f);
  return ok;
}

/// Moves a file's timestamp forward so the mtime-gated retry re-attempts.
bool advance_mtime(const char *path) noexcept {
  std::error_code error{};
  const std::filesystem::file_time_type current =
      std::filesystem::last_write_time(path, error);
  if (error) {
    return false;
  }
  std::filesystem::last_write_time(path, current + std::chrono::seconds(2),
                                   error);
  return !error;
}

/// Runs one BeginPlay phase exactly as the pipeline does.
void run_begin_play_phase(rt::World *world) noexcept {
  world->begin_begin_play_phase();
  sc::dispatch_entity_scripts_begin_play(world);
  world->end_begin_play_phase();
}

/// Clears the Lua counters shared by the per-test module files.
bool reset_lua_counters() noexcept {
  constexpr const char *kResetPath = "bp_retry_reset.lua";
  if (!write_file_at(kResetPath, "bp_fired = nil\nchunk_runs = nil\n")) {
    return false;
  }
  const bool ok = sc::load_script(kResetPath);
  static_cast<void>(std::remove(kResetPath));
  return ok;
}

/// Creates an entity carrying the given script path.
rt::Entity make_scripted_entity(rt::World *world, const char *path) noexcept {
  const rt::Entity entity = world->create_entity();
  rt::ScriptComponent scriptComponent{};
  std::snprintf(scriptComponent.scriptPath,
                sizeof(scriptComponent.scriptPath), "%s", path);
  if (!world->add_script_component(entity, scriptComponent)) {
    return rt::kInvalidEntity;
  }
  return entity;
}

constexpr const char *kMissingPath = "bp_retry_missing.lua";
constexpr const char *kBrokenPath = "bp_retry_broken.lua";
constexpr const char *kSharedPath = "bp_retry_shared.lua";
constexpr const char *kModuleBody =
    "chunk_runs = (chunk_runs or 0) + 1\n"
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    bp_fired = (bp_fired or 0) + 1\n"
    "end\n"
    "function verify_fired_once()\n"
    "    if bp_fired ~= 1 then error('bp_fired ' .. tostring(bp_fired)) end\n"
    "end\n"
    "function verify_fired_three()\n"
    "    if bp_fired ~= 3 then error('bp_fired ' .. tostring(bp_fired)) end\n"
    "end\n"
    "function verify_single_chunk_run()\n"
    "    if chunk_runs ~= 1 then\n"
    "        error('chunk_runs ' .. tostring(chunk_runs))\n"
    "    end\n"
    "end\n"
    "return M\n";

/// Missing module file at first BeginPlay: delivery happens once it appears.
void test_missing_file_retries(engine::tests::TestContext &ctx,
                               rt::World *world) {
  static_cast<void>(std::remove(kMissingPath));
  const rt::Entity entity = make_scripted_entity(world, kMissingPath);
  ctx.check(entity != rt::kInvalidEntity, "create scripted entity");

  run_begin_play_phase(world);
  ctx.check(world->begin_play_pending_count() > 0U,
            "entity stays pending while the module cannot load");

  ctx.check(write_file_at(kMissingPath, kModuleBody), "write module file");
  run_begin_play_phase(world);
  ctx.check(sc::call_script_function("verify_fired_once"),
            "on_begin_play delivered after the module appears");
  ctx.check(world->begin_play_pending_count() == 0U,
            "entity marked done after delivery");

  run_begin_play_phase(world);
  ctx.check(sc::call_script_function("verify_fired_once"),
            "delivery stays exactly-once across later phases");
  static_cast<void>(world->destroy_entity(entity));
}

/// Broken module file: delivery happens after the file is fixed.
void test_broken_file_recovers(engine::tests::TestContext &ctx,
                               rt::World *world) {
  ctx.check(write_file_at(kBrokenPath, "this is not lua (("), "write broken");
  const rt::Entity entity = make_scripted_entity(world, kBrokenPath);
  ctx.check(entity != rt::kInvalidEntity, "create scripted entity");

  run_begin_play_phase(world);
  run_begin_play_phase(world);
  ctx.check(world->begin_play_pending_count() > 0U,
            "entity stays pending while the module is broken");

  ctx.check(write_file_at(kBrokenPath, kModuleBody), "fix module file");
  ctx.check(advance_mtime(kBrokenPath), "advance fixed-file mtime");
  run_begin_play_phase(world);
  ctx.check(sc::call_script_function("verify_fired_once"),
            "on_begin_play delivered after the module is fixed");
  static_cast<void>(world->destroy_entity(entity));
}

/// Three entities sharing one late module: all delivered, one chunk run.
void test_shared_module_many_entities(engine::tests::TestContext &ctx,
                                      rt::World *world) {
  static_cast<void>(std::remove(kSharedPath));
  rt::Entity entities[3] = {};
  for (rt::Entity &entity : entities) {
    entity = make_scripted_entity(world, kSharedPath);
    ctx.check(entity != rt::kInvalidEntity, "create scripted entity");
  }

  run_begin_play_phase(world);
  ctx.check(world->begin_play_pending_count() >= 3U,
            "all entities stay pending while the module cannot load");

  ctx.check(write_file_at(kSharedPath, kModuleBody), "write shared module");
  run_begin_play_phase(world);
  ctx.check(sc::call_script_function("verify_fired_three"),
            "every pending entity receives on_begin_play");
  ctx.check(sc::call_script_function("verify_single_chunk_run"),
            "the shared module chunk runs exactly once");
  for (const rt::Entity &entity : entities) {
    static_cast<void>(world->destroy_entity(entity));
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    return 1;
  }

  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    sc::shutdown_scripting();
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);

  engine::tests::TestContext ctx;
  ctx.check(reset_lua_counters(), "reset counters");
  test_missing_file_retries(ctx, world.get());
  sc::clear_entity_script_modules();
  ctx.check(reset_lua_counters(), "reset counters");
  test_broken_file_recovers(ctx, world.get());
  sc::clear_entity_script_modules();
  ctx.check(reset_lua_counters(), "reset counters");
  test_shared_module_many_entities(ctx, world.get());

  sc::clear_entity_script_modules();
  sc::shutdown_scripting();
  static_cast<void>(std::remove(kMissingPath));
  static_cast<void>(std::remove(kBrokenPath));
  static_cast<void>(std::remove(kSharedPath));
  return ctx.finish("script_begin_play_retry");
}
