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
#include "engine/core/engine_stats.h"
#include "engine/core/input.h"
#include "engine/core/job_system.h"
#include "engine/core/logging.h"
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
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/mesh_primitives.h"
#include "engine/physics/physics_context.h"
#include "engine/renderer/shader_system.h"
#include "engine/renderer/texture_loader.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/game_binding_state.h"
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
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"
#include "spatial_transform_util.h"

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

/// Declared scene-transition reset order (audit H-16, extended by #198):
/// (0) on_end_play — load_scene/reset_world call this back once the
/// transition is guaranteed to commit but before it destructively touches
/// the outgoing world, so every outgoing scripted entity still alive and
/// callable receives on_end_play, matching editor Stop and pre-existing
/// destroy-driven EndPlay; (1) commit the replacement world — staged
/// load_scene or reset_world, which already resets the world-owned
/// managers in the H-18 order; (2) clear the outgoing scene's coroutines,
/// timer callback refs, entity pools, and per-entity script modules so no
/// stale entity identity or Lua reference can act on the new world
/// (globals persist by contract as the cross-scene handoff channel;
/// #93a/#93b: the World-owned TimerManager was already reset in step 1,
/// but only these scripting-side calls drop the Lua registry refs and pool
/// slots that point at it); (3) clear the pending op. A failed load skips
/// every reset, including step 0, and leaves all state unchanged.
static void dispatch_outgoing_scene_end_play() noexcept {
  scripting::dispatch_entity_scripts_end_for_transition();
}

/// Processes a queued script scene operation, if one exists.
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
      core::log_message(core::LogLevel::Error, "engine",
                        "failed to process pending scene load");
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

constexpr double kFixedDeltaSeconds = 1.0 / 60.0;
constexpr std::size_t kChunkSize = 256U;
constexpr std::size_t kMaxUpdateStepsPerFrame = 8U;
static_assert(kMaxUpdateStepsPerFrame <= physics::kMaxCollisionFrameSteps,
              "the frame collision buffer must cover every catch-up step so "
              "accumulation alone never drops callbacks (#103)");
constexpr std::size_t kMaxChunkJobs = 1024U;
constexpr std::size_t kMaxPhaseJobs = kMaxUpdateStepsPerFrame * 2U + 4U;
constexpr std::uint32_t kSliceDiagnosticsPeriodFrames = 60U;

