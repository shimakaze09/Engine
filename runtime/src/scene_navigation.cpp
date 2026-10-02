// Implements the loaded scene's navigation meshes: each NavMeshSurface's
// .navmesh file read through the VFS, again whenever the surfaces, their
// paths or the World's contents change, or a bake asks for it.

#include "engine/runtime/scene_navigation.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

#include "engine/core/diagnostic.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

namespace {

constexpr const char *kLogChannel = "navigation";

/// The largest .navmesh file a scene reads: a full grid of the most
/// columns a bake samples, each with the most surfaces, is about 40 MiB,
/// so a file past this is not one the bake wrote.
constexpr std::uint64_t kMaxNavMeshFileBytes = 128ULL * 1024ULL * 1024ULL;

std::atomic<std::uint32_t> g_reloadGeneration{0U};

void log_surface_error(const char *path, const char *what) noexcept {
  core::log_path_diagnostic(core::LogLevel::Error, kLogChannel, path, what);
}

} // namespace

void request_scene_navigation_reload() noexcept {
  g_reloadGeneration.fetch_add(1U, std::memory_order_relaxed);
}

bool load_nav_mesh_file(const char *path, navigation::NavMesh *out) noexcept {
  if ((path == nullptr) || (path[0] == '\0') || (out == nullptr)) {
    return false;
  }
  void *data = nullptr;
  std::size_t size = 0U;
  const core::Status read =
      core::vfs_read_binary_bounded(path, kMaxNavMeshFileBytes, &data, &size);
  if (!read.succeeded()) {
    log_surface_error(path,
                      (read.kind == core::FailureKind::NotFound)
                          ? "the navigation surface's .navmesh file is "
                            "missing; bake the surface to write it"
                          : "the navigation surface's .navmesh file could "
                            "not be read");
    return false;
  }
  const bool decoded = navigation::read_nav_mesh(
      static_cast<const std::uint8_t *>(data), size, out);
  core::vfs_free(data);
  if (!decoded) {
    log_surface_error(path, "the navigation surface's .navmesh file is "
                            "damaged; bake the surface again");
  }
  return decoded;
}

void SceneNavigation::clear() noexcept {
  for (Slot &slot : m_slots) {
    slot = Slot{};
  }
  m_count = 0U;
  m_bound = false;
  m_overflowReported = false;
}

void SceneNavigation::update(const World &world) noexcept {
  const std::uint32_t generation =
      g_reloadGeneration.load(std::memory_order_relaxed);
  const std::size_t surfaces = world.nav_mesh_surface_count();
  const std::size_t tracked =
      (surfaces < kMaxSceneNavMeshes) ? surfaces : kMaxSceneNavMeshes;
  const bool reloadAll = !m_bound ||
                         (m_contentEpoch != world.content_epoch()) ||
                         (m_reloadGeneration != generation);

  if ((surfaces > kMaxSceneNavMeshes) && !m_overflowReported) {
    char message[160] = {};
    std::snprintf(message, sizeof(message),
                  "the scene has %zu navigation surfaces; the first %zu "
                  "have meshes and the rest none",
                  surfaces, kMaxSceneNavMeshes);
    core::log_message(core::LogLevel::Warning, kLogChannel, message);
    m_overflowReported = true;
  } else if (surfaces <= kMaxSceneNavMeshes) {
    m_overflowReported = false;
  }

  for (std::size_t i = 0U; i < tracked; ++i) {
    const NavMeshSurfaceComponent *surface = world.nav_mesh_surface_at(i);
    const Entity entity = world.nav_mesh_surface_entity_at(i);
    Slot &slot = m_slots[i];
    const bool same = !reloadAll && (i < m_count) && (slot.surface == entity) &&
                      (std::strcmp(slot.path, surface->navMeshPath) == 0);
    if (same) {
      continue;
    }
    slot = Slot{};
    slot.surface = entity;
    std::memcpy(slot.path, surface->navMeshPath, sizeof(slot.path));
    navigation::NavMesh mesh{};
    if ((slot.path[0] != '\0') && load_nav_mesh_file(slot.path, &mesh)) {
      slot.mesh = std::move(mesh);
      slot.loaded = true;
    }
  }
  for (std::size_t i = tracked; i < m_count; ++i) {
    m_slots[i] = Slot{};
  }
  m_count = tracked;
  m_contentEpoch = world.content_epoch();
  m_reloadGeneration = generation;
  m_bound = true;
}

Entity SceneNavigation::surface_at(std::size_t index) const noexcept {
  return (index < m_count) ? m_slots[index].surface : Entity{};
}

const navigation::NavMesh *
SceneNavigation::mesh_at(std::size_t index) const noexcept {
  return ((index < m_count) && m_slots[index].loaded) ? &m_slots[index].mesh
                                                      : nullptr;
}

const navigation::NavMesh *
SceneNavigation::mesh_for(Entity surface) const noexcept {
  for (std::size_t i = 0U; i < m_count; ++i) {
    if (m_slots[i].surface == surface) {
      return mesh_at(i);
    }
  }
  return nullptr;
}

} // namespace engine::runtime
