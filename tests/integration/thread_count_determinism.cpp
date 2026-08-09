// Verifies thread count determinism behavior for the Engine test suite:
// scenario 1 integrates a large world of free bodies across randomized
// chunk splits; scenario 2 bounces a fast CCD mover between dynamic bodies
// while colliders are added and removed mid-run, so the CCD sweeps of the
// steps right after play start and after each add/remove run without a
// usable resolve snapshot and must stay bitwise identical across worker
// counts (audit N-08).

#include "engine/core/job_system.h"
#include "engine/runtime/physics_bridge.h"
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

// CCD scenario: independent corridor lanes, each with one mover fast
// enough to sweep and hit a heavy dynamic slab every step (2 m/step inside
// a 1.4 m corridor). Movers, their slabs, and filler blocks interleave in
// dense-index order so the randomized chunk splits put many mover sweeps
// and the slab integrations they observe into different chunk jobs.
constexpr std::size_t kCcdLaneCount = 24U;
constexpr std::size_t kCcdLaneFillerCount = 12U;
constexpr int kCcdSteps = 40;
constexpr int kCcdAddColliderStep = 20;
constexpr int kCcdRemoveColliderStep = 30;
constexpr float kCcdMoverSpeed = 120.0F;
constexpr std::size_t kCcdMinChunkSize = 16U;
constexpr std::size_t kCcdChunkRange = 48U;

struct UpdateJobData final {
  engine::runtime::World *world = nullptr;
  std::size_t startIndex = 0U;
  std::size_t count = 0U;
  float deltaSeconds = 0.0F;
};

struct ResolveJobData final {
  engine::runtime::World *world = nullptr;
  float deltaSeconds = 0.0F;
};

std::uint32_t next_random(std::uint32_t *state) {
  if (state == nullptr) {
    return 0U;
  }

  *state = (*state * 1664525U) + 1013904223U;
  return *state;
}

/// Runs the configured command, loop, or tool for update chunk.
void run_update_chunk(void *userData) noexcept {
  auto *jobData = static_cast<UpdateJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }

  static_cast<void>(jobData->world->update_transforms_range(
      jobData->startIndex, jobData->count, jobData->deltaSeconds));
  static_cast<void>(engine::runtime::step_physics_range(
      *jobData->world, jobData->startIndex, jobData->count,
      jobData->deltaSeconds));
}

