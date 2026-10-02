// Declares the open project's collision layers: the names Lua and the
// editor look layers up by, and the layer collision matrix the run installs
// on its World. Bootstrap sets them from the project document; the editor's
// Project Settings replaces them live after saving the document.

#pragma once

#include "engine/content/project_document.h"
#include "engine/physics/physics_context.h"

namespace engine::runtime {

class World;

/// Replaces the project's collision layers. Bootstrap calls it with the
/// opened project's (the defaults with no project open).
void set_project_collision_layers(
    const content::ProjectCollisionLayers &layers) noexcept;

/// The project's collision layers; the defaults until a project sets them.
const content::ProjectCollisionLayers &project_collision_layers() noexcept;

/// The physics matrix the layers describe.
physics::CollisionLayerMatrix
collision_matrix_from(const content::ProjectCollisionLayers &layers) noexcept;

/// Installs the project's matrix on `world`, waking its bodies when it
/// changes. Refused, with an Error, while the world is simulating, since
/// the parallel step reads it.
bool apply_project_collision_layers(World &world) noexcept;

} // namespace engine::runtime
