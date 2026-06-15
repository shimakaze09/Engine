// Implements timer manager behavior for the Engine runtime world.

#include "engine/runtime/timer_manager.h"

#include <cstring>

#include "engine/core/logging.h"

namespace engine::runtime {

namespace {
constexpr const char *kLogChannel = "timer";
constexpr unsigned kTimerSlotBits = 16U;
constexpr TimerId kTimerSlotMask = (1U << kTimerSlotBits) - 1U;

constexpr std::uint16_t decode_generation(TimerId id) noexcept {
  return static_cast<std::uint16_t>(id >> kTimerSlotBits);
}

constexpr std::size_t decode_slot(TimerId id) noexcept {
  const TimerId slotValue = id & kTimerSlotMask;
  if ((slotValue == 0U) || (slotValue > TimerManager::kMaxTimers)) {
    return TimerManager::kInvalidTimerSlot;
  }
  return static_cast<std::size_t>(slotValue - 1U);
}

constexpr std::size_t preferred_restore_slot(TimerId id) noexcept {
  if (id == kInvalidTimerId) {
    return TimerManager::kInvalidTimerSlot;
  }
  const std::size_t encodedSlot = decode_slot(id);
  if (encodedSlot != TimerManager::kInvalidTimerSlot) {
    return encodedSlot;
  }
  if (id <= TimerManager::kMaxTimers) {
    return static_cast<std::size_t>(id - 1U);
  }
  return TimerManager::kInvalidTimerSlot;
}
} // namespace

TimerId TimerManager::make_timer_id(std::size_t slot) const noexcept {
  if (slot >= kMaxTimers) {
    return kInvalidTimerId;
  }
  const std::uint16_t generation = m_generations[slot];
  if (generation == 0U) {
    return kInvalidTimerId;
  }
  return (static_cast<TimerId>(generation) << kTimerSlotBits) |
         static_cast<TimerId>(slot + 1U);
}

void TimerManager::ensure_generation(std::size_t slot) noexcept {
  if ((slot < kMaxTimers) && (m_generations[slot] == 0U)) {
    m_generations[slot] = 1U;
  }
}

void TimerManager::advance_generation(std::size_t slot) noexcept {
  if (slot >= kMaxTimers) {
    return;
  }
  ++m_generations[slot];
  if (m_generations[slot] == 0U) {
    m_generations[slot] = 1U;
  }
}

void TimerManager::release_slot(std::size_t slot) noexcept {
  if (slot >= kMaxTimers) {
    return;
  }
  m_timers[slot] = Entry{};
  advance_generation(slot);
}

TimerId TimerManager::set_timeout(float delaySeconds, Callback callback,
                                  void *userData) noexcept {
  if (callback == nullptr) {
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "set_timeout: null callback");
    return kInvalidTimerId;
  }
  for (std::size_t i = 0U; i < kMaxTimers; ++i) {
    if (!m_timers[i].active) {
      ensure_generation(i);
      m_timers[i].fireAt = m_elapsed + delaySeconds;
      m_timers[i].interval = 0.0F;
      m_timers[i].callback = callback;
      m_timers[i].userData = userData;
      m_timers[i].repeat = false;
      m_timers[i].active = true;
      return make_timer_id(i);
    }
  }
  core::log_message(core::LogLevel::Warning, kLogChannel,
                    "set_timeout: no free timer slots");
  return kInvalidTimerId;
}

TimerId TimerManager::set_interval(float intervalSeconds, Callback callback,
                                   void *userData) noexcept {
  if (callback == nullptr) {
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "set_interval: null callback");
    return kInvalidTimerId;
  }
  if (intervalSeconds <= 0.0F) {
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "set_interval: interval must be > 0");
    return kInvalidTimerId;
  }
  for (std::size_t i = 0U; i < kMaxTimers; ++i) {
    if (!m_timers[i].active) {
      ensure_generation(i);
      m_timers[i].fireAt = m_elapsed + intervalSeconds;
      m_timers[i].interval = intervalSeconds;
      m_timers[i].callback = callback;
      m_timers[i].userData = userData;
      m_timers[i].repeat = true;
      m_timers[i].active = true;
      return make_timer_id(i);
    }
  }
  core::log_message(core::LogLevel::Warning, kLogChannel,
                    "set_interval: no free timer slots");
  return kInvalidTimerId;
}

void TimerManager::cancel(TimerId id) noexcept {
  const std::size_t slot = slot_for_id(id);
  if (slot == kInvalidTimerSlot) {
    return;
  }
  if (m_timers[slot].active) {
    release_slot(slot);
  }
}

