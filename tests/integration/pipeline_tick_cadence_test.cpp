// Regression for issue #205 (the #176 follow-up): pins the on_tick cadence
// contract (EnginePipeline stage_timing's fixed-step decision wired to
// stage_scripting's dispatch) through the real production EnginePipeline,
// not a direct dispatch_entity_scripts_update call with a hand-computed dt.
// Drives engine::bootstrap() + EnginePipeline::execute_frame() the way
// pipeline_camera_prep_test.cpp does, controlling an EditorBridge to force
// paused, single-stepped, and multi-step catch-up frames, and reads results
// back from a shared Lua entity script through error()-raising verifier
// functions (the lua_lifecycle_test.cpp pattern). Frame time comes from the
// pipeline's frame delta override, never the wall clock, so every step
// count below is exact on every machine.

#include "engine/engine.h"
#include "engine/math/component_types.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <limits>

namespace {

constexpr float kFixedDeltaSeconds = 1.0F / 60.0F;
constexpr double kOneStepSeconds = 1.0 / 60.0;
// EnginePipeline::Impl's kMaxUpdateStepsPerFrame (runtime/src/engine_pipeline.cpp);
// not part of the public API, so the catch-up scenarios reach it by feeding
// a frame delta far past the clamp (kClampingFrameSeconds) rather than a
// delta that happens to equal it.
constexpr int kClampedStepCount = 8;
constexpr double kClampingFrameSeconds = 0.3;
constexpr const char *kScriptPath = "pipeline_tick_cadence_test.lua";

engine::runtime::World *g_world = nullptr;

/// Captures the pipeline's world so the test can author entities between frames.
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

// The test drives play state itself (paused/playing/single-step) instead of
// the fixed always-playing bridge pipeline_camera_prep_test.cpp uses.
bool g_paused = false;
bool g_stepArmed = false;

bool bridge_is_playing() noexcept { return !g_paused; }
bool bridge_is_paused() noexcept { return g_paused; }
bool bridge_consume_step_request() noexcept {
  const bool armed = g_stepArmed;
  g_stepArmed = false;
  return armed;
}

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Walks upward from the current path until the bundled assets are found
/// (same technique as pipeline_camera_prep_test.cpp).
bool set_working_directory_with_assets() noexcept {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};

  for (const std::filesystem::path &candidate : candidates) {
    std::error_code ec{};
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      continue;
    }

    if (std::filesystem::exists(normalized / "assets/main.lua", ec) &&
        std::filesystem::exists(normalized / "assets/shaders/bgfx/shaders.json",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }

  return false;
}

bool write_script_file(const char *contents) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::char_traits<char>::length(contents);
  const std::size_t written = std::fwrite(contents, 1U, length, file);
  std::fclose(file);
  return written == length;
}

void remove_script_file() noexcept { static_cast<void>(std::remove(kScriptPath)); }

