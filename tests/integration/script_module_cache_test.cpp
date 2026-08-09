// Boundary tests for the entity script module cache: capacity fills at 32
// loaded modules, the 33rd load fails cleanly with the engine still
// serving cached modules, and a never-loaded (negative) entry is evicted
// to make room for a new loadable module.

#include <cstdio>
#include <cstring>
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

constexpr int kCacheCapacity = 32;
constexpr const char *kDriverPath = "cache_driver.lua";
constexpr const char *kBrokenPath = "cache_broken.lua";
constexpr const char *kExtraPath = "cache_extra.lua";

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
  std::snprintf(out, sizeof(out), "cache_mod_%02d.lua", index);
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

/// Removes the numbered module files and the driver/broken/extra files.
void remove_test_files(int count) noexcept {
  for (int i = 1; i <= count; ++i) {
    char name[64] = {};
    module_name(i, name);
    static_cast<void>(std::remove(name));
  }
  static_cast<void>(std::remove(kDriverPath));
  static_cast<void>(std::remove(kBrokenPath));
  static_cast<void>(std::remove(kExtraPath));
}

constexpr const char *kDriver =
    "function require_range(first, last)\n"
    "    for i = first, last do\n"
    "        local name = string.format('cache_mod_%02d.lua', i)\n"
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
    "function require_one_ok(name)\n"
    "    if type(engine.require(name)) ~= 'table' then\n"
    "        error('expected table for ' .. name)\n"
    "    end\n"
    "end\n";

/// 32 modules load; the 33rd fails cleanly; cached entries keep serving.
void test_capacity_boundary(engine::tests::TestContext &ctx) {
  sc::clear_entity_script_modules();
  ctx.check(sc::call_script_function("require_all_32"),
            "modules 1..32 all load (cache fills to capacity)");
  ctx.check(sc::call_script_function("require_33_fails"),
            "module 33 is rejected at capacity");
  ctx.check(sc::call_script_function("require_all_32"),
            "all 32 cached modules still resolve after the rejection");
  ctx.check(sc::call_script_function("require_33_fails"),
            "repeated over-capacity loads keep failing cleanly");
}

/// A negative (never-loaded) entry is evicted to admit a loadable module.
void test_negative_entry_eviction(engine::tests::TestContext &ctx) {
  sc::clear_entity_script_modules();
  ctx.check(write_file_at(kBrokenPath, "this is not lua (("), "write broken");
  ctx.check(sc::call_script_function("require_broken_fails"),
            "broken module load fails and caches a negative entry");
  ctx.check(sc::call_script_function("require_first_31"),
            "modules 1..31 load beside the negative entry (cache full)");
  ctx.check(write_file_at(kExtraPath, "return { id = 99 }\n"), "write extra");
  ctx.check(sc::call_script_function("require_extra_ok"),
            "a new loadable module evicts the negative entry");
  ctx.check(sc::call_script_function("require_first_31"),
            "loaded modules survive the eviction");
  ctx.check(sc::call_script_function("require_broken_fails_again"),
            "the evicted broken path still fails cleanly at capacity");
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
  const bool filesOk = write_module_files(kCacheCapacity + 1);
  ctx.check(filesOk, "write module files");

  char driver[2048] = {};
  std::snprintf(driver, sizeof(driver),
                "%s"
                "function require_all_32() require_range(1, 32) end\n"
                "function require_first_31() require_range(1, 31) end\n"
                "function require_33_fails()\n"
                "    require_one_fails('cache_mod_33.lua')\n"
                "end\n"
                "function require_broken_fails()\n"
                "    require_one_fails('%s')\n"
                "end\n"
                "function require_broken_fails_again()\n"
                "    require_one_fails('%s')\n"
                "end\n"
                "function require_extra_ok() require_one_ok('%s') end\n",
                kDriver, kBrokenPath, kBrokenPath, kExtraPath);
  ctx.check(write_file_at(kDriverPath, driver), "write driver");
  ctx.check(sc::load_script(kDriverPath), "load driver");

  if (filesOk) {
    test_capacity_boundary(ctx);
    test_negative_entry_eviction(ctx);
  }

  sc::clear_entity_script_modules();
  sc::shutdown_scripting();
  remove_test_files(kCacheCapacity + 1);
  return ctx.finish("script_module_cache");
}
