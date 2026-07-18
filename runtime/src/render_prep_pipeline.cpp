// Implements render prep pipeline behavior for the Engine runtime world.

#include "engine/runtime/render_prep_pipeline.h"

#include <atomic>
#include <cmath>
#include <cstddef>

#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/math/aabb.h"
#include "engine/math/mat4.h"
#include "engine/math/transform.h"
#include "engine/math/vec4.h"

namespace engine::runtime {

namespace {

struct FrustumPlane final {
  float a;
  float b;
  float c;
  float d;
};

bool aabb_outside_plane(const FrustumPlane &p, const math::Vec3 &center,
                        const math::Vec3 &half) noexcept {
  const float px = center.x + (p.a >= 0.0F ? half.x : -half.x);
  const float py = center.y + (p.b >= 0.0F ? half.y : -half.y);
  const float pz = center.z + (p.c >= 0.0F ? half.z : -half.z);
  return (p.a * px + p.b * py + p.c * pz + p.d) < 0.0F;
}

// Gribb-Hartmann: extract 6 frustum planes from a column-major VP matrix.
// Row j = (columns[0][j], columns[1][j], columns[2][j], columns[3][j])
// where Vec4 x/y/z/w map to indices 0/1/2/3.
void extract_frustum_planes(const math::Mat4 &vp,
                            FrustumPlane planes[6]) noexcept {
  const math::Vec4 c0 = vp.columns[0];
  const math::Vec4 c1 = vp.columns[1];
  const math::Vec4 c2 = vp.columns[2];
  const math::Vec4 c3 = vp.columns[3];
  planes[0] = {c0.w + c0.x, c1.w + c1.x, c2.w + c2.x, c3.w + c3.x}; // left
  planes[1] = {c0.w - c0.x, c1.w - c1.x, c2.w - c2.x, c3.w - c3.x}; // right
  planes[2] = {c0.w + c0.y, c1.w + c1.y, c2.w + c2.y, c3.w + c3.y}; // bottom
  planes[3] = {c0.w - c0.y, c1.w - c1.y, c2.w - c2.y, c3.w - c3.y}; // top
  planes[4] = {c0.w + c0.z, c1.w + c1.z, c2.w + c2.z, c3.w + c3.z}; // near
  planes[5] = {c0.w - c0.z, c1.w - c1.z, c2.w - c2.z, c3.w - c3.z}; // far
}

bool aabb_culled_by_frustum(const FrustumPlane planes[6],
                            const math::Vec3 &center,
                            const math::Vec3 &half) noexcept {
  for (int p = 0; p < 6; ++p) {
    if (aabb_outside_plane(planes[p], center, half)) {
      return true;
    }
  }
  return false;
}

/// Builds the requested runtime data for draw sort key.
std::uint64_t build_draw_sort_key(const renderer::Material &material,
                                  renderer::MeshHandle runtimeMesh,
                                  const math::Vec3 &center,
                                  const math::Mat4 &viewProjection) noexcept {
  // Layout (MSB->LSB):
  //   transparent:1 | shader:7 | texture:20 | mesh:20 | depth:16
  const bool transparent = (material.opacity < 1.0F);
  const std::uint64_t transparentBit = transparent ? (1ULL << 63U) : 0ULL;

  // Shader index: 0 = PBR (only shader for now).
  const std::uint64_t shaderBits = 0ULL;

  const std::uint64_t textureBits =
      (static_cast<std::uint64_t>(material.albedoTexture.id) & 0xFFFFFULL) <<
      36U;

  const std::uint64_t meshBits =
      (static_cast<std::uint64_t>(runtimeMesh.id) & 0xFFFFFULL) << 16U;

  const math::Vec4 clipPos =
      math::mul(viewProjection, math::Vec4(center.x, center.y, center.z, 1.0F));
  const float linearDepth = (clipPos.w > 0.0F) ? clipPos.w : 0.0F;
  const float normalizedDepth =
      (linearDepth < 200.0F) ? (linearDepth / 200.0F) : 1.0F;
  std::uint16_t depthQuantized =
      static_cast<std::uint16_t>(normalizedDepth * 65535.0F);

  if (transparent) {
    depthQuantized = static_cast<std::uint16_t>(65535U - depthQuantized);
  }

  return transparentBit | shaderBits | textureBits | meshBits |
         static_cast<std::uint64_t>(depthQuantized);
}

void mark_graph_failed(std::atomic<bool> *frameGraphFailed) noexcept;

/// Submits work to the owning buffer or system for render command.
bool submit_render_command(renderer::CommandBufferBuilder &localBuffer,
                           const renderer::DrawCommand &command,
                           std::atomic<bool> *frameGraphFailed) noexcept {
  if (localBuffer.submit(command)) {
    return true;
  }

  core::log_message(core::LogLevel::Error, "render_prep",
                    "command buffer full; entity dropped from frame");
  mark_graph_failed(frameGraphFailed);
  return false;
}

renderer::AssetId fallback_foliage_mesh_asset(
    const FoliagePatchComponent &foliage, std::uint32_t lodIndex) noexcept {
  if (lodIndex < static_cast<std::uint32_t>(FoliagePatchComponent::kMaxLods)) {
    const renderer::AssetId selected = foliage.meshAssetIds[lodIndex];
    if (selected != renderer::kInvalidAssetId) {
      return selected;
    }
  }

  for (std::size_t i = 0U; i < FoliagePatchComponent::kMaxLods; ++i) {
    if (foliage.meshAssetIds[i] != renderer::kInvalidAssetId) {
      return foliage.meshAssetIds[i];
    }
  }
  return renderer::kInvalidAssetId;
}

void mark_graph_failed(std::atomic<bool> *frameGraphFailed) noexcept {
  if (frameGraphFailed != nullptr) {
    frameGraphFailed->store(true, std::memory_order_release);
  }
}

void render_prep_chunk_job(void *userData) noexcept {
  auto *jobData = static_cast<RenderPrepChunkJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr) ||
      (jobData->localBuffers == nullptr) ||
      (jobData->assetDatabase == nullptr) ||
      (jobData->meshRegistry == nullptr)) {
    return;
  }