/// Runs the serial collision resolve after all chunk jobs, mirroring the
/// production chunk -> resolve -> commit dependency chain.
void run_resolve(void *userData) noexcept {
  auto *jobData = static_cast<ResolveJobData *>(userData);
  if ((jobData == nullptr) || (jobData->world == nullptr)) {
    return;
  }

  static_cast<void>(engine::runtime::resolve_collisions(
      *jobData->world, jobData->deltaSeconds));
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

/// Runs one parallel step over randomized chunk splits; when resolveData is
/// non-null a resolve job is inserted between the chunk jobs and the commit,
/// mirroring the production fixed-step dependency chain.
bool parallel_update(engine::runtime::World *world, float deltaSeconds,
                     std::uint32_t randomSeed, std::size_t minChunkSize,
                     std::size_t chunkRange, ResolveJobData *resolveData) {
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
        minChunkSize + static_cast<std::size_t>(randomValue % chunkRange);

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

  engine::core::JobHandle resolveHandle{};
  if (resolveData != nullptr) {
    engine::core::Job resolveJob{};
    resolveJob.function = &run_resolve;
    resolveJob.data = resolveData;
    resolveHandle = engine::core::submit(resolveJob);
    if (!engine::core::is_valid_handle(resolveHandle) ||
        !engine::core::add_dependency(resolveHandle, commitHandle)) {
      world->end_frame_phase();
      static_cast<void>(engine::core::end_frame_graph());
      return false;
    }
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

    if (engine::core::is_valid_handle(resolveHandle) &&
        !engine::core::add_dependency(updateHandle, resolveHandle)) {
      world->end_frame_phase();
      static_cast<void>(engine::core::end_frame_graph());
      return false;
    }
  }

  engine::core::wait_all();

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

/// Runs the configured command, loop, or tool for with worker count.
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
                         seed + static_cast<std::uint32_t>(step * 17),
                         kMinChunkSize, kChunkRange, nullptr)) {
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

/// Adds one lane's fast mover at the world's next dense index.
bool add_ccd_mover(engine::runtime::World *world, float laneZ) {
  const engine::runtime::Entity mover = world->create_entity();
  if (mover == engine::runtime::kInvalidEntity) {
    return false;
  }

  engine::runtime::Transform moverTransform{};
  moverTransform.position = engine::math::Vec3(0.0F, 0.0F, laneZ);
  engine::runtime::Collider moverCollider{};
  moverCollider.shape = engine::runtime::ColliderShape::Sphere;
  moverCollider.halfExtents = engine::math::Vec3(0.1F, 0.1F, 0.1F);
  engine::runtime::RigidBody moverBody{};
  moverBody.inverseMass = 1.0F;
  moverBody.velocity = engine::math::Vec3(kCcdMoverSpeed, 0.0F, 0.0F);
  return world->add_transform(mover, moverTransform) &&
         world->add_collider(mover, moverCollider) &&
         world->add_rigid_body(mover, moverBody);
}

/// Adds one lane's block of distant filler bodies at the next dense
/// indices.
bool add_ccd_fillers(engine::runtime::World *world, float baseX,
                     engine::runtime::Entity *outRemovalEntity) {
  for (std::size_t i = 0U; i < kCcdLaneFillerCount; ++i) {
    const engine::runtime::Entity filler = world->create_entity();
    if (filler == engine::runtime::kInvalidEntity) {
      return false;
    }

    engine::runtime::Transform fillerTransform{};
    fillerTransform.position = engine::math::Vec3(
        baseX + (2.0F * static_cast<float>(i)), 0.0F, 0.0F);
    engine::runtime::Collider fillerCollider{};
    fillerCollider.shape = engine::runtime::ColliderShape::AABB;
    fillerCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
    engine::runtime::RigidBody fillerBody{};
    fillerBody.inverseMass = 1.0F;
    fillerBody.velocity = engine::math::Vec3(1.0F, 0.0F, 0.0F);
    if (!world->add_transform(filler, fillerTransform) ||
        !world->add_collider(filler, fillerCollider) ||
        !world->add_rigid_body(filler, fillerBody)) {
      return false;
    }

    if ((i == 5U) && (outRemovalEntity != nullptr)) {
      *outRemovalEntity = filler;
    }
  }

  return true;
}

/// Adds one lane's pair of heavy dynamic corridor slabs at the next dense
/// indices.
bool add_ccd_slabs(engine::runtime::World *world, float laneZ) {
  for (std::size_t side = 0U; side < 2U; ++side) {
    const engine::runtime::Entity slab = world->create_entity();
    if (slab == engine::runtime::kInvalidEntity) {
      return false;
    }

    engine::runtime::Transform slabTransform{};
    slabTransform.position =
        engine::math::Vec3((side == 0U) ? -1.0F : 1.0F, 0.0F, laneZ);
    engine::runtime::Collider slabCollider{};
    slabCollider.shape = engine::runtime::ColliderShape::AABB;
    slabCollider.halfExtents = engine::math::Vec3(0.2F, 0.5F, 0.5F);
    engine::runtime::RigidBody slabBody{};
    slabBody.inverseMass = 0.01F;
    if (!world->add_transform(slab, slabTransform) ||
        !world->add_collider(slab, slabCollider) ||
        !world->add_rigid_body(slab, slabBody)) {
      return false;
    }
  }

  return true;
}

/// Builds the CCD lane world: per lane a fast sphere corralled between two
/// heavy dynamic slabs, with a filler block between the mover and its
/// slabs so their dense indices usually land in different chunk jobs. All
/// bodies share default gravity so every dynamic body's velocity is
/// rewritten by its own chunk each step.
bool populate_ccd_world(engine::runtime::World *world,
                        engine::runtime::Entity *outRemovalEntity) {
  if ((world == nullptr) || (outRemovalEntity == nullptr)) {
    return false;
  }

  for (std::size_t lane = 0U; lane < kCcdLaneCount; ++lane) {
    const float laneZ = 3.0F * static_cast<float>(lane);
    const float fillerBaseX =
        1000.0F + (100.0F * static_cast<float>(lane));
    if (!add_ccd_mover(world, laneZ) ||
        !add_ccd_fillers(world, fillerBaseX,
                         (lane == 0U) ? outRemovalEntity : nullptr) ||
        !add_ccd_slabs(world, laneZ)) {
      return false;
    }
  }

  return true;
}

/// Folds one float's bit pattern into an FNV-1a style hash.
void fold_float_bits(std::uint64_t *hash, float value) {
  std::uint32_t bits = 0U;
  std::memcpy(&bits, &value, sizeof(bits));
  *hash ^= static_cast<std::uint64_t>(bits);
  *hash *= 1099511628211ULL;
}

/// Hashes every transform position and rigid-body velocity bitwise so any
/// CCD time-of-impact or reflection divergence is visible.
std::uint64_t hash_ccd_world_state(engine::runtime::World *world) {
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
    hash ^= static_cast<std::uint64_t>(entities[i].index);
    hash *= 1099511628211ULL;
    fold_float_bits(&hash, transforms[i].position.x);
    fold_float_bits(&hash, transforms[i].position.y);
    fold_float_bits(&hash, transforms[i].position.z);
  }

  const std::size_t bodyCount = world->rigid_body_count();
  const engine::runtime::Entity *bodyEntities = nullptr;
  engine::runtime::RigidBody *bodies = nullptr;
  if ((bodyCount > 0U) &&
      world->get_rigid_body_range(0U, bodyCount, &bodyEntities, &bodies)) {
    for (std::size_t i = 0U; i < bodyCount; ++i) {
      hash ^= static_cast<std::uint64_t>(bodyEntities[i].index);
      hash *= 1099511628211ULL;
      fold_float_bits(&hash, bodies[i].velocity.x);
      fold_float_bits(&hash, bodies[i].velocity.y);
      fold_float_bits(&hash, bodies[i].velocity.z);
    }
  }

  return hash;
}

