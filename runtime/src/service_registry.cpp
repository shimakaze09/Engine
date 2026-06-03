// Implements service registry behavior for the Engine runtime world.

#include "engine/runtime/service_registry.h"

#include "engine/core/service_locator.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

namespace {

/// Handles register pointer.
template <typename T> bool register_pointer(T *ptr) noexcept {
  return core::global_service_locator().register_service<T>(ptr);
}

} // namespace

/// Handles register engine subsystem services.
bool register_engine_subsystem_services(
    World *world, EnginePhysicsService *physicsService,
    EngineAudioService *audioService,
    EngineAssetDatabaseService *assetDatabaseService,
    EngineRendererService *rendererService) noexcept {
  bool ok = true;
  ok = register_pointer<World>(world) && ok;

  physics::PhysicsWorldView *worldView = nullptr;
  physics::PhysicsContext *physicsContext = nullptr;
  if (world != nullptr) {
    worldView = static_cast<physics::PhysicsWorldView *>(world);
    physicsContext = &world->physics_context();
  }
  ok = register_pointer<physics::PhysicsWorldView>(worldView) && ok;
  ok = register_pointer<physics::PhysicsContext>(physicsContext) && ok;
  ok = register_pointer<EnginePhysicsService>(physicsService) && ok;

  ok = register_pointer<EngineAudioService>(audioService) && ok;

  renderer::AssetDatabase *assetDatabase = nullptr;
  renderer::AssetManager *assetManager = nullptr;
  if (assetDatabaseService != nullptr) {
    assetDatabase = assetDatabaseService->database;
    assetManager = assetDatabaseService->manager;
  }
  ok = register_pointer<renderer::AssetDatabase>(assetDatabase) && ok;
  ok = register_pointer<renderer::AssetManager>(assetManager) && ok;
  ok = register_pointer<EngineAssetDatabaseService>(assetDatabaseService) && ok;

  renderer::CommandBufferBuilder *commandBuffer = nullptr;
  renderer::GpuMeshRegistry *meshRegistry = nullptr;
  renderer::RenderDevice *renderDevice = nullptr;
  if (rendererService != nullptr) {
    commandBuffer = rendererService->commandBuffer;
    meshRegistry = rendererService->meshRegistry;
    renderDevice = const_cast<renderer::RenderDevice *>(rendererService->device);
  }
  ok = register_pointer<renderer::CommandBufferBuilder>(commandBuffer) && ok;
  ok = register_pointer<renderer::GpuMeshRegistry>(meshRegistry) && ok;
  ok = register_pointer<renderer::RenderDevice>(renderDevice) && ok;
  ok = register_pointer<EngineRendererService>(rendererService) && ok;

  return ok;
}

/// Handles unregister engine subsystem services.
void unregister_engine_subsystem_services() noexcept {
  auto &loc = core::global_service_locator();
  static_cast<void>(loc.remove_service<EngineRendererService>());
  static_cast<void>(loc.remove_service<renderer::RenderDevice>());
  static_cast<void>(loc.remove_service<renderer::GpuMeshRegistry>());
  static_cast<void>(loc.remove_service<renderer::CommandBufferBuilder>());
  static_cast<void>(loc.remove_service<EngineAssetDatabaseService>());
  static_cast<void>(loc.remove_service<renderer::AssetManager>());
  static_cast<void>(loc.remove_service<renderer::AssetDatabase>());
  static_cast<void>(loc.remove_service<EngineAudioService>());
  static_cast<void>(loc.remove_service<EnginePhysicsService>());
  static_cast<void>(loc.remove_service<physics::PhysicsContext>());
  static_cast<void>(loc.remove_service<physics::PhysicsWorldView>());
  static_cast<void>(loc.remove_service<World>());
}

} // namespace engine::runtime
