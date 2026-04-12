#include "engine/runtime/timer_manager.h"

#include <cstring>

#include "engine/core/logging.h"

namespace engine::runtime {

namespace {
constexpr const char *kLogChannel = "timer";
} // namespace

TimerId TimerManager::set_timeout(float delaySeconds, Callback callback,
                                  void *userData) noexcept {
  if (callback == nullptr) {
    core::log_message(core::LogLevel::Warning, kLogChannel,
                      "set_timeout: null callback");
    return kInvalidTimerId;
  }
  for (std::size_t i = 0U; i < kMaxTimers; ++i) {
    if (!m_timers[i].active) {
      m_timers[i].fireAt = m_elapsed + delaySeconds;
      m_timers[i].interval = 0.0F;
      m_timers[i].callback = callback;
      m_timers[i].userData = userData;
      m_timers[i].repeat = false;
      m_timers[i].active = true;
      return static_cast<TimerId>(i + 1U);
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
      m_timers[i].fireAt = m_elapsed + intervalSeconds;
      m_timers[i].interval = intervalSeconds;
      m_timers[i].callback = callback;
      m_timers[i].userData = userData;
      m_timers[i].repeat = true;
      m_timers[i].active = true;
      return static_cast<TimerId>(i + 1U);
    }
  }
  core::log_message(core::LogLevel::Warning, kLogChannel,
                    "set_interval: no free timer slots");
  return kInvalidTimerId;
}

void TimerManager::cancel(TimerId id) noexcept {
  if ((id == kInvalidTimerId) || (id > kMaxTimers)) {
    return;
  }
  const std::size_t slot = static_cast<std::size_t>(id - 1U);
  if (m_timers[slot].active) {
    m_timers[slot] = Entry{};
  }
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
    // Fire callback.
    if (m_timers[i].callback != nullptr) {
      m_timers[i].callback(static_cast<TimerId>(i + 1U), m_timers[i].userData);
    }
    ++fired;
    if (m_timers[i].repeat) {
      m_timers[i].fireAt += m_timers[i].interval;
    } else {
      m_timers[i] = Entry{};
    }
  }
  return fired;
}

void TimerManager::clear() noexcept {
  for (std::size_t i = 0U; i < kMaxTimers; ++i) {
    m_timers[i] = Entry{};
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
    // Find a free slot.
    for (std::size_t s = 0U; s < kMaxTimers; ++s) {
      if (!m_timers[s].active) {
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
