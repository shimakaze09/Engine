// Integration tests for the hot-reload transaction across a chunk that
// mutates timers, audio, entities, joints and components and then fails:
// every effect must be discarded, and the same effects from a chunk that
// succeeds must land exactly once, in the order the chunk requested them.
// Also pins the scope's capacities (exact and one past), read-your-writes
// inside the chunk, the refused effects, an instruction-budget failure and
// a commit whose effect reports failure. Runs through the production
// watch/check_script_reload path against the production bridge table with
// only the audio calls recorded.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

#include "../test_harness.h"
#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/physics/physics_context.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/runtime_services.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr const char *kScriptPath = "hotreload_transaction_test.lua";

/// Writes the watched script file.
bool write_script(const char *code) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, kScriptPath, "wb") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(kScriptPath, "wb");
  if (f == nullptr) {
    return false;
  }
#endif
  const std::size_t len = std::strlen(code);
  const bool ok = (std::fwrite(code, 1U, len, f) == len);
  std::fclose(f);
  return ok;
}

/// Rewrites the watched script after an mtime-visible delay, so the watcher
/// sees a changed timestamp rather than skipping the reload.
bool rewrite_script(const char *code) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return write_script(code);
}

// Audio recorder: the production table with its audio entries replaced, so
// the test sees exactly which calls reached the audio service and in what
// order.
constexpr std::size_t kMaxRecorded = 160U;
char g_recorded[kMaxRecorded][48] = {};
std::size_t g_recordedCount = 0U;
/// When set, the recorded play_sound reports failure, so a held call that
/// the chunk saw succeed fails at commit.
bool g_playSoundFails = false;
/// The marker entity, read by the play_sound recorder so the test can see
/// which queued name write had applied when each held call ran.
rt::World *g_world = nullptr;
rt::Entity g_marker = rt::kInvalidEntity;

void record(const char *name) noexcept {
  if (g_recordedCount < kMaxRecorded) {
    std::snprintf(g_recorded[g_recordedCount], sizeof(g_recorded[0]), "%s",
                  name);
    ++g_recordedCount;
  }
}

bool record_play_sound(std::uint32_t, float, float, bool) noexcept {
  rt::NameComponent name{};
  if ((g_world != nullptr) &&
      g_world->get_name_component(g_marker, &name)) {
    char entry[48] = {};
    std::snprintf(entry, sizeof(entry), "play_sound:%s", name.name);
    record(entry);
  } else {
    record("play_sound");
  }
  return !g_playSoundFails;
}

/// Counts the reload driver's "committed with failures" report.
std::size_t g_commitFailureLogs = 0U;
void count_commit_failure(engine::core::LogLevel, const char *,
                          const char *message, void *) noexcept {
  if ((message != nullptr) &&
      (std::strstr(message, "hot-reload committed; some of its effects") !=
       nullptr)) {
    ++g_commitFailureLogs;
  }
}
bool record_play_sound_at(std::uint32_t, float, float, float, float) noexcept {
  record("play_sound_at");
  return true;
}
void record_set_bus_volume(std::uint32_t, float) noexcept {
  record("set_bus_volume");
}
bool record_play_music(const char *, float, bool) noexcept {
  record("play_music");
  return true;
}
void record_stop_music() noexcept { record("stop_music"); }
void record_stop_sound(std::uint32_t) noexcept { record("stop_sound"); }

/// True when the recorded call names match `expected` exactly, in order.
bool recorded_is(const char *const *expected, std::size_t count) noexcept {
  if (g_recordedCount != count) {
    return false;
  }
  for (std::size_t i = 0U; i < count; ++i) {
    if (std::strcmp(g_recorded[i], expected[i]) != 0) {
      return false;
    }
  }
  return true;
}

// The first version establishes the state later reloads mutate: an entity
// the test authored, a timer, and a pool with one entity taken.
constexpr const char *kV1 =
    "Marker = 'v1'\n"
    "E = engine.find_entity_by_name('Marker')\n"
    "if E == nil then error('marker entity missing') end\n"
    "E2 = engine.find_entity_by_name('Second')\n"
    "if E2 == nil then error('second entity missing') end\n"
    "T = engine.set_timeout(function() end, 100)\n"
    "if T == nil then error('timer missing') end\n"
    "P = engine.pool_create(2)\n"
    "if P == nil then error('pool missing') end\n"
    "X = engine.pool_spawn(P)\n"
    "if X == nil then error('pool spawn missing') end\n"
    "engine.set_name(X, 'Pooled')\n"
    "function cancel_it() engine.cancel_timer(T) end\n"
    "function spawn_from_pool()\n"
    "    if engine.pool_spawn(P) == nil then error('pool empty') end\n"
    "end\n"
    "function pool_must_be_empty()\n"
    "    if engine.pool_spawn(P) ~= nil then error('pool not empty') end\n"
    "end\n";

