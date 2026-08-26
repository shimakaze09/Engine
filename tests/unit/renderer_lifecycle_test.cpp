// Verifies the renderer backend does not lazily resurrect itself after
// shutdown_renderer (issue #318 item 2, contract #168: "no global may
// lazily resurrect a subsystem"). The backend initializes on demand from
// the first flush, and shutdown ends with `backend = BackendState{}`,
// which clears the initialized and failed flags alike — so without an
// explicit shut-down latch a stray flush is indistinguishable from a cold
// start and re-runs full initialization against the device and shader
// system that shutdown just destroyed.
//
// The observable is the device initialization itself, not a state flag:
// initialize_render_device announces every backend it actually creates
// with a "render device: " line and early-returns silently when one is
// already live, so that announcement appearing after shutdown IS the
// device call the contract forbids. It is read through the public log
// sink API, which exists to observe log_message without a second
// logging backend.
//
// The same file covers the device half of that ownership rule (issue
// #326). The render device is a subsystem of its own: engine::bootstrap
// creates it directly for swapchain-owning backends, and a run can shut
// down without anything ever flushing, so the command-buffer backend's
// state is not a witness to whether a device is live. Keying the device's
// teardown off that state leaks the device out of the engine's lifetime
// and leaves the next initialization handing back the stale one — the
// resurrection hazard above, one layer down. Those cases live here rather
// than in a file of their own because they read the device's liveness
// through the same log sink and the same public observables.
//
// Headless, on the null device (#196), so no GPU is required. Backend
// initialization is then deliberately made to FAIL, by pointing the
// shader root at a directory that holds no shaders: the cooked binary
// path is built beside the source path, so an absent root misses on
// every lane, cooked or not. That costs these cases nothing — they are
// about whether initialization is ATTEMPTED after shutdown, and the
// attempt announces its device before it reaches any shader — and it
// buys two things. The failed backend supplies the control below, since
// that is the one state which already latched correctly; and forcing the
// failure here rather than inheriting it from ENGINE_BGFX_SHADERC=OFF
// means the control cannot quietly become vacuous on a lane that does
// cook shaders, where initialization would otherwise succeed.

#include "../test_harness.h"

#include <cstddef>
#include <cstring>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/render_device.h"

