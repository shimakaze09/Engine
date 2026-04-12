#include <cstdio>

#include "engine/core/mem_tracker.h"

using namespace engine::core;

static bool test_init_zeroes() noexcept {
  mem_tracker_init();
  for (std::size_t i = 0U; i < kMemTagCount; ++i) {
    if (mem_tracker_current_bytes(static_cast<MemTag>(i)) != 0) {
      return false;
    }
  }
  return true;
}

static bool test_alloc_and_free() noexcept {
  mem_tracker_init();
  mem_tracker_alloc(MemTag::Physics, 1024U);
  if (mem_tracker_current_bytes(MemTag::Physics) != 1024) {
    return false;
  }
  mem_tracker_free(MemTag::Physics, 512U);
  if (mem_tracker_current_bytes(MemTag::Physics) != 512) {
    return false;
  }
  mem_tracker_free(MemTag::Physics, 512U);
  if (mem_tracker_current_bytes(MemTag::Physics) != 0) {
    return false;
  }
  return true;
}

static bool test_multiple_tags() noexcept {
  mem_tracker_init();
  mem_tracker_alloc(MemTag::Renderer, 2048U);
  mem_tracker_alloc(MemTag::Audio, 4096U);
  mem_tracker_alloc(MemTag::ECS, 8192U);

  if (mem_tracker_current_bytes(MemTag::Renderer) != 2048) {
    return false;
  }
  if (mem_tracker_current_bytes(MemTag::Audio) != 4096) {
    return false;
  }
  if (mem_tracker_current_bytes(MemTag::ECS) != 8192) {
    return false;
  }
  // Ensure Physics was not affected
  if (mem_tracker_current_bytes(MemTag::Physics) != 0) {
    return false;
  }
  return true;
}

static bool test_snapshot() noexcept {
  mem_tracker_init();
  mem_tracker_alloc(MemTag::Scripting, 100U);
  mem_tracker_alloc(MemTag::Scripting, 200U);
  mem_tracker_free(MemTag::Scripting, 50U);

  MemTagSnapshot snaps[kMemTagCount]{};
  const std::size_t count = mem_tracker_snapshot(snaps, kMemTagCount);
  if (count != kMemTagCount) {
    return false;
  }

  const auto &scripting = snaps[static_cast<std::size_t>(MemTag::Scripting)];
  if (scripting.currentBytes != 250) {
    return false;
  }
  if (scripting.totalAllocated != 300U) {
    return false;
  }
  if (scripting.totalFreed != 50U) {
    return false;
  }
  return true;
}

static bool test_tag_names() noexcept {
  if (mem_tag_name(MemTag::Physics) == nullptr) {
    return false;
  }
  if (mem_tag_name(MemTag::Renderer) == nullptr) {
    return false;
  }
  if (mem_tag_name(MemTag::Audio) == nullptr) {
    return false;
  }
  if (mem_tag_name(MemTag::Scripting) == nullptr) {
    return false;
  }
  if (mem_tag_name(MemTag::ECS) == nullptr) {
    return false;
  }
  if (mem_tag_name(MemTag::Assets) == nullptr) {
    return false;
  }
  if (mem_tag_name(MemTag::General) == nullptr) {
    return false;
  }
  return true;
}

static bool test_invalid_tag() noexcept {
  mem_tracker_init();
  // Should not crash with out-of-range tag
  mem_tracker_alloc(static_cast<MemTag>(255U), 100U);
  mem_tracker_free(static_cast<MemTag>(255U), 100U);
  if (mem_tracker_current_bytes(static_cast<MemTag>(255U)) != 0) {
    return false;
  }
  return true;
}

static bool test_null_snapshot() noexcept {
  if (mem_tracker_snapshot(nullptr, 10U) != 0U) {
    return false;
  }
  return true;
}

int main() {
  int failures = 0;
  const auto run = [&](const char *name, bool (*fn)()) {
    if (!fn()) {
      std::printf("FAIL: %s\n", name);
      ++failures;
    }
  };

  run("test_init_zeroes", test_init_zeroes);
  run("test_alloc_and_free", test_alloc_and_free);
  run("test_multiple_tags", test_multiple_tags);
  run("test_snapshot", test_snapshot);
  run("test_tag_names", test_tag_names);
  run("test_invalid_tag", test_invalid_tag);
  run("test_null_snapshot", test_null_snapshot);

  if (failures > 0) {
    std::printf("%d test(s) failed\n", failures);
    return 1;
  }
  std::printf("All mem_tracker tests passed\n");
  return 0;
}
