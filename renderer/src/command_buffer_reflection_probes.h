// Declares the reflection-probe passes the frame flush drives: baking a
// probe whose capture is out of date, and choosing the environment that
// lights a view.

#pragma once

#include <cstddef>

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"
#include "engine/math/vec3.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

/// Captures and bakes the first probe request whose capture is out of date
/// (a new request, changed settings or position, a new sky, or a requested
/// bake), at most one per call so a scene of probes spreads its cost over
/// frames. The capture draws `inputs` with the sky environment `skyIbl` as
/// its ambient light; `skyCubemap` is the sky the cubemap sky model draws.
/// Leaves the back buffer bound.
void bake_pending_reflection_probe(const OffscreenSceneInputs &inputs,
                                   const IblSelection &skyIbl,
                                   DeviceTextureHandle skyCubemap) noexcept;

/// The environment lighting a view whose camera is at `cameraPosition`: the
/// baked probe whose box holds it (the smallest box, then the lowest
/// request index), otherwise `skyIbl`. Records the choice for
/// active_reflection_probe(view `viewIndex`).
IblSelection select_view_environment(const BackendState &backend,
                                     std::size_t viewIndex,
                                     const math::Vec3 &cameraPosition,
                                     const IblSelection &skyIbl) noexcept;

/// Destroys every probe's capture and environment textures.
void destroy_reflection_probe_resources(BackendState &backend) noexcept;

} // namespace engine::renderer
