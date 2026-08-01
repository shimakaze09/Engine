// Verifies NativeThread semantics for the Engine test suite (audit
// H-14): spawn runs the entry and reports success, invalid spawns fail
// as return values instead of terminating, join synchronizes with the
// entry's writes, moves transfer ownership, and destruction joins.

#include "engine/core/native_thread.h"

#include <atomic>

namespace {

std::atomic<int> g_counter{0};

void increment_entry(void *userData) noexcept {
  auto *amount = static_cast<const int *>(userData);
  g_counter.fetch_add(*amount, std::memory_order_relaxed);
}

/// EXPECTATION (audit H-14): spawn/join runs the entry exactly once with
/// its user data and join makes the entry's writes visible; null entries
/// and double-spawns fail by return value; joins on empty threads no-op;
/// moved-from threads give up ownership to the destination.
int check_native_thread_semantics() {
  g_counter.store(0, std::memory_order_relaxed);
  const int five = 5;
  const int nine = 9;

  engine::core::NativeThread thread{};
  if (thread.joinable()) {
    return 501;
  }
  thread.join();

  if (thread.spawn(nullptr, nullptr)) {
    return 502;
  }
  if (!thread.spawn(&increment_entry, const_cast<int *>(&five))) {
    return 503;
  }
  if (!thread.joinable()) {
    return 504;
  }
  if (thread.spawn(&increment_entry, const_cast<int *>(&five))) {
    return 505;
  }
  thread.join();
  if (thread.joinable()) {
    return 506;
  }
  if (g_counter.load(std::memory_order_relaxed) != 5) {
    return 507;
  }

  engine::core::NativeThread source{};
  if (!source.spawn(&increment_entry, const_cast<int *>(&nine))) {
    return 508;
  }
  engine::core::NativeThread destination{std::move(source)};
  if (source.joinable() || !destination.joinable()) {
    return 509;
  }
  destination.join();
  if (g_counter.load(std::memory_order_relaxed) != 14) {
    return 510;
  }

  {
    engine::core::NativeThread scoped{};
    if (!scoped.spawn(&increment_entry, const_cast<int *>(&five))) {
      return 511;
    }
    // Destructor must join the running worker rather than abandon it.
  }
  return (g_counter.load(std::memory_order_relaxed) == 19) ? 0 : 512;
}

} // namespace

/// Runs this executable or test program.
int main() { return check_native_thread_semantics(); }
