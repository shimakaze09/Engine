// Declares editor bridge types and APIs for the Engine runtime world.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/renderer/asset_database.h"
#include "engine/content/asset_metadata.h"
#include "engine/renderer/material.h"

namespace engine::runtime {

class World;
struct EngineAssetDatabaseService;

/// One change of the editor's play state, recorded when the author causes
/// it. Comparing the play state across two frames can only ever express
/// the net change, so a frame carrying Play, Stop and Play again looks
/// identical to one carrying nothing: the session that ended between them
/// never gets its end hooks. Recording each change keeps them all, in the
/// order they happened.
///
/// Resume is a return from Paused, distinct from Start, so the end and
/// begin hooks of a session are not dispatched around a pause.
enum class PlayTransition : std::uint8_t { Start, Stop, Pause, Resume };

/// Function-pointer bridge the runtime uses to reach the editor. Initialize,
/// shutdown, new-frame, and render callbacks run with the render context
/// current.
struct EditorBridge final {
  bool (*initialize)(void *sdlWindow) noexcept = nullptr;
  void (*shutdown)() noexcept = nullptr;
  void (*new_frame)() noexcept = nullptr;
  void (*render)(float frameMs, float utilizationPct) noexcept = nullptr;
  void (*process_event)(void *sdlEvent) noexcept = nullptr;
  void (*set_world)(World *world) noexcept = nullptr;
  bool (*is_playing)() noexcept = nullptr;
  bool (*is_paused)() noexcept = nullptr;
  bool (*wants_capture_keyboard)() noexcept = nullptr;
  bool (*wants_capture_mouse)() noexcept = nullptr;
  // True at most once per Step click while paused: the pipeline consumes
  // the request and simulates exactly one fixed step that frame.
  bool (*consume_step_request)() noexcept = nullptr;
  // Called instead of an immediate quit on SDL_EVENT_QUIT; true lets the
  // runtime quit right away (the editor has nothing to protect), false
  // means the editor armed its own unsaved-change confirm flow and will
  // request the quit itself once that resolves. Null behaves as true.
  bool (*handle_quit_request)() noexcept = nullptr;
  // Takes the oldest play transition the editor has recorded and returns
  // true, or returns false when none is queued. The pipeline drains this
  // once per frame and dispatches each transition in turn. Null means the
  // bridge records none, and the pipeline falls back to the single net
  // change is_playing/is_paused can express.
  bool (*consume_play_transition)(PlayTransition *outTransition) noexcept =
      nullptr;
};

/// Sets the requested value for editor bridge.
void set_editor_bridge(const EditorBridge *bridge) noexcept;
/// Registered bridge instance, or nullptr when no editor is linked.
const EditorBridge *editor_bridge() noexcept;

/// Publishes (or clears, with nullptr) the pipeline's asset service so
/// editor authoring can request asset loads while the runtime is active.
void set_editor_asset_service(EngineAssetDatabaseService *service) noexcept;
/// Requests an async mesh load for an editor-authored reference by virtual
/// path and returns the path-derived asset id; 0 when no runtime asset
/// service is published, the path cannot resolve, or the request fails.
std::uint64_t editor_request_mesh_asset(const char *virtualPath) noexcept;

/// One asset-picker search hit: a stable id plus its registered display
/// path, so the Inspector's asset reference pickers can search
/// and select by name/path instead of a raw numeric id.
struct EditorAssetSearchResult final {
  content::AssetId assetId = content::kInvalidAssetId;
  char path[260] = {};
};

/// Searches assets of `typeTag` already known to the asset database whose
/// registered file path contains `query` as a case-insensitive substring
/// ("" matches every known asset of that type); writes up to maxResults hits
/// and returns the count actually written. Returns 0 (no results, not an
/// error) when no runtime asset service is published yet.
std::size_t editor_query_assets(content::AssetTypeTag typeTag,
                                const char *query,
                                EditorAssetSearchResult *outResults,
                                std::size_t maxResults) noexcept;
/// Display path for a known asset id; false (outPath left untouched) when
/// the id is not registered in the asset database -- the signal an asset
/// reference picker uses to render its broken-reference state.
bool editor_asset_display_path(std::uint64_t assetId, char *outPath,
                               std::size_t outPathSize) noexcept;
/// The persistent identity the catalog holds for a known asset id, or a
/// nil reference when the id is unregistered or the asset was never
/// imported. An editor gesture that points a component at an asset stores
/// this beside the id, because the id is where the bytes are this session
/// and the reference is what a saved document names.
core::AssetRef editor_asset_ref(std::uint64_t assetId) noexcept;

/// Why establishing an asset's identity did not succeed, so a caller can
/// tell "already had one" from a fault it must report and roll back.
enum class EditorIdentityResult : std::uint8_t {
  /// The sidecar was written and the asset catalogued under it.
  Created,
  /// A readable sidecar was already there; nothing was changed.
  AlreadyIdentified,
  /// A sidecar exists but will not read. Never replaced: minting a new
  /// identity over one that may still be referenced would break every
  /// reference to the asset instead of reporting a repairable file.
  SidecarUnusable,
  /// Minting or the staged sidecar write failed.
  WriteFailed,
};

/// Gives the asset at `osPath` a persistent identity if it has none,
/// writing its source-side sidecar and catalouging it so a reference to
/// it resolves in this session rather than after a restart.
///
/// An editor gesture that creates a referenceable asset calls this in the
/// same save transaction that writes the asset: an asset the editor
/// generated must not need the author to run a tool afterwards before it
/// can be referenced at all.
EditorIdentityResult
editor_establish_asset_identity(const char *osPath) noexcept;

// --- Material editor bridge ---
//
// The material editor panel (editor/) never touches renderer::AssetDatabase
// directly -- it goes through these functions, matching the asset-picker
// bridge above, so the editor stays behind the published-service seam even
// though editor->runtime->renderer is otherwise a legal dependency
// direction.

/// Full editable state of one material asset: resolved scalar/vector
/// params, alpha mode/cutoff, UV transform, texture-slot references, and
/// the parent path (hasParent=false when it has none). found=false when no
/// runtime asset service is published or the file fails to load/parse (the
/// caller's existing buffer, if any, should keep displaying rather than be
/// cleared -- the same "malformed reload preserves previous state" contract
/// reload_material_asset already gives the loader layer).
struct EditorMaterialState final {
  bool found = false;
  content::AssetId materialId = content::kInvalidAssetId;
  renderer::Material params{};
  renderer::MaterialTextureSlots textureSlots{};
  char parentVirtualPath[260] = {};
  bool hasParent = false;
};

/// Loads (if not already loaded) `virtualPath` and returns its current
/// editable state.
EditorMaterialState editor_load_material(const char *virtualPath) noexcept;

/// Writes `params`/`textureSlots` directly into the live asset database
/// record -- the same mutation register_material_asset and
/// set_material_texture_slots perform -- so the very next frame's render
/// prep and resolve_material_textures reflect the edit: the material
/// editor's live viewport feedback. Never touches disk; call
/// editor_save_material to persist. False when the id is unknown or no
/// runtime asset service is published.
bool editor_set_material_params(
    content::AssetId materialId, const renderer::Material &params,
    const renderer::MaterialTextureSlots &textureSlots) noexcept;

/// Persists the live in-memory state for `virtualPath` to disk via
/// save_material_asset (staged atomic write): a failure (including an
/// unresolvable texture-slot path) leaves the previous file on disk
/// completely untouched. `parentVirtualPath` may be null/empty for no
/// parent.
bool editor_save_material(const char *virtualPath,
                          const char *parentVirtualPath) noexcept;

/// Re-reads `virtualPath` from disk (reload_material_asset) and, on
/// success, returns the freshly loaded state with found=true. On failure
/// the live in-memory record is left exactly as it was (reload_material_
/// asset's contract) and this returns found=false -- the caller should keep
/// displaying its existing buffer rather than clearing it.
EditorMaterialState editor_reload_material(const char *virtualPath) noexcept;

} // namespace engine::runtime