/// Re-arms only the timer between the failing reloads and the committed
/// one, leaving the pool and its taken entity as v1 made them.
constexpr const char *kRearmTimer =
    "T = engine.set_timeout(function() end, 100)\n"
    "if T == nil then error('timer missing') end\n";

/// Mutates entities and components in every way a chunk can, then fails.
constexpr const char *kFailingEcs =
    "Marker = 'failed'\n"
    "engine.set_position(E, 9, 9, 9)\n"
    "engine.set_name(E, 'Renamed')\n"
    "if engine.spawn_entity() == nil then error('spawn refused') end\n"
    "if engine.spawn_shape('cube', 1, 2, 3) == nil then\n"
    "    error('spawn_shape refused')\n"
    "end\n"
    "if engine.clone_entity(E) == nil then error('clone refused') end\n"
    "engine.destroy_entity(E)\n"
    "error('intentional reload failure')\n";

/// Sets and cancels timers, then fails.
constexpr const char *kFailingTimers =
    "Marker = 'failed'\n"
    "engine.set_timeout(function() end, 5)\n"
    "engine.set_interval(function() end, 5)\n"
    "engine.cancel_timer(T)\n"
    "error('intentional reload failure')\n";

/// Reaches every staged audio call, then fails.
constexpr const char *kFailingAudio =
    "Marker = 'failed'\n"
    "if not engine.play_sound(1) then error('play_sound refused') end\n"
    "if not engine.play_sound_at(1, 0, 0, 0) then\n"
    "    error('play_sound_at refused')\n"
    "end\n"
    "engine.set_bus_volume('music', 0.5)\n"
    "if not engine.play_music('m.ogg') then error('play_music refused') end\n"
    "engine.stop_music()\n"
    "engine.stop_sound(1)\n"
    "error('intentional reload failure')\n";

/// Takes the pool's last entity, releases the one taken earlier, and fails.
constexpr const char *kFailingPool =
    "Marker = 'failed'\n"
    "if engine.pool_spawn(P) == nil then error('pool spawn refused') end\n"
    "if not engine.pool_release(P, X) then error('pool release refused') end\n"
    "if engine.pool_create(1) ~= nil then\n"
    "    error('pool_create must be refused under reload')\n"
    "end\n"
    "error('intentional reload failure')\n";

/// Reads its own queued writes back before commit, and sees a queued
/// destroy as absent; name lookup reads the committed World.
constexpr const char *kReadYourWrites =
    "Marker = 'ryw'\n"
    "engine.set_name(E, 'Dependent')\n"
    "if engine.get_name(E) ~= 'Dependent' then\n"
    "    error('name read misses the queued write')\n"
    "end\n"
    "engine.set_position(E, 5, 6, 7)\n"
    "local x, y, z = engine.get_position(E)\n"
    "if x ~= 5 or y ~= 6 or z ~= 7 then\n"
    "    error('position read misses the queued write')\n"
    "end\n"
    "local D = engine.spawn_entity()\n"
    "if D == nil then error('spawn refused') end\n"
    "engine.set_position(D, 1, 1, 1)\n"
    "engine.destroy_entity(D)\n"
    "if engine.get_position(D) ~= nil then\n"
    "    error('a queued destroy must read as absent')\n"
    "end\n"
    "if engine.find_entity_by_name('Dependent') ~= nil then\n"
    "    error('name lookup reads the committed World only')\n"
    "end\n"
    "if engine.find_entity_by_name('Renamed') == nil then\n"
    "    error('the committed name is still found')\n"
    "end\n";

/// Interleaves queued writes with held calls; the recorder reads the
/// marker's name when each held call runs.
constexpr const char *kRequestOrder =
    "Marker = 'order'\n"
    "engine.set_name(E, 'First')\n"
    "if not engine.play_sound(1) then error('play_sound refused') end\n"
    "engine.set_name(E, 'Second')\n"
    "if not engine.play_sound(2) then error('play_sound refused') end\n"
    "engine.set_name(E, 'Third')\n";

