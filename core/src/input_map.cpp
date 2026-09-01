// Implements input map behavior for the Engine core engine.

#include "engine/core/input_map.h"
#include "engine/core/atomic_file.h"
#include "engine/core/input.h"
#include "engine/core/json.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace engine::core {

namespace {

constexpr const char *kLogChannel = "InputMapper";

bool g_mapperInitialized = false;

std::array<InputAction, kMaxInputActions> g_mappedActions{};
std::array<InputAxisMapping, kMaxInputAxes> g_mappedAxes{};

// Previous-frame action state for pressed/released detection.
std::array<bool, kMaxInputActions> g_actionDown{};
std::array<bool, kMaxInputActions> g_prevActionDown{};

/// Staging for a parsed bindings document; committed only after it validates.
struct StagedBindings final {
  std::array<InputAction, kMaxInputActions> actions{};
  std::array<InputAxisMapping, kMaxInputAxes> axes{};
};

/// Copies a parsed entry name after validating presence and length; a
/// missing, empty, or overlong name rejects the whole load instead of
/// committing a truncated or blank identity (audit M-11).
bool parse_entry_name(const JsonParser &parser, const JsonValue &entry,
                      char *outName, const char *what) noexcept {
  JsonValue nameVal{};
  const char *str = nullptr;
  std::size_t strLen = 0U;
  if (!parser.get_object_field(entry, "name", &nameVal) ||
      !parser.as_string(nameVal, &str, &strLen) || (strLen == 0U) ||
      (strLen > kMaxInputNameLen)) {
    char msg[128] = {};
    std::snprintf(msg, sizeof(msg),
                  "load_input_bindings: %s entry has a missing/invalid name",
                  what);
    log_message(LogLevel::Error, kLogChannel, msg);
    return false;
  }
  std::memcpy(outName, str, strLen);
  outName[strLen] = '\0';
  return true;
}

// Current-frame mouse delta (accumulated).
float g_mouseDeltaX = 0.0F;
float g_mouseDeltaY = 0.0F;

/// Finds the matching object or resource for mapped action.
InputAction *find_mapped_action(const char *name) noexcept {
  if (name == nullptr) {
    return nullptr;
  }
  for (auto &a : g_mappedActions) {
    if (a.occupied && (std::strcmp(a.name, name) == 0)) {
      return &a;
    }
  }
  return nullptr;
}

/// Finds the matching object or resource for mapped axis.
InputAxisMapping *find_mapped_axis(const char *name) noexcept {
  if (name == nullptr) {
    return nullptr;
  }
  for (auto &a : g_mappedAxes) {
    if (a.occupied && (std::strcmp(a.name, name) == 0)) {
      return &a;
    }
  }
  return nullptr;
}

bool evaluate_binding(const InputBinding &binding) noexcept {
  switch (binding.type) {
  case InputBindingType::Key:
    return (binding.code >= 0) && is_key_down(binding.code);
  case InputBindingType::MouseButton:
    return (binding.code >= 0) && is_mouse_button_down(binding.code);
  case InputBindingType::GamepadButton:
    return is_gamepad_connected() && is_gamepad_button_down(binding.code);
  case InputBindingType::GamepadAxis: {
    if (!is_gamepad_connected()) {
      return false;
    }
    const float raw = gamepad_axis_value(binding.code);
    const float scaled = raw * binding.axisScale;
    return (scaled >= binding.axisThreshold) ||
           (scaled <= -binding.axisThreshold);
  }
  }
  return false;
}

float evaluate_axis_source(const InputAxisSource &src) noexcept {
  switch (src.type) {
  case AxisSourceType::KeyPair: {
    const bool neg = (src.negativeKey >= 0) && is_key_down(src.negativeKey);
    const bool pos = (src.positiveKey >= 0) && is_key_down(src.positiveKey);
    if (neg == pos) {
      return 0.0F;
    }
    return (pos ? 1.0F : -1.0F) * src.scale;
  }
  case AxisSourceType::GamepadAxis: {
    if (!is_gamepad_connected() || (src.axisIndex < 0)) {
      return 0.0F;
    }
    const int rawDeadZone = static_cast<int>(src.deadZone * 32767.0F);
    const float raw = gamepad_axis_value(src.axisIndex, rawDeadZone);
    return raw * src.scale;
  }
  case AxisSourceType::MouseDeltaX:
    return g_mouseDeltaX * src.scale;
  case AxisSourceType::MouseDeltaY:
    return g_mouseDeltaY * src.scale;
  }
  return 0.0F;
}

// File I/O helpers (platform-agnostic, used for JSON persistence).
bool open_file_for_read(const char *path, FILE **outFile) noexcept {
  if ((path == nullptr) || (outFile == nullptr)) {
    return false;
  }
#if defined(_MSC_VER) || defined(_WIN32)
  *outFile = nullptr;
  return fopen_s(outFile, path, "rb") == 0;
#else
  *outFile = std::fopen(path, "rb");
  return *outFile != nullptr;
#endif
}

/// Reads text file data.
bool read_text_file(const char *path, std::unique_ptr<char[]> *outBuffer,
                    std::size_t *outSize) noexcept {
  FILE *file = nullptr;
  if (!open_file_for_read(path, &file) || (file == nullptr)) {
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long fileSize = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (fileSize <= 0) {
    std::fclose(file);
    return false;
  }
  const auto size = static_cast<std::size_t>(fileSize);
  std::unique_ptr<char[]> buffer(new (std::nothrow) char[size + 1U]);
  if (buffer == nullptr) {
    std::fclose(file);
    return false;
  }
  const std::size_t readCount = std::fread(buffer.get(), 1U, size, file);
  const bool hitError = std::ferror(file) != 0;
  std::fclose(file);
  if (hitError || (readCount != size)) {
    return false;
  }
  buffer[size] = '\0';
  *outSize = size;
  outBuffer->swap(buffer);
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool initialize_input_mapper() noexcept {
  if (g_mapperInitialized) {
    return true;
  }
  g_mappedActions = {};
  g_mappedAxes = {};
  g_actionDown = {};
  g_prevActionDown = {};
  g_mouseDeltaX = 0.0F;
  g_mouseDeltaY = 0.0F;
  g_mapperInitialized = true;
  return true;
}

/// Shuts down the owning system for input mapper.
void shutdown_input_mapper() noexcept {
  g_mapperInitialized = false;
  g_mappedActions = {};
  g_mappedAxes = {};
  g_actionDown = {};
  g_prevActionDown = {};
  g_mouseDeltaX = 0.0F;
  g_mouseDeltaY = 0.0F;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bool add_input_action(const char *name, const InputBinding *bindings,
                      std::uint32_t count) noexcept {
  if (name == nullptr) {
    return false;
  }
  const std::size_t nameLen = std::strlen(name);
  if ((nameLen == 0U) || (nameLen > kMaxInputNameLen)) {
    return false;
  }
  if ((count > 0U) && (bindings == nullptr)) {
    return false;
  }
  if (count > kMaxBindingsPerAction) {
    count = static_cast<std::uint32_t>(kMaxBindingsPerAction);
  }

  InputAction *existing = find_mapped_action(name);
  if (existing != nullptr) {
    existing->bindingCount = count;
    for (std::uint32_t i = 0; i < count; ++i) {
      existing->bindings[i] = bindings[i];
    }
    return true;
  }

  for (auto &a : g_mappedActions) {
    if (!a.occupied) {
      std::memcpy(a.name, name, nameLen + 1U);
      a.bindingCount = count;
      for (std::uint32_t i = 0; i < count; ++i) {
        a.bindings[i] = bindings[i];
      }
      a.callback = nullptr;
      a.userData = nullptr;
      a.occupied = true;
      return true;
    }
  }

  log_message(LogLevel::Warning, kLogChannel,
              "add_input_action: no free action slots");
  return false;
}

bool add_input_axis(const char *name, const InputAxisSource *sources,
                    std::uint32_t count) noexcept {
  if (name == nullptr) {
    return false;
  }
  const std::size_t nameLen = std::strlen(name);
  if ((nameLen == 0U) || (nameLen > kMaxInputNameLen)) {
    return false;
  }
  if ((count > 0U) && (sources == nullptr)) {
    return false;
  }
  if (count > kMaxSourcesPerAxis) {
    count = static_cast<std::uint32_t>(kMaxSourcesPerAxis);
  }

  InputAxisMapping *existing = find_mapped_axis(name);
  if (existing != nullptr) {
    existing->sourceCount = count;
    for (std::uint32_t i = 0; i < count; ++i) {
      existing->sources[i] = sources[i];
    }
    return true;
  }

  for (auto &a : g_mappedAxes) {
    if (!a.occupied) {
      std::memcpy(a.name, name, nameLen + 1U);
      a.sourceCount = count;
      for (std::uint32_t i = 0; i < count; ++i) {
        a.sources[i] = sources[i];
      }
      a.callback = nullptr;
      a.userData = nullptr;
      a.occupied = true;
      return true;
    }
  }

  log_message(LogLevel::Warning, kLogChannel,
              "add_input_axis: no free axis slots");
  return false;
}

bool remove_input_action(const char *name) noexcept {
  InputAction *a = find_mapped_action(name);
  if (a == nullptr) {
    return false;
  }
  *a = InputAction{};
  return true;
}

bool remove_input_axis(const char *name) noexcept {
  InputAxisMapping *a = find_mapped_axis(name);
  if (a == nullptr) {
    return false;
  }
  *a = InputAxisMapping{};
  return true;
}

// ---------------------------------------------------------------------------
// Callback registration
// ---------------------------------------------------------------------------

/// Drops every callback registration while keeping the key mappings.
void clear_action_callbacks() noexcept {
  for (auto &action : g_mappedActions) {
    action.callback = nullptr;
    action.userData = nullptr;
  }
  for (auto &axis : g_mappedAxes) {
    axis.callback = nullptr;
    axis.userData = nullptr;
  }
}

bool set_action_callback(const char *name, ActionCallback cb,
                         void *userData) noexcept {
  InputAction *a = find_mapped_action(name);
  if (a == nullptr) {
    return false;
  }
  a->callback = cb;
  a->userData = userData;
  return true;
}

/// Sets the requested value for axis callback.
bool set_axis_callback(const char *name, AxisCallback cb,
                       void *userData) noexcept {
  InputAxisMapping *a = find_mapped_axis(name);
  if (a == nullptr) {
    return false;
  }
  a->callback = cb;
  a->userData = userData;
  return true;
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

bool is_mapped_action_down(const char *name) noexcept {
  const InputAction *a = find_mapped_action(name);
  if (a == nullptr) {
    return false;
  }
  for (std::uint32_t i = 0; i < a->bindingCount; ++i) {
    if (evaluate_binding(a->bindings[i])) {
      return true;
    }
  }
  return false;
}

/// Returns whether is mapped action pressed.
bool is_mapped_action_pressed(const char *name) noexcept {
  if (name == nullptr) {
    return false;
  }

  for (std::size_t i = 0; i < kMaxInputActions; ++i) {
    if (g_mappedActions[i].occupied &&
        (std::strcmp(g_mappedActions[i].name, name) == 0)) {
      return g_actionDown[i] && !g_prevActionDown[i];
    }
  }
  return false;
}

/// Combines all of the axis's sources, keeping the one with the largest
/// absolute value.
float mapped_axis_value(const char *name) noexcept {
  const InputAxisMapping *a = find_mapped_axis(name);
  if (a == nullptr) {
    return 0.0F;
  }
  float result = 0.0F;
  for (std::uint32_t i = 0; i < a->sourceCount; ++i) {
    const float val = evaluate_axis_source(a->sources[i]);
    if ((val > 0.0F ? val : -val) > (result > 0.0F ? result : -result)) {
      result = val;
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Runtime rebinding
// ---------------------------------------------------------------------------

bool rebind_action(const char *actionName, std::uint32_t bindingIndex,
                   const InputBinding &newBinding) noexcept {
  InputAction *a = find_mapped_action(actionName);
  if (a == nullptr) {
    return false;
  }
  if (bindingIndex >= a->bindingCount) {
    if ((bindingIndex == a->bindingCount) &&
        (a->bindingCount < kMaxBindingsPerAction)) {
      a->bindings[a->bindingCount] = newBinding;
      ++a->bindingCount;
      return true;
    }
    return false;
  }
  a->bindings[bindingIndex] = newBinding;
  return true;
}

// ---------------------------------------------------------------------------
// Per-frame processing
// ---------------------------------------------------------------------------

void input_mapper_begin_frame() noexcept {
  g_prevActionDown = g_actionDown;
  g_mouseDeltaX = 0.0F;
  g_mouseDeltaY = 0.0F;
}

/// Consumes mouse-motion events into the per-frame delta accumulator; all
/// other event types are seen indirectly via the underlying input state
/// (is_key_down, etc.) that input_process_event maintains.
void input_mapper_process_event(const void *nativeEvent) noexcept {
  if (nativeEvent == nullptr) {
    return;
  }
  const MouseState ms = mouse_state();
  g_mouseDeltaX = static_cast<float>(ms.deltaX);
  g_mouseDeltaY = static_cast<float>(ms.deltaY);
}

void input_mapper_end_frame() noexcept {
  for (std::size_t i = 0; i < kMaxInputActions; ++i) {
    if (!g_mappedActions[i].occupied) {
      g_actionDown[i] = false;
      continue;
    }
    bool down = false;
    for (std::uint32_t b = 0; b < g_mappedActions[i].bindingCount; ++b) {
      if (evaluate_binding(g_mappedActions[i].bindings[b])) {
        down = true;
        break;
      }
    }
    g_actionDown[i] = down;

      if (g_mappedActions[i].callback != nullptr) {
      if (down && !g_prevActionDown[i]) {
        g_mappedActions[i].callback(g_mappedActions[i].name, true,
                                    g_mappedActions[i].userData);
      } else if (!down && g_prevActionDown[i]) {
        g_mappedActions[i].callback(g_mappedActions[i].name, false,
                                    g_mappedActions[i].userData);
      }
    }
  }

  for (std::size_t i = 0; i < kMaxInputAxes; ++i) {
    if (!g_mappedAxes[i].occupied || (g_mappedAxes[i].callback == nullptr)) {
      continue;
    }
    float result = 0.0F;
    for (std::uint32_t s = 0; s < g_mappedAxes[i].sourceCount; ++s) {
      const float val = evaluate_axis_source(g_mappedAxes[i].sources[s]);
      if ((val > 0.0F ? val : -val) > (result > 0.0F ? result : -result)) {
        result = val;
      }
    }
    g_mappedAxes[i].callback(g_mappedAxes[i].name, result,
                             g_mappedAxes[i].userData);
  }
}

// ---------------------------------------------------------------------------
// JSON persistence
// ---------------------------------------------------------------------------

bool input_bindings_default_path(char *outBuffer,
                                 std::size_t bufferCapacity) noexcept {
  if ((outBuffer == nullptr) || (bufferCapacity == 0U)) {
    return false;
  }

  char saveDir[512] = {};
  if (!platform_get_save_dir(saveDir, sizeof(saveDir))) {
    return false;
  }

  const int written = std::snprintf(outBuffer, bufferCapacity,
                                    "%s/input_bindings.json", saveDir);
  return (written > 0) && (static_cast<std::size_t>(written) < bufferCapacity);
}

bool save_input_bindings(const char *path) noexcept {
  JsonWriter writer{};
  writer.begin_object();

  writer.begin_array("actions");
  for (std::size_t i = 0; i < kMaxInputActions; ++i) {
    if (!g_mappedActions[i].occupied) {
      continue;
    }
    writer.begin_object();
    writer.write_string("name", g_mappedActions[i].name);
    writer.begin_array("bindings");
    for (std::uint32_t b = 0; b < g_mappedActions[i].bindingCount; ++b) {
      const auto &binding = g_mappedActions[i].bindings[b];
      writer.begin_object();
      writer.write_uint("type", static_cast<std::uint32_t>(binding.type));
      writer.write_uint("code", static_cast<std::uint32_t>(binding.code));
      writer.write_float("axis_threshold", binding.axisThreshold);
      writer.write_float("axis_scale", binding.axisScale);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();

  writer.begin_array("axes");
  for (std::size_t i = 0; i < kMaxInputAxes; ++i) {
    if (!g_mappedAxes[i].occupied) {
      continue;
    }
    writer.begin_object();
    writer.write_string("name", g_mappedAxes[i].name);
    writer.begin_array("sources");
    for (std::uint32_t s = 0; s < g_mappedAxes[i].sourceCount; ++s) {
      const auto &src = g_mappedAxes[i].sources[s];
      writer.begin_object();
      writer.write_uint("type", static_cast<std::uint32_t>(src.type));
      writer.write_uint("negative_key",
                        static_cast<std::uint32_t>(src.negativeKey));
      writer.write_uint("positive_key",
                        static_cast<std::uint32_t>(src.positiveKey));
      writer.write_uint("axis_index",
                        static_cast<std::uint32_t>(src.axisIndex));
      writer.write_float("scale", src.scale);
      writer.write_float("dead_zone", src.deadZone);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();

  writer.end_object();

  if (writer.failed()) {
    log_message(LogLevel::Error, kLogChannel,
                "save_input_bindings: JSON serialization failed");
    return false;
  }

  if ((path == nullptr) ||
      !atomic_write_file(path, writer.result(), writer.result_size())) {
    log_message(LogLevel::Error, kLogChannel,
                "save_input_bindings: atomic file write failed");
    return false;
  }
  return true;
}

/// Loads the requested resource for input bindings.
bool load_input_bindings(const char *path) noexcept {
  std::size_t fileSize = 0U;
  std::unique_ptr<char[]> fileBuffer{};
  if (!read_text_file(path, &fileBuffer, &fileSize)) {
    log_message(LogLevel::Error, kLogChannel,
                "load_input_bindings: failed to read file");
    return false;
  }
  return load_input_bindings_from_buffer(fileBuffer.get(), fileSize);
}

/// Saves the requested resource for input bindings to buffer.
bool save_input_bindings_to_buffer(char *buffer, std::size_t capacity,
                                   std::size_t *outSize) noexcept {
  if ((buffer == nullptr) || (outSize == nullptr) || (capacity < 2U)) {
    return false;
  }

  JsonWriter writer{};
  writer.begin_object();

  writer.begin_array("actions");
  for (std::size_t i = 0; i < kMaxInputActions; ++i) {
    if (!g_mappedActions[i].occupied) {
      continue;
    }
    writer.begin_object();
    writer.write_string("name", g_mappedActions[i].name);
    writer.begin_array("bindings");
    for (std::uint32_t b = 0; b < g_mappedActions[i].bindingCount; ++b) {
      const auto &binding = g_mappedActions[i].bindings[b];
      writer.begin_object();
      writer.write_uint("type", static_cast<std::uint32_t>(binding.type));
      writer.write_uint("code", static_cast<std::uint32_t>(binding.code));
      writer.write_float("axis_threshold", binding.axisThreshold);
      writer.write_float("axis_scale", binding.axisScale);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();

  writer.begin_array("axes");
  for (std::size_t i = 0; i < kMaxInputAxes; ++i) {
    if (!g_mappedAxes[i].occupied) {
      continue;
    }
    writer.begin_object();
    writer.write_string("name", g_mappedAxes[i].name);
    writer.begin_array("sources");
    for (std::uint32_t s = 0; s < g_mappedAxes[i].sourceCount; ++s) {
      const auto &src = g_mappedAxes[i].sources[s];
      writer.begin_object();
      writer.write_uint("type", static_cast<std::uint32_t>(src.type));
      writer.write_uint("negative_key",
                        static_cast<std::uint32_t>(src.negativeKey));
      writer.write_uint("positive_key",
                        static_cast<std::uint32_t>(src.positiveKey));
      writer.write_uint("axis_index",
                        static_cast<std::uint32_t>(src.axisIndex));
      writer.write_float("scale", src.scale);
      writer.write_float("dead_zone", src.deadZone);
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();

  writer.end_object();

  if (writer.failed()) {
    return false;
  }

  const std::size_t resultSize = writer.result_size();
  if ((resultSize + 1U) > capacity) {
    return false;
  }

  std::memcpy(buffer, writer.result(), resultSize);
  buffer[resultSize] = '\0';
  *outSize = resultSize;
  return true;
}

/// Loads the requested resource for input bindings from buffer.
bool load_input_bindings_from_buffer(const char *buffer,
                                     std::size_t size) noexcept {
  if ((buffer == nullptr) || (size == 0U)) {
    return false;
  }

  JsonParser parser{};
  if (!parser.parse(buffer, size)) {
    log_message(LogLevel::Error, kLogChannel,
                "load_input_bindings: JSON parse failed");
    return false;
  }

  const JsonValue *root = parser.root();
  if ((root == nullptr) || (root->type != JsonValue::Type::Object)) {
    log_message(LogLevel::Error, kLogChannel,
                "load_input_bindings: root is not an object");
    return false;
  }

  JsonValue actionsVal{};
  JsonValue axesVal{};
  const bool shapeValid =
      parser.get_object_field(*root, "actions", &actionsVal) &&
      (actionsVal.type == JsonValue::Type::Array) &&
      parser.get_object_field(*root, "axes", &axesVal) &&
      (axesVal.type == JsonValue::Type::Array);
  if (!shapeValid) {
    log_message(LogLevel::Error, kLogChannel,
                "load_input_bindings: missing actions/axes arrays");
    return false;
  }

  std::unique_ptr<StagedBindings> staged(new (std::nothrow) StagedBindings());
  if (staged == nullptr) {
    return false;
  }

  {
    const std::size_t count = parser.array_size(actionsVal);
    if (count > kMaxInputActions) {
      log_message(LogLevel::Error, kLogChannel,
                  "load_input_bindings: actions array exceeds capacity; "
                  "rejecting the document");
      return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
      JsonValue actionVal{};
      if (!parser.get_array_element(actionsVal, i, &actionVal) ||
          (actionVal.type != JsonValue::Type::Object)) {
        log_message(LogLevel::Error, kLogChannel,
                    "load_input_bindings: action entry is not an object");
        return false;
      }

      InputAction action{};
      action.occupied = true;

      if (!parse_entry_name(parser, actionVal, action.name, "action")) {
        return false;
      }

      JsonValue bindingsVal{};
      if (parser.get_object_field(actionVal, "bindings", &bindingsVal) &&
          (bindingsVal.type == JsonValue::Type::Array)) {
        const std::size_t bCount = parser.array_size(bindingsVal);
        if (bCount > kMaxBindingsPerAction) {
          log_message(LogLevel::Error, kLogChannel,
                      "load_input_bindings: an action's bindings exceed "
                      "capacity; rejecting the document");
          return false;
        }
        for (std::size_t b = 0; b < bCount; ++b) {
          JsonValue bVal{};
          if (!parser.get_array_element(bindingsVal, b, &bVal) ||
              (bVal.type != JsonValue::Type::Object)) {
            log_message(LogLevel::Error, kLogChannel,
                        "load_input_bindings: binding is not an object");
            return false;
          }

          InputBinding binding{};
          std::uint32_t uval = 0;
          float fval = 0.0F;

          JsonValue field{};
          if (parser.get_object_field(bVal, "type", &field) &&
              parser.as_uint(field, &uval)) {
            if (uval > static_cast<std::uint32_t>(
                           InputBindingType::GamepadAxis)) {
              log_message(LogLevel::Error, kLogChannel,
                          "load_input_bindings: binding type out of range");
              return false;
            }
            binding.type = static_cast<InputBindingType>(uval);
          }
          if (parser.get_object_field(bVal, "code", &field) &&
              parser.as_uint(field, &uval)) {
            binding.code = static_cast<int>(uval);
          }
          if (parser.get_object_field(bVal, "axis_threshold", &field) &&
              parser.as_float(field, &fval)) {
            binding.axisThreshold = fval;
          }
          if (parser.get_object_field(bVal, "axis_scale", &field) &&
              parser.as_float(field, &fval)) {
            binding.axisScale = fval;
          }

          action.bindings[action.bindingCount] = binding;
          ++action.bindingCount;
        }
      }

      for (auto &slot : staged->actions) {
        if (!slot.occupied) {
          slot = action;
          break;
        }
      }
    }
  }

  {
    const std::size_t count = parser.array_size(axesVal);
    if (count > kMaxInputAxes) {
      log_message(LogLevel::Error, kLogChannel,
                  "load_input_bindings: axes array exceeds capacity; "
                  "rejecting the document");
      return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
      JsonValue axisVal{};
      if (!parser.get_array_element(axesVal, i, &axisVal) ||
          (axisVal.type != JsonValue::Type::Object)) {
        log_message(LogLevel::Error, kLogChannel,
                    "load_input_bindings: axis entry is not an object");
        return false;
      }

      InputAxisMapping axis{};
      axis.occupied = true;

      if (!parse_entry_name(parser, axisVal, axis.name, "axis")) {
        return false;
      }

      JsonValue sourcesVal{};
      if (parser.get_object_field(axisVal, "sources", &sourcesVal) &&
          (sourcesVal.type == JsonValue::Type::Array)) {
        const std::size_t sCount = parser.array_size(sourcesVal);
        if (sCount > kMaxSourcesPerAxis) {
          log_message(LogLevel::Error, kLogChannel,
                      "load_input_bindings: an axis's sources exceed "
                      "capacity; rejecting the document");
          return false;
        }
        for (std::size_t s = 0; s < sCount; ++s) {
          JsonValue sVal{};
          if (!parser.get_array_element(sourcesVal, s, &sVal) ||
              (sVal.type != JsonValue::Type::Object)) {
            log_message(LogLevel::Error, kLogChannel,
                        "load_input_bindings: axis source is not an object");
            return false;
          }

          InputAxisSource src{};
          std::uint32_t uval = 0;
          float fval = 0.0F;

          JsonValue field{};
          if (parser.get_object_field(sVal, "type", &field) &&
              parser.as_uint(field, &uval)) {
            if (uval >
                static_cast<std::uint32_t>(AxisSourceType::MouseDeltaY)) {
              log_message(LogLevel::Error, kLogChannel,
                          "load_input_bindings: axis source type out of range");
              return false;
            }
            src.type = static_cast<AxisSourceType>(uval);
          }
          if (parser.get_object_field(sVal, "negative_key", &field) &&
              parser.as_uint(field, &uval)) {
            src.negativeKey = static_cast<int>(uval);
          }
          if (parser.get_object_field(sVal, "positive_key", &field) &&
              parser.as_uint(field, &uval)) {
            src.positiveKey = static_cast<int>(uval);
          }
          if (parser.get_object_field(sVal, "axis_index", &field) &&
              parser.as_uint(field, &uval)) {
            src.axisIndex = static_cast<int>(uval);
          }
          if (parser.get_object_field(sVal, "scale", &field) &&
              parser.as_float(field, &fval)) {
            src.scale = fval;
          }
          if (parser.get_object_field(sVal, "dead_zone", &field) &&
              parser.as_float(field, &fval)) {
            src.deadZone = fval;
          }

          axis.sources[axis.sourceCount] = src;
          ++axis.sourceCount;
        }
      }

      for (auto &slot : staged->axes) {
        if (!slot.occupied) {
          slot = axis;
          break;
        }
      }
    }
  }

  g_mappedActions = staged->actions;
  g_mappedAxes = staged->axes;
  g_actionDown = {};
  g_prevActionDown = {};
  return true;
}

} // namespace engine::core
