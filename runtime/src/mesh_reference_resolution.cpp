// Implements the mesh reference resolution pass: for every mesh id the
// World references and the asset database holds Unloaded, the catalog's
// path is resolved through the mount and handed to the streaming queue at
// normal priority. Enqueue failure puts the record back to Unloaded so the
// next frame retries instead of leaving a Loading record nobody serves.

#include "mesh_reference_resolution.h"

#include <cstdio>
#include <cstring>

#include "engine/content/asset_streaming.h"
#include "engine/core/diagnostic.h"
#include "engine/core/vfs.h"
#include "engine/runtime/world.h"

namespace engine {
namespace {

constexpr const char *kChannel = "assets";

/// True when `id` was already reported for this content.
bool already_reported(const UnresolvedMeshReports &reports,
                      renderer::AssetId id) noexcept {
  for (std::size_t i = 0U; i < reports.count; ++i) {
    if (reports.ids[i] == id) {
      return true;
    }
  }
  return false;
}

/// Logs one record for an id the catalog cannot place, the first time the
/// id is met for this content; the table full is itself said once.
void report_unresolved(UnresolvedMeshReports *reports, renderer::AssetId id,
                       runtime::PersistentId entityId, const char *path,
                       const char *reason) noexcept {
  if (already_reported(*reports, id)) {
    return;
  }
  if (reports->count >= UnresolvedMeshReports::kMaxReported) {
    if (!reports->overflowReported) {
      reports->overflowReported = true;
      core::log_message(core::LogLevel::Warning, kChannel,
                        "mesh references: more ids than the report table "
                        "holds are unplaceable; later ones are not listed");
    }
    return;
  }
  reports->ids[reports->count] = id;
  ++reports->count;

  char message[192] = {};
  std::snprintf(message, sizeof(message),
                "mesh reference 0x%016llX %s; the entity draws nothing",
                static_cast<unsigned long long>(id), reason);
  core::Diagnostic record =
      core::make_diagnostic(core::LogLevel::Warning, kChannel, message);
  record.assetId = id;
  record.entityPersistentId = entityId;
  if (path != nullptr) {
    core::diagnostic_set_path(&record, path);
  }
  core::log_diagnostic(record);
}

/// One reference: places it, requests it, or reports it.
void resolve_reference(runtime::EngineAssetDatabaseService *service,
                       UnresolvedMeshReports *reports, renderer::AssetId id,
                       runtime::PersistentId entityId,
                       MeshResolutionPass *pass) noexcept {
  if (id == renderer::kInvalidAssetId) {
    return;
  }
  renderer::AssetDatabase *database = service->database;
  if (renderer::mesh_asset_state(database, id) !=
      renderer::AssetState::Unloaded) {
    return;
  }

  const renderer::AssetMetadata *metadata =
      renderer::find_asset_metadata(database, id);
  if ((metadata == nullptr) ||
      (metadata->typeTag != renderer::AssetTypeTag::Mesh) ||
      (metadata->filePath[0] == '\0')) {
    ++pass->unresolved;
    report_unresolved(reports, id, entityId, nullptr,
                      "names no catalogued mesh");
    return;
  }

  char osPath[512] = {};
  if (!core::vfs_resolve_os_path(metadata->filePath.data(), osPath,
                                 sizeof(osPath))) {
    ++pass->unresolved;
    report_unresolved(reports, id, entityId, metadata->filePath.data(),
                      "is catalogued under a prefix that is not mounted");
    return;
  }

  if (!renderer::request_mesh_asset_streaming_load(database, id, osPath)) {
    ++pass->unresolved;
    report_unresolved(reports, id, entityId, metadata->filePath.data(),
                      "cannot take a mesh record (the table is full)");
    return;
  }
  if (service->streamingQueue == nullptr) {
    // No queue means no worker: the record stays Loading for a caller
    // that serves it synchronously, as the bootstrap loader does.
    ++pass->requested;
    return;
  }
  const content::LoadHandle handle =
      content::load_asset_async(service->streamingQueue, id, osPath,
                                content::LoadPriority::Normal);
  if (!handle.valid()) {
    static_cast<void>(renderer::set_mesh_asset_state(
        database, id, renderer::AssetState::Unloaded,
        renderer::kInvalidMeshHandle));
    return;
  }
  ++pass->requested;
}

} // namespace

bool note_mesh_asset_path(renderer::AssetDatabase *database,
                          renderer::AssetId id,
                          const char *virtualPath) noexcept {
  if ((database == nullptr) || (id == renderer::kInvalidAssetId) ||
      (virtualPath == nullptr) || (virtualPath[0] == '\0')) {
    return false;
  }
  if (renderer::find_asset_metadata(database, id) != nullptr) {
    return true;
  }
  renderer::AssetMetadata metadata{};
  if (std::strlen(virtualPath) >= metadata.filePath.size()) {
    // An identity that does not fit whole would name a different asset.
    return false;
  }
  metadata.assetId = id;
  metadata.typeTag = renderer::AssetTypeTag::Mesh;
  renderer::write_metadata_path(&metadata.filePath, virtualPath);
  return renderer::register_asset_metadata(database, metadata);
}

MeshResolutionPass request_referenced_mesh_assets(
    const runtime::World &world, runtime::EngineAssetDatabaseService *service,
    UnresolvedMeshReports *reports) noexcept {
  MeshResolutionPass pass{};
  if ((service == nullptr) || (service->database == nullptr) ||
      (reports == nullptr)) {
    return pass;
  }
  const std::uint32_t epoch = world.content_epoch();
  if (reports->contentEpoch != epoch) {
    *reports = UnresolvedMeshReports{};
    reports->contentEpoch = epoch;
  }

  world.for_each<runtime::MeshComponent>(
      [&](runtime::Entity entity,
          const runtime::MeshComponent &mesh) noexcept {
        resolve_reference(service, reports, mesh.meshAssetId,
                          world.persistent_id(entity), &pass);
      });

  const std::size_t patchCount = world.foliage_patch_count();
  for (std::size_t i = 0U; i < patchCount; ++i) {
    const runtime::FoliagePatchComponent *patch = world.foliage_patch_at(i);
    if (patch == nullptr) {
      continue;
    }
    const runtime::PersistentId entityId =
        world.persistent_id(world.foliage_patch_entity_at(i));
    for (std::size_t lod = 0U; lod < runtime::FoliagePatchComponent::kMaxLods;
         ++lod) {
      resolve_reference(service, reports, patch->meshAssetIds[lod], entityId,
                        &pass);
    }
  }
  return pass;
}

} // namespace engine
