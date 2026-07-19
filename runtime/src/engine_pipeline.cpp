// Implements engine pipeline behavior for the Engine runtime world.

#include "engine/runtime/engine_pipeline.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__)) &&        \
    !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H // NOLINT(bugprone-reserved-identifier)
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#include "engine/audio/audio.h"
#include "engine/engine.h"
#include "engine/core/bootstrap.h"
#include "engine/core/engine_stats.h"
#include "engine/core/input.h"
#include "engine/core/job_system.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/profiler.h"
#include "engine/core/vfs.h"
#include "engine/math/transform.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/renderer/asset_streaming.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/mesh_primitives.h"
#include "engine/renderer/shader_system.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/render_prep_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/spring_arm_update.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

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

  if (event->type == SDL_QUIT) {
    return InputEventRoute::QuitRequested;
  }

  const bool keyboardEvent =
      (event->type == SDL_KEYDOWN) || (event->type == SDL_KEYUP) ||
      (event->type == SDL_TEXTINPUT) || (event->type == SDL_TEXTEDITING);
  const bool mouseEvent = (event->type == SDL_MOUSEMOTION) ||
                          (event->type == SDL_MOUSEBUTTONDOWN) ||
                          (event->type == SDL_MOUSEBUTTONUP) ||
                          (event->type == SDL_MOUSEWHEEL);
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

