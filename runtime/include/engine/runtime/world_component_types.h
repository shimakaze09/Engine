// Declares the runtime component types (#166 W1): the component PODs and
// their enums, extracted verbatim from world.h — they have no dependency
// on the World storage type and every consumer keeps reaching them
// through world.h's include.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/core/entity.h"
#include "engine/math/component_types.h"
#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/vec3.h"
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

/// Propagated world-space transform plus its cached composite matrix.
struct WorldTransform final {
  math::Vec3 position = math::Vec3(0.0F, 0.0F, 0.0F);
  math::Quat rotation = math::Quat();
  math::Vec3 scale = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Mat4 matrix = math::Mat4();
};

/// Fixed-capacity display name (31 chars + terminator).
struct NameComponent final {
  static constexpr std::size_t kMaxNameLength = 31U; // +1 for null terminator
  char name[kMaxNameLength + 1U] = {};
};

/// Enumerates light type values used by the engine.
enum class LightType : std::uint8_t { Directional = 0, Point = 1 };

/// Directional or point light: color, direction, and intensity.
struct LightComponent final {
  math::Vec3 color = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Vec3 direction = math::Vec3(0.4F, -1.0F, 0.6F);
  float intensity = 1.0F;
  LightType type = LightType::Directional;
};

/// Point light with color, intensity, and attenuation radius.
struct PointLightComponent final {
  math::Vec3 color = math::Vec3(1.0F, 1.0F, 1.0F);
  float intensity = 1.0F;
  float radius = 10.0F;
};

/// Spot light: color, direction, cone angles (radians), and radius.
struct SpotLightComponent final {
  math::Vec3 color = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Vec3 direction = math::Vec3(0.0F, -1.0F, 0.0F);
  float intensity = 1.0F;
  float radius = 10.0F;
  float innerConeAngle = 0.3491F; // ~20 degrees in radians
  float outerConeAngle = 0.5236F; // ~30 degrees in radians
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

// Attaches a Lua script file to an entity.
// The script must return a module table with optional on_start(self) and
// on_update(self, dt) functions. Multiple entities may share the same file.
struct ScriptComponent final {
  static constexpr std::size_t kMaxPathLength = 127U; // +1 for null terminator
  char scriptPath[kMaxPathLength + 1U] = {};
};

// Renderer-facing component; keep minimal to avoid bloating draw commands.
// When materialAssetId is set (non-zero) render prep uses the resolved
// material asset's parameters; the inline fields below are the fallback.
// sceneCaptureSourceId names the persistent id of an entity carrying an
// enabled SceneCaptureComponent; when resolvable, that capture's output
// becomes this mesh's albedo texture (overriding any material texture).
struct MeshComponent final {
  std::uint64_t meshAssetId = 0ULL;
  std::uint64_t materialAssetId = 0ULL;
  math::Vec3 albedo = math::Vec3(1.0F, 1.0F, 1.0F);
  float roughness = 0.5F;
  float metallic = 0.0F;
  float opacity = 1.0F;
  std::uint32_t sceneCaptureSourceId = 0U;
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
struct FoliagePatchComponent final {
  static constexpr std::size_t kMaxInstances = 64U;
  static constexpr std::size_t kMaxLods = 3U;

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

/// Perspective vs orthographic projection selection for CameraComponent.
/// Stored as a plain uint32 (not a scoped enum) so the field stays in the
/// component's fully-generic reflected codec/editor path alongside its
/// other numeric fields; this alias exists only for readable C++ compares.
enum class CameraProjection : std::uint32_t { Perspective = 0U,
                                              Orthographic = 1U };

/// First-class authored camera (issue #161). Pose is never stored here: it
/// comes from the entity's world transform (looks along the rotated -Z
/// axis, up is the rotated +Y axis, matching SceneCaptureComponent's
/// convention) so authoring a camera never duplicates Transform state.
/// update_persistent_cameras publishes the derived pose into the owning
/// World's CameraManager priority stack every frame, so an authored camera
/// participates in the same priority/blend/shake model Lua-pushed and
/// spring-arm cameras already use -- it is not a second camera stack.
/// `projection` selects the render path's real projection (#221): the
/// flush, render-prep culling, cascaded shadows, and lighting all honor
/// Orthographic (half-height `orthographicSize`), while the sky pass keeps
/// perspective directional sampling — parallel rays would all sample one
/// sky direction.
/// When the owning entity also carries a SpringArmComponent, the spring arm
/// supplies position/target (its collision-aware boom) and this component
/// only contributes fovRadians/near/far/priority/blendSpeed/active -- the
/// standard authored third-person rig.
struct CameraComponent final {
  std::uint32_t projection = 0U; // CameraProjection::Perspective
  float fovRadians = 1.0471975512F; // 60 degrees; used when Perspective
  float orthographicSize = 5.0F;    // half-height, world units; Orthographic
  float nearPlane = 0.1F;
  float farPlane = 100.0F;
  float priority = 0.0F;
  float blendSpeed = 5.0F; ///< How fast CameraManager blends toward this.
  bool active = true;
};

/// Spring arm component: drives a third-person camera boom that shortens on
/// collision and smoothly interpolates length.
struct SpringArmComponent final {
  float armLength = 5.0F;     ///< Desired arm length (world units).
  float currentLength = 5.0F; ///< Interpolated length after collision.
  math::Vec3 offset =
      math::Vec3(0.0F, 1.0F, 0.0F); ///< Entity-local pivot offset (scaled and
                                    ///< rotated into world space).
  float lagSpeed = 8.0F;         ///< Smoothing interpolation rate.
  float collisionRadius = 0.25F; ///< Sphere sweep radius.
  bool collisionEnabled = true;  ///< Sweep-clamp the arm against colliders.
};

using TransformVisitor = void (*)(Entity entity, const Transform &transform,
                                  void *userData) noexcept;

} // namespace engine::runtime
