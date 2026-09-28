// Declares asset labels in the editor, as Unity's Asset Labels: the row
// under the Assets panel's selection that lists the selected asset's
// labels, removes one with its button and adds one typed in, and the save
// that writes them to the asset's sidecar (the only place they live) and
// then brings the asset index and the catalog's tags up to date.

#pragma once

#include "engine/content/asset_metadata.h"

namespace engine::editor {

/// Writes `labels` into the sidecar of the asset at `assetOsPath`, keeping
/// the rest of the sidecar as it was, through the sidecar's staged write.
/// Then the asset index entry and the catalog's tags follow. False, logged,
/// with nothing changed, when the asset has no readable sidecar (it has no
/// identity to label yet) or the write fails.
bool save_asset_labels(const char *assetOsPath,
                       const content::AssetLabels &labels) noexcept;

/// Draws the label row for the asset at `assetOsPath`, the Assets panel's
/// selection: its labels, each removed by its button, and a field that
/// adds the label typed into it on Enter. An asset without a sidecar of
/// its own (a cooked output) says labels belong to its source instead.
void draw_asset_labels_row(const char *assetOsPath) noexcept;

} // namespace engine::editor