/// Processes a queued script scene operation, if one exists.
bool process_pending_scene_op(World &world) noexcept {
  if (!scripting::has_pending_scene_op()) {
    return true;
  }

  bool processed = false;
  if (scripting::pending_scene_op_is_load()) {
    const char *scenePath = scripting::get_pending_scene_path();
    if ((scenePath != nullptr) && runtime::load_scene(world, scenePath)) {
      processed = true;
    } else {
      core::log_message(core::LogLevel::Error, "engine",
                        "failed to process pending scene load");
    }
  } else if (scripting::pending_scene_op_is_new()) {
    runtime::reset_world(world);
    processed = true;
  }

  if (processed) {
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
constexpr std::size_t kMaxChunkJobs = 1024U;
constexpr std::size_t kMaxPhaseJobs = kMaxUpdateStepsPerFrame * 2U + 4U;
constexpr std::uint32_t kSliceDiagnosticsPeriodFrames = 60U;

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

bool resolve_mesh_asset_path(char *outPath, std::size_t outCapacity) noexcept {
  if ((outPath == nullptr) || (outCapacity == 0U)) {
    return false;
  }

  const char *virtualPath = active_config().bootstrapMeshPath;
  if ((virtualPath == nullptr) || (virtualPath[0] == '\0')) {
    return false;
  }
  return core::vfs_resolve_os_path(virtualPath, outPath, outCapacity);
}


renderer::AssetId register_builtin_mesh(renderer::GpuMeshRegistry *registry,
                                        renderer::AssetDatabase *database,
                                        const renderer::GpuMesh &mesh,
                                        const char *builtinPath) noexcept {
  const std::uint32_t slot = renderer::register_gpu_mesh(registry, mesh);
  if (slot == 0U) {
    return renderer::kInvalidAssetId;
  }
  const renderer::MeshHandle handle{slot};
  const renderer::AssetId id = renderer::make_asset_id_from_path(builtinPath);
  if (id == renderer::kInvalidAssetId) {
    return renderer::kInvalidAssetId;
  }
  if (!renderer::register_mesh_asset(database, id, builtinPath, handle)) {
    return renderer::kInvalidAssetId;
  }
  return id;
}

// ---------------------------------------------------------------------------
// Job functions
// ---------------------------------------------------------------------------

void mark_graph_failed(std::atomic<bool> *frameGraphFailed) noexcept {
  if (frameGraphFailed != nullptr) {
    frameGraphFailed->store(true, std::memory_order_release);
  }
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
  while (SDL_PollEvent(&event) != 0) {
    const runtime::InputEventRoute route =
        runtime::process_editor_input_event(bridge, &event);
    if (route == runtime::InputEventRoute::QuitRequested) {
      core::request_platform_quit();
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

bool vec3_has_motion(const math::Vec3 &value) noexcept {
  constexpr float kEpsilon = 0.0001F;
  return (value.x > kEpsilon) || (value.x < -kEpsilon) ||
         (value.y > kEpsilon) || (value.y < -kEpsilon) ||
         (value.z > kEpsilon) || (value.z < -kEpsilon);
}

std::size_t count_moving_rigid_bodies(const runtime::World &world) noexcept {
  std::size_t count = 0U;
  world.for_each<runtime::RigidBody>(
      [&count](runtime::Entity, const runtime::RigidBody &rigidBody) noexcept {
        if (vec3_has_motion(rigidBody.velocity) ||
            vec3_has_motion(rigidBody.acceleration)) {
          ++count;
        }
      });
  return count;
}

std::size_t count_mesh_components(const runtime::World &world) noexcept {
  std::size_t count = 0U;
  world.for_each<runtime::MeshComponent>(
      [&count](runtime::Entity, const runtime::MeshComponent &) noexcept {
        ++count;
      });
  return count;
}

std::size_t
count_ready_mesh_components(const runtime::World &world,
                            const renderer::AssetDatabase *assets) noexcept {
  if (assets == nullptr) {
    return 0U;
  }

  std::size_t count = 0U;
  world.for_each<runtime::MeshComponent>(
      [&count, assets](runtime::Entity,
                       const runtime::MeshComponent &mesh) noexcept {
        if (renderer::mesh_asset_state(assets, mesh.meshAssetId) ==
            renderer::AssetState::Ready) {
          ++count;
        }
      });
  return count;
}

struct MeshAssetStateCounts final {
  std::size_t ready = 0U;
  std::size_t loading = 0U;
  std::size_t failed = 0U;
};

MeshAssetStateCounts
count_mesh_asset_states(const renderer::AssetDatabase *assets) noexcept {
  MeshAssetStateCounts counts{};
  if (assets == nullptr) {
    return counts;
  }

  for (std::size_t i = 0U; i < assets->meshAssets.size(); ++i) {
    if (!assets->occupied[i]) {
      continue;
    }

    switch (assets->meshAssets[i].state) {
    case renderer::AssetState::Ready:
      ++counts.ready;
      break;
    case renderer::AssetState::Loading:
      ++counts.loading;
      break;
    case renderer::AssetState::Failed:
      ++counts.failed;
      break;
    case renderer::AssetState::Unloaded:
      break;
    }
  }

  return counts;
}

// ---------------------------------------------------------------------------
// Bootstrap mesh loading
// ---------------------------------------------------------------------------

struct BootstrapMeshIds final {
  renderer::AssetId bootstrap = renderer::kInvalidAssetId;
  renderer::AssetId plane = renderer::kInvalidAssetId;
  renderer::AssetId cube = renderer::kInvalidAssetId;
  renderer::AssetId sphere = renderer::kInvalidAssetId;
  renderer::AssetId cylinder = renderer::kInvalidAssetId;
  renderer::AssetId capsule = renderer::kInvalidAssetId;
  renderer::AssetId pyramid = renderer::kInvalidAssetId;
};

/// CPU mesh payloads loaded by the streaming worker and consumed on the render thread.
struct StreamingMeshTransferSlot final {
  renderer::AssetId assetId = renderer::kInvalidAssetId;
  renderer::CpuMeshData meshData{};
  std::uint64_t sizeBytes = 0ULL;
  bool occupied = false;
};

/// Stores callback state for runtime mesh streaming.
struct RuntimeAssetStreamingState final {
  renderer::AssetDatabase *database = nullptr;
  renderer::GpuMeshRegistry *meshRegistry = nullptr;
  std::array<StreamingMeshTransferSlot, renderer::AssetStreamingQueue::kMaxRequests>
      meshTransfers{};
  std::mutex mutex{};
};

/// Stores a CPU-side mesh payload for main-thread upload.
bool store_streamed_mesh_data(RuntimeAssetStreamingState *state,
                              renderer::AssetId assetId,
                              renderer::CpuMeshData &&meshData,
                              std::uint64_t sizeBytes) noexcept {
  if ((state == nullptr) || (assetId == renderer::kInvalidAssetId)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(state->mutex);
  std::size_t freeSlot = state->meshTransfers.size();
  for (std::size_t i = 0U; i < state->meshTransfers.size(); ++i) {
    StreamingMeshTransferSlot &slot = state->meshTransfers[i];
    if (slot.occupied && (slot.assetId == assetId)) {
      slot.meshData = std::move(meshData);
      slot.sizeBytes = sizeBytes;
      return true;
    }
    if (!slot.occupied && (freeSlot == state->meshTransfers.size())) {
      freeSlot = i;
    }
  }

  if (freeSlot == state->meshTransfers.size()) {
    return false;
  }

  StreamingMeshTransferSlot &slot = state->meshTransfers[freeSlot];
  slot.assetId = assetId;
  slot.meshData = std::move(meshData);
  slot.sizeBytes = sizeBytes;
  slot.occupied = true;
  return true;
}

/// Takes a CPU-side mesh payload loaded by the streaming worker.
bool take_streamed_mesh_data(RuntimeAssetStreamingState *state,
                             renderer::AssetId assetId,
                             renderer::CpuMeshData *outMeshData,
                             std::uint64_t *outSizeBytes) noexcept {
  if ((state == nullptr) || (assetId == renderer::kInvalidAssetId) ||
      (outMeshData == nullptr)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(state->mutex);
  for (StreamingMeshTransferSlot &slot : state->meshTransfers) {
    if (!slot.occupied || (slot.assetId != assetId)) {
      continue;
    }

    *outMeshData = std::move(slot.meshData);
    if (outSizeBytes != nullptr) {
      *outSizeBytes = slot.sizeBytes;
    }
    slot = StreamingMeshTransferSlot{};
    return true;
  }
  return false;
}

/// Clears any CPU-side mesh payloads not consumed by upload.
void clear_streamed_mesh_data(RuntimeAssetStreamingState *state) noexcept {
  if (state == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(state->mutex);
  for (StreamingMeshTransferSlot &slot : state->meshTransfers) {
    slot = StreamingMeshTransferSlot{};
  }
}

/// Worker-thread CPU load callback for runtime asset streaming.
bool runtime_streaming_load_mesh(renderer::AssetId assetId, const char *path,
                                 std::uint64_t *outSizeBytes,
                                 void *userData) noexcept {
  auto *state = static_cast<RuntimeAssetStreamingState *>(userData);
  renderer::CpuMeshData meshData{};
  std::uint64_t sizeBytes = 0ULL;
  if (!renderer::load_mesh_data_from_file(path, &meshData, &sizeBytes)) {
    return false;
  }

  if (!store_streamed_mesh_data(state, assetId, std::move(meshData),
                                sizeBytes)) {
    return false;
  }

  if (outSizeBytes != nullptr) {
    *outSizeBytes = sizeBytes;
  }
  return true;
}

/// Main-thread GPU upload callback for runtime asset streaming.
bool runtime_streaming_upload_mesh(renderer::AssetId assetId,
                                   void *userData) noexcept {
  auto *state = static_cast<RuntimeAssetStreamingState *>(userData);
  if ((state == nullptr) || (state->database == nullptr) ||
      (state->meshRegistry == nullptr)) {
    return false;
  }

  renderer::CpuMeshData meshData{};
  std::uint64_t sizeBytes = 0ULL;
  if (!take_streamed_mesh_data(state, assetId, &meshData, &sizeBytes)) {
    static_cast<void>(renderer::set_mesh_asset_state(
        state->database, assetId, renderer::AssetState::Failed,
        renderer::kInvalidMeshHandle));
    return false;
  }

  if (!renderer::mesh_asset_requested_resident(state->database, assetId) ||
      (renderer::mesh_asset_state(state->database, assetId) !=
       renderer::AssetState::Loading)) {
    static_cast<void>(renderer::set_mesh_asset_state(
        state->database, assetId, renderer::AssetState::Unloaded,
        renderer::kInvalidMeshHandle));
    return true;
  }

  renderer::GpuMesh mesh{};
  if (!renderer::upload_mesh_data_to_gpu(meshData, &mesh)) {
    static_cast<void>(renderer::set_mesh_asset_state(
        state->database, assetId, renderer::AssetState::Failed,
        renderer::kInvalidMeshHandle));
    return false;
  }

  const std::uint32_t meshSlot =
      renderer::register_gpu_mesh(state->meshRegistry, mesh);
  if (meshSlot == 0U) {
    renderer::unload_mesh(&mesh);
    static_cast<void>(renderer::set_mesh_asset_state(
        state->database, assetId, renderer::AssetState::Failed,
        renderer::kInvalidMeshHandle));
    core::log_message(core::LogLevel::Error, "assets",
                      "mesh registry is full; streamed asset upload failed");
    return false;
  }

  if (!renderer::set_mesh_asset_state(state->database, assetId,
                                      renderer::AssetState::Ready,
                                      renderer::MeshHandle{meshSlot})) {
    renderer::unload_mesh(&state->meshRegistry->meshes[meshSlot]);
    state->meshRegistry->occupied[meshSlot] = false;
    return false;
  }

  static_cast<void>(sizeBytes);
  return true;
}

/// Mirrors terminal streaming failures into the asset database on the main thread.
void sync_streaming_failures(runtime::EngineAssetDatabaseService *service) noexcept {
  if ((service == nullptr) || (service->database == nullptr) ||
      (service->streamingQueue == nullptr)) {
    return;
  }

  for (const auto &handle : service->scriptLoadHandles) {
    if (!handle.occupied || !handle.streamingHandle.valid() ||
        (handle.assetId == renderer::kInvalidAssetId)) {
      continue;
    }

    if (renderer::get_load_state(service->streamingQueue,
                                 handle.streamingHandle) ==
        renderer::LoadingState::Failed) {
      static_cast<void>(renderer::set_mesh_asset_state(
          service->database, handle.assetId, renderer::AssetState::Failed,
          renderer::kInvalidMeshHandle));
    }
  }
}

/// Loads the requested resource for bootstrap meshes.
bool load_bootstrap_meshes(renderer::AssetManager *assetManager,
                           renderer::AssetDatabase *assetDatabase,
                           renderer::GpuMeshRegistry *meshRegistry,
                           BootstrapMeshIds *out) noexcept {
  char meshPath[512]{};
  if (!resolve_mesh_asset_path(meshPath, sizeof(meshPath))) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to resolve mesh asset path");
    return false;
  }

  out->bootstrap = renderer::make_asset_id_from_file(meshPath);
  bool ok = (out->bootstrap != renderer::kInvalidAssetId) &&
            renderer::queue_mesh_load(assetManager, assetDatabase,
                                      out->bootstrap, meshPath);
  if (ok) {
    if (!core::make_render_context_current()) {
      core::log_message(
          core::LogLevel::Error, "engine",
          "failed to acquire OpenGL context for bootstrap mesh upload");
      return false;
    }
    ok = renderer::update_asset_manager(assetManager, assetDatabase,
                                        meshRegistry, 8U);
    core::release_render_context();
    ok = ok && (renderer::mesh_asset_state(assetDatabase, out->bootstrap) ==
                renderer::AssetState::Ready);
  }
  if (!ok) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to load bootstrap mesh asset");
    return false;
  }

  if (!core::make_render_context_current()) {
    core::log_message(
        core::LogLevel::Warning, "engine",
        "failed to acquire OpenGL context for procedural mesh upload");
  } else {
    renderer::GpuMesh m{};
    if (renderer::build_plane_mesh(&m)) {
      out->plane = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                         "builtin://plane");
    }
    m = renderer::GpuMesh{};
    if (renderer::build_cube_mesh(&m)) {
      out->cube = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                        "builtin://cube");
    }
    m = renderer::GpuMesh{};
    if (renderer::build_sphere_mesh(&m)) {
      out->sphere = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                          "builtin://sphere");
    }
    m = renderer::GpuMesh{};
    if (renderer::build_cylinder_mesh(&m)) {
      out->cylinder = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                            "builtin://cylinder");
    }
    m = renderer::GpuMesh{};
    if (renderer::build_capsule_mesh(&m)) {
      out->capsule = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                           "builtin://capsule");
    }
    m = renderer::GpuMesh{};
    if (renderer::build_pyramid_mesh(&m)) {
      out->pyramid = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                           "builtin://pyramid");
    }
    core::release_render_context();
  }

  return true;
}

