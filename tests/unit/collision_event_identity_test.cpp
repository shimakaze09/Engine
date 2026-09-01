// Regression for issue #395: collision events are pinned to the
// generation-bearing identities recorded at collision time. A handler that
// destroys a participant — with a spawn recycling its entity index before
// the next handler or pair runs — must never retarget the event onto the
// recycled replacement: later handlers receive nil for that participant.
// Every scenario drives the production wiring (physics step → frame
// accumulation → runtime::dispatch_collision_callbacks → Lua handlers).

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kTempScriptPath = "collision_event_identity_test.lua";
constexpr float kFixedDt = 1.0F / 60.0F;

/// Writes the scenario script; false on any short write.
bool write_script_file(const char *contents) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kTempScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kTempScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

void remove_script_file() noexcept {
  static_cast<void>(std::remove(kTempScriptPath));
}

/// One scenario's world + VM, torn down together so collision-handler
/// registrations can never leak between scenarios.
struct Scenario final {
  std::unique_ptr<engine::runtime::World> world{};
  engine::core::ServiceLocator locator{};
  bool ready = false;

  ~Scenario() {
    if (ready) {
      engine::runtime::unbind_scripting_runtime(locator);
      engine::scripting::shutdown_scripting();
    }
  }
};

/// Boots a VM and a World in its mutation phase, binds the production
/// bridge, installs the production collision dispatch, and runs the
/// script's on_start.
bool start_scenario(Scenario &s, const char *script) noexcept {
  if (!write_script_file(script) || !engine::scripting::initialize_scripting()) {
    return false;
  }
  s.world.reset(new (std::nothrow) engine::runtime::World());
  if (s.world == nullptr) {
    engine::scripting::shutdown_scripting();
    return false;
  }
  s.world->end_frame_phase();
  engine::runtime::bind_scripting_runtime(s.world.get(), s.locator);
  s.ready = true;
  engine::runtime::set_gravity(*s.world, 0.0F, 0.0F, 0.0F);
  engine::runtime::set_collision_dispatch(
      *s.world, &engine::scripting::dispatch_physics_callbacks);
  return engine::scripting::load_script(kTempScriptPath) &&
         engine::scripting::call_script_function("on_start");
}

/// Runs one rendered frame of `stepCount` fixed steps through the
/// production bridge sequence, then dispatches the collision callbacks in
/// the mutation phase, exactly as the pipeline's post-frame stage does.
[[nodiscard]] bool run_frame_and_dispatch(engine::runtime::World &world,
                                          std::size_t stepCount) noexcept {
  world.begin_update_phase();
  for (std::size_t step = 0U; step < stepCount; ++step) {
    if (step > 0U) {
      world.begin_update_step();
    }
    if (!world.update_transforms_range(0U, world.transform_count(),
                                       kFixedDt) ||
        !engine::runtime::step_physics(world, kFixedDt) ||
        !engine::runtime::resolve_collisions(world, kFixedDt)) {
      world.end_frame_phase();
      return false;
    }
    world.commit_update_phase();
  }
  world.begin_render_prep_phase();
  world.end_frame_phase();
  engine::runtime::dispatch_collision_callbacks(world);
  return true;
}

/// True when some alive entity carries exactly this name (scripts report
/// verdicts by naming marker entities).
[[nodiscard]] bool marker_exists(engine::runtime::World &world,
                                 const char *name) noexcept {
  bool found = false;
  world.for_each_alive([&](engine::runtime::Entity entity) noexcept {
    engine::runtime::NameComponent nc{};
    if (world.get_name_component(entity, &nc) &&
        (std::strcmp(nc.name, name) == 0)) {
      found = true;
    }
  });
  return found;
}

/// Asks the scenario script to publish its verdict marker, then checks it.
[[nodiscard]] bool report_says(Scenario &s, const char *expected) noexcept {
  if (!engine::scripting::call_script_function("report")) {
    return false;
  }
  return marker_exists(*s.world, expected);
}

// Scenario scenes place overlapping static colliders at the origin (and
// spawn_shape cubes for a second, spatially separate pair): Lua
// engine.set_position grants MovementAuthority::Script, which removes an
// entity from physics pair testing, so positions must come from the spawn
// itself.
// Shared Lua verdict helper: classifies one delivered participant slot
// against the recycled replacement. 'retargeted' is the #395 defect.
constexpr const char *kClassifyHelper =
    "function classify(x, y)\n"
    "    if replacement ~= nil and (x == replacement or y == replacement)\n"
    "    then\n"
    "        return 'retargeted'\n"
    "    end\n"
    "    if x == nil or y == nil then\n"
    "        return 'stale_nil'\n"
    "    end\n"
    "    return 'stale_handle'\n"
    "end\n"
    "function report()\n"
    "    local m = engine.spawn_entity()\n"
    "    engine.set_name(m, verdict)\n"
    "end\n";

