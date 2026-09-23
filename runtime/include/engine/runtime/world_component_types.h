// Declares the runtime component types: the component PODs and their
// enums that depend on runtime-only types, plus the re-export of the
// scene components math defines. None depends on the World storage type
// and every consumer keeps reaching them through world.h's include.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/core/entity.h"
#include "engine/math/component_types.h"
#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
#include "engine/math/world_component_types.h"
#include "engine/runtime/animation.h"

namespace engine::runtime {

// Types owned by core/math modules, re-exported into engine::runtime
// (world.h repeats the core set; repeated using-declarations are benign).
using engine::core::Entity;
using engine::core::kInvalidEntity;
using engine::core::kInvalidPersistentId;
using engine::core::PersistentId;

using engine::math::Collider;
using engine::math::ColliderShape;
using engine::math::HullSource;
using engine::math::MovementAuthority;
using engine::math::RigidBody;
using engine::math::Transform;

using engine::math::CameraComponent;
using engine::math::CameraProjection;
using engine::math::LightComponent;
using engine::math::LightType;
using engine::math::MeshComponent;
using engine::math::NameComponent;
using engine::math::PointLightComponent;
using engine::math::ScriptComponent;
using engine::math::SpotLightComponent;
using engine::math::SpringArmComponent;

/// Propagated world-space transform plus its cached composite matrix.
struct WorldTransform final {
  math::Vec3 position = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Quat rotation = math::Quat();
  math::Vec3 scale = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Mat4 matrix = math::Mat4();
};

/// IBL reflection probe: bake resolutions, influence shape, bake flag.
struct ReflectionProbeComponent final {
  math::Vec3 boxExtents = math::Vec3(5.0F, 5.0F, 5.0F);
  float radius = 10.0F;
  float intensity = 1.0F;
  std::uint32_t prefilteredResolution = 128U;
  std::uint32_t irradianceResolution = 32U;
  std::uint32_t brdfLutResolution = 512U;
  std::uint32_t mipLevels = 5U;
  bool boxProjection = false;
  bool needsBake = true;
};

/// Renders the scene from this entity each frame into an offscreen texture
/// (render-to-texture). View position/orientation come from the entity's
/// world transform (looks along the rotated -Z axis); the renderer clamps
/// the resolution into its supported range.
struct SceneCaptureComponent final {
  std::uint32_t width = 256U;
  std::uint32_t height = 256U;
  float fovRadians = 1.0471975512F; // 60 degrees
  float nearPlane = 0.1F;
  float farPlane = 100.0F;
  bool enabled = true;
};

/// One foliage instance: offset from the patch origin, scale, wind
/// phase, and LOD.
struct FoliageInstance final {
  math::Vec3 offset = math::Vec3(0.0F, 0.0F, 0.0F);
  float scale = 1.0F;
  float phase = 0.0F;
  std::uint32_t lodIndex = 0U;
};

/// Instanced foliage patch: per-LOD meshes, material, wind, instances.
/// `meshRefs` are the authored identities, one per LOD; `meshAssetIds`
/// beside them hold what the reference pass resolved each to, so they are
/// runtime state and never serialized (the same split MeshComponent
/// makes).
struct FoliagePatchComponent final {
  static constexpr std::size_t kMaxInstances = 64U;
  static constexpr std::size_t kMaxLods = 3U;

  core::AssetRef meshRefs[kMaxLods] = {};
  std::uint64_t meshAssetIds[kMaxLods] = {};
  std::uint32_t instanceCount = 0U;
  float density = 1.0F;
  math::Vec3 albedo = math::Vec3(0.25F, 0.65F, 0.25F);
  float roughness = 0.85F;
  float metallic = 0.0F;
  float opacity = 1.0F;
  float windStrength = 0.14F;
  float windFrequency = 1.6F;
  FoliageInstance instances[kMaxInstances] = {};
};

/// One animation parameter the state machine's transitions read; set from
/// gameplay by name hash.
struct AnimParam final {
  std::uint32_t nameHash = 0U;
  float value = 0.0F;
};

/// Skeletal animation playback: references an animation controller JSON
/// (skeleton, clips, states, transitions) by VFS path and carries the
/// runtime state-machine position, crossfade progress, parameters, and
/// the renderer palette slot assigned for the current frame.
struct AnimationComponent final {
  static constexpr std::size_t kMaxPathLength = 127U; // +1 for null
  static constexpr std::size_t kMaxParams = 8U;
  char controllerPath[kMaxPathLength + 1U] = {};
  float playbackSpeed = 1.0F;
  bool playing = true;
  std::uint32_t controllerSlot = kInvalidAnimSlot;
  std::uint32_t currentState = 0U;
  std::uint32_t previousState = 0U;
  float stateTime = 0.0F;
  float previousStateTime = 0.0F;
  float blendRemaining = 0.0F;
  float blendDuration = 0.0F;
  std::uint32_t paletteSlot = kInvalidAnimSlot;
  std::uint32_t paramCount = 0U;
  AnimParam params[kMaxParams] = {};
};

using TransformVisitor = void (*)(Entity entity, const Transform &transform,
                                  void *userData) noexcept;

} // namespace engine::runtime
