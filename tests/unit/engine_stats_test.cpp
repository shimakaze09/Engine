// Verifies engine stats test behavior for the Engine test suite, including
// the documented thread-safety contract: get_engine_stats must return a
// consistent snapshot while another thread publishes (audit M-13).

#include "engine/core/engine_stats.h"
#include "engine/core/native_thread.h"
#include "engine/core/platform.h"

#include <atomic>
#include <cstdint>

namespace {

constexpr std::uint32_t kPublications = 200000U;

std::atomic<bool> g_writerDone{false};

/// Publishes snapshots whose fields all encode the same counter so a torn
/// read is detectable as a field mismatch.
void stats_writer(void *) noexcept {
  using namespace engine::core;
  for (std::uint32_t i = 1U; i <= kPublications; ++i) {
    EngineStats s{};
    s.fps = static_cast<float>(i);
    s.frameTimeMs = static_cast<float>(i);
    s.drawCalls = i;
    s.triCount = i;
    s.entityCount = i;
    s.memoryUsedMb = static_cast<float>(i);
    s.gpuSceneMs = static_cast<float>(i);
    s.gpuTonemapMs = static_cast<float>(i);
    s.jobUtilizationPct = static_cast<float>(i);
    set_engine_stats(s);
  }
  g_writerDone.store(true, std::memory_order_release);
}

/// Reads snapshots concurrently with the writer and fails on any snapshot
/// whose fields disagree (a torn copy of two publications).
bool run_concurrent_snapshot_check() noexcept {
  using namespace engine::core;
  set_engine_stats(EngineStats{});

  NativeThread writer{};
  g_writerDone.store(false, std::memory_order_release);
  if (!writer.spawn(&stats_writer, nullptr)) {
    return false;
  }

  bool consistent = true;
  while (!g_writerDone.load(std::memory_order_acquire)) {
    const EngineStats s = get_engine_stats();
    const auto counter = static_cast<std::uint32_t>(s.drawCalls);
    const auto expected = static_cast<float>(counter);
    if ((s.fps != expected) || (s.frameTimeMs != expected) ||
        (s.triCount != counter) || (s.entityCount != counter) ||
        (s.memoryUsedMb != expected) || (s.gpuSceneMs != expected) ||
        (s.gpuTonemapMs != expected) || (s.jobUtilizationPct != expected)) {
      consistent = false;
      break;
    }
  }

  writer.join();
  return consistent;
}

} // namespace

/// Runs this executable or test program.
int main() {
  using namespace engine::core;

  reset_engine_stats();
  EngineStats stats = get_engine_stats();
  if ((stats.fps != 0.0F) || (stats.frameTimeMs != 0.0F) ||
      (stats.drawCalls != 0U) || (stats.triCount != 0U) ||
      (stats.entityCount != 0U) || (stats.memoryUsedMb != 0.0F)) {
    return 1;
  }

  EngineStats updated{};
  updated.fps = 60.0F;
  updated.frameTimeMs = 16.6667F;
  updated.drawCalls = 123U;
  updated.triCount = 4567U;
  updated.entityCount = 89U;
  updated.memoryUsedMb = 321.5F;
  updated.gpuSceneMs = 5.25F;
  updated.gpuTonemapMs = 0.75F;
  updated.jobUtilizationPct = 44.0F;
  set_engine_stats(updated);

  stats = get_engine_stats();
  if ((stats.fps != 60.0F) || (stats.frameTimeMs != 16.6667F) ||
      (stats.drawCalls != 123U) || (stats.triCount != 4567U) ||
      (stats.entityCount != 89U) || (stats.memoryUsedMb != 321.5F) ||
      (stats.gpuSceneMs != 5.25F) || (stats.gpuTonemapMs != 0.75F) ||
      (stats.jobUtilizationPct != 44.0F)) {
    return 2;
  }

  reset_engine_stats();
  stats = get_engine_stats();
  if ((stats.drawCalls != 0U) || (stats.triCount != 0U) ||
      (stats.entityCount != 0U)) {
    return 3;
  }

  if (process_memory_bytes() == 0U) {
    return 4;
  }

  if (!run_concurrent_snapshot_check()) {
    return 5;
  }

  reset_engine_stats();
  return 0;
}
