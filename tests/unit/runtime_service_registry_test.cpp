// Verifies runtime service registry test behavior for the Engine test suite.

#include <memory>
#include <new>

#include "engine/core/service_locator.h"
#include "engine/physics/physics_context.h"
#include "engine/physics/physics_world_view.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/render_device.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"
#include "../test_harness.h"

namespace {

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

template <std::size_t Index> struct FillerService final {};

template <std::size_t Index>
bool register_filler_service(engine::core::ServiceLocator &loc) noexcept {
  static FillerService<Index> service{};
  return loc.register_service<FillerService<Index>>(&service);
}

template <std::size_t Count>
bool register_filler_services(engine::core::ServiceLocator &loc) noexcept {
  if constexpr (Count == 0U) {
    return true;
  } else {
    return register_filler_services<Count - 1U>(loc) &&
           register_filler_service<Count>(loc);
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::core::ServiceLocator loc{};

  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  std::unique_ptr<engine::renderer::AssetDatabase> assetDatabase(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::AssetManager> assetManager(
      new (std::nothrow) engine::renderer::AssetManager());
  std::unique_ptr<engine::renderer::CommandBufferBuilder> commandBuffer(
      new (std::nothrow) engine::renderer::CommandBufferBuilder());
  std::unique_ptr<engine::renderer::GpuMeshRegistry> meshRegistry(
      new (std::nothrow) engine::renderer::GpuMeshRegistry());
  std::unique_ptr<engine::renderer::RenderDevice> renderDevice(
      new (std::nothrow) engine::renderer::RenderDevice());
  check(world != nullptr, "world allocated");
  check(assetDatabase != nullptr, "asset database allocated");
  check(assetManager != nullptr, "asset manager allocated");
  check(commandBuffer != nullptr, "command buffer allocated");
  check(meshRegistry != nullptr, "mesh registry allocated");
  check(renderDevice != nullptr, "render device allocated");
  if (g_tests.failed() != 0) {
    return 1;
  }

  engine::runtime::EnginePhysicsService physicsService{};
  physicsService.world = world.get();
  physicsService.worldView =
      static_cast<engine::physics::PhysicsWorldView *>(world.get());
  physicsService.context = &world->physics_context();

  engine::runtime::EngineAudioService audioService{};
  engine::runtime::EngineAssetDatabaseService assetService{};
  assetService.database = assetDatabase.get();
  assetService.manager = assetManager.get();
  engine::runtime::EngineRendererService rendererService{};
  rendererService.commandBuffer = commandBuffer.get();
  rendererService.meshRegistry = meshRegistry.get();
  rendererService.device = renderDevice.get();

  check(engine::runtime::register_engine_subsystem_services(
            loc, world.get(), &physicsService, &audioService, &assetService,
            &rendererService),
        "register subsystem services");

  check(loc.get_service<engine::runtime::World>() == world.get(),
        "world registered");
  check(loc.get_service<engine::physics::PhysicsWorldView>() ==
            physicsService.worldView,
        "physics world view registered");
  check(loc.get_service<engine::physics::PhysicsContext>() ==
            physicsService.context,
        "physics context registered");
  check(loc.get_service<engine::runtime::EnginePhysicsService>() ==
            &physicsService,
        "physics service registered");
  check(loc.get_service<engine::runtime::EngineAudioService>() ==
            &audioService,
        "audio service registered");
  check(loc.get_service<engine::renderer::AssetDatabase>() ==
            assetDatabase.get(),
        "asset database registered");
  check(loc.get_service<engine::renderer::AssetManager>() ==
            assetManager.get(),
        "asset manager registered");
  check(loc.get_service<engine::runtime::EngineAssetDatabaseService>() ==
            &assetService,
        "asset service registered");
  check(loc.get_service<engine::renderer::CommandBufferBuilder>() ==
            commandBuffer.get(),
        "command buffer registered");
  check(loc.get_service<engine::renderer::GpuMeshRegistry>() ==
            meshRegistry.get(),
        "mesh registry registered");
  check(loc.get_service<engine::renderer::RenderDevice>() == renderDevice.get(),
        "render device registered");
  check(loc.get_service<engine::runtime::EngineRendererService>() ==
            &rendererService,
        "renderer service registered");

  engine::runtime::unregister_engine_subsystem_services(loc);
  check(loc.get_service<engine::runtime::EngineRendererService>() == nullptr,
        "renderer service removed");
  check(loc.get_service<engine::runtime::World>() == nullptr,
        "world removed");

  engine::core::ServiceLocator fullLoc{};
  check(register_filler_services<engine::core::ServiceLocator::kMaxServices -
                                 1U>(fullLoc),
        "filled locator to one free slot");
  check(fullLoc.count() == engine::core::ServiceLocator::kMaxServices - 1U,
        "filled locator count");
  check(!engine::runtime::register_engine_subsystem_services(
            fullLoc, world.get(), &physicsService, &audioService,
            &assetService, &rendererService),
        "register subsystem services fails when locator fills");
  check(fullLoc.count() == engine::core::ServiceLocator::kMaxServices - 1U,
        "rollback restores service count");
  check(fullLoc.get_service<engine::runtime::World>() == nullptr,
        "rollback removes partial world service");
  check(fullLoc.get_service<engine::physics::PhysicsWorldView>() == nullptr,
        "rollback removes partial physics world view service");
  check(fullLoc.get_service<FillerService<1U>>() != nullptr,
        "rollback preserves existing filler service");

  engine::core::ServiceLocator optionalLoc{};
  check(engine::runtime::register_engine_subsystem_services(
            optionalLoc, world.get(), nullptr, nullptr, nullptr, nullptr),
        "register null optional subsystem services");
  check(optionalLoc.count() == 3U,
        "world physics services registered for null optionals");
  check(optionalLoc.get_service<engine::runtime::World>() == world.get(),
        "world kept when optionals null");
  check(optionalLoc.get_service<engine::physics::PhysicsWorldView>() !=
            nullptr,
        "physics world view kept when optionals null");
  check(optionalLoc.get_service<engine::physics::PhysicsContext>() != nullptr,
        "physics context kept when optionals null");
  check(optionalLoc.get_service<engine::runtime::EngineAudioService>() ==
            nullptr,
        "null audio service not registered");
  engine::runtime::unregister_engine_subsystem_services(optionalLoc);
  check(optionalLoc.count() == 0U, "optional services unregister cleanly");

  return g_tests.finish("Runtime service registry tests");
}
