#include <cstddef>
#include <cstdint>

#include "engine/core/event_bus.h"

using namespace engine::core;

namespace {

struct TestEventA final {
  int value = 0;
};

struct TestEventB final {
  float x = 0.0F;
  float y = 0.0F;
};

int g_lastValueA = 0;
int g_callCountA = 0;
float g_lastX = 0.0F;
float g_lastY = 0.0F;

void on_test_event_a(const TestEventA &e, void *) noexcept {
  g_lastValueA = e.value;
  ++g_callCountA;
}

void on_test_event_b(const TestEventB &e, void *) noexcept {
  g_lastX = e.x;
  g_lastY = e.y;
}

void on_test_event_a_double(const TestEventA &e, void *) noexcept {
  g_lastValueA = e.value * 2;
}

bool test_init_shutdown() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }
  shutdown_event_bus();
  return true;
}

bool test_typed_subscribe_emit() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }

  g_lastValueA = 0;
  g_callCountA = 0;
  g_lastX = 0.0F;
  g_lastY = 0.0F;

  if (!subscribe<TestEventA, on_test_event_a>()) {
    shutdown_event_bus();
    return false;
  }
  if (!subscribe<TestEventB, on_test_event_b>()) {
    shutdown_event_bus();
    return false;
  }

  emit(TestEventA{42});
  if ((g_lastValueA != 42) || (g_callCountA != 1)) {
    shutdown_event_bus();
    return false;
  }

  emit(TestEventB{1.5F, 2.5F});
  if ((g_lastX != 1.5F) || (g_lastY != 2.5F)) {
    shutdown_event_bus();
    return false;
  }

  // Event A should not have fired again.
  if (g_callCountA != 1) {
    shutdown_event_bus();
    return false;
  }

  shutdown_event_bus();
  return true;
}

bool test_typed_unsubscribe() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }

  g_lastValueA = 0;
  g_callCountA = 0;

  subscribe<TestEventA, on_test_event_a>();
  emit(TestEventA{10});
  if (g_callCountA != 1) {
    shutdown_event_bus();
    return false;
  }

  if (!unsubscribe<TestEventA, on_test_event_a>()) {
    shutdown_event_bus();
    return false;
  }

  emit(TestEventA{20});
  // Should NOT have been called again.
  if ((g_callCountA != 1) || (g_lastValueA != 10)) {
    shutdown_event_bus();
    return false;
  }

  shutdown_event_bus();
  return true;
}

bool test_multiple_subscribers() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }

  g_lastValueA = 0;
  g_callCountA = 0;

  subscribe<TestEventA, on_test_event_a>();
  subscribe<TestEventA, on_test_event_a_double>();

  emit(TestEventA{5});

  // on_test_event_a sets g_lastValueA = 5, g_callCountA = 1
  // on_test_event_a_double sets g_lastValueA = 10
  // Both should run; last writer wins for g_lastValueA.
  if ((g_callCountA != 1) || (g_lastValueA != 10)) {
    shutdown_event_bus();
    return false;
  }

  shutdown_event_bus();
  return true;
}

int g_channelIntValue = 0;

void on_channel_int(const void *data, std::size_t size, void *) noexcept {
  if ((data != nullptr) && (size == sizeof(int))) {
    g_channelIntValue = *static_cast<const int *>(data);
  }
}

bool test_channel_subscribe_emit() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }

  g_channelIntValue = 0;

  if (!subscribe_channel("score", on_channel_int)) {
    shutdown_event_bus();
    return false;
  }

  int score = 100;
  emit_channel("score", &score, sizeof(score));
  if (g_channelIntValue != 100) {
    shutdown_event_bus();
    return false;
  }

  // Emit on unknown channel should not crash.
  emit_channel("unknown", &score, sizeof(score));

  if (!unsubscribe_channel("score", on_channel_int)) {
    shutdown_event_bus();
    return false;
  }

  score = 200;
  emit_channel("score", &score, sizeof(score));
  // Should still be 100 after unsubscribe.
  if (g_channelIntValue != 100) {
    shutdown_event_bus();
    return false;
  }

  shutdown_event_bus();
  return true;
}

bool test_typed_event_isolation() noexcept {
  // Verify that emitting EventA does not trigger EventB subscribers.
  if (!initialize_event_bus()) {
    return false;
  }

  g_lastValueA = 0;
  g_lastX = 0.0F;

  subscribe<TestEventA, on_test_event_a>();
  subscribe<TestEventB, on_test_event_b>();

  emit(TestEventA{99});
  if ((g_lastValueA != 99) || (g_lastX != 0.0F)) {
    shutdown_event_bus();
    return false;
  }

  shutdown_event_bus();
  return true;
}

} // namespace

int main() {
  if (!test_init_shutdown()) {
    return 1;
  }
  if (!test_typed_subscribe_emit()) {
    return 2;
  }
  if (!test_typed_unsubscribe()) {
    return 3;
  }
  if (!test_multiple_subscribers()) {
    return 4;
  }
  if (!test_channel_subscribe_emit()) {
    return 5;
  }
  if (!test_typed_event_isolation()) {
    return 6;
  }
  return 0;
}
