// Declares the plain-data scene component types the runtime World stores
// and the scripting bridge carries: names, lights, scripts, meshes,
// cameras and spring arms. They live below both modules, like the
// transform and physics components, so scripting binds them without a
// runtime header and the World re-exports them unchanged.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/asset_identity.h"
#include "engine/math/vec3.h"

namespace engine::math {

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

} // namespace engine::math
