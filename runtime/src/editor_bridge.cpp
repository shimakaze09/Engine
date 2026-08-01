// Implements editor bridge behavior for the Engine runtime world: bridge
// registration plus the editor-facing asset request entry point backed by
// the pipeline's published asset service.

#include "engine/runtime/editor_bridge.h"

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_streaming.h"
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
  if ((state == renderer::AssetState::Ready) ||
      (state == renderer::AssetState::Loading)) {
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

} // namespace engine::runtime
