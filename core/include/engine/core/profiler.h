#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

bool initialize_profiler() noexcept;
void shutdown_profiler() noexcept;

void profiler_begin_frame() noexcept;
void profiler_end_frame() noexcept;

void profiler_begin_scope(const char *name) noexcept;
void profiler_end_scope() noexcept;

struct ProfileScope final {
  explicit ProfileScope(const char *name) noexcept;
  ~ProfileScope() noexcept;
  ProfileScope(const ProfileScope &) = delete;
  ProfileScope &operator=(const ProfileScope &) = delete;
};

struct ProfileEntry final {
  const char *name = nullptr;
  float durationMs = 0.0F;
  std::uint32_t depth = 0U;
  std::uint32_t parentIndex = 0xFFFFFFFFU;
};

std::size_t profiler_get_entries(ProfileEntry *out,
                                 std::size_t maxEntries) noexcept;
float profiler_frame_time_ms() noexcept;
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
