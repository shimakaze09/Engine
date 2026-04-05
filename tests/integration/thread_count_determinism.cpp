#include "engine/core/job_system.h"
#include "engine/physics/physics.h"
#include "engine/runtime/world.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace {

constexpr std::size_t kEntityCount = 10000U;
constexpr int kUpdateSteps = 120;
constexpr float kStepSeconds = 1.0F / 60.0F;
constexpr std::size_t kMinChunkSize = 64U;
constexpr std::size_t kChunkRange = 384U;
constexpr std::size_t kMaxJobs = 512U;

struct UpdateJobData final {
  engine::runtime::World *world = nullptr;
  std::size_t startIndex = 0U;
  std::size_t count = 0U;
  float deltaSeconds = 0.0F;
};

std::uint32_t next_random(std::uint32_t *state) {
  if (state == nullptr) {
    return 0U;
  }

  *state = (*state * 1664525U) + 1013904223U;
  return *state;
}

void run_update_chunk(void *userData) noexcept {
  auto *jobData = static_cast<UpdateJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }

  static_cast<void>(jobData->world->update_transforms_range(
      jobData->startIndex, jobData->count, jobData->deltaSeconds));
  static_cast<void>(engine::physics::step_physics_range(
      *jobData->world, jobData->startIndex, jobData->count,
      jobData->deltaSeconds));
}

bool populate_world(engine::runtime::World *world,
                    engine::runtime::Entity *outFirstEntity) {
  if (world == nullptr) {
    return false;
  }

  bool firstSet = false;
  for (std::size_t i = 0U; i < kEntityCount; ++i) {
    const engine::runtime::Entity entity = world->create_entity();
    if (entity == engine::runtime::kInvalidEntity) {
      return false;
    }

    if (!firstSet && (outFirstEntity != nullptr)) {
      *outFirstEntity = entity;
      firstSet = true;
    }

    engine::runtime::Transform transform{};
    transform.position = engine::math::Vec3(static_cast<float>(i), 0.0F, 0.0F);

    engine::runtime::RigidBody rigidBody{};
    rigidBody.velocity = engine::math::Vec3(1.0F, 0.0F, 0.0F);
    rigidBody.inverseMass = 1.0F;

    if (!world->add_transform(entity, transform)) {
      return false;
    }

    if (!world->add_rigid_body(entity, rigidBody)) {
      return false;
    }
  }

  return firstSet;
}

bool parallel_update(engine::runtime::World *world, float deltaSeconds,
                     std::uint32_t randomSeed) {
  if (world == nullptr) {
    return false;
  }

  if (!engine::core::begin_frame_graph()) {
    return false;
  }

  std::array<UpdateJobData, kMaxJobs> jobs{};
  std::array<std::size_t, kMaxJobs> order{};
  std::size_t jobCount = 0U;

  const std::size_t transformCount = world->transform_count();
  std::size_t cursor = 0U;
  while ((cursor < transformCount) && (jobCount < jobs.size())) {
    const std::uint32_t randomValue = next_random(&randomSeed);
    std::size_t chunkSize =
        kMinChunkSize + static_cast<std::size_t>(randomValue % kChunkRange);

    if ((cursor + chunkSize) > transformCount) {
      chunkSize = transformCount - cursor;
    }

    jobs[jobCount].world = world;
    jobs[jobCount].startIndex = cursor;
    jobs[jobCount].count = chunkSize;
    jobs[jobCount].deltaSeconds = deltaSeconds;
    order[jobCount] = jobCount;

    cursor += chunkSize;
    ++jobCount;
  }

  if (cursor != transformCount) {
    static_cast<void>(engine::core::end_frame_graph());
    return false;
  }

  for (std::size_t i = jobCount; i > 1U; --i) {
    const std::size_t swapIndex =
        static_cast<std::size_t>(next_random(&randomSeed) % i);
    const std::size_t tmp = order[i - 1U];
    order[i - 1U] = order[swapIndex];
    order[swapIndex] = tmp;
  }

  world->begin_update_phase();

  engine::core::Job commitJob{};
  commitJob.function = [](void *context) noexcept {
    auto *worldContext = static_cast<engine::runtime::World *>(context);
    if (worldContext != nullptr) {
      worldContext->commit_update_phase();
    }
  };
  commitJob.data = world;

  const engine::core::JobHandle commitHandle = engine::core::submit(commitJob);
  if (!engine::core::is_valid_handle(commitHandle)) {
    world->end_frame_phase();
    static_cast<void>(engine::core::end_frame_graph());
    return false;
  }

  for (std::size_t i = 0U; i < jobCount; ++i) {
    const std::size_t jobIndex = order[i];

    engine::core::Job job{};
    job.function = &run_update_chunk;
    job.data = &jobs[jobIndex];
    const engine::core::JobHandle updateHandle = engine::core::submit(job);
    if (!engine::core::is_valid_handle(updateHandle)) {
      world->end_frame_phase();
      static_cast<void>(engine::core::end_frame_graph());
      return false;
    }

    if (!engine::core::add_dependency(updateHandle, commitHandle)) {
      world->end_frame_phase();
      static_cast<void>(engine::core::end_frame_graph());
      return false;
    }
  }

  engine::core::wait(commitHandle);

  if (!engine::core::end_frame_graph()) {
    world->end_frame_phase();
    return false;
  }

  world->begin_render_prep_phase();
  world->end_frame_phase();
  return true;
}

