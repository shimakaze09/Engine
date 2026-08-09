// Verifies Lua allocator accounting: creation-time install (the GC baseline
// is inside the memory cap), exact grow/shrink/in-place/free accounting,
// cap rejection, zero-limit (unlimited) behavior, and wrap-safe arithmetic.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../test_harness.h"
#include "engine/scripting/scripting.h"
#include "lua_state.h"

namespace {

namespace sc = engine::scripting;

constexpr std::size_t kDefaultLimit = 64U * 1024U * 1024U;
constexpr const char *kScriptPath = "scripting_allocator_test.lua";

/// Writes the test script file.
bool write_script(const char *code) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, kScriptPath, "w") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(kScriptPath, "w");
  if (f == nullptr) {
    return false;
  }
#endif
  std::fputs(code, f);
  std::fclose(f);
  return true;
}

/// A cap far below the VM's GC baseline must fail initialization cleanly.
void test_tiny_cap_fails_initialization(engine::tests::TestContext &ctx) {
  sc::set_memory_limit(2048U);
  ctx.check(!sc::initialize_scripting(),
            "initialization fails under a 2KB memory cap");
  sc::set_memory_limit(kDefaultLimit);
  ctx.check(sc::initialize_scripting(),
            "initialization succeeds after the cap is restored");
  ctx.check(sc::get_memory_used() > 0U,
            "GC baseline is accounted after initialization");
  sc::shutdown_scripting();
  ctx.check(sc::get_memory_used() == 0U, "shutdown resets accounted bytes");
}

/// Accounting must run even when the sandbox is disabled before creation.
void test_accounting_with_sandbox_disabled(engine::tests::TestContext &ctx) {
  sc::set_sandbox_enabled(false);
  ctx.check(sc::initialize_scripting(), "initialization with sandbox off");
  ctx.check(sc::get_memory_used() > 0U,
            "allocations are accounted while the sandbox is disabled");
}

/// Exact grow/in-place/shrink/free accounting on the production allocator.
void test_direct_alloc_boundaries(engine::tests::TestContext &ctx) {
  const std::size_t before = sc::get_memory_used();
  void *grown = sc::scripting_lua_alloc(nullptr, nullptr, 7U, 1024U);
  ctx.check(grown != nullptr, "fresh 1KB allocation succeeds");
  ctx.check(sc::get_memory_used() == before + 1024U,
            "fresh allocation accounted exactly");

  void *samePtr = sc::scripting_lua_alloc(nullptr, grown, 1024U, 1024U);
  ctx.check(samePtr != nullptr, "same-size realloc succeeds");
  ctx.check(sc::get_memory_used() == before + 1024U,
            "same-size realloc leaves accounting unchanged");

  void *shrunk = sc::scripting_lua_alloc(nullptr, samePtr, 1024U, 256U);
  ctx.check(shrunk != nullptr, "shrink to 256 bytes succeeds");
  ctx.check(sc::get_memory_used() == before + 256U,
            "shrink returns the freed bytes exactly");

  void *freed = sc::scripting_lua_alloc(nullptr, shrunk, 256U, 0U);
  ctx.check(freed == nullptr, "free returns null");
  ctx.check(sc::get_memory_used() == before,
            "free returns accounting to the pre-allocation value");
}

/// An over-limit grow is rejected leaving accounting and the block intact.
void test_cap_rejection(engine::tests::TestContext &ctx) {
  sc::set_sandbox_enabled(true);
  void *block = sc::scripting_lua_alloc(nullptr, nullptr, 0U, 256U);
  ctx.check(block != nullptr, "pre-cap 256-byte allocation succeeds");
  const std::size_t used = sc::get_memory_used();
  sc::set_memory_limit(used + 128U);
  void *rejected = sc::scripting_lua_alloc(nullptr, block, 256U, 1024U);
  ctx.check(rejected == nullptr, "grow past the cap is rejected");
  ctx.check(sc::get_memory_used() == used,
            "rejected grow leaves accounting unchanged");
  sc::set_memory_limit(kDefaultLimit);
  static_cast<void>(sc::scripting_lua_alloc(nullptr, block, 256U, 0U));
}

/// Zero limit means unlimited for both direct and script-path allocations.
void test_zero_limit_unlimited(engine::tests::TestContext &ctx) {
  sc::set_memory_limit(0U);
  void *big =
      sc::scripting_lua_alloc(nullptr, nullptr, 0U, 4U * 1024U * 1024U);
  ctx.check(big != nullptr, "zero limit allows a 4MB allocation");
  static_cast<void>(
      sc::scripting_lua_alloc(nullptr, big, 4U * 1024U * 1024U, 0U));

  ctx.check(write_script("function alloc_big()\n"
                         "    big = string.rep('x', 1024 * 1024)\n"
                         "end\n"),
            "write zero-limit script");
  ctx.check(sc::load_script(kScriptPath), "load zero-limit script");
  ctx.check(sc::call_script_function("alloc_big"),
            "1MB script allocation succeeds under zero limit");
  sc::set_memory_limit(kDefaultLimit);
}

/// A near-SIZE_MAX request is rejected without corrupting the byte count.
void test_wrap_safe_request(engine::tests::TestContext &ctx) {
  const std::size_t used = sc::get_memory_used();
  void *huge = sc::scripting_lua_alloc(
      nullptr, nullptr, 0U, std::numeric_limits<std::size_t>::max() - 64U);
  ctx.check(huge == nullptr, "near-SIZE_MAX request is rejected");
  ctx.check(sc::get_memory_used() == used,
            "rejected huge request leaves accounting unchanged");
}

/// Memory released by the GC lowers the count without underflow.
void test_gc_shrink_path(engine::tests::TestContext &ctx) {
  ctx.check(write_script("function make_garbage()\n"
                         "    local t = {}\n"
                         "    for i = 1, 2000 do t[i] = 'chunk' .. i end\n"
                         "    scratch = t\n"
                         "end\n"
                         "function drop_garbage()\n"
                         "    scratch = nil\n"
                         "    collectgarbage('collect')\n"
                         "    collectgarbage('collect')\n"
                         "end\n"),
            "write GC shrink script");
  ctx.check(sc::load_script(kScriptPath), "load GC shrink script");
  const std::size_t beforeGarbage = sc::get_memory_used();
  ctx.check(sc::call_script_function("make_garbage"), "allocate garbage");
  const std::size_t peak = sc::get_memory_used();
  ctx.check(peak > beforeGarbage, "garbage allocation raises the count");
  ctx.check(sc::call_script_function("drop_garbage"), "collect garbage");
  ctx.check(sc::get_memory_used() < peak,
            "collection lowers the accounted count");
  ctx.check(sc::get_memory_used() > 0U,
            "collection does not underflow the accounted count");
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;

  test_tiny_cap_fails_initialization(ctx);
  test_accounting_with_sandbox_disabled(ctx);
  test_direct_alloc_boundaries(ctx);
  test_cap_rejection(ctx);
  test_zero_limit_unlimited(ctx);
  test_wrap_safe_request(ctx);
  test_gc_shrink_path(ctx);

  sc::set_memory_limit(kDefaultLimit);
  sc::set_sandbox_enabled(true);
  sc::shutdown_scripting();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("scripting_allocator");
}
