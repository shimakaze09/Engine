// Integration tests for the deferred scene request across a hot reload: a
// chunk that queues a scene transition and then fails must leave the request
// exactly as it was, because the runtime commits it after the frame and a
// committed transition discards the unsaved live World. Runs through the
// production watch/check_script_reload and process_pending_scene_op path.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr const char *kScriptPath = "hotreload_scene_op_rollback_test.lua";
constexpr const char *kQueuedScene = "queued_scene.json";
constexpr const char *kHijackScene = "hijack_scene.json";

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

constexpr const char *kV1 = "Marker = 'v1'\n";

/// A reload that asks for a fresh scene and then fails: the request must not
/// outlive the chunk that made it.
constexpr const char *kFailingNewScene = "Marker = 'failed'\n"
                                         "engine.new_scene()\n"
                                         "error('intentional reload failure')\n";

/// A reload that overwrites an already-queued transition and then fails: the
/// original request must survive intact, path included.
constexpr const char *kFailingHijack =
    "Marker = 'failed'\n"
    "engine.load_scene('hijack_scene.json')\n"
    "error('intentional reload failure')\n";

/// A reload that queues nothing and fails: nothing may appear from nowhere.
constexpr const char *kFailingQuiet = "Marker = 'failed'\n"
                                      "error('intentional reload failure')\n";

/// A reload that asks for a fresh scene and succeeds: its request stands.
constexpr const char *kGoodNewScene = "Marker = 'committed'\n"
                                      "engine.new_scene()\n";

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

  ctx.check(write_script(kV1), "write v1");
  ctx.check(sc::load_script(kScriptPath), "load v1");
  sc::watch_script_file(kScriptPath);

  // --- A failed reload's own request must not survive it ---
  const auto authored = world->create_scene_object();
  ctx.check(world->is_alive(authored), "authored entity created");
  sc::clear_pending_scene_op();

  ctx.check(rewrite_script(kFailingNewScene), "write failing new_scene reload");
  sc::check_script_reload();
  ctx.check(!sc::has_pending_scene_op(),
            "failed reload leaves no scene request behind");

  // The production commit point. Pre-fix the request survives the rollback,
  // reset_world runs here, and the authored entity is gone.
  ctx.check(rt::process_pending_scene_op(*world),
            "post-frame commit succeeds with nothing queued");
  ctx.check(world->is_alive(authored),
            "failed reload does not erase the live World");

  // --- A request the failed chunk overwrote must come back exactly ---
  ctx.check(sc::request_scene_load(kQueuedScene), "queue a prior scene load");
  ctx.check(rewrite_script(kFailingNewScene),
            "write failing new_scene reload over a queued load");
  sc::check_script_reload();
  ctx.check(sc::has_pending_scene_op() && sc::pending_scene_op_is_load(),
            "prior load request survives a failed reload that queued new");
  ctx.check(std::strcmp(sc::get_pending_scene_path(), kQueuedScene) == 0,
            "prior load path is unchanged after a failed new_scene");

  ctx.check(rewrite_script(kFailingHijack), "write failing hijacking reload");
  sc::check_script_reload();
  ctx.check(sc::has_pending_scene_op() && sc::pending_scene_op_is_load(),
            "prior load request survives a failed reload that redirected it");
  ctx.check(std::strcmp(sc::get_pending_scene_path(), kQueuedScene) == 0,
            "a failed reload cannot redirect a queued load to its own path");
  ctx.check(std::strcmp(sc::get_pending_scene_path(), kHijackScene) != 0,
            "the hijacking path is not what a later commit would load");

  // --- Boundaries: nothing queued, and a successful request still commits ---
  ctx.check(rewrite_script(kFailingQuiet), "write failing quiet reload");
  sc::check_script_reload();
  ctx.check(sc::has_pending_scene_op() && sc::pending_scene_op_is_load() &&
                (std::strcmp(sc::get_pending_scene_path(), kQueuedScene) == 0),
            "a failed reload that queued nothing changes nothing");

  sc::clear_pending_scene_op();
  ctx.check(rewrite_script(kFailingQuiet), "rewrite failing quiet reload");
  sc::check_script_reload();
  ctx.check(!sc::has_pending_scene_op(),
            "a failed reload invents no request when none was queued");

  ctx.check(rewrite_script(kGoodNewScene), "write succeeding new_scene reload");
  sc::check_script_reload();
  ctx.check(sc::has_pending_scene_op() && sc::pending_scene_op_is_new(),
            "a succeeding reload's scene request still commits");

  sc::clear_pending_scene_op();
  static_cast<void>(std::remove(kScriptPath));
  sc::shutdown_scripting();
  return ctx.finish("hotreload_scene_op_rollback");
}
