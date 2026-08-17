// Declares timer manager types and APIs for the Engine runtime world.

#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::runtime {

/// Opaque ID returned by set_timeout / set_interval. Zero is invalid.
using TimerId = std::uint32_t;
inline constexpr TimerId kInvalidTimerId = 0U;

/// Per-World timer manager with fixed-capacity storage.
/// Stores only C++ callback pointers + user-data; Lua integration lives in the
/// scripting layer which wraps this with luaL_ref bookkeeping.
///
/// Serialization helpers encode the minimal state (delay remaining, repeat
/// flag, interval) so that timers survive scene save/load round-trips.
class TimerManager final {
public:
  static constexpr std::size_t kMaxTimers = 256U;
  static constexpr std::size_t kInvalidTimerSlot = kMaxTimers;

  /// Callback signature for C++ timers.
  using Callback = void (*)(TimerId id, void *userData) noexcept;

  /// One-shot timer that fires after @p delaySeconds.
  TimerId set_timeout(float delaySeconds, Callback callback,
                      void *userData) noexcept;

  /// Repeating timer that fires every @p intervalSeconds.
  TimerId set_interval(float intervalSeconds, Callback callback,
                       void *userData) noexcept;

  /// Cancel a pending timer. No-op if already fired / invalid.
  void cancel(TimerId id) noexcept;

  /// Return the generation-matching slot for a TimerId, or kInvalidTimerSlot.
  std::size_t slot_for_id(TimerId id) const noexcept;

  /// Advance all timers by @p dt seconds with pcall-safe callbacks.
  /// Returns number of timers that fired this tick.
  std::size_t tick(float dt) noexcept;

  /// Reset all timers (e.g. on scene load or play-stop transition).
  void clear() noexcept;

  /// Number of currently active timers.
  std::size_t active_count() const noexcept;

  // Timers are runtime-only, per-scene state (issue #209): they are never
  // serialized — a callback has no stable cross-process identity — and
  // scripts re-arm their timers in on_begin_play after a scene loads. The
  // scene transition path clears this manager explicitly.

  // Low-level access for scripting-layer Lua ref management.
  struct Entry final {
    float fireAt = 0.0F;
    float interval = 0.0F;
    Callback callback = nullptr;
    void *userData = nullptr;
    bool repeat = false;
    bool active = false;
  };

  /// Direct read access (for scripting bridge inspection).
  const Entry &entry_at(std::size_t index) const noexcept;

private:
  TimerId make_timer_id(std::size_t slot) const noexcept;
  void ensure_generation(std::size_t slot) noexcept;
  void advance_generation(std::size_t slot) noexcept;
  void release_slot(std::size_t slot) noexcept;

  Entry m_timers[kMaxTimers]{};
  std::uint16_t m_generations[kMaxTimers]{};
  float m_elapsed = 0.0F;
};

} // namespace engine::runtime
