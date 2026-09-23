// Implements the main-thread record behind ENGINE_ASSERT_MAIN_THREAD.
//
// A thread-local flag rather than a stored thread id: the question is
// asked at every guarded entry, and reading a thread-local bool is cheaper
// than fetching and comparing an id.

#include "engine/core/thread_affinity.h"

#include <atomic>

namespace engine::core {

namespace {

thread_local bool t_isMainThread = false;
// Whether any thread is recorded. Without it, a thread-local that defaults
// to false would make every unrecorded run look like it is off the main
// thread.
std::atomic<bool> g_mainThreadRecorded{false};

} // namespace

void set_main_thread() noexcept {
  t_isMainThread = true;
  g_mainThreadRecorded.store(true, std::memory_order_release);
}

void clear_main_thread() noexcept {
  g_mainThreadRecorded.store(false, std::memory_order_release);
  t_isMainThread = false;
}

bool is_main_thread() noexcept {
  return t_isMainThread ||
         !g_mainThreadRecorded.load(std::memory_order_acquire);
}

} // namespace engine::core
