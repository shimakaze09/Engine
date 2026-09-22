// Implements engine pipeline behavior for the Engine runtime world.

#include "engine/runtime/engine_pipeline.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#include <SDL3/SDL.h>

#include "engine/audio/audio.h"
#include "engine/core/bootstrap.h"
#include "engine/core/cvar.h"
#include "engine/core/string_util.h"
#include "engine/core/engine_stats.h"
#include "engine/core/input.h"
#include "engine/core/job_system.h"
#include "engine/core/logging.h"
#include "engine/core/simulation_clock.h"
#include "engine/core/platform.h"
#include "engine/core/profiler.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"
#include "engine/math/transform.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"
#include "engine/renderer/asset_manager.h"
#include "engine/content/asset_streaming.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/shadow_map.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/dynamic_resolution.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/mesh_primitives.h"
#include "engine/physics/physics_context.h"
#include "engine/renderer/shader_system.h"
#include "engine/renderer/texture_loader.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/scripting/game_binding_state.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/render_prep_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/animation_system.h"
#include "engine/runtime/camera_component_update.h"
#include "engine/runtime/spring_arm_update.h"
#include "frame_pacing.h"
#include "engine_bootstrap_content.h"
#include "engine_frame_collect.h"
#include "engine_runtime_streaming.h"
#include "mesh_reference_resolution.h"
#include "engine/runtime/world.h"
#include "engine/scripting/dap_server.h"
#include "engine/scripting/scripting.h"
#include "engine/content/asset_staleness.h"

namespace engine {

namespace runtime {

/// Lets the editor process one native event before deciding whether gameplay
/// input should see it.
InputEventRoute process_editor_input_event(const EditorBridge *bridge,
                                           void *nativeEvent) noexcept {
  if (nativeEvent == nullptr) {
    return InputEventRoute::Gameplay;
  }

  auto *event = static_cast<SDL_Event *>(nativeEvent);
  if ((bridge != nullptr) && (bridge->process_event != nullptr)) {
    bridge->process_event(event);
  }

  if (event->type == SDL_EVENT_QUIT) {
    return InputEventRoute::QuitRequested;
  }

  const bool keyboardEvent = (event->type == SDL_EVENT_KEY_DOWN) ||
                             (event->type == SDL_EVENT_KEY_UP) ||
                             (event->type == SDL_EVENT_TEXT_INPUT) ||
                             (event->type == SDL_EVENT_TEXT_EDITING);
  const bool mouseEvent = (event->type == SDL_EVENT_MOUSE_MOTION) ||
                          (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) ||
                          (event->type == SDL_EVENT_MOUSE_BUTTON_UP) ||
                          (event->type == SDL_EVENT_MOUSE_WHEEL);
  const bool captureKeyboard = (bridge != nullptr) &&
                               (bridge->wants_capture_keyboard != nullptr) &&
                               bridge->wants_capture_keyboard();
  const bool captureMouse = (bridge != nullptr) &&
                            (bridge->wants_capture_mouse != nullptr) &&
                            bridge->wants_capture_mouse();

  if ((keyboardEvent && captureKeyboard) || (mouseEvent && captureMouse)) {
    return InputEventRoute::EditorCaptured;
  }

  return InputEventRoute::Gameplay;
}

/// Declared scene-transition reset order:
/// (0) on_end_play — load_scene/reset_world call this back once the
/// transition is guaranteed to commit but before it destructively touches
/// the outgoing world, so every outgoing scripted entity still alive and
/// callable receives on_end_play, matching editor Stop and pre-existing
/// destroy-driven EndPlay; (1) commit the replacement world — staged
/// load_scene or reset_world, which already resets the world-owned
/// managers in their declared order; (2) clear the outgoing scene's coroutines,
/// timer callback refs, entity pools, and per-entity script modules so no
/// stale entity identity or Lua reference can act on the new world
/// (globals persist by contract as the cross-scene handoff channel;
/// #93a/#93b: the World-owned TimerManager was already reset in step 1,
/// but only these scripting-side calls drop the Lua registry refs and pool
/// slots that point at it); (3) clear the pending op. A failed load skips
/// every reset, including step 0, and leaves the World and every scripting
/// state unchanged; only the request itself is consumed (see
/// process_pending_scene_op).
static void dispatch_outgoing_scene_end_play() noexcept {
  scripting::dispatch_entity_scripts_end_for_transition();
}

/// Processes a queued script scene operation, if one exists. A request is
/// attempted exactly once: a load that fails is consumed with one
/// diagnostic naming the path, because the request carries no new
/// evidence from one frame to the next and retrying it every active frame
/// would repeat the same file open, parse and error at frame rate. The
/// live World stays as it was; a script (or the player boot) re-requests
/// the load when it has reason to expect a different outcome.
bool process_pending_scene_op(World &world) noexcept {
  if (!scripting::has_pending_scene_op()) {
    return true;
  }

  bool processed = false;
  if (scripting::pending_scene_op_is_load()) {
    const char *scenePath = scripting::get_pending_scene_path();
    if ((scenePath != nullptr) &&
        runtime::load_scene(world, scenePath,
                            &dispatch_outgoing_scene_end_play)) {
      processed = true;
    } else {
      char message[400] = {};
      std::snprintf(message, sizeof(message),
                    "failed to process pending scene load '%s'; the "
                    "request is dropped and the current scene stays live",
                    (scenePath != nullptr) ? scenePath : "");
      core::log_message(core::LogLevel::Error, "engine", message);
      scripting::clear_pending_scene_op();
      return false;
    }
  } else if (scripting::pending_scene_op_is_new()) {
    runtime::reset_world(world, &dispatch_outgoing_scene_end_play);
    processed = true;
  }

  if (processed) {
    scripting::clear_coroutines();
    scripting::clear_timers();
    scripting::clear_entity_pools();
    scripting::clear_entity_script_modules();
    scripting::clear_pending_scene_op();
  }
  return processed;
}

} // namespace runtime

// ===========================================================================
// Anonymous-namespace helpers (moved verbatim from engine.cpp)
// ===========================================================================

namespace {

// Shader and watched-script timestamps are polled on this cadence, not
// every frame: up to 128 shader entries plus every watched script is a
// stat storm at frame rate, and only an attached editor can act on it.
constexpr double kHotReloadPollIntervalSeconds = 0.25;
constexpr std::size_t kChunkSize = 256U;
constexpr std::size_t kMaxUpdateStepsPerFrame = 8U;
static_assert(kMaxUpdateStepsPerFrame <= physics::kMaxCollisionFrameSteps,
              "the frame collision buffer must cover every catch-up step so "
              "accumulation alone never drops callbacks (#103)");
// One frame assembles every catch-up step's chunk jobs into one table per
// kind and never resets the cursor between steps, so each table holds
// kMaxUpdateStepsPerFrame steps of a full world. A fixed 1024
// overflowed on the fifth step at 65,536 transforms and turned one long
// frame on a large scene into a fatal run exit.
constexpr std::size_t kChunksPerStep =
    (runtime::World::kMaxEntities + kChunkSize - 1U) / kChunkSize;
constexpr std::size_t kMaxChunkJobs = kMaxUpdateStepsPerFrame * kChunksPerStep;
constexpr std::size_t kMaxPhaseJobs = kMaxUpdateStepsPerFrame * 2U + 4U;
static_assert((2U * kMaxChunkJobs) + kMaxPhaseJobs <= core::kMaxJobs,
              "a full-capacity world across every catch-up step must fit "
              "one frame graph");
constexpr std::uint32_t kSliceDiagnosticsPeriodFrames = 60U;

/// Production MaterialTextureLoadFn: the same synchronous texture loader
/// every other texture consumer (skybox, character textures) already calls.
/// Only ever invoked from stage_assets, on the main thread that owns the
/// render device.
renderer::TextureHandle load_material_texture_production(
    const char *virtualPath, void * /*userData*/) noexcept {
  return renderer::load_texture(virtualPath);
}

// ---------------------------------------------------------------------------
// Job data structures
// ---------------------------------------------------------------------------

struct UpdateChunkJobData final {
  runtime::World *world = nullptr;
  std::size_t startIndex = 0U;
  std::size_t count = 0U;
  float deltaSeconds = 0.0F;
};

struct PhysicsChunkJobData final {
  runtime::World *world = nullptr;
  std::size_t startIndex = 0U;
  std::size_t count = 0U;
  float deltaSeconds = 0.0F;
  std::atomic<bool> *frameGraphFailed = nullptr;
};

struct WorldPhaseJobData final {
  runtime::World *world = nullptr;
  // Work the pipeline needs done inside a phase job, after the World's own
  // part of it: the per-step camera evaluation rides the step job this
  // way, so it sees that step's transforms without a job of its own in
  // the graph. Null for every other phase job.
  void (*afterPhase)(void *context) noexcept = nullptr;
  void *afterPhaseContext = nullptr;
};

struct ResolveCollisionsJobData final {
  runtime::World *world = nullptr;
  float deltaSeconds = 0.0F;
  std::atomic<bool> *frameGraphFailed = nullptr;
};

struct FrameContext final {
  runtime::RenderPrepPipelineContext renderPrepPipeline{};
  std::array<UpdateChunkJobData, kMaxChunkJobs> updateJobData{};
  std::array<core::JobHandle, kMaxChunkJobs> updateJobHandles{};
  std::array<PhysicsChunkJobData, kMaxChunkJobs> physicsJobData{};
  std::array<core::JobHandle, kMaxChunkJobs> physicsJobHandles{};
  std::array<WorldPhaseJobData, kMaxPhaseJobs> phaseJobData{};
  ResolveCollisionsJobData resolveCollisionsJobData{};
  std::atomic<bool> frameGraphFailed = false;
  /// Draws render prep could not fit this frame; read after the
  /// graph drains, reported once per run and published in EngineStats.
  std::atomic<std::uint32_t> droppedDrawCommands = 0U;
};

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------


void mark_graph_failed(std::atomic<bool> *frameGraphFailed) noexcept {
  if (frameGraphFailed != nullptr) {
    frameGraphFailed->store(true, std::memory_order_release);
  }
}

/// Blends two fixed-step camera samples for smooth presentation; the
/// clip planes stay at the newer sample's values.
renderer::CameraState interpolate_camera_state(
    const renderer::CameraState &previous, const renderer::CameraState &current,
    float alpha) noexcept {
  renderer::CameraState out = current;
  out.position = math::add(
      previous.position,
      math::mul(math::sub(current.position, previous.position), alpha));
  out.target = math::add(
      previous.target,
      math::mul(math::sub(current.target, previous.target), alpha));
  out.up = math::normalize(math::add(
      previous.up, math::mul(math::sub(current.up, previous.up), alpha)));
  out.fovRadians =
      previous.fovRadians + ((current.fovRadians - previous.fovRadians) * alpha);
  // The ortho half-height lerps like fov, its perspective analogue, only
  // when the kind is stable across the pair; the projection kind itself
  // snaps with `out = current` (near/far precedent above) — there is no
  // meaningful blend between perspective and orthographic matrices.
  if (previous.projection == current.projection) {
    out.orthographicSize =
        previous.orthographicSize +
        ((current.orthographicSize - previous.orthographicSize) * alpha);
  }
  return out;
}

/// Advances this system for the current frame or tick for chunk job.
void update_chunk_job(void *userData) noexcept {
  auto *jobData = static_cast<UpdateChunkJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }

