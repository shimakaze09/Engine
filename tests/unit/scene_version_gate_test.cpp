// Pins the scene loader's schema-version refusal (issue #369): load_scene is
// what stops a scene written against any revision but this build's — newer,
// superseded, or a malformed version field — from loading as the subset this
// build recognizes and later being resaved over the author's file as that
// reduction. The gate's policy helper is covered from the prefab side; this
// suite pins the scene wiring itself: every refused document returns false
// and leaves the destination World's entities byte-for-byte reachable, and
// the one accepted revision still commits.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

// One authored entity marks the destination World; a refused load must keep
// it alive and findable, an accepted load of an empty scene replaces it.
constexpr const char *kSentinelName = "VersionGateSentinel";

bool populate_world(engine::runtime::World &world) noexcept {
  const engine::runtime::Entity entity = world.create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return false;
  }
  engine::runtime::NameComponent name{};
  std::snprintf(name.name, sizeof(name.name), "%s", kSentinelName);
  return world.add_name_component(entity, name);
}

struct VersionCase final {
  const char *json;
  bool accepted;
};

// Revision 6 is the one this build reads. Every other value is refused,
// superseded revisions included: the project is unreleased, so the tree is
// migrated once per format change and no dual-read layer is kept. Reading a
// superseded revision through this reader would drop the fields it no
// longer knows in silence, and the next save would make that permanent. An
// absent key names no revision and is refused with the rest.
constexpr VersionCase kCases[] = {
    {"{\"version\":6,\"entities\":[]}", true},
    {"{\"version\":5,\"entities\":[]}", false},
    {"{\"version\":4,\"entities\":[]}", false},
    {"{\"version\":3,\"entities\":[]}", false},
    {"{\"version\":2,\"entities\":[]}", false},
    {"{\"version\":1,\"entities\":[]}", false},
    {"{\"entities\":[]}", false},
    {"{\"version\":7,\"entities\":[]}", false},
    {"{\"version\":999,\"entities\":[]}", false},
    {"{\"version\":0,\"entities\":[]}", false},
    {"{\"version\":-1,\"entities\":[]}", false},
    {"{\"version\":1.5,\"entities\":[]}", false},
    {"{\"version\":4294967296,\"entities\":[]}", false},
    {"{\"version\":\"2\",\"entities\":[]}", false},
    {"{\"version\":true,\"entities\":[]}", false},
    {"{\"version\":null,\"entities\":[]}", false},
    {"{\"version\":[2],\"entities\":[]}", false},
};

void run_version_case(const VersionCase &testCase) noexcept {
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    check(false, "world allocation");
    return;
  }
  if (!populate_world(*world)) {
    check(false, "populate world");
    return;
  }
  const std::size_t aliveBefore = world->alive_entity_count();

  const bool loaded = engine::runtime::load_scene(
      *world, testCase.json, std::strlen(testCase.json));

  char label[128] = {};
  std::snprintf(label, sizeof(label), "%s: %s",
                testCase.accepted ? "accepted" : "refused", testCase.json);
  check(loaded == testCase.accepted, label);

  if (testCase.accepted) {
    // A committed empty scene replaces the World's content entirely.
    check(world->alive_entity_count() == 0U, "accepted load replaced world");
    check(world->find_entity_by_name(kSentinelName) ==
              engine::runtime::kInvalidEntity,
          "accepted load removed the sentinel");
  } else {
    // A refusal must leave the destination untouched: same entity count and
    // the sentinel still reachable by name.
    check(world->alive_entity_count() == aliveBefore,
          "refused load left entity count unchanged");
    check(world->find_entity_by_name(kSentinelName) !=
              engine::runtime::kInvalidEntity,
          "refused load kept the sentinel entity");
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  for (const VersionCase &testCase : kCases) {
    run_version_case(testCase);
  }
  return g_tests.finish("scene version gate tests");
}
