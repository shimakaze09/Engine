// Verifies core logging sink registration behavior for the Engine test suite.

#include "engine/core/logging.h"
#include "../test_harness.h"

#include <atomic>
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
  check_frame_index_publication();

  engine::core::shutdown_logging();
  return g_tests.finish("logging_sink_test");
}