  static_cast<void>(jobData->world->update_transforms_range(
      jobData->startIndex, jobData->count, jobData->deltaSeconds));
}

void physics_chunk_job(void *userData) noexcept {
  auto *jobData = static_cast<PhysicsChunkJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }

  if (!runtime::step_physics_range(*jobData->world, jobData->startIndex,
                                   jobData->count, jobData->deltaSeconds)) {
    mark_graph_failed(jobData->frameGraphFailed);
  }
}

void resolve_collisions_job(void *userData) noexcept {
  auto *jobData = static_cast<ResolveCollisionsJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }

  if (!runtime::resolve_collisions(*jobData->world, jobData->deltaSeconds)) {
    mark_graph_failed(jobData->frameGraphFailed);
  }
}

void commit_update_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->commit_update_phase();
  }
}

/// Begins the requested operation or profiling range for update step job.
void begin_update_step_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->begin_update_step();
    if (jobData->afterPhase != nullptr) {
      jobData->afterPhase(jobData->afterPhaseContext);
    }
  }
}

/// Begins the requested operation or profiling range for render prep phase job.
void begin_render_prep_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->begin_render_prep_phase();
  }
}

/// Begins the requested operation or profiling range for render phase job.
void begin_render_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->begin_render_phase();
  }
}

/// Ends the requested operation or profiling range for frame phase job.
void end_frame_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->end_frame_phase();
  }
}

bool link_dependency(core::JobHandle prerequisite,
                     core::JobHandle dependent) noexcept {
  if (!core::is_valid_handle(prerequisite) ||
      !core::is_valid_handle(dependent)) {
    return false;
  }

  return core::add_dependency(prerequisite, dependent);
}

/// Submits work to the owning buffer or system for world phase job.
core::JobHandle submit_world_phase_job(
    FrameContext *frameContext, runtime::World *world,
    std::size_t *phaseJobCursor, core::JobFunction function,
    void (*afterPhase)(void *context) noexcept = nullptr,
    void *afterPhaseContext = nullptr) noexcept {
  if ((frameContext == nullptr) || (world == nullptr) ||
      (phaseJobCursor == nullptr) ||
      (*phaseJobCursor >= frameContext->phaseJobData.size())) {
    return {};
  }

  WorldPhaseJobData &jobData = frameContext->phaseJobData[*phaseJobCursor];
  ++(*phaseJobCursor);
  jobData.world = world;
  // Assigned on every submit: the slots are reused across frames, and a
  // hook left over from another job would run where it has no business.
  jobData.afterPhase = afterPhase;
  jobData.afterPhaseContext = afterPhaseContext;

  core::Job job{};
  job.function = function;
  job.data = &jobData;
  return core::submit(job);
}

// ---------------------------------------------------------------------------
// Play state helpers
// ---------------------------------------------------------------------------

enum class LoopPlayState : std::uint8_t { Stopped, Playing, Paused };

LoopPlayState query_editor_play_state() noexcept {
  const runtime::EditorBridge *bridge = runtime::editor_bridge();
  if (bridge == nullptr) {
    return LoopPlayState::Playing;
  }

  if ((bridge->is_playing != nullptr) && bridge->is_playing()) {
    return LoopPlayState::Playing;
  }

  if ((bridge->is_paused != nullptr) && bridge->is_paused()) {
    return LoopPlayState::Paused;
  }

  return LoopPlayState::Stopped;
}

/// Returns whether the events included a quit that ends a live session.
bool process_input_events_with_editor() noexcept {
  bool quitRequested = false;
  core::begin_input_frame();

  const runtime::EditorBridge *bridge = runtime::editor_bridge();

  SDL_Event event{};
  while (SDL_PollEvent(&event)) {
    const runtime::InputEventRoute route =
        runtime::process_editor_input_event(bridge, &event);
    if (route == runtime::InputEventRoute::QuitRequested) {
      // The editor gets a chance to defer the quit behind its own
      // unsaved-change confirm flow; a null hook or a bound-but-clean
      // document both proceed immediately.
      const bool proceedNow = (bridge == nullptr) ||
                              (bridge->handle_quit_request == nullptr) ||
                              bridge->handle_quit_request();
      if (proceedNow) {
        // A quit that ends a live play session dispatches on_end_play
        // exactly like editor Stop and
        // scene transitions. The editor's quit hook has already routed
        // through the Stop flow (play state reads Stopped here, and
        // stage_play_transitions dispatches later this frame); only a
        // still-playing session — standalone runtime, or a bridge with no
        // stop routing — takes this direct dispatch.
        // A still-playing session ends in stage_play_transitions, as a
        // Stop: the end hooks run there and no tick, physics step or
        // collision callback of the session follows them.
        quitRequested = true;
        core::request_platform_quit();
      }
      continue;
    }
    if (route == runtime::InputEventRoute::EditorCaptured) {
      continue;
    }

    core::input_process_event(&event);
  }

  core::end_input_frame();
  return quitRequested;
}

// ---------------------------------------------------------------------------
// Diagnostic helpers
// ---------------------------------------------------------------------------

const char *world_phase_to_string(runtime::WorldPhase phase) noexcept {
  switch (phase) {
  case runtime::WorldPhase::Input:
    return "Input";
  case runtime::WorldPhase::Simulation:
    return "Simulation";
  case runtime::WorldPhase::TransformPropagation:
    return "Transform";
  case runtime::WorldPhase::RenderSubmission:
    return "RenderPrep";
  case runtime::WorldPhase::Render:
    return "Render";
  default:
    return "Unknown";
  }
}


// ---------------------------------------------------------------------------
// Bootstrap mesh loading
// ---------------------------------------------------------------------------


} // namespace

// ===========================================================================
// EnginePipeline::Impl
// ===========================================================================

struct EnginePipeline::Impl final {
  using Clock = std::chrono::steady_clock;

  Impl() noexcept;

  // --- Owned resources ---
  core::ServiceLocator serviceLocator{};
  runtime::EngineServiceRegistry serviceRegistry;
  scripting::GameBindingState gameBindingState{};
  std::unique_ptr<runtime::World> world;
  std::unique_ptr<renderer::CommandBufferBuilder> commandBuffer;
  /// Camera-culled draws the shadow and capture passes still need.
  std::unique_ptr<renderer::CommandBufferBuilder> auxiliaryCommandBuffer;
  runtime::RenderPrepAuxiliaryInputs frameAuxiliaryInputs{};
  std::unique_ptr<renderer::GpuMeshRegistry> meshRegistry;
  std::unique_ptr<renderer::AssetDatabase> assetDatabase;
  std::unique_ptr<renderer::AssetManager> assetManager;
  std::unique_ptr<content::AssetStreamingQueue> assetStreamingQueue;
  std::unique_ptr<RuntimeAssetStreamingState> assetStreamingState;
  std::unique_ptr<FrameContext> frameContext;
  BootstrapMeshIds meshIds{};
  runtime::EnginePhysicsService physicsService{};
  runtime::EngineAudioService audioService{};
  runtime::EngineAssetDatabaseService assetDatabaseService{};
  // Mesh ids the World references that the catalog cannot place, already
  // reported for the current content.
  UnresolvedMeshReports unresolvedMeshReports{};
  runtime::EngineRendererService rendererService{};

  // --- Run lifetime ---
  // The run whose values currently occupy the process-wide alias slots (the
  // editor bridge's world, the editor asset service, the scripting
  // bindings). Those slots hold one value each, so the run that published
  // last owns them and is the only one entitled to clear them. Never
  // dereferenced, and never stale: every Impl is torn down before it is
  // destroyed, and a run clears this on the way out.
  static Impl *s_publishingRun;

  // Closing a run releases what that run still owns: its own service
  // registrations, streaming workers and asset-manager content always, and
  // the process-wide aliases only while this run is the one holding them.
  // The owner closes from teardown(), from destruction, and from a replacing
  // initialize(), so the release runs exactly once per Impl.
  bool tornDown = false;

  // --- External references ---
  const runtime::EditorBridge *bridge = nullptr;

  // --- Timing state ---
  Clock::time_point previousTick{};
  Clock::time_point frameStart{};
  // Last time the frame-metrics trace line was written (rate-limited).
  Clock::time_point lastMetricsLogTime{};
  double accumulator = 0.0;
  // The clock every consumer of simulated time reads, published to
  // scripting at frame start (new frame index) and again once the frame's
  // fixed steps are decided.
  core::SimulationClock clock{};
  // Frame delta source: negative reads the wall clock, otherwise every
  // playing frame accumulates exactly this many seconds (tests, replay).
  double frameDeltaOverrideSeconds = -1.0;