namespace {

namespace rr = engine::renderer;

/// Counts what a stretch of renderer work logged, split into the device
/// announcements the contract is about and everything else.
struct LogTally final {
  std::size_t records = 0U;
  std::size_t deviceInitializations = 0U;
};

LogTally g_tally;

/// The prefix initialize_render_device uses for both the bgfx and null
/// backends; matched rather than the full text so the assertion holds
/// whichever backend a lane selects.
constexpr const char *kDeviceInitPrefix = "render device: ";

/// Log sink: tallies every record, and separately those announcing a
/// freshly created render device.
void tally_sink(engine::core::LogLevel level, const char *channel,
                const char *message, void *userData) noexcept {
  static_cast<void>(level);
  static_cast<void>(channel);
  static_cast<void>(userData);
  ++g_tally.records;
  if ((message != nullptr) &&
      (std::strncmp(message, kDeviceInitPrefix,
                    std::strlen(kDeviceInitPrefix)) == 0)) {
    ++g_tally.deviceInitializations;
  }
}

/// Runs one empty frame through the production flush entry point and
/// reports what it logged.
LogTally flush_and_tally() noexcept {
  g_tally = LogTally{};
  const rr::SceneLightData lights{};
  rr::flush_renderer(rr::CommandBufferView{}, nullptr, 0.0F, lights);
  return g_tally;
}

/// The live baseline: a cold flush really does build the backend, so the
/// post-shutdown assertions below cannot pass merely because this harness
/// never initializes anything.
///
/// It also establishes the state the control depends on. The absent
/// shader root makes the default-shader load fail, and that failure path
/// unwinds the device it had just created, so a null device afterwards is
/// public witness that initialization was attempted and left the backend
/// in its failed state — the precondition the next case reads as given.
void check_cold_flush_initializes_the_device(engine::tests::TestContext &ctx) {
  const LogTally cold = flush_and_tally();
  ctx.check(cold.deviceInitializations == 1U,
            "cold: the first flush initializes a render device");
  ctx.check(rr::render_device() == nullptr,
            "cold: the failed initialization unwound the device it made");
}

/// The control, and the reason this defect stayed latent: a backend that
/// FAILED to initialize does not retry, because `failed` survives. Only
/// the wholesale reset in shutdown loses that memory. Pinning this here
/// keeps the new latch from being credited with behavior that already
/// worked, and would catch a fix that disabled retry-suppression instead.
///
/// Load-bearing that the backend really is in the failed state and not
/// merely initialized: an already-initialized backend also returns early,
/// which would make this case assert something trivially true instead.
/// The previous case asserts that state rather than assuming it, and the
/// forced shader failure is what makes it hold on every lane.
void check_failed_backend_does_not_retry(engine::tests::TestContext &ctx) {
  const LogTally repeat = flush_and_tally();
  ctx.check(repeat.deviceInitializations == 0U,
            "control: a failed backend does not re-initialize the device");
  ctx.check(repeat.records == 0U,
            "control: a failed backend's flush is silent");
}

/// The finding: a flush issued after shutdown_renderer must not rebuild
/// the backend. On the unfixed revision this call re-initializes the
/// render device and re-runs the whole init sequence against state that
/// shutdown destroyed; here it is a no-op that says so once.
void check_flush_after_shutdown_does_not_resurrect(
    engine::tests::TestContext &ctx) {
  rr::shutdown_renderer();

  const LogTally late = flush_and_tally();
  ctx.check(late.deviceInitializations == 0U,
            "after shutdown: the flush initialized no render device");
  // Exactly the refusal and nothing else. Counting records rather than
  // matching text keeps this red on the base revision even in an
  // environment where the resurrected initialization SUCCEEDS: the
  // device announcement alone would push the count past one.
  ctx.check(late.records == 1U,
            "after shutdown: the flush is a no-op with one logged reason");

  // Repeated calls stay quiet: a caller that keeps flushing after
  // teardown must not turn one refusal into a per-frame log flood.
  const LogTally again = flush_and_tally();
  ctx.check(again.deviceInitializations == 0U,
            "after shutdown: a second flush still initializes nothing");
  ctx.check(again.records == 0U,
            "after shutdown: the refusal is logged once, not per frame");
}

/// A shut-down renderer must be revivable, or the fix would trade a
/// resurrection hazard for a permanently dead renderer across the
/// init -> shutdown -> init cycle the ownership contract requires.
void check_a_new_lifetime_re_arms_the_backend(
    engine::tests::TestContext &ctx) {
  rr::initialize_renderer();

  const LogTally revived = flush_and_tally();
  ctx.check(revived.deviceInitializations == 1U,
            "restart: a new lifetime initializes the device again");

  // And the latch re-arms with the next shutdown, so the cycle holds
  // rather than working exactly once.
  rr::shutdown_renderer();
  const LogTally afterSecondShutdown = flush_and_tally();
  ctx.check(afterSecondShutdown.deviceInitializations == 0U,
            "restart: the second shutdown latches the backend off again");
}

/// Boundary (#326): shutting down with no device live must stay a quiet
/// no-op. The state is inherited, not assumed — the case above ends with
/// a shutdown, and the failed initialization inside it unwound its device.
void check_shutdown_without_a_device_is_a_no_op(
    engine::tests::TestContext &ctx) {
  ctx.check(rr::render_device() == nullptr,
            "no device: the previous shutdown left none behind");

  g_tally = LogTally{};
  rr::shutdown_renderer();
  ctx.check(g_tally.deviceInitializations == 0U,
            "no device: shutdown initializes nothing of its own");
  ctx.check(rr::render_device() == nullptr,
            "no device: shutdown leaves the device absent");
}

/// The finding (#326): a device created outside the command-buffer
/// backend, with nothing ever flushed, must still be released by the
/// renderer's teardown — the owner releases what it acquired (#168).
/// This is bootstrap's ordering: initialize the device directly, run no
/// frame, shut down.
///
/// On the unfixed revision shutdown_renderer sees a cold backend and
/// returns before reaching the device, so the device survives the engine.
/// The consequence is read through the next initialization: it finds the
/// stale context still marked initialized and hands the same device back
/// silently, which the missing announcement reports.
void check_shutdown_releases_a_device_the_backend_never_owned(
    engine::tests::TestContext &ctx) {
  // A lifetime is open around this: the device is the subject, but a
  // renderer left latched off by the previous case would make the
  // shutdown below trivially reachable for the wrong reason.
  rr::initialize_renderer();

  g_tally = LogTally{};
  ctx.check(rr::initialize_render_device(),
            "cold backend: a bootstrap-style device initialization succeeds");
  ctx.check(g_tally.deviceInitializations == 1U,
            "cold backend: that initialization created a device");
  ctx.check(rr::render_device() != nullptr,
            "cold backend: the device is live with no frame ever flushed");

  rr::shutdown_renderer();
  ctx.check(rr::render_device() == nullptr,
            "cold backend: shutdown released the device bootstrap created");

  // Why the leak matters: an initialization after a completed shutdown
  // must build a device rather than resurrect the previous one. The
  // announcement is emitted only when a device is actually created, so
  // its absence is the stale hand-back.
  g_tally = LogTally{};
  ctx.check(rr::initialize_render_device(),
            "restart: initialization succeeds after the shutdown");
  ctx.check(g_tally.deviceInitializations == 1U,
            "restart: it created a device instead of returning the stale one");
}

/// Boundary and control (#326): the failed-backend path, which released
/// the device before this change and must keep doing so. It starts from
/// the live device the previous case leaves, opens a lifetime and flushes
/// once: initialization adopts that device, fails at the absent shader
/// root and unwinds it there, and the shutdown that follows takes the warm
/// branch. Either way the device must be gone once teardown returns.
void check_failed_backend_leaves_no_device(engine::tests::TestContext &ctx) {
  ctx.check(rr::render_device() != nullptr,
            "failed backend: the case starts with a live device");

  rr::initialize_renderer();
  static_cast<void>(flush_and_tally());
  rr::shutdown_renderer();
  ctx.check(rr::render_device() == nullptr,
            "failed backend: teardown leaves no device behind");
}

} // namespace

