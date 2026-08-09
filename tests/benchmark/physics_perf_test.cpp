// Benchmarks the fixed-step physics pipeline: one discarded warm-up
// simulation plus a median over repeated full-simulation samples so a
// single jittery run cannot skew the per-step gate metric.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

using Clock = std::chrono::high_resolution_clock;

constexpr std::size_t kBodyCount = 1000U;
constexpr int kSimulationSteps = 120;
constexpr std::size_t kWarmupRuns = 1U;
constexpr std::size_t kSampleRuns = 5U;

/// Parses text into the engine representation for json out.
bool parse_json_out(int argc, char **argv, const char **outPath) noexcept {
  if (outPath == nullptr) {
    return false;
  }
  *outPath = nullptr;

  for (int i = 1; i < argc; ++i) {
    if ((std::strcmp(argv[i], "--json-out") == 0) && ((i + 1) < argc)) {
      *outPath = argv[i + 1];
      return true;
    }
  }
  return true;
}

/// Returns the median of count samples, sorting them in place.
double median_of(double *samples, std::size_t count) noexcept {
  std::sort(samples, samples + count);
  const std::size_t mid = count / 2U;
  if ((count % 2U) == 0U) {
    return 0.5 * (samples[mid - 1U] + samples[mid]);
  }
  return samples[mid];
}

bool setup_world(engine::runtime::World *world) noexcept {
  if (world == nullptr) {
    return false;
  }

  world->end_frame_phase();

  for (std::size_t i = 0U; i < kBodyCount; ++i) {
    const engine::runtime::Entity entity = world->create_entity();
    if (entity == engine::runtime::kInvalidEntity) {
      return false;
    }

    engine::runtime::Transform transform{};
    transform.position = engine::math::Vec3(
        static_cast<float>(i % 50U), 10.0F + static_cast<float>(i / 50U), 0.0F);

    engine::runtime::RigidBody body{};
    body.inverseMass = 1.0F;

    engine::runtime::Collider collider{};
    collider.halfExtents = engine::math::Vec3(0.25F, 0.25F, 0.25F);

    if (!world->add_transform(entity, transform) ||
        !world->add_rigid_body(entity, body) ||
        !world->add_collider(entity, collider)) {
      return false;
    }
  }

  return true;
}

/// Times one full fresh-world simulation and reports its mean per-step ms.
bool measure_run(double *outStepMs) noexcept {
  if (outStepMs == nullptr) {
    return false;
  }

  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (!world) {
    return false;
  }

  if (!setup_world(world.get())) {
    return false;
  }

  const auto start = Clock::now();

  for (int step = 0; step < kSimulationSteps; ++step) {
    world->begin_update_phase();
    if (!engine::runtime::step_physics(*world, 1.0F / 60.0F)) {
      return false;
    }
    if (!engine::runtime::resolve_collisions(*world)) {
      return false;
    }
    world->commit_update_phase();
    world->begin_render_prep_phase();
    world->end_frame_phase();
  }

  const auto end = Clock::now();
  const double totalMs =
      std::chrono::duration<double, std::milli>(end - start).count();

  *outStepMs = totalMs / static_cast<double>(kSimulationSteps);
  return true;
}

/// Discards warm-up runs, then reports the median sampled per-step time.
bool run_benchmark(double *outStepMs) noexcept {
  if (outStepMs == nullptr) {
    return false;
  }

  double samples[kSampleRuns] = {};
  for (std::size_t run = 0U; run < (kWarmupRuns + kSampleRuns); ++run) {
    double runStepMs = 0.0;
    if (!measure_run(&runStepMs)) {
      return false;
    }
    if (run >= kWarmupRuns) {
      samples[run - kWarmupRuns] = runStepMs;
    }
  }

  *outStepMs = median_of(samples, kSampleRuns);
  return true;
}

/// Writes json data.
bool write_json(const char *path, double stepMs) noexcept {
  if (path == nullptr) {
    return true;
  }

  FILE *file = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }

  const int wrote = std::fprintf(file,
                                 "{\n"
                                 "  \"benchmark\": \"physics\",\n"
                                 "  \"bodies\": %zu,\n"
                                 "  \"warmup_runs\": %zu,\n"
                                 "  \"sample_runs\": %zu,\n"
                                 "  \"physics_step_ms\": %.6f\n"
                                 "}\n",
                                 kBodyCount, kWarmupRuns, kSampleRuns, stepMs);

  std::fclose(file);
  return wrote > 0;
}

} // namespace

/// Runs this executable or test program.
int main(int argc, char **argv) {
  const char *jsonOutPath = nullptr;
  if (!parse_json_out(argc, argv, &jsonOutPath)) {
    std::printf("FAIL: invalid arguments\n");
    return 1;
  }

  double stepMs = 0.0;
  if (!run_benchmark(&stepMs)) {
    std::printf("FAIL: physics benchmark execution\n");
    return 1;
  }

  if (!write_json(jsonOutPath, stepMs)) {
    std::printf("FAIL: writing json output\n");
    return 1;
  }

  std::printf("[physics_perf] bodies=%zu warmup_runs=%zu sample_runs=%zu "
              "median_step_ms=%.6f\n",
              kBodyCount, kWarmupRuns, kSampleRuns, stepMs);
  return 0;
}
