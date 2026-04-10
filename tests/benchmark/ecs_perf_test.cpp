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

bool run_benchmark(double *outIterMs, std::size_t *outVisited) noexcept {
  if ((outIterMs == nullptr) || (outVisited == nullptr)) {
    return false;
  }

  using TransformSet = engine::core::SparseSet<BenchEntity, BenchTransform,
                                                kEntityCount, kEntityCount>;
  using RigidBodySet = engine::core::SparseSet<BenchEntity, BenchRigidBody,
                                                kEntityCount, kEntityCount>;

  auto transforms = std::unique_ptr<TransformSet>(new (std::nothrow) TransformSet());
  auto rigidBodies = std::unique_ptr<RigidBodySet>(new (std::nothrow) RigidBodySet());
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

    if (!transforms->add(entity, transform) || !rigidBodies->add(entity, body)) {
      return false;
    }
  }

  const auto start = Clock::now();
  std::size_t visited = 0U;
  float checksum = 0.0F;

  for (std::size_t i = 0U; i < transforms->count(); ++i) {
    const BenchEntity entity = transforms->entity_at(i);
    const BenchRigidBody *body = rigidBodies->get_ptr(entity);
    if (body == nullptr) {
      continue;
    }

    const BenchTransform &transform = transforms->component_at(i);
    checksum += transform.position.x + transform.position.z + body->velocity.x;
    ++visited;
  }

  const auto end = Clock::now();
  const double iterMs =
      std::chrono::duration<double, std::milli>(end - start).count();

  if (checksum <= 0.0F) {
    return false;
  }

  *outIterMs = iterMs;
  *outVisited = visited;
  return visited == kEntityCount;
}

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

  const int wrote = std::fprintf(
      file,
      "{\n"
      "  \"benchmark\": \"ecs\",\n"
      "  \"entities\": %zu,\n"
      "  \"ecs_iterate_ms\": %.6f\n"
      "}\n",
      visited, iterMs);

  std::fclose(file);
  return wrote > 0;
}

} // namespace

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

  std::printf("[ecs_perf] entities=%zu iterate_ms=%.6f\n", visited, iterMs);
  return 0;
}
