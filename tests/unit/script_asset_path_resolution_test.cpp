// Verifies that the scripting bridge resolves engine.load_asset_async's
// virtual path through the VFS mount before handing it to the streaming
// worker and the asset records, through the production entry points
// (Lua binding -> RuntimeServices -> streaming queue): with "assets"
// mounted somewhere other than the working directory, the worker opens
// the OS path under the mount while the asset id stays derived from the
// virtual path; a path under no mount is refused with a diagnostic; and
// the non-streaming manager queue receives the same resolved path.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/content/asset_streaming.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"
#include "../test_harness.h"

namespace {

constexpr const char *kTempScriptPath = "script_asset_path_temp.lua";

/// The path the streaming worker was handed for the last load.
struct RecordedLoad final {
  std::mutex mutex{};
  char path[512] = {};
  std::size_t calls = 0U;
};

/// Load callback that records the path it receives and succeeds.
bool recording_load(engine::renderer::AssetId, const char *path,
                    std::uint64_t *outSizeBytes, void *userData) noexcept {
  auto *recorded = static_cast<RecordedLoad *>(userData);
  if (recorded != nullptr) {
    std::lock_guard<std::mutex> lock(recorded->mutex);
    std::snprintf(recorded->path, sizeof(recorded->path), "%s",
                  (path != nullptr) ? path : "");
    ++recorded->calls;
  }
  if (outSizeBytes != nullptr) {
    *outSizeBytes = 16ULL;
  }
  return true;
}

/// Upload callback that succeeds without touching the GPU.
bool ok_upload(engine::renderer::AssetId, void *) noexcept { return true; }

/// Counts scripting-channel errors, so a refused path is seen to log one.
struct ErrorTally final {
  int scriptingErrors = 0;
};

void tally_errors(engine::core::LogLevel level, const char *channel,
                  const char *, void *userData) noexcept {
  auto *tally = static_cast<ErrorTally *>(userData);
  if ((tally != nullptr) && (level == engine::core::LogLevel::Error) &&
      (channel != nullptr) && (std::strcmp(channel, "scripting") == 0)) {
    ++tally->scriptingErrors;
  }
}

/// Writes the temporary Lua fixture.
bool write_script_file(const char *contents) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kTempScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kTempScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(contents);
  const bool ok = std::fwrite(contents, 1U, length, file) == length;
  return (std::fclose(file) == 0) && ok;
}

/// Finds the live queue handle for an asset id under the queue lock.
engine::content::LoadHandle
find_request_handle(engine::content::AssetStreamingQueue *queue,
                    engine::renderer::AssetId assetId) noexcept {
  std::lock_guard<std::mutex> lock(queue->mutex);
  for (std::uint32_t i = 0U;
       i < engine::content::AssetStreamingQueue::kMaxRequests; ++i) {
    if (queue->requests[i].occupied &&
        (queue->requests[i].assetId == assetId)) {
      return engine::content::LoadHandle{i, queue->requests[i].generation};
    }
  }
  return engine::content::kInvalidLoadHandle;
}

/// Drives the queue until the asset's request is terminal.
engine::content::LoadingState
pump_to_terminal(engine::content::AssetStreamingQueue *queue,
                 engine::renderer::AssetId assetId,
                 RecordedLoad *recorded) noexcept {
  const engine::content::LoadHandle handle =
      find_request_handle(queue, assetId);
  if (!handle.valid()) {
    return engine::content::LoadingState::Failed;
  }
  for (std::size_t i = 0U; i < 64U; ++i) {
    engine::content::begin_streaming_frame(queue);
    static_cast<void>(engine::content::update_asset_streaming(
        queue, &recording_load, &ok_upload, recorded));
    const engine::content::LoadingState state =
        engine::content::get_load_state(queue, handle);
    if ((state == engine::content::LoadingState::Ready) ||
        (state == engine::content::LoadingState::Failed)) {
      return state;
    }
    static_cast<void>(engine::content::wait_for_load(queue, handle, 100U));
  }
  return engine::content::get_load_state(queue, handle);
}

/// Normalizes separators so the comparison is the same on every platform.
std::string forward_slashes(std::string path) {
  for (char &c : path) {
    if (c == '\\') {
      c = '/';
    }
  }
  return path;
}

