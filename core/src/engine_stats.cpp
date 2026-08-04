// Implements engine stats behavior for the Engine core engine. A mutex
// guards the published snapshot so the header's thread-safety promise holds
// for concurrent producers and consumers (audit M-13); calls are frame-level
// and never on a per-entity hot path.

#include "engine/core/engine_stats.h"

#include <mutex>

namespace engine::core {

namespace {

EngineStats g_engineStats{};
std::mutex g_engineStatsMutex{};

} // namespace

/// Resets this object back to its reusable empty state for engine stats.
void reset_engine_stats() noexcept {
  std::lock_guard<std::mutex> lock(g_engineStatsMutex);
  g_engineStats = EngineStats{};
}

/// Sets the requested value for engine stats.
void set_engine_stats(const EngineStats &stats) noexcept {
  std::lock_guard<std::mutex> lock(g_engineStatsMutex);
  g_engineStats = stats;
}

EngineStats get_engine_stats() noexcept {
  std::lock_guard<std::mutex> lock(g_engineStatsMutex);
  return g_engineStats;
}

} // namespace engine::core
