// Declares service registry types and APIs for the Engine runtime world.

#pragma once

#include <array>
#include <cstdint>

#include "engine/audio/audio.h"
#include "engine/core/service_locator.h"
#include "engine/physics/physics_world_view.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/content/asset_streaming.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/render_device.h"

namespace engine::runtime {

class World;

/// Function-pointer physics service registered for scripts/tools.
struct EnginePhysicsService final {
  World *world = nullptr;
  physics::PhysicsWorldView *worldView = nullptr;
  physics::PhysicsContext *context = nullptr;
};

/// Function-pointer audio service registered for scripts/tools.
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

/// Asset database/manager service exposed through the locator.
struct EngineAssetDatabaseService final {
  /// Stores Lua-visible async/preload handles for runtime asset requests.
  struct ScriptAssetLoadHandle final {
    renderer::AssetId assetId = renderer::kInvalidAssetId;
    content::LoadHandle streamingHandle = content::kInvalidLoadHandle;
    std::uint16_t generation = 0U;
    bool occupied = false;
  };

  static constexpr std::size_t kMaxScriptAssetLoadHandles = 1024U;

  renderer::AssetDatabase *database = nullptr;
  renderer::AssetManager *manager = nullptr;
  content::AssetStreamingQueue *streamingQueue = nullptr;
  std::array<ScriptAssetLoadHandle, kMaxScriptAssetLoadHandles>
      scriptLoadHandles{};
};

/// Renderer service exposed through the locator.
struct EngineRendererService final {
  renderer::CommandBufferBuilder *commandBuffer = nullptr;
  renderer::GpuMeshRegistry *meshRegistry = nullptr;
  const renderer::RenderDevice *device = nullptr;
};

/// The exact pointers one register_engine_subsystem_services call installed,
/// one field per locator entry (derived entries such as the physics world
/// view included). Scoped removal compares against these so an entry a newer
/// provider replaced is left in place, per the locator's scoped-owner
/// contract; a field left null means that entry was never installed (a null
/// input removes the type at registration rather than installing it).
struct EngineRegisteredServices final {
  World *world = nullptr;
  physics::PhysicsWorldView *worldView = nullptr;
  physics::PhysicsContext *physicsContext = nullptr;
  EnginePhysicsService *physicsService = nullptr;
  EngineAudioService *audioService = nullptr;
  renderer::AssetDatabase *assetDatabase = nullptr;
  renderer::AssetManager *assetManager = nullptr;
  EngineAssetDatabaseService *assetDatabaseService = nullptr;
  renderer::CommandBufferBuilder *commandBuffer = nullptr;
  renderer::GpuMeshRegistry *meshRegistry = nullptr;
  renderer::RenderDevice *renderDevice = nullptr;
  EngineRendererService *rendererService = nullptr;
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

  /// Removes the services this scoped registry installed, leaving any entry
  /// a newer provider has since replaced (the locator's scoped-owner rule:
  /// remove an entry only while get_service still returns the pointer this
  /// registry registered).
  void unregister_services() noexcept;

  /// Returns the service locator owned by the runtime context.
  core::ServiceLocator &locator() noexcept { return *m_locator; }

private:
  core::ServiceLocator *m_locator = nullptr;
  bool m_registered = false;
  EngineRegisteredServices m_registeredServices{};
};

/// Registers engine subsystem services into an explicit service locator.
/// When outRegistered is non-null it receives the exact pointers installed,
/// which a scoped owner passes back to the conditional unregister below.
bool register_engine_subsystem_services(
    core::ServiceLocator &locator, World *world,
    EnginePhysicsService *physicsService, EngineAudioService *audioService,
    EngineAssetDatabaseService *assetDatabaseService,
    EngineRendererService *rendererService,
    EngineRegisteredServices *outRegistered = nullptr) noexcept;

/// Removes engine subsystem services from an explicit service locator
/// unconditionally, whoever registered the current entries. Only for a
/// caller that owns the whole locator lifetime; a scoped owner sharing the
/// locator uses the overload below (or EngineServiceRegistry), which honors
/// the locator's remove-only-your-own-pointer rule.
void unregister_engine_subsystem_services(core::ServiceLocator &locator) noexcept;

/// Removes only the entries that still hold the exact pointers a
/// register_engine_subsystem_services call installed; entries replaced by a
/// newer provider survive.
void unregister_engine_subsystem_services(
    core::ServiceLocator &locator,
    const EngineRegisteredServices &registered) noexcept;

} // namespace engine::runtime