  // --- Loop state ---
  std::uint32_t maxFrames = 0U;
  bool running = true;
  // Draw-command overflow is reported once per run, not once per frame.
  bool droppedDrawsLogged = false;
  std::uint32_t lastDroppedDrawCommands = 0U;
  // Lights and captures are collected right after render prep, in the same
  // mutation epoch as the draw list; stage_render used to collect them
  // after the post-frame flush, so one submission carried draws from
  // before the flush and lights from after it, and a draw could name an
  // index the flush had already recycled.
  renderer::SceneLightData frameSceneLights{};
  std::array<renderer::SceneCaptureRequest, renderer::kMaxSceneCaptures>
      frameCaptureRequests{};
  std::size_t frameCaptureRequestCount = 0U;
  bool frameCollectionValid = false;
  // Distinguishes fatal loop exits from graceful stops for engine::run.
  bool fatalError = false;
  LoopPlayState previousPlayState = LoopPlayState::Playing;
  std::size_t previousAliveCount = 0U;
  std::size_t frameThreadCount = 0U;

  // --- Per-frame computed state ---
  LoopPlayState playState = LoopPlayState::Stopped;
  bool isPlaying = false;
  bool isPaused = false;
  bool singleStepping = false;
  bool runPhysics = false;
  bool runFrameGraph = false;
  renderer::DynamicResolutionState dynamicResolution{};
  // Per-frame tuning cvars read through handles so the frame stages never
  // scan the cvar table by name in steady state.
  core::CVarRef renderScaleCvar{"r_render_scale"};
  core::CVarRef dynamicResolutionCvar{"r_dynamic_resolution"};
  core::CVarRef dynamicResolutionMinCvar{"r_dynamic_resolution_min"};
  core::CVarRef maxFpsCvar{"r_max_fps"};
  core::CVarRef cacheSizeMbCvar{"asset.cache_size_mb"};
  // dbg_fail_frame_stage is consulted by every graph stage every frame;
  // its string is re-read only when its change stamp moves.
  core::CVarRef failFrameStageCvar{"dbg_fail_frame_stage"};
  std::uint64_t failFrameStageStamp = 0U;
  char failFrameStage[64] = {};
  Clock::time_point previousFrameStart{};
  double wallFrameMs = 0.0;
  // Windowed FPS readout: instantaneous 1/dt swings +-4 FPS on 1 ms of
  // present jitter, so the overlay publishes a half-second average.
  // Set by the quit event; stage_play_transitions turns it into a Stop.
  bool quitRequested = false;
  double fpsWindowSeconds = 0.0;
  std::uint32_t fpsWindowFrames = 0U;
  float smoothedFps = 0.0F;
  // Process memory is sampled on the FPS window, not per frame: reading it
  // opens /proc on Linux.
  float memoryUsedMbSample = -1.0F;
  // Starts due so the first editor frame polls, then one poll per interval.
  double hotReloadDueSeconds = kHotReloadPollIntervalSeconds;
  std::uint32_t frameHotReloadPolls = 0U;
  // Fixed-step camera history for render interpolation: the camera as
  // evaluated for the last fixed step and for the one before it, exactly
  // one step apart, because render prep blends them by the same one-step
  // fraction it blends entity poses by. Both samples were read from one
  // World's content, identified by cameraSampleEpoch: a scene replacement
  // discards that World, so the history is retired at the next camera
  // stage instead of blending the replacement's first frame from a view
  // no World owns any more.
  renderer::CameraState previousCameraSample{};
  renderer::CameraState currentCameraSample{};
  bool cameraSampleValid = false;
  std::uint32_t cameraSampleEpoch = 0U;
  double frameMs = 0.0;
  double utilizationPct = 0.0;
  core::JobSystemStats jobStats{};

  /// Total simulated time this frame: the dt every per-frame gameplay system
  /// receives, so one dispatch still accounts for every catch-up step.
  double step_seconds() const noexcept { return clock.deltaSeconds; }

  // --- Stage methods ---
  bool initialize(std::uint32_t maxFrameCount) noexcept;
  bool execute_frame() noexcept;
  void teardown() noexcept;

  void stage_input() noexcept;
  void stage_play_transitions() noexcept;
  void stage_timing() noexcept;
  void stage_scripting() noexcept;
  void stage_assets() noexcept;
  void stage_hot_reload() noexcept;
  void stage_audio() noexcept;
  void stage_animation() noexcept;
  bool stage_simulation_graph() noexcept;
  void stage_camera() noexcept;
  /// Advances spring arms, authored cameras and the camera blend by one
  /// fixed step against the World's composed transforms as they stand, and
  /// pushes the result onto the interpolation pair. Runs once per fixed
  /// step, with the transforms of that step: from the step job that
  /// recomposes them for every step but a frame's last, and from the
  /// camera stage for the last.
  void evaluate_cameras_for_step(float deltaSeconds) noexcept;
  /// evaluate_cameras_for_step as a phase-job hook; context is the Impl.
  static void camera_step_hook(void *context) noexcept;
  bool stage_render_prep_graph() noexcept;
  // True once when dbg_fail_frame_stage names this stage.
  bool consume_injected_stage_failure(const char *stageName) noexcept;
  void stage_post_frame() noexcept;
  void stage_measure_frame() noexcept;
  void stage_render() noexcept;
  void collect_frame_scene_data() noexcept;
  void build_auxiliary_inputs() noexcept;
  void stage_scene_commit() noexcept;
  void stage_diagnostics() noexcept;
  void stage_frame_cleanup() noexcept;
  void stage_frame_pacing() noexcept;
};

EnginePipeline::Impl *EnginePipeline::Impl::s_publishingRun = nullptr;

EnginePipeline::Impl::Impl() noexcept : serviceRegistry(serviceLocator) {}

// ---------------------------------------------------------------------------
// Impl::initialize
// ---------------------------------------------------------------------------

