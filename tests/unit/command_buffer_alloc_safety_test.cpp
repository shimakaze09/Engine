// Verifies the renderer command-buffer flush path's per-frame scratch
// buffers (BackendState::instanceAttributes) survive an allocation failure
// instead of terminating the process (audit #204: a noexcept-declared
// function that trusts a throwing std::vector growth op aborts the whole
// engine under the -fno-exceptions build when the allocator fails, instead
// of returning the recoverable false its bool return already promises).
// Links the real upload_instance_matrices from command_buffer_flush_uniforms
// against a stub RenderDevice so the production growth-guard logic runs
// headlessly.

#include "command_buffer_context.h"
#include "command_buffer_flush_internal.h"
#include "engine/core/nothrow_buffer.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/render_device.h"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

using namespace engine::renderer;

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

std::uint32_t g_createdBuffer = 0U;
const void *g_lastUploadData = nullptr;
std::ptrdiff_t g_lastUploadBytes = 0;

DeviceBufferHandle fake_create_buffer(const BufferDesc &) noexcept {
  return DeviceBufferHandle{++g_createdBuffer};
}
void fake_update_buffer(DeviceBufferHandle, const void *data,
                        std::ptrdiff_t bytes) noexcept {
  g_lastUploadData = data;
  g_lastUploadBytes = bytes;
}
bool fake_set_geometry_instance_stream(DeviceGeometryHandle,
                                       DeviceBufferHandle,
                                       const VertexLayout &) noexcept {
  return true;
}
void fake_draw_indexed_instanced(DeviceGeometryHandle, std::int32_t,
                                 std::int32_t) noexcept {}

/// Builds a device table stubbing exactly the entry points instanced
/// attribute upload uses.
RenderDevice make_fake_device() noexcept {
  RenderDevice device{};
  device.caps.instancing = true;
  device.create_buffer = &fake_create_buffer;
  device.update_buffer = &fake_update_buffer;
  device.set_geometry_instance_stream = &fake_set_geometry_instance_stream;
  device.draw_indexed_instanced = &fake_draw_indexed_instanced;
  return device;
}

DrawCommand make_command(float modelTranslationX) noexcept {
  DrawCommand command{};
  command.modelMatrix = engine::math::Mat4();
  command.modelMatrix.columns[3].x = modelTranslationX;
  return command;
}

/// EXPECTATION: a normal small batch uploads exactly batch.count instance
/// attributes with the batch's own model matrices.
void test_happy_path_uploads_batch() noexcept {
  BackendState backend{};
  const RenderDevice device = make_fake_device();

  DrawCommand commands[3] = {make_command(1.0F), make_command(2.0F),
                             make_command(3.0F)};
  CommandBufferView view{commands, 3U};
  GpuMesh mesh{};
  mesh.geometry = DeviceGeometryHandle{7U};
  mesh.indexCount = 36U;
  StaticMeshBatch batch{.first = 0U, .count = 3U};

  const bool ok = upload_instance_matrices(backend, &device, mesh, view, batch);
  CHECK(ok, "instanced upload succeeds with a normal batch");
  CHECK(backend.instanceAttributes.size() >= 3U,
        "buffer grows to at least the batch count");
  CHECK(backend.instanceAttributes[0].model.columns[3].x == 1.0F,
        "first instance matches the first command");
  CHECK(backend.instanceAttributes[2].model.columns[3].x == 3.0F,
        "third instance matches the third command");
  CHECK(g_lastUploadBytes >=
            static_cast<std::ptrdiff_t>(3U * sizeof(InstanceAttributes)),
        "upload covers at least the batch's byte range");
}

/// EXPECTATION (the grow-only capacity cache introduced by audit #204's
/// fix): a smaller batch after a larger one reuses the already-grown buffer
/// without shrinking, and the leading entries it actually reads back are
/// this batch's data, not stale data left over from the larger batch —
/// proving the capacity-reuse change did not corrupt the smaller batch's
/// instance data even though excess trailing capacity is uploaded too.
void test_shrinking_batch_reuses_capacity_safely() noexcept {
  BackendState backend{};
  const RenderDevice device = make_fake_device();

  DrawCommand bigBatch[5] = {make_command(10.0F), make_command(11.0F),
                            make_command(12.0F), make_command(13.0F),
                            make_command(14.0F)};
  CommandBufferView bigView{bigBatch, 5U};
  GpuMesh mesh{};
  mesh.geometry = DeviceGeometryHandle{7U};
  mesh.indexCount = 36U;
  StaticMeshBatch bigStaticBatch{.first = 0U, .count = 5U};
  CHECK(upload_instance_matrices(backend, &device, mesh, bigView,
                                 bigStaticBatch),
        "larger batch uploads and grows the buffer");
  const std::size_t grownCapacity = backend.instanceAttributes.size();
  CHECK(grownCapacity >= 5U, "buffer capacity reflects the larger batch");

  DrawCommand smallBatch[2] = {make_command(20.0F), make_command(21.0F)};
  CommandBufferView smallView{smallBatch, 2U};
  StaticMeshBatch smallStaticBatch{.first = 0U, .count = 2U};
  CHECK(upload_instance_matrices(backend, &device, mesh, smallView,
                                 smallStaticBatch),
        "smaller batch reuses the still-large buffer");
  CHECK(backend.instanceAttributes.size() == grownCapacity,
        "capacity is not shrunk back down (grow-only cache)");
  CHECK(backend.instanceAttributes[0].model.columns[3].x == 20.0F,
        "first instance is the smaller batch's own data, not stale");
  CHECK(backend.instanceAttributes[1].model.columns[3].x == 21.0F,
        "second instance is the smaller batch's own data, not stale");
}

/// EXPECTATION: the exact core::NothrowBuffer<InstanceAttributes>
/// specialization now backing BackendState::instanceAttributes (previously
/// std::vector<InstanceAttributes>::resize, which terminates the process on
/// allocation failure under the engine's -fno-exceptions build) reports
/// failure instead of terminating when asked for a byte size that cannot be
/// represented. batch.count is a std::uint32_t bounded well under this
/// threshold by CommandBufferBuilder::kMaxDrawCommands (16384), so a real
/// hostile scene cannot reach this exact overflow through
/// upload_instance_matrices; this proves the underlying allocation
/// primitive the fix now routes through is nothrow-safe (the same
/// core::NothrowBuffer<T> mechanism audit #174 proved red/green for
/// AnimationClip::payload), matching this issue's closure requirement to
/// migrate the noexcept hit to a nothrow-safe pattern.
void test_unallocatable_size_returns_false_instead_of_terminating() noexcept {
  engine::core::NothrowBuffer<InstanceAttributes> buffer;
  const std::size_t unrepresentableCount =
      (std::numeric_limits<std::size_t>::max() / sizeof(InstanceAttributes)) +
      1U;

  const bool ok = buffer.allocate(unrepresentableCount);

  CHECK(!ok, "allocate() reports failure instead of terminating");
  CHECK(buffer.empty(), "buffer is left empty on allocation failure");
  CHECK(buffer.data() == nullptr, "no dangling/partial allocation remains");
}

} // namespace

/// Runs this executable or test program.
int main() {
  std::printf("=== Command Buffer Allocation Safety Unit Tests ===\n");

  test_happy_path_uploads_batch();
  test_shrinking_batch_reuses_capacity_safely();
  test_unallocatable_size_returns_false_instead_of_terminating();

  std::printf("\n%s (%d failure(s))\n",
              g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