/// Production MaterialTextureLoadFn: the same synchronous GL texture loader
/// every other texture consumer (skybox, character textures) already calls.
/// Only ever invoked from stage_assets, which has just confirmed a GL
/// context is current.
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
core::JobHandle submit_world_phase_job(FrameContext *frameContext,
                                       runtime::World *world,
                                       std::size_t *phaseJobCursor,
                                       core::JobFunction function) noexcept {
  if ((frameContext == nullptr) || (world == nullptr) ||
      (phaseJobCursor == nullptr) ||
      (*phaseJobCursor >= frameContext->phaseJobData.size())) {
    return {};
  }

  WorldPhaseJobData &jobData = frameContext->phaseJobData[*phaseJobCursor];
  ++(*phaseJobCursor);
  jobData.world = world;

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

void process_input_events_with_editor() noexcept {
  core::begin_input_frame();

  const runtime::EditorBridge *bridge = runtime::editor_bridge();

  SDL_Event event{};
  while (SDL_PollEvent(&event)) {
    const runtime::InputEventRoute route =
        runtime::process_editor_input_event(bridge, &event);
    if (route == runtime::InputEventRoute::QuitRequested) {
      // Issue #158: the editor gets a chance to defer the quit behind its
      // own unsaved-change confirm flow; a null hook or a bound-but-clean
      // document both proceed immediately, matching the pre-#158 behavior.
      const bool proceedNow = (bridge == nullptr) ||
                              (bridge->handle_quit_request == nullptr) ||
                              bridge->handle_quit_request();
      if (proceedNow) {
        // #241 (owner decision 2026-08-19): a quit that ends a live play
        // session dispatches on_end_play exactly like editor Stop and
        // scene transitions. The editor's quit hook has already routed
        // through the Stop flow (play state reads Stopped here, and
        // stage_play_transitions dispatches later this frame); only a
        // still-playing session — standalone runtime, or a bridge with no
        // stop routing — takes this direct dispatch.
        if (query_editor_play_state() == LoopPlayState::Playing) {
          scripting::dispatch_entity_scripts_end();
        }
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
  runtime::GameBindingState gameBindingState{};
  std::unique_ptr<runtime::World> world;
  std::unique_ptr<renderer::CommandBufferBuilder> commandBuffer;
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
  runtime::EngineRendererService rendererService{};

  // --- External references ---
  const runtime::EditorBridge *bridge = nullptr;

  // --- Timing state ---
  Clock::time_point previousTick{};
  Clock::time_point frameStart{};
  // Last time the frame-metrics trace line was written (rate-limited).
  Clock::time_point lastMetricsLogTime{};
  double accumulator = 0.0;
  double simulationTimeSeconds = 0.0;

  // --- Loop state ---
  std::uint32_t frameIndex = 0U;
  std::uint32_t maxFrames = 0U;
  bool running = true;
  // Distinguishes fatal loop exits from graceful stops for engine::run (#96).
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
  int appliedVsync = 1;
  double renderAlpha = 1.0;
  Clock::time_point previousFrameStart{};
  double wallFrameMs = 0.0;
  renderer::CameraState previousCameraSample{};
  renderer::CameraState currentCameraSample{};
  bool cameraSampleValid = false;
  std::size_t updateStepCount = 0U;
  double frameMs = 0.0;
  double utilizationPct = 0.0;
  core::JobSystemStats jobStats{};

  /// Total simulated time this frame: the dt every per-frame gameplay system
  /// receives, so one dispatch still accounts for every catch-up step.
  double step_seconds() const noexcept {
    return static_cast<double>(updateStepCount) * kFixedDeltaSeconds;
  }

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
  bool stage_render_prep_graph() noexcept;
  // True once when dbg_fail_frame_stage names this stage (test seam, #120).
  bool consume_injected_stage_failure(const char *stageName) noexcept;
  void stage_post_frame() noexcept;
  void stage_measure_frame() noexcept;
  void stage_render() noexcept;
  void stage_diagnostics() noexcept;
  void stage_frame_cleanup() noexcept;
  void stage_frame_pacing() noexcept;
};

EnginePipeline::Impl::Impl() noexcept : serviceRegistry(serviceLocator) {}

// ---------------------------------------------------------------------------
// Impl::initialize
// ---------------------------------------------------------------------------

bool EnginePipeline::Impl::initialize(std::uint32_t maxFrameCount) noexcept {
  maxFrames = maxFrameCount;

  world.reset(new (std::nothrow) runtime::World());
  commandBuffer.reset(new (std::nothrow) renderer::CommandBufferBuilder());
  meshRegistry.reset(new (std::nothrow) renderer::GpuMeshRegistry());
  assetDatabase.reset(new (std::nothrow) renderer::AssetDatabase());
  assetManager.reset(new (std::nothrow) renderer::AssetManager());
  assetStreamingQueue.reset(new (std::nothrow) content::AssetStreamingQueue());
  assetStreamingState.reset(new (std::nothrow) RuntimeAssetStreamingState());

  if (!world || !commandBuffer || !meshRegistry || !assetDatabase ||
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

  bridge = runtime::editor_bridge();
  runtime::set_editor_asset_service(&assetDatabaseService);

  runtime::bind_scripting_runtime(world.get(), serviceLocator);
  // The run's game-binding state is pipeline-owned (#168 M3); the binding
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
  simulationTimeSeconds = 0.0;
  frameIndex = 0U;
  running = true;
  previousPlayState = query_editor_play_state();
  previousAliveCount = world->alive_entity_count();
  core::reset_engine_stats();

  return true;
}

// ---------------------------------------------------------------------------
// Impl::execute_frame
// ---------------------------------------------------------------------------

bool EnginePipeline::Impl::execute_frame() noexcept {
  core::profiler_begin_frame();
  // Published before any stage runs so every log_message call this frame
  // (including early stages ahead of stage_scripting) tags itself with the
  // right index for the editor Console's frame-context column (issue #155).
  core::log_set_frame_index(frameIndex);
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
  if ((bridge != nullptr) && (bridge->set_world != nullptr)) {
    bridge->set_world(nullptr);
  }

  // Run-scoped residue must not leak into a later pipeline run (#168): the
  // scripting run state and the animation controller registry are reset
  // while the VM and bindings are still alive, before anything unbinds,
  // then the engine-tier subsystems drop their run-scoped content (script
  // input bindings, scene audio, per-run renderer state).
  scripting::reset_run_state();
  runtime::reset_anim_controllers();
  core::clear_gameplay_bindings();
  audio::unload_all_sounds();
  renderer::reset_renderer_public_state();

  runtime::set_editor_asset_service(nullptr);
  scripting::bind_game_state(nullptr);
  runtime::unbind_scripting_runtime(serviceLocator);
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
  process_input_events_with_editor();
}

// ---------------------------------------------------------------------------
// Stage: play transitions
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_play_transitions() noexcept {
  playState = query_editor_play_state();

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
    simulationTimeSeconds = 0.0;
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
    accumulator += std::chrono::duration<double>(now - previousTick).count();
    previousTick = now;
  } else {
    previousTick = frameStart;
  }

  const runtime::FixedStepDecision decision = runtime::fixed_step_decision(
      isPlaying, singleStepping, accumulator, kFixedDeltaSeconds,
      kMaxUpdateStepsPerFrame);
  updateStepCount = decision.stepCount;
  accumulator = decision.remainingAccumulator;
  simulationTimeSeconds +=
      static_cast<double>(updateStepCount) * kFixedDeltaSeconds;
  renderAlpha = (isPlaying && !singleStepping)
                    ? accumulator / kFixedDeltaSeconds
                    : 1.0;
}

// ---------------------------------------------------------------------------
// Stage: scripting
//
// Cadence contract (audit M-01). Two classes of system exist in this frame:
//   * per-fixed-step: transform propagation, physics, collision resolve, and
//     animation each run exactly updateStepCount times with kFixedDeltaSeconds
//     apiece, so their integration is independent of the render rate.
//   * per-frame: entity script on_tick, Lua timers, coroutines, spring arms,
//     and camera evaluation run once per rendered frame. They are dispatched
//     once — re-entrant script dispatch per catch-up step would multiply
//     gameplay callbacks and their deferred mutations — but they receive
//     step_seconds(), the total time simulated this frame, so their dt equals
//     the time the world actually advanced. Passing the bare fixed delta made
//     timers and script-driven motion run slow whenever catch-up stepped more
//     than once.
// Spring-arm/camera work runs in stage_camera, between the last fixed step
// and render prep, so culling and interpolation see this frame's camera.
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_scripting() noexcept {
  scripting::set_frame_index(frameIndex);

  if (isPlaying && (updateStepCount > 0U)) {
    scripting::set_frame_time(static_cast<float>(step_seconds()),
                              static_cast<float>(simulationTimeSeconds));
    scripting::tick_timers();
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

  if (!core::make_render_context_current()) {
    core::log_message(core::LogLevel::Warning, "assets",
                      "skipping asset transitions: OpenGL context unavailable");
  } else {
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
    // textures's header comment). Requires the GL context just confirmed
    // current above.
    static_cast<void>(renderer::resolve_material_textures(
        assetDatabase.get(), &load_material_texture_production, nullptr));
    core::release_render_context();
  }

  const int cacheMb = core::cvar_get_int("asset.cache_size_mb", 512);
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
  for (std::size_t step = 0U; step < updateStepCount; ++step) {
    runtime::update_animations(*world, static_cast<float>(kFixedDeltaSeconds));
    scripting::dispatch_animation_event_callbacks();
  }
}

// ---------------------------------------------------------------------------
// Fault-injection seam (issue #120): dbg_fail_frame_stage forces the named
// graph stage to report a fatal failure through its production return path.
// The cvar self-clears so the injected failure fires exactly once per set.
// ---------------------------------------------------------------------------

bool EnginePipeline::Impl::consume_injected_stage_failure(
    const char *stageName) noexcept {
  const char *requested = core::cvar_get_string("dbg_fail_frame_stage", "");
  if ((requested == nullptr) || (requested[0] == '\0') ||
      (std::strcmp(requested, stageName) != 0)) {
    return false;
  }
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
  if (updateStepCount == 0U) {
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

  for (std::size_t step = 0U; step < updateStepCount; ++step) {
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
      beginStepHandle =
          submit_world_phase_job(frameContext.get(), world.get(),
                                 &phaseJobCursor, &begin_update_step_job);
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
      updateData.deltaSeconds = static_cast<float>(kFixedDeltaSeconds);

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
        physicsData.deltaSeconds = static_cast<float>(kFixedDeltaSeconds);
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
          static_cast<float>(kFixedDeltaSeconds);
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

  // end_frame_graph needs the whole graph drained, not one handle (#109).
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

void EnginePipeline::Impl::stage_camera() noexcept {
  world->begin_transform_phase();

  if (!isPlaying) {
    return;
  }

  runtime::update_spring_arm_cameras(*world,
                                     static_cast<float>(step_seconds()));
  runtime::update_persistent_cameras(*world,
                                     static_cast<float>(step_seconds()));
  math::Vec3 camPos, camTarget, camUp;
  float camFov = 0.0F;
  float camNear = 0.0F;
  float camFar = 0.0F;
  world->camera_manager().evaluate(static_cast<float>(step_seconds()),
                                   &camPos, &camTarget, &camUp, &camFov,
                                   &camNear, &camFar);
  if (world->camera_manager().camera_count() > 0U) {
    renderer::CameraState cam{};
    cam.position = camPos;
    cam.target = camTarget;
    cam.up = camUp;
    cam.fovRadians = camFov;
    cam.nearPlane = camNear;
    cam.farPlane = camFar;
    renderer::set_active_camera(cam);
  }

  previousCameraSample =
      cameraSampleValid ? currentCameraSample : renderer::get_active_camera();
  currentCameraSample = renderer::get_active_camera();
  cameraSampleValid = true;
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
    const math::Mat4 vpMatrix =
        math::mul(math::perspective(cam.fovRadians, vpAspect, cam.nearPlane,
                                    cam.farPlane),
                  math::look_at(cam.position, cam.target, cam.up));

    if (!runtime::enqueue_render_prep_pipeline(
            &frameContext->renderPrepPipeline, world.get(), commandBuffer.get(),
            assetDatabase.get(), meshRegistry.get(), renderPrepPhaseHandle,
            renderPhaseHandle, &frameContext->frameGraphFailed,
            frameThreadCount, kChunkSize, vpMatrix,
            isPlaying ? static_cast<float>(renderAlpha) : 1.0F,
            &mergeHandle)) {
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

  // end_frame_graph needs the whole graph drained, not one handle (#109).
  core::wait_all();
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

// ---------------------------------------------------------------------------
// Stage: post-frame (collision callbacks, end-play, scene ops)
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

  static_cast<void>(runtime::process_pending_scene_op(*world));
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
  if (!core::make_render_context_current()) {
    core::log_message(core::LogLevel::Error, "editor",
                      "failed to acquire OpenGL context for editor");
    fatalError = true;
    running = false;
    return;
  }

  const int requestedVsync = runtime::normalize_vsync_interval(
      core::cvar_get_int("r_vsync", 1));
  if (requestedVsync != appliedVsync) {
    appliedVsync = requestedVsync;
    static_cast<void>(core::set_render_vsync(requestedVsync));
  }

  const bool interpolateCamera =
      isPlaying && cameraSampleValid && (renderAlpha < 1.0);
  if (interpolateCamera) {
    renderer::set_active_camera(interpolate_camera_state(
        previousCameraSample, currentCameraSample,
        static_cast<float>(renderAlpha)));
  }

  const renderer::CameraState listenerCamera = renderer::get_active_camera();
  audio::set_listener(
      listenerCamera.position,
      math::sub(listenerCamera.target, listenerCamera.position),
      listenerCamera.up);

  if ((bridge != nullptr) && (bridge->new_frame != nullptr)) {
    bridge->new_frame();
  }

  const renderer::SceneLightData sceneLights = collect_scene_lights(*world);

  renderer::SceneCaptureRequest captureRequests[renderer::kMaxSceneCaptures]{};
  const std::size_t captureRequestCount = collect_scene_captures(
      *world, captureRequests, renderer::kMaxSceneCaptures);
  renderer::set_scene_capture_requests(captureRequests, captureRequestCount);

  renderer::flush_renderer(commandBuffer->view(), meshRegistry.get(),
                           static_cast<float>(simulationTimeSeconds),
                           sceneLights);

  if ((bridge != nullptr) && (bridge->render != nullptr)) {
    bridge->render(static_cast<float>(frameMs),
                   static_cast<float>(utilizationPct));
  }
  core::swap_render_buffers();
  core::release_render_context();

  if (interpolateCamera) {
    renderer::set_active_camera(currentCameraSample);
  }
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
        frameIndex, frameMs,
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
      ((frameIndex % kSliceDiagnosticsPeriodFrames) == 0U) ||
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
        frameIndex, world_phase_to_string(world->current_phase()),
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
        static_cast<unsigned long long>(updateStepCount));
    core::log_message(core::LogLevel::Info, "slice", diagnostics);
  }

  renderer::RendererFrameStats rendererStats =
      renderer::renderer_get_last_frame_stats();

  core::EngineStats frameStats{};
  frameStats.frameTimeMs = static_cast<float>(frameMs);
  // FPS reports the presented frame-to-frame rate (includes vsync and the
  // r_max_fps wait); frameTimeMs stays the busy cost of the frame.
  const double presentedMs = (wallFrameMs > 0.0) ? wallFrameMs : frameMs;
  frameStats.fps =
      (presentedMs > 0.0) ? static_cast<float>(1000.0 / presentedMs) : 0.0F;
  frameStats.drawCalls = rendererStats.drawCalls;
  frameStats.triCount = rendererStats.triangleCount;
  frameStats.entityCount = aliveCount;
  frameStats.memoryUsedMb = static_cast<float>(
      static_cast<double>(core::process_memory_bytes()) / (1024.0 * 1024.0));
  frameStats.gpuSceneMs = rendererStats.gpuSceneMs;
  frameStats.gpuTonemapMs = rendererStats.gpuTonemapMs;
  frameStats.jobUtilizationPct = static_cast<float>(utilizationPct);
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
  ++frameIndex;
  if ((maxFrames != 0U) && (frameIndex >= maxFrames)) {
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
  const int maxFps = core::cvar_get_int("r_max_fps", 0);
  if (maxFps <= 0) {
    return;
  }
  const double elapsedSeconds =
      std::chrono::duration<double>(Clock::now() - frameStart).count();
  runtime::wait_for_frame_cap(
      runtime::frame_cap_wait_seconds(elapsedSeconds, maxFps));
}

// ===========================================================================
// EnginePipeline forwarding methods
// ===========================================================================

EnginePipeline::EnginePipeline() noexcept = default;
EnginePipeline::~EnginePipeline() noexcept = default;

bool EnginePipeline::initialize(std::uint32_t maxFrames) noexcept {
  m_impl.reset(new (std::nothrow) Impl());
  if (!m_impl) {
    return false;
  }
  if (!m_impl->initialize(maxFrames)) {
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

void EnginePipeline::teardown() noexcept {
  if (m_impl) {
    m_impl->teardown();
  }
  m_impl.reset();
}

} // namespace engine
