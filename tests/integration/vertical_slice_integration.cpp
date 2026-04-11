#include "engine/core/job_system.h"
#include "engine/math/transform.h"
#include "engine/physics/physics.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/render_prep_pipeline.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

constexpr const char *kTempScriptPath = "vertical_slice_integration.lua";
constexpr const char *kMoverName = "SliceMover";
constexpr std::size_t kFrameCount = 60U;
constexpr float kStepSeconds = 1.0F / 60.0F;
constexpr float kStepX = 0.25F;

bool open_file_for_write(const char *path, FILE **outFile) noexcept {
  if ((path == nullptr) || (outFile == nullptr)) {
    return false;
  }

  *outFile = nullptr;
#ifdef _WIN32
  return fopen_s(outFile, path, "wb") == 0;
#else
  *outFile = std::fopen(path, "wb");
  return *outFile != nullptr;
#endif
}

bool nearly_equal(float lhs, float rhs) noexcept {
  return std::fabs(lhs - rhs) <= 0.0001F;
}

bool mat4_nearly_equal(const engine::math::Mat4 &lhs,
                       const engine::math::Mat4 &rhs) noexcept {
  for (std::size_t column = 0U; column < 4U; ++column) {
    if (!nearly_equal(lhs.columns[column].x, rhs.columns[column].x) ||
        !nearly_equal(lhs.columns[column].y, rhs.columns[column].y) ||
        !nearly_equal(lhs.columns[column].z, rhs.columns[column].z) ||
        !nearly_equal(lhs.columns[column].w, rhs.columns[column].w)) {
      return false;
    }
  }

  return true;
}

void remove_script_file() noexcept {
  static_cast<void>(std::remove(kTempScriptPath));
}

bool write_script_file(const char *contents) noexcept {
  if (contents == nullptr) {
    return false;
  }

  FILE *file = nullptr;
  if (!open_file_for_write(kTempScriptPath, &file) || (file == nullptr)) {
    return false;
  }

  const std::size_t length = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, length, file) == length);
  std::fclose(file);
  return ok;
}

bool find_entity_by_name(const engine::runtime::World &world, const char *name,
                         engine::runtime::Entity *outEntity) noexcept {
  if ((name == nullptr) || (outEntity == nullptr)) {
    return false;
  }

  *outEntity = engine::runtime::kInvalidEntity;
  world.for_each<engine::runtime::NameComponent>(
      [&name, outEntity](engine::runtime::Entity entity,
                         const engine::runtime::NameComponent &component) {
        if ((component.name[0] != '\0') &&
            (std::strcmp(component.name, name) == 0)) {
          *outEntity = entity;
        }
      });

  return *outEntity != engine::runtime::kInvalidEntity;
}

enum class WorldPhaseOp : std::uint8_t {
  BeginRenderPrep,
  BeginRender,
  EndFrame,
};

struct WorldPhaseJobData final {
  engine::runtime::World *world = nullptr;
  WorldPhaseOp op = WorldPhaseOp::BeginRenderPrep;
};

void world_phase_job(void *userData) noexcept {
  auto *jobData = static_cast<WorldPhaseJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }

  switch (jobData->op) {
  case WorldPhaseOp::BeginRenderPrep:
    jobData->world->begin_render_prep_phase();
    break;
  case WorldPhaseOp::BeginRender:
    jobData->world->begin_render_phase();
    break;
  case WorldPhaseOp::EndFrame:
    jobData->world->end_frame_phase();
    break;
  }
}

