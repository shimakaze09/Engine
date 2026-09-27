// Declares Frame Selected: moving the Scene camera's orbit so the
// selection fills the view, as F does in Unity, Unreal and Godot. The
// selection's extent is the union, over each selected entity's subtree,
// of what is drawn and what collides: mesh bounds under the entity's
// transform, collider bounds, and the position of anything with neither.
// The camera keeps its angle and fits the union's bounding sphere.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

namespace engine::editor {

struct EditorCamera;

/// Object-space bounds (centre, half extents) of mesh asset
/// `meshAssetId`; false while it is not loaded.
using MeshBoundsFn = bool (*)(std::uint64_t meshAssetId, math::Vec3 *center,
                              math::Vec3 *halfExtents) noexcept;

/// A sphere around what is framed.
struct FramingSphere final {
  math::Vec3 center{};
  float radius = 0.0F;
};

/// The radius a bare position frames at: a metre across.
inline constexpr float kFramePointRadius = 0.5F;

/// The bounding sphere of the union of `entities`' subtrees:
/// - a mesh by its bounds (from `meshBounds`) under the entity's world
///   transform; a mesh not loaded yet counts as its position;
/// - a collider by its world bounds;
/// - anything else by its world position.
/// The sphere is the union box's circumscribed sphere, at least
/// kFramePointRadius. False, `out` untouched, when no listed entity is
/// alive with a transform.
bool selection_framing_sphere(runtime::World &world,
                              const runtime::Entity *entities,
                              std::size_t count, MeshBoundsFn meshBounds,
                              FramingSphere *out) noexcept;

/// The orbit distance at which a sphere of `radius` fits the view: the
/// radius over the sine of the narrower half field of view, with a tenth
/// to spare, clamped to the editor camera's range.
float framing_distance(float radius, float verticalFovRadians,
                       float aspect) noexcept;

/// Frames the selection in the Scene view: the orbit target moves to the
/// selection's centre and the distance fits it, angle unchanged. False,
/// camera unchanged, when nothing selected can be framed.
bool frame_selection() noexcept;

} // namespace engine::editor
