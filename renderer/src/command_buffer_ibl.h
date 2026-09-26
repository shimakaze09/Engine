// Declares private IBL bake helpers (prefiltered environment, irradiance,
// BRDF LUT) shared by backend init/teardown and the frame flush.
// Split out of command_buffer.cpp (REVIEW_FINDINGS A1).

#pragma once

#include <cstdint>

#include "command_buffer_context.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

/// Reads the r_env_* size cvars through the flush's handle references into
/// normalized ReflectionProbeBakeSettings; a per-frame read, so it scans no
/// name and takes no lock once the references have resolved.
ReflectionProbeBakeSettings
cvar_reflection_probe_bake_settings(const FlushCVars &cvars) noexcept;
/// Prefilters sourceCubemap, the device texture of sourceTexture, into the
/// cached specular environment map and returns it (invalid when
/// unavailable). Re-bakes when the source texture, its device texture or
/// the bake settings change; the texture handle carries a generation, so a
/// new environment that reuses a released one's device handle re-bakes.
DeviceTextureHandle
ensure_prefiltered_environment(BackendState &backend, const RenderDevice *dev,
                               TextureHandle sourceTexture,
                               DeviceTextureHandle sourceCubemap,
                               ReflectionProbeBakeSettings settings) noexcept;
/// Convolves sourceCubemap, the device texture of sourceTexture, into the
/// cached diffuse irradiance map and returns it (invalid when
/// unavailable). Re-bakes on the same changes as the prefilter.
DeviceTextureHandle
ensure_irradiance_environment(BackendState &backend, const RenderDevice *dev,
                              TextureHandle sourceTexture,
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
