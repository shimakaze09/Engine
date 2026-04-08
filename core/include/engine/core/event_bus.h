#pragma once

#include <cstddef>

namespace engine::core {

// ---------------------------------------------------------------------------
// Typed Event Bus
// ---------------------------------------------------------------------------
// Events are plain structs. Each event type gets a unique EventTypeId via the
// address-of-static trick. Subscribers are synchronous (same-frame delivery).
//
// Dispatch contract:
// - emit uses a subscriber snapshot captured at call time.
// - subscribe and unsubscribe affect subsequent emits on the same event type.
// - nested emit is allowed and captures a fresh snapshot for the nested call.
// - callback order is stable within a snapshot.

using EventTypeId = const void *;

// Generate a unique compile-time id per event type.
template <typename E> EventTypeId event_type_id() noexcept {
  static const char kTag = '\0';
  return &kTag;
}

// Raw handler signature. Typed wrappers cast the pointer.
using RawEventHandler = void (*)(const void *eventData,
                                 void *userData) noexcept;

bool initialize_event_bus() noexcept;
void shutdown_event_bus() noexcept;

bool subscribe_raw(EventTypeId typeId, RawEventHandler handler,
                   void *userData) noexcept;
bool unsubscribe_raw(EventTypeId typeId, RawEventHandler handler,
                     void *userData) noexcept;
void emit_raw(EventTypeId typeId, const void *eventData) noexcept;

// Typed convenience wrappers — Handler is a NTTP to avoid function-pointer
// casts.  Usage: subscribe<MyEvent, my_handler>(userData);
namespace detail {
template <typename E, void (*Handler)(const E &, void *) noexcept>
void event_trampoline(const void *data, void *userData) noexcept {
  Handler(*static_cast<const E *>(data), userData);
}
} // namespace detail

template <typename E, void (*Handler)(const E &, void *) noexcept>
bool subscribe(void *userData = nullptr) noexcept {
  return subscribe_raw(event_type_id<E>(),
                       &detail::event_trampoline<E, Handler>, userData);
}

template <typename E, void (*Handler)(const E &, void *) noexcept>
bool unsubscribe(void *userData = nullptr) noexcept {
  return unsubscribe_raw(event_type_id<E>(),
                         &detail::event_trampoline<E, Handler>, userData);
}

template <typename E> void emit(const E &event) noexcept {
  emit_raw(event_type_id<E>(), &event);
}

// ---------------------------------------------------------------------------
// Channel Bus (for scripting / gameplay / modding)
// ---------------------------------------------------------------------------
// Named channels addressed by string. Uses FNV-1a hash internally.
//
// Dispatch contract matches typed emit: snapshot-at-call, mutations apply to
// subsequent emits, nested emits are allowed and use fresh snapshots, and
// order is stable within each snapshot.

using ChannelHandler = void (*)(const void *data, std::size_t size,
                                void *userData) noexcept;

bool subscribe_channel(const char *channelName, ChannelHandler handler,
                       void *userData = nullptr) noexcept;
bool unsubscribe_channel(const char *channelName, ChannelHandler handler,
                         void *userData = nullptr) noexcept;
void emit_channel(const char *channelName, const void *data,
                  std::size_t size) noexcept;

} // namespace engine::core
