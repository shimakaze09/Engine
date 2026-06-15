// Implements engine stats behavior for the Engine core engine.

#include "engine/core/engine_stats.h"

namespace engine::core {

namespace {

EngineStats g_engineStats{};

} // namespace

/// Resets this object back to its reusable empty state for engine stats.
void reset_engine_stats() noexcept { g_engineStats = EngineStats{}; }

/// Sets the requested value for engine stats.
void set_engine_stats(const EngineStats &stats) noexcept {
  g_engineStats = stats;
}

/// Returns the requested value for engine stats.
EngineStats get_engine_stats() noexcept { return g_engineStats; }

} // namespace engine::core
