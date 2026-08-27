// Verifies core logging sink registration behavior for the Engine test suite.

#include "engine/core/logging.h"
#include "../test_harness.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace {

engine::tests::TestContext g_tests;

void check(bool condition, const char *name) noexcept {
  g_tests.check(condition, name);
}

struct CapturedEvent final {
  engine::core::LogLevel level;
  char channel[32] = {};
  char message[256] = {};
};

constexpr std::size_t kMaxCaptured = 64U;
std::atomic<std::size_t> g_capturedCount{0U};
CapturedEvent g_captured[kMaxCaptured] = {};

void reset_capture() noexcept {
  g_capturedCount.store(0U, std::memory_order_relaxed);
  for (CapturedEvent &event : g_captured) {
    event = CapturedEvent{};
  }
}

/// Records events into the fixed g_captured array; drops overflow rather
/// than growing, matching the no-allocation sink contract under test.
void recording_sink(engine::core::LogLevel level, const char *channel,
                    const char *message, void *userData) noexcept {
  static_cast<void>(userData);
  const std::size_t slot =
      g_capturedCount.fetch_add(1U, std::memory_order_relaxed);
  if (slot >= kMaxCaptured) {
    return;
  }
  CapturedEvent &event = g_captured[slot];
  event.level = level;
  std::snprintf(event.channel, sizeof(event.channel), "%s",
               (channel != nullptr) ? channel : "");
  std::snprintf(event.message, sizeof(event.message), "%s",
               (message != nullptr) ? message : "");
}

/// Sink used only to occupy capacity/duplicate-registration slots; never
/// meant to observe events.
void noop_sink(engine::core::LogLevel, const char *, const char *,
              void *) noexcept {}

/// EXPECTATION: a registered sink observes log_message calls with the
/// exact level/channel/message, in call order.
void check_sink_receives_events() noexcept {
  reset_capture();
  check(engine::core::log_register_sink(&recording_sink, nullptr),
       "register recording sink");

  engine::core::log_message(engine::core::LogLevel::Warning, "physics",
                            "blocked body warning");
  engine::core::log_message(engine::core::LogLevel::Error, "scripting",
                            "lua error: boom");

  check(g_capturedCount.load(std::memory_order_relaxed) == 2U,
       "sink observed both events");
  check(g_captured[0].level == engine::core::LogLevel::Warning,
       "first event level");
  check(std::strcmp(g_captured[0].channel, "physics") == 0,
       "first event channel");
  check(std::strcmp(g_captured[0].message, "blocked body warning") == 0,
       "first event message");
  check(g_captured[1].level == engine::core::LogLevel::Error,
       "second event level");
  check(std::strcmp(g_captured[1].message, "lua error: boom") == 0,
       "second event message");

  engine::core::log_unregister_sink(&recording_sink, nullptr);
}

/// EXPECTATION: after unregistering, the sink stops observing events, and
/// duplicate registration of the same (fn, userData) pair is rejected.
void check_unregister_stops_delivery_and_rejects_duplicates() noexcept {
  reset_capture();
  check(engine::core::log_register_sink(&recording_sink, nullptr),
       "register for unregister test");
  check(!engine::core::log_register_sink(&recording_sink, nullptr),
       "duplicate registration rejected");

  engine::core::log_unregister_sink(&recording_sink, nullptr);
  engine::core::log_message(engine::core::LogLevel::Info, "editor",
                            "should not be observed");
  check(g_capturedCount.load(std::memory_order_relaxed) == 0U,
       "no events after unregister");

  // Re-registration after a clean unregister must succeed again.
  check(engine::core::log_register_sink(&recording_sink, nullptr),
       "re-register after unregister");
  engine::core::log_unregister_sink(&recording_sink, nullptr);
}

/// EXPECTATION: the fixed sink table never grows past kMaxLogSinks; the
/// (kMaxLogSinks + 1)-th registration is refused rather than allocating.
void check_sink_table_capacity_is_bounded() noexcept {
  std::vector<void *> tags;
  for (std::size_t i = 0U; i < engine::core::kMaxLogSinks; ++i) {
    void *tag = reinterpret_cast<void *>(static_cast<std::uintptr_t>(i + 1U));
    check(engine::core::log_register_sink(&noop_sink, tag),
         "sink slot accepted within capacity");
    tags.push_back(tag);
  }

  void *overflowTag = reinterpret_cast<void *>(
      static_cast<std::uintptr_t>(engine::core::kMaxLogSinks + 1U));
  check(!engine::core::log_register_sink(&noop_sink, overflowTag),
       "sink registration refused past capacity");

  for (void *tag : tags) {
    engine::core::log_unregister_sink(&noop_sink, tag);
  }
  // Capacity is available again once slots are freed.
  check(engine::core::log_register_sink(&noop_sink, overflowTag),
       "sink slot reusable after freeing");
  engine::core::log_unregister_sink(&noop_sink, overflowTag);
}

