// Integration tests for Lua sandboxing (P1-M2-G2).
// Tests: restricted globals, CPU instruction limit, memory limit.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

static const char *kTempScript = "sandbox_test.lua";

/// Writes script data.
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
// 1. io / os are NOT available in the sandbox
// -----------------------------------------------------------------------
bool test_io_blocked() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::core::ServiceLocator serviceLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);

  engine::scripting::set_sandbox_enabled(true);

  const char *code = "-- Try to access io.open — should fail.\n"
                     "local ok, err = pcall(function()\n"
                     "  local f = io.open('test.txt', 'w')\n"
                     "end)\n"
                     "if ok then\n"
                     "  error('io.open should not be available')\n"
                     "end\n"
                     "\n"
                     "-- Try to access os.execute — should fail.\n"
                     "local ok2, err2 = pcall(function()\n"
                     "  os.execute('echo hello')\n"
                     "end)\n"
                     "if ok2 then\n"
                     "  error('os.execute should not be available')\n"
                     "end\n";

  if (!write_script(code)) {
    return false;
  }

  // If the sandbox works, load_script succeeds (errors are caught by pcall
  // inside the script).
  bool result = engine::scripting::load_script(kTempScript);
  remove_script();
  engine::scripting::shutdown_scripting();
  return result;
}

// -----------------------------------------------------------------------
// 2. Safe globals (math, string, table, engine) ARE available
// -----------------------------------------------------------------------
bool test_safe_globals_available() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::core::ServiceLocator serviceLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);

  engine::scripting::set_sandbox_enabled(true);

  const char *code =
      "assert(type(math) == 'table', 'math missing')\n"
      "assert(type(string) == 'table', 'string missing')\n"
      "assert(type(table) == 'table', 'table missing')\n"
      "assert(type(pairs) == 'function', 'pairs missing')\n"
      "assert(type(ipairs) == 'function', 'ipairs missing')\n"
      "assert(type(tostring) == 'function', 'tostring missing')\n"
      "assert(type(tonumber) == 'function', 'tonumber missing')\n"
      "assert(type(pcall) == 'function', 'pcall missing')\n"
      "assert(type(error) == 'function', 'error missing')\n"
      "assert(type(engine) == 'table', 'engine missing')\n"
      "assert(type(coroutine) == 'table', 'coroutine missing')\n"
      "assert(math.sqrt(4) == 2, 'math.sqrt broken')\n";

  if (!write_script(code)) {
    return false;
  }

  bool result = engine::scripting::load_script(kTempScript);
  remove_script();
  engine::scripting::shutdown_scripting();
  return result;
}

// -----------------------------------------------------------------------
// 3. Infinite loop is terminated by instruction limit
// -----------------------------------------------------------------------
bool test_instruction_limit() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::core::ServiceLocator serviceLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);

  engine::scripting::set_sandbox_enabled(true);
  engine::scripting::set_instruction_limit(10000);

  const char *code = "-- Infinite loop — sandbox must terminate this.\n"
                     "while true do end\n";

  if (!write_script(code)) {
    return false;
  }

  // load_script should return false because pcall catches the hook error.
  bool loadOk = engine::scripting::load_script(kTempScript);
  remove_script();

  engine::scripting::set_instruction_limit(1000000);
  engine::scripting::shutdown_scripting();

  // The script should fail (infinite loop terminated).
  return !loadOk;
}

// -----------------------------------------------------------------------
// 3b. Coroutine threads inherit the instruction limit (hooks are
//     per-thread in Lua 5.4, so each resume must re-arm them)
// -----------------------------------------------------------------------
bool test_coroutine_instruction_limit() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::core::ServiceLocator serviceLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);

  engine::scripting::set_sandbox_enabled(true);
  engine::scripting::set_instruction_limit(10000);

  // A runaway coroutine body must be terminated on its first resume, making
  // start_coroutine return nil; without the per-thread hook this hangs.
  const char *code = "local id = engine.start_coroutine(function()\n"
                     "  while true do end\n"
                     "end)\n"
                     "if id ~= nil then\n"
                     "  error('runaway coroutine survived first resume')\n"
                     "end\n"
                     "resumed_id = engine.start_coroutine(function()\n"
                     "  engine.wait(0)\n"
                     "  while true do end\n"
                     "end)\n"
                     "if resumed_id == nil then\n"
                     "  error('yielding coroutine failed to start')\n"
                     "end\n";

  if (!write_script(code)) {
    return false;
  }

  bool result = engine::scripting::load_script(kTempScript);
  remove_script();

  // The second coroutine yields, then loops forever on its scheduler
  // resume: tick_coroutines must terminate it and return.
  engine::scripting::set_frame_time(1.0F, 1.0F);
  engine::scripting::tick_coroutines();

  engine::scripting::set_instruction_limit(1000000);
  engine::scripting::shutdown_scripting();
  return result;
}

