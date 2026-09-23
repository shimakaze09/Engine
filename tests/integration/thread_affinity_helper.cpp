// Helper for thread_affinity_test.cmake: records this thread as the main
// one the way initialize_core does, then reaches a main-thread-only entry
// -- the render device accessor -- from a second thread.
//
// argv[1]:
//   main    call it on the main thread; must return normally
//   worker  call it from a spawned thread; must abort naming the check
//
// In a build with NDEBUG the check compiles out by design, so the helper
// says SKIPPED instead of passing on a check that was never there.

#include "engine/core/logging.h"
#include "engine/core/native_thread.h"
#include "engine/core/thread_affinity.h"
#include "engine/renderer/render_device.h"

#include "../quiet_abort.h"

#include <cstdio>
#include <cstring>

#if !defined(NDEBUG)
namespace {

void touch_device(void * /*userData*/) noexcept {
  static_cast<void>(engine::renderer::render_device());
}

} // namespace
#endif

/// Runs this executable or test program.
int main(int argc, char **argv) {
#if defined(NDEBUG)
  static_cast<void>(argc);
  static_cast<void>(argv);
  std::printf("SKIPPED: thread-affinity checks are debug-only\n");
  return 0;
#else
  engine::tests::quiet_abort_dialogs();
  const char *mode = (argc > 1) ? argv[1] : "worker";
  static_cast<void>(engine::core::initialize_logging());
  engine::core::set_main_thread();

  if (std::strcmp(mode, "main") == 0) {
    touch_device(nullptr);
    std::printf("HELPER-OK main thread passed the check\n");
    return 0;
  }

  std::printf("HELPER-READY\n");
  std::fflush(stdout);
  engine::core::NativeThread worker{};
  if (!worker.spawn(&touch_device, nullptr)) {
    std::printf("HELPER-FAILED spawn\n");
    return 1;
  }
  worker.join();
  std::printf("HELPER-FAILED the worker reached the device unchecked\n");
  return 1;
#endif
}