bool EnginePipeline::Impl::initialize(std::uint32_t maxFrameCount) noexcept {
  maxFrames = maxFrameCount;

  world.reset(new (std::nothrow) runtime::World());
  commandBuffer.reset(new (std::nothrow) renderer::CommandBufferBuilder());
  auxiliaryCommandBuffer.reset(new (std::nothrow)
                                   renderer::CommandBufferBuilder());
  meshRegistry.reset(new (std::nothrow) renderer::GpuMeshRegistry());
  assetDatabase.reset(new (std::nothrow) renderer::AssetDatabase());
  assetManager.reset(new (std::nothrow) renderer::AssetManager());
  assetStreamingQueue.reset(new (std::nothrow) content::AssetStreamingQueue());
  assetStreamingState.reset(new (std::nothrow) RuntimeAssetStreamingState());

  if (!world || !commandBuffer || !auxiliaryCommandBuffer || !meshRegistry ||
      !assetDatabase ||
      !assetManager || !assetStreamingQueue || !assetStreamingState) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to allocate runtime frame state");
    return false;
  }
  renderer::clear_asset_database(assetDatabase.get());
  renderer::clear_asset_manager(assetManager.get());
  if (!content::initialize_asset_streaming(assetStreamingQueue.get())) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to initialize runtime asset streaming queue");
    return false;
  }

  assetDatabaseService = runtime::EngineAssetDatabaseService{};
  assetStreamingState->database = assetDatabase.get();
  assetStreamingState->meshRegistry = meshRegistry.get();
  physicsService.world = world.get();
  physicsService.worldView =
      static_cast<physics::PhysicsWorldView *>(world.get());
  physicsService.context = &world->physics_context();
  audioService.update = &audio::update_audio;
  audioService.load_sound = &audio::load_sound;
  audioService.unload_sound = &audio::unload_sound;
  audioService.play_sound = &audio::play_sound;
  audioService.stop_sound = &audio::stop_sound;
  audioService.stop_all = &audio::stop_all;
  audioService.set_master_volume = &audio::set_master_volume;
  assetDatabaseService.database = assetDatabase.get();
  assetDatabaseService.manager = assetManager.get();
  assetDatabaseService.streamingQueue = assetStreamingQueue.get();
  rendererService.commandBuffer = commandBuffer.get();
  rendererService.meshRegistry = meshRegistry.get();
  rendererService.device = renderer::render_device();
  if (!serviceRegistry.register_services(world.get(), &physicsService,
                                         &audioService, &assetDatabaseService,
                                         &rendererService)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to register engine subsystem services");
    serviceRegistry.unregister_services();
    return false;
  }

  s_publishingRun = this;

  bridge = runtime::editor_bridge();
  runtime::set_editor_asset_service(&assetDatabaseService);

  runtime::bind_scripting_runtime(world.get(), serviceLocator);
  // The run's game-binding state is pipeline-owned; the binding
  // survives editor Stop's VM recycle because this Impl outlives it.
  scripting::bind_game_state(&gameBindingState);
  if ((bridge != nullptr) && (bridge->set_world != nullptr)) {
    bridge->set_world(world.get());
  }

  runtime::set_collision_dispatch(*world,
                                  &scripting::dispatch_physics_callbacks);

  if (!load_bootstrap_meshes(assetManager.get(), assetDatabase.get(),
                             meshRegistry.get(), &meshIds)) {
    teardown();
    return false;
  }
  scripting::set_default_mesh_asset_id(
      (meshIds.cube != renderer::kInvalidAssetId) ? meshIds.cube
                                                  : meshIds.bootstrap);
  scripting::set_builtin_mesh_ids(meshIds.plane, meshIds.cube, meshIds.sphere,
                                  meshIds.cylinder, meshIds.capsule,
                                  meshIds.pyramid);

  frameContext.reset(new (std::nothrow) FrameContext());
  if (!frameContext) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to allocate frame context");
    teardown();
    return false;
  }

  frameThreadCount = core::thread_frame_allocator_count();
  if ((frameThreadCount == 0U) ||
      (frameThreadCount >
       frameContext->renderPrepPipeline.localCommandBuffers.size())) {
    core::log_message(core::LogLevel::Error, "engine",
                      "invalid thread allocator count");
    teardown();
    return false;
  }

  create_bootstrap_scene(world.get(), meshIds);

  core::cvar_register_int("r_vsync", 1,
                          "Present interval: 0 off, 1 on, -1 adaptive");
  core::cvar_register_int("r_max_fps", 0,
                          "Frame cap in FPS (0 = uncapped; applies on top "
                          "of vsync)");

  previousTick = Clock::now();
  accumulator = 0.0;
  clock = core::SimulationClock{};
  running = true;
  previousPlayState = query_editor_play_state();
  previousAliveCount = world->alive_entity_count();
  core::reset_engine_stats();

  // Player mode: boot the configured startup scene through the
  // deferred transition engine.load_scene uses — a failed load logs and
  // keeps the bootstrap scene rather than corrupting the run.
  if (active_config().playerMode) {
    const char *scenePath = active_config().editorScenePath;
    if ((scenePath != nullptr) && (scenePath[0] != '\0')) {
      static_cast<void>(scripting::request_scene_load(scenePath));
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Impl::execute_frame
// ---------------------------------------------------------------------------

bool EnginePipeline::Impl::execute_frame() noexcept {
  core::profiler_begin_frame();
  // Published before any stage runs so every log_message call this frame
  // (including early stages ahead of stage_scripting) tags itself with the
  // right index for the editor Console's frame-context column.
  core::log_set_frame_index(clock.frameIndex);
  // The Lua-visible clock is published at the same point, for the same
  // reason: begin-play and start callbacks dispatch in stage_play_transitions,
  // ahead of stage_scripting, and engine.frame_count() there must name the
  // frame those callbacks run in — not the previous frame's publication.
  // This frame's steps are not decided yet, so the step fields say so
  // instead of repeating the previous frame's.
  clock.stepsThisFrame = 0U;
  clock.deltaSeconds = 0.0;
  scripting::set_simulation_clock(clock);
  PROFILE_SCOPE("engine_frame");
  frameStart = Clock::now();
  wallFrameMs =
      (previousFrameStart.time_since_epoch().count() != 0)
          ? std::chrono::duration<double, std::milli>(frameStart -
                                                      previousFrameStart)
                .count()
          : 0.0;
  previousFrameStart = frameStart;

  stage_input();
  stage_play_transitions();
  stage_timing();
  stage_scripting();
  stage_assets();
  stage_hot_reload();
  stage_audio();
  stage_animation();

  if (runFrameGraph) {
    if (!stage_simulation_graph()) {
      fatalError = true;
      core::profiler_end_frame();
      return false;
    }
    stage_camera();
    if (!stage_render_prep_graph()) {
      fatalError = true;
      core::profiler_end_frame();
      return false;
    }
    stage_post_frame();
  }

  stage_measure_frame();
  stage_render();
  if (runFrameGraph) {
    stage_scene_commit();
  }
  stage_diagnostics();
  stage_frame_cleanup();
  stage_frame_pacing();

  core::profiler_end_frame();
  return running;
}

// ---------------------------------------------------------------------------
// Impl::teardown
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::teardown() noexcept {
  if (tornDown) {
    return;
  }
  tornDown = true;

  // The steps below reach process-wide state, so they belong to whichever
  // run currently owns the alias slots. A run closing while a newer one owns
  // them would otherwise clear the newer run's world alias, input bindings
  // and game state out from under it; a run that never published (an
  // initialize that failed before the publish block) owns nothing here.
  if (s_publishingRun == this) {
    s_publishingRun = nullptr;

    if ((bridge != nullptr) && (bridge->set_world != nullptr)) {
      bridge->set_world(nullptr);
    }

    // Run-scoped residue must not leak into a later pipeline run: the
    // scripting run state and the animation controller registry are reset
    // while the VM and bindings are still alive, before anything unbinds,
    // then the engine-tier subsystems drop their run-scoped content (script
    // input bindings, scene audio, per-run renderer state).
    scripting::reset_run_state();
    runtime::reset_anim_controllers();
    core::clear_gameplay_bindings();
    audio::unload_all_sounds();
    renderer::reset_renderer_public_state();
    content::reset_cooked_asset_stale_warnings();

    runtime::set_editor_asset_service(nullptr);
    scripting::bind_game_state(nullptr);
    runtime::unbind_scripting_runtime(serviceLocator);
  }

  // The rest is this Impl's own storage, released whichever run holds the
  // alias slots.
  serviceRegistry.unregister_services();

  content::shutdown_asset_streaming(assetStreamingQueue.get());
  clear_streamed_mesh_data(assetStreamingState.get());
  renderer::shutdown_asset_manager(assetManager.get(), assetDatabase.get(),
                                   meshRegistry.get());
}

// ---------------------------------------------------------------------------
// Stage: input
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_input() noexcept {
  if (process_input_events_with_editor()) {
    quitRequested = true;
  }
}

// ---------------------------------------------------------------------------
// Stage: play transitions
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_play_transitions() noexcept {
  playState = query_editor_play_state();
  if (quitRequested && (playState != LoopPlayState::Stopped)) {
    // Quit ends a live session exactly like Stop: on_end_play
    // runs once, here, and the frame continues as Stopped, so nothing of
    // the session runs after its end hooks. The scripting VM is not
    // recycled the way Stop does; teardown owns it from here.
    scripting::dispatch_entity_scripts_end();
    playState = LoopPlayState::Stopped;
    previousPlayState = LoopPlayState::Stopped;
  }

  if ((playState == LoopPlayState::Playing) &&
      (previousPlayState == LoopPlayState::Stopped)) {
    const char *mainScriptPath = active_config().mainScriptPath;
    if (mainScriptPath != nullptr) {
      scripting::watch_script_file(mainScriptPath);
    }
    scripting::dispatch_entity_scripts_start();
  }

  // Fire BeginPlay for entities that haven't received it yet. Skip the phase
  // entirely on frames with no pending entities (the common case).
  if ((playState == LoopPlayState::Playing) &&
      (world->begin_play_pending_count() > 0U)) {
    world->begin_begin_play_phase();
    scripting::dispatch_entity_scripts_begin_play(world.get());
    world->end_begin_play_phase();
    // Flush after leaving the phase: mutations only apply in Input, so a
    // flush inside BeginPlay is a no-op and the writes miss the first step.
    scripting::flush_deferred_mutations();
  }

  if ((playState == LoopPlayState::Stopped) &&
      (previousPlayState != LoopPlayState::Stopped)) {
    scripting::dispatch_entity_scripts_end();
    scripting::clear_entity_script_modules();
    scripting::shutdown_scripting();
    if (!scripting::initialize_scripting()) {
      core::log_message(core::LogLevel::Error, "scripting",
                        "failed to reinitialize scripting on stop");
    } else {
      runtime::bind_scripting_runtime(world.get(), serviceLocator);
      scripting::set_default_mesh_asset_id(
          (meshIds.cube != renderer::kInvalidAssetId) ? meshIds.cube
                                                      : meshIds.bootstrap);
      scripting::set_builtin_mesh_ids(meshIds.plane, meshIds.cube,
                                      meshIds.sphere, meshIds.cylinder,
                                      meshIds.capsule, meshIds.pyramid);
    }

    accumulator = 0.0;
    previousTick = frameStart;
    clock.simulationSeconds = 0.0;
    clock.tickIndex = 0U;
  }

  isPlaying = (playState == LoopPlayState::Playing);
  isPaused = (playState == LoopPlayState::Paused);
  runPhysics = isPlaying;
  runFrameGraph = !isPaused;

  // A consumed editor single-step promotes this paused frame to a playing
  // frame; stage_timing then simulates exactly one fixed step.
  singleStepping = isPaused && (bridge != nullptr) &&
                   (bridge->consume_step_request != nullptr) &&
                   bridge->consume_step_request();
  if (singleStepping) {
    isPlaying = true;
    runPhysics = true;
    runFrameGraph = true;
  }

  if (isPlaying && (previousPlayState != LoopPlayState::Playing) &&
      !singleStepping) {
    world->clear_world_transform_history();
    cameraSampleValid = false;
  }
}

// ---------------------------------------------------------------------------
// Stage: timing
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_timing() noexcept {
  if (isPlaying && !singleStepping) {
    const auto now = Clock::now();
    if (frameDeltaOverrideSeconds >= 0.0) {
      // An injected delta is exact by definition: it is the input a test
      // or a replay chose, so it enters the accumulator untouched.
      accumulator += frameDeltaOverrideSeconds;
    } else {
      // A measured delta is snapped so vsync-at-fixed-rate frames drain
      // exactly one step instead of alternating 0/2 on measurement noise
      // (frame_pacing).
      accumulator += runtime::snap_delta_to_fixed_step(
          std::chrono::duration<double>(now - previousTick).count(),
          core::kFixedDeltaSeconds);
    }
    previousTick = now;
  } else {
    previousTick = frameStart;
  }

  const runtime::FixedStepDecision decision = runtime::fixed_step_decision(
      isPlaying, singleStepping, accumulator, core::kFixedDeltaSeconds,
      kMaxUpdateStepsPerFrame);
  clock.stepsThisFrame = static_cast<std::uint32_t>(decision.stepCount);
  accumulator = decision.remainingAccumulator;
  clock.deltaSeconds =
      static_cast<double>(clock.stepsThisFrame) * core::kFixedDeltaSeconds;
  clock.simulationSeconds += clock.deltaSeconds;
  clock.tickIndex += clock.stepsThisFrame;
  clock.renderAlpha = (isPlaying && !singleStepping)
                          ? accumulator / core::kFixedDeltaSeconds
                          : 1.0;
  // Every frame publishes its decided steps, a paused or zero-step frame
  // included, so a script reading the clock always sees this frame's.
  scripting::set_simulation_clock(clock);

  // Timers come due on simulation time: one advance per decided step with
  // the fixed delta, so a timer fires at the same tick whatever the frame
  // rate. Their callbacks run later, once, in stage_scripting — a timer
  // reads no physics state, so advancing them here rather than inside the
  // simulation graph changes nothing about when they come due, and it
  // keeps callbacks out of a step they must not re-enter.
  if (isPlaying && (world != nullptr)) {
    for (std::uint32_t step = 0U; step < clock.stepsThisFrame; ++step) {
      static_cast<void>(world->timer_manager().advance(
          static_cast<float>(core::kFixedDeltaSeconds)));
    }
  }
}

// ---------------------------------------------------------------------------
// Stage: scripting
//
// Cadence contract. Two classes of system exist in this frame:
//   * per-fixed-step: transform propagation, physics, collision resolve,
//     animation, spring arms and camera evaluation each run exactly
//     stepsThisFrame times with the fixed delta apiece, so their
//     integration is independent of the render rate. The camera belongs
//     here because its blend and the arm's lag are single lerps per call —
//     one call with twice the delta is not two calls with one — and because
//     render prep interpolates the view by the same one-step fraction as
//     every entity, which only lines up when the two camera samples are one
//     step apart like the two entity poses.
//   * per-frame: entity script on_tick, Lua timers and coroutines run once
//     per rendered frame. They are dispatched once — re-entrant script
//     dispatch per catch-up step would multiply gameplay callbacks and their
//     deferred mutations — but they receive step_seconds(), the total time
//     simulated this frame, so their dt equals the time the world actually
//     advanced. Passing the bare fixed delta made timers and script-driven
//     motion run slow whenever catch-up stepped more than once.
// A frame's last camera evaluation runs in stage_camera, between the last
// fixed step and render prep, so culling and interpolation see this
// frame's camera; the earlier steps' run inside the simulation graph.
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_scripting() noexcept {
  // The debugger transport is serviced every frame whatever the play
  // state, so a client can attach and set breakpoints before Play or
  // disconnect while paused.
  if (scripting::dap_is_running()) {
    scripting::dap_poll();
  }
  if (isPlaying && (clock.stepsThisFrame > 0U)) {
    scripting::dispatch_timers();
    scripting::tick_coroutines();
    scripting::dispatch_entity_scripts_update(
        static_cast<float>(step_seconds()));
  }

  scripting::flush_deferred_mutations();
}

// ---------------------------------------------------------------------------
// Stage: assets
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_assets() noexcept {
  bool updatedAssets = true;
  renderer::advance_asset_database_frame(assetDatabase.get());
  if (assetStreamingQueue != nullptr) {
    content::begin_streaming_frame(assetStreamingQueue.get());
  }

  // Every mesh the World references and nothing has loaded yet is bound
  // to its catalogued asset and requested here, so a reopened scene draws
  // without a script naming its meshes. A reference is looked up once and
  // costs one integer test per frame afterwards.
  static_cast<void>(request_referenced_mesh_assets(
      *world, &assetDatabaseService, &unresolvedMeshReports));

  if ((assetStreamingQueue != nullptr) && (assetStreamingState != nullptr)) {
    static_cast<void>(content::update_asset_streaming(
        assetStreamingQueue.get(), &runtime_streaming_load_mesh,
        &runtime_streaming_upload_mesh, assetStreamingState.get()));
  }
  sync_streaming_failures(&assetDatabaseService);

  updatedAssets = renderer::update_asset_manager(
      assetManager.get(), assetDatabase.get(), meshRegistry.get(), 16U);
  // Not a hot path: cost is O(materials with an unresolved texture slot),
  // which drains to zero once content is resident (see resolve_material_
  // textures's header comment).
  static_cast<void>(renderer::resolve_material_textures(
      assetDatabase.get(), &load_material_texture_production, nullptr));

  const int cacheMb = cacheSizeMbCvar.get_int(512);
  if (cacheMb > 0) {
    static_cast<void>(renderer::evict_mesh_assets_over_budget(
        assetDatabase.get(),
        static_cast<std::uint64_t>(cacheMb) * 1024ULL * 1024ULL));
  }

  if (!updatedAssets) {
    core::log_message(core::LogLevel::Warning, "assets",
                      "one or more asset transitions failed this frame");
  }
}

// ---------------------------------------------------------------------------
// Stage: hot reload
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_hot_reload() noexcept {
  frameHotReloadPolls = 0U;
  if (runtime::editor_bridge() == nullptr) {
    return; // a player has nothing to reload into
  }
  hotReloadDueSeconds += wallFrameMs / 1000.0;
  if (hotReloadDueSeconds < kHotReloadPollIntervalSeconds) {
    return;
  }
  hotReloadDueSeconds = 0.0;
  frameHotReloadPolls = 1U;
  renderer::check_shader_reload();
  scripting::check_script_reload();
}

// ---------------------------------------------------------------------------
// Stage: audio
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_audio() noexcept { audio::update_audio(); }

// ---------------------------------------------------------------------------
// Stage: animation (must precede the frame graph: render prep bakes each
// draw's palette slot, so poses and slots have to be current-frame)
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_animation() noexcept {
  if (!isPlaying) {
    return;
  }
  // One evaluation per fixed simulation step, matching the frame graph's
  // stepping — never per render frame, or playback speed would track the
  // uncapped render rate.
  for (std::size_t step = 0U; step < clock.stepsThisFrame; ++step) {
    runtime::update_animations(*world, static_cast<float>(core::kFixedDeltaSeconds));
    scripting::dispatch_animation_event_callbacks();
  }
}

// ---------------------------------------------------------------------------
// Fault-injection seam: dbg_fail_frame_stage forces the named
// graph stage to report a fatal failure through its production return path.
// The cvar self-clears so the injected failure fires exactly once per set.
// ---------------------------------------------------------------------------

bool EnginePipeline::Impl::consume_injected_stage_failure(
    const char *stageName) noexcept {
  const std::uint64_t stamp = failFrameStageCvar.change_stamp();
  if (stamp != failFrameStageStamp) {
    core::copy_string(failFrameStage, sizeof(failFrameStage),
                      failFrameStageCvar.get_string(""));
    failFrameStageStamp = stamp;
  }
  if ((failFrameStage[0] == '\0') ||
      (std::strcmp(failFrameStage, stageName) != 0)) {
    return false;
  }
  // The clear moves the stamp, so the next call re-reads the empty value.
  static_cast<void>(core::cvar_set_string("dbg_fail_frame_stage", ""));
  core::log_message(core::LogLevel::Error, "engine",
                    "injected frame-stage failure (dbg_fail_frame_stage)");
  return true;
}

// ---------------------------------------------------------------------------
// Stage: simulation graph (fixed-step job submission + execution; ends the
// graph after the last commit so the camera stage can run before render prep)
// Returns false on fatal error; sets running = false internally.
// ---------------------------------------------------------------------------

bool EnginePipeline::Impl::stage_simulation_graph() noexcept {
  if (consume_injected_stage_failure("simulation_graph")) {
    running = false;
    return false;
  }
  frameContext->frameGraphFailed.store(false, std::memory_order_release);
  frameContext->droppedDrawCommands.store(0U, std::memory_order_release);
  if (clock.stepsThisFrame == 0U) {
    return true;
  }

  if (!core::begin_frame_graph()) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to begin frame graph");
    running = false;
    return false;
  }

  std::size_t updateJobCursor = 0U;
  std::size_t physicsJobCursor = 0U;
  std::size_t phaseJobCursor = 0U;

  world->begin_update_phase();

  core::JobHandle previousUpdateCommit{};
  bool graphFailed = false;

  for (std::size_t step = 0U; step < clock.stepsThisFrame; ++step) {
    core::JobHandle commitHandle =
        submit_world_phase_job(frameContext.get(), world.get(), &phaseJobCursor,
                               &commit_update_phase_job);
    if (!core::is_valid_handle(commitHandle)) {
      graphFailed = true;
      break;
    }

    if (core::is_valid_handle(previousUpdateCommit) &&
        !link_dependency(previousUpdateCommit, commitHandle)) {
      graphFailed = true;
      break;
    }

    core::JobHandle beginStepHandle{};
    if (step > 0U) {
      // The step job has just recomposed the transforms of the step
      // before this one, which is the pose the camera has to be evaluated
      // against for that step. It runs between that step's commit and
      // this step's work, so nothing else touches the World, the camera
      // manager or the sample pair while it does, and the main thread is
      // waiting on the graph.
      beginStepHandle = submit_world_phase_job(
          frameContext.get(), world.get(), &phaseJobCursor,
          &begin_update_step_job,
          isPlaying ? &EnginePipeline::Impl::camera_step_hook : nullptr, this);
      if (!core::is_valid_handle(beginStepHandle)) {
        graphFailed = true;
        break;
      }
      if (!link_dependency(previousUpdateCommit, beginStepHandle)) {
        graphFailed = true;
        break;
      }
      if (!link_dependency(beginStepHandle, commitHandle)) {
        graphFailed = true;
        break;
      }
    }

    // Catch-up steps gate their update jobs on the step's begin job so
    // phase preparation can never race chunk work; step 0's begin ran
    // synchronously above, so its updates gate on nothing.
    const core::JobHandle updateGate = core::is_valid_handle(beginStepHandle)
                                           ? beginStepHandle
                                           : previousUpdateCommit;

    const std::size_t transformCount = world->transform_count();
    const std::size_t updateJobStart = updateJobCursor;

    for (std::size_t start = 0U; start < transformCount; start += kChunkSize) {
      if (updateJobCursor >= frameContext->updateJobData.size()) {
        graphFailed = true;
        break;
      }

      const std::size_t count = ((start + kChunkSize) > transformCount)
                                    ? (transformCount - start)
                                    : kChunkSize;

      UpdateChunkJobData &updateData =
          frameContext->updateJobData[updateJobCursor];
      updateData.world = world.get();
      updateData.startIndex = start;
      updateData.count = count;
      updateData.deltaSeconds = static_cast<float>(core::kFixedDeltaSeconds);

      core::Job updateJob{};
      updateJob.function = &update_chunk_job;
      updateJob.data = &updateData;
      const core::JobHandle updateHandle = core::submit(updateJob);
      if (!core::is_valid_handle(updateHandle)) {
        graphFailed = true;
        break;
      }

      if (core::is_valid_handle(updateGate) &&
          !link_dependency(updateGate, updateHandle)) {
        graphFailed = true;
        break;
      }

      if (!link_dependency(updateHandle, commitHandle)) {
        graphFailed = true;
        break;
      }

      frameContext->updateJobHandles[updateJobCursor] = updateHandle;
      ++updateJobCursor;
    }

    if (graphFailed) {
      break;
    }

    if (runPhysics) {
      const std::size_t physicsJobStart = physicsJobCursor;
      std::size_t updateHandleIndex = updateJobStart;
      for (std::size_t start = 0U; start < transformCount;
           start += kChunkSize) {
        if ((physicsJobCursor >= frameContext->physicsJobData.size()) ||
            (updateHandleIndex >= updateJobCursor)) {
          graphFailed = true;
          break;
        }

        const std::size_t count = ((start + kChunkSize) > transformCount)
                                      ? (transformCount - start)
                                      : kChunkSize;

        PhysicsChunkJobData &physicsData =
            frameContext->physicsJobData[physicsJobCursor];
        physicsData.world = world.get();
        physicsData.startIndex = start;
        physicsData.count = count;
        physicsData.deltaSeconds = static_cast<float>(core::kFixedDeltaSeconds);
        physicsData.frameGraphFailed = &frameContext->frameGraphFailed;

        core::Job physicsJob{};
        physicsJob.function = &physics_chunk_job;
        physicsJob.data = &physicsData;
        const core::JobHandle physicsHandle = core::submit(physicsJob);
        if (!core::is_valid_handle(physicsHandle)) {
          graphFailed = true;
          break;
        }

        if (!link_dependency(frameContext->updateJobHandles[updateHandleIndex],
                             physicsHandle)) {
          graphFailed = true;
          break;
        }

        frameContext->physicsJobHandles[physicsJobCursor] = physicsHandle;
        ++physicsJobCursor;
        ++updateHandleIndex;
      }

      if (graphFailed) {
        break;
      }

      frameContext->resolveCollisionsJobData.world = world.get();
      frameContext->resolveCollisionsJobData.deltaSeconds =
          static_cast<float>(core::kFixedDeltaSeconds);
      frameContext->resolveCollisionsJobData.frameGraphFailed =
          &frameContext->frameGraphFailed;
      core::Job resolveJob{};
      resolveJob.function = &resolve_collisions_job;
      resolveJob.data = &frameContext->resolveCollisionsJobData;
      const core::JobHandle resolveHandle = core::submit(resolveJob);
      if (!core::is_valid_handle(resolveHandle)) {
        graphFailed = true;
        break;
      }

      // With zero transform chunks no physics jobs exist, so resolve must
      // gate on the step begin directly or it can race phase preparation.
      if (core::is_valid_handle(updateGate) &&
          !link_dependency(updateGate, resolveHandle)) {
        graphFailed = true;
        break;
      }

      for (std::size_t i = physicsJobStart; i < physicsJobCursor; ++i) {
        if (!link_dependency(frameContext->physicsJobHandles[i],
                             resolveHandle)) {
          graphFailed = true;
          break;
        }
      }

      if (!graphFailed && !link_dependency(resolveHandle, commitHandle)) {
        graphFailed = true;
        break;
      }
    }

    previousUpdateCommit = commitHandle;
  }

  if (graphFailed) {
    core::log_message(core::LogLevel::Error, "engine",
                      "job graph assembly failed");
    running = false;
    static_cast<void>(core::end_frame_graph());
    return false;
  }

  // end_frame_graph needs the whole graph drained, not one handle.
  core::wait_all();
  const bool stepJobsFailed =
      frameContext->frameGraphFailed.load(std::memory_order_acquire);
  if (!core::end_frame_graph()) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to end frame graph");
    running = false;
    return false;
  }

  if (stepJobsFailed) {
    core::log_message(core::LogLevel::Error, "engine",
                      "frame graph job execution failed");
    running = false;
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// Stage: camera (propagates world transforms, then runs spring arms and
// camera evaluation so render prep culls with this frame's camera; the
// per-frame cadence contract above stage_scripting applies)
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::camera_step_hook(void *context) noexcept {
  auto *self = static_cast<EnginePipeline::Impl *>(context);
  if (self != nullptr) {
    self->evaluate_cameras_for_step(static_cast<float>(core::kFixedDeltaSeconds));
  }
}

void EnginePipeline::Impl::evaluate_cameras_for_step(
    float deltaSeconds) noexcept {
  runtime::update_spring_arm_cameras(*world, deltaSeconds);
  runtime::update_persistent_cameras(*world, deltaSeconds);
  runtime::CameraEntry evaluated{};
  world->camera_manager().evaluate(deltaSeconds, &evaluated);
  if (world->camera_manager().camera_count() > 0U) {
    renderer::CameraState cam{};
    cam.position = evaluated.position;
    cam.target = evaluated.target;
    cam.up = evaluated.up;
    cam.fovRadians = evaluated.fovRadians;
    cam.nearPlane = evaluated.nearPlane;
    cam.farPlane = evaluated.farPlane;
    cam.projection = evaluated.projection;
    cam.orthographicSize = evaluated.orthographicSize;
    renderer::set_active_camera(cam);
  }

  previousCameraSample =
      cameraSampleValid ? currentCameraSample : renderer::get_active_camera();
  currentCameraSample = renderer::get_active_camera();
  cameraSampleValid = true;
}

void EnginePipeline::Impl::stage_camera() noexcept {
  world->begin_transform_phase();

  if (!isPlaying) {
    return;
  }

  // The World's content epoch changes only when a scene commit replaced
  // its content (script load_scene/new_scene through the pending scene op,
  // or a direct load); a failed load leaves it, and the history, intact.
  const std::uint32_t contentEpoch = world->content_epoch();
  const bool contentReplaced =
      cameraSampleValid && (cameraSampleEpoch != contentEpoch);
  if (contentReplaced) {
    cameraSampleValid = false;
    if (world->camera_manager().camera_count() == 0U) {
      // A replacement scene that publishes no camera of its own presents
      // the renderer's default view; the outgoing scene's last camera is
      // not a state this World ever established, and the audio listener
      // follows whatever camera is active here.
      renderer::set_active_camera(renderer::CameraState{});
    }
  }

  // The camera advances with the simulation: once per fixed step, with the
  // fixed delta, against that step's transforms. The steps before the last
  // were evaluated inside the graph; this is the last one's, now that its
  // transforms are composed. A frame that ran no step leaves the camera
  // and the sample pair alone and lets the render alpha carry the view
  // between them, as it carries every entity: evaluating here anyway
  // would collapse the pair onto one pose and hold the camera still for a
  // frame while the world kept moving under it. Without a valid pair
  // there is nothing to carry, so that frame still evaluates, with no
  // time passing.
  if (clock.stepsThisFrame > 0U) {
    evaluate_cameras_for_step(static_cast<float>(core::kFixedDeltaSeconds));
  } else if (!cameraSampleValid) {
    evaluate_cameras_for_step(0.0F);
  }
  cameraSampleEpoch = contentEpoch;
}

// ---------------------------------------------------------------------------
// Stage: render prep graph (render-prep/render phase jobs + command buffers)
// Returns false on fatal error; sets running = false internally.
// ---------------------------------------------------------------------------

bool EnginePipeline::Impl::stage_render_prep_graph() noexcept {
  if (consume_injected_stage_failure("render_prep_graph")) {
    running = false;
    return false;
  }
  if (!core::begin_frame_graph()) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to begin frame graph");
    running = false;
    return false;
  }

  std::size_t phaseJobCursor = 0U;
  bool graphFailed = false;

  core::JobHandle renderPrepPhaseHandle =
      submit_world_phase_job(frameContext.get(), world.get(), &phaseJobCursor,
                             &begin_render_prep_phase_job);
  if (!core::is_valid_handle(renderPrepPhaseHandle)) {
    graphFailed = true;
  }

  core::JobHandle renderPhaseHandle =
      submit_world_phase_job(frameContext.get(), world.get(), &phaseJobCursor,
                             &begin_render_phase_job);
  if (!core::is_valid_handle(renderPhaseHandle)) {
    graphFailed = true;
  }

  if (!graphFailed &&
      !link_dependency(renderPrepPhaseHandle, renderPhaseHandle)) {
    graphFailed = true;
  }

  core::JobHandle mergeHandle{};

  if (!graphFailed) {
    int vpW = 1;
    int vpH = 1;
    core::render_drawable_size(&vpW, &vpH);
    const float vpAspect =
        (vpH > 0) ? (static_cast<float>(vpW) / static_cast<float>(vpH)) : 1.0F;
    const renderer::CameraState cam = renderer::get_active_camera();
    // Shares the flush path's projection builder so CPU culling can never
    // disagree with the GPU frustum.
    const math::Mat4 vpMatrix =
        math::mul(renderer::camera_projection_matrix(cam, vpAspect),
                  math::look_at(cam.position, cam.target, cam.up));

    // Lights and captures are collected now, in the same mutation epoch
    // the draw list is built in, so render prep can keep the
    // camera-culled draws the shadow and capture passes need.
    collect_frame_scene_data();
    build_auxiliary_inputs();

    if (!runtime::enqueue_render_prep_pipeline(
            &frameContext->renderPrepPipeline, world.get(), commandBuffer.get(),
            assetDatabase.get(), meshRegistry.get(), renderPrepPhaseHandle,
            renderPhaseHandle, &frameContext->frameGraphFailed,
            &frameContext->droppedDrawCommands, frameThreadCount, kChunkSize,
            vpMatrix,
            isPlaying ? static_cast<float>(clock.renderAlpha) : 1.0F,
            &mergeHandle, auxiliaryCommandBuffer.get(),
            &frameAuxiliaryInputs)) {
      graphFailed = true;
    }
  }

  core::JobHandle endFrameHandle = submit_world_phase_job(
      frameContext.get(), world.get(), &phaseJobCursor, &end_frame_phase_job);
  if (!core::is_valid_handle(endFrameHandle)) {
    graphFailed = true;
  }

  if (!graphFailed && !link_dependency(mergeHandle, endFrameHandle)) {
    graphFailed = true;
  }

  if (graphFailed) {
    core::log_message(core::LogLevel::Error, "engine",
                      "job graph assembly failed");
    running = false;
    static_cast<void>(core::end_frame_graph());
    return false;
  }

  // end_frame_graph needs the whole graph drained, not one handle.
  core::wait_all();
  lastDroppedDrawCommands =
      frameContext->droppedDrawCommands.load(std::memory_order_acquire);
  if ((lastDroppedDrawCommands > 0U) && !droppedDrawsLogged) {
    droppedDrawsLogged = true;
    // Sized for the text plus a full ten-digit count.
    char dropMessage[256] = {};
    std::snprintf(dropMessage, sizeof(dropMessage),
                  "render prep dropped %u draws: more visible draws than a "
                  "command buffer holds, the frame is drawn incomplete "
                  "(reported once per run; EngineStats.droppedDrawCommands "
                  "carries the per-frame count)",
                  lastDroppedDrawCommands);
    core::log_message(core::LogLevel::Warning, "render_prep", dropMessage);
  }
  const bool frameJobsFailed =
      frameContext->frameGraphFailed.load(std::memory_order_acquire);
  if (!core::end_frame_graph()) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to end frame graph");
    running = false;
    return false;
  }

  if (frameJobsFailed) {
    core::log_message(core::LogLevel::Error, "engine",
                      "frame graph job execution failed");
    running = false;
    return false;
  }

  return true;
}

