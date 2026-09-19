// Regression for #566: on a frame that simulates several fixed steps, the
// render-interpolation history must hold the pose one step behind the
// current one, not one frame behind. begin_update_step snapshotted the
// composed world transforms, which are only recomposed once per frame, so
// the "previous" sample on an N-step frame was N steps old while alpha
// blended a single step: the presented pose sat a full step behind. Driven
// through the production EnginePipeline, headless on the null render
// device, reading the step count and alpha the pipeline publishes in
// EngineStats and the history render prep interpolates from.

#include "engine/core/engine_stats.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace {

constexpr float kFixedDeltaSeconds = 1.0F / 60.0F;
// Constant x velocity under zero gravity: exactly one step of travel per
// fixed step, so the history-to-current gap counts the steps it spans.
constexpr float kVelocityX = 6.0F;
constexpr float kStepTravel = kVelocityX * kFixedDeltaSeconds;
// Eight sequential float additions of kStepTravel accumulate well under
// this; the defect's error is a whole step (0.1) per extra step.
constexpr float kTravelTolerance = 1.0e-4F;
// Past one fixed step so a playing frame simulates at least one; well past
// eight so the accumulator clamp yields exactly the maximum step count.
constexpr std::chrono::milliseconds kPastOneStep{20};
constexpr std::chrono::milliseconds kPastClamp{300};
constexpr std::uint32_t kClampedStepCount = 8U;

engine::runtime::World *g_world = nullptr;
bool g_paused = false;

void capture_world(engine::runtime::World *world) noexcept { g_world = world; }
bool bridge_is_playing() noexcept { return !g_paused; }
bool bridge_is_paused() noexcept { return g_paused; }

/// Walks upward from the current path until the bundled assets are found.
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

/// The history sample and the composed current pose after a frame.
struct Poses final {
  float previousX = 0.0F;
  float currentX = 0.0F;
  bool historyValid = false;
};

bool read_poses(engine::runtime::Entity body, Poses *out) noexcept {
  const engine::runtime::WorldTransform *current =
      g_world->get_world_transform_read_ptr(body);
  if (current == nullptr) {
    return false;
  }
  out->currentX = current->position.x;
  engine::math::Vec3 previousPosition{};
  engine::math::Quat previousRotation{};
  engine::math::Vec3 previousScale{};
  out->historyValid = g_world->get_previous_world_trs(
      body, &previousPosition, &previousRotation, &previousScale);
  out->previousX = previousPosition.x;
  return true;
}

/// Sleeps then runs one frame, reading back the poses and frame stats.
bool run_frame(engine::EnginePipeline &pipeline, engine::runtime::Entity body,
               std::chrono::milliseconds sleep, Poses *poses,
               engine::core::EngineStats *stats) noexcept {
  std::this_thread::sleep_for(sleep);
  if (!pipeline.execute_frame()) {
    return false;
  }
  *stats = engine::core::get_engine_stats();
  return read_poses(body, poses);
}

/// EXPECTATION: after a frame of N >= 1 steps the history is exactly one
/// step of travel behind the current pose, whatever N was.
bool check_history_one_step_behind(const Poses &poses,
                                   const engine::core::EngineStats &stats,
                                   const char *label) noexcept {
  if (stats.fixedSteps == 0U) {
    std::fprintf(stderr, "FAIL: %s frame simulated no steps\n", label);
    return false;
  }
  if (!poses.historyValid) {
    std::fprintf(stderr, "FAIL: %s frame left no history sample\n", label);
    return false;
  }
  const float gap = poses.currentX - poses.previousX;
  if (std::fabs(gap - kStepTravel) > kTravelTolerance) {
    std::fprintf(stderr,
                 "FAIL: %s frame ran %u steps; the history sits %.4f behind "
                 "the current pose, one step is %.4f (alpha %.3f)\n",
                 label, stats.fixedSteps, gap, kStepTravel,
                 stats.interpolationAlpha);
    return false;
  }
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: assets\n");
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    return 2;
  }

  int result = 0;
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline initialize\n");
      pipeline.teardown();
      engine::shutdown();
      return 3;
    }

    engine::runtime::set_gravity(*g_world, 0.0F, 0.0F, 0.0F);
    const engine::runtime::Entity body =
        g_world->create_scene_object(engine::runtime::Transform{});
    engine::runtime::RigidBody rigidBody{};
    rigidBody.velocity = engine::math::Vec3(kVelocityX, 0.0F, 0.0F);
    if ((body == engine::runtime::kInvalidEntity) ||
        !g_world->add_rigid_body(body, rigidBody)) {
      std::fprintf(stderr, "FAIL: authoring\n");
      result = 4;
    }

    Poses poses{};
    engine::core::EngineStats stats{};

    // Boundary: a single-step frame (the steady 60 Hz case) already held.
    if ((result == 0) &&
        (!run_frame(pipeline, body, kPastOneStep, &poses, &stats) ||
         !check_history_one_step_behind(poses, stats, "single-step"))) {
      result = (result == 0) ? 5 : result;
    }

    // The defect: a catch-up frame after a hitch, clamped to the maximum
    // step count. Its remaining accumulator is (near) zero, so alpha is
    // (near) zero and the presented pose is the history sample itself.
    if (result == 0) {
      const float before = poses.currentX;
      if (!run_frame(pipeline, body, kPastClamp, &poses, &stats)) {
        std::fprintf(stderr, "FAIL: catch-up frame\n");
        result = 6;
      } else if (stats.fixedSteps != kClampedStepCount) {
        std::fprintf(stderr, "FAIL: catch-up frame ran %u steps, clamp is %u\n",
                     stats.fixedSteps, kClampedStepCount);
        result = 7;
      } else if (std::fabs((poses.currentX - before) -
                           (static_cast<float>(kClampedStepCount) *
                            kStepTravel)) > kTravelTolerance) {
        std::fprintf(stderr, "FAIL: catch-up frame moved %.4f, expected %.4f\n",
                     poses.currentX - before,
                     static_cast<float>(kClampedStepCount) * kStepTravel);
        result = 8;
      } else if (!check_history_one_step_behind(poses, stats, "catch-up")) {
        result = 9;
      } else if (stats.interpolationAlpha > 1.0e-3F) {
        std::fprintf(stderr,
                     "FAIL: clamped frame kept alpha %.6f, expected ~0\n",
                     stats.interpolationAlpha);
        result = 10;
      }
    }

    // Boundary: a paused frame simulates no step, moves nothing, and
    // presents the current pose unblended (alpha 1).
    if (result == 0) {
      const float before = poses.currentX;
      g_paused = true;
      if (!run_frame(pipeline, body, kPastOneStep, &poses, &stats)) {
        std::fprintf(stderr, "FAIL: paused frame\n");
        result = 11;
      } else if ((stats.fixedSteps != 0U) || (poses.currentX != before) ||
                 (stats.interpolationAlpha != 1.0F)) {
        std::fprintf(stderr,
                     "FAIL: paused frame ran %u steps, moved %.4f, alpha %.3f\n",
                     stats.fixedSteps, poses.currentX - before,
                     stats.interpolationAlpha);
        result = 12;
      }
      g_paused = false;
    }

    pipeline.teardown();
  }

  engine::shutdown();
  if (result == 0) {
    std::puts("pipeline_interpolation_history_test passed");
  }
  return result;
}