// ---------------------------------------------------------------------------
// Bootstrap scene
// ---------------------------------------------------------------------------

void create_bootstrap_scene(runtime::World *world,
                            const BootstrapMeshIds &meshIds) noexcept {
  const renderer::AssetId defaultMesh =
      (meshIds.cube != renderer::kInvalidAssetId) ? meshIds.cube
                                                  : meshIds.bootstrap;

  const runtime::Entity entity = world->create_entity();
  const runtime::Entity stackedEntity = world->create_entity();
  const runtime::Entity groundEntity = world->create_entity();
  const runtime::Entity foliageEntity = world->create_entity();
  const runtime::Entity lightEntity = world->create_entity();
  const runtime::Entity sceneControllerEntity = world->create_entity();
  if ((entity == runtime::kInvalidEntity) ||
      (stackedEntity == runtime::kInvalidEntity) ||
      (groundEntity == runtime::kInvalidEntity) ||
      (foliageEntity == runtime::kInvalidEntity) ||
      (lightEntity == runtime::kInvalidEntity) ||
      (sceneControllerEntity == runtime::kInvalidEntity)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to create bootstrap entities");
    return;
  }

  auto add_name = [&](runtime::Entity e, const char *label) {
    runtime::NameComponent n{};
    std::snprintf(n.name, sizeof(n.name), "%s", label);
    static_cast<void>(world->add_name_component(e, n));
  };
  add_name(entity, "Red Cube");
  add_name(stackedEntity, "Blue Cube");
  add_name(groundEntity, "Ground");
  add_name(foliageEntity, "Foliage Patch");
  add_name(lightEntity, "Sun Light");
  add_name(sceneControllerEntity, "Scene Controller");

  // Directional light.
  {
    runtime::Transform lt{};
    lt.position = math::Vec3(0.0F, 10.0F, 0.0F);
    static_cast<void>(world->add_transform(lightEntity, lt));
    runtime::LightComponent sunLight{};
    sunLight.type = runtime::LightType::Directional;
    sunLight.color = math::Vec3(1.0F, 0.95F, 0.9F);
    sunLight.direction = math::Vec3(0.4F, -1.0F, 0.6F);
    sunLight.intensity = 1.2F;
    static_cast<void>(world->add_light_component(lightEntity, sunLight));
  }

  // Red cube.
  {
    runtime::Transform t{};
    t.position = math::Vec3(-3.0F, 0.5F, -3.0F);
    static_cast<void>(world->add_transform(entity, t));
    runtime::RigidBody rb{};
    rb.velocity = math::Vec3(0.0F, 0.0F, 0.0F);
    rb.inverseMass = 0.0F;
    static_cast<void>(world->add_rigid_body(entity, rb));
    runtime::Collider c{};
    c.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    static_cast<void>(world->add_collider(entity, c));
    runtime::MeshComponent mc{};
    mc.meshAssetId = defaultMesh;
    mc.albedo = math::Vec3(0.9F, 0.2F, 0.2F);
    static_cast<void>(world->add_mesh_component(entity, mc));
  }

  // Blue cube.
  {
    runtime::Transform t{};
    t.position = math::Vec3(3.0F, 0.5F, -3.0F);
    static_cast<void>(world->add_transform(stackedEntity, t));
    runtime::RigidBody rb{};
    rb.velocity = math::Vec3(0.0F, 0.0F, 0.0F);
    rb.inverseMass = 0.0F;
    static_cast<void>(world->add_rigid_body(stackedEntity, rb));
    runtime::Collider c{};
    c.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    static_cast<void>(world->add_collider(stackedEntity, c));
    runtime::MeshComponent mc{};
    mc.meshAssetId = defaultMesh;
    mc.albedo = math::Vec3(0.2F, 0.4F, 0.9F);
    static_cast<void>(world->add_mesh_component(stackedEntity, mc));
  }

  // Ground plane.
  {
    runtime::Transform t{};
    t.position = math::Vec3(0.0F, -0.5F, 0.0F);
    static_cast<void>(world->add_transform(groundEntity, t));
    runtime::Collider gc{};
    gc.halfExtents = math::Vec3(5.0F, 0.5F, 5.0F);
    gc.staticFriction = 0.9F;
    gc.dynamicFriction = 0.7F;
    gc.restitution = 0.1F;
    static_cast<void>(world->add_collider(groundEntity, gc));
    runtime::MeshComponent mc{};
    mc.meshAssetId = (meshIds.plane != renderer::kInvalidAssetId)
                         ? meshIds.plane
                         : meshIds.bootstrap;
    mc.albedo = math::Vec3(0.45F, 0.42F, 0.38F);
    static_cast<void>(world->add_mesh_component(groundEntity, mc));
  }

  // Foliage patch demo.
  {
    runtime::Transform t{};
    t.position = math::Vec3(0.0F, 0.0F, 1.3F);
    static_cast<void>(world->add_transform(foliageEntity, t));

    runtime::FoliagePatchComponent foliage{};
    foliage.meshAssetIds[0] =
        (meshIds.pyramid != renderer::kInvalidAssetId) ? meshIds.pyramid
                                                       : defaultMesh;
    foliage.meshAssetIds[1] =
        (meshIds.cube != renderer::kInvalidAssetId) ? meshIds.cube
                                                    : foliage.meshAssetIds[0];
    foliage.meshAssetIds[2] = foliage.meshAssetIds[1];
    foliage.instanceCount = 35U;
    foliage.density = 2.5F;
    foliage.albedo = math::Vec3(0.18F, 0.62F, 0.22F);
    foliage.roughness = 0.92F;
    foliage.windStrength = 0.18F;
    foliage.windFrequency = 1.9F;

    std::uint32_t cursor = 0U;
    for (std::uint32_t z = 0U; z < 5U; ++z) {
      for (std::uint32_t x = 0U; x < 7U; ++x) {
        runtime::FoliageInstance &instance = foliage.instances[cursor];
        const float jitterX =
            (static_cast<float>((x * 17U + z * 11U) % 5U) - 2.0F) * 0.05F;
        const float jitterZ =
            (static_cast<float>((x * 7U + z * 19U) % 5U) - 2.0F) * 0.05F;
        instance.scale = 0.34F + (static_cast<float>((x + z) % 4U) * 0.05F);
        instance.offset =
            math::Vec3((static_cast<float>(x) - 3.0F) * 0.62F + jitterX,
                       instance.scale * 0.5F,
                       (static_cast<float>(z) - 2.0F) * 0.62F + jitterZ);
        instance.phase = static_cast<float>(cursor) * 0.37F;
        instance.lodIndex = ((x + z) % 5U == 0U) ? 1U : 0U;
        ++cursor;
      }
    }

    static_cast<void>(
        world->add_foliage_patch_component(foliageEntity, foliage));
  }

  // Scene controller script.
  {
    runtime::ScriptComponent sc{};
    const char *mainScriptPath = active_config().mainScriptPath;
    std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s",
                  (mainScriptPath != nullptr) ? mainScriptPath : "");
    static_cast<void>(world->add_script_component(sceneControllerEntity, sc));
  }
}