/// Snapshots the lights and capture requests the draw list is built
/// against, before the post-frame flush can change the world.
void EnginePipeline::Impl::collect_frame_scene_data() noexcept {
  frameSceneLights = collect_scene_lights(*world);
  frameCaptureRequestCount = collect_scene_captures(
      *world, frameCaptureRequests.data(), renderer::kMaxSceneCaptures);
  frameCollectionValid = true;
}

/// Derives what render prep must keep beyond the camera frustum from the
/// collected lights and captures.
void EnginePipeline::Impl::build_auxiliary_inputs() noexcept {
  runtime::RenderPrepAuxiliaryInputs &inputs = frameAuxiliaryInputs;
  inputs = runtime::RenderPrepAuxiliaryInputs{};
  if (frameSceneLights.directionalLightCount > 0U) {
    const math::Vec3 &direction = frameSceneLights.directionalLights[0].direction;
    const float length = math::length(direction);
    if (length > 1.0e-6F) {
      inputs.directionalShadow = true;
      inputs.lightDirection = math::mul(direction, 1.0F / length);
      inputs.sweepDistance = renderer::kShadowCasterSweepDistance;
    }
  }
  for (std::size_t i = 0U; i < frameSceneLights.pointLightCount; ++i) {
    const renderer::PointLightData &light = frameSceneLights.pointLights[i];
    if (light.castShadow &&
        (inputs.localCasterCount < inputs.localCasters.size())) {
      inputs.localCasters[inputs.localCasterCount++] = {light.position,
                                                        light.radius};
    }
  }
  for (std::size_t i = 0U; i < frameSceneLights.spotLightCount; ++i) {
    const renderer::SpotLightData &light = frameSceneLights.spotLights[i];
    if (light.castShadow &&
        (inputs.localCasterCount < inputs.localCasters.size())) {
      inputs.localCasters[inputs.localCasterCount++] = {light.position,
                                                        light.radius};
    }
  }
  for (std::size_t i = 0U; i < frameCaptureRequestCount; ++i) {
    const renderer::SceneCaptureRequest &request = frameCaptureRequests[i];
    const float aspect =
        (request.height > 0U)
            ? (static_cast<float>(request.width) /
               static_cast<float>(request.height))
            : 1.0F;
    inputs.captureViewProjections[inputs.captureCount++] = math::mul(
        renderer::camera_projection_matrix(request.camera, aspect),
        math::look_at(request.camera.position, request.camera.target,
                      request.camera.up));
  }
}

