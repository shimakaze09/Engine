#include <cstddef>
#include <cstdio>

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

// Mutation behavior test state.
int g_selfUnsubscribeCalls = 0;
bool g_selfUnsubscribeResult = false;

int g_unsubscribeOtherCalls = 0;
int g_unsubscribeOtherTargetCalls = 0;
bool g_unsubscribeOtherResult = false;
bool g_unsubscribeOtherAttempted = false;

int g_subscribeDuringEmitCalls = 0;
int g_subscribeDuringEmitLateCalls = 0;
bool g_subscribeDuringEmitResult = false;
bool g_subscribeDuringEmitAttempted = false;

// Nested emit behavior test state.
int g_nestedPrimaryCalls = 0;
int g_nestedObserverCalls = 0;
int g_nestedLateCalls = 0;
bool g_nestedSubscribeResult = false;
int g_nestedSequence[16] = {};
std::size_t g_nestedSequenceCount = 0U;

int g_unsubNestedPrimaryCalls = 0;
int g_unsubNestedObserverCalls = 0;
bool g_unsubNestedResult = false;

int g_deepPrimaryCalls = 0;
int g_deepObserverCalls = 0;
int g_deepSequence[16] = {};
std::size_t g_deepSequenceCount = 0U;

int g_emptyBusTouchCount = 0;

void reset_mutation_test_state() noexcept {
  g_selfUnsubscribeCalls = 0;
  g_selfUnsubscribeResult = false;

  g_unsubscribeOtherCalls = 0;
  g_unsubscribeOtherTargetCalls = 0;
  g_unsubscribeOtherResult = false;
  g_unsubscribeOtherAttempted = false;

  g_subscribeDuringEmitCalls = 0;
  g_subscribeDuringEmitLateCalls = 0;
  g_subscribeDuringEmitResult = false;
  g_subscribeDuringEmitAttempted = false;

  g_nestedPrimaryCalls = 0;
  g_nestedObserverCalls = 0;
  g_nestedLateCalls = 0;
  g_nestedSubscribeResult = false;
  g_nestedSequenceCount = 0U;
  for (std::size_t i = 0U; i < (sizeof(g_nestedSequence) / sizeof(int)); ++i) {
    g_nestedSequence[i] = 0;
  }

  g_unsubNestedPrimaryCalls = 0;
  g_unsubNestedObserverCalls = 0;
  g_unsubNestedResult = false;

  g_deepPrimaryCalls = 0;
  g_deepObserverCalls = 0;
  g_deepSequenceCount = 0U;
  for (std::size_t i = 0U; i < (sizeof(g_deepSequence) / sizeof(int)); ++i) {
    g_deepSequence[i] = 0;
  }

  g_emptyBusTouchCount = 0;
}

void record_nested_step(int code) noexcept {
  if (g_nestedSequenceCount >= (sizeof(g_nestedSequence) / sizeof(int))) {
    return;
  }

  g_nestedSequence[g_nestedSequenceCount] = code;
  ++g_nestedSequenceCount;
}

void record_deep_step(int code) noexcept {
  if (g_deepSequenceCount >= (sizeof(g_deepSequence) / sizeof(int))) {
    return;
  }

  g_deepSequence[g_deepSequenceCount] = code;
  ++g_deepSequenceCount;
}

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

void on_self_unsubscribe_event_a(const TestEventA &, void *) noexcept {
  ++g_selfUnsubscribeCalls;
  g_selfUnsubscribeResult =
      unsubscribe<TestEventA, on_self_unsubscribe_event_a>();
}

void on_unsubscribe_other_target_event_a(const TestEventA &, void *) noexcept {
  ++g_unsubscribeOtherTargetCalls;
}

void on_unsubscribe_other_event_a(const TestEventA &, void *) noexcept {
  ++g_unsubscribeOtherCalls;
  if (!g_unsubscribeOtherAttempted) {
    g_unsubscribeOtherAttempted = true;
    g_unsubscribeOtherResult =
        unsubscribe<TestEventA, on_unsubscribe_other_target_event_a>();
  }
}

void on_subscribe_late_event_a(const TestEventA &, void *) noexcept {
  ++g_subscribeDuringEmitLateCalls;
}

void on_subscribe_during_emit_event_a(const TestEventA &, void *) noexcept {
  ++g_subscribeDuringEmitCalls;
  if (!g_subscribeDuringEmitAttempted) {
    g_subscribeDuringEmitAttempted = true;
    g_subscribeDuringEmitResult =
        subscribe<TestEventA, on_subscribe_late_event_a>();
  }
}

struct NestedEvent final {
  int depth = 0;
};

void on_nested_late(const NestedEvent &event, void *) noexcept {
  ++g_nestedLateCalls;
  if (event.depth == 1) {
    record_nested_step(31);
  } else {
    record_nested_step(32);
  }
}