// ---------------------------------------------------------------------------
// Scene light collection
// ---------------------------------------------------------------------------

renderer::SceneLightData
collect_scene_lights(const runtime::World &world) noexcept {
  renderer::SceneLightData sceneLights{};

  const std::size_t lightCount = world.light_count();
  for (std::size_t li = 0U; li < lightCount; ++li) {
    const runtime::LightComponent *lc = world.light_at(li);
    if (lc == nullptr) {
      continue;
    }
    if (lc->type == runtime::LightType::Directional) {
      if (sceneLights.directionalLightCount < renderer::kMaxDirectionalLights) {
        auto &dl =
            sceneLights.directionalLights[sceneLights.directionalLightCount];
        dl.direction = lc->direction;
        dl.color = lc->color;
        dl.intensity = lc->intensity;
        ++sceneLights.directionalLightCount;
      }
    } else if (lc->type == runtime::LightType::Point) {
      if (sceneLights.pointLightCount < renderer::kMaxPointLights) {
        const runtime::Entity ple = world.light_entity_at(li);
        const runtime::WorldTransform *wt =
            world.get_world_transform_read_ptr(ple);
        auto &pl = sceneLights.pointLights[sceneLights.pointLightCount];
        pl.position =
            (wt != nullptr) ? wt->position : math::Vec3(0.0F, 0.0F, 0.0F);
        pl.color = lc->color;
        pl.intensity = lc->intensity;
        ++sceneLights.pointLightCount;
      }
    }
  }

  const std::size_t plcCount = world.point_light_count();
  for (std::size_t pi = 0U; pi < plcCount; ++pi) {
    if (sceneLights.pointLightCount >= renderer::kMaxPointLights) {
      break;
    }
    const runtime::PointLightComponent *plc = world.point_light_at(pi);
    if (plc == nullptr) {
      continue;
    }
    const runtime::Entity plEntity = world.point_light_entity_at(pi);
    const runtime::WorldTransform *wt =
        world.get_world_transform_read_ptr(plEntity);
    auto &pl = sceneLights.pointLights[sceneLights.pointLightCount];
    pl.position = (wt != nullptr) ? wt->position : math::Vec3(0.0F, 0.0F, 0.0F);
    pl.color = plc->color;
    pl.intensity = plc->intensity;
    pl.radius = plc->radius;
    ++sceneLights.pointLightCount;
  }

  const std::size_t slcCount = world.spot_light_count();
  for (std::size_t si = 0U; si < slcCount; ++si) {
    if (sceneLights.spotLightCount >= renderer::kMaxSpotLights) {
      break;
    }
    const runtime::SpotLightComponent *slc = world.spot_light_at(si);
    if (slc == nullptr) {
      continue;
    }
    const runtime::Entity slEntity = world.spot_light_entity_at(si);
    const runtime::WorldTransform *wt =
        world.get_world_transform_read_ptr(slEntity);
    auto &sl = sceneLights.spotLights[sceneLights.spotLightCount];
    sl.position = (wt != nullptr) ? wt->position : math::Vec3(0.0F, 0.0F, 0.0F);
    sl.direction = slc->direction;
    sl.color = slc->color;
    sl.intensity = slc->intensity;
    sl.radius = slc->radius;
    sl.innerConeAngle = slc->innerConeAngle;
    sl.outerConeAngle = slc->outerConeAngle;
    ++sceneLights.spotLightCount;
  }

  return sceneLights;
}

