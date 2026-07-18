// Implements service registry behavior for the Engine runtime world.

#include "engine/runtime/service_registry.h"

#include "engine/core/service_locator.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

namespace {

template <typename T>
bool register_pointer(core::ServiceLocator &locator, T *ptr) noexcept {
  return locator.register_service<T>(ptr);
}

template <typename T> struct ServiceSnapshot final {
  bool existed = false;
  T *service = nullptr;
};

template <typename T>
ServiceSnapshot<T> snapshot_service(core::ServiceLocator &locator) noexcept {
  ServiceSnapshot<T> snapshot{};
  snapshot.existed = locator.has_service<T>();
  snapshot.service = locator.get_service<T>();
  return snapshot;
}

template <typename T>
void restore_service(core::ServiceLocator &locator,
                     const ServiceSnapshot<T> &snapshot) noexcept {
  if (snapshot.existed) {
    static_cast<void>(locator.register_service<T>(snapshot.service));
  } else {
    static_cast<void>(locator.remove_service<T>());
  }
}

} // namespace

/// Creates a scoped service registry bound to one locator.
EngineServiceRegistry::EngineServiceRegistry(core::ServiceLocator &locator) noexcept
    : m_locator(&locator) {}

/// Releases any service entries still owned by this registry.
EngineServiceRegistry::~EngineServiceRegistry() noexcept { unregister_services(); }

/// Registers this runtime's subsystem service pointers.
bool EngineServiceRegistry::register_services(
    World *world, EnginePhysicsService *physicsService,
    EngineAudioService *audioService,
    EngineAssetDatabaseService *assetDatabaseService,
    EngineRendererService *rendererService) noexcept {
  unregister_services();
  if ((m_locator == nullptr) ||
      !register_engine_subsystem_services(*m_locator, world, physicsService,
                                          audioService, assetDatabaseService,
                                          rendererService)) {
    return false;
  }
  m_registered = true;
  return true;
}

/// Removes any services registered through this scoped registry.
void EngineServiceRegistry::unregister_services() noexcept {
  if ((m_locator != nullptr) && m_registered) {
    unregister_engine_subsystem_services(*m_locator);
    m_registered = false;
  }
}

