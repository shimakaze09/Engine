// Regression for issue #529: spring arms and the camera advance once per
// fixed step, and the two camera samples render prep blends are one step
// apart. On base the camera stage ran once per rendered frame with the
// frame's summed step time and pushed a sample every frame, so a follow
// camera sat at the latest step's pose while the body it followed was drawn
// interpolated — leading it by up to a whole step on any frame that did not
// run exactly one — and an arm's lag depended on how the steps were split
// into frames. Drives the production EnginePipeline headless.
//
// Both invariants hold whatever step counts the frames happen to get, so
// nothing here depends on timing: the sleeps only make sure frames with no
// step and frames with several both occur.

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/core/job_system.h"
#include "engine/engine.h"
#include "engine/math/transform.h"
#include "engine/renderer/camera.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <system_error>
#include <thread>

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

/// Runs frames until `totalSteps` fixed steps have passed, sleeping per
/// frame as `sleepFor(frameIndex)` says.
template <typename SleepFor>
RunResult run_rig(engine::EnginePipeline &pipeline, unsigned totalSteps,
                  SleepFor &&sleepFor) noexcept {
  RunResult result{};
  const Entity body = build_rig(*g_world);
  if (body == kInvalidEntity) {
    return result;
  }
  // The first frames settle the camera manager onto the rig's camera.
  for (int frame = 0; frame < 4; ++frame) {
    if (!pipeline.execute_frame()) {
      return result;
    }
  }

  unsigned steps = 0U;
  bool haveFirstOffset = false;
  double firstOffset = 0.0;
  for (unsigned frame = 0U; (steps < totalSteps) && (frame < 4000U); ++frame) {
    std::this_thread::sleep_for(sleepFor(frame));
    g_presentedCameraSeen = false;
    if (!pipeline.execute_frame()) {
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
    // Until the first fixed step has run there is no pose behind the
    // current one, so there is nothing presented to compare yet. How many
    // frames that takes depends on how long they happen to be.
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

    // Short frames: most run no step at all, the rest one. Long frames:
    // 40 ms is more than two steps, so every one runs two or three. Mixed:
    // both kinds in one run, which is where the defect showed most.
    constexpr unsigned kSteps = 60U;
    const RunResult fine = run_rig(pipeline, kSteps, [](unsigned) {
      return std::chrono::milliseconds(2);
    });
    const RunResult coarse = run_rig(pipeline, kSteps, [](unsigned) {
      return std::chrono::milliseconds(40);
    });
    const RunResult mixed = run_rig(pipeline, kSteps, [](unsigned frame) {
      return std::chrono::milliseconds(((frame % 3U) == 0U) ? 40 : 2);
    });

    if (!fine.ok || !coarse.ok || !mixed.ok) {
      std::fprintf(stderr, "FAIL: a run did not complete\n");
      result = 4;
    } else if ((coarse.multiStepFrames == 0U) || (mixed.multiStepFrames == 0U)) {
      // A 40 ms sleep is more than two steps, so this cannot happen unless
      // the pipeline stopped stepping; the invariants below would then
      // pass without testing anything.
      std::fprintf(stderr,
                   "FAIL: 40 ms frames ran no multi-step frame (%u, %u); "
                   "nothing was exercised\n",
                   coarse.multiStepFrames, mixed.multiStepFrames);
      result = 5;
    } else {
      // Frames with no step need a machine that renders an empty world in
      // under a step's time. One that cannot — a sanitizer lane under load —
      // still checks the multi-step half below, and says the other half
      // did not run rather than passing it silently.
      const bool zeroStepExercised =
          (fine.zeroStepFrames > 0U) && (mixed.zeroStepFrames > 0U);
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
      if ((result == 0) && (compared < 10U)) {
        std::fprintf(stderr, "FAIL: only %u step totals were common to both "
                             "runs\n", compared);
        result = 21;
      }
      if ((result == 0) && !zeroStepExercised) {
        std::printf("SKIPPED: no frame ran zero steps on this machine (%u, "
                    "%u), so the zero-step half was not exercised; the "
                    "multi-step half passed\n",
                    fine.zeroStepFrames, mixed.zeroStepFrames);
      }
    }
    pipeline.teardown();
  }
  return result;
}

} // namespace
