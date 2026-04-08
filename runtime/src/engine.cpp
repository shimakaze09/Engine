#include "engine/engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <new>

#if defined(__clang__) && !defined(__PRFCHWINTRIN_H)
#define __PRFCHWINTRIN_H
#endif

#if __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL2 headers not found"
#endif

#include "engine/audio/audio.h"
#include "engine/core/bootstrap.h"
#include "engine/core/input.h"
#include "engine/core/job_system.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/vfs.h"
#include "engine/math/transform.h"
#include "engine/physics/physics.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/shader_system.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/render_prep_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace engine {

namespace {

constexpr std::size_t kFrameAllocatorBytes = 1024U * 1024U;
constexpr double kFixedDeltaSeconds = 1.0 / 60.0;
constexpr std::size_t kChunkSize = 256U;
constexpr std::size_t kMaxUpdateStepsPerFrame = 8U;
constexpr std::size_t kMaxChunkJobs = 1024U;
constexpr std::size_t kMaxPhaseJobs = kMaxUpdateStepsPerFrame * 2U + 4U;
constexpr std::uint32_t kSliceDiagnosticsPeriodFrames = 60U;
constexpr const char *kMainScriptPath = "assets/main.lua";

// Runtime binaries are launched from multiple working directories (repo root,
// build root, and nested test folders), so keep a short fallback search list.
constexpr std::array<const char *, 4U> kMeshAssetPathCandidates = {
    "assets/triangle.mesh",
    "../assets/triangle.mesh",
    "../../assets/triangle.mesh",
    "../../../assets/triangle.mesh",
};

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

std::unique_ptr<FrameContext> g_frameContext;

bool file_exists(const char *path) noexcept {
  if (path == nullptr) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  std::fclose(file);
  return true;
}

const char *resolve_mesh_asset_path() noexcept {
  for (const char *candidate : kMeshAssetPathCandidates) {
    if (file_exists(candidate)) {
      return candidate;
    }
  }

  return nullptr;
}

void mark_graph_failed(std::atomic<bool> *frameGraphFailed) noexcept {
  if (frameGraphFailed != nullptr) {
    frameGraphFailed->store(true, std::memory_order_release);
  }
}

void process_input_events_with_editor() noexcept {
  core::begin_input_frame();

  const runtime::EditorBridge *bridge = runtime::editor_bridge();

  SDL_Event event{};
  while (SDL_PollEvent(&event) != 0) {
    core::input_process_event(&event);

    if ((bridge != nullptr) && (bridge->process_event != nullptr)) {
      bridge->process_event(&event);
    }

    if (event.type == SDL_QUIT) {
      core::request_platform_quit();
      continue;
    }

    const bool keyboardEvent =
        (event.type == SDL_KEYDOWN) || (event.type == SDL_KEYUP) ||
        (event.type == SDL_TEXTINPUT) || (event.type == SDL_TEXTEDITING);
    const bool mouseEvent = (event.type == SDL_MOUSEMOTION) ||
                            (event.type == SDL_MOUSEBUTTONDOWN) ||
                            (event.type == SDL_MOUSEBUTTONUP) ||
                            (event.type == SDL_MOUSEWHEEL);
    const bool captureKeyboard = (bridge != nullptr) &&
                                 (bridge->wants_capture_keyboard != nullptr) &&
                                 bridge->wants_capture_keyboard();
    const bool captureMouse = (bridge != nullptr) &&
                              (bridge->wants_capture_mouse != nullptr) &&
                              bridge->wants_capture_mouse();
    const bool editorCapturesInput =
        (keyboardEvent && captureKeyboard) || (mouseEvent && captureMouse);

    if (editorCapturesInput) {
      continue;
    }

    // Game-level input hooks are added in later phases.
  }

  core::end_input_frame();
}

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

  if (!runtime::resolve_collisions(*jobData->world)) {
    mark_graph_failed(jobData->frameGraphFailed);
  }
}

