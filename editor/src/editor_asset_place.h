// Places assets from the Assets panel into the open scene: the drag
// payload an asset row carries, and the one undoable command a Scene view
// drop, an Entities panel drop and a double-click all place through.
#pragma once

#include <cstddef>

#include "editor_asset_index.h"
#include "editor_commands.h"
#include "engine/content/asset_type_table.h"
#include "engine/runtime/world.h"

namespace engine::editor {

/// ImGui payload type an asset row carries while dragged.
inline constexpr const char *kAssetPlacePayloadType = "ASSET_PLACE";

/// What a dragged asset row carries: its virtual path and the kind the
/// index classified it as, so a drop target never re-derives the kind
/// from the path.
struct AssetPlacePayload final {
  char virtualPath[512] = {};
  content::AssetTypeTag kind = content::AssetTypeTag::Unknown;
  bool isSource = false;
};

/// True when `entry` can be placed in a scene: a prefab, a cooked mesh,
/// or a model (the .gltf or .glb a mesh is cooked from).
bool asset_entry_is_placeable(const AssetIndexEntry &entry) noexcept;

/// Fills `out` from `entry`; false when the entry is not placeable or has
/// no virtual path (a file outside the asset mount).
bool make_asset_place_payload(const AssetIndexEntry &entry,
                              AssetPlacePayload *out) noexcept;

/// Makes the last ImGui item a drag source carrying `entry` when it is
/// placeable; a no-op otherwise.
void asset_place_drag_source(const AssetIndexEntry &entry) noexcept;

/// Inside an ImGui drop target: true when an asset was dropped on it this
/// frame, with the asset in `out`.
bool accept_asset_place_payload(AssetPlacePayload *out) noexcept;

/// Places the asset as one undoable step and selects it:
/// - a prefab becomes an instance of it, every component it holds;
/// - a cooked mesh, or a model through the mesh cooked from it, becomes an
///   entity named after the file with that mesh and, when the mesh is
///   skinned and exactly one animation controller in its folder drives
///   its skeleton, an Animation component playing that controller.
/// Under `placement.parent` the instance sits at the parent's origin;
/// otherwise at `placement.position` when given, else where the prefab
/// put it (a mesh at the origin). Refused, with the world unchanged and
/// the reason logged, while the world cannot be edited (Play), for a
/// parent that is not alive, an asset that is not placeable, a prefab
/// that does not load, or a model with no cooked mesh. Returns the new
/// entity, or kInvalidEntity.
runtime::Entity
execute_asset_instantiate(const AssetPlacePayload &asset,
                          const EntitySpawnPlacement &placement) noexcept;

} // namespace engine::editor
