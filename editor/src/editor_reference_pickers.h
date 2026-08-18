// Declares the editor's searchable reference-picker widgets (issue #156):
// an entity picker backed by the live World's names/liveness and an asset
// picker backed by the asset database, both with a broken-reference state
// (missing/stale ids render a warning with repair/clear actions instead of
// silently becoming zero). The filter/search logic is factored into pure
// functions so it is headless-testable without driving ImGui.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/content/asset_metadata.h"
#include "engine/runtime/world.h"

namespace engine::editor {

/// One entity-picker search hit: a persistent id (survives delete/re-create,
/// matching the rest of the editor's undo/redo targeting convention) plus
/// its display name.
struct EntityPickerResult final {
  runtime::PersistentId persistentId = runtime::kInvalidPersistentId;
  char name[32] = {};
};

/// Searches the world's alive entities for a NameComponent whose name
/// contains `query` as a case-insensitive substring ("" matches every named
/// entity); when `requireComponent` is not ComponentEditType-agnostic (see
/// below) results are further filtered to entities carrying that marker.
/// Writes up to maxResults hits ordered by ascending entity index (stable,
/// deterministic across calls) and returns the count written. Pure/headless:
/// takes the World by reference, no ImGui dependency.
std::size_t filter_entities_by_name(
    const runtime::World &world, const char *query,
    EntityPickerResult *outResults, std::size_t maxResults,
    bool (*predicate)(const runtime::World &, runtime::Entity) noexcept =
        nullptr) noexcept;

/// True when `persistentId` resolves to a live entity in `world` (the
/// liveness/world-identity validation an entity reference picker and its
/// broken-reference state need).
bool entity_reference_is_live(const runtime::World &world,
                              runtime::PersistentId persistentId) noexcept;

/// Draws a searchable combo box for a PersistentId entity reference field.
/// Shows the referenced entity's live name, or a broken-reference warning
/// with a Clear button when the id no longer resolves. Returns true when the
/// caller should treat *value as changed (the caller stages/commits the
/// edit through the usual gesture path so it stays undoable). `predicate`
/// restricts the searchable candidate list (e.g. "only SceneCapture
/// entities"); nullptr allows every named entity.
bool draw_entity_reference_picker(
    const char *label, runtime::PersistentId *value,
    bool (*predicate)(const runtime::World &, runtime::Entity) noexcept =
        nullptr) noexcept;

/// Draws a searchable combo box for an AssetId reference field of the given
/// type. Shows the referenced asset's registered path, or a broken-
/// reference warning with a Clear button when the id is unregistered.
/// Returns true when the caller should treat *value as changed.
bool draw_asset_reference_picker(const char *label,
                                 content::AssetTypeTag typeTag,
                                 std::uint64_t *value) noexcept;

/// Draws a searchable combo box for a VFS path reference (script and
/// animation-controller fields, which are addressed by path rather than by
/// asset-database id). Scans the editor asset root under the configured
/// mount for files ending in `extension` ("" disables the extension
/// filter). Shows a broken-reference warning with a Clear button when
/// pathBuffer is non-empty but does not resolve to a file on disk. Returns
/// true when pathBuffer changed this call; the caller stages the change
/// through the usual gesture path.
bool draw_path_reference_picker(const char *label, char *pathBuffer,
                                std::size_t pathBufferSize,
                                const char *extension) noexcept;

} // namespace engine::editor
