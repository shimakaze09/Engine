// Declares engine pipeline types and APIs for the Engine runtime world.

#pragma once

#include <cstdint>
#include <memory>

namespace engine {

namespace runtime {
class World;
struct EditorBridge;

/// Result of routing one native platform input event through editor capture.
enum class InputEventRoute : std::uint8_t {
  Gameplay = 0,
  EditorCaptured,
  QuitRequested,
};

/// Lets the editor process one native event before deciding whether gameplay
/// input should see it.
InputEventRoute process_editor_input_event(const EditorBridge *bridge,
                                           void *nativeEvent) noexcept;

/// Processes a queued script scene operation, if one exists.
/// Returns false when a pending operation exists but cannot be applied.
bool process_pending_scene_op(World &world) noexcept;
} // namespace runtime

/// Decomposed engine main-loop pipeline.
///
/// Each frame is split into named stages (input, play transitions, timing,
/// scripting, assets, hot-reload, audio, animation, simulation graph, camera,
/// render-prep graph, post-frame, render, diagnostics, cleanup, pacing).
/// The camera stage runs between the last fixed step and render prep so
/// culling and interpolation consume the frame's own camera.
/// EnginePipeline owns the per-run resources
/// (World, CommandBuffer, AssetDatabase, ...) and executes one frame at a time.
///
/// Typical usage (from engine::run):
///   EnginePipeline pipeline;
///   if (!pipeline.initialize(maxFrames)) return;
///   while (pipeline.execute_frame()) {}
///   pipeline.teardown();
class EnginePipeline final {
public:
  EnginePipeline() noexcept;
  /// Closes a run that is still open, so the aliases a run publishes never
  /// outlive the storage they name.
  ~EnginePipeline() noexcept;

  EnginePipeline(const EnginePipeline &) = delete;
  EnginePipeline &operator=(const EnginePipeline &) = delete;

  /// Allocate runtime resources (World, renderers, assets, bootstrap scene).
  /// Must be called after engine::bootstrap().  A pipeline holds one run at
  /// a time: a run this pipeline still holds is closed before the
  /// replacement allocates, so the two never overlap.
  /// Returns false on allocation or subsystem failure.
  bool initialize(std::uint32_t maxFrames) noexcept;

  /// Execute one frame.  Returns true if the loop should continue, false to
  /// exit (max-frame reached, platform quit, or fatal error).
  bool execute_frame() noexcept;

  /// True when the loop exited because a frame stage failed fatally.
  bool had_fatal_error() const noexcept;

  /// The pipeline-owned World, for bridge-less embeddings (player mode)
  /// and tests; null before initialize() and after teardown().
  runtime::World *world() noexcept;

  /// Release per-run resources.  Safe to call even if initialize() failed,
  /// and safe to call repeatedly.  Destruction and a replacing initialize()
  /// release the same resources, so calling this is a matter of choosing
  /// when the run ends rather than whether it is closed.
  void teardown() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace engine