/// Runs this executable or test program.
int main() {
  // Registered before any renderer call: the null backend is what lets
  // device initialization succeed with no GPU.
  static_cast<void>(engine::core::cvar_register_bool(
      "r_null_device", true,
      "Test/CI: replace the render device with a null backend (#196)"));
  static_cast<void>(engine::core::cvar_set_bool("r_null_device", true));
  // Forces backend initialization to fail at the default-shader load on
  // every lane: the cooked binary path is built beside the source path,
  // so a root holding no shaders misses whether or not the lane cooked
  // any. See the file header — this is what keeps the control case from
  // going vacuous where shaders are present.
  rr::set_shader_root_path("engine_renderer_lifecycle_absent_shader_root");
  if (!engine::core::initialize_logging()) {
    return 1;
  }
  if (!engine::core::log_register_sink(&tally_sink, nullptr)) {
    return 1;
  }

  engine::tests::TestContext ctx;
  check_cold_flush_initializes_the_device(ctx);
  check_failed_backend_does_not_retry(ctx);
  check_flush_after_shutdown_does_not_resurrect(ctx);
  check_a_new_lifetime_re_arms_the_backend(ctx);
  check_shutdown_without_a_device_is_a_no_op(ctx);
  check_shutdown_releases_a_device_the_backend_never_owned(ctx);
  check_failed_backend_leaves_no_device(ctx);

  engine::core::log_unregister_sink(&tally_sink, nullptr);
  return ctx.finish("renderer_lifecycle");
}
