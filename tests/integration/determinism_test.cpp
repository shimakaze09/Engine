// Determinism proof through the production pipeline: two runs of one scene
// through engine::bootstrap and EnginePipeline, one per worker count, fed
// the same exact frame delta, must end on the same World::state_hash. The
// hash is printed once for CI, which compares it across platforms and
// build configurations.

#include <cstdint>
#include <cstdio>
#include <filesystem>

#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

constexpr std::size_t kBodyCount = 128U;
constexpr std::uint32_t kFrameCount = 240U;
constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr const char *kMainScriptPath = "determinism_main.lua";

/// Walks upward from the current path until the bundled assets are found.
bool set_working_directory_with_assets() noexcept {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    std::error_code ec{};
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      continue;
    }
    if (std::filesystem::exists(normalized / "assets/main.lua", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

/// The run's main script does nothing, so the hash covers the engine's own
/// systems and not the demo script's behavior.
bool write_main_script() noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kMainScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kMainScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const char *contents = "-- Empty on purpose: the determinism run has no gameplay.\n";
  const std::size_t length = std::char_traits<char>::length(contents);
  const bool ok = (std::fwrite(contents, 1U, length, file) == length);
  std::fclose(file);
  return ok;
}

/// A ground slab and a grid of spheres that fall onto it and each other,
/// so contacts, sleeping and collision pairs all enter the hash.
bool populate_world(engine::runtime::World &world,
                    engine::runtime::Entity *outFirstBody) noexcept {
  engine::runtime::Transform groundTransform{};
  groundTransform.position = engine::math::Vec3(0.0F, -1.0F, 0.0F);
  const engine::runtime::Entity ground =
      world.create_scene_object(groundTransform);
  engine::runtime::Collider groundCollider{};
  groundCollider.shape = engine::runtime::ColliderShape::AABB;
  groundCollider.halfExtents = engine::math::Vec3(64.0F, 1.0F, 64.0F);
  if ((ground == engine::runtime::kInvalidEntity) ||
      !world.add_collider(ground, groundCollider)) {
    return false;
  }

  for (std::size_t i = 0U; i < kBodyCount; ++i) {
    engine::runtime::Transform transform{};
    transform.position = engine::math::Vec3(
        static_cast<float>(i % 16U) * 1.5F - 12.0F,
        2.0F + static_cast<float>(i / 16U) * 1.5F,
        static_cast<float>((i * 7U) % 5U) * 0.9F - 2.0F);
    const engine::runtime::Entity entity = world.create_scene_object(transform);
    if (entity == engine::runtime::kInvalidEntity) {
      return false;
    }
    engine::runtime::RigidBody body{};
    body.inverseMass = 1.0F;
    body.velocity = engine::math::Vec3(0.1F * static_cast<float>(i % 3U),
                                       0.0F, 0.05F);
    body.angularVelocity = engine::math::Vec3(0.0F, 0.01F, 0.0F);
    engine::runtime::Collider collider{};
    collider.shape = engine::runtime::ColliderShape::Sphere;
    collider.halfExtents = engine::math::Vec3(0.4F, 0.4F, 0.4F);
    if (!world.add_rigid_body(entity, body) ||
        !world.add_collider(entity, collider)) {
      return false;
    }
    if (i == 0U) {
      *outFirstBody = entity;
    }
  }
  return true;
}

/// Boots the engine headless with `workerThreads` workers, runs the scene
/// for kFrameCount frames of exactly one fixed step each, and reports the
/// world's state hash and the first body's final height.
bool run_pipeline(std::uint32_t workerThreads, std::uint64_t *outHash,
                  float *outFirstBodyY,
                  engine::runtime::StateHashSections *outSections) noexcept {
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = workerThreads;
  config.mainScriptPath = kMainScriptPath;
  if (!engine::bootstrap(config)) {
    std::printf("FAIL: bootstrap with %u workers\n", workerThreads);
    return false;
  }

  bool ok = false;
  {
    engine::EnginePipeline pipeline;
    engine::runtime::Entity firstBody = engine::runtime::kInvalidEntity;
    engine::runtime::World *world = nullptr;
    if (pipeline.initialize(0U) && ((world = pipeline.world()) != nullptr)) {
      engine::runtime::reset_world(*world);
      ok = populate_world(*world, &firstBody) &&
           pipeline.set_frame_delta_override(kFrameSeconds);
    }
    for (std::uint32_t frame = 0U; ok && (frame < kFrameCount); ++frame) {
      ok = pipeline.execute_frame();
    }
    engine::runtime::Transform transform{};
    if (ok && world->get_transform(firstBody, &transform)) {
      *outHash = world->state_hash(outSections);
      *outFirstBodyY = transform.position.y;
    } else {
      ok = false;
    }
    pipeline.teardown();
  }
  engine::shutdown();
  return ok;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets() || !write_main_script()) {
    std::printf("FAIL: determinism test setup\n");
    return 1;
  }

  std::uint64_t hashA = 0U;
  std::uint64_t hashB = 0U;
  float firstBodyYA = 0.0F;
  float firstBodyYB = 0.0F;
  engine::runtime::StateHashSections sectionsA{};
  engine::runtime::StateHashSections sectionsB{};
  const bool ran = run_pipeline(1U, &hashA, &firstBodyYA, &sectionsA) &&
                   run_pipeline(4U, &hashB, &firstBodyYB, &sectionsB);
  static_cast<void>(std::remove(kMainScriptPath));
  if (!ran) {
    std::printf("FAIL: determinism pipeline run\n");
    return 1;
  }

  // The bodies start two units up and must have fallen; otherwise equal
  // hashes would only prove that nothing simulated.
  if (!(firstBodyYA < 1.0F)) {
    std::printf("FAIL: the scene did not simulate (first body y=%g)\n",
                static_cast<double>(firstBodyYA));
    return 2;
  }

  if (hashA != hashB) {
    std::printf("FAIL: state hash differs between 1 and 4 workers "
                "a=%llu b=%llu\n",
                static_cast<unsigned long long>(hashA),
                static_cast<unsigned long long>(hashB));
    return 3;
  }

  // The running fold after each section, so two platforms that disagree
  // can see which section diverged first; the final line is what CI reads.
  std::printf("[determinism] fold entities=%llu transforms=%llu "
              "bodies=%llu physics=%llu timers=%llu animation=%llu\n",
              static_cast<unsigned long long>(sectionsA.entities),
              static_cast<unsigned long long>(sectionsA.transforms),
              static_cast<unsigned long long>(sectionsA.rigidBodies),
              static_cast<unsigned long long>(sectionsA.physics),
              static_cast<unsigned long long>(sectionsA.timers),
              static_cast<unsigned long long>(sectionsA.animation));
  std::printf("[determinism] hash=%llu\n",
              static_cast<unsigned long long>(hashA));
  return 0;
}