  const std::size_t threadIndex =
      static_cast<std::size_t>(core::current_thread_index());
  const std::size_t threadCount = jobData->threadCount;
  if (threadIndex >= threadCount) {
    mark_graph_failed(jobData->frameGraphFailed);
    return;
  }

  const Entity *entities = nullptr;
  const WorldTransform *transforms = nullptr;
  if (!jobData->world->read_world_transform_range(
          jobData->startIndex, jobData->count, &entities, &transforms)) {
    mark_graph_failed(jobData->frameGraphFailed);
    return;
  }

  renderer::CommandBufferBuilder &localBuffer =
      jobData->localBuffers[threadIndex];

  // Use the pre-computed view-projection matrix from the pipeline context.
  const math::Mat4 &vp = jobData->viewProjection;
  FrustumPlane frustumPlanes[6];
  extract_frustum_planes(vp, frustumPlanes);

  for (std::size_t i = 0U; i < jobData->count; ++i) {
    const MeshComponent *meshComponent =
        jobData->world->get_mesh_component_ptr(entities[i]);
    if (meshComponent != nullptr) {
      // Derive a world-space AABB from the collider for frustum culling.
      // Spheres use (radius, radius, radius) as conservative half-extents.
      const Collider *collider = jobData->world->get_collider_ptr(entities[i]);
      const math::Vec3 center = transforms[i].position;
      math::Vec3 half(0.5F, 0.5F, 0.5F);
      if (collider != nullptr) {
        if (collider->shape == ColliderShape::Sphere) {
          const float r = collider->halfExtents.x;
          half = math::Vec3(r, r, r);
        } else {
          half = collider->halfExtents;
        }
      }

      if (!aabb_culled_by_frustum(frustumPlanes, center, half)) {
        const renderer::MeshHandle runtimeMesh = renderer::resolve_mesh_asset(
            jobData->assetDatabase, meshComponent->meshAssetId);
        if (runtimeMesh != renderer::kInvalidMeshHandle) {
          const renderer::GpuMesh *mesh =
              renderer::lookup_gpu_mesh(jobData->meshRegistry, runtimeMesh);
          if (mesh != nullptr) {
            renderer::DrawCommand command{};
            command.entity = entities[i].index;
            command.mesh = runtimeMesh;
            command.material.albedo = meshComponent->albedo;
            command.material.roughness = meshComponent->roughness;
            command.material.metallic = meshComponent->metallic;
            command.material.opacity = meshComponent->opacity;
            command.modelMatrix = transforms[i].matrix;
            command.sortKey.value =
                build_draw_sort_key(command.material, runtimeMesh, center, vp);

            if (!submit_render_command(localBuffer, command,
                                       jobData->frameGraphFailed)) {
              return;
            }
          }
        }
      }
    }

    const FoliagePatchComponent *foliage =
        jobData->world->get_foliage_patch_component_ptr(entities[i]);
    if (foliage == nullptr) {
      continue;
    }

    std::uint32_t instanceCount = foliage->instanceCount;
    if (instanceCount >
        static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances)) {
      instanceCount =
          static_cast<std::uint32_t>(FoliagePatchComponent::kMaxInstances);
    }

