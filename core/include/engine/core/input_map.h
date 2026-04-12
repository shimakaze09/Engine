#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

// ---------------------------------------------------------------------------
// Input Binding — a single physical input source that can trigger an action.
// ---------------------------------------------------------------------------

enum class InputBindingType : std::uint8_t {
  Key,
  MouseButton,
  GamepadButton,
  GamepadAxis
};

struct InputBinding final {
  InputBindingType type = InputBindingType::Key;
  int code = -1;
  float axisThreshold = 0.5F;
  float axisScale = 1.0F;
};

// ---------------------------------------------------------------------------
// Input Action — a named digital trigger with multiple bindings.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxInputActions = 64U;
inline constexpr std::size_t kMaxBindingsPerAction = 8U;
inline constexpr std::size_t kMaxInputNameLen = 63U;

using ActionCallback = void (*)(const char *actionName, bool pressed,
                                void *userData) noexcept;

struct InputAction final {
  char name[kMaxInputNameLen + 1U] = {};
  InputBinding bindings[kMaxBindingsPerAction] = {};
  std::uint32_t bindingCount = 0U;
  ActionCallback callback = nullptr;
  void *userData = nullptr;
  bool occupied = false;
};

// ---------------------------------------------------------------------------
// Axis Source — a single source contributing to an axis value.
// ---------------------------------------------------------------------------

enum class AxisSourceType : std::uint8_t {
  KeyPair,
  GamepadAxis,
  MouseDeltaX,
  MouseDeltaY
};

struct InputAxisSource final {
  AxisSourceType type = AxisSourceType::KeyPair;
  int negativeKey = -1;
  int positiveKey = -1;
  int axisIndex = -1;
  float scale = 1.0F;
  float deadZone = 0.15F;
};

// ---------------------------------------------------------------------------
// Input Axis Mapping — a named analog axis with multiple sources.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxInputAxes = 64U;
inline constexpr std::size_t kMaxSourcesPerAxis = 8U;

using AxisCallback = void (*)(const char *axisName, float value,
                              void *userData) noexcept;

struct InputAxisMapping final {
  char name[kMaxInputNameLen + 1U] = {};
  InputAxisSource sources[kMaxSourcesPerAxis] = {};
  std::uint32_t sourceCount = 0U;
  AxisCallback callback = nullptr;
  void *userData = nullptr;
  bool occupied = false;
};

// ---------------------------------------------------------------------------
// InputMapper — lifecycle
// ---------------------------------------------------------------------------

bool initialize_input_mapper() noexcept;
void shutdown_input_mapper() noexcept;

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bool add_input_action(const char *name, const InputBinding *bindings,
                      std::uint32_t count) noexcept;
bool add_input_axis(const char *name, const InputAxisSource *sources,
                    std::uint32_t count) noexcept;
bool remove_input_action(const char *name) noexcept;
bool remove_input_axis(const char *name) noexcept;

// ---------------------------------------------------------------------------
// Callback registration
// ---------------------------------------------------------------------------

bool set_action_callback(const char *name, ActionCallback cb,
                         void *userData = nullptr) noexcept;
bool set_axis_callback(const char *name, AxisCallback cb,
                       void *userData = nullptr) noexcept;

// ---------------------------------------------------------------------------
// Polling (alternative to callbacks)
// ---------------------------------------------------------------------------

bool is_mapped_action_down(const char *name) noexcept;
bool is_mapped_action_pressed(const char *name) noexcept;
float mapped_axis_value(const char *name) noexcept;

// ---------------------------------------------------------------------------
// Runtime rebinding (P1-M2-C2a)
// ---------------------------------------------------------------------------

bool rebind_action(const char *actionName, std::uint32_t bindingIndex,
                   const InputBinding &newBinding) noexcept;

// ---------------------------------------------------------------------------
// Per-frame processing — called from the main input loop.
// ---------------------------------------------------------------------------

void input_mapper_process_event(const void *nativeEvent) noexcept;
void input_mapper_begin_frame() noexcept;
void input_mapper_end_frame() noexcept;

// ---------------------------------------------------------------------------
// JSON persistence (P1-M2-C2b)
// ---------------------------------------------------------------------------

bool save_input_bindings(const char *path) noexcept;
bool load_input_bindings(const char *path) noexcept;

// Save/load from in-memory buffers (for tests without file I/O).
bool save_input_bindings_to_buffer(char *buffer, std::size_t capacity,
                                   std::size_t *outSize) noexcept;
bool load_input_bindings_from_buffer(const char *buffer,
                                     std::size_t size) noexcept;

} // namespace engine::core
