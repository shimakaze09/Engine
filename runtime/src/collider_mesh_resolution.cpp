// Implements the pass that gives TriMesh colliders their meshes: resolve the
// collider's reference through the catalog, build the collision mesh from
// the cooked file once per asset generation, share it, and report what
// cannot be built once per World content.

#include "collider_mesh_resolution.h"

#include <cstdio>
#include <memory>
#include <new>

#include "engine/content/asset_identity.h"
#include "engine/content/asset_staleness.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace engine::runtime {
namespace {

constexpr const char *kLogChannel = "physics";

bool id_failed(const ColliderMeshCache &cache, content::AssetId id) noexcept {
  for (std::size_t i = 0U; i < cache.failedIdCount; ++i) {
    if (cache.failedIds[i] == id) {
      return true;
    }
  }
  return false;
}

bool ref_failed(const ColliderMeshCache &cache,
                const core::AssetRef &ref) noexcept {
  for (std::size_t i = 0U; i < cache.failedRefCount; ++i) {
    if (cache.failedRefs[i] == ref) {
      return true;
    }
  }
  return false;
}

/// Logs once why the mesh `what` names cannot be a collider's, and
/// remembers it for this content.
void report(const char *what, const char *reason) noexcept {
  char message[512] = {};
  std::snprintf(message, sizeof(message),
                "TriMesh collider mesh %s: %s; colliders that use it collide "
                "with nothing until it is fixed",
                what, reason);
  core::log_message(core::LogLevel::Error, kLogChannel, message);
}

void remember_failed_id(ColliderMeshCache &cache,
                        content::AssetId id) noexcept {
  if (cache.failedIdCount < cache.failedIds.size()) {
    cache.failedIds[cache.failedIdCount++] = id;
  }
}

void remember_failed_ref(ColliderMeshCache &cache,
                         const core::AssetRef &ref) noexcept {
  if (cache.failedRefCount < cache.failedRefs.size()) {
    cache.failedRefs[cache.failedRefCount++] = ref;
  }
}

/// The cached mesh of `id` at `generation`, or nullptr.
const physics::TriMeshRef *cached(const ColliderMeshCache &cache,
                                  content::AssetId id,
                                  std::uint32_t generation) noexcept {
  for (std::size_t i = 0U; i < cache.count; ++i) {
    if ((cache.entries[i].id == id) &&
        (cache.entries[i].generation == generation)) {
      return &cache.entries[i].mesh;
    }
  }
  return nullptr;
}

const physics::TriMeshRef *store(ColliderMeshCache &cache, content::AssetId id,
                                 std::uint32_t generation,
                                 const physics::TriMeshRef &mesh) noexcept {
  // A newer generation replaces the asset's older mesh in place.
  std::size_t slot = cache.count;
  for (std::size_t i = 0U; i < cache.count; ++i) {
    if (cache.entries[i].id == id) {
      slot = i;
      break;
    }
  }
  if (slot == cache.count) {
    if (cache.count < cache.entries.size()) {
      ++cache.count;
    } else {
      slot = cache.nextReplaced;
      cache.nextReplaced = (cache.nextReplaced + 1U) % cache.entries.size();
    }
  }
  cache.entries[slot].id = id;
  cache.entries[slot].generation = generation;
  cache.entries[slot].mesh = mesh;
  return &cache.entries[slot].mesh;
}

} // namespace

