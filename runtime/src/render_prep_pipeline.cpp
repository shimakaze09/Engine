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
#include "engine/physics/collider.h"
#include "engine/renderer/command_buffer.h"
#include "engine/physics/physics.h"

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

// Gribb-Hartmann: extract 6 frustum planes from a column-major VP matrix,
// in order left, right, bottom, top, near, far.
// Row j = (columns[0][j], columns[1][j], columns[2][j], columns[3][j])
// where Vec4 x/y/z/w map to indices 0/1/2/3.
void extract_frustum_planes(const math::Mat4 &vp,
                            FrustumPlane planes[6]) noexcept {
  const math::Vec4 c0 = vp.columns[0];
  const math::Vec4 c1 = vp.columns[1];
  const math::Vec4 c2 = vp.columns[2];
  const math::Vec4 c3 = vp.columns[3];
  planes[0] = {c0.w + c0.x, c1.w + c1.x, c2.w + c2.x, c3.w + c3.x};
  planes[1] = {c0.w - c0.x, c1.w - c1.x, c2.w - c2.x, c3.w - c3.x};
  planes[2] = {c0.w + c0.y, c1.w + c1.y, c2.w + c2.y, c3.w + c3.y};
  planes[3] = {c0.w - c0.y, c1.w - c1.y, c2.w - c2.y, c3.w - c3.y};
  // Near plane depends on the device clip-depth convention: GL clips at
  // z = -w (row w+z), the zero-to-one APIs at z = 0 (row z alone).
  if (renderer::device_depth_zero_one()) {
    planes[4] = {c0.z, c1.z, c2.z, c3.z};
  } else {
    planes[4] = {c0.w + c0.z, c1.w + c1.z, c2.w + c2.z, c3.w + c3.z};
  }
  planes[5] = {c0.w - c0.z, c1.w - c1.z, c2.w - c2.z, c3.w - c3.z};
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

/// Conservative test of the box swept along `sweep` (a direction scaled
/// by the sweep distance) against the frustum: the swept solid lies
/// entirely outside a plane only when the box's near corner plus the
/// sweep's reach toward the plane still falls behind it.
bool swept_aabb_culled_by_frustum(const FrustumPlane planes[6],
                                  const math::Vec3 &center,
                                  const math::Vec3 &half,
                                  const math::Vec3 &sweep) noexcept {
  for (int p = 0; p < 6; ++p) {
    const FrustumPlane &plane = planes[p];
    const float px = center.x + (plane.a >= 0.0F ? half.x : -half.x);
    const float py = center.y + (plane.b >= 0.0F ? half.y : -half.y);
    const float pz = center.z + (plane.c >= 0.0F ? half.z : -half.z);
    const float along = (plane.a * sweep.x) + (plane.b * sweep.y) +
                        (plane.c * sweep.z);
    const float reach = (along > 0.0F) ? along : 0.0F;
    if ((plane.a * px + plane.b * py + plane.c * pz + plane.d + reach) <
        0.0F) {
      return true;
    }
  }
  return false;
}

/// Whether a sphere overlaps the axis-aligned box.
bool aabb_intersects_sphere(const math::Vec3 &center, const math::Vec3 &half,
                            const math::Vec3 &spherePos,
                            float radius) noexcept {
  const float dx = std::fmax(std::fabs(spherePos.x - center.x) - half.x, 0.0F);
  const float dy = std::fmax(std::fabs(spherePos.y - center.y) - half.y, 0.0F);
  const float dz = std::fmax(std::fabs(spherePos.z - center.z) - half.z, 0.0F);
  return ((dx * dx) + (dy * dy) + (dz * dz)) <= (radius * radius);
}

/// Per-job view of the auxiliary inputs with each capture's frustum
/// planes extracted once.
struct AuxiliaryCulling final {
  const RenderPrepAuxiliaryInputs *inputs = nullptr;
  math::Vec3 sweep{};
  FrustumPlane capturePlanes[renderer::kMaxSceneCaptures][6] = {};
};

void prepare_auxiliary_culling(const RenderPrepAuxiliaryInputs *inputs,
                               AuxiliaryCulling *out) noexcept {
  out->inputs = inputs;
  if (inputs == nullptr) {
    return;
  }
  out->sweep = math::mul(inputs->lightDirection, inputs->sweepDistance);
  const std::size_t captureCount =
      (inputs->captureCount < renderer::kMaxSceneCaptures)
          ? inputs->captureCount
          : renderer::kMaxSceneCaptures;
  for (std::size_t i = 0U; i < captureCount; ++i) {
    extract_frustum_planes(inputs->captureViewProjections[i],
                           out->capturePlanes[i]);
  }
}

/// Which auxiliary passes want a draw the camera culled: zero drops it.
std::uint16_t auxiliary_pass_mask(const AuxiliaryCulling &aux,
                                  const FrustumPlane cameraPlanes[6],
                                  const math::Vec3 &center,
                                  const math::Vec3 &half) noexcept {
  if (aux.inputs == nullptr) {
    return 0U;
  }
  std::uint16_t mask = 0U;
  if (aux.inputs->directionalShadow &&
      !swept_aabb_culled_by_frustum(cameraPlanes, center, half, aux.sweep)) {
    mask |= renderer::kPassShadowCaster;
  }
  if ((mask & renderer::kPassShadowCaster) == 0U) {
    const std::size_t casterCount =
        (aux.inputs->localCasterCount < aux.inputs->localCasters.size())
            ? aux.inputs->localCasterCount
            : aux.inputs->localCasters.size();
    for (std::size_t i = 0U; i < casterCount; ++i) {
      const RenderPrepAuxiliaryInputs::LocalCaster &caster =
          aux.inputs->localCasters[i];
      if (aabb_intersects_sphere(center, half, caster.position,
                                 caster.radius)) {
        mask |= renderer::kPassShadowCaster;
        break;
      }
    }
  }
  const std::size_t captureCount =
      (aux.inputs->captureCount < renderer::kMaxSceneCaptures)
          ? aux.inputs->captureCount
          : renderer::kMaxSceneCaptures;
  for (std::size_t i = 0U; i < captureCount; ++i) {
    if (!aabb_culled_by_frustum(aux.capturePlanes[i], center, half)) {
      mask |= static_cast<std::uint16_t>(renderer::kPassCaptureBase
                                         << static_cast<unsigned int>(i));
    }
  }
  return mask;
}

/// Builds the 64-bit draw sort key, MSB→LSB:
/// transparent:1 | shader:7 (0 = PBR, the only shader) | texture:20 |
/// mesh:20 | depth:16.
std::uint64_t build_draw_sort_key(const renderer::Material &material,
                                  renderer::MeshHandle runtimeMesh,
                                  const math::Vec3 &center,
                                  const math::Mat4 &viewProjection) noexcept {
  const bool transparent = (material.opacity < 1.0F);
  const std::uint64_t transparentBit = transparent ? (1ULL << 63U) : 0ULL;

  const std::uint64_t shaderBits = 0ULL;

  const std::uint64_t textureBits =
      (static_cast<std::uint64_t>(material.albedoTexture.id) & 0xFFFFFULL)
      << 36U;

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

/// Submits a draw to the thread's buffer. A full buffer drops the draw and
/// counts it; it is a per-frame degradation the pipeline reports once and
/// surfaces in EngineStats, never a graph failure — treating it as one
/// made more than kMaxDrawCommands visible draws a fatal run exit.
bool submit_render_command(renderer::CommandBufferBuilder &localBuffer,
                           const renderer::DrawCommand &command,
                           std::atomic<std::uint32_t> *droppedDrawCommands)
    noexcept {
  if (localBuffer.submit(command)) {
    return true;
  }

  if (droppedDrawCommands != nullptr) {
    droppedDrawCommands->fetch_add(1U, std::memory_order_relaxed);
  }
  return false;
}

renderer::AssetId
fallback_foliage_mesh_asset(const FoliagePatchComponent &foliage,
                            std::uint32_t lodIndex) noexcept {
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

  const math::Mat4 &vp = jobData->viewProjection;
  FrustumPlane frustumPlanes[6];
  extract_frustum_planes(vp, frustumPlanes);
  AuxiliaryCulling auxiliary{};
  prepare_auxiliary_culling(jobData->auxiliary, &auxiliary);

  for (std::size_t i = 0U; i < jobData->count; ++i) {
    const MeshComponent *meshComponent =
        jobData->world->get_mesh_component_ptr(entities[i]);
    if (meshComponent != nullptr) {
      const Collider *collider = jobData->world->get_collider_ptr(entities[i]);
      math::Vec3 center = transforms[i].position;
      math::Vec3 half = math::transform_aabb_half_extents(
          transforms[i].matrix, math::Vec3(0.5F, 0.5F, 0.5F));
      if (collider != nullptr) {
        const physics::ConvexHullData *hull =
            (collider->shape == ColliderShape::ConvexHull)
                ? physics::get_convex_hull_data(
                      jobData->world->physics_context(), entities[i])
                : nullptr;
        physics::ColliderWorldGeometry geometry{};
        if (physics::make_collider_world_geometry(
                *collider, transforms[i].matrix, hull, &geometry)) {
          center = math::aabb_center(geometry.worldAabb);
          half = math::aabb_half_extents(geometry.worldAabb);
        }
      }

      const std::uint16_t passMask =
          aabb_culled_by_frustum(frustumPlanes, center, half)
              ? auxiliary_pass_mask(auxiliary, frustumPlanes, center, half)
              : renderer::kPassCamera;
      if (passMask != 0U) {
        const renderer::MeshHandle runtimeMesh = renderer::resolve_mesh_asset(
            jobData->assetDatabase, meshComponent->meshAssetId);
        if (runtimeMesh != renderer::kInvalidMeshHandle) {
          const renderer::GpuMesh *mesh =
              renderer::lookup_gpu_mesh(jobData->meshRegistry, runtimeMesh);
          if (mesh != nullptr) {
            renderer::DrawCommand command{};
            command.entity = entities[i].index;
            command.mesh = runtimeMesh;
            // Material asset reference wins; inline fields are the fallback
            // (also used when the referenced material is not resolvable).
            const renderer::Material *materialParams =
                (meshComponent->materialAssetId != 0ULL)
                    ? renderer::find_material_params(
                          jobData->assetDatabase,
                          meshComponent->materialAssetId)
                    : nullptr;
            if (materialParams != nullptr) {
              command.material = *materialParams;
            } else {
              command.material.albedo = meshComponent->albedo;
              command.material.roughness = meshComponent->roughness;
              command.material.metallic = meshComponent->metallic;
              command.material.opacity = meshComponent->opacity;
            }
            // A referenced scene capture overrides the albedo texture; the
            // handle is a stable read-only registration, so this is safe
            // from worker threads.
            if (meshComponent->sceneCaptureSourceId != 0U) {
              const Entity captureEntity =
                  jobData->world->find_entity_by_persistent_id(
                      meshComponent->sceneCaptureSourceId);
              const std::int32_t captureSlot =
                  jobData->world->scene_capture_slot_for_entity(captureEntity);
              if (captureSlot >= 0) {
                const renderer::TextureHandle captureTexture =
                    renderer::scene_capture_texture_handle(
                        static_cast<std::size_t>(captureSlot));
                if (captureTexture != renderer::kInvalidTextureHandle) {
                  command.material.albedoTexture = captureTexture;
                }
              }
            }
            command.modelMatrix = transforms[i].matrix;
            if (jobData->interpolationAlpha < 1.0F) {
              math::Vec3 previousPosition{};
              math::Quat previousRotation{};
              math::Vec3 previousScale{};
              if (jobData->world->get_previous_world_trs(
                      entities[i], &previousPosition, &previousRotation,
                      &previousScale)) {
                const float alpha = jobData->interpolationAlpha;
                const WorldTransform &current = transforms[i];
                command.modelMatrix = math::compose_trs(
                    math::add(previousPosition,
                              math::mul(math::sub(current.position,
                                                  previousPosition),
                                        alpha)),
                    math::slerp(previousRotation, current.rotation, alpha),
                    math::add(previousScale,
                              math::mul(math::sub(current.scale,
                                                  previousScale),
                                        alpha)));
              }
            }
            static_assert(renderer::kInvalidSkinPalette == kInvalidAnimSlot,
                          "paletteSlot passes through to DrawCommand "
                          "unchanged, so the two invalid sentinels must "
                          "stay numerically equal");
            const AnimationComponent *animation =
                jobData->world->get_animation_component_ptr(entities[i]);
            if (animation != nullptr) {
              command.skinPalette = animation->paletteSlot;
            }
            command.sortKey.value =
                build_draw_sort_key(command.material, runtimeMesh, center, vp);
            command.passMask = passMask;

            // Counted and skipped: every later submit into a full buffer
            // fails the same way, so the count stays exact.
            static_cast<void>(submit_render_command(
                localBuffer, command, jobData->droppedDrawCommands));
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

      const float safeScale = (instance.scale > 0.0F) ? instance.scale : 1.0F;
      const math::Mat4 instanceLocal =
          math::compose_trs(instance.offset, math::Quat(),
                            math::Vec3(safeScale, safeScale, safeScale));
      const math::Mat4 model = math::mul(transforms[i].matrix, instanceLocal);
      const math::Vec4 center4 =
          math::mul(model, math::Vec4(0.0F, 0.0F, 0.0F, 1.0F));
      const math::Vec3 center(center4.x, center4.y, center4.z);
      const math::Vec3 half(0.5F * safeScale, 0.5F * safeScale,
                            0.5F * safeScale);
      const std::uint16_t passMask =
          aabb_culled_by_frustum(frustumPlanes, center, half)
              ? auxiliary_pass_mask(auxiliary, frustumPlanes, center, half)
              : renderer::kPassCamera;
      if (passMask == 0U) {
        continue;
      }

      renderer::DrawCommand command{};
      command.entity = entities[i].index;
      command.mesh = runtimeMesh;
      command.passMask = passMask;
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

      static_cast<void>(submit_render_command(
          localBuffer, command, jobData->droppedDrawCommands));
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
  if (jobData->mergedAuxiliary != nullptr) {
    jobData->mergedAuxiliary->reset();
  }
  std::uint32_t dropped = 0U;
  for (std::size_t i = 0U; i < jobData->threadCount; ++i) {
    const renderer::CommandBufferView local = jobData->localBuffers[i].view();
    for (std::uint32_t c = 0U; c < local.count; ++c) {
      const renderer::DrawCommand &command = local.data[c];
      // Camera-visible commands feed the main list; the rest go to the
      // auxiliary list for the shadow and capture passes. Either
      // merged buffer has the capacity of one thread's buffer, so the sum
      // of the locals can exceed it; a command that does not fit is
      // dropped and counted, and the frame draws what did fit.
      renderer::CommandBufferBuilder *target =
          ((command.passMask & renderer::kPassCamera) != 0U)
              ? jobData->merged
              : jobData->mergedAuxiliary;
      if ((target == nullptr) || !target->submit(command)) {
        ++dropped;
      }
    }
  }
  if ((dropped > 0U) && (jobData->droppedDrawCommands != nullptr)) {
    jobData->droppedDrawCommands->fetch_add(dropped,
                                            std::memory_order_relaxed);
  }
  jobData->merged->sort_by_key();
  if (jobData->mergedAuxiliary != nullptr) {
    jobData->mergedAuxiliary->sort_by_key();
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

} // namespace

bool enqueue_render_prep_pipeline(
    RenderPrepPipelineContext *context, const World *world,
    renderer::CommandBufferBuilder *mergedCommandBuffer,
    renderer::AssetDatabase *assetDatabase,
    const renderer::GpuMeshRegistry *meshRegistry,
    core::JobHandle renderPrepPhaseHandle, core::JobHandle renderPhaseHandle,
    std::atomic<bool> *frameGraphFailed,
    std::atomic<std::uint32_t> *droppedDrawCommands,
    std::size_t frameThreadCount, std::size_t chunkSize,
    const math::Mat4 &viewProjection, float interpolationAlpha,
    core::JobHandle *outMergeHandle,
    renderer::CommandBufferBuilder *mergedAuxiliaryBuffer,
    const RenderPrepAuxiliaryInputs *auxiliary) noexcept {
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
    prepData.droppedDrawCommands = droppedDrawCommands;
    prepData.viewProjection = viewProjection;
    prepData.interpolationAlpha = interpolationAlpha;
    prepData.auxiliary =
        (mergedAuxiliaryBuffer != nullptr) ? auxiliary : nullptr;

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
  context->mergeCommandsJobData.mergedAuxiliary =
      (auxiliary != nullptr) ? mergedAuxiliaryBuffer : nullptr;
  context->mergeCommandsJobData.localBuffers =
      context->localCommandBuffers.data();
  context->mergeCommandsJobData.threadCount = frameThreadCount;
  context->mergeCommandsJobData.frameGraphFailed = frameGraphFailed;
  context->mergeCommandsJobData.droppedDrawCommands = droppedDrawCommands;

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