/// Registered handler 1 destroys participant A and spawns a replacement
/// that recycles A's index; registered handler 2 for the SAME pair must
/// see nil for that slot, never the replacement. Also pins handler order:
/// registered handlers run in registration order, the legacy global last.
int check_registered_to_registered() noexcept {
  Scenario s{};
  std::string script =
      "verdict = 'unseen'\n"
      "order = ''\n"
      "function on_start()\n"
      "    a = engine.spawn_entity()\n"
      "    engine.add_collider(a, 0.5, 0.5, 0.5)\n"
      "    b = engine.spawn_entity()\n"
      "    engine.add_collider(b, 0.5, 0.5, 0.5)\n"
      "    engine.on_collision_handler(function(x, y)\n"
      "        order = order .. '1'\n"
      "        engine.destroy_entity(a)\n"
      "        replacement = engine.spawn_entity()\n"
      "    end)\n"
      "    engine.on_collision_handler(function(x, y)\n"
      "        order = order .. '2'\n"
      "        verdict = classify(x, y)\n"
      "    end)\n"
      "end\n"
      "function on_collision(x, y)\n"
      "    order = order .. 'g'\n"
      "end\n";
  script += kClassifyHelper;
  script += "function report_order()\n"
            "    local m = engine.spawn_entity()\n"
            "    engine.set_name(m, 'order_' .. order)\n"
            "end\n";
  if (!start_scenario(s, script.c_str())) {
    return 10;
  }
  if (!run_frame_and_dispatch(*s.world, 1U)) {
    return 11;
  }
  if (!report_says(s, "stale_nil")) {
    return marker_exists(*s.world, "retargeted") ? 12 : 13;
  }
  if (!engine::scripting::call_script_function("report_order") ||
      !marker_exists(*s.world, "order_12g")) {
    return 14;
  }
  return 0;
}

/// Same retarget scenario, but the second observer is the legacy global
/// on_collision fallback rather than a registered handler.
int check_registered_to_legacy_global() noexcept {
  Scenario s{};
  std::string script =
      "verdict = 'unseen'\n"
      "function on_start()\n"
      "    a = engine.spawn_entity()\n"
      "    engine.add_collider(a, 0.5, 0.5, 0.5)\n"
      "    b = engine.spawn_entity()\n"
      "    engine.add_collider(b, 0.5, 0.5, 0.5)\n"
      "    engine.on_collision_handler(function(x, y)\n"
      "        engine.destroy_entity(a)\n"
      "        replacement = engine.spawn_entity()\n"
      "    end)\n"
      "end\n"
      "function on_collision(x, y)\n"
      "    verdict = classify(x, y)\n"
      "end\n";
  script += kClassifyHelper;
  if (!start_scenario(s, script.c_str())) {
    return 20;
  }
  if (!run_frame_and_dispatch(*s.world, 1U)) {
    return 21;
  }
  return report_says(s, "stale_nil") ? 0 : 22;
}

/// A destroyed participant with NO index recycle arrives as nil while the
/// surviving participant keeps its valid handle.
int check_destroyed_without_recycle() noexcept {
  Scenario s{};
  std::string script =
      "verdict = 'unseen'\n"
      "function on_start()\n"
      "    a = engine.spawn_entity()\n"
      "    engine.add_collider(a, 0.5, 0.5, 0.5)\n"
      "    b = engine.spawn_entity()\n"
      "    engine.add_collider(b, 0.5, 0.5, 0.5)\n"
      "    engine.on_collision_handler(function(x, y)\n"
      "        engine.destroy_entity(b)\n"
      "    end)\n"
      "    engine.on_collision_handler(function(x, y)\n"
      "        if (x == a and y == nil) or (y == a and x == nil) then\n"
      "            verdict = 'survivor_kept'\n"
      "        else\n"
      "            verdict = 'wrong_slots'\n"
      "        end\n"
      "    end)\n"
      "end\n"
      "function report()\n"
      "    local m = engine.spawn_entity()\n"
      "    engine.set_name(m, verdict)\n"
      "end\n";
  if (!start_scenario(s, script.c_str())) {
    return 30;
  }
  if (!run_frame_and_dispatch(*s.world, 1U)) {
    return 31;
  }
  return report_says(s, "survivor_kept") ? 0 : 32;
}