bool build_collision_mesh_from_file(const char *osPath,
                                    physics::TriMeshRef *outMesh,
                                    const char **outReason) noexcept {
  const char *unused = nullptr;
  const char **reason = (outReason != nullptr) ? outReason : &unused;
  if ((osPath == nullptr) || (outMesh == nullptr)) {
    *reason = "no file";
    return false;
  }
  // The same gate a mesh load passes: a torn cook generation is refused,
  // and a cook older than its source is said so.
  if (!content::cooked_asset_generation_ok(osPath)) {
    *reason = "its cooked files are from different cooks; recook it";
    return false;
  }
  content::warn_if_cooked_asset_stale(osPath);
  renderer::CpuMeshData data{};
  if (!renderer::load_mesh_data_from_file(osPath, &data)) {
    *reason = "its cooked mesh will not load";
    return false;
  }
  const std::size_t vertexCount = data.vertexCount;
  const std::size_t stride = data.strideFloats;
  if ((vertexCount == 0U) || (stride < 3U) ||
      (data.vertices.size() < (vertexCount * stride))) {
    *reason = "its cooked mesh has no vertices";
    return false;
  }
  std::unique_ptr<math::Vec3[]> positions(new (std::nothrow)
                                              math::Vec3[vertexCount]);
  if (positions == nullptr) {
    *reason = "out of memory";
    return false;
  }
  const float *source = data.vertices.data();
  for (std::size_t i = 0U; i < vertexCount; ++i) {
    positions[i] =
        math::Vec3(source[(i * stride) + 0U], source[(i * stride) + 1U],
                   source[(i * stride) + 2U]);
  }
  const physics::TriMeshBuildResult result =
      physics::build_tri_mesh(positions.get(), vertexCount, data.indices.data(),
                              data.indices.size(), outMesh);
  if (result != physics::TriMeshBuildResult::Ok) {
    *reason = physics::tri_mesh_build_result_text(result);
    return false;
  }
  return true;
}

ColliderMeshPass install_collider_meshes(World &world,
                                         const content::AssetCatalog *catalog,
                                         ColliderMeshCache *cache) noexcept {
  ColliderMeshPass pass{};
  if ((catalog == nullptr) || (cache == nullptr)) {
    return pass;
  }
  // New content may fix what failed before: a reloaded scene, a reimported
  // mesh. Built meshes stay; their generation says whether they still hold.
  if (!cache->contentSeen || (cache->contentEpoch != world.content_epoch())) {
    cache->contentSeen = true;
    cache->contentEpoch = world.content_epoch();
    cache->failedIdCount = 0U;
    cache->failedRefCount = 0U;
  }
  const std::size_t count = world.collider_count();
  const Entity *entities = nullptr;
  const Collider *colliders = nullptr;
  if ((count == 0U) ||
      !world.get_collider_range(0U, count, &entities, &colliders)) {
    return pass;
  }
  for (std::size_t i = 0U; i < count; ++i) {
    const Collider &collider = colliders[i];
    if ((collider.shape != ColliderShape::TriMesh) ||
        !core::asset_ref_is_valid(collider.meshRef)) {
      continue;
    }
    const content::AssetMetadata *record =
        content::find_asset_metadata_by_ref(catalog, collider.meshRef);
    if ((record == nullptr) ||
        (record->typeTag != content::AssetTypeTag::Mesh) ||
        (record->filePath[0] == '\0')) {
      if (!ref_failed(*cache, collider.meshRef)) {
        char text[content::kAssetRefTextLength + 1U] = {};
        static_cast<void>(
            content::format_asset_ref(collider.meshRef, text, sizeof(text)));
        report(text, "names no catalogued mesh");
        remember_failed_ref(*cache, collider.meshRef);
        ++pass.failed;
      }
      continue;
    }
    const content::AssetId id = record->assetId;
    const std::uint32_t generation =
        content::asset_reload_generation(catalog, id);
    const physics::TriMeshRef *mesh = cached(*cache, id, generation);
    if (mesh == nullptr) {
      if (id_failed(*cache, id)) {
        continue;
      }
      char osPath[512] = {};
      physics::TriMeshRef built{};
      const char *reason = "is catalogued under a prefix that is not mounted";
      if (!core::vfs_resolve_os_path(record->filePath.data(), osPath,
                                     sizeof(osPath)) ||
          !build_collision_mesh_from_file(osPath, &built, &reason)) {
        report(record->filePath.data(), reason);
        remember_failed_id(*cache, id);
        ++pass.failed;
        continue;
      }
      mesh = store(*cache, id, generation, built);
      ++pass.built;
    }
    if (get_tri_mesh_data(world, entities[i]) == mesh->get()) {
      continue;
    }
    if (set_tri_mesh_data(world, entities[i], *mesh)) {
      ++pass.installed;
    }
  }
  return pass;
}

} // namespace engine::runtime
