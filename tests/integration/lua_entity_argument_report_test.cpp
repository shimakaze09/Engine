// A binding handed an entity it cannot act on reports it. A script calls
// set_position, get_position, set_velocity, set_light_intensity and
// set_camera_component_priority on a destroyed entity's handle and on a
// handle from before a scene load. Each call still answers false or nil,
// and each logs one Warning naming the script, the line, the binding and
// the reason. A line that fails again does not log again, and a new run
// reports afresh. The bindings used to return quietly, leaving an author
// to bisect a script that did nothing.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "../test_harness.h"
#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr const char *kDriverPath = "entity_argument_driver.lua";

// Lines 9-13 act on the destroyed entity, lines 17-21 on the handle from
// before the scene load; the checks below name them.
constexpr const char *kDriver =
    "function make_targets()\n"        // 1
    "  dead = engine.spawn_entity()\n" // 2
    "  engine.destroy_entity(dead)\n"  // 3
    "  old = engine.spawn_entity()\n"  // 4
    "end\n"                            // 5
    "local function expect(ok) if not ok then error('a refused call "
    "answered') end end\n"                                            // 6
    "function call_dead()\n"                                          // 7
    "  local e = dead\n"                                              // 8
    "  expect(engine.set_position(e, 1, 2, 3) == false)\n"            // 9
    "  expect(engine.get_position(e) == nil)\n"                       // 10
    "  expect(engine.set_velocity(e, 0, 1, 0) == false)\n"            // 11
    "  expect(engine.set_light_intensity(e, 2) == false)\n"           // 12
    "  expect(engine.set_camera_component_priority(e, 3) == false)\n" // 13
    "end\n"                                                           // 14
    "function call_old()\n"                                           // 15
    "  local e = old\n"                                               // 16
    "  expect(engine.set_position(e, 1, 2, 3) == false)\n"            // 17
    "  expect(engine.get_position(e) == nil)\n"                       // 18
    "  expect(engine.set_velocity(e, 0, 1, 0) == false)\n"            // 19
    "  expect(engine.set_light_intensity(e, 2) == false)\n"           // 20
    "  expect(engine.set_camera_component_priority(e, 3) == false)\n" // 21
    "end\n";                                                          // 22

std::vector<std::string> g_warnings{};

void capture(engine::core::LogLevel level, const char * /*channel*/,
             const char *message, void * /*userData*/) noexcept {
  if ((level == engine::core::LogLevel::Warning) && (message != nullptr) &&
      (std::strstr(message, "the call does nothing") != nullptr)) {
    g_warnings.emplace_back(message);
  }
}

bool write_driver() {
  FILE *file = nullptr;
#ifdef _WIN32
  if ((fopen_s(&file, kDriverPath, "wb") != 0) || (file == nullptr)) {
    return false;
  }
#else
  file = std::fopen(kDriverPath, "wb");
  if (file == nullptr) {
    return false;
  }
#endif
  const std::size_t length = std::strlen(kDriver);
  const bool ok = std::fwrite(kDriver, 1U, length, file) == length;
  std::fclose(file);
  return ok;
}

/// True when exactly one captured warning names `line` with `binding`
/// and `reason`.
bool reported_once(int line, const char *binding, const char *reason) {
  char site[96] = {};
  std::snprintf(site, sizeof(site), "%s:%d: engine.%s: argument 1 ",
                kDriverPath, line, binding);
  int matches = 0;
  for (const std::string &warning : g_warnings) {
    if ((warning.find(site) != std::string::npos) &&
        (warning.find(reason) != std::string::npos)) {
      ++matches;
    }
  }
  return matches == 1;
}

void check_reports(engine::tests::TestContext &ctx, int firstLine,
                   const char *reason, const char *what) {
  const char *const bindings[] = {"set_position", "get_position",
                                  "set_velocity", "set_light_intensity",
                                  "set_camera_component_priority"};
  for (int i = 0; i < 5; ++i) {
    char label[192] = {};
    std::snprintf(label, sizeof(label), "%s: engine.%s reports line %d once",
                  what, bindings[i], firstLine + i);
    ctx.check(reported_once(firstLine + i, bindings[i], reason), label);
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_logging() || !sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize\n");
    return 1;
  }
  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);

  engine::tests::TestContext ctx;
  const bool loaded = write_driver() && sc::load_script(kDriverPath) &&
                      sc::call_script_function("make_targets");
  ctx.check(loaded, "driver loaded and targets made");
  if (loaded) {
    ctx.check(engine::core::log_register_sink(&capture, nullptr), "log sink");
    ctx.check(sc::call_script_function("call_dead"),
              "every call on a destroyed entity answers false or nil");
    // A scene load replaces the World's content; handles from before it
    // name nothing.
    world->mark_content_replaced(world->content_epoch());
    ctx.check(sc::call_script_function("call_old"),
              "every call on an old handle answers false or nil");
    check_reports(ctx, 9, "names an entity that was destroyed",
                  "a destroyed entity");
    check_reports(ctx, 17, "is a handle from before the last scene load",
                  "a handle from before a scene load");
    ctx.check(g_warnings.size() == 10U, "ten calls, ten reports");

    ctx.check(sc::call_script_function("call_dead") &&
                  sc::call_script_function("call_old") &&
                  (g_warnings.size() == 10U),
              "lines that fail again do not report again");
    sc::reset_run_state();
    ctx.check(sc::call_script_function("call_old") &&
                  (g_warnings.size() == 15U),
              "a new run reports its lines afresh");
    engine::core::log_unregister_sink(&capture, nullptr);
  }

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kDriverPath));
  engine::core::shutdown_logging();
  return ctx.finish("lua_entity_argument_report");
}
