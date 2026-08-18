// Characterization for #168 M3: the Lua game bindings act on a
// pipeline-owned GameBindingState while one is bound and fall back to a
// scripting-local instance when unbound (standalone/test use). Pins the
// instance-isolation contract the ownership move introduces: a bound
// instance receives every write, the fallback is untouched while bound,
// and unbinding restores the fallback's values unchanged.

#include "engine/runtime/game_binding_state.h"
#include "engine/scripting/bindable_api.h"
#include "engine/scripting/scripting.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

bool state_is(const char *expected) noexcept {
  const char *actual = engine::scripting::bindable_get_game_state();
  return (actual != nullptr) && (std::strcmp(actual, expected) == 0);
}

} // namespace

/// Runs this executable or test program.
int main() {
  using engine::runtime::GameBindingState;
  using engine::scripting::bind_game_state;
  using engine::scripting::bindable_set_game_state;

  // Unbound: the fallback instance serves reads and writes.
  CHECK(state_is("startup"), "fallback starts at the default label");
  CHECK(bindable_set_game_state("fallback_value"), "fallback accepts writes");
  CHECK(state_is("fallback_value"), "fallback holds the written label");

  // Bound: reads switch to the fresh instance; writes land in it, and the
  // fallback keeps its value untouched.
  GameBindingState owned{};
  bind_game_state(&owned);
  CHECK(state_is("startup"), "bound instance starts at its own default");
  CHECK(bindable_set_game_state("owned_value"), "bound instance accepts writes");
  CHECK(std::strcmp(owned.gameState, "owned_value") == 0,
        "write landed in the bound instance");

  // Unbound again: the fallback's earlier value is exactly as it was.
  bind_game_state(nullptr);
  CHECK(state_is("fallback_value"),
        "unbind restores the untouched fallback value");

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("game_binding_state_test passed");
  return 0;
}
