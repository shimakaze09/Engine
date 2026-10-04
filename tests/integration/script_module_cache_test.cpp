// Boundary tests for the entity script module table: capacity fills at
// 1,024 loaded modules (it held 32, which a project with about thirty
// behaviour scripts and a few libraries met), the next load fails cleanly
// with the engine still serving cached modules, and a never-loaded
// (negative) entry is evicted to make room for a new loadable module.
// Through the production dispatch, every one of 0, 1, 33, 100 and 1,024
// entities with distinct scripts receives on_begin_play and on_tick, and
// with one more distinct script only that entity is left out.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr int kCacheCapacity = 1024;
constexpr const char *kDriverPath = "cache_driver.lua";
constexpr const char *kBrokenPath = "cache_broken.lua";
constexpr const char *kExtraPath = "cache_extra.lua";
constexpr int kDispatchFiles = kCacheCapacity + 1;

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

/// Builds the numbered module file name for slot-filling loads.
void module_name(int index, char (&out)[64]) noexcept {
  std::snprintf(out, sizeof(out), "cache_mod_%04d.lua", index);
}

/// Writes the numbered tiny module files used to fill the cache.
bool write_module_files(int count) noexcept {
  for (int i = 1; i <= count; ++i) {
    char name[64] = {};
    module_name(i, name);
    char body[128] = {};
    std::snprintf(body, sizeof(body), "return { id = %d }\n", i);
    if (!write_file_at(name, body)) {
      return false;
    }
  }
  return true;
}

/// Builds the numbered entity script name for dispatch rounds.
void entity_script_name(int index, char (&out)[64]) noexcept {
  std::snprintf(out, sizeof(out), "cache_entity_%04d.lua", index);
}

/// Writes entity scripts that count their own hooks by their number.
bool write_entity_script_files(int count) noexcept {
  for (int i = 1; i <= count; ++i) {
    char name[64] = {};
    entity_script_name(i, name);
    char body[256] = {};
    std::snprintf(body, sizeof(body),
                  "local M = {}\n"
                  "function M.on_begin_play(self)\n"
                  "  begun[%d] = (begun[%d] or 0) + 1\n"
                  "end\n"
                  "function M.on_tick(self, dt)\n"
                  "  ticked[%d] = (ticked[%d] or 0) + 1\n"
                  "end\n"
                  "return M\n",
                  i, i, i, i);
    if (!write_file_at(name, body)) {
      return false;
    }
  }
  return true;
}

/// Removes the numbered module files and the driver/broken/extra files.
void remove_test_files(int count) noexcept {
  for (int i = 1; i <= count; ++i) {
    char name[64] = {};
    module_name(i, name);
    static_cast<void>(std::remove(name));
  }
  for (int i = 1; i <= kDispatchFiles; ++i) {
    char name[64] = {};
    entity_script_name(i, name);
    static_cast<void>(std::remove(name));
  }
  static_cast<void>(std::remove(kDriverPath));
  static_cast<void>(std::remove(kBrokenPath));
  static_cast<void>(std::remove(kExtraPath));
}

constexpr const char *kDriver =
    "function require_range(first, last)\n"
    "    for i = first, last do\n"
    "        local name = string.format('cache_mod_%04d.lua', i)\n"
    "        local mod = engine.require(name)\n"
    "        if type(mod) ~= 'table' or mod.id ~= i then\n"
    "            error('module ' .. name .. ' failed to load')\n"
    "        end\n"
    "    end\n"
    "end\n"
    "function require_one_fails(name)\n"
    "    if engine.require(name) ~= nil then\n"
    "        error('expected nil for ' .. name)\n"
    "    end\n"
    "end\n"
    "function reset_hook_counts() begun = {}; ticked = {} end\n"
    "function expect_ran(n) expected_ran = n end\n"
    "function expect_total(n) expected_total = n end\n"
    "function check_hooks()\n"
    "    local ran, total = expected_ran, expected_total\n"
    "    for i = 1, total do\n"
    "        local want = (i <= ran) and 1 or nil\n"
    "        if begun[i] ~= want or ticked[i] ~= want then\n"
    "            error('script ' .. i .. ' began ' .. tostring(begun[i]) ..\n"
    "                  ', ticked ' .. tostring(ticked[i]))\n"
    "        end\n"
    "    end\n"
    "end\n"
    "function require_one_ok(name)\n"
    "    if type(engine.require(name)) ~= 'table' then\n"
    "        error('expected table for ' .. name)\n"
    "    end\n"
    "end\n";

/// The table fills; one more load fails cleanly; cached entries keep
/// serving.
void test_capacity_boundary(engine::tests::TestContext &ctx) {
  sc::clear_entity_script_modules();
  ctx.check(sc::call_script_function("require_all"),
            "modules 1..capacity all load (the table fills)");
  ctx.check(sc::call_script_function("require_past_capacity_fails"),
            "the module past capacity is rejected");
  ctx.check(sc::call_script_function("require_all"),
            "every cached module still resolves after the rejection");
  ctx.check(sc::call_script_function("require_past_capacity_fails"),
            "repeated over-capacity loads keep failing cleanly");
}

/// A negative (never-loaded) entry is evicted to admit a loadable module.
void test_negative_entry_eviction(engine::tests::TestContext &ctx) {
  sc::clear_entity_script_modules();
  ctx.check(write_file_at(kBrokenPath, "this is not lua (("), "write broken");
  ctx.check(sc::call_script_function("require_broken_fails"),
            "broken module load fails and caches a negative entry");
  ctx.check(sc::call_script_function("require_all_but_one"),
            "modules 1..capacity-1 load beside the negative entry (full)");
  ctx.check(write_file_at(kExtraPath, "return { id = 99 }\n"), "write extra");
  ctx.check(sc::call_script_function("require_extra_ok"),
            "a new loadable module evicts the negative entry");
  ctx.check(sc::call_script_function("require_all_but_one"),
            "loaded modules survive the eviction");
  ctx.check(sc::call_script_function("require_broken_fails_again"),
            "the evicted broken path still fails cleanly at capacity");
}

