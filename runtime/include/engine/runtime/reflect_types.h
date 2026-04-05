#pragma once

namespace engine::runtime {

// Anchor symbol for static-library linkers to pull reflect_types.cpp.
void ensure_runtime_reflection_registered() noexcept;

} // namespace engine::runtime
