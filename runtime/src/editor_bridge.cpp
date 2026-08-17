// Implements editor bridge behavior for the Engine runtime world: bridge
// registration plus the editor-facing asset request entry point backed by
// the pipeline's published asset service.

#include "engine/runtime/editor_bridge.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_streaming.h"
#include "engine/renderer/material_loader.h"
#include "engine/renderer/material_writer.h"
#include "engine/runtime/service_registry.h"

namespace engine::runtime {

namespace {

const EditorBridge *g_editorBridge = nullptr;
EngineAssetDatabaseService *g_editorAssetService = nullptr;

} // namespace

/// Sets the requested value for editor bridge.
void set_editor_bridge(const EditorBridge *bridge) noexcept {
  g_editorBridge = bridge;
}

const EditorBridge *editor_bridge() noexcept {
  return g_editorBridge;
}

void set_editor_asset_service(EngineAssetDatabaseService *service) noexcept {
  g_editorAssetService = service;
}

std::uint64_t editor_request_mesh_asset(const char *virtualPath) noexcept {
  if ((virtualPath == nullptr) || (virtualPath[0] == '\0') ||
      (g_editorAssetService == nullptr) ||
      (g_editorAssetService->database == nullptr)) {
    return renderer::kInvalidAssetId;
  }

  const renderer::AssetId assetId =
      renderer::make_asset_id_from_path(virtualPath);
  if (assetId == renderer::kInvalidAssetId) {
    return renderer::kInvalidAssetId;
  }

  const renderer::AssetState state =
      renderer::mesh_asset_state(g_editorAssetService->database, assetId);
  if (state == renderer::AssetState::Ready) {
    return assetId;
  }

  char osPath[512] = {};
  if (!core::vfs_resolve_os_path(virtualPath, osPath, sizeof(osPath))) {
    core::log_message(core::LogLevel::Error, "editor",
                      "editor_request_mesh_asset: virtual path did not "
                      "resolve to an OS path");
    return renderer::kInvalidAssetId;
  }

  if (!renderer::request_mesh_asset_streaming_load(
          g_editorAssetService->database, assetId, osPath)) {
    core::log_message(core::LogLevel::Error, "editor",
                      "editor_request_mesh_asset: streaming request rejected");
    return renderer::kInvalidAssetId;
  }

  if (g_editorAssetService->streamingQueue != nullptr) {
    const renderer::LoadHandle handle = renderer::load_asset_async(
        g_editorAssetService->streamingQueue, assetId, osPath,
        renderer::LoadPriority::High);
    if (!handle.valid()) {
      static_cast<void>(renderer::set_mesh_asset_state(
          g_editorAssetService->database, assetId,
          renderer::AssetState::Failed, renderer::kInvalidMeshHandle));
      core::log_message(core::LogLevel::Error, "editor",
                        "editor_request_mesh_asset: async load enqueue failed");
      return renderer::kInvalidAssetId;
    }
  }

  return assetId;
}

namespace {

/// Case-insensitive substring test ("" needle always matches).
bool contains_ci(const char *haystack, const char *needle) noexcept {
  if ((haystack == nullptr) || (needle == nullptr)) {
    return false;
  }
  if (needle[0] == '\0') {
    return true;
  }
  const std::size_t haystackLen = std::strlen(haystack);
  const std::size_t needleLen = std::strlen(needle);
  if (needleLen > haystackLen) {
    return false;
  }
  for (std::size_t start = 0U; start <= (haystackLen - needleLen); ++start) {
    std::size_t i = 0U;
    for (; i < needleLen; ++i) {
      const unsigned char a =
          static_cast<unsigned char>(std::tolower(haystack[start + i]));
      const unsigned char b =
          static_cast<unsigned char>(std::tolower(needle[i]));
      if (a != b) {
        break;
      }
    }
    if (i == needleLen) {
      return true;
    }
  }
  return false;
}

} // namespace

std::size_t editor_query_assets(renderer::AssetTypeTag typeTag,
                                const char *query,
                                EditorAssetSearchResult *outResults,
                                std::size_t maxResults) noexcept {
  if ((outResults == nullptr) || (maxResults == 0U) ||
      (g_editorAssetService == nullptr) ||
      (g_editorAssetService->database == nullptr)) {
    return 0U;
  }

  constexpr std::size_t kScanCapacity = 512U;
  renderer::AssetId candidateIds[kScanCapacity];
  const std::size_t candidateCount = renderer::query_assets_by_type(
      g_editorAssetService->database, typeTag, candidateIds, kScanCapacity);

  const char *effectiveQuery = (query != nullptr) ? query : "";
  std::size_t written = 0U;
  for (std::size_t i = 0U; (i < candidateCount) && (written < maxResults);
      ++i) {
    const renderer::AssetMetadata *metadata = renderer::find_asset_metadata(
        g_editorAssetService->database, candidateIds[i]);
    if (metadata == nullptr) {
      continue;
    }
    if (!contains_ci(metadata->filePath.data(), effectiveQuery)) {
      continue;
    }
    EditorAssetSearchResult &result = outResults[written];
    result.assetId = candidateIds[i];
    std::snprintf(result.path, sizeof(result.path), "%s",
                  metadata->filePath.data());
    ++written;
  }
  return written;
}

bool editor_asset_display_path(std::uint64_t assetId, char *outPath,
                               std::size_t outPathSize) noexcept {
  if ((outPath == nullptr) || (outPathSize == 0U) ||
      (assetId == renderer::kInvalidAssetId) ||
      (g_editorAssetService == nullptr) ||
      (g_editorAssetService->database == nullptr)) {
    return false;
  }
  const renderer::AssetMetadata *metadata = renderer::find_asset_metadata(
      g_editorAssetService->database, assetId);
  if (metadata == nullptr) {
    return false;
  }
  std::snprintf(outPath, outPathSize, "%s", metadata->filePath.data());
  return true;
}

namespace {

/// Builds the state struct from an already-registered material id; false
/// (state left default) when the id is not (or no longer) Ready.
bool fill_material_state(renderer::AssetId materialId,
                         EditorMaterialState *outState) noexcept {
  const renderer::Material *params = renderer::find_material_params(
      g_editorAssetService->database, materialId);
  if (params == nullptr) {
    return false;
  }

  outState->found = true;
  outState->materialId = materialId;
  outState->params = *params;
  const renderer::MaterialTextureSlots *slots =
      renderer::find_material_texture_slots(g_editorAssetService->database,
                                            materialId);
  if (slots != nullptr) {
    outState->textureSlots = *slots;
  }
  outState->hasParent = renderer::find_material_parent_virtual_path(
      g_editorAssetService->database, materialId,
      outState->parentVirtualPath, sizeof(outState->parentVirtualPath));
  return true;
}

} // namespace

EditorMaterialState editor_load_material(const char *virtualPath) noexcept {
  EditorMaterialState state{};
  if ((virtualPath == nullptr) || (virtualPath[0] == '\0') ||
      (g_editorAssetService == nullptr) ||
      (g_editorAssetService->database == nullptr)) {
    return state;
  }

  const auto loadResult =
      renderer::load_material_asset(g_editorAssetService->database, virtualPath);
  if (!loadResult.has_value()) {
    return state;
  }

  static_cast<void>(fill_material_state(*loadResult, &state));
  return state;
}

bool editor_set_material_params(
    renderer::AssetId materialId, const renderer::Material &params,
    const renderer::MaterialTextureSlots &textureSlots) noexcept {
  if ((materialId == renderer::kInvalidAssetId) ||
      (g_editorAssetService == nullptr) ||
      (g_editorAssetService->database == nullptr)) {
    return false;
  }

  const renderer::AssetMetadata *metadata = renderer::find_asset_metadata(
      g_editorAssetService->database, materialId);
  const char *sourcePath =
      (metadata != nullptr) ? metadata->filePath.data() : nullptr;
  if (!renderer::register_material_asset(g_editorAssetService->database,
                                         materialId, sourcePath, params)) {
    return false;
  }
  return renderer::set_material_texture_slots(g_editorAssetService->database,
                                              materialId, textureSlots);
}

bool editor_save_material(const char *virtualPath,
                          const char *parentVirtualPath) noexcept {
  if ((virtualPath == nullptr) || (virtualPath[0] == '\0') ||
      (g_editorAssetService == nullptr) ||
      (g_editorAssetService->database == nullptr)) {
    return false;
  }

  const renderer::AssetId materialId =
      renderer::make_asset_id_from_path(virtualPath);
  const renderer::Material *params =
      renderer::find_material_params(g_editorAssetService->database, materialId);
  if (params == nullptr) {
    return false;
  }
  const renderer::MaterialTextureSlots *slots =
      renderer::find_material_texture_slots(g_editorAssetService->database,
                                            materialId);
  const renderer::MaterialTextureSlots emptySlots{};
  return renderer::save_material_asset(
      g_editorAssetService->database, virtualPath, *params,
      (slots != nullptr) ? *slots : emptySlots, parentVirtualPath);
}

EditorMaterialState editor_reload_material(const char *virtualPath) noexcept {
  EditorMaterialState state{};
  if ((virtualPath == nullptr) || (virtualPath[0] == '\0') ||
      (g_editorAssetService == nullptr) ||
      (g_editorAssetService->database == nullptr)) {
    return state;
  }

  const auto reloadResult = renderer::reload_material_asset(
      g_editorAssetService->database, virtualPath);
  if (!reloadResult.has_value()) {
    return state;
  }

  static_cast<void>(fill_material_state(*reloadResult, &state));
  return state;
}

} // namespace engine::runtime
