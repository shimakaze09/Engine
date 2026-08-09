// Benchmarks ECS sparse-set iteration: warm-up passes plus a median over
// repeated samples so one-off scheduler jitter cannot skew the gate metric.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/sparse_set.h"
#include "engine/math/vec3.h"

namespace {

using Clock = std::chrono::high_resolution_clock;

struct BenchEntity final {
  std::uint32_t index = 0U;
};

struct BenchTransform final {
  engine::math::Vec3 position = engine::math::Vec3(0.0F, 0.0F, 0.0F);
};

struct BenchRigidBody final {
  engine::math::Vec3 velocity = engine::math::Vec3(0.0F, 0.0F, 0.0F);
};

constexpr std::size_t kEntityCount = 50000U;
constexpr std::size_t kWarmupPasses = 3U;
constexpr std::size_t kSamplePasses = 15U;

using TransformSet = engine::core::SparseSet<BenchEntity, BenchTransform,
                                             kEntityCount, kEntityCount>;
using RigidBodySet = engine::core::SparseSet<BenchEntity, BenchRigidBody,
                                             kEntityCount, kEntityCount>;

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

/// Times a single iteration pass over both component sets.
bool measure_pass(const TransformSet &transforms,
                  const RigidBodySet &rigidBodies, double *outIterMs,
                  std::size_t *outVisited) noexcept {
  if ((outIterMs == nullptr) || (outVisited == nullptr)) {
    return false;
  }

  const auto start = Clock::now();
  std::size_t visited = 0U;
  float checksum = 0.0F;

  for (std::size_t i = 0U; i < transforms.count(); ++i) {
    const BenchEntity entity = transforms.entity_at(i);
    const BenchRigidBody *body = rigidBodies.get_ptr(entity);
    if (body == nullptr) {
      continue;
    }

    const BenchTransform &transform = transforms.component_at(i);
    checksum += transform.position.x + transform.position.z + body->velocity.x;
    ++visited;
  }

  const auto end = Clock::now();
  if (checksum <= 0.0F) {
    return false;
  }

  *outIterMs = std::chrono::duration<double, std::milli>(end - start).count();
  *outVisited = visited;
  return visited == kEntityCount;
}

/// Populates the sets, warms up, then reports the median sampled pass time.
bool run_benchmark(double *outIterMs, std::size_t *outVisited) noexcept {
  if ((outIterMs == nullptr) || (outVisited == nullptr)) {
    return false;
  }

  auto transforms =
      std::unique_ptr<TransformSet>(new (std::nothrow) TransformSet());
  auto rigidBodies =
      std::unique_ptr<RigidBodySet>(new (std::nothrow) RigidBodySet());
  if (!transforms || !rigidBodies) {
    return false;
  }

  for (std::size_t i = 1U; i <= kEntityCount; ++i) {
    const BenchEntity entity{static_cast<std::uint32_t>(i)};

    BenchTransform transform{};
    transform.position =
        engine::math::Vec3(static_cast<float>(i), 1.0F, static_cast<float>(i));

    BenchRigidBody body{};
    body.velocity = engine::math::Vec3(1.0F, 0.0F, 1.0F);

    if (!transforms->add(entity, transform) ||
        !rigidBodies->add(entity, body)) {
      return false;
    }
  }

  double samples[kSamplePasses] = {};
  std::size_t visited = 0U;
  for (std::size_t pass = 0U; pass < (kWarmupPasses + kSamplePasses); ++pass) {
    double passMs = 0.0;
    if (!measure_pass(*transforms, *rigidBodies, &passMs, &visited)) {
      return false;
    }
    if (pass >= kWarmupPasses) {
      samples[pass - kWarmupPasses] = passMs;
    }
  }

  *outIterMs = median_of(samples, kSamplePasses);
  *outVisited = visited;
  return true;
}

/// Writes json data.
bool write_json(const char *path, double iterMs, std::size_t visited) noexcept {
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
                                 "  \"benchmark\": \"ecs\",\n"
                                 "  \"entities\": %zu,\n"
                                 "  \"warmup_passes\": %zu,\n"
                                 "  \"sample_passes\": %zu,\n"
                                 "  \"ecs_iterate_ms\": %.6f\n"
                                 "}\n",
                                 visited, kWarmupPasses, kSamplePasses, iterMs);

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

  double iterMs = 0.0;
  std::size_t visited = 0U;
  if (!run_benchmark(&iterMs, &visited)) {
    std::printf("FAIL: ecs benchmark execution\n");
    return 1;
  }

  if (!write_json(jsonOutPath, iterMs, visited)) {
    std::printf("FAIL: writing json output\n");
    return 1;
  }

  std::printf("[ecs_perf] entities=%zu warmup=%zu samples=%zu "
              "median_iterate_ms=%.6f\n",
              visited, kWarmupPasses, kSamplePasses, iterMs);
  return 0;
}
