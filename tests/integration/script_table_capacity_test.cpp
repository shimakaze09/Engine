// Boundary tests for the Lua-facing fixed tables: coroutines (1,024),
// global collision handlers (64) and entity pools (64). Each fills from
// empty through capacity, refuses the next request with nil and a reason
// a script can tell from a bad argument's, names the refusal in one
// Warning however often a script retries, and takes requests again once
// the table is cleared. The old sizes (32, 8 and 16) were met by normal
// games, and their refusals were silent.

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

constexpr const char *kDriverPath = "script_table_capacity_driver.lua";
constexpr int kCoroutines = 1024;
constexpr int kHandlers = 64;
constexpr int kPools = 64;

int g_fullWarnings = 0;

/// Counts the Warning lines that report a full table.
void count_full_warnings(engine::core::LogLevel level, const char * /*channel*/,
                         const char *message, void * /*userData*/) noexcept {
  if ((level == engine::core::LogLevel::Warning) && (message != nullptr) &&
      (std::strstr(message, "table full") != nullptr)) {
    ++g_fullWarnings;
  }
}

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

// Each fill function adds `count` entries to its table and raises unless
// every one is accepted; each refusal function raises unless the request
// returns nil and exactly the expected reason.
constexpr const char *kDriver =
    "count = 0\n"
    "function set_count(n) count = n end\n"
    "local function expect_refusal(id, why, want)\n"
    "  if id ~= nil or why ~= want then\n"
    "    error('got ' .. tostring(id) .. ', ' .. tostring(why))\n"
    "  end\n"
    "end\n"
    "function fill_coroutines()\n"
    "  for i = 1, count do\n"
    "    local id = engine.start_coroutine(function() engine.wait(1000) end)\n"
    "    if type(id) ~= 'number' then error('coroutine ' .. i) end\n"
    "  end\n"
    "end\n"
    "function coroutine_refused()\n"
    "  local id, why = engine.start_coroutine(function() end)\n"
    "  expect_refusal(id, why, 'coroutine table full (1024 running)')\n"
    "end\n"
    "function coroutine_bad_argument()\n"
    "  local id, why = engine.start_coroutine(42)\n"
    "  expect_refusal(id, why, 'start_coroutine expects a function')\n"
    "end\n"
    "function fill_handlers()\n"
    "  for i = 1, count do\n"
    "    local id = engine.on_collision_handler(function(a, b) end)\n"
    "    if type(id) ~= 'number' then error('handler ' .. i) end\n"
    "  end\n"
    "end\n"
    "function handler_refused()\n"
    "  local id, why = engine.on_collision_handler(function(a, b) end)\n"
    "  expect_refusal(id, why, 'collision handler table full (64 "
    "registered)')\n"
    "end\n"
    "function handler_bad_argument()\n"
    "  local id, why = engine.on_collision_handler('not a function')\n"
    "  expect_refusal(id, why, 'on_collision_handler expects a function')\n"
    "end\n"
    "function fill_pools()\n"
    "  for i = 1, count do\n"
    "    local id = engine.pool_create(1)\n"
    "    if type(id) ~= 'number' then error('pool ' .. i) end\n"
    "  end\n"
    "end\n"
    "function pool_refused()\n"
    "  local id, why = engine.pool_create(1)\n"
    "  expect_refusal(id, why, 'pool table full (64 pools)')\n"
    "end\n"
    "function pool_bad_argument()\n"
    "  local id, why = engine.pool_create(0)\n"
    "  expect_refusal(id, why, 'pool_create count must be 1 to 1024')\n"
    "  id, why = engine.pool_create('four')\n"
    "  expect_refusal(id, why, 'pool_create expects an integer count')\n"
    "end\n";

/// Fills a table to `count` entries through `fill`.
bool fill(const char *fill, int count) noexcept {
  return sc::call_script_function_float("set_count",
                                        static_cast<float>(count)) &&
         sc::call_script_function(fill);
}

/// Runs one table's boundary sequence: a bad argument on the empty table,
/// one entry, the table filled to `capacity`, three refusals that log one
/// Warning, a bad argument on the full table, and after `clear` one entry
/// again.
void check_table(engine::tests::TestContext &ctx, const char *name,
                 const char *fillFn, const char *refusedFn,
                 const char *badArgumentFn, int capacity,
                 void (*clear)() noexcept) {
  clear();
  char label[128] = {};
  std::snprintf(label, sizeof(label), "%s: a bad argument on the empty table",
                name);
  ctx.check(sc::call_script_function(badArgumentFn), label);
  std::snprintf(label, sizeof(label), "%s: one entry", name);
  ctx.check(fill(fillFn, 1), label);
  std::snprintf(label, sizeof(label), "%s: filled to capacity", name);
  ctx.check(fill(fillFn, capacity - 1), label);

  g_fullWarnings = 0;
  ctx.check(engine::core::log_register_sink(&count_full_warnings, nullptr),
            "log sink");
  std::snprintf(label, sizeof(label), "%s: one past capacity is refused", name);
  ctx.check(sc::call_script_function(refusedFn) &&
                sc::call_script_function(refusedFn) &&
                sc::call_script_function(refusedFn),
            label);
  engine::core::log_unregister_sink(&count_full_warnings, nullptr);
  std::snprintf(label, sizeof(label),
                "%s: three refusals log one Warning (got %d)", name,
                g_fullWarnings);
  ctx.check(g_fullWarnings == 1, label);
  std::snprintf(label, sizeof(label),
                "%s: a bad argument on the full table keeps its reason", name);
  ctx.check(sc::call_script_function(badArgumentFn), label);

  clear();
  std::snprintf(label, sizeof(label), "%s: the cleared table takes entries",
                name);
  ctx.check(fill(fillFn, 1), label);
  clear();
}

/// Drops every collision handler through the run reset that owns them.
void clear_handlers() noexcept { sc::reset_run_state(); }

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
  const bool loaded =
      write_file_at(kDriverPath, kDriver) && sc::load_script(kDriverPath);
  ctx.check(loaded, "load driver");
  if (loaded) {
    check_table(ctx, "coroutines", "fill_coroutines", "coroutine_refused",
                "coroutine_bad_argument", kCoroutines, &sc::clear_coroutines);
    check_table(ctx, "collision handlers", "fill_handlers", "handler_refused",
                "handler_bad_argument", kHandlers, &clear_handlers);
    check_table(ctx, "pools", "fill_pools", "pool_refused", "pool_bad_argument",
                kPools, &sc::clear_entity_pools);
  }

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kDriverPath));
  engine::core::shutdown_logging();
  return ctx.finish("script_table_capacity");
}