/// Creates exactly the scope's capacity of entities, then one more, and
/// fails; the one past capacity is refused before it exists.
constexpr const char *kCreateCapacityThenFail =
    "Marker = 'failed'\n"
    "for i = 1, 256 do\n"
    "    if engine.spawn_entity() == nil then\n"
    "        error('spawn ' .. i .. ' refused inside capacity')\n"
    "    end\n"
    "end\n"
    "if engine.spawn_entity() ~= nil then\n"
    "    error('the 257th spawn must be refused')\n"
    "end\n"
    "error('intentional reload failure')\n";

/// The same, committed: exactly the capacity survives.
constexpr const char *kCreateCapacity =
    "Marker = 'created'\n"
    "for i = 1, 256 do\n"
    "    if engine.spawn_entity() == nil then\n"
    "        error('spawn ' .. i .. ' refused inside capacity')\n"
    "    end\n"
    "end\n"
    "if engine.spawn_entity() ~= nil then\n"
    "    error('the 257th spawn must be refused')\n"
    "end\n";

/// Holds exactly the journal's capacity of audio calls, then one more.
constexpr const char *kHeldCapacity =
    "Marker = 'held'\n"
    "for i = 1, 128 do\n"
    "    if not engine.play_sound(i) then\n"
    "        error('held call ' .. i .. ' refused inside capacity')\n"
    "    end\n"
    "end\n"
    "if engine.play_sound(129) then\n"
    "    error('the 129th held call must be refused')\n"
    "end\n"
    "engine.cancel_timer(T)\n";

/// Queues a write, then spins past the instruction budget.
constexpr const char *kBudgetFailure =
    "Marker = 'failed'\n"
    "engine.set_name(E, 'Budget')\n"
    "while true do end\n";

/// Adds a joint (recorded), attempts every refused effect, and fails.
constexpr const char *kFailingRefused =
    "Marker = 'failed'\n"
    "if engine.add_distance_joint(E, E2, 1) == nil then\n"
    "    error('add_distance_joint refused')\n"
    "end\n"
    "engine.set_gravity(0, -1, 0)\n"
    "if engine.remove_joint(1) ~= nil then\n"
    "    error('remove_joint must be refused under reload')\n"
    "end\n"
    "if engine.pool_create(1) ~= nil then\n"
    "    error('pool_create must be refused under reload')\n"
    "end\n"
    "if engine.game_mode_start() then\n"
    "    error('game_mode_start must be refused under reload')\n"
    "end\n"
    "if engine.push_camera(E, 0, 0, 0, 0, 0, 1, 1) then\n"
    "    error('push_camera must be refused under reload')\n"
    "end\n"
    "error('intentional reload failure')\n";

/// A held call that reports failure at commit.
constexpr const char *kCommitFailure =
    "Marker = 'commit_failure'\n"
    "engine.set_name(E, 'CommitFailed')\n"
    "if not engine.play_sound(1) then error('play_sound refused') end\n"
    "function marker_is_commit_failure()\n"
    "    if Marker ~= 'commit_failure' then error('marker ' .. Marker) end\n"
    "end\n";

