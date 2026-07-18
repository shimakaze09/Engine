// Declares private post-processing resource helpers (bloom and luminance
// mip chains, SSAO kernel/noise) shared by backend init/teardown and the
// frame flush. Split out of command_buffer.cpp (REVIEW_FINDINGS A1).

#pragma once

#include <cstdint>

#include "command_buffer_context.h"

namespace engine::renderer {

/// (Re)allocates the bloom mip chain when the drawable size changes.
void ensure_bloom_resources(BackendState &b, int width, int height) noexcept;
/// Releases the bloom mip chain textures and framebuffers.
void destroy_bloom_resources(BackendState &b) noexcept;
/// (Re)allocates the luminance averaging mip chain when the size changes.
void ensure_luminance_resources(BackendState &b, int width,
                                int height) noexcept;
/// Releases the luminance mip chain textures and framebuffers.
void destroy_luminance_resources(BackendState &b) noexcept;
/// Fills kernel with `count` SSAO hemisphere sample vectors.
void generate_ssao_kernel(float *kernel, int count) noexcept;
/// Creates the SSAO rotation noise texture and returns its GPU id.
std::uint32_t create_ssao_noise_texture() noexcept;

} // namespace engine::renderer
