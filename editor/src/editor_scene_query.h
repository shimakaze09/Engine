// Declares the Scene view's queries: what a click or a marquee picks.
// They run on the CPU against what the world already knows, as Godot's
// editor picks, rather than through an ID buffer as Unity's and Unreal's
// do: RenderDevice has no readback, the null device could not test an ID
// pass, and a marquee needs world bounds anyway. A mesh is hit by its
// bounds under its entity's transform, a collider by its exact shape.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/frustum.h"
#include "engine/math/mat4.h"
#include "engine/math/ray.h"
#include "engine/runtime/world.h"

namespace engine::editor {

/// Object-space bounds (centre, half extents) of mesh asset
/// `meshAssetId`; false while it is not loaded.
/// runtime::editor_mesh_local_bounds is the production source.
using MeshBoundsFn = bool (*)(std::uint64_t meshAssetId, math::Vec3 *center,
                              math::Vec3 *halfExtents) noexcept;

/// The world-axis box (centre, half extents) of `entity`'s mesh bounds
/// under its world transform; false when it has no mesh, no world
/// transform, or its mesh is not loaded.
bool entity_mesh_world_box(const runtime::World &world, runtime::Entity entity,
                           MeshBoundsFn meshBounds, math::Vec3 *outCenter,
                           math::Vec3 *outHalf) noexcept;
/// The world-axis box of `entity`'s collider; false without one.
bool entity_collider_world_box(const runtime::World &world,
                               runtime::Entity entity, math::Vec3 *outCenter,
                               math::Vec3 *outHalf) noexcept;

/// One entity under a pick ray, at its nearest hit.
struct PickHit final {
  runtime::Entity entity = runtime::kInvalidEntity;
  float distance = 0.0F;
};

/// The entities `ray` (unit direction) hits within `maxDistance`, nearest
/// first, one hit per entity, at most `capacity`:
/// - a mesh by its bounds (from `meshBounds`) under the entity's world
///   transform, skipped when the box contains the ray's origin, so a room
///   or sky dome around the camera does not swallow every click;
/// - a collider by its exact shape, skipped likewise when the ray starts
///   inside it.
/// An entity with neither is not pickable in the Scene view.
std::size_t scene_pick_hits(runtime::World &world, const math::Ray &ray,
                            float maxDistance, MeshBoundsFn meshBounds,
                            PickHit *out, std::size_t capacity) noexcept;

/// Which of `hits` a click picks. A click within a few pixels of the last
/// one, while `current` is among the hits, picks the next hit behind it
/// (wrapping), so repeated clicks walk through overlapping objects in
/// depth order, as in Unity. Otherwise the nearest. kInvalidEntity when
/// there are no hits.
runtime::Entity choose_pick(const PickHit *hits, std::size_t count,
                            runtime::Entity current,
                            bool sameSpotAsLastClick) noexcept;

/// What a Scene view icon stands for.
enum class SceneIconKind : std::uint8_t { Light, Camera };

/// A light or camera shown as a screen-space icon, where it projects.
struct SceneIcon final {
  runtime::Entity entity = runtime::kInvalidEntity;
  SceneIconKind kind = SceneIconKind::Light;
  math::Vec3 ndc{}; // x, y on screen in [-1, 1]; z orders by depth
};

/// An icon's extent at scale 1, as Unity's default gizmo icons.
inline constexpr float kSceneIconBasePixels = 32.0F;
/// The icon-size preference's range, as Unity's 3D Icons slider.
inline constexpr float kMinSceneIconScale = 0.5F;
inline constexpr float kMaxSceneIconScale = 3.0F;

/// Pixel sizes of the Scene view's light and camera icons.
struct SceneIconMetrics final {
  /// Half the icon's extent. An icon is drawn within it and picked within
  /// it, so what is visible is exactly what is clickable.
  float radius = 0.0F;
  /// The ring drawn around a selected icon.
  float selectionRadius = 0.0F;
  /// Outline, ray and ring width.
  float stroke = 0.0F;
};

/// Icon metrics at `uiScale` (the editor's UI scale) times `iconScale`
/// (the icon-size preference, clamped to its range). A scale that is not
/// a positive finite number counts as 1.
SceneIconMetrics scene_icon_metrics(float uiScale, float iconScale) noexcept;

/// True when `entity` is shown as an icon: it has a light component of
/// any kind, or a camera.
bool entity_has_icon(const runtime::World &world,
                     runtime::Entity entity) noexcept;

/// The icons of every light and camera in front of the eye and within the
/// view under `viewProjection`, in entity order, at most `capacity`. A
/// light sits where runtime::light_world_pose puts it, a camera at its
/// entity's world position.
std::size_t scene_icons(const runtime::World &world,
                        const math::Mat4 &viewProjection, SceneIcon *out,
                        std::size_t capacity) noexcept;

/// The icon under the cursor at (ndcX, ndcY): the nearest on screen
/// within the ellipse of NDC radii (radiusX, radiusY), which a caller
/// sizes to a circle of pixels, the nearer in depth on a tie.
/// kInvalidEntity when none is that close. Icons are picked before
/// geometry, as Unity's gizmo icons are.
runtime::Entity pick_icon(const SceneIcon *icons, std::size_t count, float ndcX,
                          float ndcY, float radiusX, float radiusY) noexcept;

/// Called once per entity a marquee takes.
using BoxSelectVisit = void (*)(void *context, runtime::Entity entity) noexcept;

/// Visits, in entity order, every entity within `frustum` (a marquee's
/// sub-rectangle frustum): a mesh by its world bounds, a collider by its
/// world bounds, tested conservatively, so a box outside only past a
/// frustum corner may be taken, and a light or camera by its icon's
/// position. Returns how many were visited.
std::size_t scene_box_select(runtime::World &world,
                             const math::Frustum &frustum,
                             MeshBoundsFn meshBounds, BoxSelectVisit visit,
                             void *context) noexcept;

} // namespace engine::editor
