// Integration tests for hot-reload with state preservation (P1-M2-G3).
// Tests: persist/restore API, reload with state survival, error recovery.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>


#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

static const char *kTempScript = "hotreload_test.lua";

bool write_script(const char *code) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, kTempScript, "w") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(kTempScript, "w");
  if (f == nullptr) {
    return false;
  }
#endif
  std::fputs(code, f);
  std::fclose(f);
  return true;
}

void remove_script() noexcept { std::remove(kTempScript); }

// -----------------------------------------------------------------------
// 1. engine.persist / engine.restore round-trip
// -----------------------------------------------------------------------
bool test_persist_restore() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::runtime::bind_scripting_runtime(world.get());

  const char *code =
      "engine.persist('health', 42)\n"
      "engine.persist('name', 'player1')\n"
      "engine.persist('alive', true)\n"
      "\n"
      "function check_persist()\n"
      "  local h = engine.restore('health')\n"
      "  local n = engine.restore('name')\n"
      "  local a = engine.restore('alive')\n"
      "  local missing = engine.restore('nonexistent')\n"
      "  if h ~= 42 then error('health mismatch: ' .. tostring(h)) end\n"
      "  if n ~= 'player1' then error('name mismatch') end\n"
      "  if a ~= true then error('alive mismatch') end\n"
      "  if missing ~= nil then error('missing should be nil') end\n"
      "end\n";

  if (!write_script(code)) {
    return false;
  }

  if (!engine::scripting::load_script(kTempScript)) {
    remove_script();
    engine::scripting::shutdown_scripting();
    return false;
  }

  bool result = engine::scripting::call_script_function("check_persist");
  remove_script();
  engine::scripting::shutdown_scripting();
  return result;
}

// -----------------------------------------------------------------------
// 2. State survives across a simulated reload
// -----------------------------------------------------------------------
bool test_state_survives_reload() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::runtime::bind_scripting_runtime(world.get());

  // First version: persist some state.
  const char *v1 = "engine.persist('score', 100)\n"
                   "g_version = 1\n";

  if (!write_script(v1) || !engine::scripting::load_script(kTempScript)) {
    remove_script();
    engine::scripting::shutdown_scripting();
    return false;
  }

  // Watch the script for reload.
  engine::scripting::watch_script_file(kTempScript);

  // "Modify" the script — second version restores and checks.
  // We need a small delay so mtime differs.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const char *v2 =
      "local s = engine.restore('score')\n"
      "if s ~= 100 then error('score not preserved: '..tostring(s)) end\n"
      "g_version = 2\n"
      "function get_version() return g_version end\n";

  if (!write_script(v2)) {
    remove_script();
    engine::scripting::shutdown_scripting();
    return false;
  }

  // Trigger reload check.
  engine::scripting::check_script_reload();

  // Verify the reload ran (g_version should be 2 now).
  bool result = engine::scripting::call_script_function("get_version");

  remove_script();
  engine::scripting::shutdown_scripting();
  return result;
}

// -----------------------------------------------------------------------
// 3. Error recovery: failed reload keeps old version
// -----------------------------------------------------------------------
bool test_error_recovery() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::runtime::bind_scripting_runtime(world.get());

  // First version: define a working function.
  const char *v1 = "function greet() return 'hello' end\n";

  if (!write_script(v1) || !engine::scripting::load_script(kTempScript)) {
    remove_script();
    engine::scripting::shutdown_scripting();
    return false;
  }

  engine::scripting::watch_script_file(kTempScript);

  // Verify v1 works.
  if (!engine::scripting::call_script_function("greet")) {
    remove_script();
    engine::scripting::shutdown_scripting();
    return false;
  }

  // Write a broken version.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const char *v2_bad = "this is not valid lua code!!!!\n";
  if (!write_script(v2_bad)) {
    remove_script();
    engine::scripting::shutdown_scripting();
    return false;
  }

  // Trigger reload — should fail but not crash.
  engine::scripting::check_script_reload();

  // Old function should still work after failed reload.
  bool result = engine::scripting::call_script_function("greet");

  remove_script();
  engine::scripting::shutdown_scripting();
  return result;
}

// -----------------------------------------------------------------------
// 4. Persist table values (number, string, boolean, table)
// -----------------------------------------------------------------------
bool test_persist_table() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::runtime::bind_scripting_runtime(world.get());

  const char *code =
      "engine.persist('data', {x=1, y=2, z=3})\n"
      "function check_table()\n"
      "  local d = engine.restore('data')\n"
      "  if type(d) ~= 'table' then error('not a table') end\n"
      "  if d.x ~= 1 or d.y ~= 2 or d.z ~= 3 then error('wrong values') end\n"
      "end\n";

  if (!write_script(code)) {
    return false;
  }

  if (!engine::scripting::load_script(kTempScript)) {
    remove_script();
    engine::scripting::shutdown_scripting();
    return false;
  }

  bool result = engine::scripting::call_script_function("check_table");
  remove_script();
  engine::scripting::shutdown_scripting();
  return result;
}

} // namespace

int main() {
  int failures = 0;

  struct TestCase {
    const char *name;
    bool (*fn)();
  };

  const TestCase tests[] = {
      {"persist_restore", test_persist_restore},
      {"state_survives_reload", test_state_survives_reload},
      {"error_recovery", test_error_recovery},
      {"persist_table", test_persist_table},
  };

  for (const auto &tc : tests) {
    std::printf("  hotreload_test::%s ... ", tc.name);
    if (tc.fn()) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  return (failures == 0) ? 0 : 1;
}
