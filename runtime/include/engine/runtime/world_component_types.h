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

using engine::math::BodyType;
using engine::math::Collider;
using engine::math::ColliderShape;
using engine::math::HullSource;
using engine::math::PrimitiveShape;
using engine::math::RigidBody;
using engine::math::Transform;

using engine::math::CameraComponent;
using engine::math::CameraProjection;
using engine::math::character_controller_is_valid;
using engine::math::CharacterControllerComponent;
using engine::math::kMaxScriptPropertyNameLength;
using engine::math::LightComponent;
using engine::math::LightType;
using engine::math::MeshComponent;
using engine::math::NameComponent;
using engine::math::nav_agent_is_valid;
using engine::math::NavAgentComponent;
using engine::math::PointLightComponent;
using engine::math::script_properties_are_valid;
using engine::math::script_properties_clear;
using engine::math::script_properties_set;
using engine::math::script_property_override;
using engine::math::ScriptComponent;
using engine::math::ScriptPropertiesComponent;
using engine::math::ScriptPropertyType;
using engine::math::ScriptPropertyValue;
using engine::math::SpotLightComponent;
using engine::math::SpringArmComponent;
using engine::math::tag_set_add;
using engine::math::tag_set_has;
using engine::math::tag_set_is_valid;
using engine::math::tag_set_remove;
using engine::math::TagSetAdd;
using engine::math::TagSetComponent;

/// Propagated world-space transform plus its cached composite matrix.
struct WorldTransform final {
  math::Vec3 position = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Quat rotation = math::Quat();
  math::Vec3 scale = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Mat4 matrix = math::Mat4();
};

/// A reflection probe: the scene captured into a cubemap at the entity's
/// position and baked into image-based light. A view whose camera is inside
/// the probe's box is lit by it instead of the sky (the smaller box wins
/// where boxes overlap). `boxExtents` are the box's half sizes along the
/// world axes, whatever the entity's rotation; `radius` is how far the
/// capture sees; `intensity` scales the light the probe gives;
/// `boxProjection` reflects against the box's walls rather than at
/// infinity. The resolutions and mip count size the capture and its bake.
/// A probe is baked when it appears, moves, changes a field or sees a new
/// sky, when a scene loads or its asset loads finish, and on the
/// Inspector's Bake.
struct ReflectionProbeComponent final {
  math::Vec3 boxExtents = math::Vec3(5.0F, 5.0F, 5.0F);
  float radius = 10.0F;
  float intensity = 1.0F;
  std::uint32_t prefilteredResolution = 128U;
  std::uint32_t irradianceResolution = 32U;
  std::uint32_t mipLevels = 5U;
  bool boxProjection = false;
};

/// The scene's sky light: an environment map (an Environment asset, a
/// Radiance .hdr) that lights every surface as image-based light, specular
/// and diffuse, whatever sky r_sky_model draws. The first sky light in the
/// World lights the scene; its transform is not used. `environmentRef` is
/// the authored identity a scene carries; `environmentAssetId` is the
/// catalog's current answer for it, runtime state that is never serialized.
struct SkyLightComponent final {
  core::AssetRef environmentRef{};
  std::uint64_t environmentAssetId = 0ULL;
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
  /// State-machine parameters one component drives: room for a
  /// character controller's speed, direction, grounded, jump, attack
  /// index, hit, death, emotes and the rest.
  static constexpr std::size_t kMaxParams = 32U;
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

/// Navigation surface (Unity's NavMeshSurface, Godot's NavigationRegion3D):
/// the volume its entity's bake covers, the agent the mesh is baked for,
/// and the .navmesh file the editor's Bake writes and a loaded scene reads.
/// The volume is a world-aligned box of halfExtents around the entity's
/// world position; its rotation and scale do not apply, because the mesh
/// is a grid of world-aligned columns. The baked mesh itself is runtime
/// state, held by whoever loaded the file, never by the component.
struct NavMeshSurfaceComponent final {
  static constexpr std::size_t kMaxPathLength = 127U; // +1 for null
  /// Half the size of the baked volume on each axis, in metres.
  math::Vec3 halfExtents = math::Vec3(25.0F, 10.0F, 25.0F);
  /// Side of one grid column, in metres.
  float cellSize = 0.25F;
  /// The agent's radius: walkable space keeps this far from walls.
  float agentRadius = 0.4F;
  /// The agent's height: walkable space has this much room above it.
  float agentHeight = 1.8F;
  /// The highest step the agent climbs between neighbouring columns.
  float maxClimb = 0.4F;
  /// The steepest walkable slope, in degrees.
  float maxSlopeDegrees = 45.0F;
  /// The .navmesh file, a VFS path; empty until the first bake names one.
  char navMeshPath[kMaxPathLength + 1U] = {};
};

/// True when `surface` can be baked: positive, finite half extents whose
/// volume and agent settings make valid bake settings (no more columns
/// than one bake samples), and a path that is empty or names a .navmesh
/// file.
bool nav_mesh_surface_is_valid(const NavMeshSurfaceComponent &surface) noexcept;

using TransformVisitor = void (*)(Entity entity, const Transform &transform,
                                  void *userData) noexcept;

} // namespace engine::runtime