// Role assignment is lazy and session-relative (reset via reset_tracking)
// rather than keyed by a Lua-encoded entity handle passed in from C++: the
// first entity to reach on_begin_play or on_tick after a reset becomes
// "primary", the second becomes "secondary". This lets the C++ side arm
// spawn/destroy/velocity mutations without ever needing to reconstruct a
// Lua entity handle itself.
constexpr const char *kScript =
    "local M = {}\n"
    "local total_ticks = 0\n"
    "local primary = nil\n"
    "local secondary = nil\n"
    "local primary_ticks = 0\n"
    "local secondary_ticks = 0\n"
    "local primary_begin_count = 0\n"
    "local secondary_begin_count = 0\n"
    "local last_dt = 0.0\n"
    "local end_play_count = 0\n"
    "local spawn_armed = false\n"
    "local spawned_ok = false\n"
    "local destroy_armed = false\n"
    "local velocity_armed = false\n"
    "local velocity_x = 0.0\n"
    "\n"
    "local function assign_role(self)\n"
    "    if primary == nil then\n"
    "        primary = self\n"
    "    elseif secondary == nil and self ~= primary then\n"
    "        secondary = self\n"
    "    end\n"
    "end\n"
    "\n"
    "function M.on_begin_play(self)\n"
    "    assign_role(self)\n"
    "    if self == primary then\n"
    "        primary_begin_count = primary_begin_count + 1\n"
    "    elseif self == secondary then\n"
    "        secondary_begin_count = secondary_begin_count + 1\n"
    "    end\n"
    "end\n"
    "\n"
    "function M.on_tick(self, dt)\n"
    "    assign_role(self)\n"
    "    total_ticks = total_ticks + 1\n"
    "    last_dt = dt\n"
    "    if self == primary then\n"
    "        primary_ticks = primary_ticks + 1\n"
    "    elseif self == secondary then\n"
    "        secondary_ticks = secondary_ticks + 1\n"
    "    end\n"
    "\n"
    "    if spawn_armed and self == primary then\n"
    "        spawn_armed = false\n"
    "        local child = engine.spawn_entity()\n"
    "        spawned_ok = (child ~= nil) and\n"
    "            engine.add_script_component(child, "
    "'pipeline_tick_cadence_test.lua')\n"
    "    end\n"
    "\n"
    "    if destroy_armed and self == primary and secondary ~= nil then\n"
    "        destroy_armed = false\n"
    "        engine.destroy_entity(secondary)\n"
    "    end\n"
    "\n"
    "    if velocity_armed and self == primary then\n"
    "        velocity_armed = false\n"
    "        engine.set_velocity(self, velocity_x, 0.0, 0.0)\n"
    "    end\n"
    "end\n"
    "\n"
    "function M.on_end_play(self)\n"
    "    end_play_count = end_play_count + 1\n"
    "end\n"
    "\n"
    "function reset_tracking()\n"
    "    total_ticks = 0\n"
    "    primary = nil\n"
    "    secondary = nil\n"
    "    primary_ticks = 0\n"
    "    secondary_ticks = 0\n"
    "    primary_begin_count = 0\n"
    "    secondary_begin_count = 0\n"
    "    last_dt = 0.0\n"
    "    end_play_count = 0\n"
    "    spawn_armed = false\n"
    "    spawned_ok = false\n"
    "    destroy_armed = false\n"
    "    velocity_armed = false\n"
    "    velocity_x = 0.0\n"
    "end\n"
    "\n"
    "function arm_spawn() spawn_armed = true end\n"
    "function arm_destroy() destroy_armed = true end\n"
    "function arm_velocity(vx) velocity_x = vx; velocity_armed = true end\n"
    "\n"
    "function verify_total_ticks(expected)\n"
    "    if total_ticks ~= expected then\n"
    "        error('total_ticks ' .. tostring(total_ticks) .. ' expected ' ..\n"
    "              tostring(expected))\n"
    "    end\n"
    "end\n"
    "\n"
    "function verify_last_dt(expected)\n"
    "    if last_dt ~= expected then\n"
    "        error('last_dt ' .. tostring(last_dt) .. ' expected ' ..\n"
    "              tostring(expected))\n"
    "    end\n"
    "end\n"
    "\n"
    "function verify_primary_ticks(expected)\n"
    "    if primary_ticks ~= expected then\n"
    "        error('primary_ticks ' .. tostring(primary_ticks) ..\n"
    "              ' expected ' .. tostring(expected))\n"
    "    end\n"
    "end\n"
    "\n"
    "function verify_secondary_ticks(expected)\n"
    "    if secondary_ticks ~= expected then\n"
    "        error('secondary_ticks ' .. tostring(secondary_ticks) ..\n"
    "              ' expected ' .. tostring(expected))\n"
    "    end\n"
    "end\n"
    "\n"
    "function verify_secondary_begin_count(expected)\n"
    "    if secondary_begin_count ~= expected then\n"
    "        error('secondary_begin_count ' .. tostring(secondary_begin_count)\n"
    "              .. ' expected ' .. tostring(expected))\n"
    "    end\n"
    "end\n"
    "\n"
    "function verify_end_play_count(expected)\n"
    "    if end_play_count ~= expected then\n"
    "        error('end_play_count ' .. tostring(end_play_count) ..\n"
    "              ' expected ' .. tostring(expected))\n"
    "    end\n"
    "end\n"
    "\n"
    "function verify_spawned_ok()\n"
    "    if not spawned_ok then\n"
    "        error('spawn did not succeed')\n"
    "    end\n"
    "end\n"
    "\n"
    "function verify_secondary_destroyed()\n"
    "    if secondary == nil then\n"
    "        error('secondary was never assigned a role')\n"
    "    end\n"
    "    if engine.is_alive(secondary) then\n"
    "        error('secondary is still alive')\n"
    "    end\n"
    "    if primary == nil or not engine.is_alive(primary) then\n"
    "        error('primary should still be alive')\n"
    "    end\n"
    "end\n"
    "\n"
    "return M\n";

