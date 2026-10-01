// Implements the open project's collision layers and their installation on
// a World. One process-wide copy, written on the main thread at bootstrap
// and by the editor's Project Settings, read by Lua and the Inspector.

#include "engine/runtime/collision_layers.h"

#include "engine/core/logging.h"
#include "engine/physics/physics.h"
#include "engine/runtime/world.h"

namespace engine::runtime {
namespace {

content::ProjectCollisionLayers g_layers{};

} // namespace

void set_project_collision_layers(
    const content::ProjectCollisionLayers &layers) noexcept {
  g_layers = layers;
}

const content::ProjectCollisionLayers &project_collision_layers() noexcept {
  return g_layers;
}

physics::CollisionLayerMatrix
collision_matrix_from(const content::ProjectCollisionLayers &layers) noexcept {
  static_assert(content::kMaxCollisionLayers == physics::kCollisionLayerCount,
                "the document and physics name the same 32 layer bits");
  physics::CollisionLayerMatrix matrix{};
  for (std::size_t i = 0U; i < physics::kCollisionLayerCount; ++i) {
    matrix.rows[i] = layers.collides[i];
  }
  return matrix;
}

bool apply_project_collision_layers(World &world) noexcept {
  if (world.current_phase() == WorldPhase::Simulation) {
    core::log_message(core::LogLevel::Error, "physics",
                      "the layer collision matrix cannot change while the "
                      "world is simulating");
    return false;
  }
  physics::set_collision_matrix(world, collision_matrix_from(g_layers));
  return true;
}

} // namespace engine::runtime
