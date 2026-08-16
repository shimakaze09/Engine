// Verifies the editor Console's log capture, filtering, duplicate collapse,
// bounded overflow, and source/entity navigation metadata (issue #155).
// Exercises the production path: real core::log_message calls through the
// registered sink, not a copied capture model.

#include "editor_console_capture.h"
#include "engine/core/logging.h"
#include "engine/runtime/world.h"
#include "../test_harness.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <vector>

namespace {

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

using engine::core::LogLevel;
using engine::core::log_message;
using namespace engine::editor;

/// EXPECTATION: a log_message call is captured with the right level,
/// channel, message, and engine/script category classification.
void check_basic_ingest_and_category() noexcept {
  console_capture_clear();

  log_message(LogLevel::Warning, "physics", "blocked body warning");
  log_message(LogLevel::Error, "scripting", "lua error: boom");

  check(console_capture_entry_count() == 2U, "two entries captured");

  ConsoleEntry entry{};
  check(console_capture_get_entry(0U, &entry), "get entry 0");
  check(entry.level == LogLevel::Warning, "entry 0 level");
  check(std::strcmp(entry.channel, "physics") == 0, "entry 0 channel");
  check(std::strcmp(entry.message, "blocked body warning") == 0,
       "entry 0 message");
  check(entry.category == ConsoleSourceCategory::Engine,
       "physics channel classified as Engine");

  check(console_capture_get_entry(1U, &entry), "get entry 1");
  check(entry.category == ConsoleSourceCategory::Script,
       "scripting channel classified as Script");
}

/// EXPECTATION: identical adjacent (level, channel, message) events collapse
/// into one entry with a growing repeat count; a different message in
/// between breaks the collapse run.
void check_duplicate_collapse() noexcept {
  console_capture_clear();

  log_message(LogLevel::Error, "jobs", "worker stall");
  log_message(LogLevel::Error, "jobs", "worker stall");
  log_message(LogLevel::Error, "jobs", "worker stall");
  log_message(LogLevel::Error, "jobs", "different message");
  log_message(LogLevel::Error, "jobs", "worker stall");

  check(console_capture_entry_count() == 3U,
       "three runs after collapsing adjacent repeats");
  check(console_capture_total_ingested() == 5U,
       "total ingested counts every log_message call, pre-collapse");

  ConsoleEntry entry{};
  check(console_capture_get_entry(0U, &entry), "get collapsed run");
  check(entry.repeatCount == 3U, "first run collapsed to repeatCount 3");
  check(console_capture_get_entry(1U, &entry), "get interrupting entry");
  check(entry.repeatCount == 1U, "interrupting message not collapsed");
  check(console_capture_get_entry(2U, &entry), "get final run");
  check(entry.repeatCount == 1U, "post-interrupt repeat starts a new run");
}

/// EXPECTATION: the ring never exceeds kMaxConsoleEntries; once full, the
/// oldest entry is dropped to admit the newest (explicit drop policy).
void check_bounded_overflow() noexcept {
  console_capture_clear();

  const std::size_t total = kMaxConsoleEntries + 50U;
  for (std::size_t i = 0U; i < total; ++i) {
    char message[64] = {};
    std::snprintf(message, sizeof(message), "overflow probe %zu", i);
    log_message(LogLevel::Info, "assets", message);
  }

  check(console_capture_entry_count() == kMaxConsoleEntries,
       "ring never grows past its fixed capacity");
  check(console_capture_total_ingested() == total,
       "total ingested still counts every call even past capacity");

  ConsoleEntry oldest{};
  check(console_capture_get_entry(0U, &oldest), "get oldest retained entry");
  char expectedOldest[64] = {};
  std::snprintf(expectedOldest, sizeof(expectedOldest), "overflow probe %zu",
               total - kMaxConsoleEntries);
  check(std::strcmp(oldest.message, expectedOldest) == 0,
       "oldest surviving entry is the first one not dropped");
}

/// EXPECTATION: severity, channel, search-text, and session-scope filters
/// each narrow the visible set independently and in combination.
void check_filtering() noexcept {
  console_capture_clear();

  log_message(LogLevel::Info, "assets", "loaded prop pack");
  log_message(LogLevel::Warning, "shader", "shader compile warning: unused");
  console_capture_begin_session();
  log_message(LogLevel::Error, "scripting", "lua error: nil index");

  ConsoleEntry entries[3] = {};
  for (std::size_t i = 0U; i < 3U; ++i) {
    check(console_capture_get_entry(i, &entries[i]), "fetch filter fixture");
  }

  ConsoleFilter severityOnly{};
  severityOnly.showTrace = severityOnly.showInfo = false;
  check(!console_filter_matches(severityOnly, entries[0]),
       "Info hidden when showInfo is false");
  check(console_filter_matches(severityOnly, entries[1]),
       "Warning still shown");

  ConsoleFilter channelOnly{};
  std::snprintf(channelOnly.channelFilter, sizeof(channelOnly.channelFilter),
               "%s", "shader");
  check(!console_filter_matches(channelOnly, entries[0]),
       "non-matching channel filtered out");
  check(console_filter_matches(channelOnly, entries[1]),
       "matching channel passes");

  ConsoleFilter searchOnly{};
  std::snprintf(searchOnly.searchText, sizeof(searchOnly.searchText), "%s",
               "NIL INDEX");
  check(!console_filter_matches(searchOnly, entries[1]),
       "search text is case-insensitive and must actually match");
  check(console_filter_matches(searchOnly, entries[2]),
       "search text matches message substring case-insensitively");

  ConsoleFilter sessionOnly{};
  sessionOnly.sessionOnly = true;
  check(!console_filter_matches(sessionOnly, entries[0]),
       "entry before begin_session excluded");
  check(!console_filter_matches(sessionOnly, entries[1]),
       "entry before begin_session excluded (2)");
  check(console_filter_matches(sessionOnly, entries[2]),
       "entry after begin_session included");
}

/// EXPECTATION: a Lua-shaped error message ("<path>:<line>: ...", the exact
/// text binding_util's log_lua_error produces) yields ScriptLocation
/// navigation metadata with the right path and 1-based line.
void check_script_location_navigation() noexcept {
  console_capture_clear();

  log_message(LogLevel::Error, "scripting",
             "lua error (on_tick): assets/scripts/island_player.lua:42: "
             "attempt to index nil value (local 'x')\nstack traceback:\n\t"
             "[C]: in ?");

  ConsoleEntry entry{};
  check(console_capture_get_entry(0U, &entry), "get lua error entry");
  check(entry.referenceKind == ConsoleReferenceKind::ScriptLocation,
       "lua error parsed as a script location");
  check(std::strcmp(entry.referencePath,
                    "assets/scripts/island_player.lua") == 0,
       "script path parsed correctly");
  check(entry.referenceLine == 42, "script line parsed correctly");
}

/// EXPECTATION: a "<path>: <reason>" diagnostic (the animation loader's
/// fail() shape) yields AssetPath navigation metadata.
void check_asset_path_navigation() noexcept {
  console_capture_clear();

  log_message(LogLevel::Error, "animation",
             "assets/character.anim: file too short");

  ConsoleEntry entry{};
  check(console_capture_get_entry(0U, &entry), "get asset diagnostic entry");
  check(entry.referenceKind == ConsoleReferenceKind::AssetPath,
       "asset diagnostic parsed as an asset path");
  check(std::strcmp(entry.referencePath, "assets/character.anim") == 0,
       "asset path parsed correctly, trailing colon trimmed");
}

/// EXPECTATION: an "entity <n>" diagnostic yields an entity-index hint that
/// resolves to the real, currently alive entity through the production
/// World API — and refuses to resolve once that entity is destroyed.
void check_entity_hint_navigation() noexcept {
  using engine::runtime::Entity;
  using engine::runtime::kInvalidEntity;
  using engine::runtime::World;

  console_capture_clear();

  std::unique_ptr<World> world(new (std::nothrow) World());
  check(world != nullptr, "world allocated");
  if (world == nullptr) {
    return;
  }

  const Entity spawned = world->create_scene_object();
  check(spawned != kInvalidEntity, "entity spawned");

  char message[64] = {};
  std::snprintf(message, sizeof(message), "Spawned entity %u",
               spawned.index);
  log_message(LogLevel::Info, "cheat", message);

  ConsoleEntry entry{};
  check(console_capture_get_entry(0U, &entry), "get spawn entry");
  check(entry.entityIndexHint == spawned.index,
       "entity index hint parsed correctly");

  const Entity resolved =
      console_capture_resolve_entity_hint(entry.entityIndexHint, world.get());
  check(resolved == spawned, "hint resolves to the live entity");

  check(console_capture_resolve_entity_hint(entry.entityIndexHint,
                                            nullptr) == kInvalidEntity,
       "resolution refuses a null world (unsafe)");

  world->destroy_entity(spawned);
  const Entity afterDestroy =
      console_capture_resolve_entity_hint(entry.entityIndexHint, world.get());
  check(afterDestroy == kInvalidEntity,
       "resolution refuses a destroyed entity's stale index");
}

/// EXPECTATION: unseen badge counters increment on Warning/Error ingest,
/// do not double-count a collapsed repeat, and reset via mark_seen.
void check_unseen_badge_counters() noexcept {
  console_capture_clear();
  console_capture_mark_seen();
  check(console_capture_unseen_error_count() == 0U, "badge starts clean");
  check(console_capture_unseen_warning_count() == 0U,
       "warning badge starts clean");

  log_message(LogLevel::Error, "renderer", "shader link failed");
  log_message(LogLevel::Error, "renderer", "shader link failed");
  log_message(LogLevel::Warning, "audio", "voice pool exhausted");

  check(console_capture_unseen_error_count() == 1U,
       "collapsed repeat only raises the badge once");
  check(console_capture_unseen_warning_count() == 1U, "warning badge raised");

  console_capture_mark_seen();
  check(console_capture_unseen_error_count() == 0U,
       "mark_seen clears the error badge");
  check(console_capture_unseen_warning_count() == 0U,
       "mark_seen clears the warning badge");
}

/// EXPECTATION: log_set_frame_index publishes a value captured into the
/// next ingested entry's frameIndex field.
void check_frame_index_context() noexcept {
  console_capture_clear();
  engine::core::log_set_frame_index(123U);
  log_message(LogLevel::Trace, "jobs", "frame-context probe");

  ConsoleEntry entry{};
  check(console_capture_get_entry(0U, &entry), "get frame-context entry");
  check(entry.frameIndex == 123U, "captured entry records the frame index");
  engine::core::log_set_frame_index(0U);
}

/// EXPECTATION: concurrent log_message calls from multiple threads are all
/// captured (distinct messages avoid collapsing so counts stay exact),
/// proving the sink's lock protects the ring under real contention.
void check_concurrent_ingest_is_safe() noexcept {
  console_capture_clear();

  constexpr int kThreadCount = 6;
  constexpr int kMessagesPerThread = 30;
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(kThreadCount));
  for (int t = 0; t < kThreadCount; ++t) {
    threads.emplace_back([t]() noexcept {
      for (int i = 0; i < kMessagesPerThread; ++i) {
        char message[64] = {};
        std::snprintf(message, sizeof(message), "thread %d message %d", t, i);
        log_message(LogLevel::Trace, "jobs", message);
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  const std::size_t expected =
      static_cast<std::size_t>(kThreadCount) *
      static_cast<std::size_t>(kMessagesPerThread);
  check(console_capture_total_ingested() == expected,
       "every concurrent log_message call was ingested exactly once");
  check(console_capture_entry_count() == expected,
       "distinct concurrent messages are never collapsed together");
}

} // namespace

/// Runs this executable or test program.
int main() {
  check(engine::core::initialize_logging(), "initialize_logging");
  console_capture_initialize();

  check_basic_ingest_and_category();
  check_duplicate_collapse();
  check_bounded_overflow();
  check_filtering();
  check_script_location_navigation();
  check_asset_path_navigation();
  check_entity_hint_navigation();
  check_unseen_badge_counters();
  check_frame_index_context();
  check_concurrent_ingest_is_safe();

  console_capture_shutdown();
  engine::core::shutdown_logging();
  return g_tests.finish("editor_console_capture_test");
}