std::uint64_t hash_world_state(engine::runtime::World *world) {
  if (world == nullptr) {
    return 0U;
  }

  const std::size_t transformCount = world->transform_count();
  const engine::runtime::Entity *entities = nullptr;
  const engine::runtime::Transform *transforms = nullptr;

  world->begin_render_prep_phase();
  const bool readable =
      world->read_transform_range(0U, transformCount, &entities, &transforms);
  world->end_frame_phase();
  if (!readable) {
    return 0U;
  }

  std::uint64_t hash = 1469598103934665603ULL;
  for (std::size_t i = 0U; i < transformCount; ++i) {
    std::uint32_t xBits = 0U;
    std::memcpy(&xBits, &transforms[i].position.x, sizeof(xBits));

    hash ^= static_cast<std::uint64_t>(entities[i].index);
    hash *= 1099511628211ULL;
    hash ^= static_cast<std::uint64_t>(entities[i].generation);
    hash *= 1099511628211ULL;
    hash ^= static_cast<std::uint64_t>(xBits);
    hash *= 1099511628211ULL;
  }

  return hash;
}

bool run_with_worker_count(std::uint32_t workerCount, std::uint64_t *outHash,
                           std::uint32_t *outActualWorkers) {
  if (!engine::core::initialize_job_system(workerCount)) {
    return false;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    engine::core::shutdown_job_system();
    return false;
  }

  engine::runtime::Entity firstEntity{};
  if (!populate_world(world.get(), &firstEntity)) {
    engine::core::shutdown_job_system();
    return false;
  }

  if ((world->transform_count() != kEntityCount) ||
      (world->rigid_body_count() != kEntityCount)) {
    engine::core::shutdown_job_system();
    return false;
  }

  const std::uint32_t seed = 0xB16B00B5U;
  for (int step = 0; step < kUpdateSteps; ++step) {
    if (!parallel_update(world.get(), kStepSeconds,
                         seed + static_cast<std::uint32_t>(step * 17))) {
      engine::core::shutdown_job_system();
      return false;
    }
  }

  const std::uint64_t hash = hash_world_state(world.get());
  if (hash == 0U) {
    engine::core::shutdown_job_system();
    return false;
  }

  if (outHash != nullptr) {
    *outHash = hash;
  }

  if (outActualWorkers != nullptr) {
    *outActualWorkers = engine::core::worker_count();
  }

  engine::core::shutdown_job_system();
  return true;
}

} // namespace

int main() {
  constexpr std::array<std::uint32_t, 3U> kWorkerConfigs = {0U, 1U, 2U};

  std::uint64_t referenceHash = 0U;
  std::uint32_t maxObservedWorkers = 0U;

  for (std::size_t i = 0U; i < kWorkerConfigs.size(); ++i) {
    std::uint64_t runHash = 0U;
    std::uint32_t observedWorkers = 0U;
    if (!run_with_worker_count(kWorkerConfigs[i], &runHash, &observedWorkers)) {
      return 1;
    }

    if (observedWorkers > maxObservedWorkers) {
      maxObservedWorkers = observedWorkers;
    }

    if (i == 0U) {
      referenceHash = runHash;
    } else if (runHash != referenceHash) {
      return 2;
    }
  }

  if (maxObservedWorkers < 2U) {
    return 3;
  }

  return 0;
}