// ---------------------------------------------------------------------------
// Scene capture collection
// ---------------------------------------------------------------------------

// Converts enabled SceneCaptureComponents (dense order) into renderer capture
// requests; the capture camera looks along the world rotation's -Z axis.
std::size_t
collect_scene_captures(const runtime::World &world,
                       renderer::SceneCaptureRequest *outRequests,
                       std::size_t capacity) noexcept {
  if (outRequests == nullptr) {
    return 0U;
  }

  std::size_t requestCount = 0U;
  const std::size_t captureCount = world.scene_capture_count();
  for (std::size_t ci = 0U;
       (ci < captureCount) && (requestCount < capacity); ++ci) {
    const runtime::SceneCaptureComponent *capture = world.scene_capture_at(ci);
    if ((capture == nullptr) || !capture->enabled) {
      continue;
    }

    const runtime::Entity captureEntity = world.scene_capture_entity_at(ci);
    const runtime::WorldTransform *wt =
        world.get_world_transform_read_ptr(captureEntity);
    const math::Vec3 position =
        (wt != nullptr) ? wt->position : math::Vec3(0.0F, 0.0F, 0.0F);
    const math::Quat rotation = (wt != nullptr) ? wt->rotation : math::Quat();

    renderer::SceneCaptureRequest &request = outRequests[requestCount];
    request = renderer::SceneCaptureRequest{};
    request.camera.position = position;
    request.camera.target = math::add(
        position,
        math::rotate_vector(math::Vec3(0.0F, 0.0F, -1.0F), rotation));
    request.camera.up =
        math::rotate_vector(math::Vec3(0.0F, 1.0F, 0.0F), rotation);
    request.camera.fovRadians = capture->fovRadians;
    request.camera.nearPlane = capture->nearPlane;
    request.camera.farPlane = capture->farPlane;
    request.width = capture->width;
    request.height = capture->height;
    ++requestCount;
  }

  return requestCount;
}

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
  std::unique_ptr<runtime::World> world;
  std::unique_ptr<renderer::CommandBufferBuilder> commandBuffer;
  std::unique_ptr<renderer::GpuMeshRegistry> meshRegistry;
  std::unique_ptr<renderer::AssetDatabase> assetDatabase;
  std::unique_ptr<renderer::AssetManager> assetManager;
  std::unique_ptr<renderer::AssetStreamingQueue> assetStreamingQueue;
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
  LoopPlayState previousPlayState = LoopPlayState::Playing;
  std::size_t previousAliveCount = 0U;
  std::size_t frameThreadCount = 0U;

  // --- Per-frame computed state ---
  LoopPlayState playState = LoopPlayState::Stopped;
  bool isPlaying = false;
  bool isPaused = false;
  bool runPhysics = false;
  bool runFrameGraph = false;
  std::size_t updateStepCount = 0U;
  double frameMs = 0.0;
  double utilizationPct = 0.0;
  core::JobSystemStats jobStats{};

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
  bool stage_frame_graph() noexcept;
  void stage_post_frame() noexcept;
  void stage_measure_frame() noexcept;
  void stage_render() noexcept;
  void stage_diagnostics() noexcept;
  void stage_frame_cleanup() noexcept;
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
  assetStreamingQueue.reset(new (std::nothrow) renderer::AssetStreamingQueue());
  assetStreamingState.reset(new (std::nothrow) RuntimeAssetStreamingState());

  if (!world || !commandBuffer || !meshRegistry || !assetDatabase ||
      !assetManager || !assetStreamingQueue || !assetStreamingState) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to allocate runtime frame state");
    return false;
  }
  renderer::clear_asset_database(assetDatabase.get());
  renderer::clear_asset_manager(assetManager.get());
  if (!renderer::initialize_asset_streaming(assetStreamingQueue.get())) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to initialize runtime asset streaming queue");
    return false;
  }

  assetDatabaseService = runtime::EngineAssetDatabaseService{};
  assetStreamingState->database = assetDatabase.get();
  assetStreamingState->meshRegistry = meshRegistry.get();
  physicsService.world = world.get();
  physicsService.worldView = static_cast<physics::PhysicsWorldView *>(world.get());
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

  runtime::bind_scripting_runtime(world.get(), serviceLocator);
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
  PROFILE_SCOPE("engine_frame");
  frameStart = Clock::now();

  stage_input();
  stage_play_transitions();
  stage_timing();
  stage_scripting();
  stage_assets();
  stage_hot_reload();
  stage_audio();

  if (runFrameGraph) {
    if (!stage_frame_graph()) {
      core::profiler_end_frame();
      return false;
    }
    stage_post_frame();
  }

  stage_measure_frame();
  stage_render();
  stage_diagnostics();
  stage_frame_cleanup();

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

  runtime::unbind_scripting_runtime(serviceLocator);
  serviceRegistry.unregister_services();

  renderer::shutdown_asset_streaming(assetStreamingQueue.get());
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
    scripting::flush_deferred_mutations();
    world->end_begin_play_phase();
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
}