bool reset_tracking() noexcept {
  return engine::scripting::call_script_function("reset_tracking");
}

/// Spawns a plain entity with the shared test script attached.
engine::runtime::Entity spawn_scripted_entity() noexcept {
  const engine::runtime::Entity entity = g_world->create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return engine::runtime::kInvalidEntity;
  }
  engine::runtime::ScriptComponent sc{};
  std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s", kScriptPath);
  if (!g_world->add_script_component(entity, sc)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

/// Runs one playing frame whose delta is exactly `seconds`, so the frame
/// simulates a known number of fixed steps regardless of how fast the
/// frames themselves run; on_tick dispatches whenever that number is
/// non-zero.
bool frame_with_delta(engine::EnginePipeline &pipeline,
                      double seconds) noexcept {
  return pipeline.set_frame_delta_override(seconds) &&
         pipeline.execute_frame();
}

/// Runs one playing frame that simulates exactly one fixed step.
bool ticking_frame(engine::EnginePipeline &pipeline) noexcept {
  return frame_with_delta(pipeline, kOneStepSeconds);
}

/// Runs a handful of ticking frames to settle any residual accumulator/
/// pending begin_play state before a scenario's own timing-sensitive frames.
bool settle_frames(engine::EnginePipeline &pipeline, int count) noexcept {
  for (int i = 0; i < count; ++i) {
    if (!ticking_frame(pipeline)) {
      return false;
    }
  }
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: locate demo assets\n");
    return 1;
  }

  remove_script_file();
  if (!write_script_file(kScript)) {
    std::fprintf(stderr, "FAIL: write script file\n");
    remove_script_file();
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  bridge.consume_step_request = &bridge_consume_step_request;
  engine::runtime::set_editor_bridge(&bridge);

  // Full production bootstrap in headless mode: the null render device
  // stands in so the cadence contract runs on every CI lane.
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    remove_script_file();
    return 2;
  }

  engine::EnginePipeline pipeline;
  if (!pipeline.initialize(0U)) {
    pipeline.teardown();
    engine::shutdown();
    remove_script_file();
    return 3;
  }

  if (g_world == nullptr) {
    pipeline.teardown();
    engine::shutdown();
    remove_script_file();
    return 4;
  }

  // Warm up: load the shared script module once via a real playing frame so
  // its global helper functions (reset_tracking, verify_*, arm_*) exist
  // before any scenario runs, including the ones that stay paused
  // throughout and would otherwise never trigger the module's first load.
  {
    g_paused = false;
    g_stepArmed = false;
    const engine::runtime::Entity warmup = spawn_scripted_entity();
    CHECK(warmup != engine::runtime::kInvalidEntity, "spawn warmup entity");
    CHECK(ticking_frame(pipeline), "warmup frame");
    engine::runtime::reset_world(*g_world);
  }

  std::printf("=== Pipeline Tick Cadence Integration Tests (audit #205) ===\n");

  // The override refuses values that cannot be a frame's length and leaves
  // the current source in place.
  CHECK(!pipeline.set_frame_delta_override(-1.0),
        "a negative frame delta override is refused");
  CHECK(!pipeline.set_frame_delta_override(
            std::numeric_limits<double>::quiet_NaN()),
        "a NaN frame delta override is refused");

  // --- Scenario 1: paused, no single-step -> no on_tick at all. ---
  {
    std::printf("  %-52s ", "paused frame dispatches no on_tick");
    engine::runtime::reset_world(*g_world);
    g_paused = false;
    g_stepArmed = false;
    CHECK(settle_frames(pipeline, 2), "settle before pausing");
    const engine::runtime::Entity primary = spawn_scripted_entity();
    CHECK(primary != engine::runtime::kInvalidEntity, "spawn primary");
    CHECK(reset_tracking(), "reset tracking");

    g_paused = true;
    for (int i = 0; i < 3; ++i) {
      CHECK(pipeline.execute_frame(), "paused execute_frame");
    }

    CHECK(engine::scripting::call_script_function_float("verify_total_ticks",
                                                         0.0F),
          "no on_tick fired while paused");
    if (g_failures == 0) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
    }
  }

  // --- Scenario 2: single-step -> exactly one on_tick with dt == one fixed
  // step, and the request is consumed (a second paused frame ticks none). ---
  {
    std::printf("  %-52s ", "single-step ticks once with one-step dt");
    const int before = g_failures;
    engine::runtime::reset_world(*g_world);
    g_paused = true;
    g_stepArmed = false;
    const engine::runtime::Entity primary = spawn_scripted_entity();
    CHECK(primary != engine::runtime::kInvalidEntity, "spawn primary");
    CHECK(reset_tracking(), "reset tracking");

    // Let begin_play land on an ordinary paused frame first so the
    // single-step frame's dispatch is on_tick only, matching the editor's
    // real Step-button flow over an already-loaded scene.
    CHECK(pipeline.execute_frame(), "paused frame before stepping");
    CHECK(reset_tracking(), "reset tracking after begin_play settle");

    g_stepArmed = true;
    CHECK(pipeline.execute_frame(), "single-step execute_frame");

    CHECK(engine::scripting::call_script_function_float("verify_total_ticks",
                                                         1.0F),
          "single-step fires exactly one on_tick");
    CHECK(engine::scripting::call_script_function_float("verify_last_dt",
                                                         kFixedDeltaSeconds),
          "single-step dt equals exactly one fixed step");

    // The step request is one-shot: a following paused frame ticks nothing.
    CHECK(reset_tracking(), "reset tracking before follow-up paused frame");
    CHECK(pipeline.execute_frame(), "follow-up paused frame");
    CHECK(engine::scripting::call_script_function_float("verify_total_ticks",
                                                         0.0F),
          "step request does not repeat on the next paused frame");

    std::printf("%s\n", g_failures == before ? "PASS" : "FAIL");
  }

  // --- Scenario 3: a frame delta long enough to clamp the fixed-step count
  // dispatches on_tick exactly once, with dt equal to the summed clamped
  // steps -- proving stage_timing's step-count decision and
  // stage_scripting's dispatch stay wired together end to end, not just
  // individually correct. ---
  {
    std::printf("  %-52s ", "clamped catch-up: one tick, summed dt");
    const int before = g_failures;
    engine::runtime::reset_world(*g_world);
    g_paused = false;
    g_stepArmed = false;
    const engine::runtime::Entity primary = spawn_scripted_entity();
    CHECK(primary != engine::runtime::kInvalidEntity, "spawn primary");
    CHECK(settle_frames(pipeline, 2), "settle before the catch-up frame");
    CHECK(reset_tracking(), "reset tracking");

    CHECK(frame_with_delta(pipeline, kClampingFrameSeconds),
          "catch-up execute_frame");

    CHECK(engine::scripting::call_script_function_float("verify_total_ticks",
                                                         1.0F),
          "one dispatch regardless of how many steps were folded in");
    const float expectedDt = static_cast<float>(
        static_cast<double>(kClampedStepCount) * kOneStepSeconds);
    CHECK(engine::scripting::call_script_function_float("verify_last_dt",
                                                         expectedDt),
          "dt equals the clamped step count times the fixed delta");

    std::printf("%s\n", g_failures == before ? "PASS" : "FAIL");
  }

  // --- Scenario 3b: a frame delta of three steps, below the clamp, still
  // dispatches once and hands the script exactly three steps of time: the
  // step count follows the delta, not the clamp or the frame rate. ---
  {
    std::printf("  %-52s ", "three-step frame: one tick, three-step dt");
    const int before = g_failures;
    engine::runtime::reset_world(*g_world);
    g_paused = false;
    g_stepArmed = false;
    const engine::runtime::Entity primary = spawn_scripted_entity();
    CHECK(primary != engine::runtime::kInvalidEntity, "spawn primary");
    CHECK(settle_frames(pipeline, 2), "settle before the three-step frame");
    CHECK(reset_tracking(), "reset tracking");

    CHECK(frame_with_delta(pipeline, 3.0 * kOneStepSeconds),
          "three-step execute_frame");
    CHECK(engine::scripting::call_script_function_float("verify_total_ticks",
                                                         1.0F),
          "one dispatch for the three-step frame");
    CHECK(engine::scripting::call_script_function_float(
              "verify_last_dt", static_cast<float>(3.0 * kOneStepSeconds)),
          "dt equals exactly three fixed steps");

    // The following one-step frame ticks exactly once more: the three-step
    // frame left nothing in the accumulator to spill over.
    CHECK(ticking_frame(pipeline), "one-step frame after the three-step one");
    CHECK(engine::scripting::call_script_function_float("verify_total_ticks",
                                                         2.0F),
          "the next frame dispatches once");
    CHECK(engine::scripting::call_script_function_float("verify_last_dt",
                                                         kFixedDeltaSeconds),
          "the next frame's dt is exactly one fixed step");

    std::printf("%s\n", g_failures == before ? "PASS" : "FAIL");
  }

  // --- Scenario 4: spawning a ScriptComponent entity from inside on_tick is
  // safe and the spawned entity's own begin_play/on_tick only start on the
  // next frame, never the spawning frame. ---
  {
    std::printf("  %-52s ", "spawn during on_tick starts next frame");
    const int before = g_failures;
    engine::runtime::reset_world(*g_world);
    g_paused = false;
    g_stepArmed = false;
    const engine::runtime::Entity primary = spawn_scripted_entity();
    CHECK(primary != engine::runtime::kInvalidEntity, "spawn primary");
    CHECK(reset_tracking(), "reset tracking");
    CHECK(engine::scripting::call_script_function("arm_spawn"), "arm spawn");

    CHECK(ticking_frame(pipeline), "spawning frame");
    CHECK(engine::scripting::call_script_function("verify_spawned_ok"),
          "spawn succeeded");
    CHECK(engine::scripting::call_script_function_float(
              "verify_secondary_begin_count", 0.0F),
          "spawned child has not begun play yet on the spawning frame");
    CHECK(engine::scripting::call_script_function_float("verify_primary_ticks",
                                                         1.0F),
          "only the spawner ticked on the spawning frame");
    CHECK(engine::scripting::call_script_function_float("verify_total_ticks",
                                                         1.0F),
          "the newly spawned child did not tick the same frame it was created");

    CHECK(ticking_frame(pipeline), "next frame");
    CHECK(engine::scripting::call_script_function_float(
              "verify_secondary_begin_count", 1.0F),
          "spawned child begins play on the next frame");
    CHECK(engine::scripting::call_script_function_float("verify_primary_ticks",
                                                         2.0F),
          "spawner keeps ticking every frame");
    CHECK(engine::scripting::call_script_function_float(
              "verify_secondary_ticks", 1.0F),
          "spawned child ticks for the first time on the next frame");

    std::printf("%s\n", g_failures == before ? "PASS" : "FAIL");
  }

  // --- Scenario 5: destroying a ScriptComponent entity from inside another
  // entity's on_tick is safe (no crash/UAF), fires on_end_play exactly once,
  // and the destroyed entity never ticks again. ---
  {
    std::printf("  %-52s ", "destroy during on_tick is safe");
    const int before = g_failures;
    engine::runtime::reset_world(*g_world);
    g_paused = false;
    g_stepArmed = false;
    // Which of these two becomes Lua's "primary" vs "secondary" role is an
    // implementation detail of script dispatch order, not a contract this
    // test pins; the assertions below are role-relative (via Lua, which
    // knows its own role assignment), not tied to either specific handle.
    const engine::runtime::Entity entityA = spawn_scripted_entity();
    const engine::runtime::Entity entityB = spawn_scripted_entity();
    CHECK(entityA != engine::runtime::kInvalidEntity, "spawn entity A");
    CHECK(entityB != engine::runtime::kInvalidEntity, "spawn entity B");
    CHECK(reset_tracking(), "reset tracking");

    CHECK(ticking_frame(pipeline), "both entities begin play and tick");
    CHECK(engine::scripting::call_script_function_float(
              "verify_secondary_begin_count", 1.0F),
          "secondary begins play");

    CHECK(engine::scripting::call_script_function("arm_destroy"),
          "arm destroy");
    CHECK(ticking_frame(pipeline), "destroy frame");
    CHECK(engine::scripting::call_script_function("verify_secondary_destroyed"),
          "secondary is destroyed, primary is still alive");
    CHECK(engine::scripting::call_script_function_float("verify_end_play_count",
                                                         1.0F),
          "secondary's on_end_play fired exactly once");

    // Reading tick counts before the follow-up frame so the assertion below
    // is a delta, not an absolute (the destroy frame's own dispatch order
    // between primary and secondary is not part of this contract).
    CHECK(reset_tracking(), "reset tracking before the follow-up frame");
    CHECK(ticking_frame(pipeline), "follow-up frame after destroy");
    CHECK(engine::scripting::call_script_function_float("verify_primary_ticks",
                                                         1.0F),
          "the surviving entity keeps ticking normally");
    CHECK(engine::scripting::call_script_function_float(
              "verify_secondary_ticks", 0.0F),
          "the destroyed entity never ticks again");

    std::printf("%s\n", g_failures == before ? "PASS" : "FAIL");
  }

  // --- Scenario 6: a script mutation issued from on_tick during a
  // multi-step catch-up frame is visible to every one of that
  // same frame's fixed physics steps (stage_scripting runs before
  // stage_simulation_graph each frame), not merely a future frame's. ---
  {
    std::printf("  %-52s ", "mutation in a catch-up tick reaches this frame's physics");
    const int before = g_failures;
    engine::runtime::reset_world(*g_world);
    g_paused = false;
    g_stepArmed = false;
    const engine::runtime::Entity primary = spawn_scripted_entity();
    CHECK(primary != engine::runtime::kInvalidEntity, "spawn primary");
    if (primary != engine::runtime::kInvalidEntity) {
      engine::math::RigidBody body{};
      body.velocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
      body.inverseMass = 1.0F;
      CHECK(g_world->add_rigid_body(primary, body), "add rigid body");
    }
    CHECK(settle_frames(pipeline, 2), "settle before the catch-up frame");

    engine::runtime::Transform beforeTransform{};
    CHECK(g_world->get_transform(primary, &beforeTransform),
          "read transform before mutation");

    CHECK(reset_tracking(), "reset tracking");
    constexpr float kVelocityX = 5.0F;
    CHECK(engine::scripting::call_script_function_float("arm_velocity",
                                                         kVelocityX),
          "arm velocity mutation");

    CHECK(frame_with_delta(pipeline, kClampingFrameSeconds),
          "catch-up frame with a velocity mutation");

    CHECK(engine::scripting::call_script_function_float("verify_total_ticks",
                                                         1.0F),
          "one dispatch for the whole clamped catch-up frame");

    engine::runtime::Transform afterTransform{};
    CHECK(g_world->get_transform(primary, &afterTransform),
          "read transform after mutation");

    const float expectedDisplacementX =
        kVelocityX * static_cast<float>(kClampedStepCount) * kFixedDeltaSeconds;
    const float actualDisplacementX =
        afterTransform.position.x - beforeTransform.position.x;
    // Plain float Euler integration over a small, fixed number of identical
    // steps; a tight absolute tolerance catches a wrong step count (an order
    // of magnitude off) while tolerating normal float accumulation error.
    constexpr float kToleranceX = 0.01F;
    CHECK((actualDisplacementX > (expectedDisplacementX - kToleranceX)) &&
              (actualDisplacementX < (expectedDisplacementX + kToleranceX)),
          "the mutated velocity drove every one of this frame's own fixed "
          "physics steps, not just a future frame's");

    std::printf("%s\n", g_failures == before ? "PASS" : "FAIL");
  }

  pipeline.teardown();
  engine::runtime::set_editor_bridge(nullptr);
  engine::shutdown();
  remove_script_file();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
