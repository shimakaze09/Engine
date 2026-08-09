// Verifies async-load handle reclamation through the production request
// entry points: a failed editor load must not pin the database in Loading
// forever, and failed script loads must not leak streaming-queue slots
// across retries (audit H-16 / issue #85).

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/asset_manager.h"
#include "engine/renderer/asset_streaming.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/service_registry.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kTempScriptPath = "asset_recovery_temp.lua";

/// Load callback that fails every request, simulating IO failure.
bool fail_load(engine::renderer::AssetId, const char *, std::uint64_t *,
               void *) noexcept {
  return false;
}

/// Load callback that succeeds with a tiny payload.
bool ok_load(engine::renderer::AssetId, const char *,
             std::uint64_t *outSizeBytes, void *) noexcept {
  if (outSizeBytes != nullptr) {
    *outSizeBytes = 16ULL;
  }
  return true;
}

/// Upload callback that succeeds without touching the GPU.
bool ok_upload(engine::renderer::AssetId, void *) noexcept { return true; }

/// Counts occupied streaming-queue slots under the queue lock.
std::size_t occupied_request_count(
    engine::renderer::AssetStreamingQueue *queue) noexcept {
  std::lock_guard<std::mutex> lock(queue->mutex);
  std::size_t count = 0U;
  for (const auto &request : queue->requests) {
    if (request.occupied) {
      ++count;
    }
  }
  return count;
}

/// Finds the live queue handle for an asset id under the queue lock.
engine::renderer::LoadHandle find_request_handle(
    engine::renderer::AssetStreamingQueue *queue,
    engine::renderer::AssetId assetId) noexcept {
  std::lock_guard<std::mutex> lock(queue->mutex);
  for (std::uint32_t i = 0U;
       i < engine::renderer::AssetStreamingQueue::kMaxRequests; ++i) {
    if (queue->requests[i].occupied &&
        (queue->requests[i].assetId == assetId)) {
      return engine::renderer::LoadHandle{i, queue->requests[i].generation};
    }
  }
  return engine::renderer::kInvalidLoadHandle;
}

/// True when an occupied, non-Failed queue request exists for the asset.
bool has_live_request(engine::renderer::AssetStreamingQueue *queue,
                      engine::renderer::AssetId assetId) noexcept {
  std::lock_guard<std::mutex> lock(queue->mutex);
  for (const auto &request : queue->requests) {
    if (request.occupied && (request.assetId == assetId) &&
        (request.state != engine::renderer::LoadingState::Failed)) {
      return true;
    }
  }
  return false;
}

/// Drives one scheduling pass and blocks until the request is terminal.
engine::renderer::LoadingState pump_to_terminal(
    engine::renderer::AssetStreamingQueue *queue,
    engine::renderer::AssetId assetId,
    engine::renderer::AssetLoadCallback loadCallback,
    engine::renderer::AssetUploadCallback uploadCallback) noexcept {
  const engine::renderer::LoadHandle handle =
      find_request_handle(queue, assetId);
  if (!handle.valid()) {
    return engine::renderer::LoadingState::Failed;
  }
  for (std::size_t i = 0U; i < 64U; ++i) {
    engine::renderer::begin_streaming_frame(queue);
    static_cast<void>(engine::renderer::update_asset_streaming(
        queue, loadCallback, uploadCallback, nullptr));
    const engine::renderer::LoadingState state =
        engine::renderer::get_load_state(queue, handle);
    if ((state == engine::renderer::LoadingState::Ready) ||
        (state == engine::renderer::LoadingState::Failed)) {
      return state;
    }
    static_cast<void>(engine::renderer::wait_for_load(queue, handle, 100U));
  }
  return engine::renderer::get_load_state(queue, handle);
}

/// Writes the temporary Lua fixture used by the script-path checks.
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
  std::fclose(file);
  return ok;
}

/// A failed editor load must recover: retrying the same asset re-issues a
/// live streaming request instead of leaving the database Loading forever
/// behind a dead Failed slot.
int check_editor_failed_load_recovers(
    engine::renderer::AssetDatabase *database,
    engine::renderer::AssetStreamingQueue *queue) noexcept {
  engine::renderer::clear_asset_database(database);
  engine::runtime::EngineAssetDatabaseService service{};
  service.database = database;
  service.streamingQueue = queue;
  engine::runtime::set_editor_asset_service(&service);

  const char *virtualPath = "rtest/missing_recovery.mesh";
  const std::uint64_t assetId =
      engine::runtime::editor_request_mesh_asset(virtualPath);
  if (assetId == 0ULL) {
    engine::runtime::set_editor_asset_service(nullptr);
    return 20;
  }
  if (engine::renderer::mesh_asset_state(database, assetId) !=
      engine::renderer::AssetState::Loading) {
    engine::runtime::set_editor_asset_service(nullptr);
    return 21;
  }

  if (pump_to_terminal(queue, assetId, &fail_load, nullptr) !=
      engine::renderer::LoadingState::Failed) {
    engine::runtime::set_editor_asset_service(nullptr);
    return 22;
  }

  const std::uint64_t retryId =
      engine::runtime::editor_request_mesh_asset(virtualPath);
  if (retryId != assetId) {
    engine::runtime::set_editor_asset_service(nullptr);
    return 23;
  }
  if (!has_live_request(queue, assetId)) {
    engine::runtime::set_editor_asset_service(nullptr);
    return 24;
  }
  if (occupied_request_count(queue) != 1U) {
    engine::runtime::set_editor_asset_service(nullptr);
    return 25;
  }
  if (engine::renderer::mesh_asset_state(database, assetId) !=
      engine::renderer::AssetState::Loading) {
    engine::runtime::set_editor_asset_service(nullptr);
    return 26;
  }

  if (pump_to_terminal(queue, assetId, &fail_load, nullptr) !=
      engine::renderer::LoadingState::Failed) {
    engine::runtime::set_editor_asset_service(nullptr);
    return 27;
  }
  engine::runtime::set_editor_asset_service(nullptr);
  return 0;
}

