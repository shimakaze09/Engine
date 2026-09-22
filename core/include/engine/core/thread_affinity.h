// Which thread is the main one, and a check for code that must run there.
//
// bgfx submission, the Lua VM, the audio API, the native event queue and
// the streaming upload pump are main-thread only. That used to be stated
// in comments and nowhere else, so a job that reached one from a worker
// raced silently and failed rarely, far from the call that caused it.
// ENGINE_ASSERT_MAIN_THREAD turns the comment into a check at the entry.
//
// The check is debug-only, unlike ENGINE_ASSERT, and deliberately so. A
// bounds violation is memory-unsafe the moment it happens, so it aborts in
// every build. An affinity violation is a race that usually does not
// bite, and the purpose here is to find every one of them in development
// -- in a shipped build, aborting on the first would turn a rare glitch
// into a certain crash for whoever hits it.

#pragma once

#include "engine/core/assertion.h"

namespace engine::core {

/// Records the calling thread as the main thread. initialize_core calls
/// it; a test that drives subsystems without core may call it itself.
void set_main_thread() noexcept;
/// Forgets the main thread; shutdown_core calls it.
void clear_main_thread() noexcept;
/// True on the recorded main thread -- and on every thread while none is
/// recorded, so a unit test that never initialized core is not treated as
/// running off the main thread.
bool is_main_thread() noexcept;

} // namespace engine::core

#if !defined(NDEBUG)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ENGINE_ASSERT_MAIN_THREAD()                                          \
  ENGINE_ASSERT(::engine::core::is_main_thread())
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ENGINE_ASSERT_MAIN_THREAD() static_cast<void>(0)
#endif