/// EXPECTATION: log_message calls from multiple threads all reach the sink
/// exactly once each; the fixed-size dispatch snapshot must not drop, tear,
/// or duplicate events under concurrent logging.
void check_sink_dispatch_is_thread_safe() noexcept {
  reset_capture();
  check(engine::core::log_register_sink(&recording_sink, nullptr),
       "register sink for concurrency test");

  constexpr int kThreadCount = 6;
  constexpr int kMessagesPerThread = 20;
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(kThreadCount));
  for (int t = 0; t < kThreadCount; ++t) {
    static_cast<void>(t);
    threads.emplace_back([]() noexcept {
      for (int i = 0; i < kMessagesPerThread; ++i) {
        engine::core::log_message(engine::core::LogLevel::Trace, "jobs",
                                  "concurrent log line");
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  const std::size_t expected =
      static_cast<std::size_t>(kThreadCount) *
      static_cast<std::size_t>(kMessagesPerThread);
  check(g_capturedCount.load(std::memory_order_relaxed) == expected,
       "every concurrent log_message call reached the sink exactly once");

  engine::core::log_unregister_sink(&recording_sink, nullptr);
}

/// Stand-in for state a sink owner reclaims once it has unregistered:
/// `retired` marks the instant the owner considers the payload dead, which
/// in production is where the memory would be freed.
struct SinkPayload final {
  std::atomic<bool> retired{false};
};

std::atomic<bool> g_gateEntered{false};
std::atomic<bool> g_gateReleased{false};
std::atomic<bool> g_payloadSinkCalled{false};
std::atomic<bool> g_payloadObservedAfterRetire{false};
std::atomic<int> g_selfUnregisterCalls{0};

/// Holds one dispatch in flight until the test releases it, so removal can
/// be attempted while a snapshot of the sink table is still being walked.
/// Blocking here is a harness-only device: production sinks owe the header's
/// fixed-size, non-blocking contract, which is what bounds how long a
/// removal can wait.
void gate_sink(engine::core::LogLevel, const char *, const char *,
               void *) noexcept {
  g_gateEntered.store(true, std::memory_order_release);
  while (!g_gateReleased.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

/// Reads its userData on every call and reports any call that arrives after
/// the owner retired that payload — the use-after-free the barrier prevents.
void payload_sink(engine::core::LogLevel, const char *, const char *,
                  void *userData) noexcept {
  auto *payload = static_cast<SinkPayload *>(userData);
  g_payloadSinkCalled.store(true, std::memory_order_release);
  if ((payload != nullptr) && payload->retired.load(std::memory_order_acquire)) {
    g_payloadObservedAfterRetire.store(true, std::memory_order_release);
  }
}

/// Removes itself from inside its own callback; the removal must complete
/// rather than wait on the dispatch that is running it.
void self_unregistering_sink(engine::core::LogLevel, const char *,
                             const char *, void *userData) noexcept {
  g_selfUnregisterCalls.fetch_add(1, std::memory_order_relaxed);
  engine::core::log_unregister_sink(&self_unregistering_sink, userData);
}

/// Runs the two-sink barrier sequence: a logging thread parks inside
/// gate_sink with payload_sink still ahead of it in the same dispatch, a
/// second thread performs `remove` and retires the payload the moment that
/// call returns, then the gate is released. Returns true when some sink call
/// observed the retired payload.
bool removal_races_in_flight_dispatch(void (*remove)(SinkPayload &),
                                      SinkPayload &payload) noexcept {
  g_gateEntered.store(false, std::memory_order_relaxed);
  g_gateReleased.store(false, std::memory_order_relaxed);
  g_payloadSinkCalled.store(false, std::memory_order_relaxed);
  g_payloadObservedAfterRetire.store(false, std::memory_order_relaxed);

  std::thread logger([]() noexcept {
    engine::core::log_message(engine::core::LogLevel::Info, "core",
                              "dispatch held in flight");
  });
  while (!g_gateEntered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  std::atomic<bool> removalReturned{false};
  std::thread remover([&]() noexcept {
    remove(payload);
    payload.retired.store(true, std::memory_order_release);
    removalReturned.store(true, std::memory_order_release);
  });

  // Settle window: gives a removal that does NOT wait for quiescence time to
  // return and retire the payload before the held dispatch resumes, so the
  // defect shows deterministically instead of by scheduling luck. A removal
  // that does wait cannot return until the release below, and no assertion
  // reads the elapsed time.
  for (int i = 0; (i < 500) && !removalReturned.load(std::memory_order_acquire);
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  g_gateReleased.store(true, std::memory_order_release);
  logger.join();
  remover.join();
  return g_payloadObservedAfterRetire.load(std::memory_order_acquire);
}

/// EXPECTATION (regression, audit #335): log_unregister_sink does not return
/// while a dispatch that snapshotted the sink is still running, so an owner
/// may release its userData as soon as the call returns.
void check_unregister_waits_for_in_flight_dispatch() noexcept {
  SinkPayload payload{};
  check(engine::core::log_register_sink(&gate_sink, nullptr),
        "register gate sink");
  check(engine::core::log_register_sink(&payload_sink, &payload),
        "register payload sink");

  const bool observedAfterRetire = removal_races_in_flight_dispatch(
      [](SinkPayload &owned) noexcept {
        engine::core::log_unregister_sink(&payload_sink, &owned);
      },
      payload);

  check(g_payloadSinkCalled.load(std::memory_order_acquire),
        "payload sink observed the in-flight event");
  check(!observedAfterRetire,
        "no sink call after log_unregister_sink released the payload");

  engine::core::log_unregister_sink(&gate_sink, nullptr);
}

/// EXPECTATION (regression, audit #335): shutdown_logging drops the sink
/// table under the same barrier, so teardown cannot strand a dispatch that
/// is still calling a sink whose owner is being destroyed.
void check_shutdown_waits_for_in_flight_dispatch() noexcept {
  SinkPayload payload{};
  check(engine::core::log_register_sink(&gate_sink, nullptr),
        "register gate sink for shutdown barrier");
  check(engine::core::log_register_sink(&payload_sink, &payload),
        "register payload sink for shutdown barrier");

  const bool observedAfterRetire = removal_races_in_flight_dispatch(
      [](SinkPayload &) noexcept { engine::core::shutdown_logging(); }, payload);

  check(g_payloadSinkCalled.load(std::memory_order_acquire),
        "payload sink observed the event shutdown raced");
  check(!observedAfterRetire,
        "no sink call after shutdown_logging released the sink table");

  check(engine::core::initialize_logging(),
        "logging re-initialized after the shutdown barrier");
}

/// EXPECTATION (regression, audit #335): a sink that unregisters itself from
/// inside its own callback completes instead of waiting on the dispatch it
/// is running in, and stops receiving events afterwards.
void check_self_unregister_completes() noexcept {
  g_selfUnregisterCalls.store(0, std::memory_order_relaxed);
  check(engine::core::log_register_sink(&self_unregistering_sink, nullptr),
        "register self-unregistering sink");

  engine::core::log_message(engine::core::LogLevel::Info, "core",
                            "sink removes itself");
  check(g_selfUnregisterCalls.load(std::memory_order_relaxed) == 1,
        "sink observed the event during which it unregistered");

  engine::core::log_message(engine::core::LogLevel::Info, "core",
                            "after self removal");
  check(g_selfUnregisterCalls.load(std::memory_order_relaxed) == 1,
        "no delivery after a sink unregisters itself");
}

/// EXPECTATION: log_set_frame_index publishes a value log_current_frame_
/// index immediately observes, independent of any sink or log_message call.
void check_frame_index_publication() noexcept {
  engine::core::log_set_frame_index(0U);
  check(engine::core::log_current_frame_index() == 0U,
       "frame index starts at published zero");
  engine::core::log_set_frame_index(42U);
  check(engine::core::log_current_frame_index() == 42U,
       "frame index reflects latest publish");
}

} // namespace

/// Runs this executable or test program.
int main() {
  check(engine::core::initialize_logging(), "initialize_logging");

  check_sink_receives_events();
  check_unregister_stops_delivery_and_rejects_duplicates();
  check_sink_table_capacity_is_bounded();
  check_sink_dispatch_is_thread_safe();
  check_unregister_waits_for_in_flight_dispatch();
  check_self_unregister_completes();
  check_frame_index_publication();
  // Runs last among the sink cases: it tears the logging system down and
  // brings it back up, so anything after it would race that transition.
  check_shutdown_waits_for_in_flight_dispatch();

  engine::core::shutdown_logging();
  return g_tests.finish("logging_sink_test");
}
