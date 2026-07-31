// Declares shader-system internals exposed for unit testing: the
// #version-line scan that decides where variant defines splice into a
// shader source.

#pragma once

#include <cstddef>

namespace engine::renderer {

/// Returns the source length up to and including the end of the #version
/// line so variant defines splice in after it. Engine shaders open with
/// their file purpose comment, so the directive is found at any line
/// start, not only at offset zero; 0 when the source has no #version.
std::size_t shader_version_prefix_length(const char *source) noexcept;

} // namespace engine::renderer
