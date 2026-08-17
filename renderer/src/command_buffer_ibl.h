// Declares private IBL bake helpers (prefiltered environment, irradiance,
// BRDF LUT) shared by backend init/teardown and the frame flush.
// Split out of command_buffer.cpp (REVIEW_FINDINGS A1).

#pragma once

#include <cstdint>

#include "command_buffer_context.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

/// Reads the r_env_* cvars into normalized ReflectionProbeBakeSettings.
ReflectionProbeBakeSettings cvar_reflection_probe_bake_settings() noexcept;
/// Prefilters sourceCubemap into the cached specular environment map and
/// returns it (invalid when unavailable). Re-bakes when the source cubemap
/// or bake settings change.
DeviceTextureHandle
ensure_prefiltered_environment(BackendState &backend, const RenderDevice *dev,
                               DeviceTextureHandle sourceCubemap,
                               ReflectionProbeBakeSettings settings) noexcept;
/// Convolves sourceCubemap into the cached diffuse irradiance map and
/// returns it (invalid when unavailable). Re-bakes when the source cubemap
/// or bake settings change.
DeviceTextureHandle
ensure_irradiance_environment(BackendState &backend, const RenderDevice *dev,
                              DeviceTextureHandle sourceCubemap,
                              ReflectionProbeBakeSettings settings) noexcept;
/// Renders the split-sum BRDF LUT if needed and returns it (invalid when
/// unavailable).
DeviceTextureHandle ensure_brdf_lut(BackendState &backend,
                                    const RenderDevice *dev,
                                    ReflectionProbeBakeSettings settings) noexcept;
/// Releases prefiltered-environment GPU resources and bake bookkeeping.
void destroy_environment_prefilter_resources(BackendState &backend) noexcept;
/// Releases irradiance-environment GPU resources and bake bookkeeping.
void destroy_environment_irradiance_resources(BackendState &backend) noexcept;
/// Releases the BRDF LUT texture.
void destroy_brdf_lut_resources(BackendState &backend) noexcept;

} // namespace engine::renderer
