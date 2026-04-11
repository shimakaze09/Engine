#include "engine/core/engine_stats.h"

namespace engine::core {

namespace {

EngineStats g_engineStats{};

} // namespace

void reset_engine_stats() noexcept { g_engineStats = EngineStats{}; }

void set_engine_stats(const EngineStats &stats) noexcept {
  g_engineStats = stats;
}

EngineStats get_engine_stats() noexcept { return g_engineStats; }

} // namespace engine::core
