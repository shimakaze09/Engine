#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

/// Identifies a post-process pass in the ordered pipeline.
enum class PostProcessPassId : std::uint8_t {
  Bloom = 0U,
  SSAO = 1U,
  AutoExposure = 2U,
  Tonemap = 3U,
  FXAA = 4U,
  Count = 5U,
};

/// A single entry in the post-process pass list.
struct PostProcessPassEntry final {
  PostProcessPassId id = PostProcessPassId::Count;
  bool enabled = true;
};

/// Ordered list of post-process passes. Passes execute in array order.
/// The stack is fixed-capacity (no heap allocation).
struct PostProcessStack final {
  static constexpr std::size_t kMaxPasses =
      static_cast<std::size_t>(PostProcessPassId::Count);

  PostProcessPassEntry passes[kMaxPasses]{};
  std::size_t passCount = 0U;
};

/// Initialize the default post-process stack (bloom → SSAO → auto-exposure →
/// tonemap → FXAA). Called once during renderer initialization.
void initialize_post_process_stack() noexcept;

/// Returns the current ordered pass list. Passes whose CVar is disabled will
/// have `enabled = false` but remain in the list for stable ordering.
const PostProcessStack &get_post_process_stack() noexcept;

/// Query whether a specific pass is enabled (CVar + available).
bool is_post_process_pass_enabled(PostProcessPassId id) noexcept;

/// Returns a human-readable name for a pass (used in profiler/debug UI).
const char *post_process_pass_name(PostProcessPassId id) noexcept;

} // namespace engine::renderer
