// Verifies that a script's randomness is the World's randomness: that
// engine.random draws from the World stream and advances it, that
// math.random is the same source rather than Lua's own generator, that a
// seed makes a script's draws reproducible, and that Lua's string-hash
// seed is pinned so table iteration order is the same in every process.
//
// The table-order case is why this is an integration test rather than a
// unit test of the bindings: iteration order is a property of the Lua
// state the engine builds, and the only honest way to observe it is to
// build that state and iterate a table.
//
// Scripts report through engine.log, captured by a log sink. The sandbox
// gives a script no filesystem (docs/decisions/0003), so the log is the
// only channel a script actually has, which makes it the right one to
// observe through rather than a back door the engine would never grant.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/rng.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kTempScript = "script_random_test.lua";

int g_failures = 0;

void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

char g_captured[4096];
std::size_t g_capturedLength;

void capture_script_log(engine::core::LogLevel, const char *channel,
                        const char *message, void *) noexcept {
  if ((channel == nullptr) || (message == nullptr) ||
      (std::strcmp(channel, "scripting") != 0)) {
    return;
  }
  const std::size_t length = std::strlen(message);
  if ((g_capturedLength + length + 2U) >= sizeof(g_captured)) {
    return;
  }
  std::memcpy(g_captured + g_capturedLength, message, length);
  g_capturedLength += length;
  g_captured[g_capturedLength++] = '\n';
  g_captured[g_capturedLength] = '\0';
}

void reset_capture() noexcept {
  g_capturedLength = 0U;
  g_captured[0] = '\0';
}

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

/// One scripting session bound to a fresh world, mirroring the fixture the
/// Lua hardening suite uses so both drive the production binding path.
struct ScriptingSession final {
  std::unique_ptr<engine::runtime::World> world;
  engine::core::ServiceLocator serviceLocator{};
  bool ok = false;

  ScriptingSession() noexcept {
    if (!engine::scripting::initialize_scripting()) {
      return;
    }
    world = std::unique_ptr<engine::runtime::World>(
        new (std::nothrow) engine::runtime::World());
    if (world == nullptr) {
      return;
    }
    engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);
    ok = true;
  }

  ~ScriptingSession() noexcept {
    engine::scripting::clear_entity_script_modules();
    engine::scripting::shutdown_scripting();
    std::remove(kTempScript);
  }
};

/// Runs `code` as the main script and reports whether it loaded.
bool run_script(const char *code) noexcept {
  return write_script(code) && engine::scripting::load_script(kTempScript);
}

/// Logs eight draws of `drawExpression`, one line each, into the capture.
bool run_drawing_script(const char *drawExpression) noexcept {
  char code[512] = {};
  std::snprintf(code, sizeof(code),
                "for i = 1, 8 do engine.log(tostring(%s)) end\n",
                drawExpression);
  return run_script(code);
}

/// Draws eight values from a seeded world through `drawExpression` and
/// copies the logged lines into `out`.
bool draw_with_seed(std::uint64_t seed, const char *drawExpression, char *out,
                    std::size_t capacity) noexcept {
  ScriptingSession session{};
  if (!session.ok) {
    return false;
  }
  session.world->seed_random(seed);
  reset_capture();
  if (!run_drawing_script(drawExpression)) {
    return false;
  }
  if (g_capturedLength == 0U) {
    return false;
  }
  std::snprintf(out, capacity, "%s", g_captured);
  return true;
}

/// EXPECTATION: engine.random draws from the World's stream, so one seed
/// gives one sequence and the World's own stream is left advanced.
void check_engine_random_is_the_world_stream() noexcept {
  char first[sizeof(g_captured)] = {};
  char second[sizeof(g_captured)] = {};
  char other[sizeof(g_captured)] = {};

  {
    ScriptingSession session{};
    check(session.ok, "the scripting session starts");
    if (!session.ok) {
      return;
    }
    session.world->seed_random(4242U);
    const engine::core::Rng before = session.world->random();
    reset_capture();
    check(run_drawing_script("engine.random_int(1, 1000000)"),
          "a script drawing from engine.random_int runs");
    check(g_capturedLength > 0U, "the script logged its draws");
    std::snprintf(first, sizeof(first), "%s", g_captured);
    // Drawing through Lua has to advance the World's own stream, not a
    // copy: a script whose randomness lived elsewhere would leave this
    // unchanged and fall outside state_hash.
    const engine::core::Rng after = session.world->random();
    check((before.state[0] != after.state[0]) ||
              (before.state[1] != after.state[1]),
          "a script's draw advances the World stream");
  }

  check(draw_with_seed(4242U, "engine.random_int(1, 1000000)", second,
                       sizeof(second)),
        "the same seed draws again in a fresh session");
  check(std::strcmp(first, second) == 0,
        "one seed gives one sequence of script draws");

  // A different seed must change the draws, or the check above would pass
  // on a stream that ignores its seed.
  check(draw_with_seed(4243U, "engine.random_int(1, 1000000)", other,
                       sizeof(other)),
        "a different seed draws");
  check(std::strcmp(first, other) != 0,
        "a different seed gives different script draws");
}

