// Play on an edited World runs exactly as Play on the scene reloaded from
// it, which is how Stop restores it and how the player starts. The editor
// used to play on the World the author had edited in place: a delete and
// re-create left component arrays in edit-history order, and contact
// resolution, World::state_hash and script dispatch all walk storage, so
// the first Play after an edit simulated differently from every later
// one, and script hooks ran in an order the shipped game would not use.
//
// Drives the production pipeline with a bridge wired to the real editor
// session. The scene holds a pile of eight boxes on a static floor, with
// one box deleted and re-created as authors do, and three scripted
// entities A, B and C whose script components were removed and re-added
// out of order. Each session also removes and re-adds A's script during
// play. Two sessions -- the first on the edited World, the second on the
// World Stop restored -- must end on one state hash and log one hook
// order, and that order is the entities' index order, A, B, C.

#include "../asset_root.h"
#include "editor_session.h"
#include "engine/core/logging.h"
#include "engine/editor/editor.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

constexpr const char *kMainScriptPath = "play_matches_reload_main.lua";
constexpr const char *kOrderScriptPath = "play_matches_reload_order.lua";
constexpr int kPlayFrames = 90;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

bool write_file(const char *path, const char *text) noexcept {
  std::ofstream out(path, std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

/// Logs each hook as it runs, naming its entity. Stop restarts the
/// scripting VM, so the order is read back through the log.
bool write_scripts() noexcept {
  return write_file(kMainScriptPath, "local M = {}\nreturn M\n") &&
         write_file(kOrderScriptPath,
                    "local M = {}\n"
                    "function M.on_begin_play(self)\n"
                    "  engine.log('hook begin ' .. engine.get_name(self))\n"
                    "end\n"
                    "function M.on_tick(self, dt)\n"
                    "  engine.log('hook tick ' .. engine.get_name(self))\n"
                    "end\n"
                    "return M\n");
}

std::string g_hooks{};

void capture_hooks(engine::core::LogLevel /*level*/, const char * /*channel*/,
                   const char *message, void * /*userData*/) noexcept {
  const char *hook =
      (message != nullptr) ? std::strstr(message, "hook ") : nullptr;
  if (hook != nullptr) {
    g_hooks += hook + 5;
    g_hooks += ';';
  }
}

engine::runtime::Entity add_box(engine::runtime::World &world,
                                const engine::math::Vec3 &position,
                                const engine::math::Vec3 &halfExtents,
                                float inverseMass) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  const engine::runtime::Entity entity = world.create_scene_object(transform);
  engine::runtime::Collider collider{};
  collider.halfExtents = halfExtents;
  engine::runtime::RigidBody body{};
  body.inverseMass = inverseMass;
  if ((entity == engine::runtime::kInvalidEntity) ||
      !world.add_collider(entity, collider) ||
      !world.add_rigid_body(entity, body)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

engine::math::Vec3 pile_position(int i) noexcept {
  // Two loose layers, offset so boxes land on box edges and slide: a
  // result that depends on the order contacts are resolved in.
  const float x = static_cast<float>(i % 4) * 0.9F - 1.35F;
  const float y = 0.6F + (static_cast<float>(i / 4) * 1.3F);
  const float z = static_cast<float>(i / 4) * 0.35F;
  return engine::math::Vec3(x, y, z);
}

engine::runtime::Entity add_scripted(engine::runtime::World &world,
                                     const char *name) noexcept {
  const engine::runtime::Entity entity = world.create_scene_object();
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  if ((entity == engine::runtime::kInvalidEntity) ||
      !world.add_name_component(entity, nameComponent)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

bool attach_script(engine::runtime::World &world,
                   engine::runtime::Entity entity) noexcept {
  engine::runtime::ScriptComponent script{};
  std::snprintf(script.behaviours[0].scriptPath, sizeof(script.behaviours[0].scriptPath), "%s",
                kOrderScriptPath);
  return world.add_script_component(entity, script);
}

/// Authors the scene with an edit history: box 0 deleted and re-created
/// in place, and the scripts attached as C, B, A after A's was attached
/// and removed first.
bool author_scene(engine::runtime::World &world) noexcept {
  if (add_box(world, engine::math::Vec3(0.0F, -0.5F, 0.0F),
              engine::math::Vec3(10.0F, 0.5F, 10.0F),
              0.0F) == engine::runtime::kInvalidEntity) {
    return false;
  }
  engine::runtime::Entity boxes[8] = {};
  for (int i = 0; i < 8; ++i) {
    boxes[i] = add_box(world, pile_position(i),
                       engine::math::Vec3(0.5F, 0.5F, 0.5F), 1.0F);
    if (boxes[i] == engine::runtime::kInvalidEntity) {
      return false;
    }
  }
  if (!world.destroy_entity(boxes[0]) ||
      (add_box(world, pile_position(0), engine::math::Vec3(0.5F, 0.5F, 0.5F),
               1.0F) == engine::runtime::kInvalidEntity)) {
    return false;
  }
  const engine::runtime::Entity a = add_scripted(world, "A");
  const engine::runtime::Entity b = add_scripted(world, "B");
  const engine::runtime::Entity c = add_scripted(world, "C");
  return (a != engine::runtime::kInvalidEntity) &&
         (b != engine::runtime::kInvalidEntity) &&
         (c != engine::runtime::kInvalidEntity) && attach_script(world, a) &&
         attach_script(world, b) && attach_script(world, c) &&
         world.remove_script_component(a) && attach_script(world, a);
}

/// One Play of kPlayFrames frames and its Stop. After the first frame A's
/// script is removed and re-added, as an author or a script might. Returns
/// the state hash at the last frame of play and the hooks it logged.
std::uint64_t play_session(engine::EnginePipeline &pipeline,
                           engine::runtime::World &world,
                           std::string *outHooks) noexcept {
  g_hooks.clear();
  CHECK(engine::core::log_register_sink(&capture_hooks, nullptr), "log sink");
  engine::editor::start_play_mode();
  for (int frame = 0; frame < kPlayFrames; ++frame) {
    CHECK(pipeline.execute_frame(), "play frame");
    if (frame == 0) {
      const engine::runtime::Entity a = world.find_entity_by_name("A");
      CHECK(world.remove_script_component(a) && attach_script(world, a),
            "A's script re-added during play");
    }
  }
  const std::uint64_t hash = world.state_hash();
  engine::core::log_unregister_sink(&capture_hooks, nullptr);
  engine::editor::stop_play_mode();
  CHECK(pipeline.execute_frame(), "the frame that drains the Stop");
  *outHooks = g_hooks;
  return hash;
}

/// The hooks a session logs when every hook runs in A, B, C order.
std::string expected_hooks() {
  std::string hooks = "begin A;begin B;begin C;";
  for (int frame = 0; frame < kPlayFrames; ++frame) {
    hooks += "tick A;tick B;tick C;";
  }
  return hooks;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root() || !write_scripts()) {
    std::fprintf(stderr, "FAIL: fixture setup\n");
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &engine::editor::editor_set_world;
  bridge.is_playing = &engine::editor::editor_is_playing;
  bridge.is_paused = &engine::editor::editor_is_paused;
  bridge.consume_play_transition = &engine::editor::consume_play_transition;
  bridge.complete_play_stop = &engine::editor::finish_play_stop;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = 1U;
  config.mainScriptPath = kMainScriptPath;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    std::remove(kMainScriptPath);
    std::remove(kOrderScriptPath);
    return 2;
  }

  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U)) {
      pipeline.teardown();
      engine::shutdown();
      std::remove(kMainScriptPath);
      std::remove(kOrderScriptPath);
      return 3;
    }
    CHECK(pipeline.set_frame_delta_override(1.0 / 60.0), "delta override");
    CHECK(pipeline.execute_frame(), "stopped frame");
    engine::runtime::World *world = pipeline.world();
    CHECK((world != nullptr) && author_scene(*world), "scene authored");

    if (world != nullptr) {
      engine::editor::editor_session().initialized = true;
      std::string firstHooks{};
      std::string secondHooks{};
      const std::uint64_t first = play_session(pipeline, *world, &firstHooks);
      const std::uint64_t second = play_session(pipeline, *world, &secondHooks);
      std::printf("state hash: edited %llu, restored %llu\n",
                  static_cast<unsigned long long>(first),
                  static_cast<unsigned long long>(second));
      CHECK(first == second,
            "Play on the edited World ends where Play on its reload does");
      CHECK(firstHooks == secondHooks,
            "both sessions run their script hooks in one order");
      const std::string expected = expected_hooks();
      CHECK(firstHooks == expected,
            "hooks run in entity index order, A then B then C");
      if (firstHooks != expected) {
        std::fprintf(stderr, "first session hooks begin: %.120s\n",
                     firstHooks.c_str());
      }
      engine::editor::editor_session().initialized = false;
    }
    pipeline.teardown();
  }

  engine::shutdown();
  engine::runtime::set_editor_bridge(nullptr);
  std::remove(kMainScriptPath);
  std::remove(kOrderScriptPath);

  if (g_failures != 0) {
    std::fprintf(stderr, "play_matches_reload_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("play_matches_reload_test: all checks passed\n");
  return 0;
}
