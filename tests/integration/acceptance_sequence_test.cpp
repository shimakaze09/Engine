// The finite acceptance sequence, run headless through production entry
// points: author a scene, save it, load it through the deferred scene
// operation, Play, Stop, Play and Stop again, a failed main-script hot
// reload that leaves the world untouched, a successful one that applies
// exactly once, then teardown, reopen, reload the saved scene and replay
// the same gameplay slice to the same World::state_hash. Everything a GPU
// would show (the rendered frame, the editor panels) is outside this
// sequence and stays a human observation on real hardware.

#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/bindable_api.h"
#include "engine/scripting/scripting.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {

constexpr const char *kMainScript = "acceptance_sequence_main.lua";
constexpr const char *kEntityScript = "acceptance_sequence_probe.lua";
constexpr const char *kSceneFile = "acceptance_sequence.scene.json";
constexpr double kStepSeconds = 1.0 / 60.0;
constexpr int kSliceFrames = 30;

engine::runtime::World *g_world = nullptr;
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

bool g_playing = false;
bool bridge_is_playing() noexcept { return g_playing; }
bool bridge_is_paused() noexcept { return false; }

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Walks upward from the current path until the bundled assets are found
/// (same technique as pipeline_tick_cadence_test.cpp).
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
    if (std::filesystem::exists(normalized / "assets/main.lua", ec) &&
        std::filesystem::exists(normalized / "assets/shaders/bgfx/shaders.json",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

bool write_text_file(const char *path, const char *contents) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(contents);
  const std::size_t written = std::fwrite(contents, 1U, length, file);
  std::fclose(file);
  return written == length;
}

/// Rewrites the watched main script after an mtime-visible delay so the
/// watcher sees a changed timestamp rather than skipping the reload.
bool rewrite_main_script(const char *contents) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return write_text_file(kMainScript, contents);
}

void remove_files() noexcept {
  static_cast<void>(std::remove(kMainScript));
  static_cast<void>(std::remove(kEntityScript));
  static_cast<void>(std::remove(kSceneFile));
}

// The probe names its own lifecycle through the World so the observation
// survives Stop, which recycles the scripting VM: begin-play and end-play
// rename the entity, ticks label the game state.
constexpr const char *kProbeScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    engine.set_game_state('began')\n"
    "    engine.set_name(self, 'ProbeBegan')\n"
    "end\n"
    "function M.on_tick(self, dt)\n"
    "    engine.set_game_state('ticking')\n"
    "end\n"
    "function M.on_end_play(self)\n"
    "    engine.set_name(self, 'ProbeEnded')\n"
    "end\n"
    "return M\n";

constexpr const char *kMainV1 = "MainVersion = 1\n";

/// Mutates the marker and spawns, then fails: nothing of it may remain.
constexpr const char *kMainFailing =
    "local m = engine.find_entity_by_name('Marker')\n"
    "if m == nil then error('marker missing') end\n"
    "engine.set_position(m, 9, 9, 9)\n"
    "engine.set_name(m, 'Renamed')\n"
    "if engine.spawn_entity() == nil then error('spawn refused') end\n"
    "error('intentional reload failure')\n";

/// The same shape of effects, committed: applied once.
constexpr const char *kMainGood =
    "local m = engine.find_entity_by_name('Marker')\n"
    "if m == nil then error('marker missing') end\n"
    "engine.set_position(m, 7, 7, 7)\n"
    "local s = engine.spawn_entity()\n"
    "if s == nil then error('spawn refused') end\n"
    "engine.set_name(s, 'Spawned')\n";

/// One frame that simulates exactly one fixed step when playing.
bool frame(engine::EnginePipeline &pipeline) noexcept {
  return pipeline.set_frame_delta_override(kStepSeconds) &&
         pipeline.execute_frame();
}

bool game_state_is(const char *expected) noexcept {
  const char *actual = engine::scripting::bindable_get_game_state();
  return (actual != nullptr) && (std::strcmp(actual, expected) == 0);
}

bool name_entity(engine::runtime::World &world, engine::runtime::Entity entity,
                 const char *name) noexcept {
  engine::runtime::NameComponent component{};
  std::snprintf(component.name, sizeof(component.name), "%s", name);
  return world.add_name_component(entity, component);
}

