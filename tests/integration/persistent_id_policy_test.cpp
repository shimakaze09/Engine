// Authored entities take scattered persistent ids, so two authors who each
// add an entity to one scene on separate branches do not both take the
// next id (the merged scene would not load: a repeated persistentId
// refuses the document), and an id a saved scene once held is not handed
// out again after a reload. A play session still draws sequentially, so
// what a session spawns stays deterministic.
//
// Drives save_scene / load_scene (the editor's own save, open and Play
// restore) and the production pipeline's play session.

#include "../asset_root.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include <array>
#include <cstdio>
#include <memory>
#include <new>

namespace {

namespace rt = engine::runtime;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

constexpr std::size_t kBufferBytes = 64U * 1024U;

std::unique_ptr<rt::World> make_world() {
  return std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
}

rt::PersistentIdPolicy scattered(std::uint64_t seed) noexcept {
  rt::PersistentIdPolicy policy{};
  policy.source = rt::PersistentIdSource::Scattered;
  policy.streamState = seed;
  return policy;
}

/// The persistent id of a new scene object in `world`.
rt::PersistentId author_one(rt::World &world) noexcept {
  return world.persistent_id(world.create_scene_object());
}

/// Two authors open the same three-entity scene on separate machines and
/// each add one entity: with scattered ids they differ, where sequential
/// ids both come out 4.
void check_two_branches(const char *base, std::size_t baseSize) {
  std::unique_ptr<rt::World> mine = make_world();
  std::unique_ptr<rt::World> theirs = make_world();
  if ((mine == nullptr) || (theirs == nullptr)) {
    CHECK(false, "allocate the worlds");
    return;
  }
  mine->set_persistent_id_policy(scattered(0x1234U));
  theirs->set_persistent_id_policy(scattered(0x98765U));
  CHECK(rt::load_scene(*mine, base, baseSize) &&
            rt::load_scene(*theirs, base, baseSize),
        "both authors open the base scene");
  const rt::PersistentId myId = author_one(*mine);
  const rt::PersistentId theirId = author_one(*theirs);
  CHECK((myId != rt::kInvalidPersistentId) &&
            (theirId != rt::kInvalidPersistentId) && (myId != theirId),
        "two authors adding an entity take different ids");
  for (rt::PersistentId taken = 1U; taken <= 4U; ++taken) {
    CHECK((myId != taken) && (theirId != taken),
          "an authored id is not the next small integer");
  }

  // The merge: both additions in one document, which loads.
  CHECK(mine->create_scene_object_with_persistent_id(theirId) !=
            rt::kInvalidEntity,
        "their entity joins mine under its own id");
  static std::array<char, kBufferBytes> merged{};
  std::size_t mergedSize = 0U;
  std::unique_ptr<rt::World> reopened = make_world();
  CHECK((reopened != nullptr) &&
            rt::save_scene(*mine, merged.data(), merged.size(), &mergedSize) &&
            rt::load_scene(*reopened, merged.data(), mergedSize) &&
            (reopened->alive_entity_count() == 5U),
        "the merged scene loads with both additions");

  // The contract the scattered source replaces, kept as the control.
  std::unique_ptr<rt::World> first = make_world();
  std::unique_ptr<rt::World> second = make_world();
  if ((first != nullptr) && (second != nullptr) &&
      rt::load_scene(*first, base, baseSize) &&
      rt::load_scene(*second, base, baseSize)) {
    CHECK((author_one(*first) == 4U) && (author_one(*second) == 4U),
          "sequential ids collide, which is why authoring does not use them");
  }
}

/// A deleted entity's id does not come back after a save and reload, and
/// the load keeps the World's policy.
void check_freed_id_after_reload() {
  std::unique_ptr<rt::World> world = make_world();
  if (world == nullptr) {
    CHECK(false, "allocate the world");
    return;
  }
  world->set_persistent_id_policy(scattered(0xABCDEFU));
  const rt::Entity a = world->create_scene_object();
  const rt::Entity b = world->create_scene_object();
  const rt::Entity c = world->create_scene_object();
  const rt::PersistentId freed = world->persistent_id(b);
  CHECK((a != rt::kInvalidEntity) && (c != rt::kInvalidEntity) &&
            world->destroy_entity(b),
        "author three entities and delete the middle one");

  static std::array<char, kBufferBytes> saved{};
  std::size_t savedSize = 0U;
  CHECK(rt::save_scene(*world, saved.data(), saved.size(), &savedSize) &&
            rt::load_scene(*world, saved.data(), savedSize),
        "save and reload the scene");
  CHECK(world->persistent_id_policy().source ==
            rt::PersistentIdSource::Scattered,
        "the reload keeps the World's id policy");
  bool reused = false;
  for (int i = 0; i < 256; ++i) {
    reused = reused || (author_one(*world) == freed);
  }
  CHECK(!reused, "the deleted entity's id is not handed out again");
}

// --- The play session draws sequentially and gives the policy back ---

rt::World *g_world = nullptr;
bool g_playing = false;
void capture_world(rt::World *world) noexcept { g_world = world; }
bool bridge_is_playing() noexcept { return g_playing; }
bool bridge_is_paused() noexcept { return false; }

void check_play_session() {
  rt::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  rt::set_editor_bridge(&bridge);
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = 1U;
  if (!engine::bootstrap(config)) {
    CHECK(false, "bootstrap");
    rt::set_editor_bridge(nullptr);
    return;
  }
  {
    engine::EnginePipeline pipeline;
    if (pipeline.initialize(0U) && (g_world != nullptr) &&
        pipeline.set_frame_delta_override(1.0 / 60.0)) {
      const rt::PersistentIdPolicy authoring = scattered(0x5EEDU);
      g_world->set_persistent_id_policy(authoring);
      CHECK(pipeline.execute_frame(), "a stopped frame");

      g_playing = true;
      CHECK(pipeline.execute_frame(), "the first playing frame");
      CHECK(g_world->persistent_id_policy().source ==
                rt::PersistentIdSource::Sequential,
            "a play session spawns with sequential ids");

      g_playing = false;
      CHECK(pipeline.execute_frame(), "the stop frame");
      const rt::PersistentIdPolicy after = g_world->persistent_id_policy();
      CHECK((after.source == rt::PersistentIdSource::Scattered) &&
                (after.streamState == authoring.streamState),
            "Stop gives the authoring policy back where it was");
    } else {
      CHECK(false, "pipeline initialize");
    }
    pipeline.teardown();
  }
  engine::shutdown();
  rt::set_editor_bridge(nullptr);
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: locate the sample project\n");
    return 1;
  }
  std::unique_ptr<rt::World> base = make_world();
  static std::array<char, kBufferBytes> baseText{};
  std::size_t baseSize = 0U;
  if ((base == nullptr) || (author_one(*base) != 1U) ||
      (author_one(*base) != 2U) || (author_one(*base) != 3U) ||
      !rt::save_scene(*base, baseText.data(), baseText.size(), &baseSize)) {
    std::fprintf(stderr, "FAIL: build the base scene\n");
    return 1;
  }
  check_two_branches(baseText.data(), baseSize);
  check_freed_id_after_reload();
  check_play_session();
  if (g_failures != 0) {
    return 1;
  }
  std::printf("PASS: authored entities take scattered persistent ids\n");
  return 0;
}
