#pragma once

#include "engine/audio/audio.h"
#include "engine/physics/physics_world_view.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/render_device.h"

namespace engine::runtime {

class World;

struct EnginePhysicsService final {
  World *world = nullptr;
  physics::PhysicsWorldView *worldView = nullptr;
  physics::PhysicsContext *context = nullptr;
};

struct EngineAudioService final {
  void (*update)() noexcept = nullptr;
  audio::SoundHandle (*load_sound)(const char *path) noexcept = nullptr;
  void (*unload_sound)(audio::SoundHandle handle) noexcept = nullptr;
  bool (*play_sound)(audio::SoundHandle handle,
                     const audio::PlayParams &params) noexcept = nullptr;
  void (*stop_sound)(audio::SoundHandle handle) noexcept = nullptr;
  void (*stop_all)() noexcept = nullptr;
  void (*set_master_volume)(float volume) noexcept = nullptr;
};

struct EngineAssetDatabaseService final {
  renderer::AssetDatabase *database = nullptr;
  renderer::AssetManager *manager = nullptr;
};

struct EngineRendererService final {
  renderer::CommandBufferBuilder *commandBuffer = nullptr;
  renderer::GpuMeshRegistry *meshRegistry = nullptr;
  const renderer::RenderDevice *device = nullptr;
};

bool register_engine_subsystem_services(
    World *world, EnginePhysicsService *physicsService,
    EngineAudioService *audioService,
    EngineAssetDatabaseService *assetDatabaseService,
    EngineRendererService *rendererService) noexcept;

void unregister_engine_subsystem_services() noexcept;

} // namespace engine::runtime