    for (std::uint32_t instanceIndex = 0U; instanceIndex < instanceCount;
         ++instanceIndex) {
      const FoliageInstance &instance = foliage->instances[instanceIndex];
      std::uint32_t lodIndex = instance.lodIndex;
      if (lodIndex >=
          static_cast<std::uint32_t>(FoliagePatchComponent::kMaxLods)) {
        lodIndex = 0U;
      }

      const renderer::AssetId meshAsset =
          fallback_foliage_mesh_asset(*foliage, lodIndex);
      if (meshAsset == renderer::kInvalidAssetId) {
        continue;
      }

      const renderer::MeshHandle runtimeMesh =
          renderer::resolve_mesh_asset(jobData->assetDatabase, meshAsset);
      if (runtimeMesh == renderer::kInvalidMeshHandle) {
        continue;
      }

      const renderer::GpuMesh *mesh =
          renderer::lookup_gpu_mesh(jobData->meshRegistry, runtimeMesh);
      if (mesh == nullptr) {
        continue;
      }

      const float safeScale =
          (instance.scale > 0.0F) ? instance.scale : 1.0F;
      const math::Mat4 instanceLocal = math::compose_trs(
          instance.offset, math::Quat(), math::Vec3(safeScale, safeScale,
                                                    safeScale));
      const math::Mat4 model = math::mul(transforms[i].matrix, instanceLocal);
      const math::Vec4 center4 =
          math::mul(model, math::Vec4(0.0F, 0.0F, 0.0F, 1.0F));
      const math::Vec3 center(center4.x, center4.y, center4.z);
      const math::Vec3 half(0.5F * safeScale, 0.5F * safeScale,
                            0.5F * safeScale);
      if (aabb_culled_by_frustum(frustumPlanes, center, half)) {
        continue;
      }

      renderer::DrawCommand command{};
      command.entity = entities[i].index;
      command.mesh = runtimeMesh;
      command.material.albedo = foliage->albedo;
      command.material.roughness = foliage->roughness;
      command.material.metallic = foliage->metallic;
      command.material.opacity = foliage->opacity;
      command.modelMatrix = model;
      command.foliageWindStrength = foliage->windStrength;
      command.foliageWindFrequency = foliage->windFrequency;
      command.foliageWindPhase = instance.phase;
      command.foliageLodIndex = lodIndex;
      command.sortKey.value =
          build_draw_sort_key(command.material, runtimeMesh, center, vp);

      if (!submit_render_command(localBuffer, command,
                                 jobData->frameGraphFailed)) {
        return;
      }
    }
  }
}

void merge_command_buffers_job(void *userData) noexcept {
  auto *jobData = static_cast<MergeCommandsJobData *>(userData);
  if ((jobData == nullptr) || (jobData->merged == nullptr) ||
      (jobData->localBuffers == nullptr)) {
    return;
  }

  jobData->merged->reset();
  for (std::size_t i = 0U; i < jobData->threadCount; ++i) {
    if (!jobData->merged->append_from(jobData->localBuffers[i])) {
      mark_graph_failed(jobData->frameGraphFailed);
      return;
    }
  }
  jobData->merged->sort_by_key();
}