// ---------------------------------------------------------------------------
// Stage: post-frame (collision callbacks, end-play, deferred mutations)
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_post_frame() noexcept {
  if (runPhysics) {
    runtime::dispatch_collision_callbacks(*world);
  }

  if (isPlaying || (world->pending_destroy_count() > 0U)) {
    world->begin_end_play_phase();
    if (isPlaying) {
      scripting::dispatch_entity_scripts_end_play(world.get());
    }
    world->end_end_play_phase();
  }

  scripting::flush_deferred_mutations();
}

// ---------------------------------------------------------------------------
// Stage: measure frame
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_measure_frame() noexcept {
  if (runFrameGraph) {
    const auto frameGraphEnd = Clock::now();
    frameMs =
        std::chrono::duration<double, std::milli>(frameGraphEnd - frameStart)
            .count();

    jobStats = core::consume_job_stats();
    const auto frameNs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(frameGraphEnd -
                                                             frameStart)
            .count());
    const double totalCapacityNs =
        frameNs * static_cast<double>(frameThreadCount);
    utilizationPct =
        (totalCapacityNs > 0.0)
            ? ((100.0 * static_cast<double>(jobStats.busyNanoseconds)) /
               totalCapacityNs)
            : 0.0;
  } else {
    frameMs =
        std::chrono::duration<double, std::milli>(Clock::now() - frameStart)
            .count();
    jobStats = core::consume_job_stats();
    utilizationPct = 0.0;
  }
}