void run_checks(engine::tests::TestContext &ctx,
                const std::string &mountDir) noexcept {
  ctx.check(engine::core::initialize_vfs(), "vfs initialized");
  ctx.check(engine::core::mount("assets", mountDir.c_str()),
            "assets mounted away from the working directory");

  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::AssetManager> manager(
      new (std::nothrow) engine::renderer::AssetManager());
  std::unique_ptr<engine::content::AssetStreamingQueue> queue(
      new (std::nothrow) engine::content::AssetStreamingQueue());
  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  if (!database || !manager || !queue || !world) {
    ctx.fail("allocations");
    return;
  }
  engine::renderer::clear_asset_database(database.get());
  engine::renderer::clear_asset_manager(manager.get());
  ctx.check(engine::content::initialize_asset_streaming(queue.get()),
            "streaming initialized");
  ctx.check(engine::scripting::initialize_scripting(), "scripting up");

  engine::core::ServiceLocator locator{};
  engine::runtime::EngineAssetDatabaseService service{};
  service.database = database.get();
  service.manager = manager.get();
  service.streamingQueue = queue.get();
  ctx.check(locator.register_service<engine::runtime::EngineAssetDatabaseService>(
                &service),
            "service registered");
  engine::runtime::bind_scripting_runtime(world.get(), locator);

  const char *script =
      "function request_mounted()\n"
      "    mounted_handle = engine.load_asset_async('assets/probe.mesh', 2)\n"
      "    if mounted_handle == nil then\n"
      "        error('load_asset_async returned nil for a mounted path')\n"
      "    end\n"
      "end\n"
      "function request_unmounted()\n"
      "    if engine.load_asset_async('elsewhere/probe.mesh', 2) ~= nil then\n"
      "        error('a path under no mount was accepted')\n"
      "    end\n"
      "end\n"
      "function request_manager_path()\n"
      "    if engine.load_asset_async('assets/queued.mesh', 1) == nil then\n"
      "        error('load_asset_async returned nil for the manager path')\n"
      "    end\n"
      "end\n";
  ctx.check(write_script_file(script), "fixture written");
  ctx.check(engine::scripting::load_script(kTempScriptPath), "fixture loaded");

  // Mounted path: the worker receives the OS path under the mount, while
  // the asset record is keyed by the virtual path.
  RecordedLoad recorded{};
  ctx.check(engine::scripting::call_script_function("request_mounted"),
            "mounted request accepted");
  const engine::renderer::AssetId probeId =
      engine::renderer::make_asset_id_from_path("assets/probe.mesh");
  ctx.check(engine::renderer::mesh_asset_requested_resident(database.get(),
                                                            probeId),
            "record keyed by the virtual path");
  ctx.check(pump_to_terminal(queue.get(), probeId, &recorded) ==
                engine::content::LoadingState::Ready,
            "mounted load reached the worker and completed");
  {
    std::lock_guard<std::mutex> lock(recorded.mutex);
    const std::string expected = forward_slashes(mountDir + "/probe.mesh");
    const std::string actual = forward_slashes(recorded.path);
    ctx.check(recorded.calls == 1U, "worker ran exactly once");
    if (actual != expected) {
      std::fprintf(stderr, "worker path: '%s', expected '%s'\n",
                   actual.c_str(), expected.c_str());
    }
    ctx.check(actual == expected, "worker opened the OS path under the mount");
  }

  // Unmounted path: refused before any record or request is created, with
  // a scripting-channel error naming the cause.
  ErrorTally tally{};
  ctx.check(engine::core::log_register_sink(&tally_errors, &tally),
            "log sink registered");
  ctx.check(engine::scripting::call_script_function("request_unmounted"),
            "unmounted request refused");
  engine::core::log_unregister_sink(&tally_errors, &tally);
  ctx.check(tally.scriptingErrors == 1, "refusal logged one scripting error");
  const engine::renderer::AssetId strayId =
      engine::renderer::make_asset_id_from_path("elsewhere/probe.mesh");
  ctx.check(!engine::renderer::mesh_asset_requested_resident(database.get(),
                                                             strayId),
            "no record for the refused path");
  ctx.check(!find_request_handle(queue.get(), strayId).valid(),
            "no streaming request for the refused path");

  // Manager queue (no streaming queue bound): the request carries the
  // resolved path too.
  service.streamingQueue = nullptr;
  ctx.check(engine::scripting::call_script_function("request_manager_path"),
            "manager-path request accepted");
  const engine::renderer::AssetId queuedId =
      engine::renderer::make_asset_id_from_path("assets/queued.mesh");
  ctx.check(engine::renderer::mesh_asset_requested_resident(database.get(),
                                                            queuedId),
            "manager record keyed by the virtual path");
  bool sawQueuedRequest = false;
  bool queuedPathResolved = false;
  const std::string expectedQueued = forward_slashes(mountDir + "/queued.mesh");
  const std::size_t queuedCount =
      engine::content::pending_asset_request_count(manager.get());
  for (std::size_t i = 0U; i < queuedCount; ++i) {
    const engine::content::AssetRequest *request =
        engine::content::pending_asset_request_at(manager.get(), i);
    if ((request == nullptr) || (request->id != queuedId)) {
      continue;
    }
    sawQueuedRequest = true;
    queuedPathResolved =
        forward_slashes(request->sourcePath.data()) == expectedQueued;
  }
  ctx.check(sawQueuedRequest, "manager queue holds the request");
  ctx.check(queuedPathResolved, "manager request carries the OS path");

  engine::runtime::unbind_scripting_runtime(locator);
  engine::scripting::shutdown_scripting();
  engine::content::shutdown_asset_streaming(queue.get());
  engine::core::shutdown_vfs();
  static_cast<void>(std::remove(kTempScriptPath));
}

} // namespace

/// Runs this executable or test program.
int main() {
  static_cast<void>(engine::core::initialize_logging());
  engine::core::initialize_cvars();

  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path mountDir =
      fs::absolute(fs::path("script_asset_path_mount"), ec);
  fs::create_directories(mountDir, ec);

  engine::tests::TestContext ctx;
  if (ec) {
    ctx.fail("mount directory created");
  } else {
    run_checks(ctx, mountDir.generic_string());
  }

  fs::remove_all(mountDir, ec);
  engine::core::shutdown_cvars();
  engine::core::shutdown_logging();
  return ctx.finish("script_asset_path_resolution");
}
