#include "engine/core/engine_stats.h"

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

  return 0;
}
