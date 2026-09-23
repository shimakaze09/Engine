// Declares the fixed-step input snapshot shared by the input state, which
// builds one per step, and the input log, which records and replays them.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/core/input.h"
#include "engine/core/platform.h"

namespace engine::core {

inline constexpr int kMaxScancodes = kMaxKeyCode + 1;
inline constexpr int kMaxMouseButtons = 5;
inline constexpr int kMaxGamepadButtons = 16;
inline constexpr int kMaxGamepadAxes = 6;

inline constexpr std::size_t kKeyWords =
    (static_cast<std::size_t>(kMaxScancodes) + 63U) / 64U;

/// One fixed step's view of the devices: what was down when the step ended
/// and what went down or up during it. Plain data, rebuilt per step.
struct InputFrame final {
  std::array<std::uint64_t, kKeyWords> keyDown{};
  std::array<std::uint64_t, kKeyWords> keyPressed{};
  std::array<std::uint64_t, kKeyWords> keyReleased{};
  int mouseX = 0;
  int mouseY = 0;
  int mouseDeltaX = 0;
  int mouseDeltaY = 0;
  int scrollDelta = 0;
  std::uint8_t mouseDown = 0U;
  std::uint8_t mousePressed = 0U;
  std::uint8_t gamepadConnected = 0U;
  std::array<std::uint16_t, static_cast<std::size_t>(kMaxGamepads)>
      gamepadDown{};
  std::array<std::uint16_t, static_cast<std::size_t>(kMaxGamepads)>
      gamepadPressed{};
  std::array<std::array<std::int16_t, kMaxGamepadAxes>,
             static_cast<std::size_t>(kMaxGamepads)>
      gamepadAxes{};

  bool operator==(const InputFrame &) const noexcept = default;
};

/// What one fixed step read: its own snapshot and the one the action mapper
/// compares it with to find a press. The previous snapshot is the step
/// before's except after a reset, which restarts it from the live state.
struct InputStepRecord final {
  std::uint64_t tick = 0U;
  InputFrame previous{};
  InputFrame current{};
};

/// Appends the step to the open recording; no effect when none is open.
/// Called by advance_input_step once the step's snapshot is final.
void input_log_record_step(const InputStepRecord &step) noexcept;

/// The replayed step for `tick`, or nullptr when no replay is active. A
/// tick other than the log's next one ends the replay with an error, and
/// the log's last step ends it once taken.
const InputStepRecord *input_log_take_replay_step(std::uint64_t tick) noexcept;

/// Discards any open recording, leaving its destination untouched, and
/// any loaded replay. For input shutdown.
void input_log_shutdown() noexcept;

} // namespace engine::core
