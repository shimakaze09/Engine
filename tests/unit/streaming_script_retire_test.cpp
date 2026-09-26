// Retiring a script's streaming request when its upload was skipped (#546).
// An upload is skipped -- the request ends Ready while the mesh stays
// Unloaded -- when the asset stopped being wanted resident while it
// loaded. The script-handle retire pass released a request only when it
// was Failed, or Ready with the mesh Ready, so it kept this one. The
// global pass then released the queue slot underneath it, the script's
// handle went stale, and a stale handle reads as Failed: on the next frame
// the retire pass marked a correctly unloaded mesh Failed.

#include "engine_runtime_streaming.h"

#include "../test_harness.h"

#include "engine/content/asset_streaming.h"
#include "engine/core/cvar.h"
#include "engine/renderer/asset_database.h"
#include "engine/runtime/service_registry.h"

#include <chrono>
#include <memory>
#include <thread>

namespace {

using namespace engine;

bool load_ok(content::AssetId, const char *, std::uint64_t *outSize,
             void *) noexcept {
  if (outSize != nullptr) {
    *outSize = 64U;
  }
  return true;
}

/// The skipped upload: the mesh is no longer wanted resident, so it goes
/// back to Unloaded and the request still completes.
bool upload_skipped(content::AssetId id, void *userData) noexcept {
  auto *database = static_cast<renderer::AssetDatabase *>(userData);
  static_cast<void>(renderer::set_mesh_asset_state(
      database, id, content::AssetState::Unloaded,
      renderer::kInvalidMeshHandle));
  return true;
}

} // namespace

/// Runs this executable or test program.
int main() {
  engine::tests::TestContext t;
  core::initialize_cvars();
  auto database = std::make_unique<renderer::AssetDatabase>();
  auto queue = std::make_unique<content::AssetStreamingQueue>();
  t.check(content::initialize_asset_streaming(queue.get()),
          "streaming workers start");

  const content::AssetId id = content::make_asset_id_from_path("skip.mesh");
  t.check(renderer::request_mesh_asset_streaming_load(database.get(), id,
                                                      "skip.mesh"),
          "the mesh is requested");
  const content::LoadHandle request = content::load_asset_async(
      queue.get(), id, "skip.mesh", content::LoadPriority::Normal);
  t.check(request.valid(), "the load is queued");

  auto service = std::make_unique<runtime::EngineAssetDatabaseService>();
  service->database = database.get();
  service->streamingQueue = queue.get();
  service->scriptLoadHandles[0].assetId = id;
  service->scriptLoadHandles[0].streamingHandle = request;
  service->scriptLoadHandles[0].generation = 1U;
  service->scriptLoadHandles[0].occupied = true;

  for (int frame = 0;
       (frame < 2000) && (content::get_load_state(queue.get(), request) !=
                          content::LoadingState::Ready);
       ++frame) {
    content::begin_streaming_frame(queue.get());
    static_cast<void>(content::update_asset_streaming(
        queue.get(), &load_ok, &upload_skipped, database.get()));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  t.check(content::get_load_state(queue.get(), request) ==
              content::LoadingState::Ready,
          "the request completes with its upload skipped");
  t.check(renderer::mesh_asset_state(database.get(), id) ==
              content::AssetState::Unloaded,
          "the mesh is Unloaded, as it should be");

  // Two frames of the runtime's terminal pass.
  sync_streaming_failures(service.get());
  sync_streaming_failures(service.get());
  t.check(renderer::mesh_asset_state(database.get(), id) ==
              content::AssetState::Unloaded,
          "a skipped upload is not turned into a failure a frame later");
  t.check(!service->scriptLoadHandles[0].streamingHandle.valid(),
          "the script's handle lets go of the finished request");
  t.check(content::pending_load_count(queue.get()) == 0U,
          "no request is left behind");

  content::shutdown_asset_streaming(queue.get());
  core::shutdown_cvars();
  return t.finish("streaming_script_retire");
}