/// Registers engine subsystem services into an explicit service locator.
bool register_engine_subsystem_services(
    core::ServiceLocator &locator, World *world,
    EnginePhysicsService *physicsService,
    EngineAudioService *audioService,
    EngineAssetDatabaseService *assetDatabaseService,
    EngineRendererService *rendererService) noexcept {
  physics::PhysicsWorldView *worldView = nullptr;
  physics::PhysicsContext *physicsContext = nullptr;
  if (world != nullptr) {
    worldView = static_cast<physics::PhysicsWorldView *>(world);
    physicsContext = &world->physics_context();
  }

  renderer::AssetDatabase *assetDatabase = nullptr;
  renderer::AssetManager *assetManager = nullptr;
  if (assetDatabaseService != nullptr) {
    assetDatabase = assetDatabaseService->database;
    assetManager = assetDatabaseService->manager;
  }

  renderer::CommandBufferBuilder *commandBuffer = nullptr;
  renderer::GpuMeshRegistry *meshRegistry = nullptr;
  renderer::RenderDevice *renderDevice = nullptr;
  if (rendererService != nullptr) {
    commandBuffer = rendererService->commandBuffer;
    meshRegistry = rendererService->meshRegistry;
    renderDevice = const_cast<renderer::RenderDevice *>(rendererService->device);
  }

  const auto worldSnapshot = snapshot_service<World>(locator);
  const auto worldViewSnapshot =
      snapshot_service<physics::PhysicsWorldView>(locator);
  const auto physicsContextSnapshot =
      snapshot_service<physics::PhysicsContext>(locator);
  const auto physicsServiceSnapshot =
      snapshot_service<EnginePhysicsService>(locator);
  const auto audioServiceSnapshot = snapshot_service<EngineAudioService>(locator);
  const auto assetDatabaseSnapshot =
      snapshot_service<renderer::AssetDatabase>(locator);
  const auto assetManagerSnapshot =
      snapshot_service<renderer::AssetManager>(locator);
  const auto assetServiceSnapshot =
      snapshot_service<EngineAssetDatabaseService>(locator);
  const auto commandBufferSnapshot =
      snapshot_service<renderer::CommandBufferBuilder>(locator);
  const auto meshRegistrySnapshot =
      snapshot_service<renderer::GpuMeshRegistry>(locator);
  const auto renderDeviceSnapshot =
      snapshot_service<renderer::RenderDevice>(locator);
  const auto rendererServiceSnapshot =
      snapshot_service<EngineRendererService>(locator);

  auto rollback = [&]() noexcept {
    restore_service<EngineRendererService>(locator, rendererServiceSnapshot);
    restore_service<renderer::RenderDevice>(locator, renderDeviceSnapshot);
    restore_service<renderer::GpuMeshRegistry>(locator, meshRegistrySnapshot);
    restore_service<renderer::CommandBufferBuilder>(locator,
                                                    commandBufferSnapshot);
    restore_service<EngineAssetDatabaseService>(locator, assetServiceSnapshot);
    restore_service<renderer::AssetManager>(locator, assetManagerSnapshot);
    restore_service<renderer::AssetDatabase>(locator, assetDatabaseSnapshot);
    restore_service<EngineAudioService>(locator, audioServiceSnapshot);
    restore_service<EnginePhysicsService>(locator, physicsServiceSnapshot);
    restore_service<physics::PhysicsContext>(locator,
                                             physicsContextSnapshot);
    restore_service<physics::PhysicsWorldView>(locator, worldViewSnapshot);
    restore_service<World>(locator, worldSnapshot);
  };

  auto registerOrRollback = [&rollback](bool registered) noexcept -> bool {
    if (!registered) {
      rollback();
      return false;
    }
    return true;
  };

  if (!registerOrRollback(register_pointer<World>(locator, world))) {
    return false;
  }
  if (!registerOrRollback(
          register_pointer<physics::PhysicsWorldView>(locator, worldView))) {
    return false;
  }
  if (!registerOrRollback(
          register_pointer<physics::PhysicsContext>(locator, physicsContext))) {
    return false;
  }
  if (!registerOrRollback(
          register_pointer<EnginePhysicsService>(locator, physicsService))) {
    return false;
  }
  if (!registerOrRollback(
          register_pointer<EngineAudioService>(locator, audioService))) {
    return false;
  }
  if (!registerOrRollback(
          register_pointer<renderer::AssetDatabase>(locator, assetDatabase))) {
    return false;
  }
  if (!registerOrRollback(
          register_pointer<renderer::AssetManager>(locator, assetManager))) {
    return false;
  }
  if (!registerOrRollback(register_pointer<EngineAssetDatabaseService>(
          locator, assetDatabaseService))) {
    return false;
  }
  if (!registerOrRollback(register_pointer<renderer::CommandBufferBuilder>(
          locator, commandBuffer))) {
    return false;
  }
  if (!registerOrRollback(register_pointer<renderer::GpuMeshRegistry>(
          locator, meshRegistry))) {
    return false;
  }
  if (!registerOrRollback(
          register_pointer<renderer::RenderDevice>(locator, renderDevice))) {
    return false;
  }
  if (!registerOrRollback(
          register_pointer<EngineRendererService>(locator, rendererService))) {
    return false;
  }

  return true;
}

/// Removes engine subsystem services from an explicit service locator.
void unregister_engine_subsystem_services(core::ServiceLocator &locator) noexcept {
  static_cast<void>(locator.remove_service<EngineRendererService>());
  static_cast<void>(locator.remove_service<renderer::RenderDevice>());
  static_cast<void>(locator.remove_service<renderer::GpuMeshRegistry>());
  static_cast<void>(locator.remove_service<renderer::CommandBufferBuilder>());
  static_cast<void>(locator.remove_service<EngineAssetDatabaseService>());
  static_cast<void>(locator.remove_service<renderer::AssetManager>());
  static_cast<void>(locator.remove_service<renderer::AssetDatabase>());
  static_cast<void>(locator.remove_service<EngineAudioService>());
  static_cast<void>(locator.remove_service<EnginePhysicsService>());
  static_cast<void>(locator.remove_service<physics::PhysicsContext>());
  static_cast<void>(locator.remove_service<physics::PhysicsWorldView>());
  static_cast<void>(locator.remove_service<World>());
}

} // namespace engine::runtime