void commit_update_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->commit_update_phase();
  }
}

void begin_update_step_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->begin_update_step();
  }
}

void begin_render_prep_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->begin_render_prep_phase();
  }
}

void begin_render_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData != nullptr) && (jobData->world != nullptr)) {
    jobData->world->begin_render_phase();
  }
}

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

} // namespace

bool bootstrap() noexcept {
  if (!core::initialize_core(kFrameAllocatorBytes)) {
    return false;
  }

  // Mount the assets directory relative to CWD so that VFS paths like
  // "assets/shaders/pbr.vert" resolve correctly when running from build/.
  static_cast<void>(core::mount("assets", "assets"));

  const runtime::EditorBridge *bridge = runtime::editor_bridge();
  if ((bridge != nullptr) && (bridge->initialize != nullptr)) {
    if (!core::make_render_context_current()) {
      core::log_message(core::LogLevel::Error, "editor",
                        "failed to acquire OpenGL context for editor init");
      core::shutdown_core();
      return false;
    }

    if (!bridge->initialize(core::get_sdl_window(),
                            core::get_sdl_gl_context())) {
      core::log_message(core::LogLevel::Error, "editor",
                        "failed to initialize editor bridge");
      core::release_render_context();
      core::shutdown_core();
      return false;
    }

    core::release_render_context();
  }

  if (!scripting::initialize_scripting()) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "failed to initialize scripting");
    if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
      bridge->shutdown();
    }
    core::shutdown_core();
    return false;
  }

  if (!audio::initialize_audio()) {
    core::log_message(core::LogLevel::Error, "audio",
                      "failed to initialize audio");
    scripting::shutdown_scripting();
    if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
      bridge->shutdown();
    }
    core::shutdown_core();
    return false;
  }

  if (g_frameContext == nullptr) {
    g_frameContext.reset(new (std::nothrow) FrameContext());
    if (g_frameContext == nullptr) {
      core::log_message(core::LogLevel::Error, "engine",
                        "failed to allocate frame context");
      scripting::shutdown_scripting();
      if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
        bridge->shutdown();
      }
      core::shutdown_core();
      return false;
    }
  }

  core::log_message(core::LogLevel::Info, "engine", "bootstrap complete");
  return true;
}