/// Runs the CCD scenario with one worker count: the first step and the
/// steps right after the mid-run collider add/remove sweep without a usable
/// snapshot while a guaranteed CCD hit is in flight.
bool run_ccd_with_worker_count(std::uint32_t workerCount,
                               std::uint64_t *outHash) {
  if (!engine::core::initialize_job_system(workerCount)) {
    return false;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    engine::core::shutdown_job_system();
    return false;
  }

  engine::runtime::Entity removalEntity{};
  if (!populate_ccd_world(world.get(), &removalEntity)) {
    engine::core::shutdown_job_system();
    return false;
  }

  const std::uint32_t seed = 0x5EEDCCD1U;
  for (int step = 0; step < kCcdSteps; ++step) {
    if (step == kCcdAddColliderStep) {
      const engine::runtime::Entity lateWall = world->create_entity();
      engine::runtime::Transform lateTransform{};
      lateTransform.position = engine::math::Vec3(2000.0F, 0.0F, 0.0F);
      engine::runtime::Collider lateCollider{};
      lateCollider.shape = engine::runtime::ColliderShape::AABB;
      lateCollider.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
      if ((lateWall == engine::runtime::kInvalidEntity) ||
          !world->add_transform(lateWall, lateTransform) ||
          !world->add_collider(lateWall, lateCollider)) {
        engine::core::shutdown_job_system();
        return false;
      }
    }

    if ((step == kCcdRemoveColliderStep) &&
        !world->remove_collider(removalEntity)) {
      engine::core::shutdown_job_system();
      return false;
    }

    ResolveJobData resolveData{};
    resolveData.world = world.get();
    resolveData.deltaSeconds = kStepSeconds;
    if (!parallel_update(world.get(), kStepSeconds,
                         seed + static_cast<std::uint32_t>(step * 29),
                         kCcdMinChunkSize, kCcdChunkRange, &resolveData)) {
      engine::core::shutdown_job_system();
      return false;
    }
  }

  const std::uint64_t hash = hash_ccd_world_state(world.get());
  engine::core::shutdown_job_system();
  if (hash == 0U) {
    return false;
  }

  if (outHash != nullptr) {
    *outHash = hash;
  }

  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  constexpr std::array<std::uint32_t, 4U> kWorkerConfigs = {1U, 2U, 4U, 8U};

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

  if (maxObservedWorkers < 8U) {
    return 3;
  }

  std::uint64_t ccdReferenceHash = 0U;
  for (std::size_t i = 0U; i < kWorkerConfigs.size(); ++i) {
    std::uint64_t runHash = 0U;
    if (!run_ccd_with_worker_count(kWorkerConfigs[i], &runHash)) {
      return 4;
    }

    if (i == 0U) {
      ccdReferenceHash = runHash;
    } else if (runHash != ccdReferenceHash) {
      return 5;
    }
  }

  return 0;
}