// ---------------------------------------------------------------------------
// Stage: render
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_render() noexcept {
  // Device reach: the effective scene render scale is the user's
  // base scale times the dynamic controller's factor; the controller
  // steps against the presented frame budget (r_max_fps, else 60 Hz).
  {
    const float baseScale = renderScaleCvar.get_float(1.0F);
    float dynamicFactor = 1.0F;
    if (dynamicResolutionCvar.get_bool(false)) {
      const int maxFps = maxFpsCvar.get_int(0);
      const float targetMs = (maxFps > 0)
                                 ? (1000.0F / static_cast<float>(maxFps))
                                 : (1000.0F / 60.0F);
      dynamicFactor = renderer::dynamic_resolution_step(
          dynamicResolution, static_cast<float>(wallFrameMs), targetMs,
          dynamicResolutionMinCvar.get_float(0.5F));
    } else {
      dynamicResolution = renderer::DynamicResolutionState{};
    }
    renderer::set_render_scale(baseScale * dynamicFactor);
  }

  const bool interpolateCamera =
      isPlaying && cameraSampleValid && (clock.renderAlpha < 1.0);
  if (interpolateCamera) {
    renderer::set_active_camera(interpolate_camera_state(
        previousCameraSample, currentCameraSample,
        static_cast<float>(clock.renderAlpha)));
  }

  // The listener sits at the camera, so panning matches what is on
  // screen. A third-person camera is then metres from the character it
  // follows, which is why a positional sound keeps full volume out to
  // audio::kDefaultMinAudibleDistance instead of falling off from one
  // metre as the mixer's own default would.
  const renderer::CameraState listenerCamera = renderer::get_active_camera();
  audio::set_listener(
      listenerCamera.position,
      math::sub(listenerCamera.target, listenerCamera.position),
      listenerCamera.up);

  if ((bridge != nullptr) && (bridge->new_frame != nullptr)) {
    bridge->new_frame();
  }

  // A frame without the frame graph (nothing ran render prep) has had no
  // flush since the last submission either, so collecting here keeps the
  // single-epoch rule.
  if (!frameCollectionValid) {
    collect_frame_scene_data();
  }
  renderer::set_scene_capture_requests(frameCaptureRequests.data(),
                                       frameCaptureRequestCount);

  renderer::flush_renderer(commandBuffer->view(), meshRegistry.get(),
                           static_cast<float>(clock.simulationSeconds),
                           frameSceneLights, auxiliaryCommandBuffer->view());
  frameCollectionValid = false;

  if ((bridge != nullptr) && (bridge->render != nullptr)) {
    bridge->render(static_cast<float>(frameMs),
                   static_cast<float>(utilizationPct));
  }
  renderer::present_render_device();

  if (interpolateCamera) {
    renderer::set_active_camera(currentCameraSample);
  }
}

// ---------------------------------------------------------------------------
// Stage: scene commit (pending script scene op)
// ---------------------------------------------------------------------------

// A queued load_scene/new_scene commits only after the frame's render
// submission is complete. Every scene-derived input to one submitted frame
// (the camera and render-prep command buffer built before this point, and
// the lights and capture requests the render stage collects at flush time)
// must come from one World content epoch; committing here keeps the
// outgoing World live through the whole submission, and the replacement
// World's first frame builds everything from itself, with stage_camera
// retiring the camera history on the epoch change. The end-play dispatch
// and deferred-mutation flush stay ahead of the render in stage_post_frame,
// so a mutation a handler defers is still applied before this commit
// decides what content the transition replaces.
void EnginePipeline::Impl::stage_scene_commit() noexcept {
  static_cast<void>(runtime::process_pending_scene_op(*world));
}

