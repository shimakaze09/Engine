// Declares the plain-data scene component types the runtime World stores
// and the scripting bridge carries: names, tags, lights, scripts, meshes,
// cameras and spring arms. They live below both modules, like the
// transform and physics components, so scripting binds them without a
// runtime header and the World re-exports them unchanged.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/core/asset_identity.h"
#include "engine/core/string_util.h"
#include "engine/math/vec3.h"

namespace engine::math {

/// An entity's name: up to 127 bytes of UTF-8 plus the terminator, room
/// for long asset filenames and about 40 CJK characters. Names are looked
/// up by hash, so a longer one is refused, never cut.
struct NameComponent final {
  static constexpr std::size_t kMaxNameLength = 127U; // +1 for null terminator
  char name[kMaxNameLength + 1U] = {};
};

/// The gameplay tags an entity carries, as Godot's groups and Unreal's actor
/// Tags are: up to kMaxTags name tokens (core::name_token_is_valid, at most
/// kMaxTagLength characters), distinct ignoring ASCII case, in the order
/// they were given. Scripts and tools find entities by tag instead of by a
/// unique name (World::find_entities_by_tag). Tags are free-form; a project
/// list of known tags is a later layer over the same text.
struct TagSetComponent final {
  static constexpr std::size_t kMaxTags = 8U;
  static constexpr std::size_t kMaxTagLength = 31U;
  std::uint32_t count = 0U;
  char tags[kMaxTags][kMaxTagLength + 1U] = {};
};

/// True when `tags` holds `tag`, compared ignoring ASCII case.
[[nodiscard]] inline bool tag_set_has(const TagSetComponent &tags,
                                      const char *tag) noexcept {
  const std::size_t count = (tags.count < TagSetComponent::kMaxTags)
                                ? tags.count
                                : TagSetComponent::kMaxTags;
  for (std::size_t i = 0U; i < count; ++i) {
    if (core::equals_ignoring_case(tags.tags[i], tag)) {
      return true;
    }
  }
  return false;
}

/// True when every tag is a terminated name token, the count fits, and no
/// two tags are equal ignoring case: the only sets a World stores.
[[nodiscard]] inline bool
tag_set_is_valid(const TagSetComponent &tags) noexcept {
  if (tags.count > TagSetComponent::kMaxTags) {
    return false;
  }
  for (std::size_t i = 0U; i < tags.count; ++i) {
    const char *tag = tags.tags[i];
    if ((std::memchr(tag, '\0', TagSetComponent::kMaxTagLength + 1U) ==
         nullptr) ||
        !core::name_token_is_valid(tag, TagSetComponent::kMaxTagLength)) {
      return false;
    }
    for (std::size_t j = 0U; j < i; ++j) {
      if (core::equals_ignoring_case(tags.tags[j], tag)) {
        return false;
      }
    }
  }
  return true;
}

/// What adding a tag to a set did.
enum class TagSetAdd : std::uint8_t {
  Added,
  AlreadyPresent,
  InvalidTag,
  Full,
};

/// Appends `tag` unless it is already there (ignoring case), is not a name
/// token of at most kMaxTagLength characters, or the set is full; the set
/// is unchanged on anything but Added. Never truncates.
inline TagSetAdd tag_set_add(TagSetComponent *tags, const char *tag) noexcept {
  if ((tags == nullptr) ||
      !core::name_token_is_valid(tag, TagSetComponent::kMaxTagLength)) {
    return TagSetAdd::InvalidTag;
  }
  if (tag_set_has(*tags, tag)) {
    return TagSetAdd::AlreadyPresent;
  }
  if (tags->count >= TagSetComponent::kMaxTags) {
    return TagSetAdd::Full;
  }
  char *slot = tags->tags[tags->count];
  std::memset(slot, 0, TagSetComponent::kMaxTagLength + 1U);
  std::memcpy(slot, tag, std::strlen(tag));
  ++tags->count;
  return TagSetAdd::Added;
}

/// Removes `tag` (ignoring case), keeping the others in order; false when
/// the set does not hold it.
inline bool tag_set_remove(TagSetComponent *tags, const char *tag) noexcept {
  if (tags == nullptr) {
    return false;
  }
  for (std::size_t i = 0U; i < tags->count; ++i) {
    if (core::equals_ignoring_case(tags->tags[i], tag)) {
      for (std::size_t j = i + 1U; j < tags->count; ++j) {
        std::memcpy(tags->tags[j - 1U], tags->tags[j],
                    TagSetComponent::kMaxTagLength + 1U);
      }
      --tags->count;
      std::memset(tags->tags[tags->count], 0,
                  TagSetComponent::kMaxTagLength + 1U);
      return true;
    }
  }
  return false;
}

/// Enumerates light type values used by the engine.
enum class LightType : std::uint8_t { Directional = 0, Point = 1 };

/// Number of LightType values; a stored type at or past it is one this
/// build does not know, and every ingress refuses it rather than guess.
inline constexpr std::uint32_t kLightTypeCount = 2U;

/// True when `type` names a LightType this build knows.
[[nodiscard]] constexpr bool light_type_known(std::uint32_t type) noexcept {
  return type < kLightTypeCount;
}

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
  /// Renders a cubemap depth pass for this light when set; the four
  /// nearest flagged lights cast per frame.
  bool castShadow = false;
};

