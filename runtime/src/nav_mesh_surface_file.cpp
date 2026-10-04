// Implements a NavMeshSurface's .navmesh write and its staleness check:
// both bake over the World, encode the mesh, and either write the bytes
// with an asset identity or compare them with the committed file.

#include "engine/runtime/nav_mesh_surface_file.h"

#include <cstring>
#include <memory>

#include "engine/content/asset_type_table.h"
#include "engine/core/vfs.h"
#include "engine/navigation/nav_mesh.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/navigation_bake.h"

namespace engine::runtime {

namespace {

bool is_nav_mesh_path(const char *path) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    return false;
  }
  const content::AssetClassification kind = content::classify_asset_path(path);
  return (kind.tag == content::AssetTypeTag::NavMesh) && kind.source;
}

/// Bakes the surface and encodes it. False for a failed bake, an empty
/// mesh (*empty set) or a failed encoding (*encodeFailed set).
bool bake_and_encode(const World &world, Entity entity,
                     std::unique_ptr<std::uint8_t[]> *bytes, std::size_t *size,
                     std::size_t *polygons, bool *empty,
                     bool *encodeFailed) noexcept {
  navigation::NavMesh mesh{};
  if (!bake_nav_mesh_surface(world, entity, &mesh)) {
    return false;
  }
  if (mesh.empty()) {
    *empty = true;
    return false;
  }
  if (!navigation::write_nav_mesh(mesh, bytes, size)) {
    *encodeFailed = true;
    return false;
  }
  *polygons = mesh.rect_count();
  return true;
}

} // namespace

NavMeshWriteReport write_nav_mesh_surface_file(const World &world,
                                               Entity entity,
                                               const char *vfsPath) noexcept {
  NavMeshWriteReport report{};
  if (!is_nav_mesh_path(vfsPath)) {
    report.result = NavMeshWriteResult::BadPath;
    return report;
  }
  std::unique_ptr<std::uint8_t[]> bytes{};
  std::size_t size = 0U;
  bool empty = false;
  bool encodeFailed = false;
  if (!bake_and_encode(world, entity, &bytes, &size, &report.polygons, &empty,
                       &encodeFailed)) {
    report.result = empty          ? NavMeshWriteResult::Empty
                    : encodeFailed ? NavMeshWriteResult::EncodeFailed
                                   : NavMeshWriteResult::BakeFailed;
    return report;
  }
  if (!core::vfs_write_binary(vfsPath, bytes.get(), size)) {
    report.result = NavMeshWriteResult::WriteFailed;
    return report;
  }
  // The file is an asset from now on: a sidecar gives it the identity the
  // catalog lists it under. A rebake keeps the identity it already has.
  char osPath[1024] = {};
  report.identified =
      core::vfs_resolve_os_path(vfsPath, osPath, sizeof(osPath)) &&
      (editor_establish_asset_identity(osPath) !=
       EditorIdentityResult::WriteFailed);
  report.result = NavMeshWriteResult::Written;
  return report;
}

NavMeshFileState check_nav_mesh_surface_file(const World &world, Entity entity,
                                             const char *vfsPath) noexcept {
  if (!is_nav_mesh_path(vfsPath)) {
    return NavMeshFileState::BadPath;
  }
  if (!core::vfs_file_exists(vfsPath)) {
    return NavMeshFileState::Missing;
  }
  std::unique_ptr<std::uint8_t[]> bytes{};
  std::size_t size = 0U;
  std::size_t polygons = 0U;
  bool empty = false;
  bool encodeFailed = false;
  if (!bake_and_encode(world, entity, &bytes, &size, &polygons, &empty,
                       &encodeFailed)) {
    return NavMeshFileState::BakeFailed;
  }
  void *data = nullptr;
  std::size_t fileSize = 0U;
  if (!core::vfs_read_binary_bounded(vfsPath, kMaxNavMeshFileBytes, &data,
                                     &fileSize)
           .succeeded()) {
    return NavMeshFileState::Unreadable;
  }
  const bool same =
      (fileSize == size) && (std::memcmp(data, bytes.get(), size) == 0);
  core::vfs_free(data);
  return same ? NavMeshFileState::Current : NavMeshFileState::Stale;
}

} // namespace engine::runtime
