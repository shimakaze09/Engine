// Integration tests for the hot-reload transaction across a chunk that
// mutates timers, audio, entities and components and then fails: every
// effect must be discarded, and the same effects from a chunk that
// succeeds must land exactly once. Runs through the production
// watch/check_script_reload path against the production bridge table
// with only the audio calls recorded.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
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
constexpr std::size_t kMaxRecorded = 32U;
char g_recorded[kMaxRecorded][32] = {};
std::size_t g_recordedCount = 0U;

void record(const char *name) noexcept {
  if (g_recordedCount < kMaxRecorded) {
    std::snprintf(g_recorded[g_recordedCount], sizeof(g_recorded[0]), "%s",
                  name);
    ++g_recordedCount;
  }
}

bool record_play_sound(std::uint32_t, float, float, bool) noexcept {
  record("play_sound");
  return true;
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
  const char *const expected[] = {"play_sound", "stop_music"};
  ctx.check(recorded_is(expected, 2U),
            "succeeding reload's audio calls happen once, in order");
  ctx.check(!world->is_alive(pooled),
            "succeeding reload's pool release applied");
  sc::flush_deferred_mutations();
  ctx.check(world->alive_entity_count() == aliveBeforeCommit + 1U,
            "a later flush applies nothing twice");

  static_cast<void>(std::remove(kScriptPath));
  sc::shutdown_scripting();
  return ctx.finish("hotreload_transaction");
}