// ---------------------------------------------------------------------------
// Stage: diagnostics
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_diagnostics() noexcept {
  // The stats panel/overlay surface these values live; keep the console
  // traces at ~1 Hz so per-frame printf calls cannot throttle the loop.
  const auto now = Clock::now();
  const bool logTraceThisFrame =
      (now - lastMetricsLogTime) >= std::chrono::seconds(1);
  if (logTraceThisFrame) {
    lastMetricsLogTime = now;
    std::size_t threadFrameBytes = 0U;
    std::size_t threadFrameAllocs = 0U;
    for (std::size_t i = 0U; i < frameThreadCount; ++i) {
      threadFrameBytes += core::thread_frame_allocator_bytes_used(i);
      threadFrameAllocs += core::thread_frame_allocator_allocation_count(i);
    }

    core::log_frame_metrics(
        clock.frameIndex, frameMs,
        core::frame_allocator_bytes_used() + threadFrameBytes,
        core::frame_allocator_allocation_count() + threadFrameAllocs);
  }

  const std::size_t aliveCount = world->alive_entity_count();
  const std::size_t spawnedCount = (aliveCount >= previousAliveCount)
                                       ? (aliveCount - previousAliveCount)
                                       : 0U;
  const std::size_t destroyedCount = (previousAliveCount > aliveCount)
                                         ? (previousAliveCount - aliveCount)
                                         : 0U;

  const MeshAssetStateCounts assetCounts =
      count_mesh_asset_states(assetDatabase.get());

  const bool shouldLogSliceDiagnostics =
      ((clock.frameIndex % kSliceDiagnosticsPeriodFrames) == 0U) ||
      (spawnedCount > 0U) || (destroyedCount > 0U) || (assetCounts.failed > 0U);
  if (shouldLogSliceDiagnostics) {
    const std::size_t movingRigidBodyCount = count_moving_rigid_bodies(*world);
    const std::size_t meshComponentCount = count_mesh_components(*world);
    const std::size_t readyMeshComponentCount =
        count_ready_mesh_components(*world, assetDatabase.get());
    const std::size_t pendingAssetRequests =
        renderer::pending_asset_request_count(assetManager.get()) +
        content::pending_load_count(assetStreamingQueue.get());

    char diagnostics[640] = {};
    std::snprintf(
        diagnostics, sizeof(diagnostics),
        "frame=%u phase=%s alive=%llu spawned=%llu destroyed=%llu "
        "transforms=%llu worldTransforms=%llu movingBodies=%llu "
        "meshComponents=%llu readyMeshComponents=%llu drawCommands=%llu "
        "assetsReady=%llu assetsLoading=%llu assetsFailed=%llu "
        "assetRequests=%llu updateSteps=%llu",
        clock.frameIndex, world_phase_to_string(world->current_phase()),
        static_cast<unsigned long long>(aliveCount),
        static_cast<unsigned long long>(spawnedCount),
        static_cast<unsigned long long>(destroyedCount),
        static_cast<unsigned long long>(world->transform_count()),
        static_cast<unsigned long long>(world->world_transform_count()),
        static_cast<unsigned long long>(movingRigidBodyCount),
        static_cast<unsigned long long>(meshComponentCount),
        static_cast<unsigned long long>(readyMeshComponentCount),
        static_cast<unsigned long long>(commandBuffer->command_count()),
        static_cast<unsigned long long>(assetCounts.ready),
        static_cast<unsigned long long>(assetCounts.loading),
        static_cast<unsigned long long>(assetCounts.failed),
        static_cast<unsigned long long>(pendingAssetRequests),
        static_cast<unsigned long long>(clock.stepsThisFrame));
    core::log_message(core::LogLevel::Info, "slice", diagnostics);
  }

  renderer::RendererFrameStats rendererStats =
      renderer::renderer_get_last_frame_stats();

  core::EngineStats frameStats{};
  frameStats.frameTimeMs = static_cast<float>(frameMs);
  // FPS reports the presented frame-to-frame rate (includes vsync and the
  // r_max_fps wait); frameTimeMs stays the busy cost of the frame. The
  // published value is a half-second windowed average — the instantaneous
  // 1/dt readout swings +-4 FPS at 60 Hz on 1 ms of present jitter, which
  // reads as instability the pacing does not have.
  const double presentedMs = (wallFrameMs > 0.0) ? wallFrameMs : frameMs;
  fpsWindowSeconds += presentedMs / 1000.0;
  ++fpsWindowFrames;
  if ((fpsWindowSeconds >= 0.5) || (memoryUsedMbSample < 0.0F)) {
    memoryUsedMbSample = static_cast<float>(
        static_cast<double>(core::process_memory_bytes()) / (1024.0 * 1024.0));
  }
  if (fpsWindowSeconds >= 0.5) {
    smoothedFps = static_cast<float>(static_cast<double>(fpsWindowFrames) /
                                     fpsWindowSeconds);
    fpsWindowSeconds = 0.0;
    fpsWindowFrames = 0U;
  }
  frameStats.fps =
      (smoothedFps > 0.0F)
          ? smoothedFps
          : ((presentedMs > 0.0) ? static_cast<float>(1000.0 / presentedMs)
                                 : 0.0F);
  frameStats.drawCalls = rendererStats.drawCalls;
  frameStats.triCount = rendererStats.triangleCount;
  frameStats.entityCount = aliveCount;
  frameStats.memoryUsedMb = memoryUsedMbSample;
  frameStats.gpuSceneMs = rendererStats.gpuSceneMs;
  frameStats.gpuTonemapMs = rendererStats.gpuTonemapMs;
  frameStats.jobUtilizationPct = static_cast<float>(utilizationPct);
  frameStats.droppedDrawCommands = lastDroppedDrawCommands;
  frameStats.sceneLights = static_cast<std::uint32_t>(
      frameSceneLights.pointLightCount + frameSceneLights.spotLightCount);
  frameStats.drawCommands =
      static_cast<std::uint32_t>(commandBuffer->command_count());
  {
    const renderer::CommandBufferView auxiliary = auxiliaryCommandBuffer->view();
    std::uint32_t casters = 0U;
    std::uint32_t captureOnly = 0U;
    for (std::uint32_t i = 0U; i < auxiliary.count; ++i) {
      const std::uint16_t mask = auxiliary.data[i].passMask;
      casters += ((mask & renderer::kPassShadowCaster) != 0U) ? 1U : 0U;
      captureOnly += (mask >= renderer::kPassCaptureBase) ? 1U : 0U;
    }
    frameStats.offscreenShadowCasters = casters;
    frameStats.captureOnlyDraws = captureOnly;
    frameStats.hotReloadPolls = frameHotReloadPolls;
  }
  frameStats.fixedSteps = clock.stepsThisFrame;
  frameStats.interpolationAlpha = static_cast<float>(clock.renderAlpha);
  core::set_engine_stats(frameStats);

  if (logTraceThisFrame) {
    char jobMessage[192] = {};
    std::snprintf(
        jobMessage, sizeof(jobMessage),
        "jobs=%llu busyMs=%.3f utilization=%.2f%% queueContention=%llu",
        static_cast<unsigned long long>(jobStats.jobsExecuted),
        static_cast<double>(jobStats.busyNanoseconds) / 1000000.0,
        utilizationPct,
        static_cast<unsigned long long>(jobStats.queueContentionCount));
    core::log_message(core::LogLevel::Trace, "jobs", jobMessage);
  }
}

// ---------------------------------------------------------------------------
// Stage: frame cleanup
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_frame_cleanup() noexcept {
  core::reset_frame_allocator();
  core::reset_thread_frame_allocators();

  previousPlayState = playState;
  previousAliveCount = world->alive_entity_count();
  ++clock.frameIndex;
  if ((maxFrames != 0U) && (clock.frameIndex >= maxFrames)) {
    running = false;
  }

  if (!core::is_platform_running()) {
    running = false;
  }
}

// ---------------------------------------------------------------------------
// Stage: frame pacing (must stay last: waits out the r_max_fps budget)
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_frame_pacing() noexcept {
#if defined(ENGINE_PLATFORM_WEB)
  // The browser's animation loop paces the frame; a wait here would spin
  // the page's only thread.
  return;
#else
  const int maxFps = maxFpsCvar.get_int(0);
  if (maxFps <= 0) {
    return;
  }
  const double elapsedSeconds =
      std::chrono::duration<double>(Clock::now() - frameStart).count();
  runtime::wait_for_frame_cap(
      runtime::frame_cap_wait_seconds(elapsedSeconds, maxFps));
#endif
}

// ===========================================================================
// EnginePipeline forwarding methods
// ===========================================================================

EnginePipeline::EnginePipeline() noexcept = default;

EnginePipeline::~EnginePipeline() noexcept { teardown(); }

bool EnginePipeline::initialize(std::uint32_t maxFrames) noexcept {
  // A run publishes aliases into its own Impl's storage (the editor world,
  // the scripting service locator, the editor asset service), so the run
  // being replaced is closed before the replacement publishes over it.
  if (m_impl) {
    core::log_message(core::LogLevel::Warning, "engine",
                      "pipeline initialize replaced a run that was still "
                      "open; closing the previous run first");
    teardown();
  }

  m_impl.reset(new (std::nothrow) Impl());
  if (!m_impl) {
    return false;
  }
  m_impl->frameDeltaOverrideSeconds = m_frameDeltaOverrideSeconds;
  if (!m_impl->initialize(maxFrames)) {
    // Initialization stops at its first failure, which may be past the point
    // where the run published; closing regardless keeps that independent of
    // which step failed.
    m_impl->teardown();
    m_impl.reset();
    return false;
  }
  return true;
}

bool EnginePipeline::execute_frame() noexcept {
  return m_impl && m_impl->execute_frame();
}

bool EnginePipeline::had_fatal_error() const noexcept {
  return m_impl && m_impl->fatalError;
}

runtime::World *EnginePipeline::world() noexcept {
  return m_impl ? m_impl->world.get() : nullptr;
}

bool EnginePipeline::set_frame_delta_override(double seconds) noexcept {
  if (!(seconds >= 0.0) || !std::isfinite(seconds)) {
    core::log_message(core::LogLevel::Warning, "engine",
                      "frame delta override refused: not a finite, "
                      "non-negative number of seconds");
    return false;
  }
  m_frameDeltaOverrideSeconds = seconds;
  if (m_impl) {
    m_impl->frameDeltaOverrideSeconds = seconds;
  }
  return true;
}

void EnginePipeline::clear_frame_delta_override() noexcept {
  m_frameDeltaOverrideSeconds = -1.0;
  if (m_impl) {
    m_impl->frameDeltaOverrideSeconds = -1.0;
  }
}

void EnginePipeline::teardown() noexcept {
  if (m_impl) {
    m_impl->teardown();
  }
  m_impl.reset();
}

} // namespace engine