// ---------------------------------------------------------------------------
// Stage: timing
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_timing() noexcept {
  updateStepCount = 0U;
  if (isPlaying) {
    const auto now = Clock::now();
    accumulator += std::chrono::duration<double>(now - previousTick).count();
    previousTick = now;

    const double maxAccumulator =
        static_cast<double>(kMaxUpdateStepsPerFrame) * kFixedDeltaSeconds;
    if (accumulator > maxAccumulator) {
      accumulator = maxAccumulator;
    }

    while ((accumulator >= kFixedDeltaSeconds) &&
           (updateStepCount < kMaxUpdateStepsPerFrame)) {
      accumulator -= kFixedDeltaSeconds;
      ++updateStepCount;
    }

    simulationTimeSeconds +=
        static_cast<double>(updateStepCount) * kFixedDeltaSeconds;
  } else {
    accumulator = 0.0;
    previousTick = frameStart;
  }
}

// ---------------------------------------------------------------------------
// Stage: scripting
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_scripting() noexcept {
  scripting::set_frame_index(frameIndex);

  if (isPlaying && (updateStepCount > 0U)) {
    scripting::tick_timers();
    scripting::tick_coroutines();
    scripting::set_frame_time(static_cast<float>(kFixedDeltaSeconds),
                              static_cast<float>(simulationTimeSeconds));
    scripting::dispatch_entity_scripts_update(
        static_cast<float>(kFixedDeltaSeconds));
  }

  scripting::flush_deferred_mutations();
}