int g_refusalLines = 0;

/// Counts the Error lines that name the script past capacity.
void count_refusals(engine::core::LogLevel level, const char * /*channel*/,
                    const char *message, void * /*userData*/) noexcept {
  if ((level == engine::core::LogLevel::Error) && (message != nullptr) &&
      (std::strstr(message, "cache_entity_1025.lua") != nullptr)) {
    ++g_refusalLines;
  }
}

/// Attaches `total` distinct scripts to as many entities in a fresh World,
/// runs one begin-play and one tick dispatch, and checks that the first
/// `ran` scripts each received one of each hook and the rest none. Then
/// runs `extraFrames` more frames, so a refusal can show it is not
/// repeated.
bool run_dispatch_round(engine::core::ServiceLocator &serviceLocator, int total,
                        int ran, int extraFrames = 0) noexcept {
  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    return false;
  }
  rt::bind_scripting_runtime(world.get(), serviceLocator);
  sc::clear_entity_script_modules();
  bool ok = sc::call_script_function("reset_hook_counts");
  for (int i = 1; ok && (i <= total); ++i) {
    const rt::Entity entity = world->create_entity();
    rt::ScriptComponent script{};
    char name[64] = {};
    entity_script_name(i, name);
    std::snprintf(script.behaviours[0].scriptPath, sizeof(script.behaviours[0].scriptPath), "%s", name);
    ok = (entity != rt::kInvalidEntity) &&
         world->add_script_component(entity, script);
  }
  if (ok) {
    sc::dispatch_entity_scripts_begin_play(world.get());
    sc::dispatch_entity_scripts_update(1.0F / 60.0F);
    ok =
        sc::call_script_function_float("expect_ran", static_cast<float>(ran)) &&
        sc::call_script_function_float("expect_total",
                                       static_cast<float>(total)) &&
        sc::call_script_function("check_hooks");
  }
  for (int frame = 0; ok && (frame < extraFrames); ++frame) {
    sc::dispatch_entity_scripts_begin_play(world.get());
    sc::dispatch_entity_scripts_update(1.0F / 60.0F);
  }
  sc::clear_entity_script_modules();
  return ok;
}

/// Every distinct entity script up to the table's capacity loads and runs
/// through the production dispatch; one past it only that entity is left
/// out, and every other one still runs.
void test_distinct_entity_scripts(engine::tests::TestContext &ctx,
                                  engine::core::ServiceLocator &locator) {
  ctx.check(run_dispatch_round(locator, 0, 0), "no scripted entities");
  ctx.check(run_dispatch_round(locator, 1, 1), "one scripted entity runs");
  ctx.check(run_dispatch_round(locator, 33, 33),
            "33 distinct scripts all run (the old table held 32)");
  ctx.check(run_dispatch_round(locator, 100, 100),
            "100 distinct scripts all run");
  ctx.check(run_dispatch_round(locator, kCacheCapacity, kCacheCapacity),
            "a distinct script per table slot all run");
  g_refusalLines = 0;
  ctx.check(engine::core::log_register_sink(&count_refusals, nullptr),
            "log sink");
  ctx.check(run_dispatch_round(locator, kDispatchFiles, kCacheCapacity, 3),
            "one script past capacity is left out and the rest run");
  engine::core::log_unregister_sink(&count_refusals, nullptr);
  ctx.check(g_refusalLines == 1,
            "the refused script is named in one Error line over four frames");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_logging()) {
    std::fprintf(stderr, "FAIL: initialize_logging\n");
    return 1;
  }
  if (!sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    engine::core::shutdown_logging();
    return 1;
  }

  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    sc::shutdown_scripting();
    engine::core::shutdown_logging();
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);

  engine::tests::TestContext ctx;
  const bool filesOk = write_module_files(kCacheCapacity + 1);
  ctx.check(filesOk, "write module files");

  char driver[4096] = {};
  char pastCapacity[64] = {};
  module_name(kCacheCapacity + 1, pastCapacity);
  std::snprintf(driver, sizeof(driver),
                "%s"
                "function require_all() require_range(1, %d) end\n"
                "function require_all_but_one() require_range(1, %d) end\n"
                "function require_past_capacity_fails()\n"
                "    require_one_fails('%s')\n"
                "end\n"
                "function require_broken_fails()\n"
                "    require_one_fails('%s')\n"
                "end\n"
                "function require_broken_fails_again()\n"
                "    require_one_fails('%s')\n"
                "end\n"
                "function require_extra_ok() require_one_ok('%s') end\n",
                kDriver, kCacheCapacity, kCacheCapacity - 1, pastCapacity,
                kBrokenPath, kBrokenPath, kExtraPath);
  ctx.check(write_file_at(kDriverPath, driver), "write driver");
  ctx.check(sc::load_script(kDriverPath), "load driver");

  if (filesOk) {
    test_capacity_boundary(ctx);
    test_negative_entry_eviction(ctx);
  }
  const bool entityFilesOk = write_entity_script_files(kDispatchFiles);
  ctx.check(entityFilesOk, "write entity script files");
  if (entityFilesOk) {
    test_distinct_entity_scripts(ctx, serviceLocator);
    rt::bind_scripting_runtime(world.get(), serviceLocator);
  }

  sc::clear_entity_script_modules();
  sc::shutdown_scripting();
  remove_test_files(kCacheCapacity + 1);
  engine::core::shutdown_logging();
  return ctx.finish("script_module_cache");
}
