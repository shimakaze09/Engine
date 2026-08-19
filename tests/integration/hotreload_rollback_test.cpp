// Integration tests for deep hot-reload rollback: a failed reload rolls
// back nested table state (identity-preserving, cycle-safe, depth-capped)
// through the production watch/check_script_reload path, and a later good
// reload still commits. Also covers the #115a metatable extension: a
// swapped metatable's identity, a shared metatable's own mutated field,
// and a metatable a failed reload added to a previously plain table all
// roll back. #199 extends coverage to closure upvalues: cells restore by
// lua_upvalueid identity (shared cells once, aliasing preserved), and an
// upvalue-only table's mutated fields roll back too.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr const char *kScriptPath = "hotreload_rollback_test.lua";

/// Writes the watched script file.
bool write_script(const char *code) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, kScriptPath, "wb") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(kScriptPath, "wb");
  if (f == nullptr) {
    return false;
  }
#endif
  const std::size_t len = std::strlen(code);
  const bool ok = (std::fwrite(code, 1U, len, f) == len);
  std::fclose(f);
  return ok;
}

/// Rewrites the watched script after an mtime-visible delay.
bool rewrite_script(const char *code) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return write_script(code);
}

constexpr const char *kV1 =
    "Config = { speed = 1, keep = 5, nested = { value = 1, tag = 'a' } }\n"
    "ConfigAlias = Config\n"
    "NestedAlias = Config.nested\n"
    "Cyc = {}\n"
    "Cyc.self = Cyc\n"
    "Cyc.marker = 'orig'\n"
    "deep = {}\n"
    "local t = deep\n"
    "for i = 1, 9 do t.n = {}; t = t.n end\n"
    "t.value = 1\n"
    "ClassMeta = { __index = { greet = function() return 'hi' end },\n"
    "              tag = 'meta-orig' }\n"
    "Instance = setmetatable({}, ClassMeta)\n"
    "Plain = {}\n"
    "local sharedCounter = 0\n"
    "function bump_shared() sharedCounter = sharedCounter + 1 "
    "return sharedCounter end\n"
    "function read_shared() return sharedCounter end\n"
    "local solo = 'orig-solo'\n"
    "function read_solo() return solo end\n"
    "function set_solo(v) solo = v end\n"
    "local hidden = { hp = 100 }\n"
    "function poke_hidden(v) hidden.hp = v end\n"
    "function read_hidden() return hidden.hp end\n"
    "function verify_upvalue_rollback()\n"
    "  if read_shared() ~= 0 then error('shared counter upvalue not rolled "
    "back') end\n"
    "  if read_solo() ~= 'orig-solo' then error('solo upvalue not rolled "
    "back') end\n"
    "  if read_hidden() ~= 100 then error('upvalue-only table field not "
    "rolled back') end\n"
    "  set_solo('after')\n"
    "  if read_solo() ~= 'after' then error('upvalue sharing split by "
    "rollback') end\n"
    "  bump_shared()\n"
    "  if read_shared() ~= 1 then error('counter sharing split by "
    "rollback') end\n"
    "end\n"
    "function verify_rollback()\n"
    "  if Config.speed ~= 1 then error('nested table field not rolled back') end\n"
    "  if Config.nested.value ~= 1 then error('deep field not rolled back') end\n"
    "  if Config.nested.tag ~= 'a' then error('deleted nested key not restored') end\n"
    "  if Config.added ~= nil then error('added nested key not removed') end\n"
    "  if Config.keep ~= 5 then error('deleted key not restored') end\n"
    "  if ConfigAlias ~= Config then error('table identity broken') end\n"
    "  if NestedAlias ~= Config.nested then error('nested identity broken') end\n"
    "  if Cyc.self ~= Cyc then error('cycle identity broken') end\n"
    "  if Cyc.marker ~= 'orig' then error('cyclic table field not rolled back') end\n"
    "  if deep.added ~= nil then error('depth-1 added key not removed') end\n"
    "  local t = deep\n"
    "  for i = 1, 9 do t = t.n end\n"
    "  if t.value ~= 777 then error('beyond-cap leaf should stay shared') end\n"
    "  if getmetatable(Instance) ~= ClassMeta then\n"
    "    error('swapped metatable identity not rolled back')\n"
    "  end\n"
    "  if ClassMeta.tag ~= 'meta-orig' then\n"
    "    error('shared metatable field not rolled back')\n"
    "  end\n"
    "  if Instance.greet == nil or Instance.greet() ~= 'hi' then\n"
    "    error('metatable __index dispatch not rolled back')\n"
    "  end\n"
    "  if getmetatable(Plain) ~= nil then\n"
    "    error('reload-added metatable on a previously plain table not '\n"
    "          .. 'cleared')\n"
    "  end\n"
    "end\n";

constexpr const char *kFailingV2 =
    "bump_shared() bump_shared() bump_shared()\n"
    "set_solo('mutated')\n"
    "poke_hidden(1)\n"
    "Config.speed = 99\n"
    "Config.nested.value = 999\n"
    "Config.nested.tag = nil\n"
    "Config.added = { oops = true }\n"
    "Config.keep = nil\n"
    "Cyc.marker = 'mutated'\n"
    "deep.added = 1\n"
    "local t = deep\n"
    "for i = 1, 9 do t = t.n end\n"
    "t.value = 777\n"
    "ClassMeta.tag = 'meta-mutated'\n"
    "setmetatable(Instance, { __index = { greet = function() return 'bye' end } })\n"
    "setmetatable(Plain, { evil = true })\n"
    "function verify_rollback() error('replacement function leaked') end\n"
    "error('intentional reload failure')\n";

constexpr const char *kGoodV3 =
    "Config = { speed = 3 }\n"
    "function verify_recovery()\n"
    "  if Config.speed ~= 3 then error('recovery reload did not commit') end\n"
    "end\n";

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
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

  ctx.check(write_script(kV1), "write v1");
  ctx.check(sc::load_script(kScriptPath), "load v1");
  sc::watch_script_file(kScriptPath);

  ctx.check(rewrite_script(kFailingV2), "write failing v2");
  sc::check_script_reload();
  ctx.check(sc::call_script_function("verify_rollback"),
            "failed reload rolls back nested tables in place");
  ctx.check(sc::call_script_function("verify_upvalue_rollback"),
            "failed reload rolls back closure upvalue cells (#199)");

  ctx.check(rewrite_script(kGoodV3), "write good v3");
  sc::check_script_reload();
  ctx.check(sc::call_script_function("verify_recovery"),
            "later good reload still commits after a rollback");

  static_cast<void>(std::remove(kScriptPath));
  sc::shutdown_scripting();
  return ctx.finish("hotreload_rollback");
}
