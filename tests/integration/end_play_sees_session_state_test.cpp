// on_end_play sees the world the session ended in (#654). Editor Stop
// restored the pre-play snapshot before the pipeline dispatched the end
// hooks, so a hook reading its own state -- to save a result, say -- read
// the authored values, and anything spawned during play was already gone.
//
// Drives the production pipeline with a bridge wired to the real editor
// session (Play, Stop, the transition queue and the restore), and a Lua
// module that moves its entity every frame and records, in on_end_play,
// where the entity was.

#include "editor_session.h"
#include "engine/core/logging.h"
#include "engine/editor/editor.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {

constexpr const char *kScriptPath = "end_play_session_state_test.lua";

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

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
        std::filesystem::exists(
            normalized / "assets/shaders/bgfx/shaders.manifest", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

/// Moves its entity one unit along x per frame of play, and records in
/// on_end_play where the entity is when the session ends.
bool write_script() noexcept {
  std::ofstream out(kScriptPath, std::ios::trunc);
  out << "local M = {}\n"
         "function M.on_tick(self, dt)\n"
         "  local x, y, z = engine.get_position(self)\n"
         "  engine.set_position(self, x + 1.0, y, z)\n"
         "end\n"
         "function M.on_end_play(self)\n"
         "  local x = engine.get_position(self)\n"
         "  engine.log(string.format('ended at %.1f', x))\n"
         "end\n"
         "return M\n";
  return static_cast<bool>(out);
}

// What on_end_play logged. Stopping restarts the scripting VM, which resets
// every script-visible value, so the hook reports through the log.
char g_endPlayLine[64] = {};

void capture_end_play(engine::core::LogLevel /*level*/,
                      const char * /*channel*/, const char *message,
                      void * /*userData*/) noexcept {
  if ((message != nullptr) && (std::strstr(message, "ended at ") != nullptr)) {
    std::snprintf(g_endPlayLine, sizeof(g_endPlayLine), "%s",
                  std::strstr(message, "ended at "));
  }
}

/// The x of the Scene Controller entity the script is attached to.
float controller_x(const engine::runtime::World &world) noexcept {
  const engine::runtime::Entity controller =
      world.find_entity_by_name("Scene Controller");
  engine::runtime::Transform transform{};
  if ((controller == engine::runtime::kInvalidEntity) ||
      !world.get_transform(controller, &transform)) {
    return -1.0F;
  }
  return transform.position.x;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets() || !write_script()) {
    std::fprintf(stderr, "FAIL: fixture setup\n");
    return 1;
  }

  // The editor's own session behind the bridge the pipeline drains, minus
  // the UI: no window, so no initialize or frame callbacks.
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
  config.mainScriptPath = kScriptPath;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    std::remove(kScriptPath);
    return 2;
  }

  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U)) {
      pipeline.teardown();
      engine::shutdown();
      std::remove(kScriptPath);
      return 3;
    }
    CHECK(pipeline.set_frame_delta_override(1.0 / 60.0), "delta override");
    CHECK(pipeline.execute_frame(), "stopped frame");
    engine::runtime::World *world = pipeline.world();
    CHECK(world != nullptr, "pipeline world");
    const float authoredX = (world != nullptr) ? controller_x(*world) : -1.0F;

    engine::editor::editor_session().initialized = true;
    engine::editor::start_play_mode();
    constexpr int kPlayFrames = 3;
    for (int frame = 0; frame < kPlayFrames; ++frame) {
      CHECK(pipeline.execute_frame(), "play frame");
    }
    const float playX = (world != nullptr) ? controller_x(*world) : -1.0F;
    CHECK(playX > authoredX, "the script moved its entity during play");

    CHECK(engine::core::log_register_sink(&capture_end_play, nullptr),
          "log sink");
    engine::editor::stop_play_mode();
    CHECK(pipeline.execute_frame(), "the frame that drains the Stop");
    engine::core::log_unregister_sink(&capture_end_play, nullptr);

    char expected[32] = {};
    std::snprintf(expected, sizeof(expected), "ended at %.1f",
                  static_cast<double>(playX));
    CHECK(std::strncmp(g_endPlayLine, expected, std::strlen(expected)) == 0,
          "on_end_play read the position the session ended at");
    std::printf("authored %.1f, play %.1f, on_end_play logged: %s\n",
                static_cast<double>(authoredX), static_cast<double>(playX),
                g_endPlayLine);
    CHECK((world != nullptr) && (controller_x(*world) == authoredX),
          "after the end hooks the authored scene is restored");
    CHECK(!engine::editor::editor_session().playStopPending &&
              !engine::editor::editor_session().worldRestoreFailed,
          "the pipeline finished the Stop");

    // Stop and Play before one frame: the drain ends the first session,
    // restores, and only then starts the second, which must begin from the
    // authored scene and keep the snapshot a later Stop restores.
    engine::editor::start_play_mode();
    for (int frame = 0; frame < kPlayFrames; ++frame) {
      CHECK(pipeline.execute_frame(), "second session frame");
    }
    g_endPlayLine[0] = '\0';
    CHECK(engine::core::log_register_sink(&capture_end_play, nullptr),
          "log sink");
    engine::editor::stop_play_mode();
    engine::editor::start_play_mode();
    CHECK(pipeline.execute_frame(), "the Stop-then-Play frame");
    engine::core::log_unregister_sink(&capture_end_play, nullptr);
    CHECK(std::strncmp(g_endPlayLine, expected, std::strlen(expected)) == 0,
          "the stopped session's end hook saw its own final position");
    CHECK((world != nullptr) && (controller_x(*world) == authoredX + 1.0F),
          "the next session began on the restored scene and ran one frame");
    engine::editor::stop_play_mode();
    CHECK(pipeline.execute_frame(), "the final Stop frame");
    CHECK((world != nullptr) && (controller_x(*world) == authoredX),
          "the snapshot survived the same-frame restart");

    engine::editor::editor_session().initialized = false;
    pipeline.teardown();
  }

  engine::shutdown();
  engine::runtime::set_editor_bridge(nullptr);
  std::remove(kScriptPath);

  if (g_failures != 0) {
    std::fprintf(stderr, "end_play_sees_session_state_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("end_play_sees_session_state_test: all checks passed\n");
  return 0;
}
