// Compatibility shim: the cooked-asset staleness diagnostic moved to
// engine::content (#171 C1). Deletion condition: the C4 consumer migration
// removes the last include of this renderer-path header.

#pragma once

#include "engine/content/asset_staleness.h"

namespace engine::renderer {

using content::warn_if_cooked_asset_stale;
using content::reset_cooked_asset_stale_warnings;

} // namespace engine::renderer
