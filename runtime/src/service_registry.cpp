// Implements service registry behavior for the Engine runtime world.

#include "engine/runtime/service_registry.h"

#include "engine/core/service_locator.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

namespace {

/// Handles register pointer.
template <typename T>
bool register_pointer(core::ServiceLocator &locator, T *ptr) noexcept {
  return locator.register_service<T>(ptr);
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
  bool ok = true;
  ok = register_pointer<World>(locator, world) && ok;

  physics::PhysicsWorldView *worldView = nullptr;
  physics::PhysicsContext *physicsContext = nullptr;
  if (world != nullptr) {
    worldView = static_cast<physics::PhysicsWorldView *>(world);
    physicsContext = &world->physics_context();
  }
  ok = register_pointer<physics::PhysicsWorldView>(locator, worldView) && ok;
  ok = register_pointer<physics::PhysicsContext>(locator, physicsContext) && ok;
  ok = register_pointer<EnginePhysicsService>(locator, physicsService) && ok;

  ok = register_pointer<EngineAudioService>(locator, audioService) && ok;

  renderer::AssetDatabase *assetDatabase = nullptr;
  renderer::AssetManager *assetManager = nullptr;
  if (assetDatabaseService != nullptr) {
    assetDatabase = assetDatabaseService->database;
    assetManager = assetDatabaseService->manager;
  }
  ok = register_pointer<renderer::AssetDatabase>(locator, assetDatabase) && ok;
  ok = register_pointer<renderer::AssetManager>(locator, assetManager) && ok;
  ok = register_pointer<EngineAssetDatabaseService>(locator,
                                                    assetDatabaseService) &&
       ok;

  renderer::CommandBufferBuilder *commandBuffer = nullptr;
  renderer::GpuMeshRegistry *meshRegistry = nullptr;
  renderer::RenderDevice *renderDevice = nullptr;
  if (rendererService != nullptr) {
    commandBuffer = rendererService->commandBuffer;
    meshRegistry = rendererService->meshRegistry;
    renderDevice = const_cast<renderer::RenderDevice *>(rendererService->device);
  }
  ok = register_pointer<renderer::CommandBufferBuilder>(locator,
                                                        commandBuffer) &&
       ok;
  ok = register_pointer<renderer::GpuMeshRegistry>(locator, meshRegistry) && ok;
  ok = register_pointer<renderer::RenderDevice>(locator, renderDevice) && ok;
  ok = register_pointer<EngineRendererService>(locator, rendererService) && ok;

  return ok;
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