// ---------------------------------------------------------------------------
// Stage: assets
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_assets() noexcept {
  bool updatedAssets = true;
  if (assetStreamingQueue != nullptr) {
    renderer::begin_streaming_frame(assetStreamingQueue.get());
  }

  if (!core::make_render_context_current()) {
    core::log_message(core::LogLevel::Warning, "assets",
                      "skipping asset transitions: OpenGL context unavailable");
  } else {
    if ((assetStreamingQueue != nullptr) && (assetStreamingState != nullptr)) {
      static_cast<void>(renderer::update_asset_streaming(
          assetStreamingQueue.get(), &runtime_streaming_load_mesh,
          &runtime_streaming_upload_mesh, assetStreamingState.get()));
    }
    sync_streaming_failures(&assetDatabaseService);

    updatedAssets = renderer::update_asset_manager(
        assetManager.get(), assetDatabase.get(), meshRegistry.get(), 16U);
    core::release_render_context();
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
// Stage: frame graph (job submission + execution)
// Returns false on fatal error; sets running = false internally.
// ---------------------------------------------------------------------------

bool EnginePipeline::Impl::stage_frame_graph() noexcept {
  if (!core::begin_frame_graph()) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to begin frame graph");
    running = false;
    return false;
  }

  std::size_t updateJobCursor = 0U;
  std::size_t physicsJobCursor = 0U;
  std::size_t phaseJobCursor = 0U;
  frameContext->frameGraphFailed.store(false, std::memory_order_release);

  const bool hasSimulationSteps = updateStepCount > 0U;
  if (hasSimulationSteps) {
    world->begin_update_phase();
  }

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

    if (step > 0U) {
      core::JobHandle beginStepHandle =
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

      if (core::is_valid_handle(previousUpdateCommit) &&
          !link_dependency(previousUpdateCommit, updateHandle)) {
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

  core::JobHandle renderPrepPhaseHandle =
      submit_world_phase_job(frameContext.get(), world.get(), &phaseJobCursor,
                             &begin_render_prep_phase_job);
  if (!core::is_valid_handle(renderPrepPhaseHandle)) {
    graphFailed = true;
  }

  if (!graphFailed && core::is_valid_handle(previousUpdateCommit) &&
      !link_dependency(previousUpdateCommit, renderPrepPhaseHandle)) {
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
            frameThreadCount, kChunkSize, vpMatrix, &mergeHandle)) {
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

  core::wait(endFrameHandle);
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
// Stage: post-frame (collision callbacks, end-play, spring arm, scene ops)
// ---------------------------------------------------------------------------

void EnginePipeline::Impl::stage_post_frame() noexcept {
  if (runPhysics) {
    runtime::dispatch_collision_callbacks(*world);
  }

  if (isPlaying) {
    world->begin_end_play_phase();
    scripting::dispatch_entity_scripts_end_play(world.get());
    world->end_end_play_phase();
  }

  scripting::flush_deferred_mutations();

  if (isPlaying) {
    runtime::update_spring_arm_cameras(*world,
                                       static_cast<float>(kFixedDeltaSeconds));
    math::Vec3 camPos, camTarget, camUp;
    float camFov = 0.0F;
    float camNear = 0.0F;
    float camFar = 0.0F;
    world->camera_manager().evaluate(static_cast<float>(kFixedDeltaSeconds),
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
  }

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
    running = false;
    return;
  }

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
        renderer::pending_load_count(assetStreamingQueue.get());

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
  frameStats.fps =
      (frameMs > 0.0) ? static_cast<float>(1000.0 / frameMs) : 0.0F;
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

void EnginePipeline::teardown() noexcept {
  if (m_impl) {
    m_impl->teardown();
  }
  m_impl.reset();
}

} // namespace engine
