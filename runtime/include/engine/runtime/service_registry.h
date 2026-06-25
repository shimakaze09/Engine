// Declares service registry types and APIs for the Engine runtime world.

#pragma once

#include <array>
#include <cstdint>

#include "engine/audio/audio.h"
#include "engine/core/service_locator.h"
#include "engine/physics/physics_world_view.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/renderer/asset_streaming.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/render_device.h"

namespace engine::runtime {

/// Owns the world behavior and state.
class World;

/// Stores engine physics service data used by the engine.
struct EnginePhysicsService final {
  World *world = nullptr;
  physics::PhysicsWorldView *worldView = nullptr;
  physics::PhysicsContext *context = nullptr;
};

/// Stores engine audio service data used by the engine.
struct EngineAudioService final {
  void (*update)() noexcept = nullptr;
  /// Handles sound handle.
  audio::SoundHandle (*load_sound)(const char *path) noexcept = nullptr;
  void (*unload_sound)(audio::SoundHandle handle) noexcept = nullptr;
  bool (*play_sound)(audio::SoundHandle handle,
                     const audio::PlayParams &params) noexcept = nullptr;
  void (*stop_sound)(audio::SoundHandle handle) noexcept = nullptr;
  void (*stop_all)() noexcept = nullptr;
  void (*set_master_volume)(float volume) noexcept = nullptr;
};

/// Stores engine asset database service data used by the engine.
struct EngineAssetDatabaseService final {
  /// Stores Lua-visible async/preload handles for runtime asset requests.
  struct ScriptAssetLoadHandle final {
    renderer::AssetId assetId = renderer::kInvalidAssetId;
    renderer::LoadHandle streamingHandle = renderer::kInvalidLoadHandle;
    std::uint16_t generation = 0U;
    bool occupied = false;
  };

  static constexpr std::size_t kMaxScriptAssetLoadHandles = 1024U;

  renderer::AssetDatabase *database = nullptr;
  renderer::AssetManager *manager = nullptr;
  renderer::AssetStreamingQueue *streamingQueue = nullptr;
  std::array<ScriptAssetLoadHandle, kMaxScriptAssetLoadHandles>
      scriptLoadHandles{};
};

/// Stores engine renderer service data used by the engine.
struct EngineRendererService final {
  renderer::CommandBufferBuilder *commandBuffer = nullptr;
  renderer::GpuMeshRegistry *meshRegistry = nullptr;
  const renderer::RenderDevice *device = nullptr;
};

/// Owns the service-locator registrations for one runtime service lifetime.
class EngineServiceRegistry final {
public:
  explicit EngineServiceRegistry(core::ServiceLocator &locator) noexcept;
  ~EngineServiceRegistry() noexcept;

  EngineServiceRegistry(const EngineServiceRegistry &) = delete;
  EngineServiceRegistry &operator=(const EngineServiceRegistry &) = delete;

  /// Registers this runtime's subsystem service pointers.
  bool register_services(World *world, EnginePhysicsService *physicsService,
                         EngineAudioService *audioService,
                         EngineAssetDatabaseService *assetDatabaseService,
                         EngineRendererService *rendererService) noexcept;

  /// Removes any services registered through this scoped registry.
  void unregister_services() noexcept;

  /// Returns the service locator owned by the runtime context.
  core::ServiceLocator &locator() noexcept { return *m_locator; }

private:
  core::ServiceLocator *m_locator = nullptr;
  bool m_registered = false;
};

/// Registers engine subsystem services into an explicit service locator.
bool register_engine_subsystem_services(
    core::ServiceLocator &locator, World *world,
    EnginePhysicsService *physicsService, EngineAudioService *audioService,
    EngineAssetDatabaseService *assetDatabaseService,
    EngineRendererService *rendererService) noexcept;

/// Removes engine subsystem services from an explicit service locator.
void unregister_engine_subsystem_services(core::ServiceLocator &locator) noexcept;

} // namespace engine::runtime