bool run_render_prep_pipeline(
    engine::runtime::World *world,
    engine::runtime::RenderPrepPipelineContext *pipelineContext,
    engine::renderer::CommandBufferBuilder *commandBuffer,
    const engine::renderer::AssetDatabase *assetDatabase,
    const engine::renderer::GpuMeshRegistry *meshRegistry) noexcept {
  if ((world == nullptr) || (pipelineContext == nullptr) ||
      (commandBuffer == nullptr) || (assetDatabase == nullptr) ||
      (meshRegistry == nullptr)) {
    return false;
  }

  if (!engine::core::begin_frame_graph()) {
    return false;
  }

  std::atomic<bool> frameGraphFailed = false;

  WorldPhaseJobData renderPrepPhaseData{};
  renderPrepPhaseData.world = world;
  renderPrepPhaseData.op = WorldPhaseOp::BeginRenderPrep;
  engine::core::Job renderPrepPhaseJob{};
  renderPrepPhaseJob.function = &world_phase_job;
  renderPrepPhaseJob.data = &renderPrepPhaseData;
  const engine::core::JobHandle renderPrepPhaseHandle =
      engine::core::submit(renderPrepPhaseJob);
  if (!engine::core::is_valid_handle(renderPrepPhaseHandle)) {
    static_cast<void>(engine::core::end_frame_graph());
    return false;
  }

  WorldPhaseJobData renderPhaseData{};
  renderPhaseData.world = world;
  renderPhaseData.op = WorldPhaseOp::BeginRender;
  engine::core::Job renderPhaseJob{};
  renderPhaseJob.function = &world_phase_job;
  renderPhaseJob.data = &renderPhaseData;
  const engine::core::JobHandle renderPhaseHandle =
      engine::core::submit(renderPhaseJob);
  if (!engine::core::is_valid_handle(renderPhaseHandle) ||
      !engine::core::add_dependency(renderPrepPhaseHandle, renderPhaseHandle)) {
    static_cast<void>(engine::core::end_frame_graph());
    world->end_frame_phase();
    return false;
  }

  engine::core::JobHandle mergeHandle{};
  const std::size_t frameThreadCount =
      static_cast<std::size_t>(engine::core::thread_count());
  // Compute a view-projection matrix from the default camera so that frustum
  // culling mirrors what a real frame would produce.
  const engine::renderer::CameraState cam =
      engine::renderer::get_active_camera();
  constexpr float kDefaultAspect = 16.0F / 9.0F;
  const engine::math::Mat4 vpMatrix = engine::math::mul(
      engine::math::perspective(cam.fovRadians, kDefaultAspect, cam.nearPlane,
                                cam.farPlane),
      engine::math::look_at(cam.position, cam.target, cam.up));

  if (!engine::runtime::enqueue_render_prep_pipeline(
          pipelineContext, world, commandBuffer, assetDatabase, meshRegistry,
          renderPrepPhaseHandle, renderPhaseHandle, &frameGraphFailed,
          frameThreadCount, 256U, vpMatrix, &mergeHandle)) {
    static_cast<void>(engine::core::end_frame_graph());
    world->end_frame_phase();
    return false;
  }

  WorldPhaseJobData endFrameData{};
  endFrameData.world = world;
  endFrameData.op = WorldPhaseOp::EndFrame;
  engine::core::Job endFrameJob{};
  endFrameJob.function = &world_phase_job;
  endFrameJob.data = &endFrameData;
  const engine::core::JobHandle endFrameHandle =
      engine::core::submit(endFrameJob);
  if (!engine::core::is_valid_handle(endFrameHandle) ||
      !engine::core::add_dependency(mergeHandle, endFrameHandle)) {
    static_cast<void>(engine::core::end_frame_graph());
    world->end_frame_phase();
    return false;
  }

  engine::core::wait(endFrameHandle);
  const bool jobsFailed = frameGraphFailed.load(std::memory_order_acquire);
  const bool frameGraphEnded = engine::core::end_frame_graph();

  return frameGraphEnded && !jobsFailed;
}

} // namespace

