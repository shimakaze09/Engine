// Helper for assertion_release_test.cmake: breaks a SparseSet bounds
// contract on purpose.
//
// Compiled with NDEBUG defined, which is the whole point. With <cassert>
// that build compiled the check out and read past the live count, so the
// configuration users actually ship was the one with no checking in it.
// ENGINE_ASSERT does not consult NDEBUG, so this must still abort with a
// diagnostic naming the condition.
//
// Deliberately includes almost nothing: mixing NDEBUG across translation
// units would be an ODR problem for any inline function whose body
// depends on it, and math/transform.h has some. sparse_set.h and
// assertion.h have none, which is what makes this helper safe to compile
// this way.
//
// argv[1] selects the case:
//   in-range      read a valid slot; must succeed and print the value
//   out-of-range  read past count(); must abort

#include "engine/core/sparse_set.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {

// SparseSet requires an entity type with an index member; a minimal one
// keeps this helper's includes down to sparse_set.h.
struct Handle final {
  std::uint32_t index = 0U;
};

using Set = engine::core::SparseSet<Handle, int, 16U, 8U>;

} // namespace

/// Runs this executable or test program.
int main(int argc, char **argv) {
  const char *mode = (argc > 1) ? argv[1] : "out-of-range";

#ifndef NDEBUG
  // The driver builds this with NDEBUG on purpose; without it the test
  // would be checking the debug path and quietly proving nothing about
  // the shipped one.
  std::printf("HELPER-FAILED NDEBUG was not defined\n");
  std::fflush(stdout);
  return 1;
#endif

  Set set{};
  if (!set.add(Handle{1U}, 42)) {
    std::printf("HELPER-FAILED insert\n");
    std::fflush(stdout);
    return 1;
  }

  if (std::strcmp(mode, "in-range") == 0) {
    // The contract held, so nothing must abort and the value must be the
    // one stored: this is what keeps the other case meaningful, since a
    // helper that aborted on every read would pass it for free.
    std::printf("HELPER-OK entity=%u component=%d\n",
                set.entity_at(0U).index, set.component_at(0U));
    std::fflush(stdout);
    return 0;
  }

  std::printf("HELPER-READY count=%zu\n", set.count());
  std::fflush(stdout);

  // One past the live count. In-bounds of the backing array, so nothing
  // faults -- it simply returns whatever that slot holds, which is why
  // this needed a check rather than a crash to catch it.
  const Handle leaked = set.entity_at(set.count());

  std::printf("HELPER-FAILED no abort, read %u\n", leaked.index);
  std::fflush(stdout);
  return 1;
}