/// Spot light: color, direction, cone angles (radians), and radius.
struct SpotLightComponent final {
  math::Vec3 color = math::Vec3(1.0F, 1.0F, 1.0F);
  math::Vec3 direction = math::Vec3(0.0F, -1.0F, 0.0F);
  float intensity = 1.0F;
  float radius = 10.0F;
  float innerConeAngle = 0.3491F; // ~20 degrees in radians
  float outerConeAngle = 0.5236F; // ~30 degrees in radians
  /// Renders a depth pass for this light when set; the four nearest
  /// flagged lights cast per frame.
  bool castShadow = false;
};

// Attaches a Lua script file to an entity.
// The script must return a module table with optional on_start(self) and
// on_update(self, dt) functions. Multiple entities may share the same file.
struct ScriptComponent final {
  static constexpr std::size_t kMaxPathLength = 127U; // +1 for null terminator
  char scriptPath[kMaxPathLength + 1U] = {};
};

// Renderer-facing component; keep minimal to avoid bloating draw commands.
// When the material reference resolves, render prep uses that material
// asset's parameters; the inline fields below are the fallback.
// sceneCaptureSourceId names the persistent id of an entity carrying an
// enabled SceneCaptureComponent; when resolvable, that capture's output
// becomes this mesh's albedo texture (overriding any material texture).
//
// `meshRef` and `materialRef` are the authored identities: they are what a
// scene or prefab carries, and they survive the asset being renamed,
// moved or recooked. The two ids beside them are the resolution's
// result — the catalog's current answer for where those assets live — so
// they are runtime state, never serialized, and empty until the reference
// pass has run.
struct MeshComponent final {
  core::AssetRef meshRef{};
  core::AssetRef materialRef{};
  std::uint64_t meshAssetId = 0ULL;
  std::uint64_t materialAssetId = 0ULL;
  math::Vec3 albedo = math::Vec3(1.0F, 1.0F, 1.0F);
  float roughness = 0.5F;
  float metallic = 0.0F;
  float opacity = 1.0F;
  std::uint32_t sceneCaptureSourceId = 0U;
};

/// Perspective vs orthographic projection selection for CameraComponent.
/// Stored as a plain uint32 (not a scoped enum) so the field stays in the
/// component's fully-generic reflected codec/editor path alongside its
/// other numeric fields; this alias exists only for readable C++ compares.
enum class CameraProjection : std::uint32_t { Perspective = 0U,
                                              Orthographic = 1U };

/// Number of CameraProjection values; a stored projection at or past it is
/// one this build does not know, and every ingress refuses it.
inline constexpr std::uint32_t kCameraProjectionCount = 2U;

/// True when `projection` names a CameraProjection this build knows.
[[nodiscard]] constexpr bool
camera_projection_known(std::uint32_t projection) noexcept {
  return projection < kCameraProjectionCount;
}