/// Repeated failed script loads must reuse one streaming-queue slot per
/// asset instead of leaking a dead Failed slot on every retry, and a
/// reclaimed Ready load must stay visible as ready to scripts.
int check_script_failed_load_retries(
    engine::renderer::AssetDatabase *database,
    engine::renderer::AssetManager *manager,
    engine::renderer::AssetStreamingQueue *queue) noexcept {
  engine::renderer::clear_asset_database(database);
  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return 40;
  }
  if (!engine::scripting::initialize_scripting()) {
    return 41;
  }

  engine::core::ServiceLocator locator{};
  engine::runtime::EngineAssetDatabaseService service{};
  service.database = database;
  service.manager = manager;
  service.streamingQueue = queue;
  if (!locator.register_service<engine::runtime::EngineAssetDatabaseService>(
          &service)) {
    engine::scripting::shutdown_scripting();
    return 42;
  }
  engine::runtime::bind_scripting_runtime(world.get(), locator);

  const auto finish = [&locator](int code) noexcept {
    engine::runtime::unbind_scripting_runtime(locator);
    engine::scripting::shutdown_scripting();
    static_cast<void>(std::remove(kTempScriptPath));
    return code;
  };

  const char *script =
      "function request_failing_asset()\n"
      "    last_handle = engine.load_asset_async('rtest_script.mesh', 2)\n"
      "    if last_handle == nil then\n"
      "        error('load_asset_async returned nil')\n"
      "    end\n"
      "end\n"
      "function request_ready_asset()\n"
      "    ready_handle = engine.load_asset_async('rtest_ready.mesh', 2)\n"
      "    if ready_handle == nil then\n"
      "        error('load_asset_async returned nil')\n"
      "    end\n"
      "end\n"
      "function verify_ready_asset()\n"
      "    if not engine.is_asset_ready(ready_handle) then\n"
      "        error('reclaimed ready asset no longer reports ready')\n"
      "    end\n"
      "end\n";
  if (!write_script_file(script) ||
      !engine::scripting::load_script(kTempScriptPath)) {
    return finish(43);
  }

  const engine::renderer::AssetId scriptAssetId =
      engine::renderer::make_asset_id_from_path("rtest_script.mesh");

  for (int attempt = 0; attempt < 3; ++attempt) {
    if (!engine::scripting::call_script_function("request_failing_asset")) {
      return finish(44);
    }
    if (occupied_request_count(queue) != 1U) {
      return finish(45);
    }
    if (pump_to_terminal(queue, scriptAssetId, &fail_load, nullptr) !=
        engine::renderer::LoadingState::Failed) {
      return finish(46);
    }
  }

  if (!engine::scripting::call_script_function("request_ready_asset")) {
    return finish(47);
  }
  const engine::renderer::AssetId readyAssetId =
      engine::renderer::make_asset_id_from_path("rtest_ready.mesh");
  if (pump_to_terminal(queue, readyAssetId, &ok_load, &ok_upload) !=
      engine::renderer::LoadingState::Ready) {
    return finish(48);
  }
  if (!engine::renderer::set_mesh_asset_state(
          database, readyAssetId, engine::renderer::AssetState::Ready,
          engine::renderer::MeshHandle{1U})) {
    return finish(49);
  }

  if (!engine::scripting::call_script_function("request_failing_asset")) {
    return finish(50);
  }
  if (occupied_request_count(queue) != 1U) {
    return finish(51);
  }
  if (!engine::scripting::call_script_function("verify_ready_asset")) {
    return finish(52);
  }

  return finish(0);
}

} // namespace

int main() {
  static_cast<void>(engine::core::initialize_logging());
  engine::core::initialize_cvars();

  int result = 0;
  if (!engine::core::initialize_vfs() ||
      !engine::core::mount("rtest", ".")) {
    std::fprintf(stderr, "asset_load_recovery_test: vfs setup failed\n");
    return 1;
  }

  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  std::unique_ptr<engine::renderer::AssetManager> manager(
      new (std::nothrow) engine::renderer::AssetManager());
  if ((database == nullptr) || (manager == nullptr)) {
    engine::core::shutdown_vfs();
    return 1;
  }
  engine::renderer::clear_asset_manager(manager.get());

  for (int phase = 0; (phase < 2) && (result == 0); ++phase) {
    std::unique_ptr<engine::renderer::AssetStreamingQueue> queue(
        new (std::nothrow) engine::renderer::AssetStreamingQueue());
    if ((queue == nullptr) ||
        !engine::renderer::initialize_asset_streaming(queue.get())) {
      result = 1;
      break;
    }
    result = (phase == 0)
                 ? check_editor_failed_load_recovers(database.get(),
                                                     queue.get())
                 : check_script_failed_load_retries(database.get(),
                                                    manager.get(),
                                                    queue.get());
    engine::renderer::shutdown_asset_streaming(queue.get());
  }

  engine::core::shutdown_vfs();
  engine::core::shutdown_cvars();

  if (result != 0) {
    std::fprintf(stderr, "asset_load_recovery_test failed: %d\n", result);
    return result;
  }
  std::printf("asset_load_recovery_test: all tests passed\n");
  return 0;
}
