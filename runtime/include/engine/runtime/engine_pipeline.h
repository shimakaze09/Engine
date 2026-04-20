#pragma once

#include <cstdint>
#include <memory>

namespace engine {

/// Decomposed engine main-loop pipeline.
///
/// Each frame is split into named stages (input, play transitions, timing,
/// scripting, assets, hot-reload, audio, simulation/frame-graph, post-frame,
/// render, diagnostics, cleanup).  EnginePipeline owns the per-run resources
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
  ~EnginePipeline() noexcept;

  EnginePipeline(const EnginePipeline &) = delete;
  EnginePipeline &operator=(const EnginePipeline &) = delete;

  /// Allocate runtime resources (World, renderers, assets, bootstrap scene).
  /// Must be called after engine::bootstrap().
  /// Returns false on allocation or subsystem failure.
  bool initialize(std::uint32_t maxFrames) noexcept;

  /// Execute one frame.  Returns true if the loop should continue, false to
  /// exit (max-frame reached, platform quit, or fatal error).
  bool execute_frame() noexcept;

  /// Release per-run resources.  Safe to call even if initialize() failed.
  void teardown() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace engine
