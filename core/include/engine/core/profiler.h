// Declares profiler types and APIs for the Engine core engine.
//
// Single-threaded by contract: the frame buffer, the scope stack and the
// depth counter are plain globals, so a scope opened off the thread that
// initialized the profiler would corrupt all three. The engine only
// profiles pipeline stages, which all run on that thread, so enforcing
// the contract costs one thread-id comparison per scope and buys back the
// synchronisation a shared buffer would otherwise need on a hot path.
//
// profiler_begin_scope refuses a scope from any other thread and says so
// once, rather than racing quietly. Profiling inside a job means giving
// each thread its own buffer and merging at frame end, which is a change
// to make when something actually needs it.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Initializes the owning system for profiler.
bool initialize_profiler() noexcept;
/// Shuts down the owning system for profiler.
void shutdown_profiler() noexcept;

/// Clears the frame's scope entries and stamps the frame start.
void profiler_begin_frame() noexcept;
/// Stamps the frame end used by profiler_frame_time_ms().
void profiler_end_frame() noexcept;

/// Begins a profiler scope. Returns false if the scope was not recorded --
/// the frame is full, the nesting is too deep, or the caller is not the
/// thread that initialized the profiler (see the note above).
bool profiler_begin_scope(const char *name) noexcept;
/// Closes the innermost open scope.
void profiler_end_scope() noexcept;

/// RAII helper: opens a named scope for the enclosing block.
struct ProfileScope final {
  /// Opens the named scope.
  explicit ProfileScope(const char *name) noexcept;
  ~ProfileScope() noexcept;
  ProfileScope(const ProfileScope &) = delete;
  ProfileScope &operator=(const ProfileScope &) = delete;

private:
  bool m_active = false;
};

/// One recorded scope: name, timing, depth, and parent index.
struct ProfileEntry final {
  const char *name = nullptr;
  float durationMs = 0.0F;
  std::uint32_t depth = 0U;
  std::uint32_t parentIndex = 0xFFFFFFFFU;
};

/// Copies this frame's entries into out; returns the count.
std::size_t profiler_get_entries(ProfileEntry *out,
                                 std::size_t maxEntries) noexcept;
/// Duration of the last completed frame in milliseconds.
float profiler_frame_time_ms() noexcept;
/// Computes flame-graph start offsets for entries; false on bad input.
bool profiler_compute_flame_starts(const ProfileEntry *entries,
                                   std::size_t count, float *outStartMs,
                                   std::uint32_t *outMaxDepth) noexcept;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ENGINE_PROFILE_CONCAT_(a, b) a##b
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ENGINE_PROFILE_CONCAT(a, b) ENGINE_PROFILE_CONCAT_(a, b)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ENGINE_PROFILE_SCOPE(name)                                             \
  ::engine::core::ProfileScope ENGINE_PROFILE_CONCAT(engineProfileScope_,      \
                                                     __LINE__)(name)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PROFILE_SCOPE(name) ENGINE_PROFILE_SCOPE(name)

} // namespace engine::core