void on_nested_observer(const NestedEvent &event, void *) noexcept {
  ++g_nestedObserverCalls;
  if (event.depth == 1) {
    record_nested_step(21);
  } else {
    record_nested_step(22);
  }
}

void on_nested_primary(const NestedEvent &event, void *) noexcept {
  ++g_nestedPrimaryCalls;
  if (event.depth == 1) {
    record_nested_step(11);
    g_nestedSubscribeResult = subscribe<NestedEvent, on_nested_late>();
    emit(NestedEvent{2});
  } else {
    record_nested_step(12);
  }
}

void on_unsub_nested_observer(const NestedEvent &, void *) noexcept {
  ++g_unsubNestedObserverCalls;
}

void on_unsub_nested_primary(const NestedEvent &event, void *) noexcept {
  ++g_unsubNestedPrimaryCalls;
  if (event.depth == 1) {
    g_unsubNestedResult = unsubscribe<NestedEvent, on_unsub_nested_observer>();
    emit(NestedEvent{2});
  }
}

struct DeepNestedEvent final {
  int depth = 0;
};

void on_deep_nested_observer(const DeepNestedEvent &event, void *) noexcept {
  ++g_deepObserverCalls;
  record_deep_step(20 + event.depth);
}

void on_deep_nested_primary(const DeepNestedEvent &event, void *) noexcept {
  ++g_deepPrimaryCalls;
  record_deep_step(10 + event.depth);
  if (event.depth < 3) {
    emit(DeepNestedEvent{event.depth + 1});
  }
}

void on_empty_bus_raw(const void *, void *) noexcept { ++g_emptyBusTouchCount; }

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

bool test_self_unsubscribe_during_emit() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }

  reset_mutation_test_state();

  if (!subscribe<TestEventA, on_self_unsubscribe_event_a>()) {
    shutdown_event_bus();
    return false;
  }

  emit(TestEventA{1});
  if (!g_selfUnsubscribeResult || (g_selfUnsubscribeCalls != 1)) {
    shutdown_event_bus();
    return false;
  }

  emit(TestEventA{2});
  if (g_selfUnsubscribeCalls != 1) {
    shutdown_event_bus();
    return false;
  }

  shutdown_event_bus();
  return true;
}

bool test_unsubscribe_another_before_turn() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }

  reset_mutation_test_state();

  if (!subscribe<TestEventA, on_unsubscribe_other_event_a>()) {
    shutdown_event_bus();
    return false;
  }
  if (!subscribe<TestEventA, on_unsubscribe_other_target_event_a>()) {
    shutdown_event_bus();
    return false;
  }

  emit(TestEventA{1});
  // Snapshot guarantee: target still executes in this emission.
  if (!g_unsubscribeOtherResult || (g_unsubscribeOtherCalls != 1) ||
      (g_unsubscribeOtherTargetCalls != 1)) {
    shutdown_event_bus();
    return false;
  }

  emit(TestEventA{2});
  // Target is removed for subsequent emissions.
  if ((g_unsubscribeOtherCalls != 2) || (g_unsubscribeOtherTargetCalls != 1)) {
    shutdown_event_bus();
    return false;
  }

  shutdown_event_bus();
  return true;
}

bool test_subscribe_new_handler_during_emit() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }

  reset_mutation_test_state();

  if (!subscribe<TestEventA, on_subscribe_during_emit_event_a>()) {
    shutdown_event_bus();
    return false;
  }

  emit(TestEventA{1});
  // Snapshot guarantee: newly subscribed handler does not run in current emit.
  if (!g_subscribeDuringEmitResult || (g_subscribeDuringEmitCalls != 1) ||
      (g_subscribeDuringEmitLateCalls != 0)) {
    shutdown_event_bus();
    return false;
  }

  emit(TestEventA{2});
  if ((g_subscribeDuringEmitCalls != 2) ||
      (g_subscribeDuringEmitLateCalls != 1)) {
    shutdown_event_bus();
    return false;
  }

  shutdown_event_bus();
  return true;
}

bool test_nested_emit_allowed_with_fresh_snapshot() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }

  reset_mutation_test_state();

  if (!subscribe<NestedEvent, on_nested_primary>()) {
    shutdown_event_bus();
    return false;
  }
  if (!subscribe<NestedEvent, on_nested_observer>()) {
    shutdown_event_bus();
    return false;
  }

  emit(NestedEvent{1});

  // Re-entrant emit is allowed. Inner emit takes a fresh snapshot, so it sees
  // handlers subscribed by outer handlers before the inner emit call.
  if (!g_nestedSubscribeResult || (g_nestedPrimaryCalls != 2) ||
      (g_nestedObserverCalls != 2) || (g_nestedLateCalls != 1)) {
    shutdown_event_bus();
    return false;
  }

  const int expected[] = {11, 12, 22, 32, 21};
  if (g_nestedSequenceCount != (sizeof(expected) / sizeof(expected[0]))) {
    shutdown_event_bus();
    return false;
  }
  for (std::size_t i = 0U; i < g_nestedSequenceCount; ++i) {
    if (g_nestedSequence[i] != expected[i]) {
      shutdown_event_bus();
      return false;
    }
  }

  shutdown_event_bus();
  return true;
}