/// The one test for an orthographic projection, shared by the camera
/// manager, the renderer and the scripting bridge so they cannot disagree.
[[nodiscard]] constexpr bool
projection_is_orthographic(std::uint32_t projection) noexcept {
  return projection ==
         static_cast<std::uint32_t>(CameraProjection::Orthographic);
}

/// First-class authored camera. Pose is never stored here: it
/// comes from the entity's world transform (looks along the rotated -Z
/// axis, up is the rotated +Y axis, matching SceneCaptureComponent's
/// convention) so authoring a camera never duplicates Transform state.
/// update_persistent_cameras publishes the derived pose into the owning
/// World's CameraManager priority stack every frame, so an authored camera
/// participates in the same priority/blend/shake model Lua-pushed and
/// spring-arm cameras already use -- it is not a second camera stack.
/// `projection` selects the render path's real projection: the
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

/// Character controller (Unity's CharacterController, Godot's
/// CharacterBody3D): its entity's own Capsule Collider, moved by a script's
/// displacement through runtime::move_character, which slides it along what
/// it meets, climbs steps and keeps it off steep slopes. It is not a
/// rigid body: gravity, jumping and speed are the script's. Only the three
/// settings are authored; the rest is what the last move found, never
/// serialized.
struct CharacterControllerComponent final {
  /// The steepest slope walked on, in degrees, from 0 to 89.
  float slopeLimit = 45.0F;
  /// The highest step climbed without jumping, in metres, from 0 to 10.
  float stepOffset = 0.3F;
  /// The gap kept between the capsule and what it touches, in metres, from
  /// 0.001 to 1.
  float skinWidth = 0.02F;
  /// Whether the last move ended on walkable ground.
  bool grounded = false;
  /// What the last move touched: physics::kCharacterCollided* bits.
  std::uint32_t collisionFlags = 0U;
};

/// True when every setting is finite and in its range.
inline bool
character_controller_is_valid(const CharacterControllerComponent &c) noexcept {
  return (c.slopeLimit >= 0.0F) && (c.slopeLimit <= 89.0F) &&
         (c.stepOffset >= 0.0F) && (c.stepOffset <= 10.0F) &&
         (c.skinWidth >= 0.001F) && (c.skinWidth <= 1.0F);
}

/// Navigation agent (Unity's NavMeshAgent, Godot's NavigationAgent3D): its
/// entity walks a path on the scene's navigation meshes to a destination a
/// script sets, one fixed step at a time, through its Character Controller
/// when it has one. Only the movement settings are authored; the
/// destination, the path and the speed reached are runtime state the frame
/// pipeline holds (runtime/nav_agent.h), never serialized.
struct NavAgentComponent final {
  /// The top speed, in metres per second, from 0.01 to 100.
  float speed = 3.5F;
  /// How fast it speeds up and brakes, in metres per second squared, from
  /// 0.01 to 1000.
  float acceleration = 8.0F;
  /// How fast it turns to face where it walks, in degrees per second, from
  /// 0 to 3600; 0 leaves its facing alone.
  float angularSpeed = 360.0F;
  /// How close to the destination counts as arrived, in metres, from 0 to
  /// 10.
  float stoppingDistance = 0.1F;
  /// The height of the entity's origin above the surface it walks, in
  /// metres, from -10 to 10: half a capsule's height for a character whose
  /// origin is the capsule's centre.
  float baseOffset = 0.0F;
};

/// True when every setting is finite and in its range.
inline bool nav_agent_is_valid(const NavAgentComponent &c) noexcept {
  return (c.speed >= 0.01F) && (c.speed <= 100.0F) &&
         (c.acceleration >= 0.01F) && (c.acceleration <= 1000.0F) &&
         (c.angularSpeed >= 0.0F) && (c.angularSpeed <= 3600.0F) &&
         (c.stoppingDistance >= 0.0F) && (c.stoppingDistance <= 10.0F) &&
         (c.baseOffset >= -10.0F) && (c.baseOffset <= 10.0F);
}

} // namespace engine::math
