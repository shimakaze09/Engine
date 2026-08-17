// Verifies the render-device generational slot table (#165): allocated
// handles resolve to their payload, release bumps the slot generation so
// stale handle copies fail resolve instead of aliasing the slot's next
// occupant, exhaustion reports failure instead of recycling live slots,
// clear() invalidates every outstanding handle, and the invalid encoding
// (0) never resolves.

#include "device_slot_table.h"

#include <cstdint>
#include <cstdio>

namespace {

using engine::renderer::device_slot_detail::DeviceSlotTable;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

struct Payload final {
  std::uint32_t value = 0U;
};

void test_allocate_resolve_release() noexcept {
  DeviceSlotTable<Payload, 8U> table{};

  CHECK(table.resolve(0U) == nullptr, "invalid encoding never resolves");

  const std::uint32_t handle = table.allocate(Payload{42U});
  CHECK(handle != 0U, "allocation returns a nonzero handle");
  Payload *payload = table.resolve(handle);
  CHECK((payload != nullptr) && (payload->value == 42U),
        "live handle resolves to its payload");
  CHECK(table.live_count() == 1U, "one live entry after allocation");

  CHECK(table.release(handle), "release succeeds for a live handle");
  CHECK(table.resolve(handle) == nullptr, "released handle is stale");
  CHECK(!table.release(handle), "double release reports stale");
  CHECK(table.live_count() == 0U, "no live entries after release");
}

void test_stale_handle_does_not_alias_reused_slot() noexcept {
  DeviceSlotTable<Payload, 4U> table{};

  const std::uint32_t first = table.allocate(Payload{1U});
  CHECK(table.release(first), "first occupant released");

  // The freed slot is reused; the old handle must keep failing resolve.
  const std::uint32_t second = table.allocate(Payload{2U});
  CHECK(second != 0U, "slot reuse allocates");
  CHECK(second != first, "reused slot encodes a new generation");
  CHECK(table.resolve(first) == nullptr,
        "stale handle rejects instead of aliasing the reused slot");
  Payload *payload = table.resolve(second);
  CHECK((payload != nullptr) && (payload->value == 2U),
        "new occupant resolves normally");
}

void test_exhaustion_reports_failure() noexcept {
  DeviceSlotTable<Payload, 4U> table{}; // 3 usable slots (slot 0 reserved)

  std::uint32_t handles[3] = {};
  for (std::uint32_t i = 0U; i < 3U; ++i) {
    handles[i] = table.allocate(Payload{i});
    CHECK(handles[i] != 0U, "allocation inside capacity succeeds");
  }
  CHECK(table.allocate(Payload{99U}) == 0U,
        "allocation past capacity fails instead of recycling live slots");
  for (std::uint32_t i = 0U; i < 3U; ++i) {
    CHECK(table.resolve(handles[i]) != nullptr,
          "live handles survive an exhausted allocation");
  }

  CHECK(table.release(handles[1]), "releasing frees capacity");
  CHECK(table.allocate(Payload{7U}) != 0U,
        "allocation succeeds again after a release");
}

void test_clear_invalidates_all_handles() noexcept {
  DeviceSlotTable<Payload, 8U> table{};

  const std::uint32_t a = table.allocate(Payload{1U});
  const std::uint32_t b = table.allocate(Payload{2U});
  table.clear();
  CHECK(table.resolve(a) == nullptr, "clear invalidates the first handle");
  CHECK(table.resolve(b) == nullptr, "clear invalidates the second handle");
  CHECK(table.live_count() == 0U, "clear leaves no live entries");

  const std::uint32_t reused = table.allocate(Payload{3U});
  CHECK(reused != 0U, "allocation works after clear");
  CHECK((reused != a) && (reused != b),
        "post-clear handles never repeat pre-clear encodings");
}

void test_generation_wrap_skips_zero() noexcept {
  DeviceSlotTable<Payload, 2U> table{}; // 1 usable slot, 31 generation bits

  // Drive one slot through several generations; each cycle's handle must
  // be nonzero and the previous cycle's handle must be stale.
  std::uint32_t previous = 0U;
  for (int cycle = 0; cycle < 8; ++cycle) {
    const std::uint32_t handle = table.allocate(Payload{0U});
    CHECK(handle != 0U, "cycled slot allocates");
    if (previous != 0U) {
      CHECK(table.resolve(previous) == nullptr,
            "previous cycle's handle is stale");
    }
    CHECK(table.release(handle), "cycled slot releases");
    previous = handle;
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  std::printf("=== Device Slot Table Unit Tests ===\n");

  test_allocate_resolve_release();
  test_stale_handle_does_not_alias_reused_slot();
  test_exhaustion_reports_failure();
  test_clear_invalidates_all_handles();
  test_generation_wrap_skips_zero();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
