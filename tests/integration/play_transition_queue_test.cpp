// Regression for #414 item 4: the pipeline dispatches every play-state
// change the author made, in order, instead of the one net change two
// samples can express. Before the fix, a frame that carried Stop and then
// Play again read as "still playing" and the ended session's on_end_play
// never ran -- a script's end hook, and any save it performs, silently
// skipped. Drives real engine::bootstrap() + EnginePipeline frames with a
// bridge that records transitions the way the editor does.

#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace {

constexpr const char *kScriptPath = "play_transition_queue_test.lua";
constexpr const char *kSecondScriptPath = "play_transition_queue_second.lua";
constexpr double kFrameSeconds = 1.0 / 60.0;

engine::runtime::World *g_world = nullptr;

void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

// The level the bridge reports, set by the driver alongside the queue so a
// frame can look "still playing" while carrying a Stop and a Start.
bool g_playing = false;
bool g_paused = false;
bool bridge_is_playing() noexcept { return g_playing; }
bool bridge_is_paused() noexcept { return g_paused; }

// The recorded transitions, in the order the author caused them: the same
// shape as the editor session's queue, kept here so the test owns what the
// pipeline drains.
constexpr std::size_t kMaxQueued = 8U;
std::array<engine::runtime::PlayTransition, kMaxQueued> g_queued{};
std::size_t g_queuedHead = 0U;
std::size_t g_queuedCount = 0U;

bool queue_transition(engine::runtime::PlayTransition transition) noexcept {
  if (g_queuedCount >= kMaxQueued) {
    return false;
  }
  g_queued[(g_queuedHead + g_queuedCount) % kMaxQueued] = transition;
  ++g_queuedCount;
  return true;
}

bool bridge_consume_play_transition(
    engine::runtime::PlayTransition *outTransition) noexcept {
  if ((outTransition == nullptr) || (g_queuedCount == 0U)) {
    return false;
  }
  *outTransition = g_queued[g_queuedHead];
  g_queuedHead = (g_queuedHead + 1U) % kMaxQueued;
  --g_queuedCount;
  return true;
}

// The hook trace. A log sink rather than a scripting global because a Stop
// recycles the Lua VM, which would take a global with it; the trace has to
// outlive the session that wrote to it.
char g_trace[256] = {};