int main() {
  remove_script_file();

  auto fail = [](int code) noexcept { return code; };

  if (!engine::scripting::initialize_scripting()) {
    return fail(1);
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    engine::scripting::shutdown_scripting();
    return fail(2);
  }

  engine::runtime::bind_scripting_runtime(world.get());

  std::unique_ptr<engine::renderer::AssetDatabase> assetDatabase(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::GpuMeshRegistry> meshRegistry(
      new (std::nothrow) engine::renderer::GpuMeshRegistry());
  std::unique_ptr<engine::renderer::CommandBufferBuilder> commandBuffer(
      new (std::nothrow) engine::renderer::CommandBufferBuilder());
  if ((assetDatabase == nullptr) || (meshRegistry == nullptr) ||
      (commandBuffer == nullptr)) {
    engine::scripting::shutdown_scripting();
    return fail(3);
  }

  engine::renderer::clear_asset_database(assetDatabase.get());

  engine::renderer::GpuMesh dummyMesh{};
  dummyMesh.vertexCount = 3U;
  const std::uint32_t meshSlot =
      engine::renderer::register_gpu_mesh(meshRegistry.get(), dummyMesh);
  if (meshSlot == 0U) {
    engine::scripting::shutdown_scripting();
    return fail(4);
  }

  const char *meshPath = "integration://vertical-slice.mesh";
  const engine::renderer::AssetId meshAssetId =
      engine::renderer::make_asset_id_from_path(meshPath);
  if ((meshAssetId == engine::renderer::kInvalidAssetId) ||
      !engine::renderer::register_mesh_asset(
          assetDatabase.get(), meshAssetId, meshPath,
          engine::renderer::MeshHandle{meshSlot})) {
    engine::scripting::shutdown_scripting();
    return fail(5);
  }

  engine::scripting::set_default_mesh_asset_id(meshAssetId);

  const char *script =
      "local mover = nil\n"
      "local x = 0.0\n"
      "function on_start()\n"
      "    mover = engine.spawn_entity()\n"
      "    if mover == nil then\n"
      "        return\n"
      "    end\n"
      "    engine.set_name(mover, \"SliceMover\")\n"
      "    engine.set_position(mover, x, 1.0, 0.0)\n"
      "    engine.add_rigid_body(mover, 1.0)\n"
      "    local meshId = engine.get_default_mesh_asset_id()\n"
      "    if meshId ~= nil then\n"
      "        engine.set_mesh(mover, meshId)\n"
      "    end\n"
      "    engine.set_albedo(mover, 0.3, 0.7, 0.2)\n"
      "end\n"
      "function on_update()\n"
      "    if mover == nil or not engine.is_alive(mover) then\n"
      "        return\n"
      "    end\n"
      "    x = x + 0.25\n"
      "    engine.set_position(mover, x, 1.0, 0.0)\n"
      "end\n";

  if (!write_script_file(script) ||
      !engine::scripting::load_script(kTempScriptPath)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return fail(5);
  }

  // Place the camera far enough to see the entity's entire path (x 0..14.75).
  engine::renderer::CameraState testCamera{};
  testCamera.position = engine::math::Vec3(7.5F, 5.0F, 40.0F);
  testCamera.target = engine::math::Vec3(7.5F, 1.0F, 0.0F);
  testCamera.up = engine::math::Vec3(0.0F, 1.0F, 0.0F);
  testCamera.fovRadians = 1.0471975512F; // 60 degrees
  testCamera.nearPlane = 0.1F;
  testCamera.farPlane = 200.0F;
  engine::renderer::set_active_camera(testCamera);

  const std::size_t aliveBefore = world->alive_entity_count();
  engine::runtime::Entity mover = engine::runtime::kInvalidEntity;
  bool observedSpawnDelta = false;
  bool observedDrawSubmission = false;
  float expectedX = 0.0F;

  if (!engine::core::initialize_job_system(0U)) {
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return fail(6);
  }

  std::unique_ptr<engine::runtime::RenderPrepPipelineContext>
      renderPrepPipeline(new (std::nothrow)
                             engine::runtime::RenderPrepPipelineContext());
  if (renderPrepPipeline == nullptr) {
    engine::core::shutdown_job_system();
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return fail(7);
  }

  for (std::size_t frame = 1U; frame <= kFrameCount; ++frame) {
    if (frame == 1U) {
      if (!engine::scripting::call_script_function("on_start")) {
        engine::core::shutdown_job_system();
        remove_script_file();
        engine::scripting::shutdown_scripting();
        return fail(10);
      }
    } else {
      if (!engine::scripting::call_script_function("on_update")) {
        engine::core::shutdown_job_system();
        remove_script_file();
        engine::scripting::shutdown_scripting();
        return fail(11);
      }
      expectedX += kStepX;
    }

    world->begin_update_phase();
    if (!world->update_transforms(kStepSeconds) ||
        !engine::runtime::step_physics(*world, kStepSeconds)) {
      engine::core::shutdown_job_system();
      remove_script_file();
      engine::scripting::shutdown_scripting();
      return fail(12);
    }
    world->commit_update_phase();

    if (mover == engine::runtime::kInvalidEntity) {
      if (!find_entity_by_name(*world, kMoverName, &mover)) {
        engine::core::shutdown_job_system();
        remove_script_file();
        engine::scripting::shutdown_scripting();
        return fail(13);
      }
    }

    if (!run_render_prep_pipeline(world.get(), renderPrepPipeline.get(),
                                  commandBuffer.get(), assetDatabase.get(),
                                  meshRegistry.get())) {
      engine::core::shutdown_job_system();
      remove_script_file();
      engine::scripting::shutdown_scripting();
      return fail(24);
    }

    const std::size_t aliveNow = world->alive_entity_count();
    if (frame == 1U) {
      if (aliveNow <= aliveBefore) {
        engine::core::shutdown_job_system();
        remove_script_file();
        engine::scripting::shutdown_scripting();
        return fail(14);
      }
      observedSpawnDelta = true;
    }

    engine::runtime::Transform local{};
    if (!world->get_transform(mover, &local)) {
      engine::core::shutdown_job_system();
      remove_script_file();
      engine::scripting::shutdown_scripting();
      return fail(15);
    }

    const engine::runtime::WorldTransform *propagated =
        world->get_world_transform_read_ptr(mover);
    if (propagated == nullptr) {
      engine::core::shutdown_job_system();
      remove_script_file();
      engine::scripting::shutdown_scripting();
      return fail(16);
    }

    if (!nearly_equal(local.position.x, expectedX) ||
        !nearly_equal(local.position.y, 1.0F) ||
        !nearly_equal(propagated->position.x, expectedX) ||
        !nearly_equal(propagated->position.y, 1.0F)) {
      engine::core::shutdown_job_system();
      remove_script_file();
      engine::scripting::shutdown_scripting();
      return fail(17);
    }

    if (world->movement_authority(mover) !=
        engine::runtime::MovementAuthority::Script) {
      engine::core::shutdown_job_system();
      remove_script_file();
      engine::scripting::shutdown_scripting();
      return fail(18);
    }

    const engine::math::Mat4 propagatedFromLocal =
        engine::math::compose_trs(local.position, local.rotation, local.scale);
    if (!mat4_nearly_equal(propagatedFromLocal, propagated->matrix)) {
      engine::core::shutdown_job_system();
      remove_script_file();
      engine::scripting::shutdown_scripting();
      return fail(19);
    }

    if (commandBuffer->command_count() > 0U) {
      observedDrawSubmission = true;
    }

    const engine::renderer::CommandBufferView view = commandBuffer->view();
    bool foundMoverCommand = false;
    for (std::size_t i = 0U; i < view.count; ++i) {
      if (view.data[i].entity != mover.index) {
        continue;
      }

      foundMoverCommand = true;
      if ((view.data[i].mesh.id != meshSlot) ||
          !mat4_nearly_equal(view.data[i].modelMatrix, propagated->matrix)) {
        engine::core::shutdown_job_system();
        remove_script_file();
        engine::scripting::shutdown_scripting();
        return fail(21);
      }
      break;
    }

    if (!foundMoverCommand) {
      engine::core::shutdown_job_system();
      remove_script_file();
      engine::scripting::shutdown_scripting();
      return fail(22);
    }
  }

  engine::runtime::Transform finalTransform{};
  if (!world->get_transform(mover, &finalTransform) ||
      !nearly_equal(finalTransform.position.x,
                    static_cast<float>(kFrameCount - 1U) * kStepX) ||
      !observedSpawnDelta || !observedDrawSubmission) {
    engine::core::shutdown_job_system();
    remove_script_file();
    engine::scripting::shutdown_scripting();
    return fail(23);
  }

  engine::core::shutdown_job_system();
  engine::scripting::shutdown_scripting();
  remove_script_file();
  return 0;
}