/// Authors the scene the sequence saves: a falling marker body and a
/// scripted probe.
bool author_scene(engine::runtime::World &world) noexcept {
  const engine::runtime::Entity marker = world.create_scene_object();
  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(0.0F, 5.0F, 0.0F);
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  engine::runtime::Collider collider{};
  collider.shape = engine::runtime::ColliderShape::Sphere;
  collider.halfExtents = engine::math::Vec3(0.4F, 0.4F, 0.4F);
  if ((marker == engine::runtime::kInvalidEntity) ||
      !world.add_transform(marker, transform) ||
      !world.add_rigid_body(marker, body) ||
      !world.add_collider(marker, collider) ||
      !name_entity(world, marker, "Marker")) {
    return false;
  }
  // The edit: the marker moves before the save.
  transform.position = engine::math::Vec3(1.0F, 5.0F, 0.0F);
  if (!world.add_transform(marker, transform)) {
    return false;
  }

  const engine::runtime::Entity probe = world.create_scene_object();
  engine::runtime::ScriptComponent script{};
  std::snprintf(script.scriptPath, sizeof(script.scriptPath), "%s",
                kEntityScript);
  return (probe != engine::runtime::kInvalidEntity) &&
         world.add_script_component(probe, script) &&
         name_entity(world, probe, "Probe");
}

/// Loads the saved scene through the deferred scene operation a script
/// would request, processed by the pipeline's scene-commit stage.
bool load_saved_scene(engine::EnginePipeline &pipeline) noexcept {
  return engine::scripting::request_scene_load(kSceneFile) && frame(pipeline);
}

bool marker_position(engine::math::Vec3 *out) noexcept {
  const engine::runtime::Entity marker = g_world->find_entity_by_name("Marker");
  engine::runtime::Transform transform{};
  if ((marker == engine::runtime::kInvalidEntity) ||
      !g_world->get_transform(marker, &transform)) {
    return false;
  }
  *out = transform.position;
  return true;
}

