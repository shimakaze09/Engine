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

/// Called once per entity a marquee takes.
using BoxSelectVisit = void (*)(void *context, runtime::Entity entity) noexcept;

/// Visits, in entity order, every entity within `frustum` (a marquee's
/// sub-rectangle frustum): a mesh by its world bounds, a collider by its
/// world bounds, tested conservatively, so a box outside only past a
/// frustum corner may be taken. Returns how many were visited.
std::size_t scene_box_select(runtime::World &world,
                             const math::Frustum &frustum,
                             MeshBoundsFn meshBounds, BoxSelectVisit visit,
                             void *context) noexcept;

} // namespace engine::editor