/// Cross-pair recycle: the handler for the first pair destroys a later
/// pair's participant and recycles its index; the later pair's dispatch
/// must deliver nil, never the replacement. Two disjoint overlapping pairs
/// collide in the same step.
int check_cross_pair_recycle() noexcept {
  Scenario s{};
  std::string script =
      "verdict = 'unseen'\n"
      "acted = false\n"
      "function on_start()\n"
      "    a = engine.spawn_entity()\n"
      "    engine.add_collider(a, 0.5, 0.5, 0.5)\n"
      "    b = engine.spawn_entity()\n"
      "    engine.add_collider(b, 0.5, 0.5, 0.5)\n"
      "    c = engine.spawn_shape('cube', 10.0, 0.25, 0.0)\n"
      "    d = engine.spawn_shape('cube', 10.0, 0.0, 0.0)\n"
      "    engine.on_collision_handler(function(x, y)\n"
      "        if x == a or y == a or x == b or y == b then\n"
      "            if not acted then\n"
      "                acted = true\n"
      "                engine.destroy_entity(c)\n"
      "                replacement = engine.spawn_entity()\n"
      "            end\n"
      "        elseif acted then\n"
      "            verdict = classify(x, y)\n"
      "        end\n"
      "    end)\n"
      "end\n";
  script += kClassifyHelper;
  if (!start_scenario(s, script.c_str())) {
    return 40;
  }
  if (!run_frame_and_dispatch(*s.world, 1U)) {
    return 41;
  }
  // 'unseen' means the c/d pair dispatched before the a/b pair, which
  // would make the scenario vacuous rather than green — fail loudly.
  return report_says(s, "stale_nil") ? 0 : 42;
}

/// Catch-up frame: two fixed steps queue the same pair twice; the first
/// delivery's handler destroys A and recycles its index, so the second
/// delivery must arrive as nil, never as the replacement.
int check_catchup_second_delivery() noexcept {
  Scenario s{};
  std::string script =
      "verdict = 'unseen'\n"
      "deliveries = 0\n"
      "function on_start()\n"
      "    a = engine.spawn_entity()\n"
      "    engine.add_collider(a, 0.5, 0.5, 0.5)\n"
      "    b = engine.spawn_entity()\n"
      "    engine.add_collider(b, 0.5, 0.5, 0.5)\n"
      "    engine.on_collision_handler(function(x, y)\n"
      "        deliveries = deliveries + 1\n"
      "        if deliveries == 1 then\n"
      "            engine.destroy_entity(a)\n"
      "            replacement = engine.spawn_entity()\n"
      "        elseif deliveries == 2 then\n"
      "            verdict = classify(x, y)\n"
      "        end\n"
      "    end)\n"
      "end\n";
  script += kClassifyHelper;
  if (!start_scenario(s, script.c_str())) {
    return 50;
  }
  if (!run_frame_and_dispatch(*s.world, 2U)) {
    return 51;
  }
  return report_says(s, "stale_nil") ? 0 : 52;
}

} // namespace

/// Runs this executable or test program.
int main() {
  remove_script_file();
  static_cast<void>(engine::core::initialize_logging());

  struct Case final {
    const char *name;
    int (*run)() noexcept;
  };
  const Case cases[] = {
      {"registered handler cannot retarget the next registered handler",
       &check_registered_to_registered},
      {"registered handler cannot retarget the legacy global",
       &check_registered_to_legacy_global},
      {"destroyed participant arrives nil, survivor keeps its handle",
       &check_destroyed_without_recycle},
      {"first pair's handler cannot retarget a later pair",
       &check_cross_pair_recycle},
      {"catch-up frame's second delivery cannot be retargeted",
       &check_catchup_second_delivery},
  };

  int failures = 0;
  for (const Case &c : cases) {
    const int result = c.run();
    if (result != 0) {
      ++failures;
      std::fprintf(stderr, "FAIL(%d): %s\n", result, c.name);
    } else {
      std::printf("ok: %s\n", c.name);
    }
  }

  remove_script_file();
  if (failures != 0) {
    std::fprintf(stderr, "%d collision event identity case(s) failed\n",
                 failures);
    return 1;
  }
  std::printf("collision event identity tests passed\n");
  return 0;
}