bool link_dependency(core::JobHandle prerequisite,
                     core::JobHandle dependent) noexcept {
  if (!core::is_valid_handle(prerequisite) ||
      !core::is_valid_handle(dependent)) {
    return false;
  }

  return core::add_dependency(prerequisite, dependent);
}

} // namespace

bool enqueue_render_prep_pipeline(
    RenderPrepPipelineContext *context, const World *world,
    renderer::CommandBufferBuilder *mergedCommandBuffer,
    renderer::AssetDatabase *assetDatabase,
    const renderer::GpuMeshRegistry *meshRegistry,
    core::JobHandle renderPrepPhaseHandle, core::JobHandle renderPhaseHandle,
    std::atomic<bool> *frameGraphFailed, std::size_t frameThreadCount,
    std::size_t chunkSize, const math::Mat4 &viewProjection,
    core::JobHandle *outMergeHandle) noexcept {
  if ((context == nullptr) || (world == nullptr) ||
      (mergedCommandBuffer == nullptr) || (assetDatabase == nullptr) ||
      (meshRegistry == nullptr) || (chunkSize == 0U) ||
      !core::is_valid_handle(renderPrepPhaseHandle) ||
      !core::is_valid_handle(renderPhaseHandle) ||
      (outMergeHandle == nullptr)) {
    return false;
  }

  *outMergeHandle = {};

  if ((frameThreadCount == 0U) ||
      (frameThreadCount > context->localCommandBuffers.size())) {
    return false;
  }

  for (std::size_t i = 0U; i < frameThreadCount; ++i) {
    context->localCommandBuffers[i].reset();
  }

  const std::size_t transformCount = world->transform_count();

  std::size_t renderPrepJobCursor = 0U;
  std::size_t renderPrepHandleCount = 0U;

  for (std::size_t start = 0U; start < transformCount; start += chunkSize) {
    if ((renderPrepJobCursor >= context->renderPrepJobData.size()) ||
        (renderPrepHandleCount >= context->renderPrepJobHandles.size())) {
      return false;
    }

    const std::size_t count = ((start + chunkSize) > transformCount)
                                  ? (transformCount - start)
                                  : chunkSize;

    RenderPrepChunkJobData &prepData =
        context->renderPrepJobData[renderPrepJobCursor];
    prepData.world = world;
    prepData.startIndex = start;
    prepData.count = count;
    prepData.threadCount = frameThreadCount;
    prepData.localBuffers = context->localCommandBuffers.data();
    prepData.assetDatabase = assetDatabase;
    prepData.meshRegistry = meshRegistry;
    prepData.frameGraphFailed = frameGraphFailed;
    prepData.viewProjection = viewProjection;

    core::Job renderPrepJob{};
    renderPrepJob.function = &render_prep_chunk_job;
    renderPrepJob.data = &prepData;
    const core::JobHandle prepHandle = core::submit(renderPrepJob);
    if (!core::is_valid_handle(prepHandle)) {
      return false;
    }

    if (!link_dependency(renderPrepPhaseHandle, prepHandle) ||
        !link_dependency(prepHandle, renderPhaseHandle)) {
      return false;
    }

    context->renderPrepJobHandles[renderPrepHandleCount] = prepHandle;
    ++renderPrepHandleCount;
    ++renderPrepJobCursor;
  }

  context->mergeCommandsJobData.merged = mergedCommandBuffer;
  context->mergeCommandsJobData.localBuffers =
      context->localCommandBuffers.data();
  context->mergeCommandsJobData.threadCount = frameThreadCount;
  context->mergeCommandsJobData.frameGraphFailed = frameGraphFailed;

  core::Job mergeJob{};
  mergeJob.function = &merge_command_buffers_job;
  mergeJob.data = &context->mergeCommandsJobData;
  const core::JobHandle mergeHandle = core::submit(mergeJob);
  if (!core::is_valid_handle(mergeHandle)) {
    return false;
  }

  if (!link_dependency(renderPhaseHandle, mergeHandle)) {
    return false;
  }

  *outMergeHandle = mergeHandle;
  return true;
}

} // namespace engine::runtime
