// Regression for issue #529: spring arms and the camera advance once per
// fixed step, and the two camera samples render prep blends are one step
// apart. On base the camera stage ran once per rendered frame with the
// frame's summed step time and pushed a sample every frame, so a follow
// camera sat at the latest step's pose while the body it followed was drawn
// interpolated — leading it by up to a whole step on any frame that did not
// run exactly one — and an arm's lag depended on how the steps were split
// into frames. Drives the production EnginePipeline headless.
//
// Both invariants hold whatever step counts the frames get. Each frame's
// delta is injected rather than measured, so the runs exercise exactly the
// step patterns they are written for, frames with no step and frames with
// several, on any machine: nothing here depends on timing.

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/job_system.h"
#include "engine/core/simulation_clock.h"
#include "engine/engine.h"
#include "engine/math/transform.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <system_error>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;

constexpr float kVelocityX = 6.0F;

World *g_world = nullptr;
float g_presentedCameraX = 0.0F;
bool g_presentedCameraSeen = false;

void capture_world(World *world) noexcept { g_world = world; }
bool always_playing() noexcept { return true; }
bool never_paused() noexcept { return false; }

/// The bridge's render callback runs between the flush and the present,
/// the only moment the active camera is the interpolated view the frame
/// was drawn with; afterwards the pipeline restores the newest sample.
void observe_presented_camera(float, float) noexcept {
  g_presentedCameraX = engine::renderer::get_active_camera().position.x;
  g_presentedCameraSeen = true;
}

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
    if (std::filesystem::exists(normalized / "assets/main.lua", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

/// A body moving at a constant velocity under no gravity, carrying the
/// authored third-person rig: a spring arm for the pose and a camera
/// component for the lens. The arm starts short of its length, so its lag
/// has something to integrate.
Entity build_rig(World &world) noexcept {
  engine::runtime::reset_world(world);
  engine::runtime::set_gravity(world, 0.0F, 0.0F, 0.0F);
  const Entity body = world.create_scene_object(engine::runtime::Transform{});
  engine::runtime::RigidBody rigidBody{};
  rigidBody.velocity = engine::math::Vec3(kVelocityX, 0.0F, 0.0F);
  engine::runtime::SpringArmComponent arm{};
  arm.armLength = 5.0F;
  arm.currentLength = 1.0F;
  arm.lagSpeed = 2.0F;
  arm.collisionEnabled = false;
  engine::runtime::CameraComponent camera{};
  camera.priority = 100.0F;
  camera.blendSpeed = 1000.0F;
  if ((body == kInvalidEntity) || !world.add_rigid_body(body, rigidBody) ||
      !world.add_spring_arm(body, arm) ||
      !world.add_camera_component(body, camera)) {
    return kInvalidEntity;
  }
  return body;
}

/// What one run records per frame.
struct RunResult final {
  bool ok = false;
  unsigned zeroStepFrames = 0U;
  unsigned multiStepFrames = 0U;
  /// Largest change of (presented camera x - presented body x) from its
  /// first value, in metres. The arm points along z, so that offset is a
  /// constant of the rig.
  double maxOffsetDrift = 0.0;
  /// Arm length after each total number of fixed steps.
  std::map<unsigned, float> armLengthAtStep;
};

/// Runs frames until `totalSteps` fixed steps have passed, each frame
/// accumulating `deltaFor(frameIndex)` fixed steps' worth of time.
template <typename DeltaFor>
RunResult run_rig(engine::EnginePipeline &pipeline, unsigned totalSteps,
                  DeltaFor &&deltaFor) noexcept {
  RunResult result{};
  const Entity body = build_rig(*g_world);
  if (body == kInvalidEntity) {
    return result;
  }
  // The first frames settle the camera manager onto the rig's camera, one
  // step each. The arm lags through those steps as through any other, so
  // the count the arm's length is keyed by starts at the rig's creation.
  unsigned steps = 0U;
  for (int frame = 0; frame < 4; ++frame) {
    if (!pipeline.set_frame_delta_override(engine::core::kFixedDeltaSeconds) ||
        !pipeline.execute_frame()) {
      pipeline.clear_frame_delta_override();
      return result;
    }
    steps += engine::core::get_engine_stats().fixedSteps;
  }
  totalSteps += steps;

  bool haveFirstOffset = false;
  double firstOffset = 0.0;
  for (unsigned frame = 0U; (steps < totalSteps) && (frame < 4000U); ++frame) {
    g_presentedCameraSeen = false;
    if (!pipeline.set_frame_delta_override(engine::core::kFixedDeltaSeconds *
                                           deltaFor(frame)) ||
        !pipeline.execute_frame()) {
      pipeline.clear_frame_delta_override();
      return result;
    }
    const engine::core::EngineStats stats = engine::core::get_engine_stats();
    steps += stats.fixedSteps;
    if (stats.fixedSteps == 0U) {
      ++result.zeroStepFrames;
    } else if (stats.fixedSteps >= 2U) {
      ++result.multiStepFrames;
    }

    const engine::runtime::WorldTransform *current =
        g_world->get_world_transform_read_ptr(body);
    engine::math::Vec3 previousPosition{};
    engine::math::Quat previousRotation{};
    engine::math::Vec3 previousScale{};
    if ((current == nullptr) || !g_presentedCameraSeen) {
      return result;
    }
    // Until a fixed step has run there is no pose behind the current one,
    // so there is nothing presented to compare yet.
    if (!g_world->get_previous_world_trs(body, &previousPosition,
                                         &previousRotation, &previousScale)) {
      continue;
    }
    const double alpha = stats.interpolationAlpha;
    const double presentedBodyX =
        previousPosition.x + (current->position.x - previousPosition.x) * alpha;
    const double offset = static_cast<double>(g_presentedCameraX) - presentedBodyX;
    if (!haveFirstOffset) {
      firstOffset = offset;
      haveFirstOffset = true;
    }
    const double drift = std::fabs(offset - firstOffset);
    if (drift > result.maxOffsetDrift) {
      result.maxOffsetDrift = drift;
    }

    engine::runtime::SpringArmComponent arm{};
    if (!g_world->get_spring_arm(body, &arm)) {
      return result;
    }
    result.armLengthAtStep[steps] = arm.currentLength;
  }
  pipeline.clear_frame_delta_override();
  result.ok = (steps >= totalSteps);
  return result;
}

/// Runs the three pacings on a fresh pipeline whose job system has
/// `workers` worker threads, and checks both invariants. The steps before
/// a frame's last evaluate the camera from inside the simulation graph, on
/// whichever thread runs the step job, so the result must not depend on
/// how many of those there are: none (the graph runs inline on the main
/// thread), one, or as many as the machine has.
int check_with_workers(std::uint32_t workers) noexcept;

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: assets\n");
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &always_playing;
  bridge.is_paused = &never_paused;
  bridge.render = &observe_presented_camera;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    return 2;
  }

  const std::uint32_t machineWorkers = engine::core::worker_count();
  int result = 0;
  for (const std::uint32_t workers : {0U, 1U, machineWorkers}) {
    const int outcome = check_with_workers(workers);
    if (outcome != 0) {
      result = outcome;
      break;
    }
  }
  engine::runtime::set_editor_bridge(nullptr);
  engine::shutdown();

  if (result == 0) {
    std::printf("pipeline_camera_step_cadence_test: all tests passed\n");
  }
  return result;
}

