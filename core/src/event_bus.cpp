// Implements event bus behavior for the Engine core engine.

#include "engine/core/event_bus.h"

#include <array>
#include <cassert>
#include <cstring>

#include "engine/core/hash.h"
#include "engine/core/logging.h"

namespace engine::core {

namespace {

// ---------------------------------------------------------------------------
// Typed Bus storage
// ---------------------------------------------------------------------------
constexpr std::size_t kMaxEventTypes = 64U;
constexpr std::size_t kMaxSubscribersPerType = 16U;
[[maybe_unused]] constexpr std::uint32_t kMaxEmitDepth = 64U;

/// Stores typed subscriber data used by the engine.
struct TypedSubscriber final {
  RawEventHandler handler = nullptr;
  void *userData = nullptr;
};

constexpr std::size_t kTypedSnapshotBytes =
    sizeof(TypedSubscriber) * kMaxSubscribersPerType;
static_assert(kTypedSnapshotBytes <= 1024U,
              "typed emit snapshot must remain bounded");

/// Stores typed slot data used by the engine.
struct TypedSlot final {
  EventTypeId typeId = nullptr;
  std::array<TypedSubscriber, kMaxSubscribersPerType> subscribers{};
  std::size_t subscriberCount = 0U;
  bool active = false;
};

std::array<TypedSlot, kMaxEventTypes> g_typedSlots{};
bool g_eventBusInitialized = false;

/// Finds the matching object or resource for typed slot.
TypedSlot *find_typed_slot(EventTypeId typeId) noexcept {
  for (auto &slot : g_typedSlots) {
    if (slot.active && (slot.typeId == typeId)) {
      return &slot;
    }
  }
  return nullptr;
}

/// Finds an existing typed slot or creates one when capacity allows.
TypedSlot *find_or_create_typed_slot(EventTypeId typeId) noexcept {
  // Try to find existing.
  if (auto *slot = find_typed_slot(typeId)) {
    return slot;
  }
  // Allocate new.
  for (auto &slot : g_typedSlots) {
    if (!slot.active) {
      slot.typeId = typeId;
      slot.active = true;
      slot.subscriberCount = 0U;
      return &slot;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Channel Bus storage
// ---------------------------------------------------------------------------
constexpr std::size_t kMaxChannels = 64U;
constexpr std::size_t kMaxSubscribersPerChannel = 16U;
constexpr std::size_t kMaxChannelNameLength = 64U;

/// Stores channel subscriber data used by the engine.
struct ChannelSubscriber final {
  ChannelHandler handler = nullptr;
  void *userData = nullptr;
};

constexpr std::size_t kChannelSnapshotBytes =
    sizeof(ChannelSubscriber) * kMaxSubscribersPerChannel;
static_assert(kChannelSnapshotBytes <= 1024U,
              "channel emit snapshot must remain bounded");

/// Stores channel slot data used by the engine.
struct ChannelSlot final {
  char name[kMaxChannelNameLength] = {};
  std::uint32_t nameHash = 0U;
  std::array<ChannelSubscriber, kMaxSubscribersPerChannel> subscribers{};
  std::size_t subscriberCount = 0U;
  bool active = false;
};

std::array<ChannelSlot, kMaxChannels> g_channelSlots{};
thread_local std::uint32_t g_emitDepth = 0U;

/// Stores emit depth scope data used by the engine.
struct EmitDepthScope final {
  EmitDepthScope() noexcept {
    assert(g_emitDepth < kMaxEmitDepth);
    ++g_emitDepth;
  }

  ~EmitDepthScope() noexcept {
    if (g_emitDepth > 0U) {
      --g_emitDepth;
    }
  }
};

/// Finds the matching object or resource for channel slot.
ChannelSlot *find_channel_slot(const char *name, std::uint32_t hash) noexcept {
  for (auto &slot : g_channelSlots) {
    if (slot.active && (slot.nameHash == hash) &&
        (std::strcmp(slot.name, name) == 0)) {
      return &slot;
    }
  }
  return nullptr;
}

/// Finds an existing channel slot or creates one when capacity allows.
ChannelSlot *find_or_create_channel_slot(const char *name) noexcept {
  const std::uint32_t hash = fnv1a_32(name);
  if (auto *slot = find_channel_slot(name, hash)) {
    return slot;
  }
  const std::size_t nameLen = std::strlen(name);
  if (nameLen >= kMaxChannelNameLength) {
    return nullptr;
  }
  for (auto &slot : g_channelSlots) {
    if (!slot.active) {
      std::memcpy(slot.name, name, nameLen + 1U);
      slot.nameHash = hash;
      slot.active = true;
      slot.subscriberCount = 0U;
      return &slot;
    }
  }
  return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Typed Bus API
// ---------------------------------------------------------------------------

bool initialize_event_bus() noexcept {
  if (g_eventBusInitialized) {
    return true;
  }
  for (auto &slot : g_typedSlots) {
    slot = TypedSlot{};
  }
  for (auto &slot : g_channelSlots) {
    slot = ChannelSlot{};
  }
  g_eventBusInitialized = true;
  log_message(LogLevel::Info, "events", "Event bus initialized");
  return true;
}

/// Shuts down the owning system for event bus.
void shutdown_event_bus() noexcept {
  if (!g_eventBusInitialized) {
    return;
  }
  for (auto &slot : g_typedSlots) {
    slot = TypedSlot{};
  }
  for (auto &slot : g_channelSlots) {
    slot = ChannelSlot{};
  }
  g_eventBusInitialized = false;
}

/// Registers a subscriber or callback for raw.
bool subscribe_raw(EventTypeId typeId, RawEventHandler handler,
                   void *userData) noexcept {
  if ((typeId == nullptr) || (handler == nullptr)) {
    return false;
  }
  auto *slot = find_or_create_typed_slot(typeId);
  if (slot == nullptr) {
    log_message(LogLevel::Error, "events", "typed slot table full");
    return false;
  }
  if (slot->subscriberCount >= kMaxSubscribersPerType) {
    log_message(LogLevel::Error, "events",
                "subscriber limit reached for event type");
    return false;
  }
  auto &sub = slot->subscribers[slot->subscriberCount];
  sub.handler = handler;
  sub.userData = userData;
  ++slot->subscriberCount;
  return true;
}

/// Removes a subscriber or callback for raw.
bool unsubscribe_raw(EventTypeId typeId, RawEventHandler handler,
                     void *userData) noexcept {
  auto *slot = find_typed_slot(typeId);
  if (slot == nullptr) {
    return false;
  }
  for (std::size_t i = 0U; i < slot->subscriberCount; ++i) {
    if ((slot->subscribers[i].handler == handler) &&
        (slot->subscribers[i].userData == userData)) {
      // Swap with last and shrink.
      slot->subscribers[i] = slot->subscribers[slot->subscriberCount - 1U];
      slot->subscribers[slot->subscriberCount - 1U] = TypedSubscriber{};
      --slot->subscriberCount;
      return true;
    }
  }
  return false;
}

/// Emits an event or message to subscribers for raw.
void emit_raw(EventTypeId typeId, const void *eventData) noexcept {
  EmitDepthScope emitDepthScope{};
  static_cast<void>(emitDepthScope);

  auto *slot = find_typed_slot(typeId);
  if (slot == nullptr) {
    return;
  }
  // Snapshot subscribers so subscribe/unsubscribe during emit is safe.
  std::array<TypedSubscriber, kMaxSubscribersPerType> subscribersSnapshot{};
  const std::size_t count =
      (slot->subscriberCount <= subscribersSnapshot.size())
          ? slot->subscriberCount
          : subscribersSnapshot.size();
  for (std::size_t i = 0U; i < count; ++i) {
    subscribersSnapshot[i] = slot->subscribers[i];
  }

  for (std::size_t i = 0U; i < count; ++i) {
    const TypedSubscriber &subscriber = subscribersSnapshot[i];
    if (subscriber.handler != nullptr) {
      subscriber.handler(eventData, subscriber.userData);
    }
  }
}

// ---------------------------------------------------------------------------
// Channel Bus API
// ---------------------------------------------------------------------------

bool subscribe_channel(const char *channelName, ChannelHandler handler,
                       void *userData) noexcept {
  if ((channelName == nullptr) || (handler == nullptr)) {
    return false;
  }
  auto *slot = find_or_create_channel_slot(channelName);
  if (slot == nullptr) {
    log_message(LogLevel::Error, "events", "channel slot table full");
    return false;
  }
  if (slot->subscriberCount >= kMaxSubscribersPerChannel) {
    log_message(LogLevel::Error, "events",
                "subscriber limit reached for channel");
    return false;
  }
  auto &sub = slot->subscribers[slot->subscriberCount];
  sub.handler = handler;
  sub.userData = userData;
  ++slot->subscriberCount;
  return true;
}

/// Removes a subscriber or callback for channel.
bool unsubscribe_channel(const char *channelName, ChannelHandler handler,
                         void *userData) noexcept {
  if (channelName == nullptr) {
    return false;
  }
  const std::uint32_t hash = fnv1a_32(channelName);
  auto *slot = find_channel_slot(channelName, hash);
  if (slot == nullptr) {
    return false;
  }
  for (std::size_t i = 0U; i < slot->subscriberCount; ++i) {
    if ((slot->subscribers[i].handler == handler) &&
        (slot->subscribers[i].userData == userData)) {
      slot->subscribers[i] = slot->subscribers[slot->subscriberCount - 1U];
      slot->subscribers[slot->subscriberCount - 1U] = ChannelSubscriber{};
      --slot->subscriberCount;
      return true;
    }
  }
  return false;
}

/// Emits an event or message to subscribers for channel.
void emit_channel(const char *channelName, const void *data,
                  std::size_t size) noexcept {
  EmitDepthScope emitDepthScope{};
  static_cast<void>(emitDepthScope);

  if (channelName == nullptr) {
    return;
  }
  const std::uint32_t hash = fnv1a_32(channelName);
  auto *slot = find_channel_slot(channelName, hash);
  if (slot == nullptr) {
    return;
  }
  std::array<ChannelSubscriber, kMaxSubscribersPerChannel>
      subscribersSnapshot{};
  const std::size_t count =
      (slot->subscriberCount <= subscribersSnapshot.size())
          ? slot->subscriberCount
          : subscribersSnapshot.size();
  for (std::size_t i = 0U; i < count; ++i) {
    subscribersSnapshot[i] = slot->subscribers[i];
  }

  for (std::size_t i = 0U; i < count; ++i) {
    const ChannelSubscriber &subscriber = subscribersSnapshot[i];
    if (subscriber.handler != nullptr) {
      subscriber.handler(data, size, subscriber.userData);
    }
  }
}

} // namespace engine::core