/// The same effects, committed.
constexpr const char *kGood =
    "Marker = 'committed'\n"
    "engine.set_position(E, 9, 9, 9)\n"
    "engine.set_name(E, 'Renamed')\n"
    "if engine.spawn_entity() == nil then error('spawn refused') end\n"
    "engine.set_timeout(function() end, 5)\n"
    "engine.cancel_timer(T)\n"
    "if not engine.play_sound(1) then error('play_sound refused') end\n"
    "engine.stop_music()\n"
    "if not engine.pool_release(P, X) then error('pool release refused') end\n";

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext ctx;
  if (!engine::core::initialize_logging()) {
    std::fprintf(stderr, "FAIL: initialize_logging\n");
    return 1;
  }
  if (!sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    return 1;
  }
  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    sc::shutdown_scripting();
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);
  sc::RuntimeServices services =
      *serviceLocator.get_service<sc::RuntimeServices>();
  services.play_sound = &record_play_sound;
  services.play_sound_at = &record_play_sound_at;
  services.set_bus_volume = &record_set_bus_volume;
  services.play_music = &record_play_music;
  services.stop_music = &record_stop_music;
  services.stop_sound = &record_stop_sound;
  sc::bind_runtime_services(&services, serviceLocator);

  const rt::Entity marker = world->create_scene_object();
  rt::NameComponent name{};
  std::snprintf(name.name, sizeof(name.name), "%s", "Marker");
  ctx.check(world->add_name_component(marker, name), "name the marker");
  g_world = world.get();
  g_marker = marker;
  const rt::Entity second = world->create_scene_object();
  std::snprintf(name.name, sizeof(name.name), "%s", "Second");
  ctx.check(world->add_name_component(second, name), "name the second");
  ctx.check(engine::core::log_register_sink(&count_commit_failure, nullptr),
            "register the commit-failure sink");

  ctx.check(write_script(kV1), "write v1");
  ctx.check(sc::load_script(kScriptPath), "load v1");
  sc::watch_script_file(kScriptPath);
  const rt::Entity pooled = world->find_entity_by_name("Pooled");
  ctx.check(world->is_alive(pooled), "v1 took one pooled entity");
  ctx.check(sc::active_timer_ref_count() == 1U, "v1 holds one timer");
  const std::size_t aliveBefore = world->alive_entity_count();

  // --- Entities and components ---
  ctx.check(rewrite_script(kFailingEcs), "write failing ECS reload");
  sc::check_script_reload();
  rt::Transform transform{};
  ctx.check(world->get_transform(marker, &transform) &&
                (transform.position.x == 0.0F),
            "failed reload leaves the marker's transform unchanged");
  ctx.check(world->get_name_component(marker, &name) &&
                (std::strcmp(name.name, "Marker") == 0),
            "failed reload leaves the marker's name unchanged");
  ctx.check(world->is_alive(marker), "failed reload cannot destroy the marker");
  ctx.check(world->alive_entity_count() == aliveBefore,
            "failed reload leaves no spawned, shaped or cloned entity behind");
  sc::flush_deferred_mutations();
  ctx.check(world->get_transform(marker, &transform) &&
                (transform.position.x == 0.0F) && world->is_alive(marker),
            "nothing a failed reload queued applies at the next flush");

  // --- Timers ---
  ctx.check(rewrite_script(kFailingTimers), "write failing timer reload");
  sc::check_script_reload();
  ctx.check(sc::active_timer_ref_count() == 1U,
            "failed reload sets no timer and cancels none");
  ctx.check(sc::call_script_function("cancel_it"),
            "the v1 timer is still cancellable");
  ctx.check(sc::active_timer_ref_count() == 0U,
            "the held cancel never happened, so the v1 timer was live");

  // --- Audio ---
  g_recordedCount = 0U;
  ctx.check(rewrite_script(kFailingAudio), "write failing audio reload");
  sc::check_script_reload();
  ctx.check(g_recordedCount == 0U, "failed reload reaches no audio call");

  // --- Pools ---
  ctx.check(rewrite_script(kFailingPool), "write failing pool reload");
  sc::check_script_reload();
  ctx.check(world->is_alive(pooled),
            "failed reload's held pool release never happened");
  ctx.check(sc::call_script_function("spawn_from_pool"),
            "the pool entity a failed reload took is available again");
  ctx.check(sc::call_script_function("pool_must_be_empty"),
            "the pool holds no third entity");

  // --- The same effects commit exactly once ---
  ctx.check(write_script(kRearmTimer) && sc::load_script(kScriptPath),
            "re-arm the timer");
  ctx.check(sc::active_timer_ref_count() == 1U, "timer re-armed");
  const std::size_t aliveBeforeCommit = world->alive_entity_count();
  g_recordedCount = 0U;
  ctx.check(rewrite_script(kGood), "write succeeding reload");
  sc::check_script_reload();
  ctx.check(world->get_transform(marker, &transform) &&
                (transform.position.x == 9.0F),
            "succeeding reload's transform write is applied");
  ctx.check(world->get_name_component(marker, &name) &&
                (std::strcmp(name.name, "Renamed") == 0),
            "succeeding reload's name write is applied");
  ctx.check(world->alive_entity_count() == aliveBeforeCommit + 1U,
            "succeeding reload's spawn is kept");
  ctx.check(sc::active_timer_ref_count() == 1U,
            "succeeding reload's timer stays and its cancel applied");
  const char *const expected[] = {"play_sound:Renamed", "stop_music"};
  ctx.check(recorded_is(expected, 2U),
            "succeeding reload's audio calls happen once, in order, after "
            "the writes queued before them");
  ctx.check(!world->is_alive(pooled),
            "succeeding reload's pool release applied");
  sc::flush_deferred_mutations();
  ctx.check(world->alive_entity_count() == aliveBeforeCommit + 1U,
            "a later flush applies nothing twice");

  // --- Read-your-writes inside the chunk ---
  {
    const std::size_t aliveBeforeRyw = world->alive_entity_count();
    ctx.check(rewrite_script(kReadYourWrites), "write read-your-writes reload");
    sc::check_script_reload();
    ctx.check(world->get_name_component(marker, &name) &&
                  (std::strcmp(name.name, "Dependent") == 0),
              "read-your-writes chunk committed (its reads all passed)");
    ctx.check(world->get_transform(marker, &transform) &&
                  (transform.position.z == 7.0F),
              "the dependent write chain applied");
    ctx.check(world->alive_entity_count() == aliveBeforeRyw,
              "an entity spawned and destroyed in the chunk is gone");
  }

  // --- Held calls apply in request order among the queued writes ---
  {
    g_recordedCount = 0U;
    ctx.check(rewrite_script(kRequestOrder), "write request-order reload");
    sc::check_script_reload();
    const char *const ordered[] = {"play_sound:First", "play_sound:Second"};
    ctx.check(recorded_is(ordered, 2U),
              "each held call ran after the writes queued before it");
    ctx.check(world->get_name_component(marker, &name) &&
                  (std::strcmp(name.name, "Third") == 0),
              "the writes queued after the last held call applied too");
  }

  // --- Capacity: created entities, exact and one past ---
  {
    const std::size_t aliveBeforeCap = world->alive_entity_count();
    ctx.check(rewrite_script(kCreateCapacityThenFail),
              "write failing capacity reload");
    sc::check_script_reload();
    ctx.check(world->alive_entity_count() == aliveBeforeCap,
              "a failed chunk at capacity leaves no entity behind");
    ctx.check(rewrite_script(kCreateCapacity), "write capacity reload");
    sc::check_script_reload();
    ctx.check(world->alive_entity_count() == aliveBeforeCap + 256U,
              "exactly the capacity of created entities commits");
  }

  // --- Capacity: held calls, exact and one past ---
  {
    g_recordedCount = 0U;
    ctx.check(rewrite_script(kHeldCapacity), "write held-capacity reload");
    sc::check_script_reload();
    ctx.check(g_recordedCount == 128U,
              "exactly the journal's capacity of held calls commits");
    ctx.check(sc::active_timer_ref_count() == 1U,
              "the cancel past the journal's capacity was refused");
  }

  // --- Instruction budget failure rolls back ---
  {
    sc::set_sandbox_enabled(true);
    sc::set_instruction_limit(5000);
    ctx.check(rewrite_script(kBudgetFailure), "write budget-failure reload");
    sc::check_script_reload();
    sc::set_sandbox_enabled(false);
    sc::set_instruction_limit(0);
    ctx.check(world->get_name_component(marker, &name) &&
                  (std::strcmp(name.name, "Third") == 0),
              "a chunk that exhausts the instruction budget applies nothing");
  }

  // --- Refused effects and a recorded joint on a failing chunk ---
  {
    const engine::physics::PhysicsContext &physics = world->physics_context();
    const std::size_t jointsBefore = physics.jointCount;
    const engine::math::Vec3 gravityBefore = physics.gravity;
    ctx.check(rewrite_script(kFailingRefused), "write refused-effects reload");
    sc::check_script_reload();
    ctx.check(physics.jointCount == jointsBefore,
              "a failed chunk's joint is removed again");
    ctx.check((physics.gravity.x == gravityBefore.x) &&
                  (physics.gravity.y == gravityBefore.y) &&
                  (physics.gravity.z == gravityBefore.z),
              "set_gravity under reload leaves gravity unchanged");
  }

  // --- A held call that fails at commit is reported, not hidden ---
  {
    g_playSoundFails = true;
    g_recordedCount = 0U;
    g_commitFailureLogs = 0U;
    ctx.check(rewrite_script(kCommitFailure), "write commit-failure reload");
    sc::check_script_reload();
    g_playSoundFails = false;
    ctx.check(g_recordedCount == 1U, "the held call ran once at commit");
    ctx.check(g_commitFailureLogs == 1U,
              "the reload reports that a committed effect failed");
    ctx.check(world->get_name_component(marker, &name) &&
                  (std::strcmp(name.name, "CommitFailed") == 0),
              "the writes that did apply stay applied");
    ctx.check(sc::call_script_function("marker_is_commit_failure"),
              "the chunk's bindings stay in place after a commit failure");
  }

  engine::core::log_unregister_sink(&count_commit_failure, nullptr);
  static_cast<void>(std::remove(kScriptPath));
  sc::shutdown_scripting();
  engine::core::shutdown_logging();
  return ctx.finish("hotreload_transaction");
}
