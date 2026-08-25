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
// Headless, on the null device (#196), so no GPU is required. Shader
// loading then fails — cooked binaries are absent on lanes built with
// ENGINE_BGFX_SHADERC=OFF — which costs nothing here: these cases are
// about whether initialization is ATTEMPTED after shutdown, and the
// attempt announces its device before reaching any shader. The failure
// also supplies the control below, since a failed backend is the one
// state that already latched correctly.

#include "../test_harness.h"

#include <cstddef>
#include <cstring>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"
#include "engine/renderer/command_buffer.h"

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
void check_cold_flush_initializes_the_device(engine::tests::TestContext &ctx) {
  const LogTally cold = flush_and_tally();
  ctx.check(cold.deviceInitializations == 1U,
            "cold: the first flush initializes a render device");
}

/// The control, and the reason this defect stayed latent: a backend that
/// FAILED to initialize does not retry, because `failed` survives. Only
/// the wholesale reset in shutdown loses that memory. Pinning this here
/// keeps the new latch from being credited with behavior that already
/// worked, and would catch a fix that disabled retry-suppression instead.
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

} // namespace

/// Runs this executable or test program.
int main() {
  // Registered before any renderer call: the null backend is what lets
  // device initialization succeed with no GPU.
  static_cast<void>(engine::core::cvar_register_bool(
      "r_null_device", true,
      "Test/CI: replace the render device with a null backend (#196)"));
  static_cast<void>(engine::core::cvar_set_bool("r_null_device", true));
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

  engine::core::log_unregister_sink(&tally_sink, nullptr);
  return ctx.finish("renderer_lifecycle");
}