/// EXPECTATION: math.random is the engine stream, so a script using it is
/// as reproducible as one using engine.random. That is what makes the
/// alias worth having instead of a trap.
void check_math_random_is_the_same_source() noexcept {
  char viaMath[sizeof(g_captured)] = {};
  char viaEngine[sizeof(g_captured)] = {};

  check(draw_with_seed(99U, "math.random(1, 1000000)", viaMath,
                       sizeof(viaMath)),
        "a script drawing from math.random runs");
  check(draw_with_seed(99U, "engine.random_int(1, 1000000)", viaEngine,
                       sizeof(viaEngine)),
        "the engine.random_int comparison runs");
  check(std::strcmp(viaMath, viaEngine) == 0,
        "math.random draws the same values as engine.random_int");

  // math.randomseed reaches the same stream, so a script reseeding the
  // Lua way gets the engine's reproducibility rather than reseeding
  // nothing.
  ScriptingSession session{};
  if (!session.ok) {
    return;
  }
  session.world->seed_random(1U);
  reset_capture();
  check(run_script("math.randomseed(99)\n"
                   "for i = 1, 8 do\n"
                   "  engine.log(tostring(engine.random_int(1, 1000000)))\n"
                   "end\n"),
        "a script calling math.randomseed runs");
  check(std::strcmp(g_captured, viaEngine) == 0,
        "math.randomseed(n) seeds the engine stream");
}

/// EXPECTATION: the float form stays inside [0, 1) and the argument forms
/// respect their bounds, so the alias is faithful to what a script written
/// against Lua's manual expects.
void check_arities_and_bounds() noexcept {
  ScriptingSession session{};
  check(session.ok, "the scripting session starts");
  if (!session.ok) {
    return;
  }
  session.world->seed_random(7U);
  reset_capture();
  check(run_script(
            "local floatOk, oneOk, twoOk = true, true, true\n"
            "for i = 1, 500 do\n"
            "  local v = engine.random()\n"
            "  if v < 0.0 or v >= 1.0 then floatOk = false end\n"
            "  local m = engine.random(6)\n"
            "  if m < 1 or m > 6 or m ~= math.floor(m) then oneOk = false end\n"
            "  local n = engine.random(-3, 3)\n"
            "  if n < -3 or n > 3 then twoOk = false end\n"
            "end\n"
            "engine.log(tostring(floatOk) .. ' ' .. tostring(oneOk) .. ' ' ..\n"
            "           tostring(twoOk))\n"),
        "the arity script runs");
  check(std::strstr(g_captured, "true true true") != nullptr,
        "engine.random's three arities all respect their bounds");
}

/// EXPECTATION: Lua's string-hash seed is pinned, so a string-keyed table
/// iterates in the order the pinned seed produces — written down exactly,
/// the way the random stream's golden vector is.
///
/// The exact order is the whole check, and it has to be. Comparing two
/// Lua states inside one process does NOT detect an unpinned seed:
/// Lua's default mixes time(NULL) with the state pointer, and two states
/// built a moment apart in one process get the same second and usually
/// the same recycled address, so they agree with each other while
/// disagreeing with the next process. Measured: with the pin removed
/// this file's two-state comparison still passed while the order had
/// changed. A test that cannot fail is worse than no test (#575, #646),
/// so the order below is the assertion and the two-state comparison is
/// only a consistency check beside it.
///
/// If a Lua upgrade changes this string, that is a real signal: table
/// order is observable to scripts, and the change should be looked at
/// rather than pasted over.
void check_table_order_is_pinned() noexcept {
  constexpr const char *kGoldenOrder =
      "hotel,echo,charlie,foxtrot,bravo,golf,delta,alpha\n";

  const char *code = "local t = {}\n"
                     "local names = {'alpha', 'bravo', 'charlie', 'delta',\n"
                     "               'echo', 'foxtrot', 'golf', 'hotel'}\n"
                     "for i = 1, #names do t[names[i]] = i end\n"
                     "local order = {}\n"
                     "for k in pairs(t) do order[#order + 1] = k end\n"
                     "engine.log(table.concat(order, ','))\n";

  char firstOrder[sizeof(g_captured)] = {};
  char secondOrder[sizeof(g_captured)] = {};

  {
    ScriptingSession session{};
    check(session.ok, "the scripting session starts");
    if (!session.ok) {
      return;
    }
    reset_capture();
    check(run_script(code), "the first table-order script runs");
    std::snprintf(firstOrder, sizeof(firstOrder), "%s", g_captured);
  }
  {
    ScriptingSession session{};
    if (!session.ok) {
      return;
    }
    reset_capture();
    check(run_script(code), "the second table-order script runs");
    std::snprintf(secondOrder, sizeof(secondOrder), "%s", g_captured);
  }

  if (std::strcmp(firstOrder, kGoldenOrder) != 0) {
    std::fprintf(stderr, "  expected order %s  observed order %s",
                 kGoldenOrder, firstOrder);
  }
  check(std::strcmp(firstOrder, kGoldenOrder) == 0,
        "a string-keyed table iterates in the order the pinned seed gives");
  check(std::strcmp(firstOrder, secondOrder) == 0,
        "two Lua states in one process agree on that order");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_logging()) {
    return 2;
  }
  if (!engine::core::log_register_sink(&capture_script_log, nullptr)) {
    engine::core::shutdown_logging();
    return 3;
  }

  check_engine_random_is_the_world_stream();
  check_math_random_is_the_same_source();
  check_arities_and_bounds();
  check_table_order_is_pinned();

  engine::core::log_unregister_sink(&capture_script_log, nullptr);
  engine::core::shutdown_logging();

  if (g_failures != 0) {
    std::fprintf(stderr, "script_random_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("script_random_test: a script's randomness is the World's\n");
  return 0;
}