std::size_t TimerManager::slot_for_id(TimerId id) const noexcept {
  const std::size_t slot = decode_slot(id);
  if (slot == kInvalidTimerSlot) {
    return kInvalidTimerSlot;
  }
  const std::uint16_t generation = decode_generation(id);
  if ((generation == 0U) || (m_generations[slot] != generation)) {
    return kInvalidTimerSlot;
  }
  return slot;
}

std::size_t TimerManager::tick(float dt) noexcept {
  m_elapsed += dt;
  std::size_t fired = 0U;
  for (std::size_t i = 0U; i < kMaxTimers; ++i) {
    if (!m_timers[i].active) {
      continue;
    }
    if (m_elapsed < m_timers[i].fireAt) {
      continue;
    }
    const TimerId firedId = make_timer_id(i);
    const bool wasRepeating = m_timers[i].repeat;

    if (m_timers[i].callback != nullptr) {
      m_timers[i].callback(firedId, m_timers[i].userData);
    }
    ++fired;

    if ((slot_for_id(firedId) != i) || !m_timers[i].active) {
      continue;
    }

    if (wasRepeating && m_timers[i].repeat) {
      m_timers[i].fireAt += m_timers[i].interval;
    } else {
      release_slot(i);
    }
  }
  return fired;
}

void TimerManager::clear() noexcept {
  for (std::size_t i = 0U; i < kMaxTimers; ++i) {
    if (m_timers[i].active) {
      release_slot(i);
    } else {
      m_timers[i] = Entry{};
    }
  }
  m_elapsed = 0.0F;
}

std::size_t TimerManager::active_count() const noexcept {
  std::size_t count = 0U;
  for (std::size_t i = 0U; i < kMaxTimers; ++i) {
    if (m_timers[i].active) {
      ++count;
    }
  }
  return count;
}

std::size_t TimerManager::snapshot(TimerSnapshot *out,
                                   std::size_t maxOut) const noexcept {
  if (out == nullptr) {
    return 0U;
  }
  std::size_t written = 0U;
  for (std::size_t i = 0U; (i < kMaxTimers) && (written < maxOut); ++i) {
    if (!m_timers[i].active) {
      continue;
    }
    out[written].timerId = make_timer_id(i);
    out[written].remainingSeconds = m_timers[i].fireAt - m_elapsed;
    out[written].intervalSeconds = m_timers[i].interval;
    out[written].repeat = m_timers[i].repeat;
    out[written].active = true;
    ++written;
  }
  return written;
}

std::size_t TimerManager::restore(const TimerSnapshot *in,
                                  std::size_t count) noexcept {
  if (in == nullptr) {
    return 0U;
  }
  std::size_t restored = 0U;
  for (std::size_t i = 0U; i < count; ++i) {
    if (!in[i].active) {
      continue;
    }
    const std::size_t preferredSlot = preferred_restore_slot(in[i].timerId);
    const std::uint16_t restoredGeneration = decode_generation(in[i].timerId);

    for (std::size_t attempt = 0U; attempt <= kMaxTimers; ++attempt) {
      const std::size_t s = (attempt == 0U) ? preferredSlot : (attempt - 1U);
      if (s >= kMaxTimers) {
        continue;
      }
      if (!m_timers[s].active) {
        if ((s == preferredSlot) && (restoredGeneration != 0U)) {
          m_generations[s] = restoredGeneration;
        } else {
          ensure_generation(s);
        }
        m_timers[s].fireAt = m_elapsed + in[i].remainingSeconds;
        m_timers[s].interval = in[i].intervalSeconds;
        m_timers[s].repeat = in[i].repeat;
        m_timers[s].active = true;
        m_timers[s].callback = nullptr; // Caller must re-wire.
        m_timers[s].userData = nullptr;
        ++restored;
        break;
      }
    }
  }
  return restored;
}

std::size_t TimerManager::rewire_callbacks(Callback callback,
                                           void *userData) noexcept {
  if (callback == nullptr) {
    return 0U;
  }

  std::size_t rewired = 0U;
  for (std::size_t i = 0U; i < kMaxTimers; ++i) {
    if (m_timers[i].active && (m_timers[i].callback == nullptr)) {
      m_timers[i].callback = callback;
      m_timers[i].userData = userData;
      ++rewired;
    }
  }
  return rewired;
}

const TimerManager::Entry &
TimerManager::entry_at(std::size_t index) const noexcept {
  static const Entry kEmpty{};
  if (index >= kMaxTimers) {
    return kEmpty;
  }
  return m_timers[index];
}

TimerManager::Entry &TimerManager::entry_at_mut(std::size_t index) noexcept {
  static Entry kDummy{};
  if (index >= kMaxTimers) {
    return kDummy;
  }
  return m_timers[index];
}

} // namespace engine::runtime