namespace {

int check_with_workers(std::uint32_t workers) noexcept {
  // bootstrap sized the job system from the hardware; the pipeline sizes
  // its per-thread storage from the job system when it initializes, so the
  // worker count is changed between the two.
  engine::core::shutdown_job_system();
  if (!engine::core::initialize_job_system(workers)) {
    std::fprintf(stderr, "FAIL: job system with %u workers\n", workers);
    return 6;
  }
  g_world = nullptr;
  int result = 0;
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline initialize (%u workers)\n",
                   workers);
      pipeline.teardown();
      return 3;
    }
    static_cast<void>(engine::core::cvar_set_int("r_vsync", 0));
    static_cast<void>(engine::core::cvar_set_int("r_max_fps", 0));

    // Short frames: half a step each, so they alternate between no step
    // and one. Long frames: two and a half steps each, so every one runs
    // two or three. Mixed: both kinds in one run, which is where the
    // defect showed most. Deltas are in fixed steps.
    constexpr unsigned kSteps = 60U;
    const RunResult fine =
        run_rig(pipeline, kSteps, [](unsigned) { return 0.5; });
    const RunResult coarse =
        run_rig(pipeline, kSteps, [](unsigned) { return 2.5; });
    const RunResult mixed = run_rig(pipeline, kSteps, [](unsigned frame) {
      return ((frame % 3U) == 0U) ? 2.5 : 0.5;
    });

    if (!fine.ok || !coarse.ok || !mixed.ok) {
      std::fprintf(stderr, "FAIL: a run did not complete\n");
      result = 4;
    } else if ((coarse.multiStepFrames == 0U) ||
               (mixed.multiStepFrames == 0U) || (fine.zeroStepFrames == 0U) ||
               (mixed.zeroStepFrames == 0U) || (fine.multiStepFrames != 0U)) {
      // The injected deltas decide these counts, so a miss means the
      // pipeline stopped honouring them, and the invariants below would
      // pass without testing what they are for.
      std::fprintf(stderr,
                   "FAIL: the runs did not get their step patterns "
                   "(multi-step frames long %u mixed %u short %u, zero-step "
                   "frames short %u mixed %u)\n",
                   coarse.multiStepFrames, mixed.multiStepFrames,
                   fine.multiStepFrames, fine.zeroStepFrames,
                   mixed.zeroStepFrames);
      result = 5;
    } else {
      std::printf("pipeline_camera_step_cadence_test: %u workers, offset "
                  "drift fine %.6f coarse %.6f mixed %.6f m\n",
                  workers, fine.maxOffsetDrift, coarse.maxOffsetDrift,
                  mixed.maxOffsetDrift);

      // EXPECTATION: the presented camera keeps its offset from the
      // presented body on every frame, whatever that frame's step count.
      // One step of travel is 0.1 m, which is what the camera led by on
      // base; float error in two separately interpolated positions a few
      // tens of metres out is around 1e-5. A millimetre is a hundredth of
      // the defect and a hundred times the rounding.
      constexpr double kOffsetTolerance = 1.0e-3;
      const RunResult *runs[3] = {&fine, &coarse, &mixed};
      const char *names[3] = {"short", "long", "mixed"};
      for (int i = 0; i < 3; ++i) {
        if (runs[i]->maxOffsetDrift > kOffsetTolerance) {
          std::fprintf(stderr,
                       "FAIL: with %s frames the presented camera drifted "
                       "%.4f m against the body it follows (one step of "
                       "travel is 0.1 m)\n",
                       names[i], runs[i]->maxOffsetDrift);
          result = 10 + i;
        }
      }

      // EXPECTATION: the arm's lag integrates per fixed step, so its
      // length after a given number of steps does not depend on how those
      // steps were split into frames. The same float operations run in the
      // same order either way, so the lengths are equal exactly.
      unsigned compared = 0U;
      for (const auto &[stepTotal, length] : coarse.armLengthAtStep) {
        const auto match = fine.armLengthAtStep.find(stepTotal);
        if (match == fine.armLengthAtStep.end()) {
          continue;
        }
        ++compared;
        if (match->second != length) {
          std::fprintf(stderr,
                       "FAIL: after %u steps the arm is %.7f long with short "
                       "frames and %.7f with long ones\n",
                       stepTotal, match->second, length);
          result = 20;
          break;
        }
      }
      // Short frames run at most one step, so the short run recorded every
      // total up to its last, and every total the long run recorded in that
      // range is compared: some twenty-four of them over sixty steps. (The
      // long run's final frame can overshoot the short run's end by a step
      // or two.) A shortfall means the runs disagree about the steps
      // themselves.
      const unsigned shortEnd = fine.armLengthAtStep.empty()
                                    ? 0U
                                    : fine.armLengthAtStep.rbegin()->first;
      unsigned inRange = 0U;
      for (const auto &entry : coarse.armLengthAtStep) {
        inRange += (entry.first <= shortEnd) ? 1U : 0U;
      }
      if ((result == 0) && ((compared != inRange) || (compared < 10U))) {
        std::fprintf(stderr,
                     "FAIL: %u of the long run's %u step totals in the short "
                     "run's range were common to both runs\n",
                     compared, inRange);
        result = 21;
      }
    }
    pipeline.teardown();
  }
  return result;
}

} // namespace