/// Plays the gameplay slice from a freshly loaded scene and hashes the
/// result.
bool play_slice(engine::EnginePipeline &pipeline, std::uint64_t *outHash) noexcept {
  g_playing = true;
  for (int i = 0; i < kSliceFrames; ++i) {
    if (!frame(pipeline)) {
      return false;
    }
  }
  *outHash = g_world->state_hash();
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!write_text_file(kMainScript, kMainV1) ||
      !write_text_file(kEntityScript, kProbeScript)) {
    std::fprintf(stderr, "FAIL: write scripts\n");
    remove_files();
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.mainScriptPath = kMainScript;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    remove_files();
    return 2;
  }

  std::uint64_t firstSliceHash = 0U;
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline A initialize\n");
      pipeline.teardown();
      engine::shutdown();
      remove_files();
      return 3;
    }

    // --- 1. Create and edit a scene, stopped. ---
    g_playing = false;
    engine::runtime::reset_world(*g_world);
    CHECK(author_scene(*g_world), "author the scene");

    // --- 2. Save it. ---
    CHECK(engine::runtime::save_scene(*g_world, kSceneFile), "save the scene");
    std::error_code ec{};
    CHECK(std::filesystem::exists(kSceneFile, ec), "the scene file exists");

    // --- 3. Load it through the production scene operation. ---
    engine::runtime::reset_world(*g_world);
    CHECK(g_world->find_entity_by_name("Marker") ==
              engine::runtime::kInvalidEntity,
          "the world is empty before the load");
    CHECK(load_saved_scene(pipeline), "load the saved scene");
    engine::math::Vec3 position{};
    CHECK(marker_position(&position) && (position.x == 1.0F) &&
              (position.y == 5.0F) && (position.z == 0.0F),
          "the load restores the edited marker position");
    {
      engine::runtime::RigidBody body{};
      const engine::runtime::Entity marker =
          g_world->find_entity_by_name("Marker");
      CHECK(g_world->get_rigid_body(marker, &body) && !body.inertiaAuthored &&
                (body.inverseInertia.x != 1.0F),
            "the loaded body is automatic and derived from its collider");
    }

    // --- 4. Play: the probe begins, ticks, and the marker falls. ---
    CHECK(play_slice(pipeline, &firstSliceHash), "play the gameplay slice");
    CHECK(game_state_is("ticking"), "the probe ticked during play");
    CHECK(g_world->find_entity_by_name("ProbeBegan") !=
              engine::runtime::kInvalidEntity,
          "the probe received begin-play");
    CHECK(marker_position(&position) && (position.y < 5.0F),
          "the marker fell under gravity");

    // --- 5. Stop: end-play runs once and the world content stays. ---
    g_playing = false;
    CHECK(frame(pipeline), "stop frame");
    CHECK(g_world->find_entity_by_name("ProbeEnded") !=
              engine::runtime::kInvalidEntity,
          "the probe received end-play on stop");
    CHECK(g_world->find_entity_by_name("Marker") !=
              engine::runtime::kInvalidEntity,
          "the marker survives stop");

    // --- 6. Play and stop again. ---
    g_playing = true;
    CHECK(frame(pipeline), "second play frame");
    CHECK(g_world->find_entity_by_name("ProbeBegan") !=
              engine::runtime::kInvalidEntity,
          "the probe receives begin-play on the second play");
    g_playing = false;
    CHECK(frame(pipeline), "second stop frame");
    CHECK(g_world->find_entity_by_name("ProbeEnded") !=
              engine::runtime::kInvalidEntity,
          "the probe receives end-play on the second stop");

    // --- 7. A failed main-script reload leaves the world untouched. ---
    g_playing = true;
    CHECK(frame(pipeline), "play frame before the reloads");
    engine::math::Vec3 before{};
    CHECK(marker_position(&before), "marker position before the reload");
    const std::size_t aliveBefore = g_world->alive_entity_count();
    CHECK(rewrite_main_script(kMainFailing), "write the failing main script");
    engine::scripting::check_script_reload();
    CHECK(marker_position(&position) && (position.x == before.x) &&
              (position.y == before.y) && (position.z == before.z),
          "a failed reload leaves the marker where it was");
    CHECK(g_world->find_entity_by_name("Marker") !=
              engine::runtime::kInvalidEntity,
          "a failed reload leaves the marker's name");
    CHECK(g_world->alive_entity_count() == aliveBefore,
          "a failed reload leaves no spawned entity");

    // --- 8. A successful reload applies exactly once. ---
    CHECK(rewrite_main_script(kMainGood), "write the good main script");
    engine::scripting::check_script_reload();
    CHECK(marker_position(&position) && (position.x == 7.0F) &&
              (position.y == 7.0F) && (position.z == 7.0F),
          "a successful reload moves the marker");
    CHECK(g_world->alive_entity_count() == aliveBefore + 1U,
          "a successful reload spawns once");
    CHECK(frame(pipeline), "frame after the reload");
    CHECK(g_world->alive_entity_count() == aliveBefore + 1U,
          "the next frame applies nothing twice");

    // --- 9. Teardown. ---
    g_playing = false;
    pipeline.teardown();
  }

  // --- 10. Reopen, reload the saved scene, replay the slice. ---
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline B initialize\n");
      pipeline.teardown();
      engine::shutdown();
      remove_files();
      return 4;
    }
    g_playing = false;
    engine::runtime::reset_world(*g_world);
    CHECK(load_saved_scene(pipeline), "reload the saved scene after reopen");
    engine::math::Vec3 position{};
    CHECK(marker_position(&position) && (position.x == 1.0F) &&
              (position.y == 5.0F),
          "the reopened engine restores the saved scene, not the played one");
    std::uint64_t secondSliceHash = 0U;
    CHECK(play_slice(pipeline, &secondSliceHash), "replay the gameplay slice");
    CHECK(secondSliceHash == firstSliceHash,
          "the same scene and deltas reach the same state hash after reopen");
    g_playing = false;
    pipeline.teardown();
  }

  engine::shutdown();
  remove_files();

  if (g_failures != 0) {
    std::fprintf(stderr, "acceptance_sequence: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("acceptance_sequence: all checks passed\n");
  return 0;
}
