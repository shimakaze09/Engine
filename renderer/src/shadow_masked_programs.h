// Declares the alpha-mask shadow programs' lifecycle: the MASKED variants
// of the cascade/spot, skinned and point shadow depth programs, which
// shadow_caster_draw routes mask-mode casters through.

#pragma once

#include "engine/renderer/render_device.h"

namespace engine::renderer {

struct BackendState;

/// Loads the MASKED shadow programs for each shadow family that is
/// available. A program that fails to load or link leaves mask-mode
/// materials casting full-silhouette shadows in that family, with a
/// warning.
void init_masked_shadow_programs(BackendState &backend,
                                 const RenderDevice *dev) noexcept;

/// Re-resolves the MASKED shadow programs after a shader reload; a program
/// whose interface broke is disabled, with a warning.
void refresh_masked_shadow_programs(BackendState &backend,
                                    const RenderDevice *dev) noexcept;

/// Destroys the MASKED shadow programs.
void release_masked_shadow_programs(BackendState &backend) noexcept;

} // namespace engine::renderer