void run(std::uint32_t maxFrames) noexcept {
  std::unique_ptr<runtime::World> world(new (std::nothrow) runtime::World());
  std::unique_ptr<renderer::CommandBufferBuilder> commandBuffer(
      new (std::nothrow) renderer::CommandBufferBuilder());
  std::unique_ptr<renderer::GpuMeshRegistry> meshRegistry(
      new (std::nothrow) renderer::GpuMeshRegistry());
  std::unique_ptr<renderer::AssetDatabase> assetDatabase(
      new (std::nothrow) renderer::AssetDatabase());
  std::unique_ptr<renderer::AssetManager> assetManager(
      new (std::nothrow) renderer::AssetManager());

  if ((world == nullptr) || (commandBuffer == nullptr) ||
      (meshRegistry == nullptr) || (assetDatabase == nullptr) ||
      (assetManager == nullptr)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to allocate runtime frame state");
    return;
  }
  renderer::clear_asset_database(assetDatabase.get());
  renderer::clear_asset_manager(assetManager.get());

  const runtime::EditorBridge *bridge = runtime::editor_bridge();

  runtime::bind_scripting_runtime(world.get());
  if ((bridge != nullptr) && (bridge->set_world != nullptr)) {
    bridge->set_world(world.get());
  }

  // Route physics collision pairs to the Lua on_collision callback.
  physics::set_collision_dispatch(&scripting::dispatch_physics_callbacks);

  const char *bootstrapMeshPath = resolve_mesh_asset_path();
  if (bootstrapMeshPath == nullptr) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to resolve mesh asset path");
    return;
  }

  const renderer::AssetId bootstrapMeshAssetId =
      renderer::make_asset_id_from_path(bootstrapMeshPath);
  scripting::set_default_mesh_asset_id(bootstrapMeshAssetId);
  if ((bootstrapMeshAssetId == renderer::kInvalidAssetId) ||
      !renderer::queue_mesh_load(assetManager.get(), assetDatabase.get(),
                                 bootstrapMeshAssetId, bootstrapMeshPath) ||
      !renderer::update_asset_manager(assetManager.get(), assetDatabase.get(),
                                      meshRegistry.get(), 8U) ||
      (renderer::mesh_asset_state(assetDatabase.get(), bootstrapMeshAssetId) !=
       renderer::AssetState::Ready)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to load bootstrap mesh asset");
    return;
  }

  FrameContext *frameContext = g_frameContext.get();
  if (frameContext == nullptr) {
    core::log_message(core::LogLevel::Error, "engine",
                      "frame context not initialized");
    return;
  }

  const std::size_t frameThreadCount = core::thread_frame_allocator_count();
  if ((frameThreadCount == 0U) ||
      (frameThreadCount >
       frameContext->renderPrepPipeline.localCommandBuffers.size())) {
    core::log_message(core::LogLevel::Error, "engine",
                      "invalid thread allocator count");
    return;
  }

  const runtime::Entity entity = world->create_entity();
  const runtime::Entity stackedEntity = world->create_entity();
  const runtime::Entity groundEntity = world->create_entity();
  const runtime::Entity lightEntity = world->create_entity();
  const runtime::Entity sceneControllerEntity = world->create_entity();
  if ((entity == runtime::kInvalidEntity) ||
      (stackedEntity == runtime::kInvalidEntity) ||
      (groundEntity == runtime::kInvalidEntity) ||
      (lightEntity == runtime::kInvalidEntity) ||
      (sceneControllerEntity == runtime::kInvalidEntity)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to create bootstrap entities");
    return;
  }

  // Assign names to bootstrap entities.
  {
    runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "Red Cube");
    static_cast<void>(world->add_name_component(entity, name));
  }
  {
    runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "Blue Cube");
    static_cast<void>(world->add_name_component(stackedEntity, name));
  }
  {
    runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "Ground");
    static_cast<void>(world->add_name_component(groundEntity, name));
  }
  {
    runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "Sun Light");
    static_cast<void>(world->add_name_component(lightEntity, name));
  }
  {
    runtime::NameComponent name{};
    std::snprintf(name.name, sizeof(name.name), "Scene Controller");
    static_cast<void>(world->add_name_component(sceneControllerEntity, name));
  }

  // Add directional light.
  {
    runtime::Transform lightTransform{};
    lightTransform.position = math::Vec3(0.0F, 10.0F, 0.0F);
    static_cast<void>(world->add_transform(lightEntity, lightTransform));

    runtime::LightComponent sunLight{};
    sunLight.type = runtime::LightType::Directional;
    sunLight.color = math::Vec3(1.0F, 0.95F, 0.9F);
    sunLight.direction = math::Vec3(0.4F, -1.0F, 0.6F);
    sunLight.intensity = 1.2F;
    static_cast<void>(world->add_light_component(lightEntity, sunLight));
  }

  runtime::Transform transform{};
  transform.position = math::Vec3(0.0F, 1.5F, 0.0F);
  if (!world->add_transform(entity, transform)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add bootstrap transform");
    return;
  }

  runtime::RigidBody rigidBody{};
  rigidBody.velocity = math::Vec3(0.0F, 0.0F, 0.0F);
  rigidBody.inverseMass = 1.0F;
  if (!world->add_rigid_body(entity, rigidBody)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add bootstrap rigid body");
    return;
  }

  runtime::Collider collider{};
  collider.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  if (!world->add_collider(entity, collider)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add bootstrap collider");
    return;
  }

  runtime::MeshComponent meshComponent{};
  meshComponent.meshAssetId = bootstrapMeshAssetId;
  meshComponent.albedo = math::Vec3(0.9F, 0.2F, 0.2F);
  if (!world->add_mesh_component(entity, meshComponent)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add bootstrap mesh component");
    return;
  }

  runtime::Transform stackedTransform{};
  stackedTransform.position = math::Vec3(0.0F, 3.0F, 0.0F);
  if (!world->add_transform(stackedEntity, stackedTransform)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add stacked transform");
    return;
  }

  if (!world->add_rigid_body(stackedEntity, rigidBody)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add stacked rigid body");
    return;
  }

  if (!world->add_collider(stackedEntity, collider)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add stacked collider");
    return;
  }

  runtime::MeshComponent stackedMesh{};
  stackedMesh.meshAssetId = bootstrapMeshAssetId;
  stackedMesh.albedo = math::Vec3(0.2F, 0.4F, 0.9F);
  if (!world->add_mesh_component(stackedEntity, stackedMesh)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add stacked mesh component");
    return;
  }

  runtime::Transform groundTransform{};
  groundTransform.position = math::Vec3(0.0F, -0.5F, 0.0F);
  if (!world->add_transform(groundEntity, groundTransform)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add ground transform");
    return;
  }

  runtime::Collider groundCollider{};
  groundCollider.halfExtents = math::Vec3(5.0F, 0.5F, 5.0F);
  if (!world->add_collider(groundEntity, groundCollider)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add ground collider");
    return;
  }

  runtime::MeshComponent groundMesh{};
  groundMesh.meshAssetId = bootstrapMeshAssetId;
  groundMesh.albedo = math::Vec3(0.5F, 0.5F, 0.5F);
  if (!world->add_mesh_component(groundEntity, groundMesh)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to add ground mesh component");
    return;
  }

  // Attach the scene-setup script to the Scene Controller entity.
  // dispatch_entity_scripts_start() will call M.on_start(self) when Play
  // begins.
  {
    runtime::ScriptComponent sceneScript{};
    std::snprintf(sceneScript.scriptPath, sizeof(sceneScript.scriptPath), "%s",
                  kMainScriptPath);
    static_cast<void>(
        world->add_script_component(sceneControllerEntity, sceneScript));
  }

  using Clock = std::chrono::steady_clock;
  auto previousTick = Clock::now();
  double accumulator = 0.0;
  double simulationTimeSeconds = 0.0;
  std::uint32_t frameIndex = 0;
  bool running = true;
  LoopPlayState previousPlayState = query_editor_play_state();
  std::size_t previousAliveCount = world->alive_entity_count();

  while (running) {
    const auto frameStart = Clock::now();

    process_input_events_with_editor();

    const LoopPlayState playState = query_editor_play_state();
    if ((playState == LoopPlayState::Playing) &&
        (previousPlayState == LoopPlayState::Stopped)) {
      scripting::dispatch_entity_scripts_start();
    }

    if ((playState == LoopPlayState::Stopped) &&
        (previousPlayState != LoopPlayState::Stopped)) {
      scripting::clear_entity_script_modules();
      scripting::shutdown_scripting();
      if (!scripting::initialize_scripting()) {
        core::log_message(core::LogLevel::Error, "scripting",
                          "failed to reinitialize scripting on stop");
      } else {
        runtime::bind_scripting_runtime(world.get());
        scripting::set_default_mesh_asset_id(bootstrapMeshAssetId);
      }

      accumulator = 0.0;
      previousTick = frameStart;
      simulationTimeSeconds = 0.0;
    }

    const bool isPlaying = (playState == LoopPlayState::Playing);
    const bool isPaused = (playState == LoopPlayState::Paused);
    const bool runPhysics = isPlaying;
    const bool runFrameGraph = !isPaused;

    std::size_t updateStepCount = 0U;
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

    scripting::set_frame_index(frameIndex);

    if (isPlaying && (updateStepCount > 0U)) {
      scripting::tick_timers();
      scripting::tick_coroutines();
      scripting::set_frame_time(static_cast<float>(kFixedDeltaSeconds),
                                static_cast<float>(simulationTimeSeconds));
      scripting::dispatch_entity_scripts_update(
          static_cast<float>(kFixedDeltaSeconds));
    }

    if (!renderer::update_asset_manager(assetManager.get(), assetDatabase.get(),
                                        meshRegistry.get(), 16U)) {
      core::log_message(core::LogLevel::Warning, "assets",
                        "one or more asset transitions failed this frame");
    }

    renderer::check_shader_reload();

    audio::update_audio();

    double frameMs = 0.0;
    double utilizationPct = 0.0;
    core::JobSystemStats jobStats{};

    if (runFrameGraph) {
      if (!core::begin_frame_graph()) {
        core::log_message(core::LogLevel::Error, "engine",
                          "failed to begin frame graph");
        running = false;
        continue;
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
            submit_world_phase_job(frameContext, world.get(), &phaseJobCursor,
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
              submit_world_phase_job(frameContext, world.get(), &phaseJobCursor,
                                     &begin_update_step_job);
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

        for (std::size_t start = 0U; start < transformCount;
             start += kChunkSize) {
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

            if (!link_dependency(
                    frameContext->updateJobHandles[updateHandleIndex],
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
          submit_world_phase_job(frameContext, world.get(), &phaseJobCursor,
                                 &begin_render_prep_phase_job);
      if (!core::is_valid_handle(renderPrepPhaseHandle)) {
        graphFailed = true;
      }

      if (!graphFailed && core::is_valid_handle(previousUpdateCommit) &&
          !link_dependency(previousUpdateCommit, renderPrepPhaseHandle)) {
        graphFailed = true;
      }

      core::JobHandle renderPhaseHandle = submit_world_phase_job(
          frameContext, world.get(), &phaseJobCursor, &begin_render_phase_job);
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
            (vpH > 0) ? (static_cast<float>(vpW) / static_cast<float>(vpH))
                      : 1.0F;
        const renderer::CameraState cam = renderer::get_active_camera();
        const math::Mat4 vpMatrix =
            math::mul(math::perspective(cam.fovRadians, vpAspect, cam.nearPlane,
                                        cam.farPlane),
                      math::look_at(cam.position, cam.target, cam.up));

        if (!runtime::enqueue_render_prep_pipeline(
                &frameContext->renderPrepPipeline, world.get(),
                commandBuffer.get(), assetDatabase.get(), meshRegistry.get(),
                renderPrepPhaseHandle, renderPhaseHandle,
                &frameContext->frameGraphFailed, frameThreadCount, kChunkSize,
                vpMatrix, &mergeHandle)) {
          graphFailed = true;
        }
      }

      core::JobHandle endFrameHandle = submit_world_phase_job(
          frameContext, world.get(), &phaseJobCursor, &end_frame_phase_job);
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
        continue;
      }

      core::wait(endFrameHandle);
      const bool frameJobsFailed =
          frameContext->frameGraphFailed.load(std::memory_order_acquire);
      if (!core::end_frame_graph()) {
        core::log_message(core::LogLevel::Error, "engine",
                          "failed to end frame graph");
        running = false;
        continue;
      }

      if (frameJobsFailed) {
        core::log_message(core::LogLevel::Error, "engine",
                          "frame graph job execution failed");
        running = false;
        continue;
      }

      // Dispatch Lua on_collision callbacks for all pairs recorded this frame.
      if (runPhysics) {
        physics::dispatch_collision_callbacks();
      }

      // Handle deferred scene operations requested from Lua.
      if (scripting::has_pending_scene_op()) {
        if (scripting::pending_scene_op_is_load()) {
          const char *scenePath = scripting::get_pending_scene_path();
          if (scenePath != nullptr) {
            runtime::load_scene(*world, scenePath);
          }
        }
        // new_scene: destroy all entities by resetting world state.
        if (scripting::pending_scene_op_is_new()) {
          runtime::reset_world(*world);
        }
        scripting::clear_pending_scene_op();
      }

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
        (spawnedCount > 0U) || (destroyedCount > 0U) ||
        (assetCounts.failed > 0U);
    if (shouldLogSliceDiagnostics) {
      const std::size_t movingRigidBodyCount =
          count_moving_rigid_bodies(*world);
      const std::size_t meshComponentCount = count_mesh_components(*world);
      const std::size_t readyMeshComponentCount =
          count_ready_mesh_components(*world, assetDatabase.get());
      const std::size_t pendingAssetRequests =
          renderer::pending_asset_request_count(assetManager.get());

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

    if (!core::make_render_context_current()) {
      core::log_message(core::LogLevel::Error, "editor",
                        "failed to acquire OpenGL context for editor");
      running = false;
    } else {
      if ((bridge != nullptr) && (bridge->new_frame != nullptr)) {
        bridge->new_frame();
      }

      // Collect lights from ECS for the PBR shader.
      renderer::SceneLightData sceneLights{};
      const std::size_t lightCount = world->light_count();
      for (std::size_t li = 0U; li < lightCount; ++li) {
        const runtime::LightComponent *lc = world->light_at(li);
        if (lc == nullptr) {
          continue;
        }

        if (lc->type == runtime::LightType::Directional) {
          if (sceneLights.directionalLightCount <
              renderer::kMaxDirectionalLights) {
            auto &dl =
                sceneLights
                    .directionalLights[sceneLights.directionalLightCount];
            dl.direction = lc->direction;
            dl.color = lc->color;
            dl.intensity = lc->intensity;
            ++sceneLights.directionalLightCount;
          }
        } else if (lc->type == runtime::LightType::Point) {
          if (sceneLights.pointLightCount < renderer::kMaxPointLights) {
            const runtime::Entity lightEntity = world->light_entity_at(li);
            const runtime::WorldTransform *wt =
                world->get_world_transform_read_ptr(lightEntity);

            auto &pl = sceneLights.pointLights[sceneLights.pointLightCount];
            pl.position =
                (wt != nullptr) ? wt->position : math::Vec3(0.0F, 0.0F, 0.0F);
            pl.color = lc->color;
            pl.intensity = lc->intensity;
            ++sceneLights.pointLightCount;
          }
        }
      }

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

    char jobMessage[192] = {};
    std::snprintf(
        jobMessage, sizeof(jobMessage),
        "jobs=%llu busyMs=%.3f utilization=%.2f%% queueContention=%llu",
        static_cast<unsigned long long>(jobStats.jobsExecuted),
        static_cast<double>(jobStats.busyNanoseconds) / 1000000.0,
        utilizationPct,
        static_cast<unsigned long long>(jobStats.queueContentionCount));
    core::log_message(core::LogLevel::Trace, "jobs", jobMessage);

    core::reset_frame_allocator();
    core::reset_thread_frame_allocators();

    previousPlayState = playState;
    previousAliveCount = aliveCount;
    ++frameIndex;
    if ((maxFrames != 0U) && (frameIndex >= maxFrames)) {
      running = false;
    }

    if (!core::is_platform_running()) {
      running = false;
    }
  }

  if ((bridge != nullptr) && (bridge->set_world != nullptr)) {
    bridge->set_world(nullptr);
  }

  renderer::shutdown_asset_manager(assetManager.get(), assetDatabase.get(),
                                   meshRegistry.get());
}

void shutdown() noexcept {
  core::log_message(core::LogLevel::Info, "engine", "shutdown complete");

  const runtime::EditorBridge *bridge = runtime::editor_bridge();

  if ((bridge != nullptr) && (bridge->shutdown != nullptr)) {
    bridge->shutdown();
  }
  renderer::shutdown_renderer();
  audio::shutdown_audio();
  scripting::shutdown_scripting();
  g_frameContext.reset();
  core::shutdown_core();
}

} // namespace engine
