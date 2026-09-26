// Verifies that the Stats panel's "Memory by Subsystem" has something to
// show (issue #659): while a production pipeline runs, the ECS, physics,
// asset, renderer and scripting tags each carry the bytes their owners
// hold, and once it tears down every byte the pipeline reported is given
// back. Before this nothing in the engine reported, so every bar read a
// measured-looking 0.00 MB. Headless, on the null render device.

#include "engine/core/mem_tracker.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"

#include <cstdio>
#include <filesystem>

#include "../test_harness.h"

namespace {

engine::tests::TestContext g_tests{};

/// Walks upward from the current path until the bundled assets are found.
bool enter_asset_directory() noexcept {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    std::error_code ec{};
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (!ec && std::filesystem::exists(normalized / "assets/main.lua", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

std::int64_t bytes(engine::core::MemTag tag) noexcept {
  return engine::core::mem_tracker_current_bytes(tag);
}

} // namespace

int main() {
  using engine::core::MemTag;
  if (!enter_asset_directory()) {
    g_tests.fail("find the bundled assets");
    return g_tests.finish("pipeline memory report tests");
  }
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    g_tests.fail("bootstrap");
    return g_tests.finish("pipeline memory report tests");
  }

  const MemTag pooled[] = {MemTag::ECS, MemTag::Physics, MemTag::Assets,
                           MemTag::Renderer};
  std::int64_t before[4] = {};
  for (std::size_t i = 0U; i < 4U; ++i) {
    before[i] = bytes(pooled[i]);
  }
  {
    engine::EnginePipeline pipeline;
    g_tests.check(pipeline.initialize(0U), "the pipeline initializes");
    g_tests.check(pipeline.execute_frame(), "a frame runs");
    g_tests.check(bytes(MemTag::ECS) > before[0], "the World is reported");
    g_tests.check(bytes(MemTag::Physics) > before[1],
                  "the physics context is reported");
    g_tests.check(bytes(MemTag::Assets) > before[2],
                  "the asset tables are reported");
    g_tests.check(bytes(MemTag::Renderer) > before[3],
                  "the renderer's pools are reported");
    g_tests.check(bytes(MemTag::Scripting) > 0,
                  "the Lua VM reports what it allocates");
    pipeline.teardown();
  }
  for (std::size_t i = 0U; i < 4U; ++i) {
    if (bytes(pooled[i]) != before[i]) {
      std::fprintf(stderr,
                   "  %s holds %lld bytes after teardown, %lld before\n",
                   engine::core::mem_tag_name(pooled[i]),
                   static_cast<long long>(bytes(pooled[i])),
                   static_cast<long long>(before[i]));
    }
    g_tests.check(bytes(pooled[i]) == before[i],
                  "teardown gives back every byte the pipeline reported");
  }
  engine::shutdown();
  return g_tests.finish("pipeline memory report tests");
}
