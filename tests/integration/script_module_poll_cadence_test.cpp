// Regression for #528: the entity script module cache polled its file's
// timestamp on every cache hit, so N scripted entities sharing a module
// cost N stat calls per frame (and a missing script kept polling per
// entity after its retry budget was spent). A dispatch frame now polls
// each cached module at most once, and a changed file is still picked up
// on the very next dispatch, through the production scripting bridge.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr const char *kSharedScript = "script_poll_cadence_shared.lua";
constexpr const char *kMissingScript = "script_poll_cadence_missing.lua";
constexpr const char *kDriverScript = "script_poll_cadence_driver.lua";
constexpr std::size_t kEntities = 200U;
constexpr int kFrames = 10;

bool write_file(const char *path, const char *contents) noexcept {
  std::FILE *file = nullptr;
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
  const std::size_t len = std::strlen(contents);
  const bool ok = std::fwrite(contents, 1U, len, file) == len;
  std::fclose(file);
  return ok;
}

/// Gives `count` fresh entities the same script component.
bool spawn_scripted(rt::World &world, const char *path,
                    std::size_t count) noexcept {
  for (std::size_t i = 0U; i < count; ++i) {
    const rt::Entity entity = world.create_entity();
    if (entity == rt::kInvalidEntity) {
      return false;
    }
    rt::ScriptComponent script{};
    std::snprintf(script.scriptPath, sizeof(script.scriptPath), "%s", path);
    if (!world.add_script_component(entity, script)) {
      return false;
    }
  }
  return true;
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

  const char *versionOne = "local M = {}\n"
                           "g_generation = 1\n"
                           "function M.on_tick(self, dt) end\n"
                           "return M\n";
  ctx.check(write_file(kSharedScript, versionOne), "write shared script");
  ctx.check(write_file(kDriverScript,
                       "function check_generation_two()\n"
                       "    if g_generation ~= 2 then error('stale') end\n"
                       "end\n") &&
                sc::load_script(kDriverScript),
            "load the driver script");
  ctx.check(spawn_scripted(*world, kSharedScript, kEntities),
            "spawn entities sharing one module");
  ctx.check(spawn_scripted(*world, kMissingScript, kEntities),
            "spawn entities naming a missing module");

  sc::dispatch_entity_scripts_begin_play(world.get());
  sc::dispatch_entity_scripts_update(1.0F / 60.0F);

  // Steady state: at most one poll per cached module (two here) per frame,
  // however many entities share them.
  const std::uint64_t before = sc::entity_script_mtime_polls();
  for (int frame = 0; frame < kFrames; ++frame) {
    sc::dispatch_entity_scripts_update(1.0F / 60.0F);
  }
  const std::uint64_t polls = sc::entity_script_mtime_polls() - before;
  ctx.check(polls <= static_cast<std::uint64_t>(2 * kFrames),
            "steady-state polls are bounded by modules times frames");
  if (polls > static_cast<std::uint64_t>(2 * kFrames)) {
    std::printf("  %llu polls over %d frames for %zu entities\n",
                static_cast<unsigned long long>(polls), kFrames,
                2U * kEntities);
  }

  // A changed file is still seen on the next dispatch.
  const char *versionTwo = "local M = {}\n"
                           "g_generation = 2\n"
                           "function M.on_tick(self, dt) end\n"
                           "return M\n";
  std::error_code error{};
  const auto mtime = std::filesystem::last_write_time(kSharedScript, error);
  ctx.check(!error && write_file(kSharedScript, versionTwo),
            "write the second generation");
  std::filesystem::last_write_time(kSharedScript,
                                   mtime + std::chrono::seconds(2), error);
  ctx.check(!error, "advance the shared script's timestamp");
  sc::dispatch_entity_scripts_update(1.0F / 60.0F);
  ctx.check(sc::call_script_function("check_generation_two"),
            "the next dispatch after the change runs the new generation");

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kSharedScript));
  static_cast<void>(std::remove(kDriverScript));
  return ctx.finish("script_module_poll_cadence");
}
