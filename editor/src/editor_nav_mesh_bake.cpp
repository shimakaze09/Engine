// Implements the editor's NavMeshSurface Bake: the bake over the editing
// World, the .navmesh write, the asset identity and the reload request.

#include "editor_nav_mesh_bake.h"

#include "editor_scene_document.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "engine/content/asset_type_table.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/navigation_bake.h"
#include "engine/runtime/scene_navigation.h"

namespace engine::editor {

namespace {

/// The file name of `path` without its directory or its last suffix.
void file_stem(const char *path, char *out, std::size_t size) noexcept {
  const char *name = path;
  for (const char *c = path; *c != '\0'; ++c) {
    if ((*c == '/') || (*c == '\\')) {
      name = c + 1;
    }
  }
  const char *dot = std::strrchr(name, '.');
  const std::size_t length =
      (dot != nullptr) ? static_cast<std::size_t>(dot - name)
                       : std::strlen(name);
  const std::size_t copied = (length < size - 1U) ? length : (size - 1U);
  std::memcpy(out, name, copied);
  out[copied] = '\0';
}

} // namespace

bool choose_nav_mesh_path(const char *sceneDocumentPath, char *out,
                          std::size_t size) noexcept {
  if ((out == nullptr) || (size == 0U)) {
    return false;
  }
  char stem[96] = {};
  if ((sceneDocumentPath != nullptr) && (sceneDocumentPath[0] != '\0')) {
    file_stem(sceneDocumentPath, stem, sizeof(stem));
  }
  if (stem[0] == '\0') {
    std::snprintf(stem, sizeof(stem), "NavMesh");
  }
  const char *mount = active_config().assetMount;
  for (int attempt = 1; attempt < 1000; ++attempt) {
    const int written =
        (attempt == 1)
            ? std::snprintf(out, size, "%s/%s.navmesh", mount, stem)
            : std::snprintf(out, size, "%s/%s_%d.navmesh", mount, stem,
                            attempt);
    if ((written <= 0) || (static_cast<std::size_t>(written) >= size)) {
      out[0] = '\0';
      return false;
    }
    if (!core::vfs_file_exists(out)) {
      return true;
    }
  }
  out[0] = '\0';
  return false;
}

NavMeshBakeReport bake_nav_mesh_surface_file(const runtime::World &world,
                                             runtime::Entity entity,
                                             char *pathBuffer,
                                             std::size_t pathSize) noexcept {
  NavMeshBakeReport report{};
  char path[runtime::NavMeshSurfaceComponent::kMaxPathLength + 1U] = {};
  // The chosen path is written back whole, so the buffer must hold any
  // path a surface can carry.
  if ((pathBuffer == nullptr) || (pathSize < sizeof(path))) {
    std::snprintf(report.message, sizeof(report.message),
                  "no path buffer to bake to");
    return report;
  }
  if (pathBuffer[0] != '\0') {
    if (std::strlen(pathBuffer) >= sizeof(path)) {
      std::snprintf(report.message, sizeof(report.message),
                    "the path is too long");
      return report;
    }
    std::memcpy(path, pathBuffer, std::strlen(pathBuffer) + 1U);
  } else if (!choose_nav_mesh_path(scene_document_path(), path,
                                   sizeof(path))) {
    std::snprintf(report.message, sizeof(report.message),
                  "no free .navmesh name fits; type a path");
    return report;
  }
  const content::AssetClassification kind =
      content::classify_asset_path(path);
  if ((kind.tag != content::AssetTypeTag::NavMesh) || !kind.source) {
    std::snprintf(report.message, sizeof(report.message),
                  "the path must end in .navmesh");
    return report;
  }

  navigation::NavMesh mesh{};
  if (!runtime::bake_nav_mesh_surface(world, entity, &mesh)) {
    std::snprintf(report.message, sizeof(report.message),
                  "the bake failed; see the log");
    return report;
  }
  if (mesh.empty()) {
    std::snprintf(report.message, sizeof(report.message),
                  "nothing walkable inside the volume; nothing was written");
    return report;
  }
  std::unique_ptr<std::uint8_t[]> bytes{};
  std::size_t size = 0U;
  if (!navigation::write_nav_mesh(mesh, &bytes, &size) ||
      !core::vfs_write_binary(path, bytes.get(), size)) {
    std::snprintf(report.message, sizeof(report.message),
                  "%s could not be written; the previous file is kept",
                  path);
    return report;
  }

  // The file is an asset from now on: a sidecar gives it the identity the
  // catalog lists it under. A rebake keeps the identity it already has.
  char osPath[1024] = {};
  const bool identified =
      core::vfs_resolve_os_path(path, osPath, sizeof(osPath)) &&
      (runtime::editor_establish_asset_identity(osPath) !=
       runtime::EditorIdentityResult::WriteFailed);
  runtime::request_scene_navigation_reload();
  std::memcpy(pathBuffer, path, std::strlen(path) + 1U);
  report.written = true;
  std::snprintf(report.message, sizeof(report.message),
                "baked %zu polygons into %s%s", mesh.rect_count(), path,
                identified ? "" : " (its sidecar could not be written)");
  return report;
}

} // namespace engine::editor
