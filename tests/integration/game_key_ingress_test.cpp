// Regression for issue #344 at the Lua ingress: the engine.game_state_* and
// engine.game_mode_set_rule bindings report the store's own verdict, so a
// key that cannot fit its fixed identity slot must come back false to the
// script instead of true-with-truncation — a script told "saved" for a key
// no lookup can ever match again is silent data loss. Drives the production
// bindings through a loaded chunk: 31-character keys round-trip exactly,
// 32-character keys are refused with no observable state change.

#include <cstdio>
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

constexpr const char *kDriverPath = "game_key_ingress_driver.lua";

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
  const char *p = contents;
  std::size_t len = 0U;
  while (p[len] != '\0') {
    ++len;
  }
  const bool ok = (std::fwrite(contents, 1U, len, f) == len);
  std::fclose(f);
  return ok;
}

// The driver derives both key lengths from the same 31-character base so the
// boundary stays pinned to kMaxKeyLength - 1 even if the constant's spelling
// here drifts; each function raises a Lua error on the first wrong verdict,
// which call_script_function reports as failure.
constexpr const char *kDriver =
    "local fits_state = string.rep('k', 31)\n"
    "local long_state = fits_state .. 'X'\n"
    "local fits_rule = string.rep('r', 31)\n"
    "local long_rule = fits_rule .. 'X'\n"
    "function state_key_boundary()\n"
    "    if not engine.game_state_set_number(fits_state, 7) then\n"
    "        error('31-char key rejected')\n"
    "    end\n"
    "    if engine.game_state_get_number(fits_state) ~= 7 then\n"
    "        error('31-char key not retrievable')\n"
    "    end\n"
    "    if engine.game_state_set_number(long_state, 1) then\n"
    "        error('32-char numeric key accepted')\n"
    "    end\n"
    "    if engine.game_state_set_string(long_state, 'v') then\n"
    "        error('32-char string key accepted')\n"
    "    end\n"
    "    if engine.game_state_has(long_state) then\n"
    "        error('rejected key reported present')\n"
    "    end\n"
    "    if engine.game_state_get_string(long_state) ~= nil then\n"
    "        error('rejected key returned a string')\n"
    "    end\n"
    "    if engine.game_state_get_number(fits_state) ~= 7 then\n"
    "        error('fitting key disturbed by rejections')\n"
    "    end\n"
    "end\n"
    "function rule_key_boundary()\n"
    "    if not engine.game_mode_set_rule(fits_rule, 'fits') then\n"
    "        error('31-char rule key rejected')\n"
    "    end\n"
    "    if engine.game_mode_get_rule(fits_rule) ~= 'fits' then\n"
    "        error('31-char rule not retrievable')\n"
    "    end\n"
    "    if engine.game_mode_set_rule(long_rule, 'x') then\n"
    "        error('32-char rule key accepted')\n"
    "    end\n"
    "    if engine.game_mode_get_rule(long_rule) ~= nil then\n"
    "        error('rejected rule key returned a value')\n"
    "    end\n"
    "    if engine.game_mode_get_rule(fits_rule) ~= 'fits' then\n"
    "        error('fitting rule disturbed by rejections')\n"
    "    end\n"
    "end\n";

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

  ctx.check(write_file_at(kDriverPath, kDriver), "write driver");
  ctx.check(sc::load_script(kDriverPath), "load driver");

  ctx.check(sc::call_script_function("state_key_boundary"),
            "game_state keys: 31 chars round-trip, 32 chars are refused");
  ctx.check(sc::call_script_function("rule_key_boundary"),
            "game_mode rules: 31 chars round-trip, 32 chars are refused");

  rt::unbind_scripting_runtime(serviceLocator);
  sc::shutdown_scripting();
  std::remove(kDriverPath);

  return ctx.finish("game key ingress tests");
}