// -----------------------------------------------------------------------
// 4. Memory limit: huge allocation fails
// -----------------------------------------------------------------------
bool test_memory_limit() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::core::ServiceLocator serviceLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);

  engine::scripting::set_sandbox_enabled(true);
  // Set a small memory limit (256KB).
  engine::scripting::set_memory_limit(256U * 1024U);

  const char *code = "-- Try to allocate a lot of memory.\n"
                     "local ok, err = pcall(function()\n"
                     "  local t = {}\n"
                     "  for i = 1, 1000000 do\n"
                     "    t[i] = string.rep('x', 1024)\n"
                     "  end\n"
                     "end)\n"
                     "if ok then\n"
                     "  error('memory limit should have triggered')\n"
                     "end\n";

  if (!write_script(code)) {
    return false;
  }

  bool result = engine::scripting::load_script(kTempScript);
  remove_script();

  engine::scripting::set_memory_limit(64U * 1024U * 1024U);
  engine::scripting::shutdown_scripting();
  return result;
}

// -----------------------------------------------------------------------
// 4b. S-4: enabling the sandbox AFTER initialize_scripting must install
//     the capping allocator on the live state, not just flip the flag
// -----------------------------------------------------------------------
bool test_memory_limit_enabled_post_init() noexcept {
  engine::scripting::set_sandbox_enabled(false);
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::core::ServiceLocator serviceLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);

  engine::scripting::set_sandbox_enabled(true);
  engine::scripting::set_memory_limit(256U * 1024U);

  const char *code = "local ok, err = pcall(function()\n"
                     "  local t = {}\n"
                     "  for i = 1, 1000000 do\n"
                     "    t[i] = string.rep('x', 1024)\n"
                     "  end\n"
                     "end)\n"
                     "if ok then\n"
                     "  error('memory limit should have triggered')\n"
                     "end\n";

  if (!write_script(code)) {
    return false;
  }

  bool result = engine::scripting::load_script(kTempScript);
  remove_script();

  engine::scripting::set_memory_limit(64U * 1024U * 1024U);
  engine::scripting::shutdown_scripting();
  return result;
}

// -----------------------------------------------------------------------
// 4c. S-4: allocator accounting is seeded from the live state's real size
//     when installed and cleared by shutdown, so a re-initialized session
//     starts from a clean, accurate baseline
// -----------------------------------------------------------------------
bool test_memory_accounting_lifecycle() noexcept {
  engine::scripting::set_sandbox_enabled(true);
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::core::ServiceLocator serviceLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);

  const std::size_t baseline = engine::scripting::get_memory_used();
  bool result = baseline > 0U;

  const char *code = "lifecycle_marker = string.rep('y', 4096)\n";
  result = result && write_script(code) &&
           engine::scripting::load_script(kTempScript);
  remove_script();
  result = result && (engine::scripting::get_memory_used() > baseline);

  engine::scripting::shutdown_scripting();
  result = result && (engine::scripting::get_memory_used() == 0U);

  engine::scripting::initialize_scripting();
  const std::size_t rebaseline = engine::scripting::get_memory_used();
  result = result && (rebaseline > 0U);
  const char *again = "lifecycle_marker2 = string.rep('z', 4096)\n";
  result = result && write_script(again) &&
           engine::scripting::load_script(kTempScript);
  remove_script();
  engine::scripting::shutdown_scripting();
  return result;
}

// -----------------------------------------------------------------------
// 5. debug library is NOT available
// -----------------------------------------------------------------------
bool test_debug_blocked() noexcept {
  engine::scripting::initialize_scripting();
  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }
  engine::core::ServiceLocator serviceLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);

  engine::scripting::set_sandbox_enabled(true);

  const char *code = "local ok, err = pcall(function()\n"
                     "  debug.getinfo(1)\n"
                     "end)\n"
                     "if ok then\n"
                     "  error('debug should not be available')\n"
                     "end\n";

  if (!write_script(code)) {
    return false;
  }

  bool result = engine::scripting::load_script(kTempScript);
  remove_script();
  engine::scripting::shutdown_scripting();
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int failures = 0;

  struct TestCase {
    const char *name;
    bool (*fn)();
  };

  const TestCase tests[] = {
      {"io_blocked", test_io_blocked},
      {"safe_globals_available", test_safe_globals_available},
      {"instruction_limit", test_instruction_limit},
      {"coroutine_instruction_limit", test_coroutine_instruction_limit},
      {"memory_limit", test_memory_limit},
      {"memory_limit_enabled_post_init", test_memory_limit_enabled_post_init},
      {"memory_accounting_lifecycle", test_memory_accounting_lifecycle},
      {"debug_blocked", test_debug_blocked},
  };

  for (const auto &tc : tests) {
    std::printf("  sandbox_test::%s ... ", tc.name);
    if (tc.fn()) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  return (failures == 0) ? 0 : 1;
}