bool test_unsubscribe_then_nested_emit_uses_fresh_snapshot() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }

  reset_mutation_test_state();

  if (!subscribe<NestedEvent, on_unsub_nested_primary>()) {
    shutdown_event_bus();
    return false;
  }
  if (!subscribe<NestedEvent, on_unsub_nested_observer>()) {
    shutdown_event_bus();
    return false;
  }

  emit(NestedEvent{1});

  // Outer snapshot still includes observer once; inner fresh snapshot does not.
  if (!g_unsubNestedResult || (g_unsubNestedPrimaryCalls != 2) ||
      (g_unsubNestedObserverCalls != 1)) {
    shutdown_event_bus();
    return false;
  }

  shutdown_event_bus();
  return true;
}

bool test_multiple_nested_levels() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }

  reset_mutation_test_state();

  if (!subscribe<DeepNestedEvent, on_deep_nested_primary>()) {
    shutdown_event_bus();
    return false;
  }
  if (!subscribe<DeepNestedEvent, on_deep_nested_observer>()) {
    shutdown_event_bus();
    return false;
  }

  emit(DeepNestedEvent{1});

  if ((g_deepPrimaryCalls != 3) || (g_deepObserverCalls != 3)) {
    shutdown_event_bus();
    return false;
  }

  const int expected[] = {11, 12, 13, 23, 22, 21};
  if (g_deepSequenceCount != (sizeof(expected) / sizeof(expected[0]))) {
    shutdown_event_bus();
    return false;
  }
  for (std::size_t i = 0U; i < g_deepSequenceCount; ++i) {
    if (g_deepSequence[i] != expected[i]) {
      shutdown_event_bus();
      return false;
    }
  }

  shutdown_event_bus();
  return true;
}

bool test_empty_bus_edge_cases() noexcept {
  if (!initialize_event_bus()) {
    return false;
  }

  reset_mutation_test_state();

  // No subscribers for these emits.
  emit(TestEventA{7});
  emit_channel("missing", nullptr, 0U);

  if (unsubscribe<TestEventA, on_test_event_a>()) {
    shutdown_event_bus();
    return false;
  }
  if (unsubscribe_channel("missing", on_channel_int)) {
    shutdown_event_bus();
    return false;
  }

  if (subscribe_raw(nullptr, &on_empty_bus_raw, nullptr)) {
    shutdown_event_bus();
    return false;
  }
  if (unsubscribe_raw(nullptr, &on_empty_bus_raw, nullptr)) {
    shutdown_event_bus();
    return false;
  }

  // Ensure raw handler was never called by empty-bus operations.
  if (g_emptyBusTouchCount != 0) {
    shutdown_event_bus();
    return false;
  }

  shutdown_event_bus();
  return true;
}

} // namespace

int main() {
  int passed = 0;
  int failed = 0;

  auto run = [&](const char *name, bool (*fn)() noexcept) {
    if (fn()) {
      ++passed;
      std::printf("  PASS  %s\n", name);
    } else {
      ++failed;
      std::printf("  FAIL  %s\n", name);
    }
  };

  std::printf("--- event_bus tests ---\n");
  run("init_shutdown", &test_init_shutdown);
  run("typed_subscribe_emit", &test_typed_subscribe_emit);
  run("typed_unsubscribe", &test_typed_unsubscribe);
  run("multiple_subscribers", &test_multiple_subscribers);
  run("channel_subscribe_emit", &test_channel_subscribe_emit);
  run("typed_event_isolation", &test_typed_event_isolation);
  run("self_unsubscribe_during_emit", &test_self_unsubscribe_during_emit);
  run("unsubscribe_another_before_turn", &test_unsubscribe_another_before_turn);
  run("subscribe_new_handler_during_emit",
      &test_subscribe_new_handler_during_emit);
  run("nested_emit_allowed_with_fresh_snapshot",
      &test_nested_emit_allowed_with_fresh_snapshot);
  run("unsubscribe_then_nested_emit_uses_fresh_snapshot",
      &test_unsubscribe_then_nested_emit_uses_fresh_snapshot);
  run("multiple_nested_levels", &test_multiple_nested_levels);
  run("empty_bus_edge_cases", &test_empty_bus_edge_cases);

  std::printf("--- %d passed, %d failed ---\n", passed, failed);
  return (failed > 0) ? 1 : 0;
}
