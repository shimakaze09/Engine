#include "engine/core/event_bus.h"

#include <array>
#include <cstring>

#include "engine/core/logging.h"

namespace engine::core {

namespace {

// ---------------------------------------------------------------------------
// Typed Bus storage
// ---------------------------------------------------------------------------
constexpr std::size_t kMaxEventTypes = 64U;
constexpr std::size_t kMaxSubscribersPerType = 16U;

struct TypedSubscriber final {
  RawEventHandler handler = nullptr;
  void *userData = nullptr;
};

struct TypedSlot final {
  EventTypeId typeId = nullptr;
  std::array<TypedSubscriber, kMaxSubscribersPerType> subscribers{};
  std::size_t subscriberCount = 0U;
  bool active = false;
};

std::array<TypedSlot, kMaxEventTypes> g_typedSlots{};
bool g_eventBusInitialized = false;

TypedSlot *find_typed_slot(EventTypeId typeId) noexcept {
  for (auto &slot : g_typedSlots) {
    if (slot.active && (slot.typeId == typeId)) {
      return &slot;
    }
  }
  return nullptr;
}

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

struct ChannelSubscriber final {
  ChannelHandler handler = nullptr;
  void *userData = nullptr;
};

struct ChannelSlot final {
  char name[kMaxChannelNameLength] = {};
  std::uint32_t nameHash = 0U;
  std::array<ChannelSubscriber, kMaxSubscribersPerChannel> subscribers{};
  std::size_t subscriberCount = 0U;
  bool active = false;
};

std::array<ChannelSlot, kMaxChannels> g_channelSlots{};

std::uint32_t fnv1a_hash(const char *str) noexcept {
  std::uint32_t hash = 2166136261U;
  while (*str != '\0') {
    hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(*str));
    hash *= 16777619U;
    ++str;
  }
  return hash;
}

ChannelSlot *find_channel_slot(const char *name, std::uint32_t hash) noexcept {
  for (auto &slot : g_channelSlots) {
    if (slot.active && (slot.nameHash == hash)
        && (std::strcmp(slot.name, name) == 0)) {
      return &slot;
    }
  }
  return nullptr;
}

ChannelSlot *find_or_create_channel_slot(const char *name) noexcept {
  const std::uint32_t hash = fnv1a_hash(name);
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

bool subscribe_raw(EventTypeId typeId,
                   RawEventHandler handler,
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
    log_message(
        LogLevel::Error, "events", "subscriber limit reached for event type");
    return false;
  }
  auto &sub = slot->subscribers[slot->subscriberCount];
  sub.handler = handler;
  sub.userData = userData;
  ++slot->subscriberCount;
  return true;
}

bool unsubscribe_raw(EventTypeId typeId,
                     RawEventHandler handler,
                     void *userData) noexcept {
  auto *slot = find_typed_slot(typeId);
  if (slot == nullptr) {
    return false;
  }
  for (std::size_t i = 0U; i < slot->subscriberCount; ++i) {
    if ((slot->subscribers[i].handler == handler)
        && (slot->subscribers[i].userData == userData)) {
      // Swap with last and shrink.
      slot->subscribers[i] = slot->subscribers[slot->subscriberCount - 1U];
      slot->subscribers[slot->subscriberCount - 1U] = TypedSubscriber{};
      --slot->subscriberCount;
      return true;
    }
  }
  return false;
}

void emit_raw(EventTypeId typeId, const void *eventData) noexcept {
  auto *slot = find_typed_slot(typeId);
  if (slot == nullptr) {
    return;
  }
  // Copy count to avoid issues if a handler subscribes/unsubscribes during
  // emission.
  const std::size_t count = slot->subscriberCount;
  for (std::size_t i = 0U; i < count; ++i) {
    slot->subscribers[i].handler(eventData, slot->subscribers[i].userData);
  }
}

// ---------------------------------------------------------------------------
// Channel Bus API
// ---------------------------------------------------------------------------

bool subscribe_channel(const char *channelName,
                       ChannelHandler handler,
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
    log_message(
        LogLevel::Error, "events", "subscriber limit reached for channel");
    return false;
  }
  auto &sub = slot->subscribers[slot->subscriberCount];
  sub.handler = handler;
  sub.userData = userData;
  ++slot->subscriberCount;
  return true;
}

bool unsubscribe_channel(const char *channelName,
                         ChannelHandler handler,
                         void *userData) noexcept {
  if (channelName == nullptr) {
    return false;
  }
  const std::uint32_t hash = fnv1a_hash(channelName);
  auto *slot = find_channel_slot(channelName, hash);
  if (slot == nullptr) {
    return false;
  }
  for (std::size_t i = 0U; i < slot->subscriberCount; ++i) {
    if ((slot->subscribers[i].handler == handler)
        && (slot->subscribers[i].userData == userData)) {
      slot->subscribers[i] = slot->subscribers[slot->subscriberCount - 1U];
      slot->subscribers[slot->subscriberCount - 1U] = ChannelSubscriber{};
      --slot->subscriberCount;
      return true;
    }
  }
  return false;
}

void emit_channel(const char *channelName,
                  const void *data,
                  std::size_t size) noexcept {
  if (channelName == nullptr) {
    return;
  }
  const std::uint32_t hash = fnv1a_hash(channelName);
  auto *slot = find_channel_slot(channelName, hash);
  if (slot == nullptr) {
    return;
  }
  const std::size_t count = slot->subscriberCount;
  for (std::size_t i = 0U; i < count; ++i) {
    slot->subscribers[i].handler(data, size, slot->subscribers[i].userData);
  }
}

} // namespace engine::core