void trace_sink(engine::core::LogLevel level, const char *channel,
                const char *message, void * /*userData*/) noexcept {
  static_cast<void>(level);
  if ((channel == nullptr) || (message == nullptr) ||
      (std::strcmp(channel, "scripting") != 0)) {
    return;
  }
  if (std::strncmp(message, "pt ", 3U) != 0) {
    return;
  }
  const char *event = message + 3;
  const std::size_t used = std::strlen(g_trace);
  const std::size_t room = sizeof(g_trace) - used - 1U;
  const std::size_t length = std::strlen(event);
  if ((length + 1U) > room) {
    return;
  }
  std::memcpy(g_trace + used, event, length);
  g_trace[used + length] = '|';
  g_trace[used + length + 1U] = '\0';
}

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

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
    if (std::filesystem::exists(normalized / "assets/main.lua", ec) &&
        std::filesystem::exists(
            normalized / "assets/shaders/bgfx/shaders.manifest", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

// The two session hooks, written to the log so the trace survives the VM
// recycle a Stop performs. on_tick stays silent: the trace is about
// session boundaries, not about frames.
constexpr const char *kScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    engine.log('pt begin')\n"
    "end\n"
    "function M.on_end_play(self)\n"
    "    engine.log('pt end')\n"
    "end\n"
    "return M\n";

// The second entity's module. A separate file so the trace names which
// entity each hook belongs to rather than leaving two begins ambiguous.
constexpr const char *kSecondScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    engine.log('pt begin2')\n"
    "end\n"
    "function M.on_end_play(self)\n"
    "    engine.log('pt end2')\n"
    "end\n"
    "return M\n";

bool write_script_file(const char *path, const char *text) noexcept {
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
  const std::size_t length = std::char_traits<char>::length(text);
  const bool ok = (std::fwrite(text, 1U, length, file) == length);
  static_cast<void>(std::fclose(file));
  return ok;
}

/// Spawns one entity carrying a traced script module.
engine::runtime::Entity spawn_scripted(const char *scriptPath) noexcept {
  const engine::runtime::Entity entity = g_world->create_scene_object();
  if (entity == engine::runtime::kInvalidEntity) {
    return entity;
  }
  engine::runtime::ScriptComponent script{};
  const int written = std::snprintf(script.scriptPath,
                                    sizeof(script.scriptPath), "%s",
                                    scriptPath);
  if ((written < 0) ||
      (static_cast<std::size_t>(written) >= sizeof(script.scriptPath)) ||
      !g_world->add_script_component(entity, script)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!write_script_file(kScriptPath, kScript) ||
      !write_script_file(kSecondScriptPath, kSecondScript)) {
    std::fprintf(stderr, "FAIL: write script files\n");
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  bridge.consume_play_transition = &bridge_consume_play_transition;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = 1U;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    static_cast<void>(std::remove(kScriptPath));
  static_cast<void>(std::remove(kSecondScriptPath));
    return 2;
  }
  CHECK(engine::core::log_register_sink(&trace_sink, nullptr),
        "register the trace sink");

  int result = 0;
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline initialize\n");
      pipeline.teardown();
      engine::core::log_unregister_sink(&trace_sink, nullptr);
      engine::shutdown();
      static_cast<void>(std::remove(kScriptPath));
  static_cast<void>(std::remove(kSecondScriptPath));
      return 3;
    }
    CHECK(pipeline.set_frame_delta_override(kFrameSeconds),
          "frame delta override");

    // Stopped frames: the entity exists, nothing has begun.
    CHECK(pipeline.execute_frame(), "settle frame");
    const engine::runtime::Entity first = spawn_scripted(kScriptPath);
    CHECK(first != engine::runtime::kInvalidEntity, "spawn the first entity");
    CHECK(pipeline.execute_frame(), "stopped frame with the entity");
    CHECK(std::strlen(g_trace) == 0U, "nothing begins while stopped");

    // The author hits Play. One transition, and the level agrees.
    CHECK(queue_transition(engine::runtime::PlayTransition::Start),
          "queue Start");
    g_playing = true;
    CHECK(pipeline.execute_frame(), "first playing frame");
    CHECK(std::strcmp(g_trace, "begin|") == 0, "the first session began");

    // The author hits Stop and Play again before the next frame. The level
    // still reads Playing, so nothing about it distinguishes this frame
    // from an uninterrupted one -- only the recorded transitions do. A
    // second entity spawned now begins play in the new session, so the
    // trace carries the whole sequence rather than just its end.
    const engine::runtime::Entity second = spawn_scripted(kSecondScriptPath);
    CHECK(second != engine::runtime::kInvalidEntity, "spawn the second entity");
    CHECK(queue_transition(engine::runtime::PlayTransition::Stop),
          "queue Stop");
    CHECK(queue_transition(engine::runtime::PlayTransition::Start),
          "queue the second Start");
    CHECK(pipeline.execute_frame(), "the stop-and-start frame");

    // The contract, in order: the first session's entity began, that
    // session ended, and the new session began for both entities. The
    // session-start dispatch gives every alive scripted entity its begin
    // hook, which is why the first entity appears twice and the second
    // once.
    //
    // Without the queue the "end" is missing: both samples of the play
    // state read Playing, so no transition is inferred, the first
    // session's end hook never runs, and only the newly spawned entity
    // begins.
    constexpr const char *kExpectedTrace = "begin|end|begin|begin2|";
    CHECK(std::strcmp(g_trace, kExpectedTrace) == 0,
          "the whole session sequence dispatched in order");
    if (std::strcmp(g_trace, kExpectedTrace) != 0) {
      std::fprintf(stderr, "       trace was \"%s\", expected \"%s\"\n",
                   g_trace, kExpectedTrace);
      result = 4;
    }

    // A quit arriving on a frame that already carries a Stop must not
    // dispatch a second set of end hooks over the session the Stop just
    // ended. The level still reads Playing, which is what the quit
    // branch keys on, so only the drained Stop keeps it from firing
    // twice.
    CHECK(queue_transition(engine::runtime::PlayTransition::Stop),
          "queue the Stop the quit lands on");
    SDL_Event quitEvent{};
    quitEvent.type = SDL_EVENT_QUIT;
    CHECK(SDL_PushEvent(&quitEvent), "push the quit event");
    static_cast<void>(pipeline.execute_frame());
    constexpr const char *kAfterQuitTrace =
        "begin|end|begin|begin2|end|end2|";
    CHECK(std::strcmp(g_trace, kAfterQuitTrace) == 0,
          "a quit over a drained Stop ends the session once");
    if (std::strcmp(g_trace, kAfterQuitTrace) != 0) {
      std::fprintf(stderr, "       trace was \"%s\", expected \"%s\"\n",
                   g_trace, kAfterQuitTrace);
      result = (result != 0) ? result : 5;
    }

    pipeline.teardown();
  }

  engine::core::log_unregister_sink(&trace_sink, nullptr);
  engine::shutdown();
  static_cast<void>(std::remove(kScriptPath));
  static_cast<void>(std::remove(kSecondScriptPath));

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return (result != 0) ? result : 1;
  }

  std::puts("play_transition_queue_test passed");
  return 0;
}
