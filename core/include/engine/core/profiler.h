// Declares profiler types and APIs for the Engine core engine.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Initializes the owning system for profiler.
bool initialize_profiler() noexcept;
/// Shuts down the owning system for profiler.
void shutdown_profiler() noexcept;

/// Handles profiler begin frame.
void profiler_begin_frame() noexcept;
/// Handles profiler end frame.
void profiler_end_frame() noexcept;

/// Begins a profiler scope. Returns false if the scope was not recorded.
bool profiler_begin_scope(const char *name) noexcept;
/// Handles profiler end scope.
void profiler_end_scope() noexcept;

/// Stores profile scope data used by the engine.
struct ProfileScope final {
  /// Handles profile scope.
  explicit ProfileScope(const char *name) noexcept;
  ~ProfileScope() noexcept;
  ProfileScope(const ProfileScope &) = delete;
  /// Handles operator=.
  ProfileScope &operator=(const ProfileScope &) = delete;

private:
  bool m_active = false;
};

/// Stores profile entry data used by the engine.
struct ProfileEntry final {
  const char *name = nullptr;
  float durationMs = 0.0F;
  std::uint32_t depth = 0U;
  std::uint32_t parentIndex = 0xFFFFFFFFU;
};

/// Handles profiler get entries.
std::size_t profiler_get_entries(ProfileEntry *out,
                                 std::size_t maxEntries) noexcept;
/// Handles profiler frame time ms.
float profiler_frame_time_ms() noexcept;
/// Handles profiler compute flame starts.
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
