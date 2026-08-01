// Declares NativeThread: non-throwing thread creation for the
// no-exception build. std::thread's constructor reports resource
// failure by throwing, which this build converts into process
// termination, so subsystem initialization spawns workers through this
// wrapper and rolls back partial worker sets instead (audit H-14).

#pragma once

#include <cstdint>

namespace engine::core {

/// Move-only owner of one OS thread. spawn reports failure as a return
/// value instead of terminating; join is required before destruction
/// (the destructor asserts nothing is running by joining defensively).
class NativeThread final {
public:
  using EntryFn = void (*)(void *userData);

  NativeThread() noexcept = default;
  ~NativeThread() noexcept;

  NativeThread(const NativeThread &) = delete;
  NativeThread &operator=(const NativeThread &) = delete;
  NativeThread(NativeThread &&other) noexcept;
  NativeThread &operator=(NativeThread &&other) noexcept;

  /// Starts entry(userData) on a new OS thread; false when the OS (or
  /// the trampoline allocation) refuses, leaving this object empty.
  bool spawn(EntryFn entry, void *userData) noexcept;

  /// Whether a spawned thread has not been joined yet.
  bool joinable() const noexcept;

  /// Blocks until the thread finishes and releases it; no-op when empty.
  void join() noexcept;

private:
  void *m_handle = nullptr;
#ifndef _WIN32
  // pthread_t is opaque; store it by value beside a validity flag.
  std::uint64_t m_thread = 0U;
#endif
};

} // namespace engine::core
