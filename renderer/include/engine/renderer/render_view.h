// Declares the render views a frame can draw: the Game view the pipeline
// always owns (the gameplay camera; the player's window or the editor's
// Game panel) and the editor's Scene view (its own camera, size and
// overlays). Each view keeps its own targets and history; programs,
// shadow atlases and environment maps are shared.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

/// Identifies one render view. Game is always index 0.
enum class RenderViewId : std::uint8_t {
  Game = 0,
  Scene = 1,
};

/// Number of views a frame can render.
inline constexpr std::size_t kMaxRenderViews = 2U;

/// The storage index of a view.
constexpr std::size_t render_view_index(RenderViewId id) noexcept {
  return static_cast<std::size_t>(id);
}

} // namespace engine::renderer
